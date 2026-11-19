// SPDX-License-Identifier: GPL-3.0-only

// Simplest implementation: decode terminal escape sequences byte by byte.
// 1. Keep a buffer of at most 6 bytes plus a count. A normal byte is
//    returned immediately as its ASCII key.
// 2. On ESC, collect the following bytes until they match a known sequence
//    or can no longer match one. A small table covers the common cases:
//    ESC [ A/B/C/D = up/down/right/left, ESC [ H/F = home/end,
//    ESC [ 2~/3~/5~/6~ = insert/delete/pageup/pagedown, ESC O P..S = F1-F4.
// 3. On a full match return the mapped KB_KEY_*; if the bytes can never
//    match, return them as literal characters; otherwise wait for more.
// uart.c uses this, and any backend that receives a character stream can.
