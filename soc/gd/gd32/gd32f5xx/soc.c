/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/init.h>
#include <soc.h>

/*
 * HAL's SystemInit() calls nvic_vector_table_set() which is in gd32f5xx_misc.c.
 * Provide it here to avoid needing CONFIG_USE_GD32_MISC. Zephyr already sets
 * VTOR during boot.
 */
void nvic_vector_table_set(uint32_t nvic_vict_tab, uint32_t offset)
{
	ARG_UNUSED(nvic_vict_tab);
	ARG_UNUSED(offset);
}

void soc_early_init_hook(void)
{
	/*
	 * Use HAL's SystemInit which handles:
	 * - FPU enable
	 * - RCU reset to default state
	 * - IRC16M clock selection (via __SYSTEM_CLOCK_IRC16M compile flag)
	 * - Flash wait state configuration
	 */
	SystemInit();
}
