/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#define USER_NODE DT_PATH(zephyr_user)
#define WELD_DEBOUNCE K_MSEC(10)
#define DRIVER_SETTLE K_MSEC(5)

static const struct gpio_dt_spec relay_enable =
	GPIO_DT_SPEC_GET(USER_NODE, relay_enable_gpios);
static const struct gpio_dt_spec relay_control1 =
	GPIO_DT_SPEC_GET(USER_NODE, relay_control1_gpios);
static const struct gpio_dt_spec relay_control2 =
	GPIO_DT_SPEC_GET(USER_NODE, relay_control2_gpios);
static const struct gpio_dt_spec relay_weld =
	GPIO_DT_SPEC_GET(USER_NODE, relay_weld_gpios);
static const struct device *const console = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));

static struct gpio_callback weld_callback;
static struct k_work_delayable weld_work;
static bool weld_fault;
static bool control1_state;
static bool control2_state;

static void print_status(void)
{
	printk("Relay: enable=%u control1=%u control2=%u weld_fault=%u\n",
	       control1_state || control2_state, control1_state, control2_state,
	       weld_fault);
}

static void update_weld_state(struct k_work *work)
{
	int value;

	ARG_UNUSED(work);
	value = gpio_pin_get_dt(&relay_weld);
	if (value < 0) {
		printk("Relay weld input read failed: %d\n", value);
		return;
	}

	if ((value != 0) != weld_fault) {
		weld_fault = value != 0;
		printk("Relay weld detection: %s (PE4=%s)\n",
		       weld_fault ? "FAULT" : "normal",
		       weld_fault ? "low" : "high");
	}
}

static void weld_isr(const struct device *port, struct gpio_callback *callback,
		     gpio_port_pins_t pins)
{
	ARG_UNUSED(port);
	ARG_UNUSED(callback);
	ARG_UNUSED(pins);
	(void)k_work_reschedule(&weld_work, WELD_DEBOUNCE);
}

static int relay_set(bool control1, bool control2)
{
	int ret;

	ret = gpio_pin_set_dt(&relay_enable, 0);
	ret |= gpio_pin_set_dt(&relay_control1, 0);
	ret |= gpio_pin_set_dt(&relay_control2, 0);
	if (ret < 0) {
		return ret;
	}
	k_sleep(DRIVER_SETTLE);

	if (control1 || control2) {
		ret = gpio_pin_set_dt(&relay_control1, control1);
		ret |= gpio_pin_set_dt(&relay_control2, control2);
		ret |= gpio_pin_set_dt(&relay_enable, 1);
	}

	if (ret == 0) {
		control1_state = control1;
		control2_state = control2;
	}
	return ret;
}

static void print_help(void)
{
	printk("Commands: 0=off, 1=control1, 2=control2, 3=both, s=status, h=help\n");
}

static void handle_console(void)
{
	unsigned char command;
	bool control1;
	bool control2;
	int ret;

	if (uart_poll_in(console, &command) != 0) {
		return;
	}

	switch (command) {
	case '0':
		control1 = false;
		control2 = false;
		break;
	case '1':
		control1 = true;
		control2 = false;
		break;
	case '2':
		control1 = false;
		control2 = true;
		break;
	case '3':
		control1 = true;
		control2 = true;
		break;
	case 's':
		print_status();
		return;
	case 'h':
	case '?':
		print_help();
		return;
	default:
		return;
	}

	ret = relay_set(control1, control2);
	if (ret < 0) {
		printk("Relay update failed: %d\n", ret);
	} else {
		print_status();
	}
}

int main(void)
{
	int value;
	int ret;

	printk("\nACBoard relay control test\n");
	printk("PE1 enable, PE2/PE3 controls, PE4 active-low weld detection\n");

	if (!gpio_is_ready_dt(&relay_enable) || !gpio_is_ready_dt(&relay_control1) ||
	    !gpio_is_ready_dt(&relay_control2) || !gpio_is_ready_dt(&relay_weld) ||
	    !device_is_ready(console)) {
		printk("Relay device resource not ready\n");
		return 0;
	}

	ret = gpio_pin_configure_dt(&relay_enable, GPIO_OUTPUT_INACTIVE);
	ret |= gpio_pin_configure_dt(&relay_control1, GPIO_OUTPUT_INACTIVE);
	ret |= gpio_pin_configure_dt(&relay_control2, GPIO_OUTPUT_INACTIVE);
	ret |= gpio_pin_configure_dt(&relay_weld, GPIO_INPUT);
	if (ret < 0) {
		printk("Relay GPIO setup failed: %d\n", ret);
		return 0;
	}

	k_work_init_delayable(&weld_work, update_weld_state);
	gpio_init_callback(&weld_callback, weld_isr, BIT(relay_weld.pin));
	ret = gpio_add_callback(relay_weld.port, &weld_callback);
	ret |= gpio_pin_interrupt_configure_dt(&relay_weld, GPIO_INT_EDGE_BOTH);
	if (ret < 0) {
		printk("Relay weld interrupt setup failed: %d\n", ret);
		return 0;
	}

	value = gpio_pin_get_dt(&relay_weld);
	if (value < 0) {
		printk("Relay weld initial read failed: %d\n", value);
		return 0;
	}
	weld_fault = value != 0;
	print_status();
	print_help();

	while (true) {
		handle_console();
		k_sleep(K_MSEC(10));
	}

	return 0;
}
