// SPDX-License-Identifier: GPL-3.0-only

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <logging.h>
#include <status.h>

#include <dl/bindgen.h>
#include <dl/callback.h>

typedef struct FatFile FatFile;
typedef struct FatDrive FatDrive;

#ifdef COREFS
typedef struct FatOperations FatOperations;
#endif

static int fat_read_sector(FatDrive *drive, uint64_t lba, void *buffer);
static int fat_type_from_bpb(const uint8_t *bpb, uint32_t total_clusters);
static int read_bpb(FatDrive *drive);
static uint32_t fat_next_cluster(FatDrive *drive, uint32_t cluster);
static int find_component(FatDrive *drive, uint32_t dir_cluster,
                          const char *component, int comp_len,
                          uint32_t *out_cluster, uint32_t *out_size,
                          uint8_t *out_attrs);
static int find_in_rootdir(FatDrive *drive, const char *component, int comp_len,
                           uint32_t *out_cluster, uint32_t *out_size,
                           uint8_t *out_attrs);
static int check_partition(uint8_t drive, int part_lba,
                           const uint8_t *expected_label,
                           const uint8_t *expected_uuid);

#define SECTOR_SIZE 512
#define MAX_OPEN_FILES 8

// BPB offsets
#define BPB_BYTES_PER_SECTOR_OFF 11
#define BPB_SECTORS_PER_CLUSTER_OFF 13
#define BPB_RESERVED_SECTORS_OFF 14
#define BPB_NUM_FATS_OFF 16
#define BPB_ROOT_ENTRIES_OFF 17
#define BPB_TOTAL_SECTORS16_OFF 19
#define BPB_MEDIA_DESCRIPTOR_OFF 21
#define BPB_SECTORS_PER_FAT16_OFF 22
#define BPB_SECTORS_PER_TRACK_OFF 24
#define BPB_NUM_HEADS_OFF 26
#define BPB_HIDDEN_SECTORS_OFF 28
#define BPB_TOTAL_SECTORS32_OFF 32

// FAT32 specific offsets
#define BPB_FAT32_SECTORS_PER_FAT_OFF 36
#define BPB_FAT32_FLAGS_OFF 40
#define BPB_FAT32_VERSION_OFF 42
#define BPB_FAT32_ROOT_CLUSTER_OFF 44
#define BPB_FAT32_FSINFO_OFF 48
#define BPB_FAT32_BK_BOOT_SEC_OFF 50

// Extended BPB (FAT12/16)
#define BPB_EXT_DRIVE_NUMBER_OFF 36
#define BPB_EXT_BOOT_SIGNATURE_OFF 38
#define BPB_EXT_VOLUME_ID_OFF 39
#define BPB_EXT_VOLUME_LABEL_OFF 43
#define BPB_EXT_FS_TYPE_OFF 54

// FAT32 extended BPB
#define BPB32_EXT_DRIVE_NUMBER_OFF 64
#define BPB32_EXT_BOOT_SIGNATURE_OFF 66
#define BPB32_EXT_VOLUME_ID_OFF 67
#define BPB32_EXT_VOLUME_LABEL_OFF 71
#define BPB32_EXT_FS_TYPE_OFF 82

// Boot sector signature
#define BOOT_SIG_OFFSET 510
#define BOOT_SIGNATURE 0xAA55

// Directory entry offsets
#define DIR_NAME_OFF 0
#define DIR_ATTR_OFF 11
#define DIR_NT_RES_OFF 12
#define DIR_CRT_TIME_TENTH_OFF 13
#define DIR_CRT_TIME_OFF 14
#define DIR_CRT_DATE_OFF 16
#define DIR_LAST_ACCESS_DATE_OFF 18
#define DIR_CLUSTER_HI_OFF 20
#define DIR_WRITE_TIME_OFF 22
#define DIR_WRITE_DATE_OFF 24
#define DIR_CLUSTER_LO_OFF 26
#define DIR_SIZE_OFF 28

// Directory entry attribute masks
#define ATTR_READ_ONLY 0x01
#define ATTR_HIDDEN 0x02
#define ATTR_SYSTEM 0x04
#define ATTR_VOLUME_ID 0x08
#define ATTR_DIRECTORY 0x10
#define ATTR_ARCHIVE 0x20
#define ATTR_LFN 0x0F

// VFAT LFN entry offsets
#define LFN_SEQ_NO_OFF 0
#define LFN_CHAR1_OFF 1
#define LFN_ATTR_OFF 11
#define LFN_TYPE_OFF 12
#define LFN_CHECKSUM_OFF 13
#define LFN_CHAR2_OFF 14
#define LFN_CHAR3_OFF 28
#define LFN_CHAR4_OFF 32

#define LFN_LAST 0x40

// FAT entry special values
#define FAT12_BAD 0xFF7
#define FAT16_BAD 0xFFF7
#define FAT32_BAD 0x0FFFFFF7
#define FAT12_EOC 0xFF8
#define FAT16_EOC 0xFFF8
#define FAT32_EOC 0x0FFFFFF8
#define FAT12_MASK 0xFFF
#define FAT16_MASK 0xFFFF
#define FAT32_MASK 0x0FFFFFFF

#define FAT12_MAX_CLUSTERS 4085
#define FAT16_MAX_CLUSTERS 65525

#define MAX_DRIVES 4
#define LFN_BUF_SIZE 256

