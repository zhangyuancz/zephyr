/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * Lynq L511 LTE Cat.1 cellular modem driver.
 *
 * The L511 speaks a plain 3GPP AT command set and provides connectivity through
 * a PPP dial-up data session rather than 27.010 multiplexing, so it does not fit
 * the CMUX-based generic modem_cellular driver. This is a dedicated, lightweight
 * driver: it powers the module, drives the AT init/registration/dial sequence
 * directly over the UART pipe (poll-based receive), then hands the pipe to
 * modem_ppp which brings up the PPP network interface. A per-instance thread
 * runs (and retries) the bring-up.
 */

#define DT_DRV_COMPAT lynq_l511

#include <stdlib.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/modem/backend/uart.h>
#include <zephyr/modem/chat.h>
#include <zephyr/modem/pipe.h>
#include <zephyr/modem/ppp.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/ppp.h>

LOG_MODULE_REGISTER(lynq_l511, CONFIG_MODEM_LYNQ_L511_LOG_LEVEL);

#define L511_UART_RX_BUF_SIZE  2048U
#define L511_UART_TX_BUF_SIZE  1024U
#define L511_RESP_BUF_SIZE     768U
#define L511_CHAT_RX_BUF_SIZE  256U
#define L511_CHAT_ARGV_SIZE    8U
#define L511_PPP_MTU           1500
#define L511_PPP_BUF_SIZE      512

#define L511_EVENT_LINK_DOWN   BIT(0)

struct l511_config {
	const struct device *uart;
	struct gpio_dt_spec power_gpio;
	struct gpio_dt_spec reset_gpio;
	uint32_t reset_pulse_ms;
	uint32_t boot_drain_timeout_ms;
	uint32_t at_ready_delay_ms;
	struct modem_ppp *ppp;
};

struct l511_data {
	const struct device *dev;
	struct modem_backend_uart backend;
	struct modem_chat chat;
	struct modem_pipe *pipe;
	struct net_mgmt_event_callback net_cb;
	struct k_event events;
	uint8_t uart_rx_buf[L511_UART_RX_BUF_SIZE];
	uint8_t uart_tx_buf[L511_UART_TX_BUF_SIZE];
	uint8_t resp_buf[L511_RESP_BUF_SIZE];
	uint8_t chat_rx_buf[L511_CHAT_RX_BUF_SIZE];
	uint8_t chat_delimiter[1];
	uint8_t chat_filter[1];
	uint8_t *chat_argv[L511_CHAT_ARGV_SIZE];
	bool registered;
};

static void l511_net_event_handler(struct net_mgmt_event_callback *cb, uint64_t event,
				   struct net_if *iface)
{
	struct l511_data *data = CONTAINER_OF(cb, struct l511_data, net_cb);
	const struct l511_config *cfg = data->dev->config;
	struct net_if *ppp_iface = modem_ppp_get_iface(cfg->ppp);

	if (iface != NULL && iface != ppp_iface) {
		return;
	}

	switch (event) {
	case NET_EVENT_PPP_PHASE_DEAD:
	case NET_EVENT_PPP_CARRIER_OFF:
		LOG_DBG("link-down event 0x%llx", event);
		k_event_post(&data->events, L511_EVENT_LINK_DOWN);
		break;
	default:
		break;
	}
}

/* ------------------------------------------------------------------------- */
/* AT chat scripts                                                           */
/* ------------------------------------------------------------------------- */

static void l511_chat_on_line(struct modem_chat *chat, char **argv, uint16_t argc,
			      void *user_data)
{
	ARG_UNUSED(chat);
	ARG_UNUSED(user_data);

	if (argc >= 2U && argv[1][0] != '\0') {
		LOG_DBG("AT: %s", argv[1]);
	}
}

static void l511_chat_on_cpin(struct modem_chat *chat, char **argv, uint16_t argc,
			      void *user_data)
{
	ARG_UNUSED(chat);
	ARG_UNUSED(user_data);

	if (argc >= 2U) {
		LOG_DBG("AT: +CPIN: %s", argv[1]);
	}
}

static void l511_chat_on_csq(struct modem_chat *chat, char **argv, uint16_t argc,
			     void *user_data)
{
	ARG_UNUSED(chat);
	ARG_UNUSED(user_data);

	if (argc >= 3U) {
		LOG_DBG("AT: +CSQ: %s,%s", argv[1], argv[2]);
	}
}

