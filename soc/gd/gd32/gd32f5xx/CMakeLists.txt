# Copyright (c) 2026
# SPDX-License-Identifier: Apache-2.0

zephyr_include_directories(.)
zephyr_sources(soc.c)
zephyr_sources_ifdef(CONFIG_GD32F5XX_SRAM_ECC_SCRUB early_reset.S)

set(SOC_LINKER_SCRIPT ${ZEPHYR_BASE}/include/zephyr/arch/arm/cortex_m/scripts/linker.ld CACHE INTERNAL "")
