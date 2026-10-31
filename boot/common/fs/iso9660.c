// SPDX-License-Identifier: GPL-3.0-only
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <logging.h>
#include <status.h>

#include <dl/bindgen.h>
#include <dl/callback.h>

typedef struct Iso9660File Iso9660File;
typedef struct Iso9660Drive Iso9660Drive;

#ifdef COREFS
typedef struct Iso9660Operations Iso9660Operations;
#endif

static int iso_read_sector(Iso9660Drive *drive, uint64_t iso_lba, void *buffer);
static int parse_dir_record(const uint8_t *buf, int off, uint32_t *extent_lba,
                            uint32_t *extent_size, uint8_t *flags,
                            uint8_t *name_len);
static int name_match(const char *component, int comp_len,
                      const uint8_t *rec_name, int rec_name_len);
static int label_nonzero(const uint8_t *label);
static int label_match(const uint8_t *iso_label, const uint8_t *expected);
static int check_partition(uint8_t drive, int part_lba,
                           const uint8_t *expected_label,
                           const uint8_t *expected_uuid);
static int lookup_component(Iso9660Drive *drive, uint64_t dir_lba,
                            uint32_t dir_size, const char *component,
                            int comp_len, uint32_t *out_lba, uint32_t *out_size,
                            uint8_t *out_flags);
static int resolve_path(Iso9660Drive *drive, const char *path,
                        uint32_t *out_lba, uint32_t *out_size);

#define SECTOR_SIZE_ISO 2048
#define SECTOR_SIZE_CHS 512

#define VOLUME_LABEL_OFFSET 0x28
#define VOLUME_UUID_OFFSET 0x32D
#define LABEL_SIZE 32
#define UUID_SIZE 16

#define PVD_LBA 16
#define PVD_ROOT_OFFSET 156
#define PVD_SECTOR_512                                                         \
   (PVD_LBA * (SECTOR_SIZE_ISO / SECTOR_SIZE_CHS))           /* = 64 */
#define PVD_SECTOR_COUNT (SECTOR_SIZE_ISO / SECTOR_SIZE_CHS) /* = 4 */

#define ISO_SIGNATURE "CD001"

#define MAX_OPEN_FILES 8
#define MAX_DRIVES 4

struct Iso9660File
{
   int used;
   uint64_t start_lba; /* absolute LBA in native sector size */
   uint32_t size;      /* file size in bytes                 */
   uint32_t position;  /* current read position              */
};

struct Iso9660Drive
{
   int used;
   uint8_t boot_drive;
   uint32_t part_start;
   uint64_t root_dir_lba;
   uint32_t root_dir_size;
   Iso9660File open_files[MAX_OPEN_FILES];
};

#ifdef COREFS
struct Iso9660Operations
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

static Iso9660Drive s_Drives[MAX_DRIVES] = {0};

#ifdef COREFS
static int s_CoreFsDriveId = -1;
#endif

#ifdef COREFS
extern uint8_t __corefs_bss_start[];
extern uint8_t __corefs_bss_end[];
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

static inline int mem_eq(const void *a, const void *b, int len)
{
   const uint8_t *pa = (const uint8_t *)a;
   const uint8_t *pb = (const uint8_t *)b;
   for (int i = 0; i < len; i++)
   {
      if (pa[i] != pb[i]) return 0;
   }
   return 1;
}

static int iso_read_sector(Iso9660Drive *drive, uint64_t iso_lba, void *buffer)
{
   uint8_t bios_drive = drive->boot_drive;
   uint64_t lba;
   uint16_t count;

   if (bios_drive >= 0xE0)
   {
      lba = (uint64_t)drive->part_start + iso_lba;
      count = 1;
   }
   else
   {
      lba = (uint64_t)drive->part_start + iso_lba * 4;
      count = 4;
   }

   if (bios_drive >= 0xE0)
   {
      /* First read may return stale data (BIOS CD-ROM cache). */
      DISK_ReadLBA(bios_drive, lba, count, buffer);
   }

   return DISK_ReadLBA(bios_drive, lba, count, buffer);
}

