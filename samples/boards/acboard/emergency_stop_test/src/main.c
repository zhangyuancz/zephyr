/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdbool.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/input/input.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>

#define ESTOP_KEYS_NODE DT_NODELABEL(emergency_stop_keys)
#define ESTOP_KEY_NODE DT_NODELABEL(estop_key)
#define ESTOP_CODE DT_PROP(ESTOP_KEY_NODE, zephyr_code)

static const struct device *const estop_keys = DEVICE_DT_GET(ESTOP_KEYS_NODE);
static const struct gpio_dt_spec estop_gpio = GPIO_DT_SPEC_GET(ESTOP_KEY_NODE, gpios);
static atomic_t emergency_stop_latched;

/*
 * Edge events arrive debounced from the gpio-keys driver: value 1 means the
 * active-low input is asserted (emergency stop pressed), value 0 means it has
 * been released. Releasing deliberately keeps the latch set; production control
 * logic must run an explicit safety reset before re-enabling power outputs.
 */
static void emergency_stop_cb(struct input_event *evt, void *user_data)
{
	ARG_UNUSED(user_data);

	if (evt->type != INPUT_EV_KEY || evt->code != ESTOP_CODE) {
		return;
	}

	if (evt->value != 0) {
		atomic_set(&emergency_stop_latched, 1);
		printk("EMERGENCY STOP ACTIVE: PA4 low, event latched\n");
	} else {
		printk("Emergency-stop input recovered: PA4 high, latch remains set=%d\n",
		       atomic_get(&emergency_stop_latched) != 0);
	}
}
INPUT_CALLBACK_DEFINE(estop_keys, emergency_stop_cb, NULL);

int main(void)
{
	bool active;
	int value;

	printk("\nACBoard emergency-stop input test\n");
	printk("PA4: normal high, emergency stop active low (gpio-keys/input)\n");

	if (!device_is_ready(estop_keys)) {
		printk("Emergency-stop input device not ready\n");
		return 0;
	}

	/*
	 * The input subsystem only reports transitions, so sample the pin once
	 * at start-up to latch an emergency stop that is already asserted. The
	 * gpio-keys driver owns and configures the pin, so this is a read-only
	 * snapshot.
	 */
	value = gpio_pin_get_dt(&estop_gpio);
	if (value < 0) {
		printk("Emergency-stop initial read failed: %d\n", value);
		return 0;
	}

	active = value != 0;
	atomic_set(&emergency_stop_latched, active);
	printk("Initial state: %s (PA4=%s), latched=%d\n",
	       active ? "EMERGENCY STOP ACTIVE" : "normal",
	       active ? "low" : "high", atomic_get(&emergency_stop_latched) != 0);

	return 0;
}
