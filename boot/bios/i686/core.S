/* SPDX-License-Identifier: GPL-3.0-only */

.code16
.section .entry, "ax"
.global _start

_start:
	/*
	 * El Torito detection signature — must be the first 6 bytes
	 * at 0x7E00.  Stage1 checks for "VLSC" at offset +2 to decide
	 * whether to load from disk or jump directly.
	 */
	jmp _start_code
	.ascii "VLSC"
_start_code:
	/* Save the BIOS boot-drive number (passed in DL) immediately. */
	movb	%dl, boot_drive

	xorw	%ax, %ax
	movw	%ax, %ds
	movw	%ax, %es

	/* Detect hardware and build the Multiboot2 Boot Information
	 * structure while BIOS services are still available. */
	movb	boot_drive, %dl
	call	detect_hardware
	movb	%al, available_outputs  /* Save output bitmask for C entry */

	movw	$stage2_message, %si

print_loop:
	lodsb
	testb %al, %al
	jz done
	movb $0x0e, %ah
	movb $0x00, %bh
	movb $0x0a, %bl
	int $0x10
	jmp print_loop

done:
	cli

	/* Enable A20 through System Control Port A (0x92). */
	inb $0x92, %al
	orb $0x02, %al
	andb $0xfe, %al
	outb %al, $0x92

	lgdtl gdt_descriptor

	movl %cr0, %eax
	orl $0x1, %eax
	movl %eax, %cr0

	ljmp $0x08, $protected_mode_entry

.code32
protected_mode_entry:
	movw $0x10, %ax
	movw %ax, %ds
	movw %ax, %es
	movw %ax, %fs
	movw %ax, %gs
	movw %ax, %ss
	movl $0x9fc00, %esp

	cld
	movl $__bss_start, %edi
	movl $__bss_end, %ecx
	subl %edi, %ecx
	xorl %eax, %eax
	rep stosb

	movl	$(MBI_BASE + 8), %eax
	movl	%eax, boot_params
	movl	corefs_start_address, %eax
	movl	%eax, boot_params + 4
	movb	available_outputs, %al
	movb	%al, boot_params + 8
	movb	boot_drive, %al
	movb	%al, boot_params + 9
	movl	BIOS_DRIVE_LIST_PTR, %eax
	movl	%eax, boot_params + 10
	movb	BIOS_DRIVE_LIST_COUNT, %al
	movb	%al, boot_params + 14
	leal	corefs_partition_uuid, %eax
	movl	%eax, boot_params + 15
	leal	corefs_partition_label, %eax
	movl	%eax, boot_params + 19

	pushl	$boot_params
	call	main
	addl	$4, %esp               /* 1 arg × 4 bytes */

hang:
	hlt
	jmp hang

/* Data referenced by 16-bit entry code must stay below 0x10000;
 * placed before .text in core.ld so -O3 code growth cannot push it out. */
.section .early_data, "aw"

.align 8
gdt_start:
	.quad 0x0000000000000000
	.quad 0x00cf9a000000ffff
	.quad 0x00cf92000000ffff
gdt_end:

gdt_descriptor:
	.word gdt_end - gdt_start - 1
	.long gdt_start

/* ================================================================== */
/*  Data section                                                       */
/* ================================================================== */

	.balign 4
boot_drive:
	.byte 0

available_outputs:
	.byte 0

	/* Patched with the corefs load address (real-mode physical). */
	.balign 4
corefs_patch_signature:
	.ascii "VLSF"
corefs_start_address:
	.long 0
corefs_partition_label:
	.space 32
corefs_partition_uuid:
	.space 16

	/* Multiboot2 Boot Information structure base (matches detect.S). */
	.set MBI_BASE,     0x6000
	.set MBI_PTR_SAVE, 0x5000
	.set BIOS_DRIVE_LIST_PTR,   0x5004
	.set BIOS_DRIVE_LIST_COUNT, 0x5008

	.balign 4
boot_params:
	.long 0 /* mbi_tags */
	.long 0 /* corefs_ops */
	.byte 0 /* available_outputs */
	.byte 0 /* boot_drive */
	.long 0 /* bios_drive_list */
	.byte 0 /* bios_drive_count */
	.long 0 /* corefs_partition_uuid */
	.long 0 /* corefs_partition_label */

	/* ---- String constants ---- */
	stage2_message:
		.asciz "Valecium stage2 bootloader loaded"
