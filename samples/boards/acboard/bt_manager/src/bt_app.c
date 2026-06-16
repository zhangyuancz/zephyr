/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 */

#include "bt_app.h"

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(bt_app, LOG_LEVEL_INF);

#define BT_APP_START_FIELD      0xffU
#define BT_APP_NODE             0x10U
#define BT_WALLBOX_NODE         0x11U
#define BT_APP_HEADER_SIZE      4U
#define BT_APP_CHECKSUM_SIZE    1U
#define BT_APP_MAC_SIZE         6U
/* Large enough for the worst-case paired-device list response:
 * BT_MGR_MAX_DEVICES * (MAC + name-length byte + maximum name).
 */
#define BT_APP_MAX_PAYLOAD      (BT_MGR_MAX_DEVICES * (BT_APP_MAC_SIZE + 1U + BT_MGR_NAME_MAX))
#define BT_APP_MAX_FRAME        (BT_APP_HEADER_SIZE + BT_APP_MAX_PAYLOAD + BT_APP_CHECKSUM_SIZE)
#define BT_APP_AUTH_TOKEN_MIN   1U
#define BT_APP_AUTH_TOKEN_MAX   20U

#define BT_APP_CMD_AUTH_CHECK              0x02U
#define BT_APP_CMD_QUERY_PILE_STATUS       0x03U
#define BT_APP_CMD_CHARGE_CONTROL          0x04U
#define BT_APP_CMD_QUERY_PAIRED_DEVICES    0x05U
#define BT_APP_CMD_REPORT_APP_DEVICE_INFO  0x06U
#define BT_APP_CMD_DELETE_PAIRED_DEVICE    0x07U
#define BT_APP_CMD_DELETE_CURRENT_DEVICE   0x08U
#define BT_APP_CMD_PLUG_AND_CHARGE_CONTROL 0x09U

#define BT_APP_AUTH_PASSED          1U
#define BT_APP_AUTH_FAILED          2U
#define BT_APP_AUTH_DEVICE_LIMIT    4U

#define BT_APP_RESULT_UNDEFINED 0U
#define BT_APP_RESULT_SUCCESS   1U
#define BT_APP_RESULT_FAILED    2U

#define BT_APP_STATUS_IDLE                  1U
#define BT_APP_STATUS_CHARGING              2U
#define BT_APP_STATUS_CHARGE_FINISHED       3U
#define BT_APP_STATUS_PLUGGED_UNAUTHORIZED  4U

#define BT_APP_CHARGE_START 1U
#define BT_APP_CHARGE_STOP  2U

#define BT_APP_PNC_ENABLE  1U
#define BT_APP_PNC_DISABLE 2U
#define BT_APP_PNC_QUERY   3U

struct app_frame {
	uint8_t source;
	uint8_t command;
	const uint8_t *payload;
	size_t payload_len;
};

struct app_session {
	bool connected;
	bool authenticated;
	char address[BT_MGR_ADDR_TEXT + 1U];
};

static struct app_session sessions[BT_MGR_MAX_CHANNELS];
static uint8_t simulated_pile_status = BT_APP_STATUS_IDLE;

static uint8_t checksum(const uint8_t *data, size_t len)
{
	uint8_t sum = 0U;

	for (size_t i = 0; i < len; i++) {
		sum = (uint8_t)(sum + data[i]);
	}
	return sum;
}

static char lower_hex(char ch)
{
	if (ch >= 'A' && ch <= 'F') {
		return ch + ('a' - 'A');
	}
	return ch;
}

static bool address_equal(const char *a, const char *b)
{
	for (size_t i = 0; i < BT_MGR_ADDR_TEXT; i++) {
		if (lower_hex(a[i]) != lower_hex(b[i])) {
			return false;
		}
	}
	return true;
}

static int hex_nibble(uint8_t ch)
{
	if (ch >= '0' && ch <= '9') {
		return ch - '0';
	}
	if (ch >= 'A' && ch <= 'F') {
		return ch - 'A' + 10;
	}
	if (ch >= 'a' && ch <= 'f') {
		return ch - 'a' + 10;
	}
	return -EINVAL;
}

static char hex_lower(uint8_t value)
{
	value &= 0x0fU;
	return value < 10U ? (char)('0' + value) : (char)('a' + value - 10U);
}

static bool address_to_mac(const char *address, uint8_t *mac)
{
	for (size_t i = 0; i < BT_APP_MAC_SIZE; i++) {
		int hi = hex_nibble((uint8_t)address[i * 2U]);
		int lo = hex_nibble((uint8_t)address[(i * 2U) + 1U]);

		if (hi < 0 || lo < 0) {
			return false;
		}
		mac[i] = (uint8_t)((hi << 4) | lo);
	}
	return true;
}

