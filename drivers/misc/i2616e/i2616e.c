/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * BARROT i2616e BLE module UART AT-command driver.
 *
 * The module speaks an ASCII AT protocol: commands sent by the MCU end with a
 * bare '\r', while responses and asynchronous indications are framed with
 * '\r\n'. A command transaction ends on "OK" or "ERROR:<code>". Lines that
 * start with "IM_" are asynchronous events; received application data arrives
 * as a length-prefixed, possibly binary "DATA:<cid>,<len>,<payload>" frame that
 * must be parsed by length rather than split on '\r\n'.
 *
 * Transport is a modem_pipe over modem_backend_uart. Received bytes are drained
 * in a workqueue context, assembled, and classified by a hand-rolled parser
 * (modem_chat cannot carry the binary DATA payload). Command responses are
 * handed back to the calling thread through a semaphore; events are delivered
 * through a message queue consumed by i2616e_get_event().
 */

#define DT_DRV_COMPAT barrot_i2616e

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/modem/backend/uart.h>
#include <zephyr/modem/pipe.h>
#include <zephyr/sys/util.h>

#include <zephyr/drivers/misc/i2616e/i2616e.h>

LOG_MODULE_REGISTER(i2616e, CONFIG_I2616E_LOG_LEVEL);

#define I2616E_UART_RX_BUF_SIZE 256U
#define I2616E_UART_TX_BUF_SIZE 640U
#define I2616E_PARSE_BUF_SIZE   640U
#define I2616E_RESP_BUF_SIZE    256U
#define I2616E_CMD_BUF_SIZE     576U
#define I2616E_RX_CHUNK_SIZE    64U

#define I2616E_CMD_TIMEOUT      K_MSEC(1500)
/* Flash writes for reboot-sensitive settings need time to settle. */
#define I2616E_FLASH_SETTLE     K_MSEC(600)
#define I2616E_TX_RETRY_MS      1U
#define I2616E_TX_RETRY_LIMIT   200U

struct i2616e_config {
	const struct device *uart;
	struct gpio_dt_spec reset;
	struct gpio_dt_spec sleep_enable;
	uint16_t reset_assert_ms;
	uint16_t ready_timeout_ms;
};

struct i2616e_data {
	const struct device *dev;

	struct modem_backend_uart backend;
	struct modem_pipe *pipe;
	uint8_t uart_rx_buf[I2616E_UART_RX_BUF_SIZE];
	uint8_t uart_tx_buf[I2616E_UART_TX_BUF_SIZE];

	struct k_work rx_work;
	uint8_t parse_buf[I2616E_PARSE_BUF_SIZE];
	size_t parse_len;

	/* Command transaction state, protected by cmd_lock (serialisation) and
	 * the spinlock (concurrent access from the rx work).
	 */
	struct k_mutex cmd_lock;
	struct k_sem cmd_sem;
	struct k_spinlock lock;
	bool cmd_active;
	int cmd_status;
	char resp[I2616E_RESP_BUF_SIZE];
	size_t resp_len;

	struct k_sem ready_sem;

	struct k_msgq event_q;
	char event_q_buf[CONFIG_I2616E_EVENT_QUEUE_SIZE * sizeof(struct i2616e_event)];

	/* Scratch event filled by the (single-threaded) receive work before it
	 * is copied into event_q, kept here to avoid large stack frames on the
	 * system workqueue.
	 */
	struct i2616e_event scratch;
};

/* ------------------------------------------------------------------------- */
/* Small parsing helpers                                                     */
/* ------------------------------------------------------------------------- */

static bool i2616e_starts_with(const uint8_t *data, size_t len, const char *prefix)
{
	size_t prefix_len = strlen(prefix);

	return len >= prefix_len && memcmp(data, prefix, prefix_len) == 0;
}

static uint32_t i2616e_parse_uint(const char *s)
{
	uint32_t value = 0U;

	while (*s >= '0' && *s <= '9') {
		value = (value * 10U) + (uint32_t)(*s - '0');
		s++;
	}

	return value;
}

static bool i2616e_valid_cid(const char *cid)
{
	return cid != NULL && strlen(cid) == I2616E_CID_TEXT_SIZE;
}

/* Copy the value of the first response line that begins with @prefix into
 * @out (NUL terminated). Response lines are separated by '\n'.
 */
