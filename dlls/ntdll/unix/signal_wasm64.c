/*
 * wasm64 signal handling routines: 64-bit native Wine under gex (W64, gex
 * docs/wine-native-x64.md). The program's own code is x86-64, run by gex;
 * Wine's Unix side is LP64 wasm64, lowered to wasm32 by default. The
 * x86-64 register file is gex's thread context, the "user mode" state a
 * system call saves in its frame.
 *
 * The frame, context and dispatcher layouts are x86-64's (signal_x86_64.c,
 * from which the portable parts are copied); the control flow is
 * signal_wasm32.c's, through gex:
 *   - system calls: PE ntdll's x86-64 stubs call through [0x7ffe1000]
 *     (USD.SystemCall is 1), which points at W64_SYSCALL_ENTRY, a `ud2` in
 *     the page after the USD. gex hands that fault to __wine_gex_fault,
 *     which runs the call on the Unix side and resumes the program at the
 *     stub's return.
 *   - Unix calls from PE code (winecrt0's __wine_unix_call): PE ntdll's
 *     __wine_unix_call_dispatcher pointer is W64_UNIX_CALL_ENTRY, another
 *     `ud2` on that page, handled the same way.
 *   - faults in the program's code: gex's fault hook (__wine_gex_fault).
 *   - the program's code runs under gex.call_out.
 *
 * The `ud2` route needs nothing new from gex. A direct route (a native
 * PE ntdll calling the dispatcher, W64.2) replaces it for speed.
 *
 * Copyright 1999, 2005 Alexandre Julliard (the parts from signal_x86_64.c)
 * Copyright 2026 the gex authors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#if 0
#pragma makedep unix
#endif

#ifdef __wasm64__

#include "config.h"

#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <unistd.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winternl.h"
#include "ddk/wdm.h"
#include "unix_private.h"
#include "wine/debug.h"
#include "wine/gex/gex_abi.h"

WINE_DEFAULT_DEBUG_CHANNEL(seh);

/* gex's side: the thread's register context, a guest call, the hooks. */
unsigned long __gex_get_ctx(void);
__attribute__((import_module("gex"), import_name("fault_hook"))) void gex_fault_hook( int idx );
__attribute__((import_module("gex"), import_name("signal_hook"))) void gex_signal_hook( int idx );

static inline int cur_ctx(void) { return (int)__gex_get_ctx(); }

/* x86-64 registers in gex's context, numbered as x86 encodes them */
enum { R_RAX, R_RCX, R_RDX, R_RBX, R_RSP, R_RBP, R_RSI, R_RDI, R_R8, R_R9, R_R10, R_R11, R_R12, R_R13, R_R14, R_R15 };
#define REG(r) GEX_C64( GEX_CTX_XN(r) )

/* gex's trampoline page's return pad (gex-kernel nt::boot::tramp::
 * NATIVE_RETURN_PAD): a call-out that waits for it is a native caller's */
#define GEX_RETURN_PAD 0x3ffff1

static const WORD cs64_sel = 0x33, ds64_sel = 0x2b;

struct machine_frame
{
    ULONG64 rip;
    ULONG64 cs;
    ULONG64 eflags;
    ULONG64 rsp;
    ULONG64 ss;
};

/* stack layout when calling KiUserExceptionDispatcher */
struct exc_stack_layout
{
    CONTEXT              context;        /* 000 */
    CONTEXT_EX           context_ex;     /* 4d0 */
    EXCEPTION_RECORD     rec;            /* 4f0 */
    ULONG64              align;          /* 588 */
    struct machine_frame machine_frame;  /* 590 */
    ULONG64              align2;         /* 5b8 */
};
C_ASSERT( offsetof(struct exc_stack_layout, rec) == 0x4f0 );
C_ASSERT( offsetof(struct exc_stack_layout, machine_frame) == 0x590 );
C_ASSERT( sizeof(struct exc_stack_layout) == 0x5c0 );

/* stack layout when calling KiUserApcDispatcher */
struct apc_stack_layout
{
    CONTEXT              context;        /* 000 */
    CONTEXT_EX           context_ex;     /* 4d0 */
    KCONTINUE_ARGUMENT   continue_arg;   /* 4f0 */
    ULONG64              align1;         /* 508 */
    APC_CALLBACK_DATA    callback_data;  /* 510 */
    struct machine_frame machine_frame;  /* 530 */
    ULONG64              align2;         /* 558 */
};
C_ASSERT( offsetof(struct apc_stack_layout, continue_arg) == 0x4f0 );
C_ASSERT( offsetof(struct apc_stack_layout, machine_frame) == 0x530 );
C_ASSERT( sizeof(struct apc_stack_layout) == 0x560 );

/* stack layout when calling KiUserCallbackDispatcher */
struct callback_stack_layout
{
    ULONG64              padding[4];     /* 000 parameter space for called function */
    void                *args;           /* 020 arguments */
    ULONG                len;            /* 028 arguments len */
    ULONG                id;             /* 02c function id */
    struct machine_frame machine_frame;  /* 030 machine frame */
    BYTE                 args_data[0];   /* 058 copied argument data */
};
C_ASSERT( offsetof(struct callback_stack_layout, machine_frame) == 0x30 );
C_ASSERT( sizeof(struct callback_stack_layout) == 0x58 );

struct syscall_frame
{
    ULONG64               rax;           /* 0000 */
    ULONG64               rbx;           /* 0008 */
    ULONG64               rcx;           /* 0010 */
    ULONG64               rdx;           /* 0018 */
    ULONG64               rsi;           /* 0020 */
    ULONG64               rdi;           /* 0028 */
    ULONG64               r8;            /* 0030 */
    ULONG64               r9;            /* 0038 */
    ULONG64               r10;           /* 0040 */
    ULONG64               r11;           /* 0048 */
    ULONG64               r12;           /* 0050 */
    ULONG64               r13;           /* 0058 */
    ULONG64               r14;           /* 0060 */
    ULONG64               r15;           /* 0068 */
    ULONG64               rip;           /* 0070 */
    ULONG64               cs;            /* 0078 */
    ULONG64               eflags;        /* 0080 */
    ULONG64               rsp;           /* 0088 */
    ULONG64               ss;            /* 0090 */
    ULONG64               rbp;           /* 0098 */
    struct syscall_frame *prev_frame;    /* 00a0 */
    void                 *syscall_cfa;   /* 00a8 */
    DWORD                 syscall_id;    /* 00b0 */
    DWORD                 restore_flags; /* 00b4 */
    DWORD                 align[2];      /* 00b8 */
    XSAVE_FORMAT          xsave;         /* 00c0 */
    DECLSPEC_ALIGN(64) XSAVE_AREA_HEADER xstate;    /* 02c0 */
};

C_ASSERT( offsetof( struct syscall_frame, xsave ) == 0xc0 );
C_ASSERT( offsetof( struct syscall_frame, xstate ) == 0x2c0 );
C_ASSERT( sizeof( struct syscall_frame ) == 0x300);

