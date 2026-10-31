/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * detect.S — Hardware detection for BIOS/i686 stage2 bootloader.
 *
 * Builds a Multiboot2 Boot Information structure at 0x6000 containing:
 *   - Basic Memory Info tag   (type 4)
 *   - Boot Device tag         (type 5)
 *   - Memory Map tag          (type 6) — via INT 0x15 E820
 *   - End tag                 (type 0)
 *
 * The pointer (0x6000) is saved to memory at 0x5000 so that
 * 32-bit protected mode code can retrieve and display the data.
 *
 * Runs in 16-bit real mode.  Expects the BIOS boot-drive number
 * in DL.  Clobbers AX, CX, DX, SI, DI, BX, ES, DS.
 */

.code16
/* Keep 16-bit code with .entry so the real-mode call from core.S stays
 * within rel16 range regardless of .text size at higher optimization. */
.section .entry, "ax"
.global detect_hardware

/* ------------------------------------------------------------------ */
/*  Memory layout constants                                            */
/* ------------------------------------------------------------------ */
.equ MBI_BASE,     0x6000          /* Multiboot2 info structure base   */
.equ MBI_PTR_SAVE, 0x5000          /* Where to stash the pointer       */
/* BIOS drive list (real-mode, identity). */
.equ BIOS_DRIVE_LIST_BASE,  0x5100 /* u8[] drive numbers               */
.equ BIOS_DRIVE_LIST_PTR,   0x5004 /* Where to stash list pointer      */
.equ BIOS_DRIVE_LIST_COUNT, 0x5008 /* Where to stash list count (u32)  */
/* VBE buffers/scratch (real-mode, identity). */
.equ VBE_INFO,      0x0500         /* VBE Controller Info buffer       */
.equ VBE_MODE_INFO, 0x0600         /* VBE Mode Info buffer             */
.equ VBE_SCRATCH,   0x5200         /* Scratch storage for VBE selects  */
.equ VBE_BEST_MODE, (VBE_SCRATCH + 0)
.equ VBE_BEST_AREA, (VBE_SCRATCH + 2)
.equ VBE_BEST_BPP,  (VBE_SCRATCH + 6)
.equ VBE_MBI_DI,    (VBE_SCRATCH + 8)
.equ VBE_REQ_WIDTH, (VBE_SCRATCH + 10)
.equ VBE_REQ_HEIGHT, (VBE_SCRATCH + 12)
.equ VBE_REQ_WIDTH_VAL, 1920
.equ VBE_REQ_HEIGHT_VAL, 1080

/* ------------------------------------------------------------------ */
/*  Output Mode Enable/Disable Flags (edit to change)                 */
/*  Set these to 1 to enable, 0 to disable                            */
/* ------------------------------------------------------------------ */
.equ ENABLE_VBE, 1                  /* Enable VBE detection             */
.equ ENABLE_VGA_GFX, 1              /* Enable VGA graphics detection    */
.equ ENABLE_VGA_TEXT, 1             /* Enable VGA text mode detection   */
.equ ENABLE_UART_SERIAL, 1          /* Enable UART serial detection     */

/* ------------------------------------------------------------------ */
/*  detect_hardware                                                    */
/* ------------------------------------------------------------------ */
detect_hardware:
   pushw %bp
   movw  %sp, %bp

   /* Segment registers → 0 for physical addressing. */
   xorw  %ax, %ax
   movw  %ax, %ds
   movw  %ax, %es

   /* Save MBI pointer so 32-bit code can find it. */
   movl  $MBI_BASE, %eax
   movl  %eax, (MBI_PTR_SAVE)

   /* Save BIOS drive list pointer and clear count. */
   movl  $BIOS_DRIVE_LIST_BASE, %eax
   movl  %eax, (BIOS_DRIVE_LIST_PTR)
   movl  $0, (BIOS_DRIVE_LIST_COUNT)

   /* DI = current write offset inside the MBI structure. */
   movw  $MBI_BASE, %di

   /* Skip the 8-byte header (total_size + reserved), fill later. */
   addw  $8, %di

   /* ============================================================ */
   /*  Tag 1 — Basic Memory Info  (type 4, size 16)                */
   /* ============================================================ */
   movl  $4,  %es:(%di)          /* type                          */
   movl  $16, %es:4(%di)         /* size                          */

   /* Lower memory via INT 0x12.  Returns AX = KB. */
   int   $0x12
   movw  %ax, %es:8(%di)         /* mem_lower  low word           */
   movw  $0,  %es:10(%di)        /* mem_lower  high word          */

   /* mem_upper is filled from E820 when possible; init to 0. */
   movw  $0,  %es:12(%di)
   movw  $0,  %es:14(%di)

