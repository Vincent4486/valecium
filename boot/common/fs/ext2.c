// SPDX-License-Identifier: GPL-3.0-only

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <logging.h>
#include <status.h>

#include <dl/bindgen.h>
#include <dl/callback.h>

typedef struct Ext2File Ext2File;
typedef struct Ext2Drive Ext2Drive;

#ifdef COREFS
typedef struct Ext2Operations Ext2Operations;
#endif

static int ext2_read_block(Ext2Drive *drive, uint32_t block_idx, void *buffer);
static int ext2_read_sector(Ext2Drive *drive, uint64_t lba, void *buffer);
static int ext2_bgdt_offset(Ext2Drive *drive);
static int read_superblock(Ext2Drive *drive);
static int read_inode(Ext2Drive *drive, uint32_t inode_num, uint8_t *out);
static uint32_t ext2_find_block(Ext2Drive *drive, uint32_t inode_block_ptr,
                                uint32_t *indirect, int level,
                                uint32_t block_index);
static uint32_t inode_get_block(Ext2Drive *drive, uint8_t *inode,
                                uint32_t block_index);
static int ext2_lookup(Ext2Drive *drive, uint32_t dir_inode,
                       const char *component, int comp_len, uint32_t *out_inode,
                       uint32_t *out_size);
static int ext2_resolve(Ext2Drive *drive, const char *path, uint32_t *out_inode,
                        uint32_t *out_size);
static int check_partition(uint8_t drive, int part_lba,
                           const uint8_t *expected_label,
                           const uint8_t *expected_uuid);

#define SECTOR_SIZE 512
#define MAX_OPEN_FILES 8
#define SUPERBLOCK_OFFSET 1024

// Superblock offsets
#define SB_INODES_COUNT_OFF 0
#define SB_BLOCKS_COUNT_OFF 4
#define SB_FREE_BLOCKS_COUNT_OFF 12
#define SB_FREE_INODES_COUNT_OFF 16
#define SB_FIRST_DATA_BLOCK_OFF 20
#define SB_LOG_BLOCK_SIZE_OFF 24
#define SB_BLOCKS_PER_GROUP_OFF 32
#define SB_FRAGS_PER_GROUP_OFF 36
#define SB_INODES_PER_GROUP_OFF 40
#define SB_MAGIC_OFF 56
#define SB_STATE_OFF 58
#define SB_REV_LEVEL_OFF 80
#define SB_FIRST_INO_OFF 84
#define SB_INODE_SIZE_OFF 88
#define SB_FEATURE_COMPAT_OFF 92
#define SB_FEATURE_INCOMPAT_OFF 96
#define SB_FEATURE_RO_COMPAT_OFF 100
#define SB_VOLUME_NAME_OFF 120

// Feature constants
#define EXT2_FEATURE_INCOMPAT_FILETYPE 0x00000002
#define EXT2_FEATURE_INCOMPAT_RECOVER 0x00000004
#define EXT2_FEATURE_INCOMPAT_EXTENTS 0x00000040
#define EXT2_FEATURE_INCOMPAT_64BIT 0x00000080
#define EXT2_FEATURE_INCOMPAT_MMP 0x00000100
#define EXT2_FEATURE_INCOMPAT_FLEX_BG 0x00000200
#define EXT2_FEATURE_INCOMPAT_EA_INODE 0x00000400
#define EXT2_FEATURE_INCOMPAT_DIRDATA 0x00001000
#define EXT2_FEATURE_INCOMPAT_CSUM_SEED 0x00002000
#define EXT2_FEATURE_INCOMPAT_LARGEDIR 0x00004000

// Features we handle (safe for read-only bootloader with ext4 support)
#define EXT2_INCOMPAT_HANDLED                                                  \
   (EXT2_FEATURE_INCOMPAT_FILETYPE | EXT2_FEATURE_INCOMPAT_RECOVER |           \
    EXT2_FEATURE_INCOMPAT_EXTENTS | EXT2_FEATURE_INCOMPAT_64BIT |              \
    EXT2_FEATURE_INCOMPAT_MMP | EXT2_FEATURE_INCOMPAT_FLEX_BG |                \
    EXT2_FEATURE_INCOMPAT_EA_INODE | EXT2_FEATURE_INCOMPAT_DIRDATA |           \
    EXT2_FEATURE_INCOMPAT_CSUM_SEED | EXT2_FEATURE_INCOMPAT_LARGEDIR)

// Extent tree constants
#define EXT4_EXTENT_MAGIC 0xF30A
#define EXT4_EXTENTS_FL 0x00080000 /* inode i_flags bit */
#define EXT4_EXTENT_HEADER_SIZE 12
#define EXT4_EXTENT_ENTRY_SIZE 12
#define EXT4_EXTENT_INDEX_SIZE 12

// Superblock
#define SB_DESC_SIZE_OFF 132

// Block group descriptor offsets (32 bytes each)
#define BG_BLOCK_BITMAP_OFF 0
#define BG_INODE_BITMAP_OFF 4
#define BG_INODE_TABLE_OFF 8
#define BG_FREE_BLOCKS_COUNT_OFF 12
#define BG_FREE_INODES_COUNT_OFF 16
#define BG_USED_DIRS_COUNT_OFF 20