static void mac_to_address(const uint8_t *mac, char *address)
{
	for (size_t i = 0; i < BT_APP_MAC_SIZE; i++) {
		address[i * 2U] = hex_lower(mac[i] >> 4);
		address[(i * 2U) + 1U] = hex_lower(mac[i]);
	}
	address[BT_MGR_ADDR_TEXT] = '\0';
}

static struct app_session *session_for(uint8_t channel)
{
	if (channel >= BT_MGR_MAX_CHANNELS) {
		return NULL;
	}
	return &sessions[channel];
}

static void revoke_sessions_by_address(const char *address)
{
	for (uint8_t i = 0; i < BT_MGR_MAX_CHANNELS; i++) {
		if (sessions[i].connected && sessions[i].authenticated &&
		    address_equal(sessions[i].address, address)) {
			sessions[i].authenticated = false;
			memset(sessions[i].address, 0, sizeof(sessions[i].address));
		}
	}
}

static bool find_frame(const uint8_t *data, size_t len, size_t *offset, size_t *frame_len)
{
	for (size_t i = 0; i + BT_APP_HEADER_SIZE + BT_APP_CHECKSUM_SIZE <= len; i++) {
		if (data[i] != BT_APP_START_FIELD) {
			continue;
		}

		size_t payload_len = data[i + 3U];
		size_t total = BT_APP_HEADER_SIZE + payload_len + BT_APP_CHECKSUM_SIZE;

		if (payload_len > BT_APP_MAX_PAYLOAD) {
			continue;
		}
		if (i + total > len) {
			return false;
		}
		if (data[i + total - 1U] != checksum(&data[i], BT_APP_HEADER_SIZE + payload_len)) {
			continue;
		}

		*offset = i;
		*frame_len = total;
		return true;
	}

	return false;
}

static bool decode_frame(const uint8_t *data, size_t len, struct app_frame *frame)
{
	if (data == NULL || frame == NULL || len < BT_APP_HEADER_SIZE + BT_APP_CHECKSUM_SIZE ||
	    data[0] != BT_APP_START_FIELD) {
		return false;
	}

	size_t payload_len = data[3];
	size_t total = BT_APP_HEADER_SIZE + payload_len + BT_APP_CHECKSUM_SIZE;

	if (payload_len > BT_APP_MAX_PAYLOAD || total > len ||
	    data[total - 1U] != checksum(data, BT_APP_HEADER_SIZE + payload_len)) {
		return false;
	}

	frame->source = data[1];
	frame->command = data[2];
	frame->payload = payload_len > 0U ? &data[BT_APP_HEADER_SIZE] : NULL;
	frame->payload_len = payload_len;
	return true;
}

static int send_response(uint8_t channel, uint8_t command, const uint8_t *payload, size_t payload_len)
{
	uint8_t frame[BT_APP_MAX_FRAME];
	size_t frame_len;

	if (payload_len > BT_APP_MAX_PAYLOAD || (payload == NULL && payload_len > 0U)) {
		return -EINVAL;
	}

	frame[0] = BT_APP_START_FIELD;
	frame[1] = BT_WALLBOX_NODE;
	frame[2] = command;
	frame[3] = (uint8_t)payload_len;
	if (payload_len > 0U) {
		memcpy(&frame[BT_APP_HEADER_SIZE], payload, payload_len);
	}

	frame_len = BT_APP_HEADER_SIZE + payload_len + BT_APP_CHECKSUM_SIZE;
	frame[frame_len - 1U] = checksum(frame, BT_APP_HEADER_SIZE + payload_len);

	return bt_manager_send(channel, frame, frame_len);
}

static void handle_auth_check(uint8_t channel, const struct app_frame *frame)
{
	static const uint8_t serial[12] = "ACBOARD00001";
	uint8_t payload[1U + sizeof(serial)] = { BT_APP_AUTH_FAILED };
	size_t payload_len = 1U;
	struct app_session *session = session_for(channel);

	if (session != NULL && session->authenticated && frame->payload != NULL &&
	    frame->payload_len >= BT_APP_AUTH_TOKEN_MIN &&
	    frame->payload_len <= BT_APP_AUTH_TOKEN_MAX) {
		bool known = bt_manager_has_device(session->address);
		bool full = bt_manager_device_count() >= BT_MGR_MAX_DEVICES;

		if (!known && full) {
			payload[0] = BT_APP_AUTH_DEVICE_LIMIT;
		} else {
			payload[0] = BT_APP_AUTH_PASSED;
			memcpy(&payload[1], serial, sizeof(serial));
			payload_len = sizeof(payload);
		}
	}

	LOG_INF("auth check channel=%u result=%u", channel, payload[0]);
	(void)send_response(channel, frame->command, payload, payload_len);
}

