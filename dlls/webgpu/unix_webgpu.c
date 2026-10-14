/*
 * webgpu.dll's Unix side: the gex WebGPU wire's server, on the host's
 * webgpu.h (Dawn, linked in through DAWN_LIBS), or, in a gex guest, the
 * wire handed on to the host's server through /dev/gex-gpu
 * (transport-dev.h).
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winternl.h"
#include "unixlib.h"

#include "wire.h"
#include "wire-server.h"
#include "transport-dev.h"

/* Messages are handled on a thread of their own: Dawn keeps some state per
 * thread (error scopes), and the wire's client batches calls from all the
 * process's threads, so they must all reach Dawn from one. */
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t request_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t request_cond = PTHREAD_COND_INITIALIZER;
static pthread_t server_thread;
static struct gexgpu_server *server;
static const void *last_reply;
static size_t last_reply_size;

static struct
{
    const void *data;
    size_t size;
    BOOL pending, done;
} request;

/* gex's device, when the process has one: -1 before the first message,
 * -2 when there is none. */
static int device = -1;

static void *server_main(void *arg)
{
    struct gexgpu_procs procs;

#ifdef HAVE_DAWN
    GEXGPU_PROCS_INIT_DIRECT(&procs);
#else
    memset(&procs, 0, sizeof(procs));
    return NULL;
#endif
    server = gexgpu_server_create(&procs);

    pthread_mutex_lock(&request_lock);
    for (;;)
    {
        while (!request.pending)
            pthread_cond_wait(&request_cond, &request_lock);
        request.pending = FALSE;
        pthread_mutex_unlock(&request_lock);

        gexgpu_server_handle(server, request.data, request.size, &last_reply, &last_reply_size);

        pthread_mutex_lock(&request_lock);
        request.done = TRUE;
        pthread_cond_broadcast(&request_cond);
    }
    return NULL;
}

static int device_call(unsigned long request, const void *data, UINT32 size, void *reply,
        UINT32 reply_capacity, UINT32 *reply_size)
{
    struct gex_gpu_submit s = {0};

    s.data = (uintptr_t)data;
    s.size = size;
    s.reply = (uintptr_t)reply;
    s.reply_capacity = reply_capacity;
    s.version = GEXGPU_WIRE_VERSION;
    if (ioctl(device, request, &s) < 0)
        return -1;
    *reply_size = s.reply_size;
    return s.status;
}

static NTSTATUS send_message(const void *data, UINT32 size, void *reply, UINT32 reply_capacity, UINT32 *reply_size)
{
    pthread_mutex_lock(&lock);
    if (device == -1 && (device = open("/dev/gex-gpu", O_RDWR | O_CLOEXEC)) < 0)
        device = -2;
    if (device >= 0)
    {
        int status = device_call(GEX_GPU_SUBMIT, data, size, reply, reply_capacity, reply_size);

        if (status < 0)
        {
            pthread_mutex_unlock(&lock);
            return STATUS_UNSUCCESSFUL;
        }
        /* Held by the device, and the lock by us, for get_reply. */
        if (status != GEX_GPU_STATUS_HELD)
            pthread_mutex_unlock(&lock);
        return STATUS_SUCCESS;
    }
#ifndef HAVE_DAWN
    pthread_mutex_unlock(&lock);
    return STATUS_NOT_SUPPORTED;
#endif
    if (!server_thread && pthread_create(&server_thread, NULL, server_main, NULL))
    {
        pthread_mutex_unlock(&lock);
        return STATUS_UNSUCCESSFUL;
    }

    pthread_mutex_lock(&request_lock);
    request.data = data;
    request.size = size;
    request.done = FALSE;
    request.pending = TRUE;
    pthread_cond_broadcast(&request_cond);
    while (!request.done)
        pthread_cond_wait(&request_cond, &request_lock);
    pthread_mutex_unlock(&request_lock);

    *reply_size = last_reply_size;
    /* A reply too big for the caller's buffer is held, with the lock, for
     * the get_reply call that follows at once. */
    if (last_reply_size > reply_capacity)
        return STATUS_SUCCESS;
    memcpy(reply, last_reply, last_reply_size);
    pthread_mutex_unlock(&lock);
    return STATUS_SUCCESS;
}

static NTSTATUS get_reply(void *reply, UINT32 reply_capacity)
{
    if (device >= 0)
    {
        UINT32 size;
        int status = device_call(GEX_GPU_FETCH, NULL, 0, reply, reply_capacity, &size);

        pthread_mutex_unlock(&lock);
        return status == GEX_GPU_STATUS_OK || status == GEX_GPU_STATUS_MALFORMED ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
    }
    if (last_reply_size <= reply_capacity)
        memcpy(reply, last_reply, last_reply_size);
    pthread_mutex_unlock(&lock);
    return STATUS_SUCCESS;
}

static NTSTATUS webgpu_send(void *args)
{
    struct webgpu_send_params *params = args;

    return send_message(params->data, params->size, params->reply, params->reply_capacity, &params->reply_size);
}

static NTSTATUS webgpu_get_reply(void *args)
{
    struct webgpu_send_params *params = args;

    return get_reply(params->reply, params->reply_capacity);
}

const unixlib_entry_t __wine_unix_call_funcs[] =
{
    webgpu_send,
    webgpu_get_reply,
};

C_ASSERT(ARRAYSIZE(__wine_unix_call_funcs) == unix_funcs_count);

#ifdef _WIN64

struct webgpu_send_params32
{
    ULONG data;
    UINT32 size;
    ULONG reply;
    UINT32 reply_capacity;
    UINT32 reply_size;
};

static NTSTATUS wow64_webgpu_send(void *args)
{
    struct webgpu_send_params32 *params = args;

    return send_message(ULongToPtr(params->data), params->size, ULongToPtr(params->reply),
            params->reply_capacity, &params->reply_size);
}

static NTSTATUS wow64_webgpu_get_reply(void *args)
{
    struct webgpu_send_params32 *params = args;

    return get_reply(ULongToPtr(params->reply), params->reply_capacity);
}

const unixlib_entry_t __wine_unix_call_wow64_funcs[] =
{
    wow64_webgpu_send,
    wow64_webgpu_get_reply,
};

C_ASSERT(ARRAYSIZE(__wine_unix_call_wow64_funcs) == unix_funcs_count);

#endif
