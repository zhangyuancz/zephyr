/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/device.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#define SP706_TIMEOUT_MS 1600U
#define FEED_INTERVAL K_MSEC(500)
#define FEED_COUNT 10

static const struct device *const watchdog = DEVICE_DT_GET(DT_ALIAS(watchdog0));

int main(void)
{
	const struct wdt_timeout_cfg timeout = {
		.window = {
			.min = 0U,
			.max = SP706_TIMEOUT_MS,
		},
		.flags = WDT_FLAG_RESET_SOC,
	};
	int channel;
	int ret;

	printk("\nACBoard SP706 external watchdog test\n");
	printk("WDI: PD11, nominal timeout 1.6 s\n");

	if (!device_is_ready(watchdog)) {
		printk("SP706 watchdog device not ready\n");
		return 0;
	}

	channel = wdt_install_timeout(watchdog, &timeout);
	if (channel < 0) {
		printk("SP706 timeout install failed: %d\n", channel);
		return 0;
	}

	ret = wdt_setup(watchdog, 0U);
	if (ret < 0) {
		printk("SP706 setup failed: %d\n", ret);
		return 0;
	}

	for (int i = 1; i <= FEED_COUNT; ++i) {
		ret = wdt_feed(watchdog, channel);
		if (ret < 0) {
			printk("SP706 feed failed: %d\n", ret);
			return 0;
		}
		printk("SP706 feed %d/%d\n", i, FEED_COUNT);
		k_sleep(FEED_INTERVAL);
	}

	printk("Feeding stopped; reset expected within 1.0 to 2.25 seconds\n");
	while (true) {
		k_sleep(K_FOREVER);
	}

	return 0;
}
