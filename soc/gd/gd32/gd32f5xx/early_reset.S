/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/toolchain.h>
#include <zephyr/linker/sections.h>

_ASM_FILE_PROLOGUE

GTEXT(soc_early_reset_hook)

/*
 * GD32F527 startup code from GigaDevice initializes the SRAM ECC state by
 * writing the full TCMSRAM and SRAM ranges before C runtime setup. Zephyr calls
 * this hook before it sets up or uses RAM-backed stacks.
 */
SECTION_FUNC(TEXT, soc_early_reset_hook)
    ldr r0, =0x10000000
    movs r1, #64
    lsls r1, r1, #10
    movs r2, #0
.Ltcmsram_init:
    stm r0!, {r2}
    subs r1, r1, #4
    cmp r1, #0
    bne .Ltcmsram_init

    ldr r0, =0x1FFF7A20
    ldr r2, [r0]
    subs r2, r2, #64
    ldr r0, =0x0000FFFF
    ands r2, r2, r0
    lsls r2, r2, #10
    ldr r1, =0x20000000
    movs r0, #0
.Lsram_init:
    stm r1!, {r0}
    subs r2, r2, #4
    cmp r2, #0
    bne .Lsram_init

    bx lr