static int parse_dir_record(const uint8_t *buf, int off, uint32_t *extent_lba,
                            uint32_t *extent_size, uint8_t *flags,
                            uint8_t *name_len)
{
   uint8_t len = buf[off];
   if (len == 0) return 0;

   *extent_lba = (uint32_t)buf[off + 2] | ((uint32_t)buf[off + 3] << 8) |
                 ((uint32_t)buf[off + 4] << 16) |
                 ((uint32_t)buf[off + 5] << 24);

   *extent_size = (uint32_t)buf[off + 10] | ((uint32_t)buf[off + 11] << 8) |
                  ((uint32_t)buf[off + 12] << 16) |
                  ((uint32_t)buf[off + 13] << 24);

   *flags = buf[off + 25];
   *name_len = buf[off + 32];
   return len;
}

static int name_match(const char *component, int comp_len,
                      const uint8_t *rec_name, int rec_name_len)
{
   int i;
   for (i = 0; i < comp_len; i++)
   {
      if (i >= rec_name_len) return 0;
      char c1 = component[i];
      char c2 = (char)rec_name[i];
      if (c1 >= 'a' && c1 <= 'z') c1 -= 32;
      if (c2 >= 'a' && c2 <= 'z') c2 -= 32;
      if (c1 != c2) return 0;
   }
   if (i == rec_name_len) return 1;
   if (rec_name[i] == ';') return 1;
   return 0;
}

static int label_nonzero(const uint8_t *label)
{
   for (int i = 0; i < LABEL_SIZE; i++)
   {
      if (label[i] != 0) return 1;
   }
   return 0;
}

static int label_match(const uint8_t *iso_label, const uint8_t *expected)
{
   int i = 0;
   for (; i < LABEL_SIZE && expected[i] != 0; i++)
   {
      if (iso_label[i] != expected[i]) return 0;
   }
   if (i == LABEL_SIZE) return 1;
   for (; i < LABEL_SIZE; i++)
   {
      if (iso_label[i] != ' ') return 0;
   }
   return 1;
}

static int check_partition(uint8_t drive, int part_lba,
                           const uint8_t *expected_label,
                           const uint8_t *expected_uuid)
{
   uint8_t buf[SECTOR_SIZE_ISO];
   uint64_t pvd_lba = 0;
   uint16_t pvd_count = 0;
   int identity_requested = 0;

   if (drive < 0x80) return 0;

   if (drive >= 0xE0)
   {
      pvd_lba = (uint64_t)(part_lba + PVD_LBA);
      pvd_count = 1;
   }
   else
   {
      pvd_lba = (uint64_t)(part_lba + PVD_SECTOR_512);
      pvd_count = PVD_SECTOR_COUNT;
   }

   if (DISK_ReadLBA(drive, pvd_lba, pvd_count, buf) != 0) return 0;

   if (buf[0] != 1) return 0;
   if (!mem_eq(&buf[1], ISO_SIGNATURE, 5)) return 0;

   if (expected_label && label_nonzero(expected_label))
   {
      identity_requested = 1;
      if (label_match(&buf[VOLUME_LABEL_OFFSET], expected_label)) return 1;
   }

   if (expected_uuid)
   {
      int uuid_nonzero = 0;
      for (int i = 0; i < UUID_SIZE; i++)
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
         if (mem_eq(&buf[VOLUME_UUID_OFFSET], expected_uuid, UUID_SIZE))
            return 1;
      }
   }

   return !identity_requested;
}

