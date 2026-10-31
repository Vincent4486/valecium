// SPDX-License-Identifier: GPL-3.0-only

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include <bits.h>
#include <status.h>

#include <dl/bindgen.h>
#include <dl/loader.h>

typedef struct Elf32Ehdr Elf32Ehdr;
typedef struct Elf32Shdr Elf32Shdr;
typedef struct Elf32Symbol Elf32Symbol;
typedef struct Elf32Rel Elf32Rel;
typedef struct Elf64Ehdr Elf64Ehdr;
typedef struct Elf64Shdr Elf64Shdr;
typedef struct Elf64Symbol Elf64Symbol;
typedef struct Elf64Rela Elf64Rela;

typedef struct DlHandle DlHandle;

/* ELF identification indices */
#define EI_MAG0 0
#define EI_MAG1 1
#define EI_MAG2 2
#define EI_MAG3 3
#define EI_CLASS 4
#define EI_DATA 5
#define EI_VERSION 6
#define EI_OSABI 7
#define EI_ABIVERSION 8

#define ELFMAG0 0x7f
#define ELFMAG1 'E'
#define ELFMAG2 'L'
#define ELFMAG3 'F'

#define ELFCLASS32 1
#define ELFCLASS64 2

/* Section types */
#define SHT_NULL 0
#define SHT_PROGBITS 1
#define SHT_SYMTAB 2
#define SHT_STRTAB 3
#define SHT_NOBITS 8
#define SHT_REL 9
#define SHT_DYNSYM 11

/* Relocation types */
#if defined(BIT32)
#define R_REL_GLOB_DAT 6
#define R_REL_JMP_SLOT 7
#define R_REL_RELATIVE 8
#elif defined(BIT64)
#define R_REL_GLOB_DAT 6
#define R_REL_JMP_SLOT 7
#define R_REL_RELATIVE 8
#endif

/* Symbol binding and type helpers */
#define ELF32_ST_BIND(i) ((i) >> 4)
#define ELF32_ST_TYPE(i) ((i) & 0xf)
#define ELF64_ST_BIND(i) ((i) >> 4)
#define ELF64_ST_TYPE(i) ((i) & 0xf)
#define STB_GLOBAL 1
#define STT_FUNC 2

#if defined(BIT32)
#define ElfEhdr Elf32Ehdr
#define ElfShdr Elf32Shdr
#define ElfSymbol Elf32Symbol
#define ELF_ST_BIND(i) ELF32_ST_BIND(i)
#define ELF_ST_TYPE(i) ELF32_ST_TYPE(i)
#define ELFCLASS_EXPECTED ELFCLASS32
#elif defined(BIT64)
#define ElfEhdr Elf64Ehdr
#define ElfShdr Elf64Shdr
#define ElfSymbol Elf64Symbol
#define ELF_ST_BIND(i) ELF64_ST_BIND(i)
#define ELF_ST_TYPE(i) ELF64_ST_TYPE(i)
#define ELFCLASS_EXPECTED ELFCLASS64
#endif

/* Opaque handle buffer size (must match DlHandle layout below) */
#define DL_HANDLE_SIZE 32

struct __attribute__((packed)) Elf32Ehdr
{
   unsigned char e_ident[16];
   uint16_t e_type;
   uint16_t e_machine;
   uint32_t e_version;
   uint32_t e_entry;
   uint32_t e_phoff;
   uint32_t e_shoff;
   uint32_t e_flags;
   uint16_t e_ehsize;
   uint16_t e_phentsize;
   uint16_t e_phnum;
   uint16_t e_shentsize;
   uint16_t e_shnum;
   uint16_t e_shstrndx;
};

struct __attribute__((packed)) Elf32Shdr
{
   uint32_t sh_name;
   uint32_t sh_type;
   uint32_t sh_flags;
   uint32_t sh_addr;
   uint32_t sh_offset;
   uint32_t sh_size;
   uint32_t sh_link;
   uint32_t sh_info;
   uint32_t sh_addralign;
   uint32_t sh_entsize;
};

struct __attribute__((packed)) Elf32Symbol
{
   uint32_t st_name;
   uint32_t st_value;
   uint32_t st_size;
   uint8_t st_info;
   uint8_t st_other;
   uint16_t st_shndx;
};

struct __attribute__((packed)) Elf32Rel
{
   uint32_t r_offset;
   uint32_t r_info;
};

struct __attribute__((packed)) Elf64Ehdr
{
   unsigned char e_ident[16];
   uint16_t e_type;
   uint16_t e_machine;
   uint32_t e_version;
   uint64_t e_entry;
   uint64_t e_phoff;
   uint64_t e_shoff;
   uint32_t e_flags;
   uint16_t e_ehsize;
   uint16_t e_phentsize;
   uint16_t e_phnum;
   uint16_t e_shentsize;
   uint16_t e_shnum;
   uint16_t e_shstrndx;
};