static bool i2616e_resp_extract(const char *resp, const char *prefix, char *out, size_t out_size)
{
	size_t prefix_len = strlen(prefix);
	const char *line = resp;

	while (*line != '\0') {
		const char *end = strchr(line, '\n');
		size_t line_len = (end != NULL) ? (size_t)(end - line) : strlen(line);

		if (line_len >= prefix_len && memcmp(line, prefix, prefix_len) == 0) {
			size_t value_len = line_len - prefix_len;

			if (value_len >= out_size) {
				value_len = out_size - 1U;
			}
			memcpy(out, line + prefix_len, value_len);
			out[value_len] = '\0';
			return true;
		}

		if (end == NULL) {
			break;
		}
		line = end + 1;
	}

	return false;
}

/* Copy the first response line into @out (NUL terminated). */
static bool i2616e_resp_first_line(const char *resp, char *out, size_t out_size)
{
	const char *end = strchr(resp, '\n');
	size_t line_len = (end != NULL) ? (size_t)(end - resp) : strlen(resp);

	if (line_len == 0U) {
		return false;
	}
	if (line_len >= out_size) {
		line_len = out_size - 1U;
	}
	memcpy(out, resp, line_len);
	out[line_len] = '\0';
	return true;
}

/* ------------------------------------------------------------------------- */
/* Event delivery                                                            */
/* ------------------------------------------------------------------------- */

static void i2616e_push_event(struct i2616e_data *data, const struct i2616e_event *event)
{
	if (k_msgq_put(&data->event_q, event, K_NO_WAIT) != 0) {
		LOG_WRN("event queue full, dropping event type %d", event->type);
	}
}

static void i2616e_copy_cid(struct i2616e_event *event, const uint8_t *src)
{
	memcpy(event->cid, src, I2616E_CID_TEXT_SIZE);
	event->cid[I2616E_CID_TEXT_SIZE] = '\0';
}

static void i2616e_copy_address(struct i2616e_event *event, const uint8_t *src)
{
	memcpy(event->address, src, I2616E_ADDRESS_TEXT_SIZE);
	event->address[I2616E_ADDRESS_TEXT_SIZE] = '\0';
}

/* ------------------------------------------------------------------------- */
/* Line / frame classification                                               */
/* ------------------------------------------------------------------------- */

static void i2616e_handle_response_line(struct i2616e_data *data, const uint8_t *line, size_t len)
{
	k_spinlock_key_t key = k_spin_lock(&data->lock);

	if (data->cmd_active && (data->resp_len + len + 1U) < sizeof(data->resp)) {
		memcpy(&data->resp[data->resp_len], line, len);
		data->resp_len += len;
		data->resp[data->resp_len++] = '\n';
		data->resp[data->resp_len] = '\0';
	}

	k_spin_unlock(&data->lock, key);
}

static void i2616e_complete_command(struct i2616e_data *data, int status)
{
	k_spinlock_key_t key = k_spin_lock(&data->lock);

	if (data->cmd_active) {
		data->cmd_active = false;
		data->cmd_status = status;
		k_spin_unlock(&data->lock, key);
		k_sem_give(&data->cmd_sem);
		return;
	}

	k_spin_unlock(&data->lock, key);
}