/* ------------------------------------------------------------------ */
/*  Rock Ridge NM (Alternate Name) SUSP entry scan.                   */
/*  Returns 1 if the NM name matches component / comp_len, else 0.    */
/* ------------------------------------------------------------------ */
static int suspend_match_nm(const uint8_t *buf, int off, int rec_len,
                            int name_len, const char *component, int comp_len)
{
   /* System Use area starts after the file identifier.
    * SUSP requires it to be at an even offset from the record start.
    * File identifier is at offset 33 (odd), so padding is needed
    * when name_len is EVEN (making 33+even = odd → pad → even).      */
   int su_off = off + 33 + name_len;
   if (!(name_len & 1)) su_off++;
   int su_end = off + rec_len;

   int nm_pos = 0;
   int in_continue = 0;

   while (su_off + 4 <= su_end)
   {
      if (buf[su_off] == 'N' && buf[su_off + 1] == 'M')
      {
         uint8_t entry_len = buf[su_off + 2];
         if (entry_len < 5) return 0;
         if (su_off + entry_len > su_end) return 0;

         uint8_t flags_rr = buf[su_off + 4];
         const uint8_t *nm_data = &buf[su_off + 5];
         int nm_len = entry_len - 5;

         /* Compare this piece against the remaining component chars. */
         for (int i = 0; i < nm_len; i++)
         {
            if (nm_pos >= comp_len) return 0; /* NM longer than component */
            char c1 = component[nm_pos];
            char c2 = (char)nm_data[i];
            if (c1 >= 'a' && c1 <= 'z') c1 -= 32;
            if (c2 >= 'a' && c2 <= 'z') c2 -= 32;
            if (c1 != c2) return 0;
            nm_pos++;
         }

         in_continue = 1;

         if (flags_rr & 0x01)
         {
            /* CONTINUE set – more NM entries follow. */
            su_off += entry_len;
            continue;
         }

         /* Last NM entry in chain.  Require exact length match. */
         return (nm_pos == comp_len) ? 1 : 0;
      }

      /* If we already saw NM entries and this is not NM, the
       * Rock Ridge name chain is broken – fall back to ISO name. */
      if (in_continue) return 0;

      uint8_t entry_len = buf[su_off + 2];
      if (entry_len < 4) break;
      su_off += entry_len;
   }

   return 0;
}

static int lookup_component(Iso9660Drive *drive, uint64_t dir_lba,
                            uint32_t dir_size, const char *component,
                            int comp_len, uint32_t *out_lba, uint32_t *out_size,
                            uint8_t *out_flags)
{
   uint8_t buf[SECTOR_SIZE_ISO];
   uint32_t bytes_read = 0;

   while (bytes_read < dir_size)
   {
      uint64_t sector_idx = bytes_read / SECTOR_SIZE_ISO;

      if (iso_read_sector(drive, dir_lba + sector_idx, buf) != 0)
      {
         logfmt(
             LOG_ERROR,
             "iso9660: lookup_component: I/O error reading directory sector\n");
         return -EIO;
      }

      int off = 0;
      while (off < SECTOR_SIZE_ISO)
      {
         uint32_t extent_lba, extent_size;
         uint8_t flags, name_len;

         int rec_len = parse_dir_record(buf, off, &extent_lba, &extent_size,
                                        &flags, &name_len);
         if (rec_len == 0) break;

         if (name_len == 1 && (buf[off + 33] == 0 || buf[off + 33] == 1))
         {
            off += rec_len;
            continue;
         }

         /* Try Rock Ridge NM name first (handles long filenames). */
         if (suspend_match_nm(buf, off, rec_len, name_len, component, comp_len))
         {
            *out_lba = extent_lba;
            *out_size = extent_size;
            *out_flags = flags;
            return 0;
         }

         /* Fall back to ISO9660 basic-name matching. */
         if (comp_len == name_len ||
             (name_len > comp_len && buf[off + 33 + comp_len] == ';'))
         {
            if (name_match(component, comp_len, &buf[off + 33], name_len))
            {
               *out_lba = extent_lba;
               *out_size = extent_size;
               *out_flags = flags;
               return 0;
            }
         }
         off += rec_len;
      }
      bytes_read += SECTOR_SIZE_ISO;
   }
   logfmt(LOG_ERROR, "iso9660: lookup_component: entry not found\n");
   return -ENOENT;
}

static int resolve_path(Iso9660Drive *drive, const char *path,
                        uint32_t *out_lba, uint32_t *out_size)
{
   uint64_t dir_lba = drive->root_dir_lba;
   uint32_t dir_size = drive->root_dir_size;
   uint8_t dir_flags = 2;

   if (*path == '/') path++;

   if (*path == '\0')
   {
      *out_lba = (uint32_t)dir_lba;
      *out_size = dir_size;
      return 0;
   }

   while (*path != '\0')
   {
      if (!(dir_flags & 2))
      {
         logfmt(LOG_ERROR, "iso9660: resolve_path: not a directory\n");
         return -ENOTDIR;
      }

      const char *start = path;
      while (*path != '/' && *path != '\0')
         path++;
      int comp_len = (int)(path - start);

      uint32_t child_lba, child_size;
      uint8_t child_flags;
      int rc = lookup_component(drive, dir_lba, dir_size, start, comp_len,
                                &child_lba, &child_size, &child_flags);

      if (rc != 0) return rc;

      dir_lba = child_lba;
      dir_size = child_size;
      dir_flags = child_flags;

      while (*path == '/')
         path++;
   }

   *out_lba = (uint32_t)dir_lba;
   *out_size = dir_size;
   return 0;
}

