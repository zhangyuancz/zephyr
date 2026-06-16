/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * Demonstrates the Zephyr Settings subsystem as the ACBoard configuration
 * store. Settings are persisted through the SETTINGS_FILE backend into the
 * LittleFS on Bank1 (/lfs/settings). A Bluetooth paired-device table is modeled
 * under the "cfg/bt" subtree, with one record per key "cfg/bt/dev/<i>".
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/printk.h>

#define MAX_DEV 3U

/* One persisted record. Keep a version byte so fields can grow later. */
struct bt_dev {
	uint8_t version;
	uint8_t mac[6];
	uint8_t pnc;		/* plug-and-charge enabled */
	char name[30];
};

#define BT_DEV_VERSION 1U

static struct bt_dev devices[MAX_DEV];
static bool dev_used[MAX_DEV];

/* Settings load callback: invoked once per stored key under "cfg/bt". */
static int bt_set(const char *key, size_t len, settings_read_cb read_cb, void *cb_arg)
{
	const char *next;

	if (settings_name_steq(key, "dev", &next) && next != NULL) {
		long idx = strtol(next, NULL, 10);

		if (idx < 0 || idx >= (long)MAX_DEV || len != sizeof(struct bt_dev)) {
			return -EINVAL;
		}
		if (read_cb(cb_arg, &devices[idx], sizeof(struct bt_dev)) < 0) {
			return -EIO;
		}
		dev_used[idx] = true;
		return 0;
	}

	return -ENOENT;
}

/* Settings save callback: dump every in-use record. */
static int bt_export(int (*cb)(const char *name, const void *val, size_t val_len))
{
	char name[24];

	for (uint8_t i = 0; i < MAX_DEV; i++) {
		if (dev_used[i]) {
			(void)snprintf(name, sizeof(name), "cfg/bt/dev/%u", i);
			(void)cb(name, &devices[i], sizeof(devices[i]));
		}
	}

	return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(cfg_bt, "cfg/bt", NULL, bt_set, NULL, bt_export);

static void print_table(void)
{
	unsigned int count = 0U;

	for (uint8_t i = 0; i < MAX_DEV; i++) {
		if (dev_used[i]) {
			printk("  [%u] %-8s %02x:%02x:%02x:%02x:%02x:%02x pnc=%u\n", i,
			       devices[i].name, devices[i].mac[0], devices[i].mac[1],
			       devices[i].mac[2], devices[i].mac[3], devices[i].mac[4],
			       devices[i].mac[5], devices[i].pnc);
			count++;
		}
	}
	if (count == 0U) {
		printk("  (empty)\n");
	}
}

/* Add a record and immediately persist just that key. */
static void enroll(uint8_t i, const char *name, const uint8_t mac[6], uint8_t pnc)
{
	char key[24];
	int ret;

	memset(&devices[i], 0, sizeof(devices[i]));
	devices[i].version = BT_DEV_VERSION;
	memcpy(devices[i].mac, mac, sizeof(devices[i].mac));
	strncpy(devices[i].name, name, sizeof(devices[i].name) - 1U);
	devices[i].pnc = pnc;
	dev_used[i] = true;

	(void)snprintf(key, sizeof(key), "cfg/bt/dev/%u", i);
	ret = settings_save_one(key, &devices[i], sizeof(devices[i]));
	printk("enroll %s -> %s (ret %d)\n", name, key, ret);
}

int main(void)
{
	unsigned int count = 0U;
	int ret;

	printk("\nACBoard settings config store (cfg/ subtree on /lfs)\n");

	ret = settings_subsys_init();
	if (ret < 0) {
		printk("settings_subsys_init failed: %d\n", ret);
		return 0;
	}

	ret = settings_load_subtree("cfg/bt");
	if (ret < 0) {
		printk("settings_load failed: %d\n", ret);
		return 0;
	}

	printk("loaded paired devices:\n");
	print_table();

	for (uint8_t i = 0; i < MAX_DEV; i++) {
		count += dev_used[i] ? 1U : 0U;
	}

	if (count == 0U) {
		const uint8_t mac_a[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
		const uint8_t mac_b[6] = {0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};

		printk("first boot: enrolling demo devices\n");
		enroll(0, "Phone-A", mac_a, 1);
		enroll(1, "Phone-B", mac_b, 0);
	} else {
		/* Prove that updates persist: toggle dev0 plug-and-charge. */
		devices[0].pnc ^= 1U;
		ret = settings_save_one("cfg/bt/dev/0", &devices[0], sizeof(devices[0]));
		printk("toggled dev0 pnc -> %u (ret %d)\n", devices[0].pnc, ret);
	}

	printk("device table now:\n");
	print_table();
	printk("done -- reset to see the table persist and dev0 pnc toggle\n");
	return 0;
}