struct FatFile
{
   int used;
   uint32_t start_cluster;
   uint32_t current_cluster;
   uint32_t cluster_sector;
   uint32_t byte_offset;
   uint32_t size;
   uint32_t position;
};

struct FatDrive
{
   int used;
   uint8_t boot_drive;
   uint32_t part_start;
   uint16_t bytes_per_sector;
   uint8_t sectors_per_cluster;
   uint16_t reserved_sectors;
   uint8_t num_fats;
   uint16_t root_entries;
   uint32_t total_sectors;
   uint16_t sectors_per_fat16;
   uint32_t sectors_per_fat32;
   uint32_t hidden_sectors;
   uint32_t root_dir_sectors;
   uint32_t first_data_sector;
   uint32_t first_fat_sector;
   uint32_t total_clusters;
   uint32_t root_cluster;
   int fat_type;
   FatFile open_files[MAX_OPEN_FILES];
};

#ifdef COREFS
struct FatOperations
{
   int (*Initialize)(const uint8_t *, uint32_t, const uint8_t *,
                     const uint8_t *);
   int (*Open)(const char *);
   int (*Read)(int, void *, int);
   int (*Close)(int);
   int (*DISK_Read)(uint8_t, uint16_t, uint8_t, uint8_t, uint8_t, void *);
   int (*DISK_ReadLBA)(uint8_t, uint64_t, uint16_t, void *);
};
#endif

static FatDrive s_Drives[MAX_DRIVES] = {0};

#ifdef COREFS
static int s_CoreFsDriveId = -1;
#endif

#ifdef COREFS
extern int DISK_Read(uint8_t drive, uint16_t cylinder, uint8_t sector,
                     uint8_t head, uint8_t count, void *buffer);
extern int DISK_ReadLBA(uint8_t drive, uint64_t lba, uint16_t count,
                        void *buffer);
#define logfmt(...) ((void)0)
#else
#define DISK_Read g_DlCallbackOps->DISK_Read
#define DISK_ReadLBA g_DlCallbackOps->DISK_ReadLBA
#define logfmt g_DlCallbackOps->logfmt
#endif

extern bool MBR_Probe(int drive_id);
extern int MBR_List(int drive_id, int **offset);
extern bool GPT_Probe(int drive_id);
extern int GPT_List(int drive_id, int **offset);

static int fat_read_sector(FatDrive *drive, uint64_t lba, void *buffer)
{
   uint64_t abs_lba = (uint64_t)drive->part_start + lba;
   return DISK_ReadLBA(drive->boot_drive, abs_lba, 1, buffer);
}

static int fat_type_from_bpb(const uint8_t *bpb, uint32_t total_clusters)
{
   (void)bpb;
   if (total_clusters < FAT12_MAX_CLUSTERS)
      return 12;
   else if (total_clusters < FAT16_MAX_CLUSTERS)
      return 16;
   else
      return 32;
}

