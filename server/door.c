/*
 * wasm32 (gex): door calls into the wineserver.
 *
 * The server is its own gex process. Its memory (the server memory) is
 * runtime-owned and every Worker of the session can instantiate this
 * module on it (gex docs/wineserver-kernel.md). A client thread then runs
 * a request itself: it takes the door lock, the gex host copies the
 * request into the door buffer, calls __wine_door_dispatch on the client's
 * own instance of this module, and copies the reply out. No pipe, no wake.
 *
 * The server's main thread (the pump) keeps Wine's main loop for what is
 * not a request (timeouts, fd events). It holds the door lock except while
 * it waits in epoll_wait. A system call made by server code inside a door
 * is either answered by the host in place (the clock, getpid) or proxied:
 * posted here, and made by the pump as this process, which serves proxies
 * without the lock from both of its waits (the kick pipe ends epoll_wait;
 * a post notifies the lock word).
 *
 * Copyright 2026 the gex authors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifdef __wasm32__

#include "config.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "winternl.h"

#include "file.h"
#include "thread.h"
#include "request.h"

/* The area the gex host reads, at the address gex_door_publish gives it.
 * Offsets are the host's ABI (the gex host's door glue): append only. */
struct door_area
{
    int lock;              /*  0: 0 free, 1 held, 2 held with waiters */
    int proxy_state;       /*  4: 0 idle, 1 posted, 2 done */
    int proxy_nr;          /*  8 */
    int proxy_args[6];     /* 12 */
    int proxy_ret;         /* 36 */
    int version;           /* 40 */
    unsigned int buf;      /* 44: the door buffer */
    unsigned int buf_size; /* 48 */
    unsigned int stack_top;/* 52: the door stack (one: the lock admits one caller) */
    unsigned int tp;       /* 56: the door's thread pointer (a struct pthread copy) */
    unsigned int dispatch; /* 60: table index of __wine_door_dispatch */
    unsigned int ready;    /* 64: table index of door_instance_ready */
    int kick_fd;           /* 68: the kick pipe's write end, in this process */
    unsigned int ctx;      /* 72: a register context for door instances (33 x 8 bytes) */
    unsigned int frame;    /* 76: the frame __gex_thread_start reads (func, arg, tp, 0) */
    int kick_needed;       /* 80: set by a dispatch: the pump must look again */
    int pump_sleeping;     /* 84: the pump is in (or entering) epoll_wait */
    long long pump_wake;   /* 88: monotonic time the pump's epoll_wait ends at, -1 never */
    unsigned int stats;    /* 96: door_stats */
    int proxies;           /* 100: syscalls proxied, all time */
    int calls;             /* 104: door calls served, all time */
    unsigned int thread_start; /* 108: table index of __gex_thread_start */
    int pid;               /* 112: this process's pid */
    int owner;             /* 116: who holds the lock: a client's tid, or -1 the pump (diagnostics) */
};

#define DOOR_VERSION 1
#define DOOR_BUF_SIZE (1024 * 1024 + 4096)
#define DOOR_STACK_SIZE (256 * 1024)

static struct door_area door_area_storage = { .lock = 1 /* the pump holds it until its first wait */ };
struct door_area *door_area;

static int kick_rfd = -1;

/* Imports from the gex host */
__attribute__((import_module("gex"), import_name("door_publish")))
extern void gex_door_publish( struct door_area *area );

void __gex_set_tp( unsigned long );
/* musl's thread entry (gex_thread.s), with gex's F_body signature: a door
 * instance starts through it */
int __gex_thread_start( int ctx, int depth, int need, long long ret );

int wineserver_no_fork(void)
{
    errno = ENOSYS;
    return -1;
}

/* Tell whoever started the server (ntdll's start_server) that it is up:
 * a byte on the descriptor named by WINEGEX_SERVER_READY. */
void wineserver_ready(void)
{
    const char *fd_str = getenv( "WINEGEX_SERVER_READY" );
    if (fd_str)
    {
        int fd = atoi( fd_str );
        char c = 1;
        write( fd, &c, 1 );
        close( fd );
        unsetenv( "WINEGEX_SERVER_READY" );
    }
}

/* A door instance's first call (gex host: __gex_thread_start on the area's
 * frame): it sets the instance's context, thread pointer and stack, then
 * the host ends it at the exit system call. */
static int door_instance_ready( void *arg )
{
    return 0;
}

unsigned int __wine_door_dispatch( int unix_pid, int unix_tid, long long now, long long boot );

/* proxied system calls, by number, for the census (WINEGEX_DOOR_STATS) */
static int door_stats[512];

