/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 */

#include "bt_manager.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/drivers/misc/i2616e/i2616e.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>

LOG_MODULE_REGISTER(bt_manager, LOG_LEVEL_INF);

#define BT_MGR_STACK_SIZE 2048
#define BT_MGR_PRIORITY   5

/* Persisted paired-device record. The version byte lets fields grow later. */
struct bt_device {
	uint8_t version;
	uint8_t pnc;				/* plug-and-charge enabled */
	char address[BT_MGR_ADDR_TEXT + 1U];
	char name[BT_MGR_NAME_MAX + 1U];
};

#define BT_DEVICE_VERSION 1U

/* Runtime mapping of a module CID to a logical channel. */
struct bt_channel {
	bool in_use;
	bool authenticated;
	char cid[I2616E_CID_TEXT_SIZE + 1U];
	char address[BT_MGR_ADDR_TEXT + 1U];
};

static struct {
	const struct device *dev;
	bt_mgr_callback_t cb;
	void *user_data;
	struct k_mutex lock;
	struct bt_channel channels[BT_MGR_MAX_CHANNELS];
	struct bt_device devices[BT_MGR_MAX_DEVICES];
	bool device_used[BT_MGR_MAX_DEVICES];
} ctx;

K_THREAD_STACK_DEFINE(bt_mgr_stack, BT_MGR_STACK_SIZE);
static struct k_thread bt_mgr_thread_data;

/* ------------------------------------------------------------------------- */
/* Paired-device table (Settings cfg/bt subtree)                             */
/* ------------------------------------------------------------------------- */

static int bt_dev_set(const char *key, size_t len, settings_read_cb read_cb, void *cb_arg)
{
	const char *next;

	if (settings_name_steq(key, "dev", &next) && next != NULL) {
		long idx = strtol(next, NULL, 10);

		if (idx < 0 || idx >= (long)BT_MGR_MAX_DEVICES) {
			return -EINVAL;
		}
		/* Tolerate incompatible records (old layout or foreign data):
		 * skip them gracefully; the slot is reused on the next save.
		 */
		if (len != sizeof(struct bt_device)) {
			LOG_WRN("ignoring incompatible record %s (len %zu)", key, len);
			return 0;
		}
		if (read_cb(cb_arg, &ctx.devices[idx], sizeof(struct bt_device)) < 0) {
			return -EIO;
		}
		ctx.device_used[idx] = true;
		return 0;
	}

	return -ENOENT;
}