struct amd64_thread_data
{
    DWORD_PTR             dr0;           /* 02f0 debug registers */
    DWORD_PTR             dr1;           /* 02f8 */
    DWORD_PTR             dr2;           /* 0300 */
    DWORD_PTR             dr3;           /* 0308 */
    DWORD_PTR             dr6;           /* 0310 */
    DWORD_PTR             dr7;           /* 0318 */
    void                 *pthread_teb;   /* 0320 thread data for pthread */
    DWORD_PTR             frame_size;    /* 0328 syscall frame size including xstate */
    void                **instrumentation_callback; /* 0330 */
    DWORD                 fs;            /* 0338 WOW TEB selector */
    DWORD                 mxcsr;         /* 033c Unix-side mxcsr register */
    char                  syscall_dispatch; /* 0340 */
};

C_ASSERT( sizeof(struct amd64_thread_data) <= sizeof(((struct teb_data *)0)->cpu_data) );
C_ASSERT( offsetof( TEB, GdiTebBatch ) + offsetof( struct amd64_thread_data, frame_size ) == 0x328 );

static inline struct amd64_thread_data *amd64_thread_data( struct thread_data *data )
{
    if (!data->teb) return NULL;
    return (struct amd64_thread_data *)get_teb_data( data )->cpu_data;
}

static unsigned int frame_size;
static unsigned int xstate_size = sizeof(XSAVE_AREA_HEADER);
static UINT64 xstate_extended_features;

struct xcontext
{
    CONTEXT c;
    CONTEXT_EX c_ex;
};

static inline XSAVE_AREA_HEADER *xstate_from_context( const CONTEXT *context )
{
    CONTEXT_EX *xctx = (CONTEXT_EX *)(context + 1);

    if ((context->ContextFlags & CONTEXT_XSTATE) != CONTEXT_XSTATE) return NULL;
    return (XSAVE_AREA_HEADER *)((char *)xctx + xctx->XState.Offset);
}

static inline void context_init_xstate( CONTEXT *context, void *xstate_buffer )
{
    CONTEXT_EX *xctx;

    xctx = (CONTEXT_EX *)(context + 1);
    xctx->Legacy.Length = sizeof(CONTEXT);
    xctx->Legacy.Offset = -(LONG)sizeof(CONTEXT);

    if (xstate_buffer)
    {
        xctx->XState.Length = xstate_size;
        xctx->XState.Offset = (BYTE *)xstate_buffer - (BYTE *)xctx;
        context->ContextFlags |= CONTEXT_XSTATE;

        xctx->All.Length = sizeof(CONTEXT) + xctx->XState.Offset + xctx->XState.Length;
    }
    else
    {
        xctx->XState.Length = 25;
        xctx->XState.Offset = 0;

        xctx->All.Length = sizeof(CONTEXT) + 24; /* sizeof(CONTEXT_EX) minus 8 alignment bytes on x64. */
    }

    xctx->All.Offset = -(LONG)sizeof(CONTEXT);
}


/***********************************************************************
 * The x86-64 state in gex's context and the syscall frame
 */

static inline ULONG64 get_eflags( int ctx )
{
    ULONG64 f = 0x202;  /* IF, and bit 1 */
    if (GEX_C8( GEX_CTX_CF )) f |= 0x001;
    if (GEX_C8( GEX_CTX_PF )) f |= 0x004;
    if (GEX_C8( GEX_CTX_AF )) f |= 0x010;
    if (GEX_C8( GEX_CTX_ZF )) f |= 0x040;
    if (GEX_C8( GEX_CTX_NF )) f |= 0x080;
    if (GEX_C8( GEX_CTX_DF )) f |= 0x400;
    if (GEX_C8( GEX_CTX_VF )) f |= 0x800;
    return f;
}

static inline void set_eflags( int ctx, ULONG64 f )
{
    GEX_C8( GEX_CTX_CF ) = !!(f & 0x001);
    GEX_C8( GEX_CTX_PF ) = !!(f & 0x004);
    GEX_C8( GEX_CTX_AF ) = !!(f & 0x010);
    GEX_C8( GEX_CTX_ZF ) = !!(f & 0x040);
    GEX_C8( GEX_CTX_NF ) = !!(f & 0x080);
    GEX_C8( GEX_CTX_DF ) = !!(f & 0x400);
    GEX_C8( GEX_CTX_VF ) = !!(f & 0x800);
}

/* gex's x87 stack is physical registers ST(i) = R[(TOP + i) & 7] with a
 * valid bit each; FXSAVE's are in stack order with an abridged tag byte. */
static void ctx_to_xsave( int ctx, XSAVE_FORMAT *xs )
{
    UINT top = GEX_C32( GEX_CTX_X87_TOP ) & 7, tags = GEX_C32( GEX_CTX_X87_TAGS ), i;

    memset( xs, 0, sizeof(*xs) );
    xs->ControlWord = GEX_C32( GEX_CTX_X87_CW );
    xs->StatusWord = (GEX_C32( GEX_CTX_X87_SW ) & ~0x3800) | (top << 11);
    for (i = 0; i < 8; i++)
    {
        UINT r = (top + i) & 7;
        memcpy( &xs->FloatRegisters[i], GEX_CTXP( ctx ) + GEX_CTX_X87 + 16 * r, 10 );
        if (tags & (1u << r)) xs->TagWord |= 1u << r;
    }
    xs->MxCsr = GEX_C32( GEX_CTX_MXCSR );
    xs->MxCsr_Mask = 0x2ffff;
    for (i = 0; i < 16; i++) memcpy( &xs->XmmRegisters[i], GEX_CTXP( ctx ) + GEX_CTX_VN(i), 16 );
}

static void xsave_to_ctx( int ctx, const XSAVE_FORMAT *xs )
{
    UINT top = (xs->StatusWord >> 11) & 7, tags = 0, i;

    GEX_C32( GEX_CTX_X87_CW ) = xs->ControlWord;
    GEX_C32( GEX_CTX_X87_SW ) = xs->StatusWord & ~0x3800;
    GEX_C32( GEX_CTX_X87_TOP ) = top;
    for (i = 0; i < 8; i++)
    {
        UINT r = (top + i) & 7;
        memcpy( GEX_CTXP( ctx ) + GEX_CTX_X87 + 16 * r, &xs->FloatRegisters[i], 10 );
        if (xs->TagWord & (1u << r)) tags |= 1u << r;
    }
    GEX_C32( GEX_CTX_X87_TAGS ) = tags;
    GEX_C32( GEX_CTX_MXCSR ) = xs->MxCsr;
    for (i = 0; i < 16; i++) memcpy( GEX_CTXP( ctx ) + GEX_CTX_VN(i), &xs->XmmRegisters[i], 16 );
}

/* The program's state, as a system call or a fault finds it, into the
 * frame. */
static void ctx_to_frame( int ctx, struct syscall_frame *frame, ULONG64 rip, ULONG64 rsp )
{
    frame->restore_flags = 0;
    frame->rax = REG( R_RAX );
    frame->rbx = REG( R_RBX );
    frame->rcx = REG( R_RCX );
    frame->rdx = REG( R_RDX );
    frame->rsi = REG( R_RSI );
    frame->rdi = REG( R_RDI );
    frame->r8  = REG( R_R8 );
    frame->r9  = REG( R_R9 );
    frame->r10 = REG( R_R10 );
    frame->r11 = REG( R_R11 );
    frame->r12 = REG( R_R12 );
    frame->r13 = REG( R_R13 );
    frame->r14 = REG( R_R14 );
    frame->r15 = REG( R_R15 );
    frame->rbp = REG( R_RBP );
    frame->rip = rip;
    frame->rsp = rsp;
    frame->eflags = get_eflags( ctx );
    frame->cs = cs64_sel;
    frame->ss = ds64_sel;
    ctx_to_xsave( ctx, &frame->xsave );
    frame->xstate.Mask = XSTATE_MASK_LEGACY;
    frame->xstate.CompactionMask = 0;
}