static int read_bpb(FatDrive *drive)
{
   uint8_t sector[SECTOR_SIZE];

   if (fat_read_sector(drive, 0, sector) != 0)
   {
      logfmt(LOG_ERROR, "fat: read_bpb: I/O error reading boot sector\n");
      return -EIO;
   }

   uint16_t sig = (uint16_t)(sector[BOOT_SIG_OFFSET] |
                             ((uint16_t)sector[BOOT_SIG_OFFSET + 1] << 8));
   if (sig != BOOT_SIGNATURE)
   {
      logfmt(LOG_ERROR, "fat: read_bpb: invalid boot signature 0x%04x\n", sig);
      return -EINVAL;
   }

   drive->bytes_per_sector =
       (uint16_t)sector[BPB_BYTES_PER_SECTOR_OFF] |
       ((uint16_t)sector[BPB_BYTES_PER_SECTOR_OFF + 1] << 8);
   if (drive->bytes_per_sector == 0) drive->bytes_per_sector = 512;
   if (drive->bytes_per_sector != 512)
   {
      logfmt(LOG_ERROR, "fat: read_bpb: unsupported bytes per sector %u\n",
             drive->bytes_per_sector);
      return -EINVAL;
   }

   drive->sectors_per_cluster = sector[BPB_SECTORS_PER_CLUSTER_OFF];
   if (drive->sectors_per_cluster == 0)
   {
      logfmt(LOG_ERROR, "fat: read_bpb: sectors per cluster is 0\n");
      return -EINVAL;
   }

   drive->reserved_sectors =
       (uint16_t)sector[BPB_RESERVED_SECTORS_OFF] |
       ((uint16_t)sector[BPB_RESERVED_SECTORS_OFF + 1] << 8);
   drive->num_fats = sector[BPB_NUM_FATS_OFF];
   if (drive->num_fats == 0)
   {
      logfmt(LOG_ERROR, "fat: read_bpb: number of FATs is 0\n");
      return -EINVAL;
   }

   drive->root_entries = (uint16_t)sector[BPB_ROOT_ENTRIES_OFF] |
                         ((uint16_t)sector[BPB_ROOT_ENTRIES_OFF + 1] << 8);
   drive->total_sectors = (uint16_t)sector[BPB_TOTAL_SECTORS16_OFF] |
                          ((uint16_t)sector[BPB_TOTAL_SECTORS16_OFF + 1] << 8);
   if (drive->total_sectors == 0)
   {
      drive->total_sectors =
          (uint32_t)sector[BPB_TOTAL_SECTORS32_OFF] |
          ((uint32_t)sector[BPB_TOTAL_SECTORS32_OFF + 1] << 8) |
          ((uint32_t)sector[BPB_TOTAL_SECTORS32_OFF + 2] << 16) |
          ((uint32_t)sector[BPB_TOTAL_SECTORS32_OFF + 3] << 24);
   }

   drive->sectors_per_fat16 =
       (uint16_t)sector[BPB_SECTORS_PER_FAT16_OFF] |
       ((uint16_t)sector[BPB_SECTORS_PER_FAT16_OFF + 1] << 8);
   drive->hidden_sectors =
       (uint32_t)sector[BPB_HIDDEN_SECTORS_OFF] |
       ((uint32_t)sector[BPB_HIDDEN_SECTORS_OFF + 1] << 8) |
       ((uint32_t)sector[BPB_HIDDEN_SECTORS_OFF + 2] << 16) |
       ((uint32_t)sector[BPB_HIDDEN_SECTORS_OFF + 3] << 24);

   drive->sectors_per_fat32 = 0;
   drive->root_cluster = 0;

   drive->root_dir_sectors =
       ((uint32_t)drive->root_entries * 32 + drive->bytes_per_sector - 1) /
       drive->bytes_per_sector;

   if (drive->sectors_per_fat16 == 0)
   {
      drive->sectors_per_fat32 =
          (uint32_t)sector[BPB_FAT32_SECTORS_PER_FAT_OFF] |
          ((uint32_t)sector[BPB_FAT32_SECTORS_PER_FAT_OFF + 1] << 8) |
          ((uint32_t)sector[BPB_FAT32_SECTORS_PER_FAT_OFF + 2] << 16) |
          ((uint32_t)sector[BPB_FAT32_SECTORS_PER_FAT_OFF + 3] << 24);
      drive->root_cluster =
          (uint32_t)sector[BPB_FAT32_ROOT_CLUSTER_OFF] |
          ((uint32_t)sector[BPB_FAT32_ROOT_CLUSTER_OFF + 1] << 8) |
          ((uint32_t)sector[BPB_FAT32_ROOT_CLUSTER_OFF + 2] << 16) |
          ((uint32_t)sector[BPB_FAT32_ROOT_CLUSTER_OFF + 3] << 24);
   }

   drive->first_fat_sector = drive->reserved_sectors;

   uint32_t fat_total_sectors;
   if (drive->sectors_per_fat16 != 0)
      fat_total_sectors = (uint32_t)drive->num_fats * drive->sectors_per_fat16;
   else
      fat_total_sectors = (uint32_t)drive->num_fats * drive->sectors_per_fat32;

   drive->first_data_sector =
       drive->reserved_sectors + fat_total_sectors + drive->root_dir_sectors;

   if (drive->sectors_per_cluster == 0)
   {
      logfmt(LOG_ERROR,
             "fat: read_bpb: sectors per cluster is 0 (late check)\n");
      return -EINVAL;
   }

   uint32_t data_sectors = drive->total_sectors - drive->first_data_sector;
   drive->total_clusters = data_sectors / (uint32_t)drive->sectors_per_cluster;
   drive->fat_type =
       fat_type_from_bpb((const uint8_t *)sector, drive->total_clusters);

   return SUCCESS;
}

static uint32_t fat_next_cluster(FatDrive *drive, uint32_t cluster)
{
   uint8_t fat_sector[SECTOR_SIZE];
   uint32_t fat_offset;
   uint32_t fat_sector_num;
   uint32_t fat_entry_offset;

   if (drive->fat_type == 32)
   {
      fat_offset = cluster * 4;
      fat_sector_num = fat_offset / drive->bytes_per_sector;
      fat_entry_offset = fat_offset % drive->bytes_per_sector;

      if (fat_read_sector(drive, drive->first_fat_sector + fat_sector_num,
                          fat_sector) != 0)
         return FAT32_EOC;

      uint32_t entry = (uint32_t)fat_sector[fat_entry_offset] |
                       ((uint32_t)fat_sector[fat_entry_offset + 1] << 8) |
                       ((uint32_t)fat_sector[fat_entry_offset + 2] << 16) |
                       ((uint32_t)fat_sector[fat_entry_offset + 3] << 24);
      entry &= FAT32_MASK;

      if (entry >= (FAT32_BAD & FAT32_MASK)) return FAT32_EOC;
      return entry;
   }
   else if (drive->fat_type == 16)
   {
      fat_offset = cluster * 2;
      fat_sector_num = fat_offset / drive->bytes_per_sector;
      fat_entry_offset = fat_offset % drive->bytes_per_sector;

      if (fat_read_sector(drive, drive->first_fat_sector + fat_sector_num,
                          fat_sector) != 0)
         return FAT16_EOC;

      uint16_t entry = (uint16_t)fat_sector[fat_entry_offset] |
                       ((uint16_t)fat_sector[fat_entry_offset + 1] << 8);

      if (entry >= FAT16_BAD) return FAT16_EOC;
      return (uint32_t)entry;
   }
   else
   {
      fat_offset = cluster * 3 / 2;
      fat_sector_num = fat_offset / drive->bytes_per_sector;
      fat_entry_offset = fat_offset % drive->bytes_per_sector;

      if (fat_read_sector(drive, drive->first_fat_sector + fat_sector_num,
                          fat_sector) != 0)
         return FAT12_EOC;

      uint16_t entry;
      if (cluster & 1)
      {
         entry = (uint16_t)((fat_sector[fat_entry_offset] >> 4) |
                            ((uint16_t)fat_sector[fat_entry_offset + 1] << 4));
      }
      else
      {
         entry = (uint16_t)(fat_sector[fat_entry_offset] |
                            ((uint16_t)fat_sector[fat_entry_offset + 1] << 8));
      }
      entry &= FAT12_MASK;

      if (entry >= FAT12_BAD) return FAT12_EOC;
      return (uint32_t)entry;
   }
}

