/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdbool.h>

#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

static const struct gpio_dt_spec led0 = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
static const struct gpio_dt_spec led1 = GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios);

int main(void)
{
	bool phase = false;
	int ret;

	printk("\nACBoard onboard LED test\n");
	printk("LED0: PE8, LED1: PE9, active low\n");

	if (!gpio_is_ready_dt(&led0) || !gpio_is_ready_dt(&led1)) {
		printk("LED GPIO device not ready\n");
		return 0;
	}

	ret = gpio_pin_configure_dt(&led0, GPIO_OUTPUT_INACTIVE);
	ret |= gpio_pin_configure_dt(&led1, GPIO_OUTPUT_INACTIVE);
	if (ret < 0) {
		printk("LED GPIO setup failed: %d\n", ret);
		return 0;
	}

	while (true) {
		ret = gpio_pin_set_dt(&led0, phase);
		ret |= gpio_pin_set_dt(&led1, !phase);
		if (ret < 0) {
			printk("LED update failed: %d\n", ret);
			return 0;
		}

		printk("LED0=%s LED1=%s\n", phase ? "on" : "off",
		       phase ? "off" : "on");
		phase = !phase;
		k_sleep(K_MSEC(500));
	}

	return 0;
}
