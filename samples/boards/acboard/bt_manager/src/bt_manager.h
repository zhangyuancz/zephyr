/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * Bluetooth manager: a thin port over the i2616e BLE driver that maps module
 * connection ids (CIDs) to small logical channel numbers, normalises module
 * events, enforces the paired-device policy, and persists the paired-device
 * table through the Settings subsystem under cfg/bt. The application layer
 * (APP PDU protocol) talks to this port by channel number and never sees the
 * i2616e CID strings.
 */

#ifndef ACBOARD_BT_MANAGER_H_
#define ACBOARD_BT_MANAGER_H_

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include <zephyr/device.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BT_MGR_MAX_CHANNELS 3U
#define BT_MGR_MAX_DEVICES  3U
#define BT_MGR_ADDR_TEXT    12U
#define BT_MGR_NAME_MAX     29U

enum bt_mgr_event_type {
	BT_MGR_EVENT_CONNECTED,
	BT_MGR_EVENT_AUTHENTICATED,
	BT_MGR_EVENT_DISCONNECTED,
	BT_MGR_EVENT_DATA,
};

struct bt_mgr_event {
	enum bt_mgr_event_type type;
	uint8_t channel;
	char address[BT_MGR_ADDR_TEXT + 1U];	/* valid for AUTHENTICATED */
	const uint8_t *data;			/* valid for DATA */
	size_t length;
};

typedef void (*bt_mgr_callback_t)(const struct bt_mgr_event *event, void *user_data);

struct bt_mgr_device {
	char address[BT_MGR_ADDR_TEXT + 1U];
	char name[BT_MGR_NAME_MAX + 1U];
	uint8_t name_len;
	bool plug_and_charge;
};

/**
 * @brief Bring up Bluetooth: load the paired-device table, reset and configure
 *        the i2616e module, sync the whitelist, and start event processing.
 */
int bt_manager_init(const struct device *i2616e, bt_mgr_callback_t cb, void *user_data);

/** Send a PDU to a connected channel. */
int bt_manager_send(uint8_t channel, const uint8_t *data, size_t length);

/** Number of currently stored paired devices. */
size_t bt_manager_device_count(void);

/** Query whether a paired device is stored locally. */
bool bt_manager_has_device(const char *address);

/** Copy the paired-device table into @p devices and return the total count. */
int bt_manager_get_devices(struct bt_mgr_device *devices, size_t capacity, size_t *count);

/** Add or update APP-owned metadata for a paired device. */
int bt_manager_update_device(const char *address, const uint8_t *name, size_t name_len);

/** Get or set the per-device plug-and-charge flag. */
int bt_manager_get_plug_and_charge(const char *address, bool *enabled);
int bt_manager_set_plug_and_charge(const char *address, bool enabled);

/** Remove a paired device: drop it from the module whitelist and the table. */
int bt_manager_forget(const char *address);

#ifdef __cplusplus
}
#endif

#endif /* ACBOARD_BT_MANAGER_H_ */
