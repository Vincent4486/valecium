// SPDX-License-Identifier: GPL-3.0-only

#include <stdint.h>

#include "video/video.h"
#include <bits.h>
#include <colors.h>
#include <logging.h>
#include <paths.h>
#include <status.h>

#define DL_RESOLVE
#include <dl/binding_gen.h>
#include <dl/callback.h>
#include <dl/loader.h>
#undef DL_RESOLVE

typedef struct CoreFsOperations CoreFsOperations;
typedef struct MbiTagFramebuffer MbiTagFramebuffer;
typedef struct BootParams BootParams;

static void init_framebuffer_info();
static void print_bios_drive_list(void);
static void print_stage3_fs_location(void);

/* Multiboot2 tag types */
#define MBI_TAG_END 0
#define MBI_TAG_MMAP 6
#define MBI_TAG_FRAMEBUFFER 8

#define STAGE3_BUF_ADDR 0x100000
#define STAGE3_BUF_SIZE (512 * 1024)

struct CoreFsOperations
{
   int (*Initialize)(const uint8_t *, uint32_t, const uint8_t *,
                     const uint8_t *);
   int (*Open)(const char *);
   int (*Read)(int, void *, int);
   int (*Close)(int);
   int (*DISK_Read)(uint8_t drive, uint16_t cylinder, uint8_t sector,
                    uint8_t head, uint8_t count, void *buffer);
   int (*DISK_ReadLBA)(uint8_t drive, uint64_t lba, uint16_t count,
                       void *buffer);
};

struct MbiTagFramebuffer
{
   uint32_t type;
   uint32_t size;
   uint64_t framebuffer_addr;
   uint32_t framebuffer_pitch;
   uint32_t framebuffer_width;
   uint32_t framebuffer_height;
   uint8_t framebuffer_bpp;
   uint8_t framebuffer_type;
   uint16_t reserved;
   uint8_t red_field_position;
   uint8_t red_mask_size;
   uint8_t green_field_position;
   uint8_t green_mask_size;
   uint8_t blue_field_position;
   uint8_t blue_mask_size;
   uint8_t rgb_reserved[2];
};

struct __attribute__((packed)) BootParams
{
   MbiTagFramebuffer *mbi_tags;
   CoreFsOperations *corefs_ops;
   uint8_t available_outputs;
   uint8_t boot_drive;
   uint8_t *bios_drive_list;
   uint8_t bios_drive_count;
   uint8_t *corefs_partition_uuid;
   uint8_t *corefs_partition_label;
};

int g_PreferredOutput = OUTPUT_VGATEXT;

static BootParams s_BootParams = {0};
static DL_CallbackOperations s_DlCallbackOps = {0};

static void init_framebuffer_info()
{
   MbiTagFramebuffer *tag = s_BootParams.mbi_tags;
   for (;;)
   {
      if (tag->type == MBI_TAG_END) break;

      if (tag->type == MBI_TAG_FRAMEBUFFER &&
          tag->size >= sizeof(MbiTagFramebuffer))
      {
         if (tag->framebuffer_type == 1)
         {
            VBE_Info info;
            info.framebuffer_addr = tag->framebuffer_addr;
            info.pitch = tag->framebuffer_pitch;
            info.width = tag->framebuffer_width;
            info.height = tag->framebuffer_height;
            info.bpp = tag->framebuffer_bpp;
            info.red_field_position = tag->red_field_position;
            info.red_mask_size = tag->red_mask_size;
            info.green_field_position = tag->green_field_position;
            info.green_mask_size = tag->green_mask_size;
            info.blue_field_position = tag->blue_field_position;
            info.blue_mask_size = tag->blue_mask_size;
            VBE_SetInfo(&info);
         }
      }

      /* Advance to next tag (8-byte aligned); size is in bytes. */
      tag = (MbiTagFramebuffer *)((uint8_t *)tag + tag->size);
      tag = (MbiTagFramebuffer *)(((uintptr_t)tag + 7) & ~(uintptr_t)7);
   }
}

static void print_bios_drive_list(void)
{
   uint32_t i;

   printf("Detected BIOS drives:\n");

   if (!s_BootParams.bios_drive_list || s_BootParams.bios_drive_count == 0)
   {
      printf("  (none)\n");
      return;
   }

   for (i = 0; i < s_BootParams.bios_drive_count; i++)
   {
      printf("  0x%x\n", s_BootParams.bios_drive_list[i]);
   }
}