static int sfn_to_name(const uint8_t *entry, char *name, int name_size)
{
   int len = 0;
   int i;

   for (i = 0; i < 8 && entry[DIR_NAME_OFF + i] != ' '; i++)
   {
      if (len < name_size - 1) name[len++] = (char)entry[DIR_NAME_OFF + i];
   }

   int ext_start = len;
   for (i = 0; i < 3 && entry[DIR_NAME_OFF + 8 + i] != ' '; i++)
   {
      if (len < name_size - 1) name[len++] = (char)entry[DIR_NAME_OFF + 8 + i];
   }
   name[len] = '\0';

   uint8_t nt_res = entry[DIR_NT_RES_OFF];
   if (nt_res & 0x08)
   {
      for (i = ext_start; i < len; i++)
      {
         if (name[i] >= 'A' && name[i] <= 'Z') name[i] += 0x20;
      }
   }
   if (nt_res & 0x10)
   {
      for (i = 0; i < ext_start; i++)
      {
         if (name[i] >= 'A' && name[i] <= 'Z') name[i] += 0x20;
      }
   }

   return len;
}

static int name_matches(const uint8_t *entry, const char *lfn_name,
                        const char *component, int comp_len)
{
   if (lfn_name && lfn_name[0] != '\0')
   {
      int i;
      for (i = 0; i < comp_len; i++)
      {
         if (lfn_name[i] == '\0') return 0;
         char c1 = component[i];
         char c2 = lfn_name[i];
         if (c1 >= 'A' && c1 <= 'Z') c1 += 0x20;
         if (c2 >= 'A' && c2 <= 'Z') c2 += 0x20;
         if (c1 != c2) return 0;
      }
      return (lfn_name[i] == '\0');
   }

   char sfn_name[13];
   sfn_to_name(entry, sfn_name, sizeof(sfn_name));

   int i;
   for (i = 0; i < comp_len; i++)
   {
      if (sfn_name[i] == '\0') return 0;
      char c1 = component[i];
      char c2 = sfn_name[i];
      if (c1 >= 'A' && c1 <= 'Z') c1 += 0x20;
      if (c2 >= 'A' && c2 <= 'Z') c2 += 0x20;
      if (c1 != c2) return 0;
   }
   return (sfn_name[i] == '\0');
}

