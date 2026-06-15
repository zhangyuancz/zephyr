/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include <zephyr/drivers/misc/ws1850t/ws1850t.h>

#define POLL_INTERVAL K_MSEC(100)
#define CARD_REMOVAL_POLLS 3U

static const struct device *const ws1850t = DEVICE_DT_GET_ONE(wisesun_ws1850t);

int main(void)
{
	uint8_t atqa[2];
	uint8_t uid[4];
	uint8_t version;
	uint32_t active_uid = 0U;
	uint32_t missed_polls = 0U;
	bool card_present = false;
	int ret;

	printk("\nACBoard WS1850T RFID reader test\n");

	if (!device_is_ready(ws1850t)) {
		printk("WS1850T device not ready\n");
		return 0;
	}

	if (ws1850t_get_version(ws1850t, &version) == 0) {
		printk("WS1850T version: 0x%02x\n", version);
	}
	printk("Waiting for ISO 14443-A card\n");

	while (true) {
		ret = ws1850t_read_card(ws1850t, atqa, uid);
		if (ret == 0) {
			uint32_t uid_value = (uint32_t)uid[0] | ((uint32_t)uid[1] << 8) |
					     ((uint32_t)uid[2] << 16) | ((uint32_t)uid[3] << 24);

			missed_polls = 0U;
			if (!card_present || uid_value != active_uid) {
				active_uid = uid_value;
				card_present = true;
				printk("Card: ATQA=%02x%02x UID=%02x%02x%02x%02x\n",
				       atqa[0], atqa[1], uid[0], uid[1], uid[2], uid[3]);
			}
		} else if (ret == -EAGAIN) {
			if (card_present && ++missed_polls >= CARD_REMOVAL_POLLS) {
				card_present = false;
				missed_polls = 0U;
				printk("Card removed\n");
			}
		} else {
			printk("Card read failed: %d\n", ret);
		}

		k_sleep(POLL_INTERVAL);
	}

	return 0;
}