/* And back, as the frame now says. */
static void frame_to_ctx( int ctx, const struct syscall_frame *frame )
{
    REG( R_RAX ) = frame->rax;
    REG( R_RBX ) = frame->rbx;
    REG( R_RCX ) = frame->rcx;
    REG( R_RDX ) = frame->rdx;
    REG( R_RSI ) = frame->rsi;
    REG( R_RDI ) = frame->rdi;
    REG( R_R8 )  = frame->r8;
    REG( R_R9 )  = frame->r9;
    REG( R_R10 ) = frame->r10;
    REG( R_R11 ) = frame->r11;
    REG( R_R12 ) = frame->r12;
    REG( R_R13 ) = frame->r13;
    REG( R_R14 ) = frame->r14;
    REG( R_R15 ) = frame->r15;
    REG( R_RBP ) = frame->rbp;
    REG( R_RSP ) = frame->rsp;
    set_eflags( ctx, frame->eflags );
    if (frame->restore_flags & (CONTEXT_FLOATING_POINT | CONTEXT_XSTATE))
        xsave_to_ctx( ctx, &frame->xsave );
    GEX_C64( GEX_CTX_PC ) = frame->rip;
}


/***********************************************************************
 *           signal_set_full_context
 */
NTSTATUS signal_set_full_context( CONTEXT *context )
{
    NTSTATUS status = NtSetContextThread( GetCurrentThread(), context );

    if (!status && (context->ContextFlags & CONTEXT_INTEGER) == CONTEXT_INTEGER)
        get_syscall_frame(get_thread_data())->restore_flags |= CONTEXT_INTEGER;
    return status;
}


/***********************************************************************
 *              get_native_context
 */
void *get_native_context( CONTEXT *context )
{
    return context;
}


/***********************************************************************
 *              get_wow_context
 *
 * No wow64 in W64 (docs/wine-native-x64.md §3).
 */
void *get_wow_context( CONTEXT *context )
{
    return NULL;
}


/***********************************************************************
 *              NtSetContextThread  (NTDLL.@)
 *              ZwSetContextThread  (NTDLL.@)
 */
NTSTATUS WINAPI NtSetContextThread( HANDLE handle, const CONTEXT *context )
{
    NTSTATUS ret = STATUS_SUCCESS;
    DWORD flags = context->ContextFlags & ~CONTEXT_AMD64;
    BOOL self = (handle == GetCurrentThread());
    struct thread_data *data = get_thread_data();
    struct syscall_frame *frame = get_syscall_frame( data );
    struct amd64_thread_data *amd64_data = amd64_thread_data( data );

    if (self && !frame) return STATUS_ACCESS_DENIED;

    if ((flags & CONTEXT_XSTATE) && xstate_extended_features)
    {
        CONTEXT_EX *context_ex = (CONTEXT_EX *)(context + 1);
        XSAVE_AREA_HEADER *xs = (XSAVE_AREA_HEADER *)((char *)context_ex + context_ex->XState.Offset);

        if (context_ex->XState.Length < sizeof(XSAVE_AREA_HEADER) ||
            context_ex->XState.Length > xstate_size)
            return STATUS_INVALID_PARAMETER;
        if ((xs->Mask & xstate_extended_features)
            && (context_ex->XState.Length < xstate_get_size( xs->CompactionMask, xs->Mask )))
            return STATUS_BUFFER_OVERFLOW;
    }
    else flags &= ~CONTEXT_XSTATE;

    /* debug registers require a server call */
    if (self && (flags & CONTEXT_DEBUG_REGISTERS))
        self = (amd64_data->dr0 == context->Dr0 &&
                amd64_data->dr1 == context->Dr1 &&
                amd64_data->dr2 == context->Dr2 &&
                amd64_data->dr3 == context->Dr3 &&
                amd64_data->dr6 == context->Dr6 &&
                amd64_data->dr7 == context->Dr7);

    if (!self)
    {
        ret = set_thread_context( handle, context, &self, IMAGE_FILE_MACHINE_AMD64 );
        if (ret || !self) return ret;
        if (flags & CONTEXT_DEBUG_REGISTERS)
        {
            amd64_data->dr0 = context->Dr0;
            amd64_data->dr1 = context->Dr1;
            amd64_data->dr2 = context->Dr2;
            amd64_data->dr3 = context->Dr3;
            amd64_data->dr6 = context->Dr6;
            amd64_data->dr7 = context->Dr7;
        }
    }

    if (flags & CONTEXT_INTEGER)
    {
        frame->rax = context->Rax;
        frame->rbx = context->Rbx;
        frame->rcx = context->Rcx;
        frame->rdx = context->Rdx;
        frame->rbp = context->Rbp;
        frame->rsi = context->Rsi;
        frame->rdi = context->Rdi;
        frame->r8  = context->R8;
        frame->r9  = context->R9;
        frame->r10 = context->R10;
        frame->r11 = context->R11;
        frame->r12 = context->R12;
        frame->r13 = context->R13;
        frame->r14 = context->R14;
        frame->r15 = context->R15;
    }
    if (flags & CONTEXT_CONTROL)
    {
        frame->rsp    = context->Rsp;
        frame->rip    = context->Rip;
        frame->eflags = context->EFlags;
    }
    if (flags & CONTEXT_FLOATING_POINT)
    {
        frame->xsave = context->FltSave;
        frame->xsave.MxCsr = context->MxCsr;
        frame->xstate.Mask |= XSTATE_MASK_LEGACY;
    }
    if (flags & CONTEXT_XSTATE)
    {
        CONTEXT_EX *context_ex = (CONTEXT_EX *)(context + 1);
        XSAVE_AREA_HEADER *xs = (XSAVE_AREA_HEADER *)((char *)context_ex + context_ex->XState.Offset);
        UINT64 mask = frame->xstate.Mask;

        if (user_shared_data->XState.CompactionEnabled)
            frame->xstate.CompactionMask |= xstate_extended_features;
        copy_xstate( &frame->xstate, xs, xs->Mask );
        if (xs->CompactionMask) frame->xstate.Mask |= mask & ~xs->CompactionMask;
    }

    frame->restore_flags |= flags & ~CONTEXT_INTEGER;
    return STATUS_SUCCESS;
}


/***********************************************************************
 *              NtGetContextThread  (NTDLL.@)
 *              ZwGetContextThread  (NTDLL.@)
 */
