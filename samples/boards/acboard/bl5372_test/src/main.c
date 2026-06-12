/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/rtc.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

static const struct device *const rtc = DEVICE_DT_GET(DT_NODELABEL(bl5372));

static const struct rtc_time test_time = {
	.tm_sec = 0,
	.tm_min = 30,
	.tm_hour = 14,
	.tm_mday = 12,
	.tm_mon = 5,
	.tm_year = 126,
	.tm_wday = 5,
	.tm_yday = -1,
	.tm_isdst = -1,
};

int main(void)
{
	struct rtc_time time;
	int ret;

	printk("\nACBoard BL5372 RTC test\n");
	printk("I2C2: PA8 SCL, PC9 SDA, 100 kHz, address 0x32\n");

	if (!device_is_ready(rtc)) {
		printk("BL5372 device not ready\n");
		return 0;
	}

	ret = rtc_get_time(rtc, &time);
	if (ret == -ENODATA) {
		printk("BL5372 XSTP set, initializing test time: 2026-06-12 14:30:00\n");
		ret = rtc_set_time(rtc, &test_time);
		if (ret < 0) {
			printk("BL5372 test time write failed: %d\n", ret);
			return 0;
		}
		printk("BL5372 test time written successfully\n");
	}

	while (true) {
		ret = rtc_get_time(rtc, &time);
		if (ret == -ENODATA) {
			printk("BL5372 time invalid: XSTP indicates power loss or oscillator stop\n");
		} else if (ret == -ENOTSUP) {
			printk("BL5372 is configured for 12-hour mode; set time to select 24-hour mode\n");
		} else if (ret == -EAGAIN) {
			printk("BL5372 time changed while reading; retrying\n");
		} else if (ret < 0) {
			printk("BL5372 read failed: %d\n", ret);
		} else {
			printk("BL5372: %04d-%02d-%02d %02d:%02d:%02d weekday=%d\n",
			       time.tm_year + 1900, time.tm_mon + 1, time.tm_mday,
			       time.tm_hour, time.tm_min, time.tm_sec, time.tm_wday);
		}

		k_sleep(K_SECONDS(1));
	}

	return 0;
}