static void print_stage3_fs_location(void)
{
   printf("Partition label: \"");
   for (int i = 0; i < 32; i++)
   {
      printf("%c", s_BootParams.corefs_partition_label[i]);
   }
   printf("\".\n");

   printf("Partition UUID: ");
   for (int i = 0; i < 16; i++)
   {
      printf("%02x", s_BootParams.corefs_partition_uuid[i]);
   }
   printf(".\n");
}

void print_memory_map(void)
{
   uint8_t *ptr = (uint8_t *)s_BootParams.mbi_tags;
   printf("Memory Map:\n");
   for (;;)
   {
      uint32_t type = *(uint32_t *)ptr;
      uint32_t size = *(uint32_t *)(ptr + 4);

      if (type == MBI_TAG_END) break;

      if (type == MBI_TAG_MMAP)
      {
         uint32_t entry_size = *(uint32_t *)(ptr + 8);
         /* uint32_t entry_version = *(uint32_t *)(ptr + 12); */
         uint8_t *entry = ptr + 16;
         uint32_t total_size = size - 16;
         uint32_t count = total_size / entry_size;
         uint32_t i;

         for (i = 0; i < count; i++)
         {
            uint64_t base = *(uint64_t *)entry;
            uint64_t len = *(uint64_t *)(entry + 8);
            uint32_t type2 = *(uint32_t *)(entry + 16);

            printf("  base=%x\n", base);
            printf("  len =%x\n", len);
            printf("  type=%d\n", (int)type2);
            printf("  --\n");

            entry += entry_size;
         }
      }

      /* Advance to next tag (8-byte aligned) */
      ptr += size;
      ptr = (uint8_t *)(((uintptr_t)ptr + 7) & ~(uintptr_t)7);
   }
}

/* Print which output systems are reported as available. */
void print_available_outputs(void)
{
   printf("Available outputs:\n");

   if (s_BootParams.available_outputs & (1 << OUTPUT_VBE)) printf("  VBE\n");
   if (s_BootParams.available_outputs & (1 << OUTPUT_VGA))
      printf("  VGA graphics\n");
   if (s_BootParams.available_outputs & (1 << OUTPUT_VGATEXT))
      printf("  VGA text\n");
   if (s_BootParams.available_outputs & (1 << OUTPUT_UART))
      printf("  UART (COM1)\n");
}

void print_boot_drive_number(void)
{
   char *driveType;
   if (s_BootParams.boot_drive == 0xe0)
      driveType = "CD/DVD";
   else if (s_BootParams.boot_drive < 0x80)
      driveType = "Floppy Disk";
   else
      driveType = "Hard Disk";

   printf("Boot drive information:\n");

   printf("  Boot Drive Number: 0x%x.\n", s_BootParams.boot_drive);

   printf("  Booted from a %s.\n", driveType);
}

void print_corefs_memory_address(void)
{
   printf("Corefs Module location: %x.\n", s_BootParams.corefs_ops);
}

static int init_fs(void)
{
   logfmt(LOG_INFO, "Entering filesystem setup.\n");

   int rc = s_BootParams.corefs_ops->Initialize(
       s_BootParams.bios_drive_list, s_BootParams.bios_drive_count,
       s_BootParams.corefs_partition_uuid, s_BootParams.corefs_partition_label);
   if (rc != SUCCESS)
   {
      logfmt(LOG_ERROR, "  FS_Initialize failed: %d.\n", rc);
   }
   else
   {
      logfmt(LOG_INFO, "  FS initialized successful.\n");
   }
   return rc;
}