NTSTATUS WINAPI NtGetContextThread( HANDLE handle, CONTEXT *context )
{
    struct thread_data *data = get_thread_data();
    struct syscall_frame *frame = get_syscall_frame( data );
    struct amd64_thread_data *amd64_data = amd64_thread_data( data );
    DWORD needed_flags = context->ContextFlags & ~CONTEXT_AMD64;
    BOOL self = (handle == GetCurrentThread());

    if (self && !frame) return STATUS_ACCESS_DENIED;

    /* debug registers require a server call */
    if (needed_flags & CONTEXT_DEBUG_REGISTERS) self = FALSE;

    if (!self)
    {
        NTSTATUS ret = get_thread_context( handle, context, &self, IMAGE_FILE_MACHINE_AMD64 );
        if (ret || !self) return ret;
    }

    if (needed_flags & CONTEXT_INTEGER)
    {
        context->Rax = frame->rax;
        context->Rbx = frame->rbx;
        context->Rcx = frame->rcx;
        context->Rdx = frame->rdx;
        context->Rbp = frame->rbp;
        context->Rsi = frame->rsi;
        context->Rdi = frame->rdi;
        context->R8  = frame->r8;
        context->R9  = frame->r9;
        context->R10 = frame->r10;
        context->R11 = frame->r11;
        context->R12 = frame->r12;
        context->R13 = frame->r13;
        context->R14 = frame->r14;
        context->R15 = frame->r15;
        context->ContextFlags |= CONTEXT_INTEGER;
    }
    if (needed_flags & CONTEXT_CONTROL)
    {
        context->Rsp    = frame->rsp;
        context->Rip    = frame->rip;
        context->EFlags = frame->eflags;
        context->SegCs  = cs64_sel;
        context->SegSs  = ds64_sel;
        context->ContextFlags |= CONTEXT_CONTROL;
    }
    if (needed_flags & CONTEXT_SEGMENTS)
    {
        context->SegDs  = ds64_sel;
        context->SegEs  = ds64_sel;
        context->SegFs  = amd64_data->fs;
        context->SegGs  = ds64_sel;
        context->ContextFlags |= CONTEXT_SEGMENTS;
    }
    if (needed_flags & CONTEXT_FLOATING_POINT)
    {
        context->FltSave = frame->xsave;
        context->MxCsr = context->FltSave.MxCsr;
        context->ContextFlags |= CONTEXT_FLOATING_POINT;
    }
    if ((needed_flags & CONTEXT_XSTATE) && xstate_extended_features)
    {
        CONTEXT_EX *context_ex = (CONTEXT_EX *)(context + 1);
        XSAVE_AREA_HEADER *xstate = (XSAVE_AREA_HEADER *)((char *)context_ex + context_ex->XState.Offset);
        UINT64 mask;

        if (context_ex->XState.Length < sizeof(XSAVE_AREA_HEADER) ||
            context_ex->XState.Length > xstate_size)
            return STATUS_INVALID_PARAMETER;

        if (user_shared_data->XState.CompactionEnabled)
        {
            frame->xstate.CompactionMask |= xstate_extended_features;
            mask = xstate->CompactionMask & xstate_extended_features;
            xstate->Mask = frame->xstate.Mask & mask;
            xstate->CompactionMask = 0x8000000000000000 | mask;
        }
        else
        {
            mask = xstate->Mask & xstate_extended_features;
            xstate->Mask = frame->xstate.Mask & mask;
            xstate->CompactionMask = 0;
        }
        memset( xstate->Reserved2, 0, sizeof(xstate->Reserved2) );
        if (xstate->Mask)
        {
            if (context_ex->XState.Length < xstate_get_size( xstate->CompactionMask, xstate->Mask ))
                return STATUS_BUFFER_OVERFLOW;
            copy_xstate( xstate, &frame->xstate, xstate->Mask );
        }
    }
    /* update the cached version of the debug registers */
    if (needed_flags & CONTEXT_DEBUG_REGISTERS)
    {
        amd64_data->dr0 = context->Dr0;
        amd64_data->dr1 = context->Dr1;
        amd64_data->dr2 = context->Dr2;
        amd64_data->dr3 = context->Dr3;
        amd64_data->dr6 = context->Dr6;
        amd64_data->dr7 = context->Dr7;
    }
    set_context_exception_reporting_flags( &context->ContextFlags, CONTEXT_SERVICE_ACTIVE );
    return STATUS_SUCCESS;
}


/***********************************************************************
 *              set_thread_wow64_context
 */
NTSTATUS set_thread_wow64_context( HANDLE handle, const void *ctx, ULONG size )
{
    return STATUS_INVALID_INFO_CLASS;
}


/***********************************************************************
 *              get_thread_wow64_context
 */
NTSTATUS get_thread_wow64_context( HANDLE handle, void *ctx, ULONG size )
{
    return STATUS_INVALID_INFO_CLASS;
}


/***********************************************************************
 *           call_user_apc_dispatcher
 */
NTSTATUS call_user_apc_dispatcher( CONTEXT *context, unsigned int flags, ULONG_PTR arg1, ULONG_PTR arg2, ULONG_PTR arg3,
                                   PNTAPCFUNC func, NTSTATUS status )
{
    struct thread_data *data = get_thread_data();
    struct syscall_frame *frame = get_syscall_frame( data );
    ULONG64 rsp = context ? context->Rsp : frame->rsp;
    struct apc_stack_layout *stack;

    if (flags & ~SERVER_USER_APC_CALLBACK_DATA_CONTEXT) FIXME( "flags %#x are not supported.\n", flags );

    rsp &= ~15;
    stack = (struct apc_stack_layout *)rsp - 1;
    if (context)
    {
        memmove( &stack->context, context, sizeof(stack->context) );
        NtSetContextThread( GetCurrentThread(), &stack->context );
    }
    else
    {
        stack->context.ContextFlags = CONTEXT_FULL;
        NtGetContextThread( GetCurrentThread(), &stack->context );
        stack->context.Rax = status;
    }
    memset( &stack->continue_arg, 0, sizeof(stack->continue_arg) );
    stack->continue_arg.ContinueType = KCONTINUE_RESUME;
    stack->continue_arg.ContinueFlags = KCONTINUE_FLAG_TEST_ALERT | KCONTINUE_FLAG_DELIVER_APC;
    stack->context.P1Home    = arg1;
    if (flags & SERVER_USER_APC_CALLBACK_DATA_CONTEXT)
    {
        stack->callback_data.ContextRecord = &stack->context;
        stack->callback_data.Parameter = arg2;
        stack->callback_data.Reserved0 = 0;
        stack->callback_data.Reserved1 = 0;
        stack->context.P2Home = (ULONG_PTR)&stack->callback_data;
    }
    else stack->context.P2Home = arg2;
    stack->context.P3Home    = arg3;
    stack->context.P4Home    = (ULONG64)func;
    stack->machine_frame.rip = stack->context.Rip;
    stack->machine_frame.rsp = stack->context.Rsp;
    frame->rbp = stack->context.Rbp;
    frame->rsp = (ULONG64)stack;
    frame->rip = (ULONG64)pKiUserApcDispatcher;
    frame->restore_flags |= CONTEXT_CONTROL;
    return status;
}


/***********************************************************************
 *           call_raise_user_exception_dispatcher
 */
void call_raise_user_exception_dispatcher( struct thread_data *data )
{
    get_syscall_frame(data)->rip = (UINT64)pKiRaiseUserExceptionDispatcher;
}