// Inode offsets (128-byte default)
#define INODE_MODE_OFF 0
#define INODE_UID_OFF 2
#define INODE_SIZE_OFF 4
#define INODE_GID_OFF 24
#define INODE_LINKS_COUNT_OFF 26
#define INODE_BLOCKS_OFF 28
#define INODE_FLAGS_OFF 32
#define INODE_BLOCK_OFF 40 // 15 x 4 = 60 bytes of block pointers

// Inode mode masks
#define IFMT 0xF000
#define IFREG 0x8000
#define IFDIR 0x4000

// Directory entry offsets
#define DIR_INODE_OFF 0
#define DIR_RECLEN_OFF 4
#define DIR_NAMELEN_OFF 6
#define DIR_FILE_TYPE_OFF 7
#define DIR_NAME_OFF 8

// Ext2 constants
#define EXT2_MAGIC 0xEF53
#define EXT2_GOOD_OLD_REV 0
#define EXT2_DYNAMIC_REV 1
#define EXT2_NDIR_BLOCKS 12
#define EXT2_IND_BLOCK 12
#define EXT2_DIND_BLOCK 13
#define EXT2_TIND_BLOCK 14
#define EXT2_N_BLOCKS 15

#define MAX_DRIVES 4

struct Ext2File
{
   int used;
   uint32_t inode;    /* inode number */
   uint32_t size;     /* file size in bytes */
   uint32_t position; /* current read position */
};

struct Ext2Drive
{
   int used;
   uint8_t boot_drive;
   uint32_t part_start;
   uint32_t block_size;
   uint32_t blocks_per_group;
   uint32_t inodes_per_group;
   uint32_t inode_size;
   uint32_t first_data_block;
   uint32_t total_inodes;
   uint32_t rev_level;
   uint32_t desc_size;
   uint32_t root_inode;
   uint32_t root_size;
   Ext2File open_files[MAX_OPEN_FILES];
};

#ifdef COREFS
struct Ext2Operations
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

static Ext2Drive s_Drives[MAX_DRIVES] = {0};

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

static int ext2_read_sector(Ext2Drive *drive, uint64_t lba, void *buffer)
{
   return DISK_ReadLBA(drive->boot_drive, (uint64_t)drive->part_start + lba, 1,
                       buffer);
}

static int ext2_read_block(Ext2Drive *drive, uint32_t block_idx, void *buffer)
{
   uint32_t sectors_per_block = drive->block_size / SECTOR_SIZE;
   uint32_t first_lba;

   if (drive->block_size == 1024)
   {
      first_lba = block_idx * (1024 / SECTOR_SIZE);
   }
   else
   {
      first_lba = block_idx * (drive->block_size / SECTOR_SIZE);
   }

   return DISK_ReadLBA(drive->boot_drive,
                       (uint64_t)drive->part_start + first_lba,
                       (uint16_t)sectors_per_block, buffer);
}