struct __attribute__((packed)) Elf64Shdr
{
   uint32_t sh_name;
   uint32_t sh_type;
   uint64_t sh_flags;
   uint64_t sh_addr;
   uint64_t sh_offset;
   uint64_t sh_size;
   uint32_t sh_link;
   uint32_t sh_info;
   uint64_t sh_addralign;
   uint64_t sh_entsize;
};

struct __attribute__((packed)) Elf64Symbol
{
   uint32_t st_name;
   uint8_t st_info;
   uint8_t st_other;
   uint16_t st_shndx;
   uint64_t st_value;
   uint64_t st_size;
};

struct __attribute__((packed)) Elf64Rela
{
   uint64_t r_offset;
   uint64_t r_info;
   int64_t r_addend;
};

struct DlHandle
{
   void *symtab;     /* pointer to symbol table in file data */
   void *strtab;     /* pointer to string table in file data */
   void *image_base; /* base address where the ELF was loaded */
   void *shdr;       /* pointer to section headers */
   int sym_count;    /* number of symbol entries */
   int shnum;        /* number of section headers */
   size_t file_size;
   size_t str_size;
};

/* Internal handle storage — one library at a time */
static DlHandle s_Handle = {0};

/* Convert a virtual address to a file-data pointer using the section headers. */
static bool file_range_valid(size_t file_size, uint64_t offset,
                             uint64_t size)
{
   return offset <= file_size && size <= file_size - (size_t)offset;
}

static void *vaddr_to_ptr(void *file_data, ElfShdr *shdr, int shnum,
                          size_t file_size, uintptr_t vaddr, size_t size)
{
   for (int i = 0; i < shnum; i++)
   {
      uintptr_t start = (uintptr_t)shdr[i].sh_addr;
      uintptr_t length = (uintptr_t)shdr[i].sh_size;
      if (start == 0 || vaddr < start || vaddr - start > length ||
          size > length - (vaddr - start) ||
          !file_range_valid(file_size, shdr[i].sh_offset + (vaddr - start),
                            size))
         continue;

      return (void *)((uintptr_t)file_data + shdr[i].sh_offset +
                      (vaddr - start));
   }
   return NULL;
}