/***********************************************************************
 *           call_user_exception_dispatcher
 */
NTSTATUS call_user_exception_dispatcher( struct thread_data *data, EXCEPTION_RECORD *rec, CONTEXT *context )
{
    struct syscall_frame *frame = get_syscall_frame( data );
    struct exc_stack_layout *stack;
    NTSTATUS status = NtSetContextThread( GetCurrentThread(), context );

    if (status) return status;
    stack = (struct exc_stack_layout *)((context->Rsp - sizeof(*stack) - xstate_size) & ~(ULONG_PTR)63);
    memmove( &stack->context, context, sizeof(*context) );

    if ((context->ContextFlags & CONTEXT_XSTATE) == CONTEXT_XSTATE)
    {
        XSAVE_AREA_HEADER *dst_xs = (XSAVE_AREA_HEADER *)(stack + 1);
        assert( !((ULONG_PTR)dst_xs & 63) );
        context_init_xstate( &stack->context, dst_xs );
        memcpy( dst_xs, &frame->xstate, xstate_size );
    }
    else context_init_xstate( &stack->context, NULL );

    stack->rec = *rec;
    /* fix up instruction pointer in context for EXCEPTION_BREAKPOINT */
    if (stack->rec.ExceptionCode == EXCEPTION_BREAKPOINT) stack->context.Rip--;
    stack->machine_frame.rip = stack->context.Rip;
    stack->machine_frame.rsp = stack->context.Rsp;
    frame->rbp = stack->context.Rbp;
    frame->rsp = (ULONG64)stack;
    frame->rip = (ULONG64)pKiUserExceptionDispatcher;
    frame->restore_flags |= CONTEXT_CONTROL;
    return status;
}


/***********************************************************************
 * System calls and Unix calls from the program's x86-64 code
 *
 * Both arrive as a `ud2` on the page after the USD (W64_SYSCALL_ENTRY and
 * W64_UNIX_CALL_ENTRY), reached by a `call`: [rsp] is the return address,
 * the first four arguments are in r10 (a system call's stub moved rcx
 * there) or rcx, then rdx, r8, r9, and the rest at rsp + 0x30 on (the
 * caller's return address and its 0x20 bytes of home space between).
 *
 * A system call's arguments are 64-bit words, and an indirect call in wasm
 * must have the callee's exact type: each of ntdll's system calls is
 * called through its typed wrapper (__wine_wasm64_syscalls,
 * syscall_wasm64.cpp), which converts the words to its parameters.
 */

typedef NTSTATUS (*wasm64_syscall)( const ULONG64 *args );
extern const wasm64_syscall __wine_wasm64_syscalls[];
extern const unsigned int __wine_wasm64_syscall_count;

#define MAX_SYSCALL_ARGS 17

/* a system call Wine does not implement (syscall.c's stub_syscall) */
void __wine_wasm64_stub_syscall( const char *name )
{
    CONTEXT context = { .ContextFlags = CONTEXT_FULL };
    EXCEPTION_RECORD rec =
    {
        .ExceptionCode = EXCEPTION_WINE_STUB,
        .ExceptionFlags = EXCEPTION_NONCONTINUABLE,
        .NumberParameters = 2,
        .ExceptionInformation[0] = (ULONG_PTR)"ntdll",
        .ExceptionInformation[1] = (ULONG_PTR)name,
    };
    NtGetContextThread( GetCurrentThread(), &context );
    rec.ExceptionAddress = (void *)context.Rip;
    NtRaiseException( &rec, &context, TRUE );
}

static NTSTATUS call_table( UINT table, UINT id, const ULONG64 *args )
{
    /* only ntdll's table is typed so far; win32u's comes with its Unix
     * side (W64.4) */
    if (table != 0 || id >= __wine_wasm64_syscall_count) return STATUS_INVALID_SYSTEM_SERVICE;
    return __wine_wasm64_syscalls[id]( args );
}

static void syscall_body( int ctx )
{
    struct thread_data *data = get_thread_data();
    struct syscall_frame *frame = get_syscall_frame( data );
    ULONG64 rsp = REG( R_RSP ), args[MAX_SYSCALL_ARGS];
    UINT id = (UINT)REG( R_RAX ), i;
    NTSTATUS status;

    ctx_to_frame( ctx, frame, *(const ULONG64 *)rsp, rsp + 8 );
    frame->syscall_id = id;
    args[0] = REG( R_R10 );
    args[1] = REG( R_RDX );
    args[2] = REG( R_R8 );
    args[3] = REG( R_R9 );
    for (i = 4; i < MAX_SYSCALL_ARGS; i++) args[i] = ((const ULONG64 *)(rsp + 0x30))[i - 4];
    status = call_table( (id >> 12) & 3, id & 0xfff, args );
    frame = get_syscall_frame( data );
    if (!(frame->restore_flags & CONTEXT_INTEGER)) frame->rax = status;
    frame_to_ctx( ctx, frame );
}

/* Unix calls from PE code (winecrt0's __wine_unix_call): a unixlib's
 * function table is in this module, so the call goes straight through;
 * its frame is a system call's, for the callbacks it may make. */
static void unix_call_body( int ctx )
{
    struct thread_data *data = get_thread_data();
    struct syscall_frame *frame = get_syscall_frame( data );
    ULONG64 rsp = REG( R_RSP );
    unixlib_handle_t handle = REG( R_RCX );
    unsigned int code = (unsigned int)REG( R_RDX );
    void *args = (void *)REG( R_R8 );
    NTSTATUS status;

    ctx_to_frame( ctx, frame, *(const ULONG64 *)rsp, rsp + 8 );
    status = handle ? ((const unixlib_entry_t *)handle)[code]( args ) : STATUS_DLL_NOT_FOUND;
    frame = get_syscall_frame( data );
    frame->rax = status;
    frame_to_ctx( ctx, frame );
}

/* The Unix side runs on the thread's own stack, below its syscall frame,
 * as x86-64's runs on the kernel stack: from the fault hook the C stack
 * pointer is just under the guest's, and what Unix code writes below the
 * guest's stack pointer (a callback's or an APC's frame) would land on its
 * own frames. */
ULONG64 __wine_gex_syscall_sp(void)
{
    return ((ULONG64)get_syscall_frame( get_thread_data() ) - 256) & ~(ULONG64)15;
}

void __wine_gex_entry_body( int ctx, int unix_call )
{
    if (unix_call) unix_call_body( ctx );
    else syscall_body( ctx );
}

extern void __wine_gex_entry_switch( int ctx, int unix_call );
__asm__(
    "\t.globaltype __stack_pointer, i64\n"
    "\t.functype __wine_gex_syscall_sp () -> (i64)\n"
    "\t.functype __wine_gex_entry_body (i32, i32) -> ()\n"
    "\t.globl __wine_gex_entry_switch\n"
    "\t.type __wine_gex_entry_switch,@function\n"
    "__wine_gex_entry_switch:\n"
    "\t.functype __wine_gex_entry_switch (i32, i32) -> ()\n"
    "\t.local i64\n"
    "\tglobal.get __stack_pointer\n"
    "\tlocal.set 2\n"
    "\tcall __wine_gex_syscall_sp\n"
    "\tglobal.set __stack_pointer\n"
    "\tlocal.get 0\n"
    "\tlocal.get 1\n"
    "\tcall __wine_gex_entry_body\n"
    "\tlocal.get 2\n"
    "\tglobal.set __stack_pointer\n"
    "\tend_function\n" );