static int find_component(FatDrive *drive, uint32_t dir_cluster,
                          const char *component, int comp_len,
                          uint32_t *out_cluster, uint32_t *out_size,
                          uint8_t *out_attrs)
{
   uint32_t current_cluster = dir_cluster;

   while (current_cluster >= 2 && current_cluster < FAT12_EOC)
   {
      uint32_t cluster_size =
          (uint32_t)drive->sectors_per_cluster * drive->bytes_per_sector;
      uint8_t sector_buf[SECTOR_SIZE];
      char lfn_buf[LFN_BUF_SIZE];
      int lfn_pending = 0;
      int lfn_entries[20];
      int lfn_count = 0;
      uint32_t pos = 0;

      while (pos + 32 <= cluster_size)
      {
         uint32_t sector_idx = pos / SECTOR_SIZE;
         uint32_t offset_in_sector = pos % SECTOR_SIZE;

         if (offset_in_sector == 0)
         {
            uint32_t lba =
                drive->first_data_sector +
                (current_cluster - 2) * (uint32_t)drive->sectors_per_cluster +
                sector_idx;
            if (fat_read_sector(drive, lba, sector_buf) != 0)
            {
               logfmt(
                   LOG_ERROR,
                   "fat: find_component: I/O error reading directory sector\n");
               return -EIO;
            }
         }

         const uint8_t *entry = sector_buf + offset_in_sector;
         uint8_t name0 = entry[DIR_NAME_OFF];
         uint8_t attr = entry[DIR_ATTR_OFF];

         if (name0 == 0x00) break;

         if (name0 == 0xE5)
         {
            lfn_pending = 0;
            lfn_count = 0;
            pos += 32;
            continue;
         }

         if (attr == ATTR_LFN)
         {
            if (lfn_count < 20) lfn_entries[lfn_count++] = (int)pos;
            lfn_pending = 1;
            pos += 32;
            continue;
         }

         if (attr & ATTR_VOLUME_ID)
         {
            lfn_pending = 0;
            lfn_count = 0;
            pos += 32;
            continue;
         }

         // Build LFN name from pending entries
         if (lfn_pending && lfn_count > 0)
         {
            int lfn_idx = 0;
            for (int li = lfn_count - 1; li >= 0; li--)
            {
               int lfn_pos = lfn_entries[li];
               uint32_t ls_idx = (uint32_t)lfn_pos / SECTOR_SIZE;
               uint32_t ls_off = (uint32_t)lfn_pos % SECTOR_SIZE;
               uint8_t ls_buf[SECTOR_SIZE];

               uint32_t llba = drive->first_data_sector +
                               (current_cluster - 2) *
                                   (uint32_t)drive->sectors_per_cluster +
                               ls_idx;
               if (fat_read_sector(drive, llba, ls_buf) != 0)
               {
                  logfmt(LOG_ERROR,
                         "fat: find_component: I/O error reading LFN sector\n");
                  return -EIO;
               }

               const uint8_t *le = ls_buf + ls_off;
               for (int ci = 0; ci < 5 && lfn_idx < LFN_BUF_SIZE - 1; ci++)
               {
                  uint16_t ch = (uint16_t)le[LFN_CHAR1_OFF + ci * 2] |
                                ((uint16_t)le[LFN_CHAR1_OFF + ci * 2 + 1] << 8);
                  if (ch == 0x0000 || ch == 0xFFFF) break;
                  lfn_buf[lfn_idx++] = (ch < 0x80) ? (char)ch : '?';
               }
               for (int ci = 0; ci < 6 && lfn_idx < LFN_BUF_SIZE - 1; ci++)
               {
                  uint16_t ch = (uint16_t)le[LFN_CHAR2_OFF + ci * 2] |
                                ((uint16_t)le[LFN_CHAR2_OFF + ci * 2 + 1] << 8);
                  if (ch == 0x0000 || ch == 0xFFFF) break;
                  lfn_buf[lfn_idx++] = (ch < 0x80) ? (char)ch : '?';
               }
               for (int ci = 0; ci < 2 && lfn_idx < LFN_BUF_SIZE - 1; ci++)
               {
                  uint16_t ch = (uint16_t)le[LFN_CHAR3_OFF + ci * 2] |
                                ((uint16_t)le[LFN_CHAR3_OFF + ci * 2 + 1] << 8);
                  if (ch == 0x0000 || ch == 0xFFFF) break;
                  lfn_buf[lfn_idx++] = (ch < 0x80) ? (char)ch : '?';
               }
            }
            lfn_buf[lfn_idx] = '\0';
         }
         else
         {
            lfn_buf[0] = '\0';
         }

         lfn_pending = 0;
         lfn_count = 0;

         if (name_matches(entry, lfn_buf, component, comp_len))
         {
            uint32_t cluster_lo =
                (uint32_t)entry[DIR_CLUSTER_LO_OFF] |
                ((uint32_t)entry[DIR_CLUSTER_LO_OFF + 1] << 8);
            uint32_t cluster_hi =
                (uint32_t)entry[DIR_CLUSTER_HI_OFF] |
                ((uint32_t)entry[DIR_CLUSTER_HI_OFF + 1] << 8);
            *out_cluster = (cluster_hi << 16) | cluster_lo;
            *out_size = (uint32_t)entry[DIR_SIZE_OFF] |
                        ((uint32_t)entry[DIR_SIZE_OFF + 1] << 8) |
                        ((uint32_t)entry[DIR_SIZE_OFF + 2] << 16) |
                        ((uint32_t)entry[DIR_SIZE_OFF + 3] << 24);
            *out_attrs = entry[DIR_ATTR_OFF];
            return 0;
         }

         pos += 32;
      }

      current_cluster = fat_next_cluster(drive, current_cluster);
   }

   logfmt(LOG_ERROR, "fat: find_component: entry not found\n");
   return -ENOENT;
}

