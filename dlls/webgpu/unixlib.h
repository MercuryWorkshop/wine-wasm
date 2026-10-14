/*
 * webgpu.dll's Unix side: the gex WebGPU wire's transport.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef __WINE_WEBGPU_UNIXLIB_H
#define __WINE_WEBGPU_UNIXLIB_H

#include "wine/unixlib.h"

/* Sends a message of wire commands; the reply is copied to "reply" if it
 * fits, and its size is returned either way (get_reply then copies it). */
struct webgpu_send_params
{
    const void *data;
    UINT32 size;
    void *reply;
    UINT32 reply_capacity;
    UINT32 reply_size;
};

enum webgpu_funcs
{
    unix_send,
    unix_get_reply,
    unix_funcs_count,
};

#endif