static int read_superblock(Ext2Drive *drive)
{
   uint8_t buf[SECTOR_SIZE];

   uint32_t sb_lba = SUPERBLOCK_OFFSET / SECTOR_SIZE;
   uint32_t sb_off = SUPERBLOCK_OFFSET % SECTOR_SIZE;

   if (ext2_read_sector(drive, sb_lba, buf) != 0)
   {
      logfmt(LOG_ERROR,
             "ext2: read_superblock: I/O error reading superblock sector\n");
      return -EIO;
   }

   uint16_t magic = (uint16_t)buf[sb_off + SB_MAGIC_OFF] |
                    ((uint16_t)buf[sb_off + SB_MAGIC_OFF + 1] << 8);
   if (magic != EXT2_MAGIC)
   {
      logfmt(LOG_ERROR, "ext2: read_superblock: invalid magic 0x%04x\n", magic);
      return -EINVAL;
   }

   uint32_t log_block_size =
       (uint32_t)buf[sb_off + SB_LOG_BLOCK_SIZE_OFF] |
       ((uint32_t)buf[sb_off + SB_LOG_BLOCK_SIZE_OFF + 1] << 8) |
       ((uint32_t)buf[sb_off + SB_LOG_BLOCK_SIZE_OFF + 2] << 16) |
       ((uint32_t)buf[sb_off + SB_LOG_BLOCK_SIZE_OFF + 3] << 24);
   drive->block_size = 1024 << log_block_size;

   drive->blocks_per_group =
       (uint32_t)buf[sb_off + SB_BLOCKS_PER_GROUP_OFF] |
       ((uint32_t)buf[sb_off + SB_BLOCKS_PER_GROUP_OFF + 1] << 8) |
       ((uint32_t)buf[sb_off + SB_BLOCKS_PER_GROUP_OFF + 2] << 16) |
       ((uint32_t)buf[sb_off + SB_BLOCKS_PER_GROUP_OFF + 3] << 24);

   drive->inodes_per_group =
       (uint32_t)buf[sb_off + SB_INODES_PER_GROUP_OFF] |
       ((uint32_t)buf[sb_off + SB_INODES_PER_GROUP_OFF + 1] << 8) |
       ((uint32_t)buf[sb_off + SB_INODES_PER_GROUP_OFF + 2] << 16) |
       ((uint32_t)buf[sb_off + SB_INODES_PER_GROUP_OFF + 3] << 24);

   drive->total_inodes =
       (uint32_t)buf[sb_off + SB_INODES_COUNT_OFF] |
       ((uint32_t)buf[sb_off + SB_INODES_COUNT_OFF + 1] << 8) |
       ((uint32_t)buf[sb_off + SB_INODES_COUNT_OFF + 2] << 16) |
       ((uint32_t)buf[sb_off + SB_INODES_COUNT_OFF + 3] << 24);

   drive->first_data_block =
       (uint32_t)buf[sb_off + SB_FIRST_DATA_BLOCK_OFF] |
       ((uint32_t)buf[sb_off + SB_FIRST_DATA_BLOCK_OFF + 1] << 8) |
       ((uint32_t)buf[sb_off + SB_FIRST_DATA_BLOCK_OFF + 2] << 16) |
       ((uint32_t)buf[sb_off + SB_FIRST_DATA_BLOCK_OFF + 3] << 24);

   drive->rev_level = (uint32_t)buf[sb_off + SB_REV_LEVEL_OFF] |
                      ((uint32_t)buf[sb_off + SB_REV_LEVEL_OFF + 1] << 8) |
                      ((uint32_t)buf[sb_off + SB_REV_LEVEL_OFF + 2] << 16) |
                      ((uint32_t)buf[sb_off + SB_REV_LEVEL_OFF + 3] << 24);

   drive->inode_size = (uint32_t)buf[sb_off + SB_INODE_SIZE_OFF] |
                       ((uint32_t)buf[sb_off + SB_INODE_SIZE_OFF + 1] << 8) |
                       ((uint32_t)buf[sb_off + SB_INODE_SIZE_OFF + 2] << 16) |
                       ((uint32_t)buf[sb_off + SB_INODE_SIZE_OFF + 3] << 24);
   if (drive->inode_size < 128) drive->inode_size = 128;

   uint32_t incompat =
       (uint32_t)buf[sb_off + SB_FEATURE_INCOMPAT_OFF] |
       ((uint32_t)buf[sb_off + SB_FEATURE_INCOMPAT_OFF + 1] << 8) |
       ((uint32_t)buf[sb_off + SB_FEATURE_INCOMPAT_OFF + 2] << 16) |
       ((uint32_t)buf[sb_off + SB_FEATURE_INCOMPAT_OFF + 3] << 24);

   if (incompat & ~EXT2_INCOMPAT_HANDLED)
   {
      logfmt(LOG_ERROR,
             "ext2: read_superblock: unsupported incompat features 0x%08x\n",
             incompat);
      return -EINVAL;
   }

   drive->desc_size = 32;
   if (drive->rev_level >= EXT2_DYNAMIC_REV)
   {
      uint32_t ds = (uint32_t)buf[sb_off + SB_DESC_SIZE_OFF] |
                    ((uint32_t)buf[sb_off + SB_DESC_SIZE_OFF + 1] << 8) |
                    ((uint32_t)buf[sb_off + SB_DESC_SIZE_OFF + 2] << 16) |
                    ((uint32_t)buf[sb_off + SB_DESC_SIZE_OFF + 3] << 24);
      if (ds >= 64)
         drive->desc_size = 64;
      else if (ds >= 32)
         drive->desc_size = 32;
   }
   if (drive->desc_size < 64 && (incompat & EXT2_FEATURE_INCOMPAT_64BIT))
      drive->desc_size = 64;

   return SUCCESS;
}

/**
 * Calculate offset (in bytes from partition start) of the
 * block group descriptor table.
 */
static int ext2_bgdt_offset(Ext2Drive *drive)
{
   if (drive->block_size == 1024)
      return 2 * drive->block_size;
   else
      return drive->block_size;
}

static int read_inode(Ext2Drive *drive, uint32_t inode_num, uint8_t *out)
{
   if (inode_num == 0)
   {
      logfmt(LOG_ERROR, "ext2: read_inode: inode number is 0\n");
      return -EINVAL;
   }

   uint32_t group = (inode_num - 1) / drive->inodes_per_group;
   uint32_t index = (inode_num - 1) % drive->inodes_per_group;

   uint8_t bgdt_buf[64];
   int bgdt_off = ext2_bgdt_offset(drive);
   uint32_t bgdt_block = (uint32_t)bgdt_off / drive->block_size;
   uint32_t bgdt_byte = (uint32_t)bgdt_off % drive->block_size;

   {
      uint8_t block_buf[4096];
      if (ext2_read_block(drive, bgdt_block, block_buf) != 0)
      {
         logfmt(LOG_ERROR,
                "ext2: read_inode: I/O error reading BGD table block\n");
         return -EIO;
      }

      uint32_t bgdt_entry_off = bgdt_byte + group * drive->desc_size;
      for (uint32_t i = 0; i < drive->desc_size && i < 64; i++)
         bgdt_buf[i] = block_buf[bgdt_entry_off + i];
   }

   uint32_t inode_table_block =
       (uint32_t)bgdt_buf[BG_INODE_TABLE_OFF] |
       ((uint32_t)bgdt_buf[BG_INODE_TABLE_OFF + 1] << 8) |
       ((uint32_t)bgdt_buf[BG_INODE_TABLE_OFF + 2] << 16) |
       ((uint32_t)bgdt_buf[BG_INODE_TABLE_OFF + 3] << 24);

   uint32_t inodes_per_block = drive->block_size / drive->inode_size;
   uint32_t inode_block_idx = inode_table_block + (index / inodes_per_block);
   uint32_t inode_byte_off = (index % inodes_per_block) * drive->inode_size;

   uint8_t block_buf[4096];
   if (ext2_read_block(drive, inode_block_idx, block_buf) != 0)
   {
      logfmt(LOG_ERROR,
             "ext2: read_inode: I/O error reading inode table block\n");
      return -EIO;
   }

   for (uint32_t i = 0; i < drive->inode_size && i < 128; i++)
      out[i] = block_buf[inode_byte_off + i];

   return SUCCESS;
}