.Lmeminfo_done:
   addw  $16, %di                /* Advance past this tag        */

   /* ============================================================ */
   /*  Tag 2 — Boot Device  (type 5, size 20)                      */
   /* ============================================================ */
   movl  $5,  %es:(%di)          /* type                          */
   movl  $20, %es:4(%di)         /* size                          */
   movw  %dx, %es:8(%di)         /* biosdev  (DL = boot drive)   */
   movw  $0,  %es:10(%di)        /* high word of biosdev         */
   movl  $0,  %es:12(%di)        /* partition   = 0              */
   movl  $0,  %es:16(%di)        /* subpartition = 0             */

   addw  $20, %di

   /* Align DI up to the next 8-byte boundary for the next tag. */
   movw  %di, %ax
   addw  $7, %ax
   andw  $0xFFF8, %ax
   movw  %ax, %di

   /* ============================================================ */
   /*  Tag 3 — Memory Map  (type 6)  —  E820 detection             */
   /* ============================================================ */
   movw  %di, %bp                /* Remember tag start offset    */

   movl  $6,  %es:(%di)          /* type                          */
   movl  $0,  %es:4(%di)         /* size    ← placeholder        */
   movl  $24, %es:8(%di)         /* entry_size                   */
   movl  $0,  %es:12(%di)        /* entry_version                */
   addw  $16, %di                /* Advance past mmap header     */

   /* Query the BIOS E820 memory map.
    * We fill entries directly into the MBI buffer.
    * SI = entry counter; DI = next entry offset;
    * BP = tag start offset.
    */
   xorw  %si, %si                /* entry counter                */
   movl  $0xE820, %eax
   xorl  %ebx, %ebx              /* start at beginning           */
   movl  $0x534D4150, %edx       /* 'SMAP'                       */
   movl  $24, %ecx               /* request 24-byte entries      */
   jmp   .Le820_first

.Le820_loop:
   int   $0x15
   jc .Le820_done             /* CF set → error / end        */
   cmpl  $0x534D4150, %eax
   jne   .Le820_done             /* SMAP mismatch → stop        */

   /* ECX = bytes actually written (20 or 24).  Skip if zero. */
   testl %ecx, %ecx
   jz .Le820_skip
   /* Record actual entry_size (20 or 24) in the tag header. */
   movl  %ecx, %es:8(%bp)

   addl  %ecx, %edi              /* advance past this entry     */
   incw  %si                     /* one more valid entry        */

.Le820_skip:
   testl %ebx, %ebx
   jz .Le820_done             /* EBX = 0 → no more entries  */

.Le820_first:
   /* Re-arm registers that INT 0x15 may have clobbered. */
   movl  $0xE820, %eax
   movl  $0x534D4150, %edx
   movl  $24, %ecx
   jmp   .Le820_loop