static void i2616e_handle_event_line(struct i2616e_data *data, const uint8_t *line, size_t len)
{
	struct i2616e_event *event = &data->scratch;
	size_t prefix_len;

	memset(event, 0, sizeof(*event));

	if (i2616e_starts_with(line, len, "IM_READY")) {
		event->type = I2616E_EVENT_READY;
		k_sem_give(&data->ready_sem);
		i2616e_push_event(data, event);
		return;
	}

	if (i2616e_starts_with(line, len, "IM_CTO")) {
		event->type = I2616E_EVENT_CONNECTION_TIMEOUT;
		i2616e_push_event(data, event);
		return;
	}

	if (i2616e_starts_with(line, len, "IM_NOSVC")) {
		event->type = I2616E_EVENT_SERVICE_MISMATCH;
		i2616e_push_event(data, event);
		return;
	}

	/* The remaining events all carry a 4-character cid right after the
	 * prefix.
	 */
	prefix_len = strlen("IM_CONN:");
	if (i2616e_starts_with(line, len, "IM_CONN:") && len >= prefix_len + I2616E_CID_TEXT_SIZE) {
		event->type = I2616E_EVENT_CONNECTED;
		i2616e_copy_cid(event, line + prefix_len);
		i2616e_push_event(data, event);
		return;
	}

	if (i2616e_starts_with(line, len, "IM_DISC:") && len >= prefix_len + I2616E_CID_TEXT_SIZE) {
		event->type = I2616E_EVENT_DISCONNECTED;
		i2616e_copy_cid(event, line + prefix_len);
		i2616e_push_event(data, event);
		return;
	}

	if (i2616e_starts_with(line, len, "IM_PAIR:") && len >= prefix_len + I2616E_CID_TEXT_SIZE) {
		event->type = I2616E_EVENT_PAIR_REQUEST;
		i2616e_copy_cid(event, line + prefix_len);
		i2616e_push_event(data, event);
		return;
	}

	prefix_len = strlen("IM_PSKENTRY:");
	if (i2616e_starts_with(line, len, "IM_PSKENTRY:") &&
	    len >= prefix_len + I2616E_CID_TEXT_SIZE) {
		event->type = I2616E_EVENT_PASSKEY_REQUEST;
		i2616e_copy_cid(event, line + prefix_len);
		i2616e_push_event(data, event);
		return;
	}

	/* IM_DISPPSK:<cid>,<passkey> */
	prefix_len = strlen("IM_DISPPSK:");
	if (i2616e_starts_with(line, len, "IM_DISPPSK:") &&
	    len > prefix_len + I2616E_CID_TEXT_SIZE + 1U &&
	    line[prefix_len + I2616E_CID_TEXT_SIZE] == ',') {
		char digits[12] = {0};
		size_t off = prefix_len + I2616E_CID_TEXT_SIZE + 1U;
		size_t copy = MIN(len - off, sizeof(digits) - 1U);

		event->type = I2616E_EVENT_PASSKEY_DISPLAY;
		i2616e_copy_cid(event, line + prefix_len);
		memcpy(digits, line + off, copy);
		event->passkey = i2616e_parse_uint(digits);
		i2616e_push_event(data, event);
		return;
	}

	/* IM_AUTH:<cid>,<addr> */
	prefix_len = strlen("IM_AUTH:");
	if (i2616e_starts_with(line, len, "IM_AUTH:") &&
	    len >= prefix_len + I2616E_CID_TEXT_SIZE + 1U + I2616E_ADDRESS_TEXT_SIZE &&
	    line[prefix_len + I2616E_CID_TEXT_SIZE] == ',') {
		event->type = I2616E_EVENT_PAIRED;
		i2616e_copy_cid(event, line + prefix_len);
		i2616e_copy_address(event, line + prefix_len + I2616E_CID_TEXT_SIZE + 1U);
		i2616e_push_event(data, event);
		return;
	}

	LOG_DBG("unhandled line: %.*s", (int)len, line);
}

static void i2616e_handle_line(struct i2616e_data *data, const uint8_t *line, size_t len)
{
	if (len == 0U) {
		return;
	}

	if (i2616e_starts_with(line, len, "OK")) {
		i2616e_complete_command(data, 0);
		return;
	}

	if (i2616e_starts_with(line, len, "ERROR")) {
		LOG_DBG("command error: %.*s", (int)len, line);
		i2616e_complete_command(data, -EIO);
		return;
	}

	if (i2616e_starts_with(line, len, "IM_")) {
		i2616e_handle_event_line(data, line, len);
		return;
	}

	/* Anything else is a response data line for the in-flight command. */
	i2616e_handle_response_line(data, line, len);
}

/* Try to parse a binary "DATA:<cid>,<len>,<payload>\r\n" frame starting at
 * @data. Returns the number of bytes consumed, or 0 if the frame is not yet
 * complete (and SIZE_MAX if it is malformed and one byte should be skipped).
 */
static size_t i2616e_try_parse_data(struct i2616e_data *drv_data, const uint8_t *data, size_t len)
{
	const size_t prefix_len = strlen("DATA:");
	size_t idx = prefix_len + I2616E_CID_TEXT_SIZE;
	size_t payload_len = 0U;
	size_t payload_off;
	size_t total;
	struct i2616e_event *event = &drv_data->scratch;

	if (len < prefix_len + I2616E_CID_TEXT_SIZE + 3U) {
		return 0U;
	}
	if (data[idx] != ',') {
		return SIZE_MAX;
	}
	idx++;

	while (idx < len && data[idx] != ',') {
		if (data[idx] < '0' || data[idx] > '9') {
			return SIZE_MAX;
		}
		payload_len = (payload_len * 10U) + (size_t)(data[idx] - '0');
		idx++;
	}

	if (idx >= len) {
		return 0U;
	}
	if (payload_len > I2616E_PDU_MAX_SIZE) {
		return SIZE_MAX;
	}

	payload_off = idx + 1U;
	total = payload_off + payload_len + 2U;
	if (len < total) {
		return 0U;
	}
	if (data[payload_off + payload_len] != '\r' || data[payload_off + payload_len + 1U] != '\n') {
		return SIZE_MAX;
	}

	memset(event, 0, sizeof(*event));
	event->type = I2616E_EVENT_DATA_RECEIVED;
	i2616e_copy_cid(event, data + prefix_len);
	event->length = payload_len;
	memcpy(event->data, data + payload_off, payload_len);
	i2616e_push_event(drv_data, event);

	return total;
}