static uint32_t ext2_find_block(Ext2Drive *drive, uint32_t block_ptr,
                                uint32_t *indirect, int level,
                                uint32_t block_index)
{
   uint32_t ptrs_per_block = drive->block_size / 4;

   if (level == 0)
   {
      // Single indirect: block_ptr points to an array of data block pointers
      if (indirect[0] == 0 || indirect[0] >= 0xFFFFFFF0u)
      {
         // Lazy init: read the indirect block
         uint8_t buf[4096];
         if (ext2_read_block(drive, block_ptr, buf) != 0) return 0;
         for (uint32_t i = 0; i < ptrs_per_block; i++)
         {
            indirect[i] = (uint32_t)buf[i * 4] |
                          ((uint32_t)buf[i * 4 + 1] << 8) |
                          ((uint32_t)buf[i * 4 + 2] << 16) |
                          ((uint32_t)buf[i * 4 + 3] << 24);
         }
      }
      return indirect[block_index];
   }

   // Higher level: first index selects which child indirect block
   uint32_t child_idx = block_index / ptrs_per_block;
   uint32_t sub_idx = block_index % ptrs_per_block;

   if (level == 1)
   {
      // Double indirect: block_ptr points to array of indirect block pointers
      uint8_t buf[4096];
      if (ext2_read_block(drive, block_ptr, buf) != 0) return 0;
      uint32_t child_block = (uint32_t)buf[child_idx * 4] |
                             ((uint32_t)buf[child_idx * 4 + 1] << 8) |
                             ((uint32_t)buf[child_idx * 4 + 2] << 16) |
                             ((uint32_t)buf[child_idx * 4 + 3] << 24);
      if (child_block == 0) return 0;

      // Now resolve single indirect
      uint8_t child_buf[4096];
      if (ext2_read_block(drive, child_block, child_buf) != 0) return 0;
      return (uint32_t)child_buf[sub_idx * 4] |
             ((uint32_t)child_buf[sub_idx * 4 + 1] << 8) |
             ((uint32_t)child_buf[sub_idx * 4 + 2] << 16) |
             ((uint32_t)child_buf[sub_idx * 4 + 3] << 24);
   }

   // Triple indirect
   {
      uint8_t buf[4096];
      if (ext2_read_block(drive, block_ptr, buf) != 0) return 0;
      uint32_t l1_block = (uint32_t)buf[child_idx * 4] |
                          ((uint32_t)buf[child_idx * 4 + 1] << 8) |
                          ((uint32_t)buf[child_idx * 4 + 2] << 16) |
                          ((uint32_t)buf[child_idx * 4 + 3] << 24);
      if (l1_block == 0) return 0;

      uint32_t l2_idx = sub_idx / ptrs_per_block;
      uint32_t l2_sub = sub_idx % ptrs_per_block;

      if (ext2_read_block(drive, l1_block, buf) != 0) return 0;
      uint32_t l2_block = (uint32_t)buf[l2_idx * 4] |
                          ((uint32_t)buf[l2_idx * 4 + 1] << 8) |
                          ((uint32_t)buf[l2_idx * 4 + 2] << 16) |
                          ((uint32_t)buf[l2_idx * 4 + 3] << 24);
      if (l2_block == 0) return 0;

      if (ext2_read_block(drive, l2_block, buf) != 0) return 0;
      return (uint32_t)buf[l2_sub * 4] | ((uint32_t)buf[l2_sub * 4 + 1] << 8) |
             ((uint32_t)buf[l2_sub * 4 + 2] << 16) |
             ((uint32_t)buf[l2_sub * 4 + 3] << 24);
   }
}

/**
 * Resolve block number through ext4 extent tree.
 * inode points to the inode buffer (i_block starts at INODE_BLOCK_OFF).
 */
