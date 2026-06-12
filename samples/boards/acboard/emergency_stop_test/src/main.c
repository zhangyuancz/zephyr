/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdbool.h>

#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>

#define USER_NODE DT_PATH(zephyr_user)
#define ESTOP_DEBOUNCE K_MSEC(5)

static const struct gpio_dt_spec emergency_stop =
	GPIO_DT_SPEC_GET(USER_NODE, emergency_stop_gpios);

static struct gpio_callback emergency_stop_callback;
static struct k_work_delayable emergency_stop_work;
static atomic_t emergency_stop_latched;
static bool emergency_stop_active;

static void confirm_emergency_stop(struct k_work *work)
{
	int value;

	ARG_UNUSED(work);

	value = gpio_pin_get_dt(&emergency_stop);
	if (value < 0) {
		printk("Emergency-stop input read failed: %d\n", value);
		return;
	}

	if ((value != 0) == emergency_stop_active) {
		return;
	}

	emergency_stop_active = value != 0;
	if (emergency_stop_active) {
		atomic_set(&emergency_stop_latched, 1);
		printk("EMERGENCY STOP ACTIVE: PA4 low, event latched\n");
	} else {
		printk("Emergency-stop input recovered: PA4 high, latch remains set=%d\n",
		       atomic_get(&emergency_stop_latched) != 0);
	}
}

static void emergency_stop_isr(const struct device *port,
			       struct gpio_callback *callback,
			       gpio_port_pins_t pins)
{
	ARG_UNUSED(port);
	ARG_UNUSED(callback);
	ARG_UNUSED(pins);

	/* Latch immediately when the active-low input is observed in the ISR. */
	if (gpio_pin_get_dt(&emergency_stop) > 0) {
		atomic_set(&emergency_stop_latched, 1);
	}
	(void)k_work_reschedule(&emergency_stop_work, ESTOP_DEBOUNCE);
}

int main(void)
{
	int value;
	int ret;

	printk("\nACBoard emergency-stop input test\n");
	printk("PA4: normal high, emergency stop active low\n");

	if (!gpio_is_ready_dt(&emergency_stop)) {
		printk("Emergency-stop GPIO device not ready\n");
		return 0;
	}

	ret = gpio_pin_configure_dt(&emergency_stop, GPIO_INPUT);
	if (ret < 0) {
		printk("Emergency-stop GPIO setup failed: %d\n", ret);
		return 0;
	}

	k_work_init_delayable(&emergency_stop_work, confirm_emergency_stop);
	gpio_init_callback(&emergency_stop_callback, emergency_stop_isr,
			   BIT(emergency_stop.pin));
	ret = gpio_add_callback(emergency_stop.port, &emergency_stop_callback);
	if (ret < 0) {
		printk("Emergency-stop callback setup failed: %d\n", ret);
		return 0;
	}

	ret = gpio_pin_interrupt_configure_dt(&emergency_stop, GPIO_INT_EDGE_BOTH);
	if (ret < 0) {
		printk("Emergency-stop interrupt setup failed: %d\n", ret);
		return 0;
	}

	value = gpio_pin_get_dt(&emergency_stop);
	if (value < 0) {
		printk("Emergency-stop initial read failed: %d\n", value);
		return 0;
	}

	emergency_stop_active = value != 0;
	atomic_set(&emergency_stop_latched, emergency_stop_active);
	printk("Initial state: %s (PA4=%s), latched=%d\n",
	       emergency_stop_active ? "EMERGENCY STOP ACTIVE" : "normal",
	       emergency_stop_active ? "low" : "high",
	       atomic_get(&emergency_stop_latched) != 0);

	while (true) {
		k_sleep(K_FOREVER);
	}

	return 0;
}