DL_INC
int ISO9660_Initialize(const uint8_t *bios_drive_list,
                       uint32_t bios_drive_list_count,
                       const uint8_t *partition_uuid,
                       const uint8_t *partition_label)
{
   uint8_t buf[SECTOR_SIZE_ISO];
   int drive_id;
   Iso9660Drive *drive;

   if (!bios_drive_list || bios_drive_list_count == 0)
   {
      logfmt(LOG_ERROR, "iso9660: Initialize: invalid drive list\n");
      return -EINVAL;
   }

   /* Find a free drive slot. */
   for (drive_id = 0; drive_id < MAX_DRIVES; drive_id++)
   {
      if (!s_Drives[drive_id].used) break;
   }
   if (drive_id == MAX_DRIVES)
   {
      logfmt(LOG_ERROR, "iso9660: Initialize: too many open drives\n");
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
            if (check_partition(bios_drive, 0, partition_label,
                                partition_uuid) == 1)
            {
               drive->boot_drive = bios_drive;
               drive->part_start = 0;
               found = 1;
            }
            continue;
         }

         for (int j = 0; j < count && !found; j++)
         {
            if (check_partition(bios_drive, offsets[j], partition_label,
                                partition_uuid) == 1)
            {
               drive->boot_drive = bios_drive;
               drive->part_start = (uint32_t)offsets[j];
               found = 1;
            }
         }
      }
      if (!found)
      {
         logfmt(LOG_ERROR,
                "iso9660: Initialize: no valid ISO9660 partition found\n");
         return -ENODEV;
      }
   }

   if (iso_read_sector(drive, PVD_LBA, buf) != 0)
   {
      logfmt(LOG_ERROR, "iso9660: Initialize: I/O error reading PVD\n");
      return -EIO;
   }
   if (buf[0] != 1)
   {
      logfmt(LOG_ERROR,
             "iso9660: Initialize: invalid PVD volume descriptor type\n");
      return -EINVAL;
   }
   if (!mem_eq(&buf[1], ISO_SIGNATURE, 5))
   {
      logfmt(LOG_ERROR, "iso9660: Initialize: invalid ISO9660 signature\n");
      return -EINVAL;
   }

   {
      uint32_t root_lba, root_size;
      uint8_t root_flags, root_name_len;
      int rl = parse_dir_record(buf, PVD_ROOT_OFFSET, &root_lba, &root_size,
                                &root_flags, &root_name_len);
      if (rl == 0)
      {
         logfmt(LOG_ERROR,
                "iso9660: Initialize: failed to parse root directory record\n");
         return -EINVAL;
      }
      if (!(root_flags & 2))
      {
         logfmt(LOG_ERROR,
                "iso9660: Initialize: root entry is not a directory\n");
         return -EINVAL;
      }

      drive->root_dir_lba = root_lba;
      drive->root_dir_size = root_size;
   }

   drive->used = 1;
   return drive_id;
}

DL_INC
int ISO9660_Open(int drive_id, const char *path)
{
   uint32_t file_lba, file_size;
   Iso9660Drive *drive;

   if (drive_id < 0 || drive_id >= MAX_DRIVES || !s_Drives[drive_id].used)
   {
      logfmt(LOG_ERROR, "iso9660: Open: invalid drive id %d\n", drive_id);
      return -EINVAL;
   }

   drive = &s_Drives[drive_id];

   if (!path || *path == '\0')
   {
      logfmt(LOG_ERROR, "iso9660: Open: empty path\n");
      return -EINVAL;
   }

   int rc = resolve_path(drive, path, &file_lba, &file_size);
   if (rc != 0) return rc;

   int fd;
   for (fd = 0; fd < MAX_OPEN_FILES; fd++)
   {
      if (!drive->open_files[fd].used) break;
   }
   if (fd == MAX_OPEN_FILES)
   {
      logfmt(LOG_ERROR, "iso9660: Open: too many open files\n");
      return -EMFILE;
   }

   uint8_t bios_drive = drive->boot_drive;
   uint64_t abs_lba;
   if (bios_drive >= 0xE0)
      abs_lba = (uint64_t)drive->part_start + (uint64_t)file_lba;
   else
      abs_lba = (uint64_t)drive->part_start + (uint64_t)file_lba * 4;

   drive->open_files[fd].used = 1;
   drive->open_files[fd].start_lba = abs_lba;
   drive->open_files[fd].size = file_size;
   drive->open_files[fd].position = 0;

   return fd;
}

