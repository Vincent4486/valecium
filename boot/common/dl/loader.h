// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <stddef.h>

void *DL_LoadLibrary(void *file_data, size_t file_size, size_t capacity);
void *DL_LoadSymbol(void *handle, const char *symbol);