/* ------------------------------------------------------------------------- */
/* Receive path                                                              */
/* ------------------------------------------------------------------------- */

static void i2616e_process_parse_buffer(struct i2616e_data *data)
{
	size_t pos = 0U;

	while (pos < data->parse_len) {
		const uint8_t *p = &data->parse_buf[pos];
		size_t remaining = data->parse_len - pos;

		/* Strip the '\r\n' framing between lines. */
		if (*p == '\r' || *p == '\n') {
			pos++;
			continue;
		}

		if (i2616e_starts_with(p, remaining, "DATA:")) {
			size_t consumed = i2616e_try_parse_data(data, p, remaining);

			if (consumed == 0U) {
				break; /* wait for more bytes */
			}
			if (consumed == SIZE_MAX) {
				pos++; /* malformed, resync */
				continue;
			}
			pos += consumed;
			continue;
		}

		/* Generic line: find the terminating '\r' or '\n'. */
		size_t line_len = 0U;

		while (line_len < remaining && p[line_len] != '\r' && p[line_len] != '\n') {
			line_len++;
		}

		if (line_len == remaining) {
			break; /* line not terminated yet */
		}

		i2616e_handle_line(data, p, line_len);
		pos += line_len;
	}

	if (pos > 0U) {
		data->parse_len -= pos;
		memmove(data->parse_buf, &data->parse_buf[pos], data->parse_len);
	}
}

static void i2616e_rx_work(struct k_work *work)
{
	struct i2616e_data *data = CONTAINER_OF(work, struct i2616e_data, rx_work);
	uint8_t chunk[I2616E_RX_CHUNK_SIZE];
	int ret;

	while ((ret = modem_pipe_receive(data->pipe, chunk, sizeof(chunk))) > 0) {
		size_t avail = sizeof(data->parse_buf) - data->parse_len;

		if ((size_t)ret > avail) {
			LOG_WRN("parse buffer overflow, resetting");
			data->parse_len = 0U;
			avail = sizeof(data->parse_buf);
			if ((size_t)ret > avail) {
				continue;
			}
		}

		memcpy(&data->parse_buf[data->parse_len], chunk, ret);
		data->parse_len += ret;
		i2616e_process_parse_buffer(data);
	}
}

static void i2616e_pipe_callback(struct modem_pipe *pipe, enum modem_pipe_event event,
				 void *user_data)
{
	struct i2616e_data *data = user_data;

	ARG_UNUSED(pipe);

	if (event == MODEM_PIPE_EVENT_RECEIVE_READY) {
		k_work_submit(&data->rx_work);
	}
}

/* ------------------------------------------------------------------------- */
/* Command transmission                                                      */
/* ------------------------------------------------------------------------- */

static int i2616e_transmit_all(struct i2616e_data *data, const uint8_t *buf, size_t len)
{
	size_t off = 0U;
	uint32_t retries = 0U;

	while (off < len) {
		int ret = modem_pipe_transmit(data->pipe, &buf[off], len - off);

		if (ret < 0) {
			return ret;
		}
		if (ret == 0) {
			if (++retries > I2616E_TX_RETRY_LIMIT) {
				return -ETIMEDOUT;
			}
			k_msleep(I2616E_TX_RETRY_MS);
			continue;
		}
		off += ret;
		retries = 0U;
	}

	return 0;
}

/* Run one command transaction. Must be called with cmd_lock held. On success
 * the captured response lines are available in data->resp (NUL terminated).
 */
