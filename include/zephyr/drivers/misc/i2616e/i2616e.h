/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_INCLUDE_DRIVERS_MISC_I2616E_H_
#define ZEPHYR_INCLUDE_DRIVERS_MISC_I2616E_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/kernel.h>

#ifdef __cplusplus
extern "C" {
#endif

#define I2616E_CID_TEXT_SIZE 4U
#define I2616E_ADDRESS_TEXT_SIZE 12U
#define I2616E_NAME_MAX_SIZE 29U
#define I2616E_VERSION_MAX_SIZE 64U
#define I2616E_PDU_MAX_SIZE 512U
#define I2616E_WHITELIST_MAX_SIZE 192U
#define I2616E_MAX_CONNECTIONS 4U

enum i2616e_event_type {
	I2616E_EVENT_READY,
	I2616E_EVENT_CONNECTED,
	I2616E_EVENT_DISCONNECTED,
	I2616E_EVENT_PAIR_REQUEST,
	I2616E_EVENT_PASSKEY_DISPLAY,
	I2616E_EVENT_PASSKEY_REQUEST,
	I2616E_EVENT_PAIRED,
	I2616E_EVENT_DATA_RECEIVED,
	I2616E_EVENT_CONNECTION_TIMEOUT,
	I2616E_EVENT_SERVICE_MISMATCH,
};

struct i2616e_event {
	enum i2616e_event_type type;
	char cid[I2616E_CID_TEXT_SIZE + 1U];
	char address[I2616E_ADDRESS_TEXT_SIZE + 1U];
	uint32_t passkey;
	size_t length;
	uint8_t data[I2616E_PDU_MAX_SIZE];
};

struct i2616e_info {
	char firmware_version[I2616E_VERSION_MAX_SIZE + 1U];
	char config_version[I2616E_VERSION_MAX_SIZE + 1U];
	char name[I2616E_NAME_MAX_SIZE + 1U];
	char address[I2616E_ADDRESS_TEXT_SIZE + 1U];
	uint32_t baudrate;
	bool flow_control;
	uint8_t bluetooth_mode;
};

struct i2616e_settings {
	const char *name;
	uint8_t bluetooth_mode;
	bool silent;
	bool command_mode;
	bool multi_connection;
	bool auto_unlock;
	bool advertising;
};

int i2616e_hardware_reset(const struct device *dev);
int i2616e_get_info(const struct device *dev, struct i2616e_info *info);
int i2616e_apply_settings(const struct device *dev, const struct i2616e_settings *settings);
int i2616e_get_event(const struct device *dev, struct i2616e_event *event,
		     k_timeout_t timeout);
int i2616e_send(const struct device *dev, const char *cid, const uint8_t *payload,
		size_t length);
int i2616e_get_connection_mtu(const struct device *dev, const char *cid, uint16_t *mtu);
int i2616e_pair_confirm(const struct device *dev, const char *cid, bool accept);
int i2616e_passkey_entry(const struct device *dev, const char *cid, uint32_t passkey);
int i2616e_whitelist_get(const struct device *dev, char *buffer, size_t buffer_size);
int i2616e_whitelist_add_address(const struct device *dev, const char *address);
int i2616e_whitelist_add_connection(const struct device *dev, const char *cid);
int i2616e_whitelist_remove_address(const struct device *dev, const char *address);
int i2616e_whitelist_remove_index(const struct device *dev, uint8_t index);

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_DRIVERS_MISC_I2616E_H_ */