void door_init(void)
{
    struct door_area *a = &door_area_storage;
    int fds[2];
    unsigned int *frame;
    unsigned long long *ctx;
    void *self_copy;
    char *stack;

    if (getenv( "WINEGEX_DOOR" ) && !strcmp( getenv( "WINEGEX_DOOR" ), "0" )) return;
    if (pipe2( fds, O_CLOEXEC | O_NONBLOCK ) == -1) return;
    if (!(a->buf = (unsigned int)(unsigned long)malloc( DOOR_BUF_SIZE ))) return;
    if (!(stack = malloc( DOOR_STACK_SIZE ))) return;
    /* the door's own struct pthread: errno and the like, apart from the
     * pump's (which may be inside a system call while a door runs) */
    if (!(self_copy = malloc( 4096 ))) return;
    memcpy( self_copy, (void *)pthread_self(), 512 );
    if (!(ctx = calloc( 34, 8 ))) return;

    kick_rfd = fds[0];
    a->kick_fd = fds[1];
    a->buf_size = DOOR_BUF_SIZE;
    /* the frame __gex_thread_start reads at the context's ESP: (func, arg,
     * thread pointer, no fresh TLS), at the top of the door stack */
    frame = (unsigned int *)((((unsigned long)stack + DOOR_STACK_SIZE) & ~15ul) - 32);
    a->tp = (unsigned int)(unsigned long)self_copy;
    a->dispatch = (unsigned int)(unsigned long)__wine_door_dispatch;
    a->ready = (unsigned int)(unsigned long)door_instance_ready;
    frame[0] = a->ready;
    frame[1] = 0;
    frame[2] = a->tp;
    frame[3] = 0;
    a->frame = (unsigned int)(unsigned long)frame;
    a->stack_top = a->frame - 16;
    a->ctx = (unsigned int)(unsigned long)ctx;
    ctx[4] = a->frame;  /* ESP */
    a->stats = (unsigned int)(unsigned long)door_stats;
    a->thread_start = (unsigned int)(unsigned long)__gex_thread_start;
    a->pid = getpid();
    a->pump_wake = -1;
    a->version = DOOR_VERSION;
    door_area = a;
    gex_door_publish( a );
}

/* door calls served so far (the pump: did any run while it waited?) */
unsigned int door_calls(void)
{
    return door_area ? __atomic_load_n( (unsigned int *)&door_area->calls, __ATOMIC_ACQUIRE ) : 0;
}

/* the kick pipe's read end, for the pump's epoll set (-1 without doors) */
int door_kick_fd(void)
{
    return kick_rfd;
}

static void door_serve_proxy(void)
{
    struct door_area *a = door_area;
    long r;

    if (__atomic_load_n( &a->proxy_state, __ATOMIC_ACQUIRE ) != 1) return;
    r = syscall( a->proxy_nr, a->proxy_args[0], a->proxy_args[1], a->proxy_args[2],
                 a->proxy_args[3], a->proxy_args[4], a->proxy_args[5] );
    if (r == -1) r = -errno;
    a->proxy_ret = r;
    __atomic_store_n( &a->proxy_state, 2, __ATOMIC_RELEASE );
    __builtin_wasm_memory_atomic_notify( &a->proxy_state, -1 );
}

/* drain the kick pipe and serve a posted proxy (the pump, from epoll) */
void door_kicked(void)
{
    char buf[64];
    while (read( kick_rfd, buf, sizeof(buf) ) > 0);
    if (door_area) door_serve_proxy();
}

/* The pump takes the door lock. While a door caller holds it, the pump
 * serves that caller's proxied system calls: the caller cannot give the
 * lock up mid-request, and nothing else would. */
void door_pump_lock(void)
{
    struct door_area *a = door_area;
    if (!a) return;
    __atomic_store_n( &a->pump_sleeping, 0, __ATOMIC_SEQ_CST );
    for (;;)
    {
        int v = 0;
        if (__atomic_compare_exchange_n( &a->lock, &v, 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED ))
        {
            a->owner = -1;
            return;
        }
        door_serve_proxy();
        if (v == 1 && !__atomic_compare_exchange_n( &a->lock, &v, 2, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED ))
            continue;
        /* bounded: a post notifies this word, but a missed one costs 1 ms, not a hang */
        __builtin_wasm_memory_atomic_wait32( &a->lock, 2, 1000000 );
    }
}

void door_pump_unlock( long long wake )
{
    struct door_area *a = door_area;
    if (!a) return;
    a->pump_wake = wake;
    __atomic_store_n( &a->pump_sleeping, 1, __ATOMIC_SEQ_CST );
    if (__atomic_exchange_n( &a->lock, 0, __ATOMIC_RELEASE ) == 2)
        __builtin_wasm_memory_atomic_notify( &a->lock, -1 );
}

/* Run one request from the door buffer as thread (unix_pid, unix_tid). The
 * caller (the gex host) holds the lock. Returns where the reply is in the
 * buffer, ~0u if no such thread (the client uses its pipes), ~1u if the
 * thread died in the request (no reply, as a closed reply pipe). `now`
 * and `boot`, when not 0, are the time (100 ns since 1601) and
 * CLOCK_BOOTTIME (100 ns) the host read: set_current_time without its two
 * clock calls out of the door. */
unsigned int __wine_door_dispatch( int unix_pid, int unix_tid, long long now, long long boot )
{
    struct door_area *a = door_area;
    struct thread *thread;
    unsigned int ret;
    timeout_t deadline;

    if (!(thread = door_find_thread( unix_pid, unix_tid ))) return ~0u;
    a->calls++;
    if (now)
    {
        current_time = now;
        monotonic_time = boot;
    }
    else set_current_time();
    ret = door_run_request( thread, (char *)(unsigned long)a->buf );
    /* a timeout earlier than the pump's sleep: the pump must look again */
    deadline = door_earliest_deadline();
    if (deadline != -1 && (a->pump_wake == -1 || deadline < a->pump_wake)) a->kick_needed = 1;
    return ret;
}

#endif /* __wasm32__ */
