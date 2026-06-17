/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/toolchain.h>
#include <zephyr/linker/sections.h>

_ASM_FILE_PROLOGUE

GTEXT(soc_early_reset_hook)

/*
 * GD32F527-specific (gated by CONFIG_GD32F5XX_SRAM_ECC_SCRUB).
 *
 * GD32F527 startup code from GigaDevice initializes the SRAM ECC state by
 * writing the full TCMSRAM and SRAM ranges before C runtime setup. Zephyr calls
 * this hook before it sets up or uses RAM-backed stacks.
 *
 * This is NOT portable: the F527 memory map is hard-coded here, so the Kconfig
 * defaults it on only for SOC_GD32F527. It also runs on *every* reset (including
 * warm/soft resets) and zeroes all of TCMSRAM + SRAM, so this SoC does not
 * preserve any .noinit / retained-RAM contents across a reset.
 */
SECTION_FUNC(TEXT, soc_early_reset_hook)
    /* Zero the 64 KB TCMSRAM at 0x10000000. */
    ldr r0, =0x10000000
    movs r1, #64
    lsls r1, r1, #10           /* r1 = 64 KB byte count */
    movs r2, #0
.Ltcmsram_init:
    stm r0!, {r2}
    subs r1, r1, #4
    cmp r1, #0
    bne .Ltcmsram_init

    /*
     * Zero the main SRAM at 0x20000000. The size word at 0x1FFF7A20 reports
     * total on-chip RAM in KB; subtract the 64 KB TCMSRAM handled above, mask
     * to 16 bits, and convert KB -> bytes (<<10). On the F527 this word is a
     * fixed, valid value (well above 64), so the subtract never underflows;
     * the masking-after-subtract ordering is only safe under that guarantee,
     * which is why this hook is restricted to the F527 part.
     */
    ldr r0, =0x1FFF7A20
    ldr r2, [r0]
    subs r2, r2, #64
    ldr r0, =0x0000FFFF
    ands r2, r2, r0
    lsls r2, r2, #10           /* r2 = (total_kb - 64) bytes */
    ldr r1, =0x20000000
    movs r0, #0
.Lsram_init:
    stm r1!, {r0}
    subs r2, r2, #4
    cmp r2, #0
    bne .Lsram_init

    bx lr