static uint32_t ext4_extent_get_block(Ext2Drive *drive, uint8_t *inode,
                                      uint32_t block_index)
{
   // Extent header is at i_block[0..11], i.e. INODE_BLOCK_OFF
   const uint8_t *eh = inode + INODE_BLOCK_OFF;
   uint16_t magic = (uint16_t)eh[0] | ((uint16_t)eh[1] << 8);
   if (magic != EXT4_EXTENT_MAGIC) return 0;

   uint16_t depth = (uint16_t)eh[6] | ((uint16_t)eh[7] << 8);
   uint16_t entries = (uint16_t)eh[2] | ((uint16_t)eh[3] << 8);

   if (depth == 0)
   {
      // Leaf: entries are ext4_extent (12 bytes each)
      for (uint16_t i = 0; i < entries; i++)
      {
         const uint8_t *ex =
             eh + EXT4_EXTENT_HEADER_SIZE + i * EXT4_EXTENT_ENTRY_SIZE;
         uint32_t ee_block = (uint32_t)ex[0] | ((uint32_t)ex[1] << 8) |
                             ((uint32_t)ex[2] << 16) | ((uint32_t)ex[3] << 24);
         uint16_t ee_len = (uint16_t)ex[4] | ((uint16_t)ex[5] << 8);
         // ee_len of 0x8000 means 32768 blocks (uninitialized)
         uint16_t len = ee_len & 0x7FFF;
         if (len == 0) len = 32768;

         if (block_index >= ee_block && block_index < ee_block + len)
         {
            uint32_t start_lo = (uint32_t)ex[8] | ((uint32_t)ex[9] << 8) |
                                ((uint32_t)ex[10] << 16) |
                                ((uint32_t)ex[11] << 24);
            uint32_t start_hi = (uint32_t)ex[6] | ((uint32_t)ex[7] << 8);
            return (start_lo | (start_hi << 16)) + (block_index - ee_block);
         }
      }
      return 0;
   }

   // Depth > 0: index nodes. Follow the tree.
   uint8_t block_buf[4096];
   const uint8_t *current_eh = eh;
   uint32_t current_entries = entries;
   uint32_t current_depth = depth;

   // Walk down the tree until we reach a leaf
   while (current_depth > 0)
   {
      uint16_t i;
      for (i = 0; i < current_entries - 1; i++)
      {
         const uint8_t *ix =
             current_eh + EXT4_EXTENT_HEADER_SIZE + i * EXT4_EXTENT_INDEX_SIZE;
         uint32_t ei_block = (uint32_t)ix[0] | ((uint32_t)ix[1] << 8) |
                             ((uint32_t)ix[2] << 16) | ((uint32_t)ix[3] << 24);
         // Next entry's starting block
         const uint8_t *ix_next = current_eh + EXT4_EXTENT_HEADER_SIZE +
                                  (i + 1) * EXT4_EXTENT_INDEX_SIZE;
         uint32_t next_block =
             (uint32_t)ix_next[0] | ((uint32_t)ix_next[1] << 8) |
             ((uint32_t)ix_next[2] << 16) | ((uint32_t)ix_next[3] << 24);
         if (block_index >= ei_block && block_index < next_block) break;
      }

      const uint8_t *ix =
          current_eh + EXT4_EXTENT_HEADER_SIZE + i * EXT4_EXTENT_INDEX_SIZE;
      uint32_t leaf_lo = (uint32_t)ix[4] | ((uint32_t)ix[5] << 8) |
                         ((uint32_t)ix[6] << 16) | ((uint32_t)ix[7] << 24);
      uint32_t leaf_hi = (uint32_t)ix[8] | ((uint32_t)ix[9] << 8) |
                         ((uint32_t)ix[10] << 16) | ((uint32_t)ix[11] << 24);
      uint32_t child_block = leaf_lo | (leaf_hi << 16);

      if (ext2_read_block(drive, child_block, block_buf) != 0) return 0;

      current_eh = block_buf;
      current_depth--;
      uint16_t child_entries =
          (uint16_t)block_buf[2] | ((uint16_t)block_buf[3] << 8);
      current_entries = child_entries;
   }

   // Now at leaf level
   for (uint16_t i = 0; i < current_entries; i++)
   {
      const uint8_t *ex =
          current_eh + EXT4_EXTENT_HEADER_SIZE + i * EXT4_EXTENT_ENTRY_SIZE;
      uint32_t ee_block = (uint32_t)ex[0] | ((uint32_t)ex[1] << 8) |
                          ((uint32_t)ex[2] << 16) | ((uint32_t)ex[3] << 24);
      uint16_t ee_len = (uint16_t)ex[4] | ((uint16_t)ex[5] << 8);
      uint16_t len = ee_len & 0x7FFF;
      if (len == 0) len = 32768;

      if (block_index >= ee_block && block_index < ee_block + len)
      {
         uint32_t start_lo = (uint32_t)ex[8] | ((uint32_t)ex[9] << 8) |
                             ((uint32_t)ex[10] << 16) |
                             ((uint32_t)ex[11] << 24);
         uint32_t start_hi = (uint32_t)ex[6] | ((uint32_t)ex[7] << 8);
         return (start_lo | (start_hi << 16)) + (block_index - ee_block);
      }
   }

   return 0;
}