static int i2616e_transact_locked(struct i2616e_data *data, const uint8_t *cmd, size_t cmd_len,
				  k_timeout_t timeout)
{
	k_spinlock_key_t key;
	int ret;

	k_sem_reset(&data->cmd_sem);

	key = k_spin_lock(&data->lock);
	data->resp_len = 0U;
	data->resp[0] = '\0';
	data->cmd_status = -ETIMEDOUT;
	data->cmd_active = true;
	k_spin_unlock(&data->lock, key);

	ret = i2616e_transmit_all(data, cmd, cmd_len);
	if (ret < 0) {
		key = k_spin_lock(&data->lock);
		data->cmd_active = false;
		k_spin_unlock(&data->lock, key);
		return ret;
	}

	if (k_sem_take(&data->cmd_sem, timeout) != 0) {
		key = k_spin_lock(&data->lock);
		data->cmd_active = false;
		k_spin_unlock(&data->lock, key);
		return -ETIMEDOUT;
	}

	return data->cmd_status;
}

/* Send a fixed string command (terminated by the caller with '\r'). */
static int i2616e_command(struct i2616e_data *data, const char *cmd)
{
	int ret;

	k_mutex_lock(&data->cmd_lock, K_FOREVER);
	ret = i2616e_transact_locked(data, (const uint8_t *)cmd, strlen(cmd), I2616E_CMD_TIMEOUT);
	k_mutex_unlock(&data->cmd_lock);

	return ret;
}

/* Send a query and extract the value following @prefix. When @prefix is NULL,
 * the first response line is returned instead.
 */
static int i2616e_query(struct i2616e_data *data, const char *cmd, const char *prefix, char *out,
			size_t out_size)
{
	bool ok;
	int ret;

	k_mutex_lock(&data->cmd_lock, K_FOREVER);
	ret = i2616e_transact_locked(data, (const uint8_t *)cmd, strlen(cmd), I2616E_CMD_TIMEOUT);
	if (ret == 0) {
		ok = (prefix != NULL) ? i2616e_resp_extract(data->resp, prefix, out, out_size)
				      : i2616e_resp_first_line(data->resp, out, out_size);
		if (!ok) {
			ret = -ENOMSG;
		}
	}
	k_mutex_unlock(&data->cmd_lock);

	return ret;
}

static int i2616e_query_bool(struct i2616e_data *data, const char *cmd, const char *prefix,
			     bool *enabled)
{
	char value[8];
	int ret = i2616e_query(data, cmd, prefix, value, sizeof(value));

	if (ret == 0) {
		*enabled = value[0] != '0';
	}

	return ret;
}

/* ------------------------------------------------------------------------- */
/* Public API                                                                */
/* ------------------------------------------------------------------------- */

int i2616e_hardware_reset(const struct device *dev)
{
	const struct i2616e_config *cfg = dev->config;
	struct i2616e_data *data = dev->data;
	int ret;

	k_sem_reset(&data->ready_sem);

	k_spinlock_key_t key = k_spin_lock(&data->lock);

	data->parse_len = 0U;
	k_spin_unlock(&data->lock, key);

	ret = gpio_pin_set_dt(&cfg->reset, 1);
	if (ret < 0) {
		return ret;
	}
	k_msleep(cfg->reset_assert_ms);
	ret = gpio_pin_set_dt(&cfg->reset, 0);
	if (ret < 0) {
		return ret;
	}

	if (k_sem_take(&data->ready_sem, K_MSEC(cfg->ready_timeout_ms)) != 0) {
		LOG_ERR("timed out waiting for IM_READY");
		return -ETIMEDOUT;
	}

	return 0;
}

int i2616e_get_info(const struct device *dev, struct i2616e_info *info)
{
	struct i2616e_data *data = dev->data;
	char value[24];
	int ret;

	if (info == NULL) {
		return -EINVAL;
	}

	memset(info, 0, sizeof(*info));

	ret = i2616e_query(data, "AT+GFWVER?\r", NULL, info->firmware_version,
			   sizeof(info->firmware_version));
	if (ret < 0) {
		return ret;
	}

	ret = i2616e_query(data, "AT+GVER?\r", NULL, info->config_version,
			   sizeof(info->config_version));
	if (ret < 0) {
		return ret;
	}

	ret = i2616e_query(data, "AT+NAME?\r", "+NAME:", info->name, sizeof(info->name));
	if (ret < 0) {
		return ret;
	}

	ret = i2616e_query(data, "AT+LBDADDR?\r", "+LBDADDR:", info->address, sizeof(info->address));
	if (ret < 0) {
		return ret;
	}

	ret = i2616e_query(data, "AT+BAUD?\r", "+BAUD:", value, sizeof(value));
	if (ret < 0) {
		return ret;
	}
	info->baudrate = i2616e_parse_uint(value);

	ret = i2616e_query(data, "AT+FLOWCTRL?\r", "+FLOWCTRL:", value, sizeof(value));
	if (ret < 0) {
		return ret;
	}
	info->flow_control = value[0] != '0';

	ret = i2616e_query(data, "AT+BTMODE?\r", "+BTMODE:", value, sizeof(value));
	if (ret < 0) {
		return ret;
	}
	info->bluetooth_mode = (uint8_t)i2616e_parse_uint(value);

	return 0;
}