static void l511_chat_on_cereg(struct modem_chat *chat, char **argv, uint16_t argc,
			       void *user_data)
{
	struct l511_data *data = user_data;
	int stat;
	uint8_t base;

	ARG_UNUSED(chat);

	if (argc >= 3U && argv[2][0] != '"') {
		base = 2U;
		LOG_DBG("AT: +CEREG: %s,%s", argv[1], argv[2]);
	} else if (argc >= 2U) {
		base = 1U;
		LOG_DBG("AT: +CEREG: %s", argv[1]);
	} else {
		return;
	}

	stat = atoi(argv[base]);
	data->registered = (stat == 1) || (stat == 5);
}

static void l511_chat_on_eciccid(struct modem_chat *chat, char **argv, uint16_t argc,
				 void *user_data)
{
	ARG_UNUSED(chat);
	ARG_UNUSED(user_data);

	if (argc >= 2U) {
		LOG_DBG("AT: +ECICCID: %s", argv[1]);
	}
}

static void l511_chat_on_cgatt(struct modem_chat *chat, char **argv, uint16_t argc,
			       void *user_data)
{
	ARG_UNUSED(chat);
	ARG_UNUSED(user_data);

	if (argc >= 2U) {
		LOG_DBG("AT: +CGATT: %s", argv[1]);
	}
}

MODEM_CHAT_MATCH_DEFINE(ok_match, "OK", "", NULL);
MODEM_CHAT_MATCH_DEFINE(any_line_match, "", "", l511_chat_on_line);
MODEM_CHAT_MATCH_DEFINE(cpin_match, "+CPIN: ", "", l511_chat_on_cpin);
MODEM_CHAT_MATCH_DEFINE(csq_match, "+CSQ: ", ",", l511_chat_on_csq);
MODEM_CHAT_MATCH_DEFINE(cereg_match, "+CEREG: ", ",", l511_chat_on_cereg);
MODEM_CHAT_MATCH_DEFINE(eciccid_match, "+ECICCID: ", "", l511_chat_on_eciccid);
MODEM_CHAT_MATCH_DEFINE(cgatt_match, "+CGATT: ", "", l511_chat_on_cgatt);

MODEM_CHAT_MATCHES_DEFINE(abort_matches,
			  MODEM_CHAT_MATCH("ERROR", "", NULL),
			  MODEM_CHAT_MATCH("NO CARRIER", "", NULL));

MODEM_CHAT_SCRIPT_CMDS_DEFINE(
	init_script_cmds,
	MODEM_CHAT_SCRIPT_CMD_RESP("AT", ok_match),
	MODEM_CHAT_SCRIPT_CMD_RESP("ATE0", ok_match),
	MODEM_CHAT_SCRIPT_CMD_RESP("ATI", any_line_match),
	MODEM_CHAT_SCRIPT_CMD_RESP("", ok_match));

MODEM_CHAT_SCRIPT_DEFINE(init_script, init_script_cmds, abort_matches, NULL,
			 CONFIG_MODEM_LYNQ_L511_INIT_SCRIPT_TIMEOUT_SEC);

MODEM_CHAT_SCRIPT_CMDS_DEFINE(
	sim_script_cmds,
	MODEM_CHAT_SCRIPT_CMD_RESP("AT+CPIN?", cpin_match),
	MODEM_CHAT_SCRIPT_CMD_RESP("", ok_match));

MODEM_CHAT_SCRIPT_DEFINE(sim_script, sim_script_cmds, abort_matches, NULL,
			 CONFIG_MODEM_LYNQ_L511_QUERY_SCRIPT_TIMEOUT_SEC);

MODEM_CHAT_SCRIPT_CMDS_DEFINE(
	signal_script_cmds,
	MODEM_CHAT_SCRIPT_CMD_RESP("AT+CSQ", csq_match),
	MODEM_CHAT_SCRIPT_CMD_RESP("", ok_match));

MODEM_CHAT_SCRIPT_DEFINE(signal_script, signal_script_cmds, abort_matches, NULL,
			 CONFIG_MODEM_LYNQ_L511_QUERY_SCRIPT_TIMEOUT_SEC);

