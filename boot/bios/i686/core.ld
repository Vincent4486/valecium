/* SPDX-License-Identifier: GPL-3.0-only */

ENTRY(_start)

SECTIONS
{
    /* Core stage is loaded by stage1 to linear address 0x7E00. */
    . = 0x7E00;

    .entry : AT(0)
    {
        *(.entry*)
    }

    /* Kept before .text: 16-bit entry code references this data with
     * R_386_16 relocations, which cannot reach past 0xFFFF. */
    .early_data :
    {
        *(.early_data*)
    }

    .text :
    {
        *(.text*)
        *(.rodata*)
        *(.data*)
    }

    .bss (NOLOAD) :
    {
        __bss_start = .;
        *(.bss*)
        *(COMMON)
        __bss_end = .;
    }
    ASSERT(__bss_end <= 0x90000, "stage2 overlaps corefs")
}