/* What x86-64 assembles: never reached here (the program's stubs go
 * through W64_SYSCALL_ENTRY), only named. */
void __wine_syscall_dispatcher(void)
{
    abort();
}

/* The Unix side's own entry for Unix calls (a native caller's, W64.2):
 * straight through. */
NTSTATUS WINAPI __wine_unix_call_dispatcher( unixlib_handle_t handle, unsigned int code, void *args )
{
    if (!handle) return STATUS_DLL_NOT_FOUND;
    return ((const unixlib_entry_t *)handle)[code]( args );
}


/***********************************************************************
 * Faults in the program's code (gex's fault hook), and the two entries
 */
/* As x86-64's: a general-protection fault at a privileged instruction. */
static inline DWORD is_privileged_instr( CONTEXT *context )
{
    BYTE instr[16];
    unsigned int i, prefix_count = 0;
    unsigned int len = virtual_uninterrupted_read_memory( (BYTE *)context->Rip, instr, sizeof(instr) );

    for (i = 0; i < len; i++) switch (instr[i])
    {
    /* instruction prefixes */
    case 0x2e:  /* %cs: */
    case 0x36:  /* %ss: */
    case 0x3e:  /* %ds: */
    case 0x26:  /* %es: */
    case 0x40:  /* rex */
    case 0x41:  /* rex */
    case 0x42:  /* rex */
    case 0x43:  /* rex */
    case 0x44:  /* rex */
    case 0x45:  /* rex */
    case 0x46:  /* rex */
    case 0x47:  /* rex */
    case 0x48:  /* rex */
    case 0x49:  /* rex */
    case 0x4a:  /* rex */
    case 0x4b:  /* rex */
    case 0x4c:  /* rex */
    case 0x4d:  /* rex */
    case 0x4e:  /* rex */
    case 0x4f:  /* rex */
    case 0x64:  /* %fs: */
    case 0x65:  /* %gs: */
    case 0x66:  /* opcode size */
    case 0x67:  /* addr size */
    case 0xf0:  /* lock */
    case 0xf2:  /* repne */
    case 0xf3:  /* repe */
        if (++prefix_count >= 15) return EXCEPTION_ILLEGAL_INSTRUCTION;
        continue;

    case 0x0f: /* extended instruction */
        if (i == len - 1) return 0;
        switch (instr[i + 1])
        {
        case 0x06: /* clts */
        case 0x08: /* invd */
        case 0x09: /* wbinvd */
        case 0x20: /* mov crX, reg */
        case 0x21: /* mov drX, reg */
        case 0x22: /* mov reg, crX */
        case 0x23: /* mov reg drX */
            return EXCEPTION_PRIV_INSTRUCTION;
        }
        return 0;
    case 0x6c: /* insb (%dx) */
    case 0x6d: /* insl (%dx) */
    case 0x6e: /* outsb (%dx) */
    case 0x6f: /* outsl (%dx) */
    case 0xcd: /* int $xx */
    case 0xe4: /* inb al,XX */
    case 0xe5: /* in (e)ax,XX */
    case 0xe6: /* outb XX,al */
    case 0xe7: /* out XX,(e)ax */
    case 0xec: /* inb (%dx),%al */
    case 0xed: /* inl (%dx),%eax */
    case 0xee: /* outb %al,(%dx) */
    case 0xef: /* outl %eax,(%dx) */
    case 0xf4: /* hlt */
    case 0xfa: /* cli */
    case 0xfb: /* sti */
        return EXCEPTION_PRIV_INSTRUCTION;
    default:
        return 0;
    }
    return 0;
}

static void __wine_gex_fault( int ctx, unsigned int code, unsigned int addr, unsigned int kind )
{
    struct thread_data *data = get_thread_data();
    struct syscall_frame *frame = get_syscall_frame( data );
    struct xcontext xcontext;
    EXCEPTION_RECORD rec = { .ExceptionCode = code };
    ULONG64 pc = GEX_C64( GEX_CTX_PC );

    if (code == EXCEPTION_ILLEGAL_INSTRUCTION && (pc == W64_SYSCALL_ENTRY || pc == W64_UNIX_CALL_ENTRY))
    {
        __wine_gex_entry_switch( ctx, pc == W64_UNIX_CALL_ENTRY );
        return;
    }

    /* The fault is handled as x86-64's signal handlers handle one: the state
     * at the fault into a context, the record, and the exception dispatched
     * from the program's stack. A frame is borrowed for it. */
    ctx_to_frame( ctx, frame, pc, REG( R_RSP ) );
    memset( &xcontext, 0, sizeof(xcontext) );
    xcontext.c.ContextFlags = CONTEXT_FULL;
    NtGetContextThread( GetCurrentThread(), &xcontext.c );
    rec.ExceptionAddress = (void *)pc;
    switch (code)
    {
    case EXCEPTION_ACCESS_VIOLATION:
        /* a general-protection fault (gex says so with ~0) */
        if (addr == ~0u && (rec.ExceptionCode = is_privileged_instr( &xcontext.c ))) break;
        rec.ExceptionCode = code;
        rec.NumberParameters = 2;
        rec.ExceptionInformation[0] = kind;  /* 8: executing; else 0 (gex cannot tell a read from a write) */
        rec.ExceptionInformation[1] = addr;
        break;
    case EXCEPTION_BREAKPOINT:
        /* gex resumes after the int3; the address is the int3's own (the
         * context's Rip goes back one in call_user_exception_dispatcher) */
        rec.ExceptionAddress = (void *)(pc - 1);
        rec.NumberParameters = 1;
        break;
    }
    call_user_exception_dispatcher( data, &rec, &xcontext.c );
    frame_to_ctx( ctx, frame );
}


/***********************************************************************
 *           KeUserModeCallback
 *
 * The program's KiUserCallbackDispatcher, run nested through gex until it
 * comes back by NtCallbackReturn, which ends the callback's guest run.
 */
struct callback_return
{
    void    *ret_ptr;
    ULONG    ret_len;
    NTSTATUS status;
    int      done;
    ULONG64  rsp;     /* the guest stack pointer the call-out waits for */
};