MODEM_CHAT_SCRIPT_CMDS_DEFINE(
	registration_script_cmds,
	MODEM_CHAT_SCRIPT_CMD_RESP("AT+CEREG?", cereg_match),
	MODEM_CHAT_SCRIPT_CMD_RESP("", ok_match));

MODEM_CHAT_SCRIPT_DEFINE(registration_script, registration_script_cmds, abort_matches, NULL,
			 CONFIG_MODEM_LYNQ_L511_QUERY_SCRIPT_TIMEOUT_SEC);

MODEM_CHAT_SCRIPT_CMDS_DEFINE(
	identity_script_cmds,
	MODEM_CHAT_SCRIPT_CMD_RESP("AT+ECICCID", eciccid_match),
	MODEM_CHAT_SCRIPT_CMD_RESP("", ok_match),
	MODEM_CHAT_SCRIPT_CMD_RESP("AT+CIMI", any_line_match),
	MODEM_CHAT_SCRIPT_CMD_RESP("", ok_match));

MODEM_CHAT_SCRIPT_DEFINE(identity_script, identity_script_cmds, abort_matches, NULL,
			 CONFIG_MODEM_LYNQ_L511_IDENTITY_SCRIPT_TIMEOUT_SEC);

MODEM_CHAT_SCRIPT_CMDS_DEFINE(
	attach_script_cmds,
	MODEM_CHAT_SCRIPT_CMD_RESP("AT+CGATT?", cgatt_match),
	MODEM_CHAT_SCRIPT_CMD_RESP("", ok_match),
	MODEM_CHAT_SCRIPT_CMD_RESP("AT+CGDCONT=1,\"IP\",\"" CONFIG_MODEM_LYNQ_L511_APN "\"",
				   ok_match));

MODEM_CHAT_SCRIPT_DEFINE(attach_script, attach_script_cmds, abort_matches, NULL,
			 CONFIG_MODEM_LYNQ_L511_ATTACH_SCRIPT_TIMEOUT_SEC);

/* ------------------------------------------------------------------------- */
/* Raw AT over the UART pipe                                                 */
/* ------------------------------------------------------------------------- */

static int l511_write_all(struct l511_data *data, const uint8_t *buf, size_t len)
{
	size_t off = 0U;
	int64_t deadline = k_uptime_get() + 1000;

	while (off < len) {
		int ret = modem_pipe_transmit(data->pipe, &buf[off], len - off);

		if (ret < 0) {
			return ret;
		}
		if (ret == 0) {
			if (k_uptime_get() >= deadline) {
				return -ETIMEDOUT;
			}
			k_msleep(1);
			continue;
		}
		off += ret;
		deadline = k_uptime_get() + 1000;
	}

	return 0;
}

/* Read into resp_buf until the line is idle for @idle_ms; NUL-terminated. */
static size_t l511_collect(struct l511_data *data, int idle_ms)
{
	size_t total = 0U;
	int64_t deadline = k_uptime_get() + idle_ms;

	while (k_uptime_get() < deadline && total < (sizeof(data->resp_buf) - 1U)) {
		int ret = modem_pipe_receive(data->pipe, &data->resp_buf[total],
					     sizeof(data->resp_buf) - 1U - total);

		if (ret < 0) {
			break;
		}
		if (ret == 0) {
			k_msleep(5);
			continue;
		}
		total += ret;
		deadline = k_uptime_get() + idle_ms;
	}

	data->resp_buf[total] = '\0';
	return total;
}

static int l511_chat_run(struct l511_data *data, const struct modem_chat_script *script)
{
	int ret;

	ret = modem_chat_attach(&data->chat, data->pipe);
	if (ret < 0) {
		return ret;
	}

	ret = modem_chat_run_script(&data->chat, script);
	modem_chat_release(&data->chat);
	return ret;
}

/* ------------------------------------------------------------------------- */
/* Bring-up                                                                  */
/* ------------------------------------------------------------------------- */

static void l511_power_on(const struct l511_config *cfg)
{
	(void)gpio_pin_set_dt(&cfg->power_gpio, 1);
	(void)gpio_pin_set_dt(&cfg->reset_gpio, 1);
	k_msleep(cfg->reset_pulse_ms);
	(void)gpio_pin_set_dt(&cfg->reset_gpio, 0);
}

static int l511_run_init(struct l511_data *data)
{
	int ret;

	ret = l511_chat_run(data, &init_script);
	if (ret < 0) {
		LOG_ERR("init script failed: %d", ret);
		return ret;
	}

	return 0;
}

