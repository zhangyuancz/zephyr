/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include <zephyr/drivers/misc/relay/relay.h>

static const struct device *const relay = DEVICE_DT_GET_ONE(zephyr_gpio_relay);
static const struct device *const console = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));

static void print_status(void)
{
	uint32_t mask = relay_get(relay);

	printk("Relay: channels=0x%x (L=%u N=%u) weld_fault=%u\n", mask,
	       (mask >> 0) & 1U, (mask >> 1) & 1U, relay_weld_fault(relay));
}

static void weld_handler(const struct device *dev, bool welded, void *user_data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(user_data);

	printk("Relay weld detection: %s\n", welded ? "FAULT (output live while open)" : "normal");
}

static void print_help(void)
{
	printk("Commands: 0=off, 1=L only, 2=N only, 3=both, s=status, h=help\n");
}

static void handle_console(void)
{
	unsigned char command;
	uint32_t mask;
	int ret;

	if (uart_poll_in(console, &command) != 0) {
		return;
	}

	switch (command) {
	case '0':
		mask = 0x0;
		break;
	case '1':
		mask = 0x1;
		break;
	case '2':
		mask = 0x2;
		break;
	case '3':
		mask = 0x3;
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

	ret = relay_set(relay, mask);
	if (ret < 0) {
		printk("Relay update failed: %d\n", ret);
	} else {
		print_status();
	}
}

int main(void)
{
	printk("\nACBoard relay control test\n");

	if (!device_is_ready(relay) || !device_is_ready(console)) {
		printk("Relay device resource not ready\n");
		return 0;
	}

	(void)relay_set_weld_handler(relay, weld_handler, NULL);

	print_status();
	print_help();

	while (true) {
		handle_console();
		k_sleep(K_MSEC(10));
	}

	return 0;
}
