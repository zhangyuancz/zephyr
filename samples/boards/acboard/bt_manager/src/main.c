/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * Application-layer demo over the Bluetooth manager port. It only deals with
 * logical channels and the normalised event callback; the i2616e CID strings
 * and the paired-device persistence are hidden inside bt_manager.
 */

#include "bt_manager.h"

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

static const struct device *const bluetooth = DEVICE_DT_GET(DT_NODELABEL(bluetooth));

static void on_event(const struct bt_mgr_event *event, void *user_data)
{
	ARG_UNUSED(user_data);

	switch (event->type) {
	case BT_MGR_EVENT_CONNECTED:
		printk("APP: channel %u connected\n", event->channel);
		break;
	case BT_MGR_EVENT_AUTHENTICATED:
		printk("APP: channel %u authenticated addr=%s (paired %zu)\n", event->channel,
		       event->address, bt_manager_device_count());
		break;
	case BT_MGR_EVENT_DISCONNECTED:
		printk("APP: channel %u disconnected\n", event->channel);
		break;
	case BT_MGR_EVENT_DATA:
		printk("APP: channel %u data len=%zu (echo)\n", event->channel, event->length);
		(void)bt_manager_send(event->channel, event->data, event->length);
		break;
	default:
		break;
	}
}

int main(void)
{
	int ret;

	printk("\nACBoard Bluetooth manager\n");

	ret = bt_manager_init(bluetooth, on_event, NULL);
	if (ret < 0) {
		printk("bt_manager_init failed: %d\n", ret);
		return 0;
	}

	printk("bluetooth ready, %zu paired device(s)\n", bt_manager_device_count());
	return 0;
}
