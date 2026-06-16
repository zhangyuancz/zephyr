/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * Demonstrates LittleFS configuration storage on the GD32F527 Bank1 flash
 * (2MB) of ACBoard-F527. The filesystem is auto-mounted at /lfs by the fstab
 * entry. The sample keeps a persistent boot counter and a small key-value
 * config file to show that data survives resets and power cycles.
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#define MOUNT_POINT	"/lfs"
#define COUNTER_PATH	MOUNT_POINT "/boot_count"
#define CONFIG_PATH	MOUNT_POINT "/device.cfg"

static int read_boot_count(uint32_t *count)
{
	struct fs_file_t file;
	int ret;

	fs_file_t_init(&file);
	*count = 0U;

	ret = fs_open(&file, COUNTER_PATH, FS_O_CREATE | FS_O_RDWR);
	if (ret < 0) {
		printk("open %s failed: %d\n", COUNTER_PATH, ret);
		return ret;
	}

	ret = fs_read(&file, count, sizeof(*count));
	if (ret < 0) {
		printk("read boot count failed: %d\n", ret);
		(void)fs_close(&file);
		return ret;
	}
	if (ret < (int)sizeof(*count)) {
		/* Fresh file. */
		*count = 0U;
	}

	(void)fs_close(&file);
	return 0;
}

static int write_boot_count(uint32_t count)
{
	struct fs_file_t file;
	int ret;

	fs_file_t_init(&file);

	ret = fs_open(&file, COUNTER_PATH, FS_O_CREATE | FS_O_RDWR);
	if (ret < 0) {
		return ret;
	}

	ret = fs_seek(&file, 0, FS_SEEK_SET);
	if (ret == 0) {
		ret = fs_write(&file, &count, sizeof(count));
	}

	(void)fs_close(&file);
	return (ret < 0) ? ret : 0;
}

static int store_config(const char *text)
{
	struct fs_file_t file;
	int ret;

	fs_file_t_init(&file);

	ret = fs_open(&file, CONFIG_PATH, FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC);
	if (ret < 0) {
		return ret;
	}

	ret = fs_write(&file, text, strlen(text));
	(void)fs_close(&file);
	return (ret < 0) ? ret : 0;
}

static int load_config(char *buf, size_t buf_size)
{
	struct fs_file_t file;
	int ret;

	fs_file_t_init(&file);

	ret = fs_open(&file, CONFIG_PATH, FS_O_READ);
	if (ret < 0) {
		return ret;
	}

	ret = fs_read(&file, buf, buf_size - 1U);
	if (ret >= 0) {
		buf[ret] = '\0';
	}
	(void)fs_close(&file);
	return ret;
}

int main(void)
{
	struct fs_statvfs stat;
	char config[64];
	uint32_t boot_count;
	int ret;

	printk("\nACBoard GD32F527 LittleFS config storage (Bank1, 2MB)\n");

	ret = fs_statvfs(MOUNT_POINT, &stat);
	if (ret < 0) {
		printk("%s not mounted: %d\n", MOUNT_POINT, ret);
		return 0;
	}
	printk("%s mounted: block size %lu, blocks %lu, free %lu\n", MOUNT_POINT,
	       stat.f_bsize, stat.f_blocks, stat.f_bfree);

	ret = read_boot_count(&boot_count);
	if (ret < 0) {
		return 0;
	}
	boot_count++;
	printk("boot count: %u\n", boot_count);

	ret = write_boot_count(boot_count);
	if (ret < 0) {
		printk("persist boot count failed: %d\n", ret);
		return 0;
	}

	/* On the first boot store an example device config; afterwards read it
	 * back to prove persistence.
	 */
	struct fs_dirent ent;

	ret = fs_stat(CONFIG_PATH, &ent);
	if (ret == -ENOENT) {
		const char *initial = "name=ID.UNYX;pnc=1";

		printk("no config yet, writing defaults\n");
		ret = store_config(initial);
		if (ret < 0) {
			printk("store config failed: %d\n", ret);
			return 0;
		}
		strcpy(config, initial);
	} else if (ret == 0) {
		ret = load_config(config, sizeof(config));
		if (ret < 0) {
			printk("load config failed: %d\n", ret);
			return 0;
		}
	} else {
		printk("stat config failed: %d\n", ret);
		return 0;
	}
	printk("config: %s\n", config);

	printk("done -- reset the board to see the boot count increment\n");
	return 0;
}
