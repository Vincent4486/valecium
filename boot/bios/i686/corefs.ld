/* SPDX-License-Identifier: GPL-3.0-only */

SECTIONS
{
    . = 0x90000;

    .exports : AT(0)
    {
        *(.exports*)
    }

    .text :
    {
        *(.text*)
        *(.rodata*)
        *(.data*)
    }

    .bss (NOLOAD) :
    {
        __corefs_bss_start = .;
        *(.bss*)
        *(COMMON)
        __corefs_bss_end = .;
    }
    ASSERT(__corefs_bss_end <= 0x9BC00, "corefs leaves no stack reserve")
}