static int l511_network_ready(struct l511_data *data)
{
	int ret;

	data->registered = false;
	ret = l511_chat_run(data, &sim_script);
	if (ret < 0) {
		LOG_WRN("SIM not ready (%d)", ret);
		return -ENODEV;
	}

	ret = l511_chat_run(data, &signal_script);
	if (ret < 0) {
		LOG_WRN("signal query failed (%d)", ret);
		return ret;
	}

	ret = l511_chat_run(data, &registration_script);
	if (ret < 0) {
		LOG_WRN("registration query failed (%d)", ret);
		return ret;
	}
	if (!data->registered) {
		LOG_INF("not registered yet");
		return -EAGAIN;
	}

	ret = l511_chat_run(data, &identity_script);
	if (ret < 0) {
		LOG_WRN("SIM identity query failed (%d)", ret);
		return ret;
	}

	ret = l511_chat_run(data, &attach_script);
	if (ret < 0) {
		LOG_WRN("packet attach/PDP setup failed (%d)", ret);
		return ret;
	}

	return 0;
}

static int l511_dial(struct l511_data *data)
{
	char line[160];
	size_t len = 0U;
	int64_t deadline;
	int ret;

	LOG_DBG("AT: ATD*99#");
	ret = l511_write_all(data, (const uint8_t *)"ATD*99#\r\n", 9U);
	if (ret < 0) {
		return ret;
	}

	deadline = k_uptime_get() + CONFIG_MODEM_LYNQ_L511_DIAL_TIMEOUT_MS;
	while (k_uptime_get() < deadline) {
		uint8_t b;

		ret = modem_pipe_receive(data->pipe, &b, 1U);
		if (ret < 0) {
			return ret;
		}
		if (ret == 0) {
			k_msleep(5);
			continue;
		}
		if (b == '\r') {
			continue;
		}
		if (b == '\n') {
			if (len == 0U) {
				continue;
			}
			line[len] = '\0';
			LOG_DBG("AT: %s", line);
			if (strcmp(line, "CONNECT") == 0) {
				return 0;
			}
			len = 0U;
			continue;
		}
		if (len < sizeof(line) - 1U) {
			line[len++] = (char)b;
		}
	}

	return -ETIMEDOUT;
}

static int l511_bringup(const struct device *dev)
{
	const struct l511_config *cfg = dev->config;
	struct l511_data *data = dev->data;
	struct net_if *iface = modem_ppp_get_iface(cfg->ppp);
	int ret;

	LOG_INF("bring-up begin");
	k_event_clear(&data->events, L511_EVENT_LINK_DOWN);

	ret = modem_pipe_open(data->pipe, K_MSEC(1000));
	if (ret < 0) {
		LOG_ERR("pipe open failed: %d", ret);
		return ret;
	}
	LOG_DBG("pipe open");

	l511_power_on(cfg);
	(void)l511_collect(data, cfg->boot_drain_timeout_ms);
	k_msleep(cfg->at_ready_delay_ms);

	ret = l511_run_init(data);
	if (ret < 0) {
		LOG_ERR("init failed: %d", ret);
		goto close_pipe;
	}

	ret = -ETIMEDOUT;
	int sim_not_ready_count = 0;
	for (int i = 0; i < CONFIG_MODEM_LYNQ_L511_REGISTRATION_RETRY_COUNT; i++) {
		ret = l511_network_ready(data);
		if (ret == 0) {
			ret = 0;
			break;
		}

		if (ret == -ENODEV) {
			sim_not_ready_count++;
			if (sim_not_ready_count >= CONFIG_MODEM_LYNQ_L511_SIM_READY_RESTART_RETRY_COUNT) {
				LOG_WRN("SIM not ready after %d tries, restarting bring-up",
					sim_not_ready_count);
				break;
			}
			LOG_INF("SIM not ready, retry %d/%d",
				sim_not_ready_count,
				CONFIG_MODEM_LYNQ_L511_SIM_READY_RESTART_RETRY_COUNT);
			k_msleep(CONFIG_MODEM_LYNQ_L511_REGISTRATION_RETRY_INTERVAL_MS);
			continue;
		} else {
			sim_not_ready_count = 0;
		}

		LOG_INF("network not ready, retry %d/%d", i + 1,
			CONFIG_MODEM_LYNQ_L511_REGISTRATION_RETRY_COUNT);
		k_msleep(CONFIG_MODEM_LYNQ_L511_REGISTRATION_RETRY_INTERVAL_MS);
	}
	if (ret < 0) {
		if (ret == -ENODEV) {
			LOG_WRN("SIM unavailable, restarting bring-up");
		} else if (ret == -EAGAIN) {
			LOG_WRN("network registration not ready after %d retries",
				CONFIG_MODEM_LYNQ_L511_REGISTRATION_RETRY_COUNT);
		} else {
			LOG_WRN("network readiness failed after %d retries (%d)",
				CONFIG_MODEM_LYNQ_L511_REGISTRATION_RETRY_COUNT, ret);
		}
		goto close_pipe;
	}
	LOG_INF("registered");

	ret = l511_dial(data);
	if (ret < 0) {
		LOG_ERR("dial failed: %d", ret);
		goto close_pipe;
	}

	ret = modem_ppp_attach(cfg->ppp, data->pipe);
	if (ret < 0) {
		LOG_ERR("ppp attach failed: %d", ret);
		goto close_pipe;
	}
	net_if_carrier_on(iface);
	ret = net_if_up(iface);
	if (ret == -EALREADY) {
		LOG_DBG("ppp iface already up");
		ret = 0;
	}
	if (ret < 0) {
		LOG_ERR("ppp iface up failed: %d", ret);
		goto release_ppp;
	}
	net_if_dormant_off(iface);

	LOG_INF("data session up, PPP attached");
	return 0;

release_ppp:
	modem_ppp_release(cfg->ppp);
	net_if_carrier_off(iface);
	net_if_dormant_on(iface);
close_pipe:
	(void)modem_pipe_close(data->pipe, K_MSEC(1000));
	LOG_DBG("pipe closed after failed bring-up (%d)", ret);
	return ret;
}