.Le820_done:
   /* Compute mmap tag size from actual bytes written.
    * tag_size = (DI - BP)  (total from mmap start to after entries)
    * This correctly handles both 20-byte and 24-byte entries.
    */
   movw  %di, %ax
   subw  %bp, %ax                /* AX = mmap tag + entries total */
   movw  %ax, %es:4(%bp)         /* store real mmap tag size     */

   /* If E820 returned no entries, use legacy INT 0x15 (E801/88). */
   cmpw  $0, %si
   jne   .Le820_has_entries

   /* Extended memory — try INT 0x15, AX = 0xE801 first. */
   movw  $0xE801, %ax
   stc
   int   $0x15
   jc .Ltry_int88_legacy

   /* E801 succeeded.
    * AX = memory 1-16 MB  (KB)
    * BX = memory >16 MB   (64 KB blocks)
    * Upper = AX + (BX << 6)   KB
    */
   movw  %bx, %cx
   xorw  %bx, %bx
   shlw  $6, %cx                 /* CX = BX * 64                 */
   addw  %cx, %ax
   movw  %ax, %es:(MBI_BASE + 12)
   movw  $0,  %es:(MBI_BASE + 14)
   jmp   .Lmeminfo_e820_done

.Ltry_int88_legacy:
   /* Fallback: INT 0x15, AH = 0x88.  Returns AX = KB (>1 MB). */
   movw  $0x8800, %ax
   stc
   int   $0x15
   movw  %ax, %es:(MBI_BASE + 12)
   movw  $0,  %es:(MBI_BASE + 14)
   jmp   .Lmeminfo_e820_done

.Le820_has_entries:

   /* Zero mem_lower/mem_upper (KB) before summing. */
   movl  $0, %es:(MBI_BASE + 16)
   movl  $0, %es:(MBI_BASE + 20)

   /* Setup entry walk. */
   movw  %si, %cx                /* entry count (16-bit)         */
   movl  %ebp, %edi
   addl  $16, %edi               /* EDI = first entry            */
   movl  %es:8(%ebp), %ebx       /* entry_size                   */

.Lmeminfo_e820_loop:
   cmpw  $0, %cx
   je .Lmeminfo_e820_done

   /* type == 1? */
   movl  16(%edi), %eax
   cmpl  $1, %eax
   jne   .Lmeminfo_next

   /* Skip if base_high or len_high are non-zero (beyond 4 GB). */
   movl  4(%edi), %eax
   testl %eax, %eax
   jne   .Lmeminfo_next
   movl  12(%edi), %eax
   testl %eax, %eax
   jne   .Lmeminfo_next

   /* base_low -> ESI, len_low -> EAX, end_low -> EDX */
   movl  (%edi), %esi
   movl  8(%edi), %eax
   movl  %esi, %edx
   addl  %eax, %edx
   jc .Lmeminfo_end_overflow
   shrl  $10, %esi               /* base_kb */
   shrl  $10, %edx               /* end_kb  */
   jmp   .Lmeminfo_have_kb

.Lmeminfo_end_overflow:
   shrl  $10, %esi
   movl  $0x100000, %edx         /* 4 GB in KB */

.Lmeminfo_have_kb:
   /* Save end_kb on stack for reuse. */
   pushl %edx

   /* mem_lower: overlap with [0, 1024) KB */
   cmpl  $0x400, %esi
   jge   .Lmeminfo_lower_done
   movl  %edx, %eax
   cmpl  $0x400, %eax
   jle   .Lmeminfo_lower_end_ok
   movl  $0x400, %eax
.Lmeminfo_lower_end_ok:
   cmpl  %esi, %eax
   jle   .Lmeminfo_lower_done
   subl  %esi, %eax
   addl  %eax, %es:(MBI_BASE + 16)

.Lmeminfo_lower_done:
   /* mem_upper: overlap with [1024, end) KB */
   popl  %eax                   /* end_kb */
   cmpl  $0x400, %eax
   jle   .Lmeminfo_next
   movl  %esi, %edx
   cmpl  $0x400, %edx
   jge   .Lmeminfo_upper_ok
   movl  $0x400, %edx
.Lmeminfo_upper_ok:
   subl  %edx, %eax
   jle   .Lmeminfo_next
   addl  %eax, %es:(MBI_BASE + 20)

.Lmeminfo_next:
   addl  %ebx, %edi              /* next entry */
   decw  %cx
   jmp   .Lmeminfo_e820_loop

