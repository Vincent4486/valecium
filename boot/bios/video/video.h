// SPDX-License-Identifier: GPL-3.0-only

#pragma once
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include <colors.h>
#include <status.h>

/* Output system identifiers (availability bit positions). */
#define OUTPUT_VBE 0
#define OUTPUT_VGA 1
#define OUTPUT_VGATEXT 2
#define OUTPUT_UART 3

#define TEXT_DEFAULT_COLOR ((Video_Color){85, 255, 85}) /* light green */

/* VBE framebuffer details (populated from boot-time mode selection). */
typedef struct VBE_Info
{
   uint64_t framebuffer_addr;
   uint32_t pitch;
   uint32_t width;
   uint32_t height;
   uint8_t bpp;
   uint8_t red_field_position;
   uint8_t red_mask_size;
   uint8_t green_field_position;
   uint8_t green_mask_size;
   uint8_t blue_field_position;
   uint8_t blue_mask_size;
} VBE_Info;

extern void outb(uint16_t port, uint8_t val); /* Write port byte. */
extern uint8_t inb(uint16_t port);            /* Read port byte. */

extern int g_PreferredOutput; /* Preferred output system. */

int Video_Initialize(void); /* Initialize active driver. */
int Video_PutChar(char c, int x, int y,
                  Video_Color color); /* Put char via active driver. */
int Video_PutPixel(Video_Color color, int x,
                   int y);                 /* Put pixel via active driver. */
uint32_t Video_GetWidth(void);             /* Get width of active driver. */
uint32_t Video_GetHeight(void);            /* Get height of active driver. */
void Video_ClearScreen(Video_Color color); /* Clear screen of active driver. */

int VGATEXT_Initialize(void); /* Initialize VGA text mode. */
int VGATEXT_PutChar(char c, int x, int y,
                    Video_Color color); /* Put text char. */
int VGATEXT_PutPixel(Video_Color color, int x,
                     int y); /* Put pixel as block char + colour attribute. */
uint32_t VGATEXT_GetWidth(void);             /* Get VGA text width (80). */
uint32_t VGATEXT_GetHeight(void);            /* Get VGA text height (25). */
void VGATEXT_ClearScreen(Video_Color color); /* Clears the VGA text screen */

int UART_Initialize(void); /* Initialize COM1 UART. */
int UART_PutChar(
    char c, int x, int y,
    Video_Color color); /* Put char via ANSI codes (cursor + colour). */
int UART_PutPixel(Video_Color color, int x,
                  int y);      /* Put pixel via ANSI 256-colour background. */
uint32_t UART_GetWidth(void);  /* Get terminal width via VT100 CPR. */
uint32_t UART_GetHeight(void); /* Get terminal height via VT100 CPR. */
void UART_ClearScreen(Video_Color color); /* Clear terminal via VT100 ESC[2J. */

int VBE_Initialize(void); /* Initialize VBE framebuffer. */
int VBE_PutChar(char c, int x, int y, Video_Color color); /* Put VBE char. */
int VBE_PutPixel(Video_Color color, int x, int y);        /* Put VBE pixel. */
void VBE_SetInfo(const VBE_Info *info);  /* Set VBE info before init. */
int VBE_HasInfo(void);                   /* Check if VBE info is available. */
uint32_t VBE_GetWidth(void);             /* Get current VBE width. */
uint32_t VBE_GetHeight(void);            /* Get current VBE height. */
void VBE_ClearScreen(Video_Color color); /* Clears the current VBE screen */

int VGA_Initialize(void); /* Initialize VGA graphics. */
int VGA_PutChar(char c, int x, int y, Video_Color color); /* Put VGA char. */
int VGA_PutPixel(Video_Color color, int x, int y);        /* Put VGA pixel. */
uint32_t VGA_GetWidth(void);             /* Get VGA width (320). */
uint32_t VGA_GetHeight(void);            /* Get VGA height (200). */
void VGA_ClearScreen(Video_Color color); /* Clears the VGA screen */

void putc(char c);          /* Write a single character. */
void puts(const char *str); /* Write a null-terminated string. */
void printf(const char *fmt, ...) 
    __attribute__((format(printf, 1, 2)));
void vprintf(const char *fmt, va_list args)
    __attribute__((format(printf, 1, 0)));
void puti(int val);                          /* Write signed int as decimal. */
void putd(int val);                          /* Write signed int as decimal. */
void putl(long val);                         /* Write signed long as decimal. */
void putll(long long val);           /* Write signed long long as decimal. */
void putu(unsigned val);             /* Write unsigned int as decimal. */
void putul(unsigned long val);       /* Write unsigned long as decimal. */
void putull(unsigned long long val); /* Write unsigned long long as decimal. */
void putx(unsigned long long val);   /* Write unsigned long long as hex. */
void putX(unsigned long long val);   /* Write unsigned long long as HEX. */
void puto(unsigned long long val);   /* Write unsigned long long as octal. */
void putb(unsigned long long val);   /* Write unsigned long long as binary. */
void putp(const void *ptr);          /* Write pointer as 0x hex. */

/* Log messages controlled. */
void logfmt(int log_type, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
