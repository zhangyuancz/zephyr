/*
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "flash_gd32.h"

#include <zephyr/logging/log.h>
#include <zephyr/kernel.h>
#include <gd32_fmc.h>

LOG_MODULE_DECLARE(flash_gd32);

#define GD32_NV_FLASH_V4_NODE		DT_INST(0, gd_gd32_nv_flash_v4)
#define GD32_NV_FLASH_V4_TIMEOUT	DT_PROP(GD32_NV_FLASH_V4_NODE, max_erase_time_ms)

/**
 * @brief GD32 FMC v4 flash memory layout for the GD32F5xx series.
 *
 * Unlike the FMC v3 (GD32F4xx) the sector numbers are contiguous, but the
 * sector-number register field is split: SN[4:0] live in FMC_CTL[7:3] and the
 * 6th bit SN[5] lives in FMC_CTL[11]. Each 2MB bank is laid out as
 * 4x16KB + 1x64KB + 15x128KB, and the optional Bank1 extension (only on the
 * 7680KB devices) adds 14x256KB sectors that follow Bank1 contiguously.
 */
#if defined(CONFIG_FLASH_PAGE_LAYOUT) && \
	defined(CONFIG_SOC_SERIES_GD32F5XX)

#define GD32_FMC_V4_BANK_LAYOUT			\
	{.pages_count = 4, .pages_size = KB(16)},	\
	{.pages_count = 1, .pages_size = KB(64)},	\
	{.pages_count = 15, .pages_size = KB(128)}

#if (PRE_KB(4096) == SOC_NV_FLASH_SIZE)
/* 4MB dual bank: Bank0 (sectors 0-19) + Bank1 (sectors 20-39). */
static const struct flash_pages_layout gd32_fmc_v4_layout[] = {
	GD32_FMC_V4_BANK_LAYOUT,
	GD32_FMC_V4_BANK_LAYOUT,
};
#elif (PRE_KB(7680) == SOC_NV_FLASH_SIZE)
/* 7680KB: Bank0 + Bank1 + Bank1 extension (sectors 40-53, 256KB each). */
static const struct flash_pages_layout gd32_fmc_v4_layout[] = {
	GD32_FMC_V4_BANK_LAYOUT,
	GD32_FMC_V4_BANK_LAYOUT,
	{.pages_count = 14, .pages_size = KB(256)},
};
#else
#error "Unknown FMC layout for GD32F5xx series."
#endif
#endif /* CONFIG_FLASH_PAGE_LAYOUT */

#define gd32_fmc_v4_WRITE_ERR (FMC_STAT_PGMERR | FMC_STAT_PGSERR | FMC_STAT_WPERR)
#define gd32_fmc_v4_ERASE_ERR FMC_STAT_OPERR

static inline void gd32_fmc_v4_unlock(void)
{
	FMC_KEY = UNLOCK_KEY0;
	FMC_KEY = UNLOCK_KEY1;
}

static inline void gd32_fmc_v4_lock(void)
{
	FMC_CTL |= FMC_CTL_LK;
}

/* Build the FMC_CTL sector-number bits for a contiguous sector index. */
static inline uint32_t gd32_fmc_v4_sn(uint8_t sector)
{
	if (sector < 32U) {
		return CTL_SN(sector);
	}

	return FMC_CTL_SN_5 | CTL_SN(sector - 32U);
}

static int gd32_fmc_v4_wait_idle(void)
{
	const int64_t expired_time = k_uptime_get() + GD32_NV_FLASH_V4_TIMEOUT;

	while (FMC_STAT & FMC_STAT_BUSY) {
		if (k_uptime_get() > expired_time) {
			return -ETIMEDOUT;
		}
	}

	return 0;
}