.Lmeminfo_e820_done:

   /* Preserve the current MBI write pointer. */
   movw  %di, VBE_MBI_DI

   /* ============================================================ */
   /*  BIOS Drive Enumeration                                      */
   /*  Scans all BIOS drive numbers (0x00-0xFF) and records         */
   /*  present drives in BIOS_DRIVE_LIST_BASE.                      */
   /*                                                               */
   /*  Uses INT 13h AH=15h (Get Disk Type) which reliably reports   */
   /*  whether a drive is present (AH != 0 on success, CF=0).       */
   /*  This works for floppies (0x00-0x7F), hard disks (0x80-0xDF), */
   /*  and CD-ROMs (0xE0-0xFF).                                     */
   /* ============================================================ */
   movw  $BIOS_DRIVE_LIST_BASE, %si
   xorw  %cx, %cx                /* CX = drive count              */
   xorb  %bl, %bl                /* BL = drive number             */

.Ldrive_scan_loop:
   movb  %bl, %dl
   movb  $0x15, %ah              /* INT 13h: Get Disk Type        */

   /* Preserve registers that INT 13h may clobber (BX, CX, SI). */
   pushw %bx
   pushw %cx
   pushw %si
   int   $0x13
   popw  %si
   popw  %cx
   popw  %bx

   jc .Ldrive_scan_next       /* CF=1 → BIOS doesn't support  */
   testb %ah, %ah
   jz .Ldrive_scan_next       /* AH=0 → drive not present      */

   /* Keep one byte for the terminator before VBE scratch at 0x5200. */
   cmpw  $255, %cx
   jae   .Ldrive_scan_next
   movb  %bl, %es:(%si)
   incw  %si
   incw  %cx

.Ldrive_scan_next:
   incb  %bl
   jnz   .Ldrive_scan_loop

   /* Store count as u32 for 32-bit code. */
   movw  %cx, (BIOS_DRIVE_LIST_COUNT)
   movw  $0, (BIOS_DRIVE_LIST_COUNT + 2)
   /* Terminate list with 0x00 so FS_Initialize can iterate
      without a separate count. */
   movb  $0, %es:(%si)
   /* ============================================================ */
   /*  Output System Availability Checks                            */
   /*  Builds an 8-bit bitmask in AL:                                */
   /*    bit 0: VBE (VESA BIOS Extensions)                          */
   /*    bit 1: VGA graphics mode                                   */
   /*    bit 2: VGA text mode                                       */
   /*    bit 3: UART serial port (COM1)                             */
   /* ============================================================ */

   xorw  %cx, %cx                /* CX = bitmask accumulator     */

   /* --- VBE (bit 0) via INT 0x10, AX = 0x4F00 ------------------ */
.if ENABLE_VBE
   movw  $0x4F00, %ax
   movw  $VBE_INFO, %di          /* ES:DI → VBE info buffer       */
   int   $0x10
   cmpw  $0x004F, %ax
   jne   .Lout_vbe_no

   /* Initialize VBE selection scratch. */
   movw  $0xFFFF, VBE_BEST_MODE
   movl  $0, VBE_BEST_AREA
   movb  $0, VBE_BEST_BPP
   /* Requested resolution (0x0 = auto-detect). */
   movw  $VBE_REQ_WIDTH_VAL, VBE_REQ_WIDTH
   movw  $VBE_REQ_HEIGHT_VAL, VBE_REQ_HEIGHT

   /* Walk mode list and pick the largest RGB LFB mode. */
   movw  VBE_INFO + 0x0E, %si    /* mode list offset */
   movw  VBE_INFO + 0x10, %bx    /* mode list segment */
   movw  %bx, %es
