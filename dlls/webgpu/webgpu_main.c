/*
 * webgpu.dll: webgpu.h for Windows programs, as the gex WebGPU wire's
 * client (wire-client.c); the Unix side makes the calls on a real webgpu.h.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <stdarg.h>
#include <stdlib.h>

#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#include "unixlib.h"

extern const void *(*gexgpu_client_transport)(void *context, const void *data, size_t size, size_t *reply_size);
void gexgpu_client_init_transport(void);

static void *reply_buffer;
static UINT32 reply_capacity;

static const void *transport(void *context, const void *data, size_t size, size_t *reply_size)
{
    struct webgpu_send_params params = {data, size, reply_buffer, reply_capacity, 0};

    if (WINE_UNIX_CALL(unix_send, &params))
    {
        *reply_size = 0;
        return NULL;
    }
    if (params.reply_size > reply_capacity)
    {
        UINT32 capacity = reply_capacity ? reply_capacity : 65536;

        while (capacity < params.reply_size)
            capacity *= 2;
        free(reply_buffer);
        if (!(reply_buffer = malloc(capacity)))
        {
            reply_capacity = 0;
            *reply_size = 0;
            return NULL;
        }
        reply_capacity = capacity;
        params.reply = reply_buffer;
        params.reply_capacity = reply_capacity;
        WINE_UNIX_CALL(unix_get_reply, &params);
    }
    *reply_size = params.reply_size;
    return reply_buffer;
}

void gexgpu_client_init_transport(void)
{
    gexgpu_client_transport = transport;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void *reserved)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(instance);
        return !__wine_init_unix_call();
    }
    return TRUE;
}