NTSTATUS KeUserModeCallback( ULONG id, const void *args, ULONG len, void **ret_ptr, ULONG *ret_len )
{
    struct thread_data *data = get_thread_data();
    struct syscall_frame *frame = get_syscall_frame( data ), *cb_frame;
    ULONG64 rsp = (frame->rsp - offsetof( struct callback_stack_layout, args_data[len] )) & ~15;
    struct callback_stack_layout *stack = (struct callback_stack_layout *)rsp;
    struct callback_return ret = { 0 };
    int ctx = cur_ctx();
    ULONG64 saved_rsp = REG( R_RSP ), saved_pc = GEX_C64( GEX_CTX_PC );
    void *saved_list = data->teb->Tib.ExceptionList;

    if ((char *)get_kernel_stack( data ) + min_kernel_stack > (char *)&frame) return STATUS_STACK_OVERFLOW;

    stack->args              = stack->args_data;
    stack->len               = len;
    stack->id                = id;
    stack->machine_frame.rip = frame->rip;
    stack->machine_frame.rsp = frame->rsp;
    memcpy( stack->args_data, args, len );

    /* a frame of its own for the system calls the callback makes, in this
     * function's own frame (as signal_wasm32.c's) */
    cb_frame = (struct syscall_frame *)(((ULONG_PTR)__builtin_alloca( frame_size + 63 ) + 63) & ~(ULONG_PTR)63);
    cb_frame->prev_frame = frame;
    cb_frame->syscall_cfa = &ret;
    get_teb_data( data )->syscall_frame = cb_frame;

    /* the callback dispatcher is jumped to with the stack layout at RSP,
     * and never returns (NtCallbackReturn ends it) */
    REG( R_RSP ) = rsp;
    ret.rsp = rsp;
    GEX_C64( GEX_CTX_LAST_TARGET ) = (ULONG64)pKiUserCallbackDispatcher;
    gex_call_out( ctx, (int64_t)(ULONG_PTR)pKiUserCallbackDispatcher, GEX_RETURN_PAD, rsp );

    get_teb_data( data )->syscall_frame = frame;
    data->teb->Tib.ExceptionList = saved_list;
    REG( R_RSP ) = saved_rsp;
    GEX_C64( GEX_CTX_PC ) = saved_pc;
    if (!ret.done) return STATUS_INTERNAL_ERROR;
    *ret_ptr = ret.ret_ptr;
    *ret_len = ret.ret_len;
    return ret.status;
}


/***********************************************************************
 *           NtCallbackReturn  (NTDLL.@)
 */
NTSTATUS WINAPI NtCallbackReturn( void *ret_ptr, ULONG ret_len, NTSTATUS status )
{
    struct thread_data *data = get_thread_data();
    struct syscall_frame *frame = get_syscall_frame( data );
    struct callback_return *ret;

    if (!frame->prev_frame) return STATUS_NO_CALLBACK_ACTIVE;
    ret = frame->syscall_cfa;
    ret->ret_ptr = ret_ptr;
    ret->ret_len = ret_len;
    ret->status = status;
    ret->done = 1;
    /* back to KeUserModeCallback: the guest run ends at the address the
     * call-out waits for */
    frame->rip = GEX_RETURN_PAD;
    frame->rsp = ret->rsp;
    frame->restore_flags |= CONTEXT_CONTROL;
    return status;
}


/**********************************************************************
 *           Threads and the process
 */
void set_process_instrumentation_callback( void *callback )
{
    if (callback) FIXME( "Not supported.\n" );
}

NTSTATUS unwind_builtin_dll( void *args )
{
    return STATUS_UNSUCCESSFUL;
}

void ldt_set_entry( WORD sel, LDT_ENTRY entry )
{
}