_DL_FORCE_EXCLUDE
void *DL_LoadLibrary(void *file_data, size_t file_size, size_t capacity)
{
   if (!file_data || file_size < sizeof(ElfEhdr) || file_size > capacity ||
       capacity > UINTPTR_MAX - (uintptr_t)file_data)
      return NULL;

   unsigned char *ident = (unsigned char *)file_data;
   DlHandle *h = &s_Handle;

   /* Validate ELF magic and class */
   if (ident[EI_MAG0] != ELFMAG0 || ident[EI_MAG1] != ELFMAG1 ||
       ident[EI_MAG2] != ELFMAG2 || ident[EI_MAG3] != ELFMAG3 ||
       ident[EI_CLASS] != ELFCLASS_EXPECTED || ident[EI_DATA] != 1 ||
       ident[EI_VERSION] != 1)
      return NULL;

   h->symtab = NULL;
   h->strtab = NULL;
   h->image_base = file_data;
   h->sym_count = 0;

   ElfEhdr *ehdr = (ElfEhdr *)file_data;
   if (ehdr->e_shentsize != sizeof(ElfShdr) || ehdr->e_shnum == 0 ||
       !file_range_valid(file_size, ehdr->e_shoff,
                         (uint64_t)ehdr->e_shnum * ehdr->e_shentsize))
      return NULL;

   ElfShdr *shdr = (ElfShdr *)((uintptr_t)file_data + ehdr->e_shoff);

   h->shdr = shdr;
   h->shnum = ehdr->e_shnum;
   size_t image_size = file_size;
   for (int i = 0; i < ehdr->e_shnum; i++)
   {
      if (shdr[i].sh_size > UINTPTR_MAX - shdr[i].sh_addr)
         return NULL;
      if (shdr[i].sh_type == SHT_NOBITS)
      {
         if (image_size > capacity || shdr[i].sh_size > capacity - image_size)
            return NULL;
         image_size += shdr[i].sh_size;
      }
      else if (!file_range_valid(file_size, shdr[i].sh_offset,
                                 shdr[i].sh_size))
         return NULL;
   }

   for (int i = 0; i < ehdr->e_shnum; i++)
   {
      if (shdr[i].sh_type != SHT_NOBITS || shdr[i].sh_size == 0)
         continue;
      size_t offset = shdr[i].sh_offset;
      size_t size = shdr[i].sh_size;
      size_t gap = 0;
      for (int j = 0; j < ehdr->e_shnum; j++)
      {
         if (!(shdr[j].sh_flags & 2) ||
             shdr[j].sh_type == SHT_NOBITS || shdr[j].sh_size == 0)
            continue;
         uintptr_t bias = shdr[j].sh_addr - shdr[j].sh_offset;
         if (shdr[i].sh_addr < bias || shdr[i].sh_addr - bias < offset)
            return NULL;
         gap = shdr[i].sh_addr - bias - offset;
         break;
      }
      if (offset > file_size || gap > capacity - file_size ||
          size > capacity - file_size - gap || ehdr->e_shoff < offset)
         return NULL;
      size += gap;
      for (size_t j = file_size; j > offset; j--)
         ((uint8_t *)file_data)[j + size - 1] =
             ((uint8_t *)file_data)[j - 1];
      ehdr->e_shoff += size;
      shdr = (ElfShdr *)((uint8_t *)file_data + ehdr->e_shoff);
      for (int j = 0; j < ehdr->e_shnum; j++)
      {
         if (j != i && shdr[j].sh_offset >= offset)
            shdr[j].sh_offset += size;
      }
      shdr[i].sh_offset = offset + gap;
      for (size_t j = 0; j < size; j++)
         ((uint8_t *)file_data)[offset + j] = 0;
      file_size += size;
   }
   h->shdr = shdr;
   h->file_size = file_size;

   /* Find symbol table (prefer .dynsym for shared libraries). */
   for (int i = 0; i < ehdr->e_shnum; i++)
   {
      if (shdr[i].sh_type == SHT_DYNSYM)
      {
         if (shdr[i].sh_entsize != sizeof(ElfSymbol) ||
             shdr[i].sh_size % shdr[i].sh_entsize != 0 ||
             !file_range_valid(file_size, shdr[i].sh_offset,
                               shdr[i].sh_size) ||
             shdr[i].sh_link >= ehdr->e_shnum)
            return NULL;
         ElfShdr *str = &shdr[shdr[i].sh_link];
         if (str->sh_type != SHT_STRTAB ||
             !file_range_valid(file_size, str->sh_offset, str->sh_size))
            return NULL;
         h->symtab = (void *)((uintptr_t)file_data + shdr[i].sh_offset);
         h->sym_count = shdr[i].sh_size / shdr[i].sh_entsize;
         h->strtab = (void *)((uintptr_t)file_data + str->sh_offset);
         h->str_size = str->sh_size;
         break;
      }
   }

   /* If no .dynsym, fall back to .symtab. */
   if (!h->symtab)
   {
      for (int i = 0; i < ehdr->e_shnum; i++)
      {
         if (shdr[i].sh_type == SHT_SYMTAB)
         {
            if (shdr[i].sh_entsize != sizeof(ElfSymbol) ||
                shdr[i].sh_size % shdr[i].sh_entsize != 0 ||
                !file_range_valid(file_size, shdr[i].sh_offset,
                                  shdr[i].sh_size) ||
                shdr[i].sh_link >= ehdr->e_shnum)
               return NULL;
            ElfShdr *str = &shdr[shdr[i].sh_link];
            if (str->sh_type != SHT_STRTAB ||
                !file_range_valid(file_size, str->sh_offset, str->sh_size))
               return NULL;
            h->symtab = (void *)((uintptr_t)file_data + shdr[i].sh_offset);
            h->sym_count = shdr[i].sh_size / shdr[i].sh_entsize;
            h->strtab = (void *)((uintptr_t)file_data + str->sh_offset);
            h->str_size = str->sh_size;
            break;
         }
      }
   }

   if (!h->symtab || !h->strtab) return NULL;

   /* Process .rel.dyn and .rel.plt relocations to fix up GOT entries. */
   for (int i = 0; i < ehdr->e_shnum; i++)
   {
      if (shdr[i].sh_type != SHT_REL) continue;

#if defined(BIT32)
      size_t rel_size = sizeof(Elf32Rel);
#elif defined(BIT64)
      size_t rel_size = sizeof(Elf64Rela);
#endif
      if (shdr[i].sh_entsize != rel_size ||
          shdr[i].sh_size % rel_size != 0 ||
          !file_range_valid(file_size, shdr[i].sh_offset, shdr[i].sh_size))
         return NULL;

      /* Find the associated symbol table for this relocation section. */
      ElfSymbol *dynsym = NULL;
      int dynsym_count = 0;
      if (shdr[i].sh_link < (uint32_t)ehdr->e_shnum &&
          shdr[shdr[i].sh_link].sh_type == SHT_DYNSYM)
      {
         ElfShdr *sym = &shdr[shdr[i].sh_link];
         if (sym->sh_entsize != sizeof(ElfSymbol) ||
             sym->sh_size % sym->sh_entsize != 0 ||
             !file_range_valid(file_size, sym->sh_offset, sym->sh_size))
            return NULL;
         dynsym = (ElfSymbol *)((uintptr_t)file_data + sym->sh_offset);
         dynsym_count = sym->sh_size / sym->sh_entsize;
      }

#if defined(BIT32)
      Elf32Rel *rel = (Elf32Rel *)((uintptr_t)file_data + shdr[i].sh_offset);
      int rel_count = shdr[i].sh_size / sizeof(Elf32Rel);

      for (int j = 0; j < rel_count; j++)
      {
         uint32_t r_type = rel[j].r_info & 0xFF;
         uint32_t r_sym = rel[j].r_info >> 8;
         void *patch_addr = vaddr_to_ptr(file_data, shdr, ehdr->e_shnum, file_size,
                                         rel[j].r_offset, sizeof(uint32_t));
         if (!patch_addr) continue;

         if (r_type == R_REL_GLOB_DAT || r_type == R_REL_JMP_SLOT)
         {
            if (dynsym && r_sym < (uint32_t)dynsym_count)
            {
               void *sym_ptr = vaddr_to_ptr(file_data, shdr, ehdr->e_shnum, file_size,
                                             dynsym[r_sym].st_value, 1);
               if (sym_ptr)
                  *(uint32_t *)patch_addr = (uint32_t)(uintptr_t)sym_ptr;
            }
         }
         else if (r_type == R_REL_RELATIVE)
         {
            void *target = vaddr_to_ptr(file_data, shdr, ehdr->e_shnum, file_size,
                                        *(uint32_t *)patch_addr, 1);
            if (target)
               *(uint32_t *)patch_addr = (uint32_t)(uintptr_t)target;
         }
      }
#elif defined(BIT64)
      Elf64Rela *rel = (Elf64Rela *)((uintptr_t)file_data + shdr[i].sh_offset);
      int rel_count = shdr[i].sh_size / sizeof(Elf64Rela);

      for (int j = 0; j < rel_count; j++)
      {
         uint32_t r_type = rel[j].r_info & 0xFF;
         uint32_t r_sym = rel[j].r_info >> 8;
         void *patch_addr = vaddr_to_ptr(file_data, shdr, ehdr->e_shnum, file_size,
                                         (uintptr_t)rel[j].r_offset,
                                         sizeof(uint64_t));
         if (!patch_addr) continue;

         if (r_type == R_REL_GLOB_DAT || r_type == R_REL_JMP_SLOT)
         {
            if (dynsym && r_sym < (uint32_t)dynsym_count)
            {
               void *sym_ptr = vaddr_to_ptr(file_data, shdr, ehdr->e_shnum, file_size,
                                             (uintptr_t)dynsym[r_sym].st_value,
                                             1);
               if (sym_ptr)
                  *(uint64_t *)patch_addr = (uint64_t)(uintptr_t)sym_ptr;
            }
         }
         else if (r_type == R_REL_RELATIVE)
         {
            void *target = vaddr_to_ptr(file_data, shdr, ehdr->e_shnum, file_size,
                                        (uintptr_t)rel[j].r_addend, 1);
            if (target)
               *(uint64_t *)patch_addr = (uint64_t)(uintptr_t)target;
         }
      }
#endif
   }

   return h;
}

_DL_FORCE_EXCLUDE
void *DL_LoadSymbol(void *handle, const char *symbol)
{
   DlHandle *h = (DlHandle *)handle;

   if (!h || !h->symtab || !h->strtab || !symbol) return NULL;

   ElfSymbol *sym = (ElfSymbol *)h->symtab;
   char *strtab = (char *)h->strtab;

   for (int i = 0; i < h->sym_count; i++)
   {
      if (sym[i].st_name >= h->str_size)
         continue;
      char *name = strtab + sym[i].st_name;
      if (name[0] == '\0') continue;

      size_t j = 0;
      size_t remaining = h->str_size - sym[i].st_name;
      while (j < remaining && name[j] == symbol[j] && name[j] != '\0')
         j++;
      if (j < remaining && name[j] == '\0' && symbol[j] == '\0')
      {
         return vaddr_to_ptr(h->image_base, (ElfShdr *)h->shdr,
                             h->shnum, h->file_size, sym[i].st_value, 1);
      }
   }

   return NULL;
}