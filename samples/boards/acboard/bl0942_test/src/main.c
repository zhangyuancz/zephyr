/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>

#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include <zephyr/drivers/sensor/bl0942.h>

#define BL0942_POLL_INTERVAL K_SECONDS(2)

static const struct device *const bl0942 = DEVICE_DT_GET_ONE(belling_bl0942);

static void print_channel(const char *name, enum sensor_channel chan, const char *unit)
{
	struct sensor_value val;
	int ret;

	ret = sensor_channel_get(bl0942, chan, &val);
	if (ret < 0) {
		printk("  %s: get failed (%d)\n", name, ret);
		return;
	}

	printk("  %s: %d.%06d %s\n", name, val.val1, abs(val.val2), unit);
}

int main(void)
{
	int ret;

	printk("\nACBoard BL0942 energy-meter sensor test\n");

	if (!device_is_ready(bl0942)) {
		printk("BL0942 device not ready\n");
		return 0;
	}

	while (true) {
		ret = sensor_sample_fetch(bl0942);
		if (ret < 0) {
			printk("sample_fetch failed: %d\n", ret);
			k_sleep(BL0942_POLL_INTERVAL);
			continue;
		}

		printk("BL0942:\n");
		print_channel("voltage", SENSOR_CHAN_VOLTAGE, "V");
		print_channel("current", SENSOR_CHAN_CURRENT, "A");
		print_channel("power", SENSOR_CHAN_POWER, "W");
		print_channel("frequency", SENSOR_CHAN_FREQUENCY, "Hz");
		print_channel("energy pulses", (enum sensor_channel)SENSOR_CHAN_BL0942_CF_CNT,
			      "cnt");

		k_sleep(BL0942_POLL_INTERVAL);
	}

	return 0;
}