NTSTATUS get_thread_ldt_entry( HANDLE handle, THREAD_DESCRIPTOR_INFORMATION *info, ULONG len )
{
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS signal_alloc_thread( TEB *teb )
{
    struct amd64_thread_data *thread_data = (struct amd64_thread_data *)&teb->GdiTebBatch;

    thread_data->frame_size = frame_size;
    return STATUS_SUCCESS;
}

void signal_free_thread( TEB *teb )
{
}

/***********************************************************************
 * Signals (SIGUSR1: suspend, SIGQUIT: thread abort, SIGINT: Ctrl-C)
 *
 * A signal that arrives while the thread runs guest code is taken at a
 * safepoint: gex-kernel builds x86-64's frame on the guest's stack and gex
 * runs it through __wine_gex_signal (gex.signal_hook), which calls these
 * handlers with the frame's ucontext, the guest's state. One that arrives
 * in Unix code is taken when a system call returns (libgex64), as on
 * x86-64 when inside a system call: the state is the syscall frame's.
 */

/* Linux's x86-64 sigcontext, in gex-kernel's ucontext */
struct gex_x86_64_sigcontext
{
    ULONG64 r8, r9, r10, r11, r12, r13, r14, r15;
    ULONG64 rdi, rsi, rbp, rbx, rdx, rax, rcx, rsp, rip, eflags;
    WORD    cs, gs, fs, ss;
    ULONG64 err, trapno, oldmask, cr2, fpstate;
    ULONG64 reserved[8];
};
struct gex_x86_64_ucontext
{
    ULONG64 flags, link;
    ULONG64 ss_sp;
    DWORD   ss_flags, pad;
    ULONG64 ss_size;
    struct gex_x86_64_sigcontext sc;
};
C_ASSERT( offsetof(struct gex_x86_64_ucontext, sc) == 40 );

extern void __gex_signal_frames( volatile unsigned long long *x, unsigned long long pc );

static __thread int in_guest_signal;

static void __wine_gex_signal( int ctx, unsigned int pc )
{
    in_guest_signal++;
    __gex_signal_frames( (volatile unsigned long long *)GEX_CTXP( ctx ), pc );
    in_guest_signal--;
}

static void sc_to_context( const struct gex_x86_64_sigcontext *sc, CONTEXT *context )
{
    memset( context, 0, sizeof(*context) );
    context->ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER | CONTEXT_SEGMENTS;
    context->Rax = sc->rax; context->Rbx = sc->rbx; context->Rcx = sc->rcx; context->Rdx = sc->rdx;
    context->Rsi = sc->rsi; context->Rdi = sc->rdi; context->Rbp = sc->rbp; context->Rsp = sc->rsp;
    context->R8  = sc->r8;  context->R9  = sc->r9;  context->R10 = sc->r10; context->R11 = sc->r11;
    context->R12 = sc->r12; context->R13 = sc->r13; context->R14 = sc->r14; context->R15 = sc->r15;
    context->Rip = sc->rip; context->EFlags = sc->eflags;
    context->SegCs = cs64_sel; context->SegSs = ds64_sel; context->SegDs = ds64_sel;
    context->SegEs = ds64_sel; context->SegFs = sc->fs; context->SegGs = ds64_sel;
}

static void context_to_sc( const CONTEXT *context, struct gex_x86_64_sigcontext *sc )
{
    DWORD flags = context->ContextFlags & ~CONTEXT_AMD64;
    if (flags & CONTEXT_INTEGER)
    {
        sc->rax = context->Rax; sc->rbx = context->Rbx; sc->rcx = context->Rcx;
        sc->rdx = context->Rdx; sc->rsi = context->Rsi; sc->rdi = context->Rdi;
        sc->r8  = context->R8;  sc->r9  = context->R9;  sc->r10 = context->R10;
        sc->r11 = context->R11; sc->r12 = context->R12; sc->r13 = context->R13;
        sc->r14 = context->R14; sc->r15 = context->R15;
    }
    if (flags & CONTEXT_CONTROL)
    {
        sc->rbp = context->Rbp; sc->rsp = context->Rsp; sc->rip = context->Rip;
        sc->eflags = context->EFlags;
    }
}

/* the thread's state at the signal, the guest's or the syscall frame's */
static void signal_wait_suspend( void *ucontext )
{
    struct thread_data *data = get_thread_data();

    if (!data->teb)
    {
        server_select( NULL, 0, SELECT_INTERRUPTIBLE, 0, NULL, NULL );
    }
    else if (in_guest_signal)
    {
        struct gex_x86_64_ucontext *uc = ucontext;
        CONTEXT context;

        sc_to_context( &uc->sc, &context );
        context.ContextFlags |= CONTEXT_EXCEPTION_REPORTING;
        wait_suspend( &context );
        context_to_sc( &context, &uc->sc );
    }
    else
    {
        struct xcontext context;

        memset( &context, 0, sizeof(context) );
        context.c.ContextFlags = CONTEXT_FULL | CONTEXT_EXCEPTION_REQUEST;
        NtGetContextThread( GetCurrentThread(), &context.c );
        wait_suspend( &context.c );
        NtSetContextThread( GetCurrentThread(), &context.c );
    }
}

static void usr1_handler( int signal, siginfo_t *siginfo, void *ucontext )
{
    signal_wait_suspend( ucontext );
}

static void quit_handler( int signal, siginfo_t *siginfo, void *ucontext )
{
    abort_thread( 0 );
}

static void int_handler( int signal, siginfo_t *siginfo, void *ucontext )
{
    HANDLE handle;

    if (!p__wine_ctrl_routine) return;
    if (!NtCreateThreadEx( &handle, THREAD_ALL_ACCESS, NULL, NtCurrentProcess(),
                           p__wine_ctrl_routine, 0 /* CTRL_C_EVENT */, 0, 0, 0, 0, NULL ))
        NtClose( handle );
}

static void install_signal_handlers(void)
{
    struct sigaction sig_act;

    memset( &sig_act, 0, sizeof(sig_act) );
    sig_act.sa_mask = server_block_set;
    sig_act.sa_flags = SA_SIGINFO | SA_RESTART;
    sig_act.sa_sigaction = usr1_handler;
    if (sigaction( SIGUSR1, &sig_act, NULL ) == -1) goto error;
    sig_act.sa_sigaction = quit_handler;
    if (sigaction( SIGQUIT, &sig_act, NULL ) == -1) goto error;
    sig_act.sa_sigaction = int_handler;
    if (sigaction( SIGINT, &sig_act, NULL ) == -1) goto error;
    gex_signal_hook( GEX_ADDR( __wine_gex_signal ) );
    return;
error:
    perror( "sigaction" );
    exit(1);
}

/* The page after the USD: the dispatcher pointer x86-64 stubs call
 * through, and the two entries it and PE ntdll's Unix-call pointer name. */
static void init_entry_page(void)
{
    BYTE *page = (BYTE *)user_shared_data + page_size;

    if (anon_mmap_fixed( page, page_size, PROT_READ | PROT_WRITE | PROT_EXEC, 0 ) != page)
    {
        ERR( "cannot map the system call page at %p\n", page );
        exit(1);
    }
    *(ULONG64 *)page = W64_SYSCALL_ENTRY;
    memcpy( (BYTE *)W64_SYSCALL_ENTRY, "\x0f\x0b", 2 );    /* ud2 */
    memcpy( (BYTE *)W64_UNIX_CALL_ENTRY, "\x0f\x0b", 2 );  /* ud2 */
}

void signal_init_process( TEB *teb )
{
    if (user_shared_data->XState.Size) xstate_size = user_shared_data->XState.Size - sizeof(XSAVE_FORMAT);
    frame_size = offsetof( struct syscall_frame, xstate ) + xstate_size;
    xstate_extended_features = user_shared_data->XState.EnabledFeatures & ~(UINT64)3;

    init_entry_page();
    alloc_syscall_frame( (frame_size + 63) & ~63 );
    signal_alloc_thread( teb );

    /* gex's side: both entries and the program's faults come by the hook */
    gex_fault_hook( GEX_ADDR( __wine_gex_fault ) );
    install_signal_handlers();
}

/***********************************************************************
 *           signal_start_thread
 *
 * As x86-64's init_syscall_frame: the frame set up for LdrInitializeThunk
 * with the initial context, then user mode, run through gex. The thread
 * never comes back here: it ends in NtTerminateThread.
 */
void signal_start_thread( PRTL_THREAD_START_ROUTINE entry, void *arg, TEB *teb )
{
    struct thread_data *data = get_thread_data();
    struct teb_data *tdata = (struct teb_data *)&teb->GdiTebBatch;
    struct amd64_thread_data *thread_data = (struct amd64_thread_data *)tdata->cpu_data;
    struct syscall_frame *frame;
    CONTEXT *cctx, context = { 0 };
    int ctx = cur_ctx();

    if (!tdata->syscall_frame)
        tdata->syscall_frame = (struct syscall_frame *)(((ULONG_PTR)&context - 0x10000 - thread_data->frame_size) & ~(ULONG_PTR)63);
    frame = tdata->syscall_frame;
    frame->restore_flags = 0;
    frame->syscall_cfa = NULL;
    frame->prev_frame = NULL;

    /* the program's view of the TEB: %gs's base */
    GEX_C64( GEX_CTX_TPIDRRO ) = (ULONG_PTR)teb;

    context.ContextFlags = CONTEXT_ALL | CONTEXT_EXCEPTION_REPORTING | CONTEXT_EXCEPTION_ACTIVE;
    context.Rcx    = (ULONG_PTR)entry;
    context.Rdx    = (ULONG_PTR)arg;
    context.Rsp    = (ULONG_PTR)teb->Tib.StackBase - 0x28;
    context.Rip    = (ULONG_PTR)pRtlUserThreadStart;
    context.SegCs  = cs64_sel;
    context.SegDs  = ds64_sel;
    context.SegEs  = ds64_sel;
    context.SegFs  = thread_data->fs;
    context.SegGs  = ds64_sel;
    context.SegSs  = ds64_sel;
    context.EFlags = 0x200;
    context.FltSave.ControlWord = 0x27f;
    context.FltSave.MxCsr = context.MxCsr = 0x1f80;

    if (data->suspend) wait_suspend( &context );

    cctx = (CONTEXT *)((ULONG_PTR)context.Rsp & ~15) - 1;
    *cctx = context;
    cctx->ContextFlags = CONTEXT_FULL | CONTEXT_FLOATING_POINT;
    memset( &frame->xstate, 0, sizeof(frame->xstate) );
    signal_set_full_context( cctx );

    frame->cs  = cs64_sel;
    frame->ss  = ds64_sel;
    frame->rsp = (ULONG64)cctx - 8;
    frame->rip = (ULONG64)pLdrInitializeThunk;
    frame->rcx = (ULONG64)cctx;
    frame->restore_flags |= CONTEXT_FULL | CONTEXT_FLOATING_POINT;
    /* (before the context is written: a system call of ours uses its
     * registers) */
    pthread_sigmask( SIG_UNBLOCK, &server_block_set, NULL );
    frame_to_ctx( ctx, frame );

    /* user mode, for good: each call-out runs the program until something
     * leaves it (a resume elsewhere), and the next goes on from there */
    for (;;)
    {
        ULONG64 pc = GEX_C64( GEX_CTX_PC );
        gex_call_out( ctx, (int64_t)pc, 0, 0 );
    }
}

#endif  /* __wasm64__ */