.Lvbe_mode_loop:
   movw  %es:(%si), %dx          /* mode number */
   cmpw  $0xFFFF, %dx
   je .Lvbe_mode_done
   addw  $2, %si

   pushw %es
   pushw %cx
   movw  $0x0000, %ax
   movw  %ax, %es
   movw  $VBE_MODE_INFO, %di
   movw  $0x4F01, %ax
   movw  %dx, %cx
   int   $0x10
   popw  %cx
   popw  %es
   cmpw  $0x004F, %ax
   jne   .Lvbe_mode_loop

   movw  VBE_MODE_INFO + 0x00, %ax
   testw $0x0001, %ax            /* mode supported */
   jz .Lvbe_mode_loop
   testw $0x0080, %ax            /* LFB available */
   jz .Lvbe_mode_loop

   movb  VBE_MODE_INFO + 0x19, %al
   cmpb  $15, %al                /* require >= 15bpp */
   jb .Lvbe_mode_loop
   movb  VBE_MODE_INFO + 0x1B, %al
   cmpb  $0x06, %al              /* direct color */
   jne   .Lvbe_mode_loop

   movw  VBE_REQ_WIDTH, %ax
   orw   VBE_REQ_HEIGHT, %ax
   jz .Lvbe_auto_mode

   movw  VBE_MODE_INFO + 0x12, %ax
   cmpw  VBE_REQ_WIDTH, %ax
   jne   .Lvbe_mode_loop
   movw  VBE_MODE_INFO + 0x14, %ax
   cmpw  VBE_REQ_HEIGHT, %ax
   jne   .Lvbe_mode_loop

   movb  VBE_MODE_INFO + 0x19, %al
   movb  VBE_BEST_BPP, %bl
   cmpb  %bl, %al
   jle   .Lvbe_mode_loop
   movw  %dx, VBE_BEST_MODE
   movb  %al, VBE_BEST_BPP
   jmp   .Lvbe_mode_loop

.Lvbe_auto_mode:
   movw  VBE_MODE_INFO + 0x12, %ax
   movw  VBE_MODE_INFO + 0x14, %bx
   movzwl   %ax, %eax
   movzwl   %bx, %ebx
   imull %ebx, %eax

   movl  VBE_BEST_AREA, %ebx
   cmpl  %ebx, %eax
   jl .Lvbe_mode_loop
   jg .Lvbe_take_mode
   movb  VBE_BEST_BPP, %bl
   movb  VBE_MODE_INFO + 0x19, %al
   cmpb  %bl, %al
   jle   .Lvbe_mode_loop
.Lvbe_take_mode:
   movw  %dx, VBE_BEST_MODE
   movl  %eax, VBE_BEST_AREA
   movb  VBE_MODE_INFO + 0x19, %al
   movb  %al, VBE_BEST_BPP
   jmp   .Lvbe_mode_loop

.Lvbe_mode_done:
   movw  VBE_BEST_MODE, %dx
   cmpw  $0xFFFF, %dx
   je .Lout_vbe_no

   /* Set the selected VBE mode with LFB enabled. */
   movw  $0x4F02, %ax
   movw  %dx, %bx
   orw   $0x4000, %bx
   int   $0x10
   cmpw  $0x004F, %ax
   jne   .Lout_vbe_no

   /* Refresh mode info for the selected mode. */
   pushw %cx
   movw  $0x0000, %ax
   movw  %ax, %es
   movw  $VBE_MODE_INFO, %di
   movw  $0x4F01, %ax
   movw  %dx, %cx
   int   $0x10
   popw  %cx
   cmpw  $0x004F, %ax
   jne   .Lout_vbe_no

   /* Build Multiboot2 framebuffer tag (type 8). */
   movw  VBE_MBI_DI, %di
   movw  %di, %ax
   addw  $7, %ax
   andw  $0xFFF8, %ax
   movw  %ax, %di

   movl  $8, %es:(%di)
   movl  $40, %es:4(%di)
   movl  VBE_MODE_INFO + 0x28, %eax
   movl  %eax, %es:8(%di)
   movl  $0, %es:12(%di)

   movw  VBE_MODE_INFO + 0x10, %ax
   movw  %ax, %es:16(%di)
   movw  $0, %es:18(%di)
   movw  VBE_MODE_INFO + 0x12, %ax
   movw  %ax, %es:20(%di)
   movw  $0, %es:22(%di)
   movw  VBE_MODE_INFO + 0x14, %ax
   movw  %ax, %es:24(%di)
   movw  $0, %es:26(%di)

   movb  VBE_MODE_INFO + 0x19, %al
   movb  %al, %es:28(%di)
   movb  $1, %es:29(%di)         /* framebuffer_type = RGB */
   movw  $0, %es:30(%di)

   movb  VBE_MODE_INFO + 0x20, %al
   movb  %al, %es:32(%di)
   movb  VBE_MODE_INFO + 0x1F, %al
   movb  %al, %es:33(%di)
   movb  VBE_MODE_INFO + 0x22, %al
   movb  %al, %es:34(%di)
   movb  VBE_MODE_INFO + 0x21, %al
   movb  %al, %es:35(%di)
   movb  VBE_MODE_INFO + 0x24, %al
   movb  %al, %es:36(%di)
   movb  VBE_MODE_INFO + 0x23, %al
   movb  %al, %es:37(%di)
   movw  $0, %es:38(%di)

   addw  $40, %di
   movw  %di, VBE_MBI_DI
   orw   $0x01, %cx              /* Set bit 0 */
