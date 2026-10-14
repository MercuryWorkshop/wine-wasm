/*
 * webgpu.dll's Unix side: the gex WebGPU wire's server, on the host's
 * webgpu.h (Dawn, linked in through DAWN_LIBS).
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#if 0
#pragma makedep unix
#endif

#include <pthread.h>
#include <stdarg.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winternl.h"
#include "unixlib.h"

#include "wire.h"
#include "wire-server.h"

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

static void *server_main(void *arg)
{
    struct gexgpu_procs procs;

    GEXGPU_PROCS_INIT_DIRECT(&procs);
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

static NTSTATUS send_message(const void *data, UINT32 size, void *reply, UINT32 reply_capacity, UINT32 *reply_size)
{
    pthread_mutex_lock(&lock);
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