static void l511_teardown(const struct device *dev)
{
	const struct l511_config *cfg = dev->config;
	struct l511_data *data = dev->data;
	struct net_if *iface = modem_ppp_get_iface(cfg->ppp);

	LOG_INF("PPP link down, tearing down");
	net_if_carrier_off(iface);
	net_if_dormant_on(iface);
	int ret = net_if_down(iface);
	if (ret == -EAGAIN) {
		LOG_DBG("ppp iface down still settling (%d)", ret);
	} else if (ret < 0 && ret != -EALREADY) {
		LOG_WRN("ppp iface down returned %d", ret);
	}
	modem_ppp_release(cfg->ppp);
	(void)modem_pipe_close(data->pipe, K_MSEC(1000));
	LOG_INF("teardown complete");
}

static void l511_bringup_thread(void *p1, void *p2, void *p3)
{
	const struct device *dev = p1;
	struct l511_data *data = dev->data;

	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (true) {
		if (l511_bringup(dev) < 0) {
			LOG_INF("bring-up failed, retrying in %d ms",
				CONFIG_MODEM_LYNQ_L511_BRINGUP_RETRY_INTERVAL_MS);
			k_msleep(CONFIG_MODEM_LYNQ_L511_BRINGUP_RETRY_INTERVAL_MS);
			continue;
		}

		LOG_DBG("waiting for link-down event");
		(void)k_event_wait(&data->events, L511_EVENT_LINK_DOWN, true, K_FOREVER);
		l511_teardown(dev);
		LOG_INF("reconnecting in %d ms", CONFIG_MODEM_LYNQ_L511_BRINGUP_RETRY_INTERVAL_MS);
		k_msleep(CONFIG_MODEM_LYNQ_L511_BRINGUP_RETRY_INTERVAL_MS);
	}
}

/* ------------------------------------------------------------------------- */
/* Init                                                                      */
/* ------------------------------------------------------------------------- */