.endif
.Lout_vbe_no:
   /* Restore MBI write pointer for subsequent tags. */
   movw  VBE_MBI_DI, %di
   movw  $0x0000, %ax
   movw  %ax, %es

   /* --- VGA Graphics (bit 1) via INT 0x10, AX = 0x1A00 --------- */
.if ENABLE_VGA_GFX
   /* Get Display Combination Code — supported by VGA BIOSes. */
   movw  $0x1A00, %ax
   int   $0x10
   cmpb  $0x1A, %al
   jne   .Lout_vga_gfx_no
   orw   $0x02, %cx              /* Set bit 1                     */
.endif
.Lout_vga_gfx_no:

   /* --- VGA Text (bit 2) via INT 0x10, AH = 0x0F -------------- */
.if ENABLE_VGA_TEXT
   /* Get current video mode — confirms INT 0x10 text I/O works. */
   movb  $0x0F, %ah
   int   $0x10
   orw   $0x04, %cx              /* Set bit 2 (always succeeds)   */
.endif

   /* --- UART Serial (bit 3) via COM1 scratch register --------- */
.if ENABLE_UART_SERIAL
   /* Probe the UART scratch register at COM1 + 7 = 0x3FF. */
   movw  $0x3FF, %dx
   inb   %dx, %al                /* Save original                 */
   movb  %al, %ah
   movb  $0xA5, %al              /* Test pattern                  */
   outb  %al, %dx                /* Write to scratch register     */
   inb   %dx, %al                 /* Read back                     */
   cmpb  $0xA5, %al
   jne   .Lout_uart_no
   orw   $0x08, %cx              /* Set bit 3                     */
.endif
.Lout_uart_no:
   /* Restore scratch register to original value. */
   movb  %ah, %al
   outb  %al, %dx

   /* Align DI up to 8-byte boundary for the end tag. */
   movw  %di, %ax
   addw  $7, %ax
   andw  $0xFFF8, %ax
   movw  %ax, %di

   /* ============================================================ */
   /*  End Tag  (type 0, size 8)                                    */
   /* ============================================================ */
   movl  $0, %es:(%di)
   movl  $8, %es:4(%di)
   addw  $8, %di

   /* ============================================================ */
   /*  Write total_size at the beginning of the structure.          */
   /* ============================================================ */
   movw  %di, %ax
   subw  $MBI_BASE, %ax          /* AX = total size             */
   movw  %ax, (MBI_BASE)         /* total_size low word         */
   movw  $0,  (MBI_BASE + 2)     /* total_size high word        */
   movl  $0,  (MBI_BASE + 4)     /* reserved = 0                */

   /* Move final bitmask from CL to AL for return. */
   movw  %cx, %ax

   popw  %bp
   retw
