# Copyright (c) 2026
# SPDX-License-Identifier: Apache-2.0

zephyr_include_directories(.)
zephyr_sources(soc.c)

# Select GD32F527 device for HAL
zephyr_compile_definitions(GD32F527)

# Crystal frequency (25MHz on acboard)
zephyr_compile_definitions(HXTAL_VALUE=25000000)

# Use 200MHz PLL from 25MHz HXTAL (PSC=25, N=400, P=2)
zephyr_compile_definitions(__SYSTEM_CLOCK_200M_PLL_25M_HXTAL=200000000)

set(SOC_LINKER_SCRIPT ${ZEPHYR_BASE}/include/zephyr/arch/arm/cortex_m/scripts/linker.ld CACHE INTERNAL "")