static int i2616e_apply_bool(struct i2616e_data *data, const char *query, const char *prefix,
			     const char *set_on, const char *set_off, bool desired, bool *changed)
{
	bool current = false;
	int ret;

	*changed = false;

	ret = i2616e_query_bool(data, query, prefix, &current);
	if (ret < 0) {
		return ret;
	}
	if (current == desired) {
		return 0;
	}

	ret = i2616e_command(data, desired ? set_on : set_off);
	if (ret < 0) {
		return ret;
	}

	*changed = true;
	return 0;
}

int i2616e_apply_settings(const struct device *dev, const struct i2616e_settings *settings)
{
	struct i2616e_data *data = dev->data;
	bool changed;
	bool reboot = false;
	char current[I2616E_NAME_MAX_SIZE + 1];
	int ret;

	if (settings == NULL) {
		return -EINVAL;
	}

	/* Device name (no reboot required). */
	if (settings->name != NULL && settings->name[0] != '\0') {
		if (strlen(settings->name) > I2616E_NAME_MAX_SIZE) {
			return -EINVAL;
		}

		ret = i2616e_query(data, "AT+NAME?\r", "+NAME:", current, sizeof(current));
		if (ret < 0) {
			return ret;
		}
		if (strcmp(current, settings->name) != 0) {
			char cmd[8 + I2616E_NAME_MAX_SIZE + 2];

			(void)snprintf(cmd, sizeof(cmd), "AT+NAME=%s\r", settings->name);
			ret = i2616e_command(data, cmd);
			if (ret < 0) {
				return ret;
			}
		}
	}

	ret = i2616e_apply_bool(data, "AT+SILENT?\r", "+SILENT:", "AT+SILENT=1\r", "AT+SILENT=0\r",
				settings->silent, &changed);
	if (ret < 0) {
		return ret;
	}

	/* Enable PDU command mode before multi-connection, per the module
	 * application note.
	 */
	ret = i2616e_apply_bool(data, "AT+COMMAND?\r", "+COMMAND:", "AT+COMMAND=1\r",
				"AT+COMMAND=0\r", settings->command_mode, &changed);
	if (ret < 0) {
		return ret;
	}

	/* BLE mode change requires a reboot. */
	ret = i2616e_query(data, "AT+BTMODE?\r", "+BTMODE:", current, sizeof(current));
	if (ret < 0) {
		return ret;
	}
	if ((uint8_t)i2616e_parse_uint(current) != settings->bluetooth_mode) {
		char cmd[16];

		(void)snprintf(cmd, sizeof(cmd), "AT+BTMODE=%u\r", settings->bluetooth_mode);
		ret = i2616e_command(data, cmd);
		if (ret < 0) {
			return ret;
		}
		reboot = true;
	}

	ret = i2616e_apply_bool(data, "AT+AUTOUNLOCK?\r", "+AUTOUNLOCK:", "AT+AUTOUNLOCK=1\r",
				"AT+AUTOUNLOCK=0\r", settings->auto_unlock, &changed);
	if (ret < 0) {
		return ret;
	}
	reboot = reboot || changed;

	ret = i2616e_apply_bool(data, "AT+MULTICONN?\r", "+MULTICONN:", "AT+MULTICONN=1\r",
				"AT+MULTICONN=0\r", settings->multi_connection, &changed);
	if (ret < 0) {
		return ret;
	}
	reboot = reboot || changed;

	if (reboot) {
		k_sleep(I2616E_FLASH_SETTLE);
		ret = i2616e_hardware_reset(dev);
		if (ret < 0) {
			return ret;
		}
	}

	/* Advertising is applied last, after any reboot. */
	ret = i2616e_apply_bool(data, "AT+ADV?\r", "+ADV:", "AT+ADV=1\r", "AT+ADV=0\r",
				settings->advertising, &changed);
	if (ret < 0) {
		return ret;
	}

	return 0;
}