static int init_main_boot(void)
{
   logfmt(LOG_INFO, "Loading libTheBootloader.\n");

   int fd = s_BootParams.corefs_ops->Open(THEBOOTLOADER_SO_PATH);
   if (fd < 0)
   {
      logfmt(LOG_ERROR, "  Failed to open %s: %d\n", THEBOOTLOADER_SO_PATH, fd);
      return fd;
   }

   uint8_t *stage3_buf = (uint8_t *)STAGE3_BUF_ADDR;
   int total = 0;
   int rc;

   while (total < STAGE3_BUF_SIZE)
   {
      rc = s_BootParams.corefs_ops->Read(fd, stage3_buf + total,
                                         STAGE3_BUF_SIZE - total);
      if (rc <= 0) break;
      total += rc;
   }

   s_BootParams.corefs_ops->Close(fd);

   if (rc < 0 || total == 0)
   {
      logfmt(LOG_ERROR, "  Failed to read %s\n", THEBOOTLOADER_SO_PATH);
      return rc < 0 ? rc : -EIO;
   }

   logfmt(LOG_INFO, "  Read %d bytes from %s\n", total, THEBOOTLOADER_SO_PATH);

   // Populate s_DlCallbackOps
   s_DlCallbackOps.DISK_Read = s_BootParams.corefs_ops->DISK_Read;
   s_DlCallbackOps.DISK_ReadLBA = s_BootParams.corefs_ops->DISK_ReadLBA;
   s_DlCallbackOps.Open = s_BootParams.corefs_ops->Open;
   s_DlCallbackOps.Read = s_BootParams.corefs_ops->Read;
   s_DlCallbackOps.Close = s_BootParams.corefs_ops->Close;
   s_DlCallbackOps.Video_ClearScreen = Video_ClearScreen;
   s_DlCallbackOps.Video_GetHeight = Video_GetHeight;
   s_DlCallbackOps.Video_GetWidth = Video_GetWidth;
   s_DlCallbackOps.Video_PutChar = Video_PutChar;
   s_DlCallbackOps.Video_PutPixel = Video_PutPixel;
   s_DlCallbackOps.logfmt = logfmt;

   // Check for VLSO to patch callback address
   for (int i = 0; i + 4 <= total;
        i++) /* only iterate over bytes that were actually read */
   {
      if (stage3_buf[i] == 'V' && stage3_buf[i + 1] == 'L' &&
          stage3_buf[i + 2] == 'S' && stage3_buf[i + 3] == 'O')
      {
#if defined(BIT32)
         *(uint32_t *)(stage3_buf + i + 4) =
             (uint32_t)(uintptr_t)&s_DlCallbackOps;
#elif defined(BIT64)
         *(uint64_t *)(stage3_buf + i + 4) =
             (uint64_t)(uintptr_t)&s_DlCallbackOps;
#endif
         break;
      }
   }

   void *handle = DL_LoadLibrary(stage3_buf);
   if (!handle)
   {
      logfmt(LOG_ERROR, "  DL_LoadLibrary failed\n");
      return -EINVAL;
   }

   if (dl_resolve_all(handle) != 0)
   {
      logfmt(LOG_ERROR, "  dl_resolve_all failed\n");
      return -EINVAL;
   }

   logfmt(LOG_INFO, "  Stage3 loaded and resolved successfully.\n");
   return SUCCESS;
}

void init_conf(void)
{
   CONF_GlobalBoot *conf = NULL;
   int rc;

   logfmt(LOG_INFO, "Parsing config file: %s\n", THEBOOTLOADER_CONF_PATH);

   rc = g_MainBootOperations.CONF_ParseConf(THEBOOTLOADER_CONF_PATH);
   if (rc != SUCCESS)
   {
      logfmt(LOG_ERROR, "Config parse failed (%d), continuing without it\n",
             rc);
      return;
   }

   g_MainBootOperations.CONF_GetGlobal(&conf);
   if (conf)
   {
      logfmt(LOG_INFO, "Loaded %d boot profile(s), default is '%s'\n",
             conf->profile_count, conf->profiles[conf->default_profile].name);
   }
}

int main(const BootParams *boot_params)
{
   if (!boot_params || !boot_params->corefs_ops ||
       !boot_params->corefs_ops->Initialize || !boot_params->corefs_ops->Open ||
       !boot_params->corefs_ops->Read || !boot_params->corefs_ops->Close)
      return -EINVAL;

   s_BootParams = *boot_params;

   init_framebuffer_info();

   g_PreferredOutput = OUTPUT_UART; /* fallback  */
   if (s_BootParams.available_outputs & (1 << OUTPUT_VGATEXT))
      g_PreferredOutput = OUTPUT_VGATEXT;
   if (s_BootParams.available_outputs & (1 << OUTPUT_VGA))
      g_PreferredOutput = OUTPUT_VGA;
   if ((s_BootParams.available_outputs & (1 << OUTPUT_VBE)) && VBE_HasInfo())
      g_PreferredOutput = OUTPUT_VBE;

   int rc = Video_Initialize();
   if (rc != SUCCESS)
      return rc;

#ifdef DEBUG
   print_available_outputs();
   print_memory_map();
   print_boot_drive_number();
   print_bios_drive_list();
   print_corefs_memory_address();
   print_stage3_fs_location();
#endif

   rc = init_fs();
   if (rc != SUCCESS)
      return rc;

   rc = init_main_boot();
   if (rc != SUCCESS)
      return rc;

   init_conf();

   g_MainBootOperations.LOGO_DrawOnScreen();
   for (;;)
      ;

   return 0;
}
