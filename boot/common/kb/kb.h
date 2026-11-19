// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <stdint.h>

#define KB_MOD_NONE    0x0000
#define KB_MOD_LCTRL   0x0001
#define KB_MOD_RCTRL   0x0002
#define KB_MOD_LSHIFT  0x0004
#define KB_MOD_RSHIFT  0x0008
#define KB_MOD_LALT    0x0010
#define KB_MOD_RALT    0x0020
#define KB_MOD_LSUPER  0x0040
#define KB_MOD_RSUPER  0x0080
#define KB_MOD_CAPS    0x0100
#define KB_MOD_NUM     0x0200

#define KB_MODE_PRESS       0x00
#define KB_MODE_RELEASE     0x01
#define KB_MODE_CLICK       0x02

#define KB_KEY_NONE       0x00
#define KB_KEY_A          0x01
#define KB_KEY_B          0x02
#define KB_KEY_C          0x03
#define KB_KEY_D          0x04
#define KB_KEY_E          0x05
#define KB_KEY_F          0x06
#define KB_KEY_G          0x07
#define KB_KEY_H          0x08
#define KB_KEY_I          0x09
#define KB_KEY_J          0x0A
#define KB_KEY_K          0x0B
#define KB_KEY_L          0x0C
#define KB_KEY_M          0x0D
#define KB_KEY_N          0x0E
#define KB_KEY_O          0x0F
#define KB_KEY_P          0x10
#define KB_KEY_Q          0x11
#define KB_KEY_R          0x12
#define KB_KEY_S          0x13
#define KB_KEY_T          0x14
#define KB_KEY_U          0x15
#define KB_KEY_V          0x16
#define KB_KEY_W          0x17
#define KB_KEY_X          0x18
#define KB_KEY_Y          0x19
#define KB_KEY_Z          0x1A

#define KB_KEY_0          0x1B
#define KB_KEY_1          0x1C
#define KB_KEY_2          0x1D
#define KB_KEY_3          0x1E
#define KB_KEY_4          0x1F
#define KB_KEY_5          0x20
#define KB_KEY_6          0x21
#define KB_KEY_7          0x22
#define KB_KEY_8          0x23
#define KB_KEY_9          0x24

#define KB_KEY_ENTER      0x25
#define KB_KEY_ESCAPE     0x26
#define KB_KEY_BACKSPACE  0x27
#define KB_KEY_TAB        0x28
#define KB_KEY_SPACE      0x29
#define KB_KEY_MINUS      0x2A
#define KB_KEY_EQUAL      0x2B
#define KB_KEY_LBRACKET   0x2C
#define KB_KEY_RBRACKET   0x2D
#define KB_KEY_BACKSLASH  0x2E
#define KB_KEY_SEMICOLON  0x2F
#define KB_KEY_APOSTROPHE 0x30
#define KB_KEY_GRAVE      0x31
#define KB_KEY_COMMA      0x32
#define KB_KEY_PERIOD     0x33
#define KB_KEY_SLASH      0x34

#define KB_KEY_CAPSLOCK   0x35
#define KB_KEY_F1          0x36
#define KB_KEY_F2          0x37
#define KB_KEY_F3          0x38
#define KB_KEY_F4          0x39
#define KB_KEY_F5          0x3A
#define KB_KEY_F6          0x3B
#define KB_KEY_F7          0x3C
#define KB_KEY_F8          0x3D
#define KB_KEY_F9          0x3E
#define KB_KEY_F10         0x3F
#define KB_KEY_F11         0x40
#define KB_KEY_F12         0x41

#define KB_KEY_PRINTSCREEN 0x42
#define KB_KEY_SCROLLLOCK  0x43
#define KB_KEY_PAUSE       0x44
#define KB_KEY_INSERT      0x45
#define KB_KEY_HOME        0x46
#define KB_KEY_PAGEUP      0x47
#define KB_KEY_DELETE      0x48
#define KB_KEY_END         0x49
#define KB_KEY_PAGEDOWN    0x4A
#define KB_KEY_RIGHT       0x4B
#define KB_KEY_LEFT        0x4C
#define KB_KEY_DOWN        0x4D
#define KB_KEY_UP          0x4E

#define KB_KEY_NUMLOCK     0x4F
#define KB_KEY_KP_DIVIDE   0x50
#define KB_KEY_KP_MULTIPLY 0x51
#define KB_KEY_KP_MINUS    0x52
#define KB_KEY_KP_PLUS     0x53
#define KB_KEY_KP_ENTER    0x54
#define KB_KEY_KP_1        0x55
#define KB_KEY_KP_2        0x56
#define KB_KEY_KP_3        0x57
#define KB_KEY_KP_4        0x58
#define KB_KEY_KP_5        0x59
#define KB_KEY_KP_6        0x5A
#define KB_KEY_KP_7        0x5B
#define KB_KEY_KP_8        0x5C
#define KB_KEY_KP_9        0x5D
#define KB_KEY_KP_0        0x5E
#define KB_KEY_KP_PERIOD   0x5F

typedef uint16_t KB_Key;
typedef uint16_t KB_Modifiers;
typedef uint8_t KB_Mode;

typedef struct {
    KB_Key key;
    KB_Modifiers modifiers;
    KB_Mode mode;
} KB_KeyCombination;

typedef struct {
    KB_Key key;
    KB_Modifiers modifiers;
    KB_Mode mode;
} KB_Event;

typedef void (*KB_Callback)(const KB_Event *event, void *userdata);

int KB_Initialize(void);
int KB_RegisterCallback(
    KB_Callback callback, 
    KB_KeyCombination combination, 
    void *userdata
);
int KB_UnregisterCallback(int handle);
const char *KB_GetDefaultKB();
