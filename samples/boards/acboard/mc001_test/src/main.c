/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#define USER_NODE DT_PATH(zephyr_user)

#define POWER_ON_SETTLE K_MSEC(100)
#define ZERO_CAL_PULSE K_MSEC(75)
#define ZERO_CAL_SETTLE K_MSEC(500)
#define TRIP_DEBOUNCE K_MSEC(5)

static const struct gpio_dt_spec trip_input =
	GPIO_DT_SPEC_GET(USER_NODE, mc001_trip_gpios);
static const struct gpio_dt_spec zero_cal =
	GPIO_DT_SPEC_GET(USER_NODE, mc001_zero_cal_gpios);

static struct gpio_callback trip_callback;
static struct k_work_delayable trip_work;
static bool trip_state;

static void report_trip_state(struct k_work *work)
{
	int value;

	ARG_UNUSED(work);

	value = gpio_pin_get_dt(&trip_input);
	if (value < 0) {
		printk("MC001 trip input read failed: %d\n", value);
		return;
	}

	if ((value != 0) == trip_state) {
		return;
	}

	trip_state = value != 0;
	printk("MC001: %s (PB8=%s)\n",
	       trip_state ? "TRIP active" : "normal",
	       trip_state ? "low" : "high");
}

static void trip_isr(const struct device *port, struct gpio_callback *callback,
		     gpio_port_pins_t pins)
{
	ARG_UNUSED(port);
	ARG_UNUSED(callback);
	ARG_UNUSED(pins);

	(void)k_work_reschedule(&trip_work, TRIP_DEBOUNCE);
}

static int mc001_zero_calibrate(void)
{
	int ret;

	printk("MC001 zero calibration: PE0 high for 75 ms\n");
	ret = gpio_pin_set_dt(&zero_cal, 1);
	if (ret < 0) {
		return ret;
	}
	k_sleep(ZERO_CAL_PULSE);

	ret = gpio_pin_set_dt(&zero_cal, 0);
	if (ret < 0) {
		return ret;
	}
	k_sleep(ZERO_CAL_SETTLE);

	return 0;
}

int main(void)
{
	int value;
	int ret;

	printk("\nACBoard MC001 residual-current detector test\n");
	printk("PB8: active-low trip, PE0: active-high zero calibration\n");

	if (!gpio_is_ready_dt(&trip_input) || !gpio_is_ready_dt(&zero_cal)) {
		printk("MC001 GPIO resource not ready\n");
		return 0;
	}

	ret = gpio_pin_configure_dt(&zero_cal, GPIO_OUTPUT_INACTIVE);
	if (ret < 0) {
		printk("MC001 zero-cal GPIO setup failed: %d\n", ret);
		return 0;
	}

	ret = gpio_pin_configure_dt(&trip_input, GPIO_INPUT);
	if (ret < 0) {
		printk("MC001 trip GPIO setup failed: %d\n", ret);
		return 0;
	}

	k_sleep(POWER_ON_SETTLE);
	ret = mc001_zero_calibrate();
	if (ret < 0) {
		printk("MC001 zero calibration failed: %d\n", ret);
		return 0;
	}
	printk("MC001 zero calibration complete\n");

	k_work_init_delayable(&trip_work, report_trip_state);
	gpio_init_callback(&trip_callback, trip_isr, BIT(trip_input.pin));
	ret = gpio_add_callback(trip_input.port, &trip_callback);
	if (ret < 0) {
		printk("MC001 trip callback setup failed: %d\n", ret);
		return 0;
	}

	ret = gpio_pin_interrupt_configure_dt(&trip_input, GPIO_INT_EDGE_BOTH);
	if (ret < 0) {
		printk("MC001 trip interrupt setup failed: %d\n", ret);
		return 0;
	}

	value = gpio_pin_get_dt(&trip_input);
	if (value < 0) {
		printk("MC001 initial trip input read failed: %d\n", value);
		return 0;
	}
	trip_state = value != 0;
	printk("MC001 initial state: %s (PB8=%s)\n",
	       trip_state ? "TRIP active" : "normal",
	       trip_state ? "low" : "high");

	while (true) {
		k_sleep(K_FOREVER);
	}

	return 0;
}