DL_INC
int ISO9660_Read(int drive_id, int fd, void *buffer, int count)
{
   Iso9660Drive *drive;

   if (drive_id < 0 || drive_id >= MAX_DRIVES || !s_Drives[drive_id].used)
   {
      logfmt(LOG_ERROR, "iso9660: Read: invalid drive id %d\n", drive_id);
      return -EINVAL;
   }

   drive = &s_Drives[drive_id];

   if (fd < 0 || fd >= MAX_OPEN_FILES || !drive->open_files[fd].used)
   {
      logfmt(LOG_ERROR, "iso9660: Read: invalid file descriptor %d\n", fd);
      return -EBADF;
   }

   Iso9660File *f = &drive->open_files[fd];
   uint8_t *buf = (uint8_t *)buffer;

   if (f->position >= f->size) return 0;
   if (count <= 0) return 0;

   uint32_t remaining = f->size - f->position;
   if ((uint32_t)count > remaining) count = (int)remaining;

   uint8_t bios_drive = drive->boot_drive;
   uint32_t phys_ss = (bios_drive >= 0xE0) ? SECTOR_SIZE_ISO : SECTOR_SIZE_CHS;
   uint32_t bytes_done = 0;

   while (bytes_done < (uint32_t)count)
   {
      uint32_t file_off = f->position + bytes_done;
      uint64_t phys_sector = f->start_lba + file_off / phys_ss;
      uint32_t phys_off = file_off % phys_ss;
      uint32_t chunk = phys_ss - phys_off;

      if (chunk > (uint32_t)count - bytes_done)
         chunk = (uint32_t)count - bytes_done;

      uint8_t tmp[SECTOR_SIZE_ISO];
      if (DISK_ReadLBA(bios_drive, phys_sector, 1, tmp) != 0)
      {
         if (bytes_done > 0) return (int)bytes_done;
         logfmt(LOG_ERROR, "iso9660: Read: I/O error at sector %llu\n",
                (unsigned long long)phys_sector);
         return -EIO;
      }

      for (uint32_t i = 0; i < chunk; i++)
         buf[bytes_done + i] = tmp[phys_off + i];

      bytes_done += chunk;
   }

   f->position += bytes_done;
   return (int)bytes_done;
}

DL_INC
int ISO9660_Close(int drive_id, int fd)
{
   Iso9660Drive *drive;

   if (drive_id < 0 || drive_id >= MAX_DRIVES || !s_Drives[drive_id].used)
   {
      logfmt(LOG_ERROR, "iso9660: Close: invalid drive id %d\n", drive_id);
      return -EINVAL;
   }

   drive = &s_Drives[drive_id];

   if (fd < 0 || fd >= MAX_OPEN_FILES || !drive->open_files[fd].used)
   {
      logfmt(LOG_ERROR, "iso9660: Close: invalid file descriptor %d\n", fd);
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
   for (uint8_t *ptr = __corefs_bss_start; ptr < __corefs_bss_end; ptr++)
      *ptr = 0;
   int id = ISO9660_Initialize(bios_drive_list, bios_drive_list_count,
                               partition_uuid, partition_label);
   if (id >= 0) s_CoreFsDriveId = id;
   return id;
}

static int corefs_Open(const char *path)
{
   return ISO9660_Open(s_CoreFsDriveId, path);
}

static int corefs_Read(int fd, void *buffer, int count)
{
   return ISO9660_Read(s_CoreFsDriveId, fd, buffer, count);
}

static int corefs_Close(int fd) { return ISO9660_Close(s_CoreFsDriveId, fd); }

static const Iso9660Operations fs_exports
    __attribute__((section(".exports"), used)) = {
        .Initialize = corefs_Initialize,
        .Open = corefs_Open,
        .Read = corefs_Read,
        .Close = corefs_Close,
        .DISK_Read = DISK_Read,
        .DISK_ReadLBA = DISK_ReadLBA,
};

#endif /* COREFS */
