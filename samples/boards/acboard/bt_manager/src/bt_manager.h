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

/**
 * @brief Bring up Bluetooth: load the paired-device table, reset and configure
 *        the i2616e module, sync the whitelist, and start event processing.
 */
int bt_manager_init(const struct device *i2616e, bt_mgr_callback_t cb, void *user_data);

/** Send a PDU to a connected channel. */
int bt_manager_send(uint8_t channel, const uint8_t *data, size_t length);

/** Number of currently stored paired devices. */
size_t bt_manager_device_count(void);

/** Remove a paired device: drop it from the module whitelist and the table. */
int bt_manager_forget(const char *address);

#ifdef __cplusplus
}
#endif

#endif /* ACBOARD_BT_MANAGER_H_ */