static int find_in_rootdir(FatDrive *drive, const char *component, int comp_len,
                           uint32_t *out_cluster, uint32_t *out_size,
                           uint8_t *out_attrs)
{
   uint8_t sector[SECTOR_SIZE];
   char lfn_buf[LFN_BUF_SIZE];
   int lfn_pending = 0;
   int lfn_entries[20];
   int lfn_count = 0;
   uint32_t root_start_lba = drive->first_data_sector - drive->root_dir_sectors;

   for (uint32_t sec = 0; sec < drive->root_dir_sectors; sec++)
   {
      if (fat_read_sector(drive, root_start_lba + sec, sector) != 0)
      {
         logfmt(
             LOG_ERROR,
             "fat: find_in_rootdir: I/O error reading root directory sector\n");
         return -EIO;
      }

      for (int off = 0; off < (int)drive->bytes_per_sector; off += 32)
      {
         const uint8_t *entry = sector + off;
         uint8_t name0 = entry[DIR_NAME_OFF];
         uint8_t attr = entry[DIR_ATTR_OFF];

         if (name0 == 0x00)
         {
            logfmt(LOG_ERROR, "fat: find_in_rootdir: entry not found\n");
            return -ENOENT;
         }

         if (name0 == 0xE5)
         {
            lfn_pending = 0;
            lfn_count = 0;
            continue;
         }

         if (attr == ATTR_LFN)
         {
            if (lfn_count < 20)
               lfn_entries[lfn_count++] =
                   (int)(sec * drive->bytes_per_sector + off);
            lfn_pending = 1;
            continue;
         }

         if (lfn_pending && lfn_count > 0)
         {
            int lfn_idx = 0;
            for (int li = lfn_count - 1; li >= 0; li--)
            {
               int lfn_abs_off = lfn_entries[li];
               uint32_t lfn_sec =
                   (uint32_t)lfn_abs_off / drive->bytes_per_sector;
               uint32_t lfn_off =
                   (uint32_t)lfn_abs_off % drive->bytes_per_sector;
               uint8_t lfn_sec_buf[SECTOR_SIZE];

               if (fat_read_sector(drive, root_start_lba + lfn_sec,
                                   lfn_sec_buf) != 0)
               {
                  logfmt(
                      LOG_ERROR,
                      "fat: find_in_rootdir: I/O error reading LFN sector\n");
                  return -EIO;
               }

               const uint8_t *le = lfn_sec_buf + lfn_off;
               for (int ci = 0; ci < 5 && lfn_idx < LFN_BUF_SIZE - 1; ci++)
               {
                  uint16_t ch = (uint16_t)le[LFN_CHAR1_OFF + ci * 2] |
                                ((uint16_t)le[LFN_CHAR1_OFF + ci * 2 + 1] << 8);
                  if (ch == 0x0000 || ch == 0xFFFF) break;
                  lfn_buf[lfn_idx++] = (ch < 0x80) ? (char)ch : '?';
               }
               for (int ci = 0; ci < 6 && lfn_idx < LFN_BUF_SIZE - 1; ci++)
               {
                  uint16_t ch = (uint16_t)le[LFN_CHAR2_OFF + ci * 2] |
                                ((uint16_t)le[LFN_CHAR2_OFF + ci * 2 + 1] << 8);
                  if (ch == 0x0000 || ch == 0xFFFF) break;
                  lfn_buf[lfn_idx++] = (ch < 0x80) ? (char)ch : '?';
               }
               for (int ci = 0; ci < 2 && lfn_idx < LFN_BUF_SIZE - 1; ci++)
               {
                  uint16_t ch = (uint16_t)le[LFN_CHAR3_OFF + ci * 2] |
                                ((uint16_t)le[LFN_CHAR3_OFF + ci * 2 + 1] << 8);
                  if (ch == 0x0000 || ch == 0xFFFF) break;
                  lfn_buf[lfn_idx++] = (ch < 0x80) ? (char)ch : '?';
               }
            }
            lfn_buf[lfn_idx] = '\0';
         }
         else
         {
            lfn_buf[0] = '\0';
         }

         lfn_pending = 0;
         lfn_count = 0;

         if (attr & ATTR_VOLUME_ID) continue;

         if (name_matches(entry, lfn_buf, component, comp_len))
         {
            uint32_t cluster_lo =
                (uint32_t)entry[DIR_CLUSTER_LO_OFF] |
                ((uint32_t)entry[DIR_CLUSTER_LO_OFF + 1] << 8);
            uint32_t cluster_hi =
                (uint32_t)entry[DIR_CLUSTER_HI_OFF] |
                ((uint32_t)entry[DIR_CLUSTER_HI_OFF + 1] << 8);
            *out_cluster = (cluster_hi << 16) | cluster_lo;
            *out_size = (uint32_t)entry[DIR_SIZE_OFF] |
                        ((uint32_t)entry[DIR_SIZE_OFF + 1] << 8) |
                        ((uint32_t)entry[DIR_SIZE_OFF + 2] << 16) |
                        ((uint32_t)entry[DIR_SIZE_OFF + 3] << 24);
            *out_attrs = entry[DIR_ATTR_OFF];
            return 0;
         }
      }
   }

   logfmt(LOG_ERROR, "fat: find_in_rootdir: entry not found\n");
   return -ENOENT;
}

static int check_partition(uint8_t bios_drive, int part_lba,
                           const uint8_t *expected_label,
                           const uint8_t *expected_uuid)
{
   uint8_t sector[SECTOR_SIZE];
   int identity_requested = 0;
   uint64_t abs_lba = (uint64_t)(uint32_t)part_lba;

   if (DISK_ReadLBA(bios_drive, abs_lba, 1, sector) != 0) return 0;

   uint16_t sig = (uint16_t)(sector[BOOT_SIG_OFFSET] |
                             ((uint16_t)sector[BOOT_SIG_OFFSET + 1] << 8));
   if (sig != BOOT_SIGNATURE) return 0;

   uint8_t media = sector[BPB_MEDIA_DESCRIPTOR_OFF];
   if (media != 0xF0 && media < 0xF8) return 0;

   uint16_t bps = (uint16_t)sector[BPB_BYTES_PER_SECTOR_OFF] |
                  ((uint16_t)sector[BPB_BYTES_PER_SECTOR_OFF + 1] << 8);
   if (bps != 512 && bps != 0) return 0;

   if (expected_label)
   {
      int label_nonzero = 0;
      for (int i = 0; i < 11; i++)
      {
         if (expected_label[i] != 0 && expected_label[i] != ' ')
         {
            label_nonzero = 1;
            break;
         }
      }

      if (label_nonzero)
      {
         identity_requested = 1;
         const uint8_t *label_ptr;
         if (sector[BPB_SECTORS_PER_FAT16_OFF] == 0 &&
             sector[BPB_SECTORS_PER_FAT16_OFF + 1] == 0)
            label_ptr = &sector[BPB32_EXT_VOLUME_LABEL_OFF];
         else
            label_ptr = &sector[BPB_EXT_VOLUME_LABEL_OFF];

         int match = 1;
         for (int i = 0; i < 11; i++)
         {
            char c1 = (char)expected_label[i];
            char c2 = (char)label_ptr[i];

            int end1 = (c1 == '\0' || c1 == ' ');
            if (end1) {
               if (!(c2 == '\0' || c2 == ' ')) match = 0;
               break;
            }

            if (c1 >= 'a' && c1 <= 'z') c1 -= 32;
            if (c2 >= 'a' && c2 <= 'z') c2 -= 32;

            if (c1 != c2)
            {
               match = 0;
               break;
            }
         }
         if (match) return 1;
      }
   }

   if (expected_uuid)
   {
      int uuid_nonzero = 0;
      for (int i = 0; i < 4; i++)
      {
         if (expected_uuid[i] != 0)
         {
            uuid_nonzero = 1;
            break;
         }
      }

      if (uuid_nonzero)
      {
         identity_requested = 1;
         const uint8_t *vol_id_ptr;
         if (sector[BPB_SECTORS_PER_FAT16_OFF] == 0 &&
             sector[BPB_SECTORS_PER_FAT16_OFF + 1] == 0)
            vol_id_ptr = &sector[BPB32_EXT_VOLUME_ID_OFF];
         else
            vol_id_ptr = &sector[BPB_EXT_VOLUME_ID_OFF];

         int match = 1;
         for (int i = 0; i < 4; i++)
         {
            if (expected_uuid[i] != vol_id_ptr[i])
            {
               match = 0;
               break;
            }
         }
         if (match) return 1;
      }
   }

   return !identity_requested;
}

