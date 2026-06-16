/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * Exercises the GD32F527 internal flash (FMC v4) driver: it erases one sector
 * of a scratch partition, verifies the erased pattern, writes a test buffer,
 * and reads it back. The partition lives in Bank1, away from the application
 * image, so the test is non-destructive to running code.
 */

#include <stdint.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/kernel.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/printk.h>

#define TEST_PARTITION		storage_partition
#define TEST_PARTITION_ID	PARTITION_ID(TEST_PARTITION)

#define TEST_OFFSET		0
#define TEST_SIZE		64U

int main(void)
{
	const struct flash_area *fa;
	const struct device *flash_dev;
	struct flash_pages_info page;
	uint8_t write_buf[TEST_SIZE];
	uint8_t read_buf[TEST_SIZE];
	int ret;

	printk("\nACBoard GD32F527 internal flash test\n");

	ret = flash_area_open(TEST_PARTITION_ID, &fa);
	if (ret < 0) {
		printk("flash_area_open failed: %d\n", ret);
		return 0;
	}

	flash_dev = flash_area_get_device(fa);
	printk("partition on %s: offset 0x%lx, size %zu bytes\n",
	       flash_dev->name, (unsigned long)fa->fa_off, fa->fa_size);

	if (!device_is_ready(flash_dev)) {
		printk("flash device not ready\n");
		return 0;
	}

	ret = flash_get_page_info_by_offs(flash_dev, fa->fa_off, &page);
	if (ret < 0) {
		printk("flash_get_page_info failed: %d\n", ret);
		return 0;
	}
	printk("sector at partition start: index %u, size %zu bytes\n",
	       page.index, page.size);

	/* Erase the first sector of the partition. */
	ret = flash_area_erase(fa, TEST_OFFSET, page.size);
	if (ret < 0) {
		printk("erase failed: %d\n", ret);
		return 0;
	}
	printk("erased %zu bytes\n", page.size);

	/* Confirm the erased region reads back as 0xff. */
	ret = flash_area_read(fa, TEST_OFFSET, read_buf, TEST_SIZE);
	if (ret < 0) {
		printk("read after erase failed: %d\n", ret);
		return 0;
	}
	for (size_t i = 0; i < TEST_SIZE; i++) {
		if (read_buf[i] != 0xff) {
			printk("erase verify failed at %zu: 0x%02x\n", i, read_buf[i]);
			return 0;
		}
	}
	printk("erase verified (all 0xff)\n");

	/* Write a known pattern. */
	for (size_t i = 0; i < TEST_SIZE; i++) {
		write_buf[i] = (uint8_t)(i ^ 0xa5);
	}
	ret = flash_area_write(fa, TEST_OFFSET, write_buf, TEST_SIZE);
	if (ret < 0) {
		printk("write failed: %d\n", ret);
		return 0;
	}
	printk("wrote %u bytes\n", TEST_SIZE);

	/* Read it back and compare. */
	memset(read_buf, 0, sizeof(read_buf));
	ret = flash_area_read(fa, TEST_OFFSET, read_buf, TEST_SIZE);
	if (ret < 0) {
		printk("read back failed: %d\n", ret);
		return 0;
	}
	if (memcmp(write_buf, read_buf, TEST_SIZE) != 0) {
		printk("FAIL: read-back data does not match\n");
		return 0;
	}

	printk("PASS: write/read-back verified\n");

	flash_area_close(fa);
	return 0;
}
