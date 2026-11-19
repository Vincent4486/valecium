// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <stdint.h>

#include <colors.h>

typedef struct DL_CallbackOperations DL_CallbackOperations;
typedef struct DL_CallbackOpsPatch DL_CallbackOpsPatch;

struct __attribute__((packed)) DL_CallbackOperations
{
   int (*DISK_Read)(uint8_t drive, uint16_t cylinder, uint8_t sector,
                    uint8_t head, uint8_t count, void *buffer);
   int (*DISK_ReadLBA)(uint8_t drive, uint64_t lba, uint16_t count,
                       void *buffer);
   int (*Open)(const char *path);
   int (*Read)(int fd, void *buffer, int count);
   int (*Close)(int fd);
   int (*Video_PutChar)(char c, int x, int y, Video_Color color);
   int (*Video_PutPixel)(Video_Color color, int x, int y);
   uint32_t (*Video_GetWidth)(void);
   uint32_t (*Video_GetHeight)(void);
   void (*Video_ClearScreen)(Video_Color color);
   void (*logfmt)(int log_type, const char *fmt, ...)
       __attribute__((format(printf, 2, 3)));
};

struct __attribute__((packed)) DL_CallbackOpsPatch
{
   char _signature[4];
   DL_CallbackOperations *dl_callback_ops;
};

#if !defined CORE || !defined COREFS
extern DL_CallbackOpsPatch g_DlCallbackOpsPatch;

#define g_DlCallbackOps g_DlCallbackOpsPatch.dl_callback_ops
#endif
