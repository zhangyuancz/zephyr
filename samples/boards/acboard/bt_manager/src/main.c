/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * Application-layer demo over the Bluetooth manager port. It handles the
 * wallbox APP PDU protocol while the i2616e CID strings and paired-device
 * persistence stay hidden inside bt_manager.
 */

#include "bt_app.h"
#include "bt_manager.h"

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

static const struct device *const bluetooth = DEVICE_DT_GET(DT_NODELABEL(bluetooth));

static void on_event(const struct bt_mgr_event *event, void *user_data)
{
	ARG_UNUSED(user_data);
	bt_app_on_event(event);
}

int main(void)
{
	int ret;

	printk("\nACBoard Bluetooth APP protocol\n");

	ret = bt_manager_init(bluetooth, on_event, NULL);
	if (ret < 0) {
		printk("bt_manager_init failed: %d\n", ret);
		return 0;
	}

	printk("bluetooth ready, %zu paired device(s)\n", bt_manager_device_count());
	return 0;
}