int i2616e_get_event(const struct device *dev, struct i2616e_event *event, k_timeout_t timeout)
{
	struct i2616e_data *data = dev->data;

	if (event == NULL) {
		return -EINVAL;
	}

	if (k_msgq_get(&data->event_q, event, timeout) != 0) {
		return -EAGAIN;
	}

	return 0;
}

int i2616e_send(const struct device *dev, const char *cid, const uint8_t *payload, size_t length)
{
	struct i2616e_data *data = dev->data;
	uint8_t cmd[I2616E_CMD_BUF_SIZE];
	size_t pos = 0U;
	int written;
	int ret;

	if (!i2616e_valid_cid(cid) || payload == NULL || length == 0U ||
	    length > I2616E_PDU_MAX_SIZE) {
		return -EINVAL;
	}

	written = snprintf((char *)cmd, sizeof(cmd), "AT+SEND=%s,%zu,", cid, length);
	if (written < 0 || (size_t)written + length + 1U > sizeof(cmd)) {
		return -EINVAL;
	}
	pos = (size_t)written;
	memcpy(&cmd[pos], payload, length);
	pos += length;
	cmd[pos++] = '\r';

	k_mutex_lock(&data->cmd_lock, K_FOREVER);
	ret = i2616e_transact_locked(data, cmd, pos, I2616E_CMD_TIMEOUT);
	k_mutex_unlock(&data->cmd_lock);

	return ret;
}

int i2616e_get_connection_mtu(const struct device *dev, const char *cid, uint16_t *mtu)
{
	struct i2616e_data *data = dev->data;
	char value[I2616E_RESP_BUF_SIZE];
	const char *entry;
	int ret;

	if (!i2616e_valid_cid(cid) || mtu == NULL) {
		return -EINVAL;
	}

	ret = i2616e_query(data, "AT+CONNMTU?\r", "+CONNMTU:", value, sizeof(value));
	if (ret < 0) {
		return ret;
	}

	/* value is "<cid>,<mtu>;<cid>,<mtu>;..." */
	entry = value;
	while (entry != NULL && *entry != '\0') {
		if (strncmp(entry, cid, I2616E_CID_TEXT_SIZE) == 0 &&
		    entry[I2616E_CID_TEXT_SIZE] == ',') {
			*mtu = (uint16_t)i2616e_parse_uint(entry + I2616E_CID_TEXT_SIZE + 1U);
			return 0;
		}
		entry = strchr(entry, ';');
		if (entry != NULL) {
			entry++;
		}
	}

	return -ENOENT;
}

int i2616e_pair_confirm(const struct device *dev, const char *cid, bool accept)
{
	struct i2616e_data *data = dev->data;
	char cmd[24];

	if (!i2616e_valid_cid(cid)) {
		return -EINVAL;
	}

	(void)snprintf(cmd, sizeof(cmd), "AT+PAIR=%s,%u\r", cid, accept ? 1U : 0U);
	return i2616e_command(data, cmd);
}

int i2616e_passkey_entry(const struct device *dev, const char *cid, uint32_t passkey)
{
	struct i2616e_data *data = dev->data;
	char cmd[32];

	if (!i2616e_valid_cid(cid)) {
		return -EINVAL;
	}

	(void)snprintf(cmd, sizeof(cmd), "AT+PSKENTRY=%s,%u\r", cid, passkey);
	return i2616e_command(data, cmd);
}

int i2616e_whitelist_get(const struct device *dev, char *buffer, size_t buffer_size)
{
	struct i2616e_data *data = dev->data;

	if (buffer == NULL || buffer_size == 0U) {
		return -EINVAL;
	}

	return i2616e_query(data, "AT+WLST?\r", "+WLST:", buffer, buffer_size);
}

int i2616e_whitelist_add_address(const struct device *dev, const char *address)
{
	struct i2616e_data *data = dev->data;
	char cmd[24];

	if (address == NULL || strlen(address) != I2616E_ADDRESS_TEXT_SIZE) {
		return -EINVAL;
	}

	(void)snprintf(cmd, sizeof(cmd), "AT+WLST=%s\r", address);
	return i2616e_command(data, cmd);
}

int i2616e_whitelist_add_connection(const struct device *dev, const char *cid)
{
	struct i2616e_data *data = dev->data;
	char cmd[16];

	if (!i2616e_valid_cid(cid)) {
		return -EINVAL;
	}

	(void)snprintf(cmd, sizeof(cmd), "AT+WLST=%s\r", cid);
	return i2616e_command(data, cmd);
}