static uint32_t inode_get_block(Ext2Drive *drive, uint8_t *inode,
                                uint32_t block_index)
{
   uint32_t flags = (uint32_t)inode[INODE_FLAGS_OFF] |
                    ((uint32_t)inode[INODE_FLAGS_OFF + 1] << 8) |
                    ((uint32_t)inode[INODE_FLAGS_OFF + 2] << 16) |
                    ((uint32_t)inode[INODE_FLAGS_OFF + 3] << 24);
   if (flags & EXT4_EXTENTS_FL)
      return ext4_extent_get_block(drive, inode, block_index);

   uint32_t direct[EXT2_NDIR_BLOCKS];
   for (int i = 0; i < EXT2_NDIR_BLOCKS; i++)
   {
      direct[i] = (uint32_t)inode[INODE_BLOCK_OFF + i * 4] |
                  ((uint32_t)inode[INODE_BLOCK_OFF + i * 4 + 1] << 8) |
                  ((uint32_t)inode[INODE_BLOCK_OFF + i * 4 + 2] << 16) |
                  ((uint32_t)inode[INODE_BLOCK_OFF + i * 4 + 3] << 24);
   }

   if (block_index < EXT2_NDIR_BLOCKS) return direct[block_index];

   uint32_t ptrs_per_block = drive->block_size / 4;

   // Single indirect
   uint32_t ind_block =
       (uint32_t)inode[INODE_BLOCK_OFF + EXT2_IND_BLOCK * 4] |
       ((uint32_t)inode[INODE_BLOCK_OFF + EXT2_IND_BLOCK * 4 + 1] << 8) |
       ((uint32_t)inode[INODE_BLOCK_OFF + EXT2_IND_BLOCK * 4 + 2] << 16) |
       ((uint32_t)inode[INODE_BLOCK_OFF + EXT2_IND_BLOCK * 4 + 3] << 24);
   uint32_t ind_start = EXT2_NDIR_BLOCKS;
   if (block_index < ind_start + ptrs_per_block)
   {
      uint8_t buf[4096];
      if (ext2_read_block(drive, ind_block, buf) != 0) return 0;
      uint32_t idx = block_index - ind_start;
      return (uint32_t)buf[idx * 4] | ((uint32_t)buf[idx * 4 + 1] << 8) |
             ((uint32_t)buf[idx * 4 + 2] << 16) |
             ((uint32_t)buf[idx * 4 + 3] << 24);
   }

   uint32_t dind_block =
       (uint32_t)inode[INODE_BLOCK_OFF + EXT2_DIND_BLOCK * 4] |
       ((uint32_t)inode[INODE_BLOCK_OFF + EXT2_DIND_BLOCK * 4 + 1] << 8) |
       ((uint32_t)inode[INODE_BLOCK_OFF + EXT2_DIND_BLOCK * 4 + 2] << 16) |
       ((uint32_t)inode[INODE_BLOCK_OFF + EXT2_DIND_BLOCK * 4 + 3] << 24);
   uint32_t dind_start = ind_start + ptrs_per_block;
   if (block_index < dind_start + ptrs_per_block * ptrs_per_block)
   {
      uint32_t idx = block_index - dind_start;
      return ext2_find_block(drive, dind_block, NULL, 1, idx);
   }

   uint32_t tind_block =
       (uint32_t)inode[INODE_BLOCK_OFF + EXT2_TIND_BLOCK * 4] |
       ((uint32_t)inode[INODE_BLOCK_OFF + EXT2_TIND_BLOCK * 4 + 1] << 8) |
       ((uint32_t)inode[INODE_BLOCK_OFF + EXT2_TIND_BLOCK * 4 + 2] << 16) |
       ((uint32_t)inode[INODE_BLOCK_OFF + EXT2_TIND_BLOCK * 4 + 3] << 24);
   uint32_t tind_start = dind_start + ptrs_per_block * ptrs_per_block;
   uint32_t idx = block_index - tind_start;
   return ext2_find_block(drive, tind_block, NULL, 2, idx);
}

static int ext2_lookup(Ext2Drive *drive, uint32_t dir_inode,
                       const char *component, int comp_len, uint32_t *out_inode,
                       uint32_t *out_size)
{
   uint8_t inode_buf[128];
   if (read_inode(drive, dir_inode, inode_buf) != 0)
   {
      logfmt(LOG_ERROR,
             "ext2: ext2_lookup: failed to read directory inode %u\n",
             dir_inode);
      return -EIO;
   }

   uint32_t dir_size = (uint32_t)inode_buf[INODE_SIZE_OFF] |
                       ((uint32_t)inode_buf[INODE_SIZE_OFF + 1] << 8) |
                       ((uint32_t)inode_buf[INODE_SIZE_OFF + 2] << 16) |
                       ((uint32_t)inode_buf[INODE_SIZE_OFF + 3] << 24);
   uint32_t bytes_read = 0;

   while (bytes_read < dir_size)
   {
      uint32_t block_idx = bytes_read / drive->block_size;
      uint32_t block_off = bytes_read % drive->block_size;

      uint32_t phys_block = inode_get_block(drive, inode_buf, block_idx);
      if (phys_block == 0) break;

      uint8_t block_data[4096];
      if (ext2_read_block(drive, phys_block, block_data) != 0)
      {
         logfmt(LOG_ERROR,
                "ext2: ext2_lookup: I/O error reading directory block\n");
         return -EIO;
      }

      uint32_t pos = block_off;
      while (pos + 8 <= drive->block_size &&
             bytes_read + (pos - block_off) < dir_size)
      {
         uint32_t entry_inode =
             (uint32_t)block_data[pos + DIR_INODE_OFF] |
             ((uint32_t)block_data[pos + DIR_INODE_OFF + 1] << 8) |
             ((uint32_t)block_data[pos + DIR_INODE_OFF + 2] << 16) |
             ((uint32_t)block_data[pos + DIR_INODE_OFF + 3] << 24);
         uint16_t rec_len =
             (uint16_t)block_data[pos + DIR_RECLEN_OFF] |
             ((uint16_t)block_data[pos + DIR_RECLEN_OFF + 1] << 8);
         uint8_t name_len = block_data[pos + DIR_NAMELEN_OFF];

         if (entry_inode == 0 || rec_len == 0) break;

         if (name_len == (uint8_t)comp_len)
         {
            int match = 1;
            for (int i = 0; i < comp_len; i++)
            {
               char c1 = component[i];
               char c2 = (char)block_data[pos + DIR_NAME_OFF + i];
               if (c1 >= 'A' && c1 <= 'Z') c1 += 0x20;
               if (c2 >= 'A' && c2 <= 'Z') c2 += 0x20;
               if (c1 != c2)
               {
                  match = 0;
                  break;
               }
            }
            if (match)
            {
               *out_inode = entry_inode;
               uint8_t found_inode[128];
               if (read_inode(drive, entry_inode, found_inode) != 0)
               {
                  logfmt(
                      LOG_ERROR,
                      "ext2: ext2_lookup: I/O error reading found inode %u\n",
                      entry_inode);
                  return -EIO;
               }
               *out_size = (uint32_t)found_inode[INODE_SIZE_OFF] |
                           ((uint32_t)found_inode[INODE_SIZE_OFF + 1] << 8) |
                           ((uint32_t)found_inode[INODE_SIZE_OFF + 2] << 16) |
                           ((uint32_t)found_inode[INODE_SIZE_OFF + 3] << 24);
               return 0;
            }
         }

         pos += rec_len;
      }

      bytes_read += drive->block_size;
   }

   logfmt(LOG_ERROR, "ext2: ext2_lookup: entry not found in directory\n");
   return -ENOENT;
}