static void handle_query_pile_status(uint8_t channel, const struct app_frame *frame)
{
	ARG_UNUSED(frame);

	LOG_INF("pile status query channel=%u status=%u", channel, simulated_pile_status);
	(void)send_response(channel, BT_APP_CMD_QUERY_PILE_STATUS, &simulated_pile_status,
			    sizeof(simulated_pile_status));
}

static void handle_charge_control(uint8_t channel, const struct app_frame *frame)
{
	uint8_t result = BT_APP_RESULT_FAILED;
	struct app_session *session = session_for(channel);

	if (session != NULL && session->authenticated && frame->payload != NULL &&
	    frame->payload_len == 1U) {
		if (frame->payload[0] == BT_APP_CHARGE_START) {
			simulated_pile_status = BT_APP_STATUS_CHARGING;
			result = BT_APP_RESULT_SUCCESS;
		} else if (frame->payload[0] == BT_APP_CHARGE_STOP) {
			simulated_pile_status = BT_APP_STATUS_CHARGE_FINISHED;
			result = BT_APP_RESULT_SUCCESS;
		}
	}

	LOG_INF("charge control channel=%u result=%u", channel, result);
	(void)send_response(channel, frame->command, &result, sizeof(result));
}

static void handle_query_paired_devices(uint8_t channel, const struct app_frame *frame)
{
	ARG_UNUSED(frame);

	struct app_session *session = session_for(channel);
	struct bt_mgr_device devices[BT_MGR_MAX_DEVICES];
	uint8_t payload[BT_APP_MAX_PAYLOAD];
	size_t count = 0U;
	size_t payload_len = 0U;

	if (session == NULL || !session->authenticated) {
		LOG_WRN("paired devices query from unauthenticated channel=%u", channel);
		return;
	}

	(void)bt_manager_get_devices(devices, BT_MGR_MAX_DEVICES, &count);

	for (size_t i = 0; i < count && i < BT_MGR_MAX_DEVICES; i++) {
		uint8_t mac[BT_APP_MAC_SIZE];
		size_t name_len = devices[i].name_len;

		if (payload_len + BT_APP_MAC_SIZE + 1U + name_len > sizeof(payload)) {
			break;
		}
		if (!address_to_mac(devices[i].address, mac)) {
			continue;
		}

		memcpy(&payload[payload_len], mac, sizeof(mac));
		payload_len += sizeof(mac);
		payload[payload_len++] = (uint8_t)name_len;
		memcpy(&payload[payload_len], devices[i].name, name_len);
		payload_len += name_len;
	}

	LOG_INF("paired devices query channel=%u len=%zu", channel, payload_len);
	(void)send_response(channel, BT_APP_CMD_QUERY_PAIRED_DEVICES, payload, payload_len);
}

static void handle_report_app_device_info(uint8_t channel, const struct app_frame *frame)
{
	uint8_t result = BT_APP_RESULT_FAILED;
	struct app_session *session = session_for(channel);

	if (session != NULL && session->authenticated && frame->payload != NULL &&
	    frame->payload_len > 0U && frame->payload_len <= BT_MGR_NAME_MAX) {
		bool known = bt_manager_has_device(session->address);
		bool full = bt_manager_device_count() >= BT_MGR_MAX_DEVICES;

		if ((known || !full) &&
		    bt_manager_update_device(session->address, frame->payload, frame->payload_len) == 0) {
			result = BT_APP_RESULT_SUCCESS;
		}
	}

	LOG_INF("app device info channel=%u result=%u", channel, result);
	(void)send_response(channel, frame->command, &result, sizeof(result));
}

static void handle_delete_paired_device(uint8_t channel, const struct app_frame *frame)
{
	uint8_t result = BT_APP_RESULT_FAILED;
	struct app_session *session = session_for(channel);
	char address[BT_MGR_ADDR_TEXT + 1U];

	if (session != NULL && session->authenticated && frame->payload != NULL &&
	    frame->payload_len == BT_APP_MAC_SIZE) {
		mac_to_address(frame->payload, address);
		if (bt_manager_forget(address) == 0) {
			revoke_sessions_by_address(address);
			result = BT_APP_RESULT_SUCCESS;
		}
	}

	LOG_INF("delete paired device channel=%u result=%u", channel, result);
	(void)send_response(channel, frame->command, &result, sizeof(result));
}

