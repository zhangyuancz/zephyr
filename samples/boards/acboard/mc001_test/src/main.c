/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include <zephyr/drivers/sensor/mc001.h>

static const struct device *const mc001 = DEVICE_DT_GET_ONE(megasenway_mc001);

/* Kept static so the pointer handed to sensor_trigger_set() stays valid. */
static struct sensor_trigger trip_trigger = {
	.type = SENSOR_TRIG_THRESHOLD,
	.chan = (enum sensor_channel)SENSOR_CHAN_MC001_TRIP,
};

static void report_trip(const char *prefix)
{
	struct sensor_value val;
	int ret;

	ret = sensor_channel_get(mc001, (enum sensor_channel)SENSOR_CHAN_MC001_TRIP, &val);
	if (ret < 0) {
		printk("%s trip read failed: %d\n", prefix, ret);
		return;
	}

	printk("%s %s\n", prefix, val.val1 ? "TRIP active" : "normal");
}

static void trip_handler(const struct device *dev, const struct sensor_trigger *trig)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(trig);

	report_trip("MC001:");
}

int main(void)
{
	int ret;

	printk("\nACBoard MC001 residual-current detector test\n");

	if (!device_is_ready(mc001)) {
		printk("MC001 device not ready\n");
		return 0;
	}

	/* The driver ran zero-calibration during init; report the initial state. */
	ret = sensor_sample_fetch(mc001);
	if (ret < 0) {
		printk("sample_fetch failed: %d\n", ret);
		return 0;
	}
	report_trip("MC001 initial state:");

	ret = sensor_trigger_set(mc001, &trip_trigger, trip_handler);
	if (ret < 0) {
		printk("trip trigger setup failed: %d\n", ret);
		return 0;
	}

	/*
	 * Run the module self-test: it injects a simulated residual current, so
	 * the TRIP path (and the trigger callback above) is exercised without a
	 * real fault. The trip_handler prints the TRIP active/normal edges.
	 */
	printk("Running self-test...\n");
	ret = sensor_attr_set(mc001, (enum sensor_channel)SENSOR_CHAN_MC001_TRIP,
			      (enum sensor_attribute)SENSOR_ATTR_MC001_SELF_TEST, NULL);
	if (ret == 0) {
		printk("Self-test PASS: module tripped on simulated residual current\n");
	} else if (ret == -ENOTSUP) {
		printk("Self-test not available (no self-test GPIO)\n");
	} else {
		printk("Self-test FAIL: TRIP did not assert (%d)\n", ret);
	}

	printk("Waiting for residual-current trip events\n");
	return 0;
}