static int ext2_resolve(Ext2Drive *drive, const char *path, uint32_t *out_inode,
                        uint32_t *out_size)
{
   uint32_t cur_inode = drive->root_inode;
   uint32_t cur_size = drive->root_size;

   if (*path == '/') path++;

   if (*path == '\0')
   {
      *out_inode = cur_inode;
      *out_size = cur_size;
      return 0;
   }

   while (*path != '\0')
   {
      const char *start = path;
      while (*path != '/' && *path != '\0')
         path++;
      int comp_len = (int)(path - start);

      uint32_t child_inode, child_size;
      int rc = ext2_lookup(drive, cur_inode, start, comp_len, &child_inode,
                           &child_size);
      if (rc != 0) return rc;

      cur_inode = child_inode;
      cur_size = child_size;

      while (*path == '/')
         path++;
   }

   *out_inode = cur_inode;
   *out_size = cur_size;
   return 0;
}

static int check_partition(uint8_t bios_drive, int part_lba,
                           const uint8_t *expected_label,
                           const uint8_t *expected_uuid)
{
   uint8_t buf[SECTOR_SIZE];
   int identity_requested = 0;

   uint32_t sb_lba = SUPERBLOCK_OFFSET / SECTOR_SIZE;
   uint32_t sb_off = SUPERBLOCK_OFFSET % SECTOR_SIZE;

   uint64_t abs_lba = (uint64_t)(uint32_t)part_lba + sb_lba;
   if (DISK_ReadLBA(bios_drive, abs_lba, 1, buf) != 0) return 0;

   uint16_t magic = (uint16_t)buf[sb_off + SB_MAGIC_OFF] |
                    ((uint16_t)buf[sb_off + SB_MAGIC_OFF + 1] << 8);
   if (magic != EXT2_MAGIC) return 0;

   // Check label if provided
   if (expected_label)
   {
      int label_nonzero = 0;
      for (int i = 0; i < 16; i++)
      {
         if (expected_label[i] != 0)
         {
            label_nonzero = 1;
            break;
         }
      }

      if (label_nonzero)
      {
         identity_requested = 1;
         int match = 1;
         for (int i = 0; i < 16; i++)
         {
            char c1 = (char)expected_label[i];
            char c2 = (char)buf[sb_off + SB_VOLUME_NAME_OFF + i];
            if (c1 == '\0') break;
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
      int match = 1;
      for (int i = 0; i < 16; i++)
      {
         if (expected_uuid[i] != 0)
            uuid_nonzero = 1;
         if (expected_uuid[i] != buf[sb_off + 104 + i])
            match = 0;
      }
      if (uuid_nonzero)
      {
         identity_requested = 1;
         if (match) return 1;
      }
   }

   return !identity_requested;
}

DL_INC
int EXT2_Initialize(const uint8_t *bios_drive_list,
                    uint32_t bios_drive_list_count,
                    const uint8_t *partition_uuid,
                    const uint8_t *partition_label)
{
   int drive_id;
   Ext2Drive *drive;

   if (!bios_drive_list || bios_drive_list_count == 0)
   {
      logfmt(LOG_ERROR, "ext2: Initialize: invalid drive list\n");
      return -EINVAL;
   }

   for (drive_id = 0; drive_id < MAX_DRIVES; drive_id++)
   {
      if (!s_Drives[drive_id].used) break;
   }
   if (drive_id == MAX_DRIVES)
   {
      logfmt(LOG_ERROR, "ext2: Initialize: too many open drives\n");
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
                                partition_uuid))
            {
               drive->boot_drive = bios_drive;
               drive->part_start = 0;
               if (read_superblock(drive) == SUCCESS) found = 1;
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
               if (read_superblock(drive) == SUCCESS) found = 1;
            }
         }
      }
      if (!found)
      {
         logfmt(LOG_ERROR,
                "ext2: Initialize: no valid ext2/ext4 partition found\n");
         return -ENODEV;
      }
   }

   drive->root_inode = 2;

   uint8_t root_inode[128];
   if (read_inode(drive, drive->root_inode, root_inode) != 0)
   {
      logfmt(LOG_ERROR, "ext2: Initialize: I/O error reading root inode\n");
      return -EIO;
   }

   drive->root_size = (uint32_t)root_inode[INODE_SIZE_OFF] |
                      ((uint32_t)root_inode[INODE_SIZE_OFF + 1] << 8) |
                      ((uint32_t)root_inode[INODE_SIZE_OFF + 2] << 16) |
                      ((uint32_t)root_inode[INODE_SIZE_OFF + 3] << 24);

   drive->used = 1;
   return drive_id;
}