DL_INC
int FAT_Initialize(const uint8_t *bios_drive_list,
                   uint32_t bios_drive_list_count,
                   const uint8_t *partition_uuid,
                   const uint8_t *partition_label)
{
   int drive_id;
   FatDrive *drive;

   if (!bios_drive_list || bios_drive_list_count == 0)
   {
      logfmt(LOG_ERROR, "fat: Initialize: invalid drive list\n");
      return -EINVAL;
   }

   /* Find a free drive slot. */
   for (drive_id = 0; drive_id < MAX_DRIVES; drive_id++)
   {
      if (!s_Drives[drive_id].used) break;
   }
   if (drive_id == MAX_DRIVES)
   {
      logfmt(LOG_ERROR, "fat: Initialize: too many open drives\n");
      return -EMFILE;
   }

   drive = &s_Drives[drive_id];

   {
      int found = 0;
      for (uint32_t i = 0; i < bios_drive_list_count && !found; i++)
      {
         uint8_t bios_drive = bios_drive_list[i];
         int *offsets = NULL;
         int count = -1;

         if (GPT_Probe(bios_drive))
            count = GPT_List(bios_drive, &offsets);
         else if (MBR_Probe(bios_drive))
            count = MBR_List(bios_drive, &offsets);

         if (count <= 0)
         {
            if (check_partition(bios_drive, 0, partition_label, partition_uuid))
            {
               drive->boot_drive = bios_drive;
               drive->part_start = 0;
               if (read_bpb(drive) == SUCCESS) found = 1;
            }
            continue;
         }

         for (int j = 0; j < count && !found; j++)
         {
            if (check_partition(bios_drive, offsets[j], partition_label,
                                partition_uuid))
            {
               drive->boot_drive = bios_drive;
               drive->part_start = (uint32_t)offsets[j];
               if (read_bpb(drive) == SUCCESS) found = 1;
            }
         }
      }
      if (!found)
      {
         logfmt(LOG_ERROR, "fat: Initialize: no valid FAT partition found\n");
         return -ENODEV;
      }
   }

   drive->used = 1;
   return drive_id;
}

DL_INC
int FAT_Open(int drive_id, const char *path)
{
   FatDrive *drive;

   if (drive_id < 0 || drive_id >= MAX_DRIVES || !s_Drives[drive_id].used)
   {
      logfmt(LOG_ERROR, "fat: Open: invalid drive id %d\n", drive_id);
      return -EINVAL;
   }

   drive = &s_Drives[drive_id];

   if (!path || *path == '\0')
   {
      logfmt(LOG_ERROR, "fat: Open: empty path\n");
      return -EINVAL;
   }

   if (*path == '/') path++;

   if (*path == '\0')
   {
      logfmt(LOG_ERROR, "fat: Open: path is root directory\n");
      return -EINVAL;
   }

   uint32_t current_cluster = 0;
   uint8_t current_attrs = ATTR_DIRECTORY;

   if (drive->fat_type == 32) current_cluster = drive->root_cluster;

   uint32_t file_cluster = 0;
   uint32_t file_size = 0;
   uint8_t file_attrs = 0;

   while (*path != '\0')
   {
      const char *start = path;
      while (*path != '/' && *path != '\0')
         path++;
      int comp_len = (int)(path - start);

      if (comp_len == 0)
      {
         while (*path == '/')
            path++;
         continue;
      }

      int rc;
      if (drive->fat_type == 32 || current_cluster != 0)
      {
         rc = find_component(drive, current_cluster, start, comp_len,
                             &file_cluster, &file_size, &file_attrs);
      }
      else
      {
         rc = find_in_rootdir(drive, start, comp_len, &file_cluster, &file_size,
                              &file_attrs);
      }

      if (rc != 0) return rc;

      current_cluster = file_cluster;
      current_attrs = file_attrs;

      while (*path == '/')
         path++;
   }

   if (current_attrs & ATTR_DIRECTORY)
   {
      logfmt(LOG_ERROR, "fat: Open: path is a directory\n");
      return -EINVAL;
   }

   int fd;
   for (fd = 0; fd < MAX_OPEN_FILES; fd++)
   {
      if (!drive->open_files[fd].used) break;
   }
   if (fd == MAX_OPEN_FILES)
   {
      logfmt(LOG_ERROR, "fat: Open: too many open files\n");
      return -EMFILE;
   }

   drive->open_files[fd].used = 1;
   drive->open_files[fd].start_cluster = file_cluster;
   drive->open_files[fd].current_cluster = file_cluster;
   drive->open_files[fd].cluster_sector = 0;
   drive->open_files[fd].byte_offset = 0;
   drive->open_files[fd].size = file_size;
   drive->open_files[fd].position = 0;

   return fd;
}