static void handle_delete_current_device(uint8_t channel, const struct app_frame *frame)
{
	uint8_t result = BT_APP_RESULT_FAILED;
	struct app_session *session = session_for(channel);
	char address[BT_MGR_ADDR_TEXT + 1U];

	ARG_UNUSED(frame);

	if (session != NULL && session->authenticated) {
		strncpy(address, session->address, sizeof(address));
		if (bt_manager_forget(address) == 0) {
			revoke_sessions_by_address(address);
			result = BT_APP_RESULT_SUCCESS;
		}
	}

	LOG_INF("delete current device channel=%u result=%u", channel, result);
	(void)send_response(channel, BT_APP_CMD_DELETE_CURRENT_DEVICE, &result, sizeof(result));
}

static void handle_plug_and_charge_control(uint8_t channel, const struct app_frame *frame)
{
	uint8_t result = BT_APP_RESULT_UNDEFINED;
	struct app_session *session = session_for(channel);
	uint8_t op = frame->payload != NULL && frame->payload_len == 1U ? frame->payload[0] : 0U;

	if (session != NULL && session->authenticated && frame->payload != NULL &&
	    frame->payload_len == 1U && bt_manager_has_device(session->address)) {
		bool enabled = false;

		switch (frame->payload[0]) {
		case BT_APP_PNC_ENABLE:
			if (bt_manager_set_plug_and_charge(session->address, true) == 0) {
				result = BT_APP_PNC_ENABLE;
			}
			break;
		case BT_APP_PNC_DISABLE:
			if (bt_manager_set_plug_and_charge(session->address, false) == 0) {
				result = BT_APP_PNC_DISABLE;
			}
			break;
		case BT_APP_PNC_QUERY:
			if (bt_manager_get_plug_and_charge(session->address, &enabled) == 0) {
				result = enabled ? BT_APP_PNC_ENABLE : BT_APP_PNC_DISABLE;
			}
			break;
		default:
			break;
		}
	}

	LOG_INF("plug-and-charge channel=%u op=%u result=%u", channel, op, result);
	(void)send_response(channel, frame->command, &result, sizeof(result));
}

static void dispatch_frame(uint8_t channel, const struct app_frame *frame)
{
	if (frame->source != BT_APP_NODE) {
		LOG_WRN("unexpected source node 0x%02x", frame->source);
		return;
	}

	switch (frame->command) {
	case BT_APP_CMD_AUTH_CHECK:
		handle_auth_check(channel, frame);
		break;
	case BT_APP_CMD_QUERY_PILE_STATUS:
		handle_query_pile_status(channel, frame);
		break;
	case BT_APP_CMD_CHARGE_CONTROL:
		handle_charge_control(channel, frame);
		break;
	case BT_APP_CMD_QUERY_PAIRED_DEVICES:
		handle_query_paired_devices(channel, frame);
		break;
	case BT_APP_CMD_REPORT_APP_DEVICE_INFO:
		handle_report_app_device_info(channel, frame);
		break;
	case BT_APP_CMD_DELETE_PAIRED_DEVICE:
		handle_delete_paired_device(channel, frame);
		break;
	case BT_APP_CMD_DELETE_CURRENT_DEVICE:
		handle_delete_current_device(channel, frame);
		break;
	case BT_APP_CMD_PLUG_AND_CHARGE_CONTROL:
		handle_plug_and_charge_control(channel, frame);
		break;
	default:
		LOG_WRN("unsupported app command 0x%02x", frame->command);
		break;
	}
}

static void handle_data(uint8_t channel, const uint8_t *data, size_t len)
{
	size_t offset = 0U;
	size_t frame_len = 0U;
	struct app_frame frame;

	if (!find_frame(data, len, &offset, &frame_len) ||
	    !decode_frame(&data[offset], frame_len, &frame)) {
		LOG_WRN("invalid app frame channel=%u len=%zu", channel, len);
		return;
	}

	LOG_INF("app frame channel=%u cmd=0x%02x payload=%zu", channel, frame.command,
		frame.payload_len);
	dispatch_frame(channel, &frame);
}

void bt_app_on_event(const struct bt_mgr_event *event)
{
	struct app_session *session;

	if (event == NULL) {
		return;
	}

	session = session_for(event->channel);
	if (session == NULL) {
		return;
	}

	switch (event->type) {
	case BT_MGR_EVENT_CONNECTED:
		memset(session, 0, sizeof(*session));
		session->connected = true;
		LOG_INF("connected channel=%u", event->channel);
		break;
	case BT_MGR_EVENT_AUTHENTICATED:
		session->connected = true;
		session->authenticated = true;
		strncpy(session->address, event->address, BT_MGR_ADDR_TEXT);
		LOG_INF("authenticated channel=%u addr=%s", event->channel, session->address);
		break;
	case BT_MGR_EVENT_DISCONNECTED:
		memset(session, 0, sizeof(*session));
		LOG_INF("disconnected channel=%u", event->channel);
		break;
	case BT_MGR_EVENT_DATA:
		handle_data(event->channel, event->data, event->length);
		break;
	default:
		break;
	}
}