static int l511_init(const struct device *dev)
{
	const struct l511_config *cfg = dev->config;
	struct l511_data *data = dev->data;
	const struct modem_backend_uart_config backend_config = {
		.uart = cfg->uart,
		.receive_buf = data->uart_rx_buf,
		.receive_buf_size = sizeof(data->uart_rx_buf),
		.transmit_buf = data->uart_tx_buf,
		.transmit_buf_size = sizeof(data->uart_tx_buf),
	};
	const struct modem_chat_config chat_config = {
		.user_data = data,
		.receive_buf = data->chat_rx_buf,
		.receive_buf_size = sizeof(data->chat_rx_buf),
		.delimiter = data->chat_delimiter,
		.delimiter_size = sizeof(data->chat_delimiter),
		.filter = data->chat_filter,
		.filter_size = sizeof(data->chat_filter),
		.argv = data->chat_argv,
		.argv_size = ARRAY_SIZE(data->chat_argv),
		.unsol_matches = NULL,
		.unsol_matches_size = 0,
	};
	int ret;

	data->dev = dev;
	data->chat_delimiter[0] = '\r';
	data->chat_filter[0] = '\n';
	k_event_init(&data->events);

	if (!device_is_ready(cfg->uart)) {
		LOG_ERR("UART not ready");
		return -ENODEV;
	}
	if (!gpio_is_ready_dt(&cfg->power_gpio) || !gpio_is_ready_dt(&cfg->reset_gpio)) {
		LOG_ERR("modem GPIOs not ready");
		return -ENODEV;
	}

	ret = gpio_pin_configure_dt(&cfg->power_gpio, GPIO_OUTPUT_INACTIVE);
	if (ret < 0) {
		return ret;
	}
	ret = gpio_pin_configure_dt(&cfg->reset_gpio, GPIO_OUTPUT_INACTIVE);
	if (ret < 0) {
		return ret;
	}

	data->pipe = modem_backend_uart_init(&data->backend, &backend_config);
	if (data->pipe == NULL) {
		return -ENODEV;
	}

	ret = modem_chat_init(&data->chat, &chat_config);
	if (ret < 0) {
		return ret;
	}

	net_mgmt_init_event_callback(&data->net_cb, l511_net_event_handler,
				     NET_EVENT_PPP_PHASE_DEAD | NET_EVENT_PPP_CARRIER_OFF);
	net_mgmt_add_event_callback(&data->net_cb);

	return 0;
}

#define L511_INIT(inst)									\
	MODEM_PPP_DEFINE(l511_ppp_##inst, NULL, CONFIG_KERNEL_INIT_PRIORITY_DEVICE,	\
			 L511_PPP_MTU, L511_PPP_BUF_SIZE);				\
											\
	static struct l511_data l511_data_##inst;					\
											\
	static const struct l511_config l511_config_##inst = {				\
		.uart = DEVICE_DT_GET(DT_INST_BUS(inst)),				\
		.power_gpio = GPIO_DT_SPEC_INST_GET(inst, mdm_power_gpios),		\
		.reset_gpio = GPIO_DT_SPEC_INST_GET(inst, mdm_reset_gpios),		\
		.reset_pulse_ms = DT_INST_PROP(inst, reset_pulse_ms),			\
		.boot_drain_timeout_ms = DT_INST_PROP(inst, boot_drain_timeout_ms),	\
		.at_ready_delay_ms = DT_INST_PROP(inst, at_ready_delay_ms),		\
		.ppp = &l511_ppp_##inst,						\
	};										\
											\
	K_THREAD_STACK_DEFINE(l511_stack_##inst, CONFIG_MODEM_LYNQ_L511_THREAD_STACK_SIZE); \
	static struct k_thread l511_thread_##inst;					\
											\
	static int l511_init_##inst(const struct device *dev)				\
	{										\
		int ret = l511_init(dev);						\
											\
		if (ret < 0) {								\
			return ret;							\
		}									\
		k_thread_create(&l511_thread_##inst, l511_stack_##inst,			\
				K_THREAD_STACK_SIZEOF(l511_stack_##inst),		\
				l511_bringup_thread, (void *)dev, NULL, NULL,		\
				CONFIG_MODEM_LYNQ_L511_THREAD_PRIORITY, 0, K_NO_WAIT);	\
		k_thread_name_set(&l511_thread_##inst, "lynq_l511");			\
		return 0;								\
	}										\
											\
	DEVICE_DT_INST_DEFINE(inst, l511_init_##inst, NULL, &l511_data_##inst,		\
			      &l511_config_##inst, POST_KERNEL,				\
			      CONFIG_MODEM_LYNQ_L511_INIT_PRIORITY, NULL);

DT_INST_FOREACH_STATUS_OKAY(L511_INIT)
