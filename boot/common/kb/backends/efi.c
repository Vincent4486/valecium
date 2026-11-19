// SPDX-License-Identifier: GPL-3.0-only

// Simplest implementation: use the firmware input protocol.
// 1. Take the EFI System Table passed into stage2 and get ConIn, the
//    EFI_SIMPLE_TEXT_INPUT_PROTOCOL.
// 2. Wait on ConIn->WaitForKey, then call ConIn->ReadKeyStroke to get one
//    EFI_INPUT_KEY, which carries a ScanCode and a UnicodeChar.
// 3. Map ScanCode (up, down, home, F1..F10, and so on) to KB_KEY_* and keep
//    UnicodeChar for printable keys.
// Simple Text Input reports no modifier state; use the Ex variant of the
// protocol if shift, ctrl and alt have to be seen.
