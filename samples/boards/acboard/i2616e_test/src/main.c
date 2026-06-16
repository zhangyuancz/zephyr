/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stddef.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include <zephyr/drivers/misc/i2616e/i2616e.h>

static const struct device *const bluetooth = DEVICE_DT_GET(DT_NODELABEL(bluetooth));

static const char *event_name(enum i2616e_event_type type)
{
	switch (type) {
	case I2616E_EVENT_READY:
		return "READY";
	case I2616E_EVENT_CONNECTED:
		return "CONNECTED";
	case I2616E_EVENT_DISCONNECTED:
		return "DISCONNECTED";
	case I2616E_EVENT_PAIR_REQUEST:
		return "PAIR_REQUEST";
	case I2616E_EVENT_PASSKEY_DISPLAY:
		return "PASSKEY_DISPLAY";
	case I2616E_EVENT_PASSKEY_REQUEST:
		return "PASSKEY_REQUEST";
	case I2616E_EVENT_PAIRED:
		return "PAIRED";
	case I2616E_EVENT_DATA_RECEIVED:
		return "DATA_RECEIVED";
	case I2616E_EVENT_CONNECTION_TIMEOUT:
		return "CONNECTION_TIMEOUT";
	case I2616E_EVENT_SERVICE_MISMATCH:
		return "SERVICE_MISMATCH";
	default:
		return "UNKNOWN";
	}
}

int main(void)
{
	static struct i2616e_event event;
	struct i2616e_info info;
	const struct i2616e_settings settings = {
		.name = "ID. UNYX Pro AC999",
		.bluetooth_mode = 0U,
		.silent = false,
		.command_mode = true,
		.multi_connection = true,
		.auto_unlock = true,
		.advertising = true,
	};
	char whitelist[I2616E_WHITELIST_MAX_SIZE + 1];
	int ret;

	printk("\nACBoard i2616e BLE module test\n");

	if (!device_is_ready(bluetooth)) {
		printk("i2616e device not ready\n");
		return 0;
	}

	ret = i2616e_hardware_reset(bluetooth);
	if (ret < 0) {
		printk("reset failed: %d\n", ret);
		return 0;
	}
	printk("module ready\n");

	ret = i2616e_get_info(bluetooth, &info);
	if (ret < 0) {
		printk("get_info failed: %d\n", ret);
		return 0;
	}
	printk("firmware : %s\n", info.firmware_version);
	printk("config   : %s\n", info.config_version);
	printk("name     : %s\n", info.name);
	printk("address  : %s\n", info.address);
	printk("baudrate : %u\n", info.baudrate);
	printk("flowctrl : %d\n", info.flow_control);
	printk("btmode   : %u\n", info.bluetooth_mode);

	ret = i2616e_apply_settings(bluetooth, &settings);
	if (ret < 0) {
		printk("apply_settings failed: %d\n", ret);
		return 0;
	}
	printk("settings applied\n");

	ret = i2616e_whitelist_get(bluetooth, whitelist, sizeof(whitelist));
	if (ret == 0) {
		printk("whitelist: %s\n", whitelist);
	}

	printk("waiting for events...\n");
	while (true) {
		ret = i2616e_get_event(bluetooth, &event, K_FOREVER);
		if (ret < 0) {
			continue;
		}

		switch (event.type) {
		case I2616E_EVENT_PAIR_REQUEST:
			printk("event: PAIR_REQUEST cid=%s -> accept\n", event.cid);
			(void)i2616e_pair_confirm(bluetooth, event.cid, true);
			break;
		case I2616E_EVENT_PASSKEY_DISPLAY:
			printk("event: PASSKEY_DISPLAY cid=%s passkey=%06u\n", event.cid,
			       event.passkey);
			break;
		case I2616E_EVENT_PAIRED:
			printk("event: PAIRED cid=%s addr=%s\n", event.cid, event.address);
			break;
		case I2616E_EVENT_DATA_RECEIVED:
			printk("event: DATA cid=%s len=%zu\n", event.cid, event.length);
			/* Echo the payload back to the peer. */
			(void)i2616e_send(bluetooth, event.cid, event.data, event.length);
			break;
		default:
			printk("event: %s cid=%s\n", event_name(event.type), event.cid);
			break;
		}
	}

	return 0;
}