static int bt_dev_export(int (*cb)(const char *name, const void *val, size_t val_len))
{
	char name[24];

	for (uint8_t i = 0; i < BT_MGR_MAX_DEVICES; i++) {
		if (ctx.device_used[i]) {
			(void)snprintf(name, sizeof(name), "cfg/bt/dev/%u", i);
			(void)cb(name, &ctx.devices[i], sizeof(ctx.devices[i]));
		}
	}

	return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(cfg_bt, "cfg/bt", NULL, bt_dev_set, NULL, bt_dev_export);

/* Caller must hold ctx.lock. */
static int devstore_find(const char *address)
{
	for (uint8_t i = 0; i < BT_MGR_MAX_DEVICES; i++) {
		if (ctx.device_used[i] && strcmp(ctx.devices[i].address, address) == 0) {
			return i;
		}
	}
	return -1;
}

static size_t devstore_count(void)
{
	size_t count = 0;

	for (uint8_t i = 0; i < BT_MGR_MAX_DEVICES; i++) {
		count += ctx.device_used[i] ? 1U : 0U;
	}
	return count;
}

/* Add a newly authenticated device if there is room. Caller holds ctx.lock. */
static int devstore_add(const char *address)
{
	char key[24];
	int slot = -1;

	for (uint8_t i = 0; i < BT_MGR_MAX_DEVICES; i++) {
		if (!ctx.device_used[i]) {
			slot = i;
			break;
		}
	}
	if (slot < 0) {
		LOG_WRN("device table full, not recording %s", address);
		return -ENOSPC;
	}

	memset(&ctx.devices[slot], 0, sizeof(ctx.devices[slot]));
	ctx.devices[slot].version = BT_DEVICE_VERSION;
	strncpy(ctx.devices[slot].address, address, BT_MGR_ADDR_TEXT);
	ctx.device_used[slot] = true;

	(void)snprintf(key, sizeof(key), "cfg/bt/dev/%d", slot);
	(void)settings_save_one(key, &ctx.devices[slot], sizeof(ctx.devices[slot]));
	LOG_INF("recorded device %s in slot %d", address, slot);
	return slot;
}

/* ------------------------------------------------------------------------- */
/* Channel mapping                                                           */
/* ------------------------------------------------------------------------- */

/* Caller must hold ctx.lock. */
static struct bt_channel *channel_find(const char *cid)
{
	for (uint8_t i = 0; i < BT_MGR_MAX_CHANNELS; i++) {
		if (ctx.channels[i].in_use && strcmp(ctx.channels[i].cid, cid) == 0) {
			return &ctx.channels[i];
		}
	}
	return NULL;
}

static struct bt_channel *channel_alloc(const char *cid, uint8_t *index)
{
	struct bt_channel *ch = channel_find(cid);

	if (ch != NULL) {
		*index = ch - ctx.channels;
		return ch;
	}

	for (uint8_t i = 0; i < BT_MGR_MAX_CHANNELS; i++) {
		if (!ctx.channels[i].in_use) {
			ch = &ctx.channels[i];
			memset(ch, 0, sizeof(*ch));
			ch->in_use = true;
			strncpy(ch->cid, cid, I2616E_CID_TEXT_SIZE);
			*index = i;
			return ch;
		}
	}

	LOG_WRN("no free channel for cid %s", cid);
	return NULL;
}

/* ------------------------------------------------------------------------- */
/* Event dispatch                                                            */
/* ------------------------------------------------------------------------- */

static void emit(enum bt_mgr_event_type type, uint8_t channel, const char *address,
		 const uint8_t *data, size_t length)
{
	struct bt_mgr_event ev = {
		.type = type,
		.channel = channel,
		.data = data,
		.length = length,
	};

	if (address != NULL) {
		strncpy(ev.address, address, BT_MGR_ADDR_TEXT);
	}
	if (ctx.cb != NULL) {
		ctx.cb(&ev, ctx.user_data);
	}
}

static void handle_connected(const struct i2616e_event *event)
{
	uint8_t index;

	k_mutex_lock(&ctx.lock, K_FOREVER);
	struct bt_channel *ch = channel_alloc(event->cid, &index);

	k_mutex_unlock(&ctx.lock);

	if (ch != NULL) {
		LOG_INF("connected cid=%s -> channel %u", event->cid, index);
		emit(BT_MGR_EVENT_CONNECTED, index, NULL, NULL, 0);
	}
}

static void handle_pair_request(const struct i2616e_event *event)
{
	size_t count;

	k_mutex_lock(&ctx.lock, K_FOREVER);
	count = devstore_count();
	k_mutex_unlock(&ctx.lock);

	/* Bring-up policy: accept while there is room, otherwise let the peer
	 * pair at the link layer but it will not be recorded as authorised.
	 */
	bool accept = (count < BT_MGR_MAX_DEVICES);

	LOG_INF("pair request cid=%s -> %s (paired %zu/%u)", event->cid,
		accept ? "accept" : "reject", count, BT_MGR_MAX_DEVICES);
	(void)i2616e_pair_confirm(ctx.dev, event->cid, accept);
}

static void handle_authenticated(const struct i2616e_event *event)
{
	uint8_t index = 0;
	bool mapped = false;
	bool added = false;

	k_mutex_lock(&ctx.lock, K_FOREVER);
	struct bt_channel *ch = channel_alloc(event->cid, &index);

	if (ch != NULL) {
		ch->authenticated = true;
		strncpy(ch->address, event->address, BT_MGR_ADDR_TEXT);
		mapped = true;
	}
	if (devstore_find(event->address) < 0) {
		added = (devstore_add(event->address) >= 0);
	}
	k_mutex_unlock(&ctx.lock);

	/* Mirror a new authorisation into the module whitelist outside the lock
	 * (it issues a blocking AT command).
	 */
	if (added) {
		(void)i2616e_whitelist_add_address(ctx.dev, event->address);
	}
	if (mapped) {
		LOG_INF("authenticated channel %u addr=%s", index, event->address);
		emit(BT_MGR_EVENT_AUTHENTICATED, index, event->address, NULL, 0);
	}
}

static void handle_disconnected(const struct i2616e_event *event)
{
	uint8_t index = 0;
	bool found = false;

	k_mutex_lock(&ctx.lock, K_FOREVER);
	struct bt_channel *ch = channel_find(event->cid);

	if (ch != NULL) {
		index = ch - ctx.channels;
		found = true;
		memset(ch, 0, sizeof(*ch));
	}
	k_mutex_unlock(&ctx.lock);

	if (found) {
		LOG_INF("disconnected channel %u", index);
		emit(BT_MGR_EVENT_DISCONNECTED, index, NULL, NULL, 0);
	}
}

static void handle_data(const struct i2616e_event *event)
{
	uint8_t index = 0;
	bool found = false;

	k_mutex_lock(&ctx.lock, K_FOREVER);
	struct bt_channel *ch = channel_find(event->cid);

	if (ch != NULL) {
		index = ch - ctx.channels;
		found = true;
	}
	k_mutex_unlock(&ctx.lock);

	if (found) {
		emit(BT_MGR_EVENT_DATA, index, NULL, event->data, event->length);
	}
}

static void bt_mgr_thread_fn(void *p1, void *p2, void *p3)
{
	static struct i2616e_event event;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (true) {
		if (i2616e_get_event(ctx.dev, &event, K_FOREVER) < 0) {
			continue;
		}

		switch (event.type) {
		case I2616E_EVENT_CONNECTED:
			handle_connected(&event);
			break;
		case I2616E_EVENT_PAIR_REQUEST:
			handle_pair_request(&event);
			break;
		case I2616E_EVENT_PASSKEY_DISPLAY:
			LOG_INF("passkey for cid=%s: %06u", event.cid, event.passkey);
			break;
		case I2616E_EVENT_PAIRED:
			handle_authenticated(&event);
			break;
		case I2616E_EVENT_DISCONNECTED:
			handle_disconnected(&event);
			break;
		case I2616E_EVENT_DATA_RECEIVED:
			handle_data(&event);
			break;
		case I2616E_EVENT_READY:
			LOG_WRN("module restarted unexpectedly");
			break;
		default:
			break;
		}
	}
}

/* ------------------------------------------------------------------------- */
/* Public API                                                                */
/* ------------------------------------------------------------------------- */

int bt_manager_send(uint8_t channel, const uint8_t *data, size_t length)
{
	char cid[I2616E_CID_TEXT_SIZE + 1U];

	if (channel >= BT_MGR_MAX_CHANNELS) {
		return -EINVAL;
	}

	k_mutex_lock(&ctx.lock, K_FOREVER);
	if (!ctx.channels[channel].in_use) {
		k_mutex_unlock(&ctx.lock);
		return -ENOTCONN;
	}
	strcpy(cid, ctx.channels[channel].cid);
	k_mutex_unlock(&ctx.lock);

	return i2616e_send(ctx.dev, cid, data, length);
}

size_t bt_manager_device_count(void)
{
	size_t count;

	k_mutex_lock(&ctx.lock, K_FOREVER);
	count = devstore_count();
	k_mutex_unlock(&ctx.lock);
	return count;
}

int bt_manager_forget(const char *address)
{
	char key[24];
	int slot;

	if (address == NULL) {
		return -EINVAL;
	}

	k_mutex_lock(&ctx.lock, K_FOREVER);
	slot = devstore_find(address);
	if (slot >= 0) {
		ctx.device_used[slot] = false;
		memset(&ctx.devices[slot], 0, sizeof(ctx.devices[slot]));
	}
	k_mutex_unlock(&ctx.lock);

	if (slot < 0) {
		return -ENOENT;
	}

	(void)snprintf(key, sizeof(key), "cfg/bt/dev/%d", slot);
	(void)settings_delete(key);
	(void)i2616e_whitelist_remove_address(ctx.dev, address);
	LOG_INF("forgot device %s (slot %d)", address, slot);
	return 0;
}

int bt_manager_init(const struct device *i2616e, bt_mgr_callback_t cb, void *user_data)
{
	const struct i2616e_settings settings = {
		.name = "ID. UNYX Pro AC999",
		.bluetooth_mode = 0U,
		.silent = false,
		.command_mode = true,
		.multi_connection = true,
		.auto_unlock = true,
		.advertising = true,
	};
	int ret;

	if (i2616e == NULL || !device_is_ready(i2616e)) {
		return -ENODEV;
	}

	ctx.dev = i2616e;
	ctx.cb = cb;
	ctx.user_data = user_data;
	k_mutex_init(&ctx.lock);

	/* Restore the paired-device table from flash. */
	ret = settings_subsys_init();
	if (ret < 0) {
		LOG_ERR("settings_subsys_init: %d", ret);
		return ret;
	}
	(void)settings_load_subtree("cfg/bt");

	/* Bring the module up and apply the bring-up configuration. */
	ret = i2616e_hardware_reset(i2616e);
	if (ret < 0) {
		LOG_ERR("module reset: %d", ret);
		return ret;
	}
	ret = i2616e_apply_settings(i2616e, &settings);
	if (ret < 0) {
		LOG_ERR("apply settings: %d", ret);
		return ret;
	}

	/* Push the stored authorisations into the module whitelist. */
	for (uint8_t i = 0; i < BT_MGR_MAX_DEVICES; i++) {
		if (ctx.device_used[i]) {
			(void)i2616e_whitelist_add_address(i2616e, ctx.devices[i].address);
		}
	}

	LOG_INF("bluetooth up, %zu paired device(s)", devstore_count());

	k_thread_create(&bt_mgr_thread_data, bt_mgr_stack, K_THREAD_STACK_SIZEOF(bt_mgr_stack),
			bt_mgr_thread_fn, NULL, NULL, NULL, BT_MGR_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&bt_mgr_thread_data, "bt_manager");

	return 0;
}