DL_INC
int EXT2_Open(int drive_id, const char *path)
{
   Ext2Drive *drive;

   if (drive_id < 0 || drive_id >= MAX_DRIVES || !s_Drives[drive_id].used)
   {
      logfmt(LOG_ERROR, "ext2: Open: invalid drive id %d\n", drive_id);
      return -EINVAL;
   }

   drive = &s_Drives[drive_id];

   if (!path || *path == '\0')
   {
      logfmt(LOG_ERROR, "ext2: Open: empty path\n");
      return -EINVAL;
   }

   uint32_t file_inode, file_size;
   int rc = ext2_resolve(drive, path, &file_inode, &file_size);
   if (rc != 0) return rc;

   uint8_t inode[128];
   if (read_inode(drive, file_inode, inode) != 0)
   {
      logfmt(LOG_ERROR, "ext2: Open: I/O error reading inode %u\n", file_inode);
      return -EIO;
   }
   uint16_t mode = (uint16_t)inode[INODE_MODE_OFF] |
                   ((uint16_t)inode[INODE_MODE_OFF + 1] << 8);
   if ((mode & IFMT) == IFDIR)
   {
      logfmt(LOG_ERROR, "ext2: Open: path is a directory\n");
      return -EINVAL;
   }

   int fd;
   for (fd = 0; fd < MAX_OPEN_FILES; fd++)
   {
      if (!drive->open_files[fd].used) break;
   }
   if (fd == MAX_OPEN_FILES)
   {
      logfmt(LOG_ERROR, "ext2: Open: too many open files\n");
      return -EMFILE;
   }

   drive->open_files[fd].used = 1;
   drive->open_files[fd].inode = file_inode;
   drive->open_files[fd].size = file_size;
   drive->open_files[fd].position = 0;

   return fd;
}

DL_INC
int EXT2_Read(int drive_id, int fd, void *buffer, int count)
{
   Ext2Drive *drive;

   if (drive_id < 0 || drive_id >= MAX_DRIVES || !s_Drives[drive_id].used)
   {
      logfmt(LOG_ERROR, "ext2: Read: invalid drive id %d\n", drive_id);
      return -EINVAL;
   }

   drive = &s_Drives[drive_id];

   if (fd < 0 || fd >= MAX_OPEN_FILES || !drive->open_files[fd].used)
   {
      logfmt(LOG_ERROR, "ext2: Read: invalid file descriptor %d\n", fd);
      return -EBADF;
   }

   Ext2File *f = &drive->open_files[fd];
   uint8_t *buf = (uint8_t *)buffer;

   if (f->position >= f->size) return 0;
   if (count <= 0) return 0;

   uint32_t remaining = f->size - f->position;
   if ((uint32_t)count > remaining) count = (int)remaining;

   uint8_t inode[128];
   if (read_inode(drive, f->inode, inode) != 0)
   {
      logfmt(LOG_ERROR, "ext2: Read: I/O error reading inode %u\n", f->inode);
      return -EIO;
   }

   uint32_t bytes_done = 0;
   while (bytes_done < (uint32_t)count)
   {
      uint32_t file_off = f->position + bytes_done;
      uint32_t block_idx = file_off / drive->block_size;
      uint32_t block_off = file_off % drive->block_size;

      uint32_t phys_block = inode_get_block(drive, inode, block_idx);
      if (phys_block == 0) break;

      uint8_t block_data[4096];
      if (ext2_read_block(drive, phys_block, block_data) != 0)
      {
         if (bytes_done > 0) return (int)bytes_done;
         logfmt(LOG_ERROR, "ext2: Read: I/O error reading data block\n");
         return -EIO;
      }

      uint32_t chunk = drive->block_size - block_off;
      if (chunk > (uint32_t)count - bytes_done)
         chunk = (uint32_t)count - bytes_done;

      for (uint32_t i = 0; i < chunk; i++)
         buf[bytes_done + i] = block_data[block_off + i];

      bytes_done += chunk;
   }

   f->position += bytes_done;
   return (int)bytes_done;
}

DL_INC
int EXT2_Close(int drive_id, int fd)
{
   Ext2Drive *drive;

   if (drive_id < 0 || drive_id >= MAX_DRIVES || !s_Drives[drive_id].used)
   {
      logfmt(LOG_ERROR, "ext2: Close: invalid drive id %d\n", drive_id);
      return -EINVAL;
   }

   drive = &s_Drives[drive_id];

   if (fd < 0 || fd >= MAX_OPEN_FILES || !drive->open_files[fd].used)
   {
      logfmt(LOG_ERROR, "ext2: Close: invalid file descriptor %d\n", fd);
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
   int id = EXT2_Initialize(bios_drive_list, bios_drive_list_count,
                            partition_uuid, partition_label);
   if (id >= 0) s_CoreFsDriveId = id;
   return id;
}

static int corefs_Open(const char *path)
{
   return EXT2_Open(s_CoreFsDriveId, path);
}

static int corefs_Read(int fd, void *buffer, int count)
{
   return EXT2_Read(s_CoreFsDriveId, fd, buffer, count);
}

static int corefs_Close(int fd) { return EXT2_Close(s_CoreFsDriveId, fd); }

static const Ext2Operations fs_exports
    __attribute__((section(".exports"), used)) = {
        .Initialize = corefs_Initialize,
        .Open = corefs_Open,
        .Read = corefs_Read,
        .Close = corefs_Close,
        .DISK_Read = DISK_Read,
        .DISK_ReadLBA = DISK_ReadLBA,
};

#endif /* COREFS */
