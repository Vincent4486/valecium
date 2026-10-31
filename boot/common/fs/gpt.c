// SPDX-License-Identifier: GPL-3.0-only

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <dl/bindgen.h>
#include <dl/callback.h>

typedef struct GptHeader GptHeader;
typedef struct GptPartitionEntry GptPartitionEntry;

static bool gpt_signature_valid(const uint8_t *sector);
static bool gpt_guid_is_zero(const uint8_t *guid);

#define GPT_SECTOR_SIZE 512
#define GPT_SIGNATURE_SIZE 8
#define GPT_ENTRY_SIZE 128
#define GPT_MAX_ENTRIES 128
#define GPT_MAX_ENTRY_SECTORS                                                  \
   ((GPT_MAX_ENTRIES * GPT_ENTRY_SIZE + GPT_SECTOR_SIZE - 1) / GPT_SECTOR_SIZE)
#define GPT_MAX_OFFSET 0x7FFFFFFF

struct __attribute__((packed)) GptHeader
{
   uint8_t signature[GPT_SIGNATURE_SIZE];
   uint32_t revision;
   uint32_t header_size;
   uint32_t header_crc32;
   uint32_t reserved;
   uint64_t current_lba;
   uint64_t backup_lba;
   uint64_t first_usable_lba;
   uint64_t last_usable_lba;
   uint8_t disk_guid[16];
   uint64_t partition_entry_lba;
   uint32_t num_partition_entries;
   uint32_t size_of_partition_entry;
   uint32_t partition_entry_array_crc32;
};

struct __attribute__((packed)) GptPartitionEntry
{
   uint8_t type_guid[16];
   uint8_t unique_guid[16];
   uint64_t first_lba;
   uint64_t last_lba;
   uint64_t attributes;
   uint16_t name[36];
};

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

static bool gpt_signature_valid(const uint8_t *sector)
{
   static const uint8_t signature[GPT_SIGNATURE_SIZE] = {'E', 'F', 'I', ' ',
                                                         'P', 'A', 'R', 'T'};

   for (int i = 0; i < GPT_SIGNATURE_SIZE; i++)
   {
      if (sector[i] != signature[i]) return false;
   }
   return true;
}

static bool gpt_guid_is_zero(const uint8_t *guid)
{
   for (int i = 0; i < 16; i++)
   {
      if (guid[i] != 0) return false;
   }
   return true;
}

DL_INC
bool GPT_Probe(int drive_id)
{
   if (drive_id < 0x80 || drive_id >= 0xE0) return false;

   uint8_t sector[GPT_SECTOR_SIZE];

   if (DISK_ReadLBA((uint8_t)drive_id, 1, 1, sector) != 0)
      return false;

   return gpt_signature_valid(sector);
}

DL_INC
int GPT_List(int drive_id, int **offsets_out)
{
   static int offsets[GPT_MAX_ENTRIES];
   static uint8_t entry_sectors[GPT_MAX_ENTRY_SECTORS * GPT_SECTOR_SIZE];
   uint8_t header_sector[GPT_SECTOR_SIZE];

   if (!offsets_out || drive_id < 0x80 || drive_id >= 0xE0) return -1;
   if (DISK_ReadLBA((uint8_t)drive_id, 1, 1, header_sector) != 0)
      return -1;

   if (!gpt_signature_valid(header_sector)) return -1;

   GptHeader *header = (GptHeader *)header_sector;
   if (header->size_of_partition_entry != GPT_ENTRY_SIZE) return -1;

   uint32_t entry_count = header->num_partition_entries;
   if (entry_count == 0)
   {
      *offsets_out = offsets;
      return 0;
   }

   if (entry_count > GPT_MAX_ENTRIES) entry_count = GPT_MAX_ENTRIES;

   uint64_t entry_bytes = (uint64_t)entry_count * GPT_ENTRY_SIZE;
   uint32_t entry_sectors_needed =
       (uint32_t)((entry_bytes + GPT_SECTOR_SIZE - 1) / GPT_SECTOR_SIZE);

   if (entry_sectors_needed > GPT_MAX_ENTRY_SECTORS) return -1;

   uint64_t entry_lba = header->partition_entry_lba;
   if (entry_lba > UINT64_MAX - entry_sectors_needed) return -1;

   if (DISK_ReadLBA((uint8_t)drive_id, entry_lba,
                    (uint16_t)entry_sectors_needed, entry_sectors) != 0)
      return -1;

   int count = 0;
   for (uint32_t i = 0; i < entry_count; i++)
   {
      GptPartitionEntry *entry =
          (GptPartitionEntry *)(entry_sectors + i * GPT_ENTRY_SIZE);

      if (gpt_guid_is_zero(entry->type_guid)) continue;
      if (entry->first_lba > GPT_MAX_OFFSET) continue;

      offsets[count++] = (int)entry->first_lba;
   }

   *offsets_out = offsets;
   return count;
}