bool flash_gd32_valid_range(off_t offset, uint32_t len, bool write)
{
	const struct flash_pages_layout *page_layout;
	uint32_t cur = 0U, next = 0U;

	if ((offset > SOC_NV_FLASH_SIZE) ||
	    ((offset + len) > SOC_NV_FLASH_SIZE)) {
		return false;
	}

	if (write) {
		/* Check offset and len aligned to write-block-size. */
		if ((offset % sizeof(flash_prg_t)) ||
		    (len % sizeof(flash_prg_t))) {
			return false;
		}

	} else {
		for (size_t i = 0; i < ARRAY_SIZE(gd32_fmc_v4_layout); i++) {
			page_layout = &gd32_fmc_v4_layout[i];

			for (size_t j = 0; j < page_layout->pages_count; j++) {
				cur = next;

				next += page_layout->pages_size;

				/* Check bad offset. */
				if ((offset > cur) && (offset < next)) {
					return false;
				}

				/* Check bad len. */
				if (((offset + len) > cur) &&
				    ((offset + len) < next)) {
					return false;
				}

				if ((offset + len) == next) {
					return true;
				}
			}
		}
	}

	return true;
}

int flash_gd32_write_range(off_t offset, const void *data, size_t len)
{
	flash_prg_t *prg_flash = (flash_prg_t *)((uint8_t *)SOC_NV_FLASH_ADDR + offset);
	flash_prg_t *prg_data = (flash_prg_t *)data;
	int ret = 0;

	gd32_fmc_v4_unlock();

	if (FMC_STAT & FMC_STAT_BUSY) {
		return -EBUSY;
	}

	FMC_CTL |= FMC_CTL_PG;

	FMC_CTL &= ~FMC_CTL_PSZ;
	FMC_CTL |= CTL_PSZ(sizeof(flash_prg_t) - 1);

	for (size_t i = 0U; i < (len / sizeof(flash_prg_t)); i++) {
		*prg_flash++ = *prg_data++;
	}

	ret = gd32_fmc_v4_wait_idle();
	if (ret < 0) {
		goto expired_out;
	}

	if (FMC_STAT & gd32_fmc_v4_WRITE_ERR) {
		ret = -EIO;
		FMC_STAT |= gd32_fmc_v4_WRITE_ERR;
		LOG_ERR("FMC programming failed");
	}

expired_out:
	FMC_CTL &= ~FMC_CTL_PG;

	gd32_fmc_v4_lock();

	return ret;
}

static int gd32_fmc_v4_sector_erase(uint8_t sector)
{
	int ret = 0;

	gd32_fmc_v4_unlock();

	if (FMC_STAT & FMC_STAT_BUSY) {
		return -EBUSY;
	}

	FMC_CTL |= FMC_CTL_SER;

	FMC_CTL &= ~(FMC_CTL_SN | FMC_CTL_SN_5);
	FMC_CTL |= gd32_fmc_v4_sn(sector);

	FMC_CTL |= FMC_CTL_START;

	ret = gd32_fmc_v4_wait_idle();
	if (ret < 0) {
		goto expired_out;
	}

	if (FMC_STAT & gd32_fmc_v4_ERASE_ERR) {
		ret = -EIO;
		FMC_STAT |= gd32_fmc_v4_ERASE_ERR;
		LOG_ERR("FMC sector %u erase failed", sector);
	}

expired_out:
	FMC_CTL &= ~FMC_CTL_SER;

	gd32_fmc_v4_lock();

	return ret;
}

int flash_gd32_erase_block(off_t offset, size_t size)
{
	const struct flash_pages_layout *page_layout;
	uint32_t erase_offset = 0U;
	uint8_t sector = 0U;
	int ret = 0;

	for (size_t i = 0; i < ARRAY_SIZE(gd32_fmc_v4_layout); i++) {
		page_layout = &gd32_fmc_v4_layout[i];

		for (size_t j = 0; j < page_layout->pages_count; j++) {
			if (erase_offset < offset) {
				sector++;
				erase_offset += page_layout->pages_size;

				continue;
			}

			ret = gd32_fmc_v4_sector_erase(sector++);
			if (ret < 0) {
				return ret;
			}

			erase_offset += page_layout->pages_size;

			if (erase_offset - offset >= size) {
				return 0;
			}
		}
	}

	return 0;
}

#ifdef CONFIG_FLASH_PAGE_LAYOUT
void flash_gd32_pages_layout(const struct device *dev,
			     const struct flash_pages_layout **layout,
			     size_t *layout_size)
{
	ARG_UNUSED(dev);

	*layout = gd32_fmc_v4_layout;
	*layout_size = ARRAY_SIZE(gd32_fmc_v4_layout);
}
#endif /* CONFIG_FLASH_PAGE_LAYOUT */