DL_INC
int FAT_Read(int drive_id, int fd, void *buffer, int count)
{
   FatDrive *drive;

   if (drive_id < 0 || drive_id >= MAX_DRIVES || !s_Drives[drive_id].used)
   {
      logfmt(LOG_ERROR, "fat: Read: invalid drive id %d\n", drive_id);
      return -EINVAL;
   }

   drive = &s_Drives[drive_id];

   if (fd < 0 || fd >= MAX_OPEN_FILES || !drive->open_files[fd].used)
   {
      logfmt(LOG_ERROR, "fat: Read: invalid file descriptor %d\n", fd);
      return -EBADF;
   }

   FatFile *f = &drive->open_files[fd];
   uint8_t *buf = (uint8_t *)buffer;

   if (f->position >= f->size) return 0;
   if (count <= 0) return 0;

   uint32_t remaining = f->size - f->position;
   if ((uint32_t)count > remaining) count = (int)remaining;

   uint32_t cluster_size =
       (uint32_t)drive->sectors_per_cluster * drive->bytes_per_sector;
   uint32_t bytes_done = 0;

   while (bytes_done < (uint32_t)count)
   {
      uint32_t file_offset = f->position + bytes_done;
      uint32_t offset_in_cluster = file_offset % cluster_size;
      uint32_t sector_idx = offset_in_cluster / drive->bytes_per_sector;
      uint32_t offset_in_sector = offset_in_cluster % drive->bytes_per_sector;

      // Determine correct cluster
      uint32_t target_cluster = f->current_cluster;
      if (f->cluster_sector > sector_idx || (f->cluster_sector == sector_idx &&
                                             f->byte_offset > offset_in_sector))
      {
         uint32_t clusters_to_skip = file_offset / cluster_size;
         target_cluster = f->start_cluster;
         for (uint32_t c = 0; c < clusters_to_skip; c++)
         {
            target_cluster = fat_next_cluster(drive, target_cluster);
            if (target_cluster >= FAT12_EOC)
            {
               if (bytes_done > 0) return (int)bytes_done;
               logfmt(LOG_ERROR, "fat: Read: invalid cluster chain (EOC)\n");
               return -EIO;
            }
         }
         f->current_cluster = target_cluster;
         f->cluster_sector = 0;
         f->byte_offset = 0;
      }

      target_cluster = f->current_cluster;

      uint32_t lba =
          drive->first_data_sector +
          (target_cluster - 2) * (uint32_t)drive->sectors_per_cluster +
          sector_idx;

      uint8_t sector_buf[SECTOR_SIZE];
      if (fat_read_sector(drive, lba, sector_buf) != 0)
      {
         if (bytes_done > 0) return (int)bytes_done;
         logfmt(LOG_ERROR, "fat: Read: I/O error reading data sector\n");
         return -EIO;
      }

      uint32_t chunk = drive->bytes_per_sector - offset_in_sector;
      if (chunk > (uint32_t)count - bytes_done)
         chunk = (uint32_t)count - bytes_done;

      for (uint32_t i = 0; i < chunk; i++)
         buf[bytes_done + i] = sector_buf[offset_in_sector + i];

      bytes_done += chunk;
      f->byte_offset = offset_in_sector + chunk;

      if (f->byte_offset >= drive->bytes_per_sector)
      {
         f->byte_offset = 0;
         f->cluster_sector = sector_idx + 1;

         if (f->cluster_sector >= (uint32_t)drive->sectors_per_cluster)
         {
            f->cluster_sector = 0;
            f->current_cluster = fat_next_cluster(drive, target_cluster);
         }
      }
   }

   f->position += bytes_done;
   return (int)bytes_done;
}

DL_INC
int FAT_Close(int drive_id, int fd)
{
   FatDrive *drive;

   if (drive_id < 0 || drive_id >= MAX_DRIVES || !s_Drives[drive_id].used)
   {
      logfmt(LOG_ERROR, "fat: Close: invalid drive id %d\n", drive_id);
      return -EINVAL;
   }

   drive = &s_Drives[drive_id];

   if (fd < 0 || fd >= MAX_OPEN_FILES || !drive->open_files[fd].used)
   {
      logfmt(LOG_ERROR, "fat: Close: invalid file descriptor %d\n", fd);
      return -EBADF;
   }

   drive->open_files[fd].used = 0;
   return SUCCESS;
}

#ifdef COREFS

static int corefs_Initialize(const uint8_t *bios_drive_list,
                             uint32_t bios_drive_list_count,
                             const uint8_t *partition_uuid,
                             const uint8_t *partition_label)
{
   int id = FAT_Initialize(bios_drive_list, bios_drive_list_count,
                           partition_uuid, partition_label);
   if (id >= 0) s_CoreFsDriveId = id;
   return id;
}

static int corefs_Open(const char *path)
{
   return FAT_Open(s_CoreFsDriveId, path);
}

static int corefs_Read(int fd, void *buffer, int count)
{
   return FAT_Read(s_CoreFsDriveId, fd, buffer, count);
}

static int corefs_Close(int fd) { return FAT_Close(s_CoreFsDriveId, fd); }

static const FatOperations fs_exports
    __attribute__((section(".exports"), used)) = {
        .Initialize = corefs_Initialize,
        .Open = corefs_Open,
        .Read = corefs_Read,
        .Close = corefs_Close,
        .DISK_Read = DISK_Read,
        .DISK_ReadLBA = DISK_ReadLBA,
};

#endif /* COREFS */