int i2616e_whitelist_remove_address(const struct device *dev, const char *address)
{
	struct i2616e_data *data = dev->data;
	char cmd[24];

	if (address == NULL || strlen(address) != I2616E_ADDRESS_TEXT_SIZE) {
		return -EINVAL;
	}

	(void)snprintf(cmd, sizeof(cmd), "AT+DWLST=%s\r", address);
	return i2616e_command(data, cmd);
}

int i2616e_whitelist_remove_index(const struct device *dev, uint8_t index)
{
	struct i2616e_data *data = dev->data;
	char cmd[16];

	if (index >= 10U) {
		return -EINVAL;
	}

	(void)snprintf(cmd, sizeof(cmd), "AT+DWLST=%u\r", index);
	return i2616e_command(data, cmd);
}

/* ------------------------------------------------------------------------- */
/* Initialisation                                                            */
/* ------------------------------------------------------------------------- */

static int i2616e_init(const struct device *dev)
{
	const struct i2616e_config *cfg = dev->config;
	struct i2616e_data *data = dev->data;
	const struct modem_backend_uart_config backend_config = {
		.uart = cfg->uart,
		.receive_buf = data->uart_rx_buf,
		.receive_buf_size = sizeof(data->uart_rx_buf),
		.transmit_buf = data->uart_tx_buf,
		.transmit_buf_size = sizeof(data->uart_tx_buf),
	};
	int ret;

	data->dev = dev;
	k_mutex_init(&data->cmd_lock);
	k_sem_init(&data->cmd_sem, 0, 1);
	k_sem_init(&data->ready_sem, 0, 1);
	k_work_init(&data->rx_work, i2616e_rx_work);
	k_msgq_init(&data->event_q, data->event_q_buf, sizeof(struct i2616e_event),
		    CONFIG_I2616E_EVENT_QUEUE_SIZE);

	if (!device_is_ready(cfg->uart)) {
		LOG_ERR("UART device not ready");
		return -ENODEV;
	}

	if (!gpio_is_ready_dt(&cfg->reset)) {
		LOG_ERR("reset GPIO not ready");
		return -ENODEV;
	}

	ret = gpio_pin_configure_dt(&cfg->reset, GPIO_OUTPUT_INACTIVE);
	if (ret < 0) {
		return ret;
	}

	if (cfg->sleep_enable.port != NULL) {
		if (!gpio_is_ready_dt(&cfg->sleep_enable)) {
			LOG_ERR("sleep-enable GPIO not ready");
			return -ENODEV;
		}
		/* Keep the module awake (sleep disabled). */
		ret = gpio_pin_configure_dt(&cfg->sleep_enable, GPIO_OUTPUT_INACTIVE);
		if (ret < 0) {
			return ret;
		}
	}

	data->pipe = modem_backend_uart_init(&data->backend, &backend_config);
	if (data->pipe == NULL) {
		return -ENODEV;
	}

	modem_pipe_attach(data->pipe, i2616e_pipe_callback, data);

	ret = modem_pipe_open(data->pipe, K_MSEC(100));
	if (ret < 0) {
		LOG_ERR("failed to open pipe: %d", ret);
		return ret;
	}

	LOG_INF("i2616e driver initialised");
	return 0;
}

#define I2616E_INIT(inst)								\
	static struct i2616e_data i2616e_data_##inst;					\
											\
	static const struct i2616e_config i2616e_config_##inst = {			\
		.uart = DEVICE_DT_GET(DT_INST_BUS(inst)),				\
		.reset = GPIO_DT_SPEC_INST_GET(inst, reset_gpios),			\
		.sleep_enable = GPIO_DT_SPEC_INST_GET_OR(inst, sleep_enable_gpios, {0}),	\
		.reset_assert_ms = DT_INST_PROP(inst, reset_assert_duration_ms),		\
		.ready_timeout_ms = DT_INST_PROP(inst, ready_timeout_ms),		\
	};										\
											\
	DEVICE_DT_INST_DEFINE(inst, i2616e_init, NULL,					\
			      &i2616e_data_##inst, &i2616e_config_##inst,		\
			      POST_KERNEL, CONFIG_I2616E_INIT_PRIORITY, NULL);

DT_INST_FOREACH_STATUS_OKAY(I2616E_INIT)
