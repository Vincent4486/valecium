// SPDX-License-Identifier: GPL-3.0-only

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <logging.h>
#include <status.h>

#include <dl/bindgen.h>
#include <dl/callback.h>

#include "kb.h"

DL_INC
int KB_Initialize(void)
{
    g_DlCallbackOps->logfmt(LOG_FATAL, "Not Implemented");
    return -ENOENT;
}

DL_INC
int KB_RegisterCallback(
    KB_Callback callback, 
    KB_KeyCombination combination, 
    void *userdata
)
{
    g_DlCallbackOps->logfmt(LOG_FATAL, "Not Implemented");
    return -ENOENT;
}

DL_INC
int KB_UnregisterCallback(int handle)
{
    g_DlCallbackOps->logfmt(LOG_FATAL, "Not Implemented");
    return -ENOENT;
}
