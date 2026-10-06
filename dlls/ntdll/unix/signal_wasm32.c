/*
 * wasm32 signal handling routines: native Wine on wasm32 under gex
 * (docs/wine-native.md in the gex tree, §4.0 W3). The program's own code is
 * i386, run by gex; Wine's is wasm. The i386 register file is gex's thread
 * context, the "user mode" state a system call saves in its frame.
 *
 * The frame, context and dispatcher layouts are i386's (signal_i386.c,
 * from which the portable parts are copied); what i386 does with signal
 * contexts and assembly is done here through gex: system calls arrive as a
 * C call from the PE side (__wine_gex_syscall, through gexrt), faults in
 * the program's code through gex's fault hook (__wine_gex_fault), and the
 * program's code runs under gex.call_out.
 *
 * Copyright 1999, 2005 Alexandre Julliard (the parts from signal_i386.c)
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

#ifdef __wasm32__

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
#include <unistd.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winternl.h"
#include "ddk/wdm.h"
#include "unix_private.h"
#include "ntsyscalls.h"
#include "wine/debug.h"
#include "wine/gex/gex_abi.h"
#include "wine/gex/provider.h"

WINE_DEFAULT_DEBUG_CHANNEL(seh);

/* gex's side: the thread's register context, a guest call, the hooks. */
unsigned long __gex_get_ctx(void);
__attribute__((import_module("gex"), import_name("fault_hook"))) void gex_fault_hook( int idx );
__attribute__((import_module("gex"), import_name("unix_dispatcher"))) int gex_unix_dispatcher( int op, int idx );

static inline int cur_ctx(void) { return (int)__gex_get_ctx(); }

/* stack layout when calling KiUserExceptionDispatcher */
struct exc_stack_layout
{
    EXCEPTION_RECORD *rec_ptr;       /* 000 first arg for KiUserExceptionDispatcher */
    CONTEXT          *context_ptr;   /* 004 second arg for KiUserExceptionDispatcher */
    EXCEPTION_RECORD  rec;           /* 008 */
    CONTEXT           context;       /* 058 */
    CONTEXT_EX        context_ex;    /* 324 */
    DWORD             align;         /* 33c */
};
C_ASSERT( offsetof(struct exc_stack_layout, context) == 0x58 );
C_ASSERT( sizeof(struct exc_stack_layout) == 0x340 );

/* stack layout when calling KiUserApcDispatcher */
struct apc_stack_layout
{
    PNTAPCFUNC        func;          /* 000 */
    UINT              arg1;          /* 004 */
    UINT              arg2;          /* 008 */
    UINT              arg3;          /* 00c */
    UINT              alertable;     /* 010 */
    CONTEXT           context;       /* 014 */
    CONTEXT_EX        xctx;          /* 2e0 */
    UINT              unk2[4];       /* 2f8 */
};
C_ASSERT( offsetof(struct apc_stack_layout, context) == 0x14 );
C_ASSERT( sizeof(struct apc_stack_layout) == 0x308 );

/* stack layout when calling KiUserCallbackDispatcher */
struct callback_stack_layout
{
    ULONG             eip;           /* 000 */
    ULONG             id;            /* 004 */
    void             *args;          /* 008 */
    ULONG             len;           /* 00c */
    ULONG             unk[2];        /* 010 */
    ULONG             esp;           /* 018 */
    BYTE              args_data[0];  /* 01c */
};
C_ASSERT( sizeof(struct callback_stack_layout) == 0x1c );

struct syscall_frame
{
    UINT                  restore_flags;  /* 000 */
    UINT                  eflags;         /* 004 */
    UINT                  eip;            /* 008 */
    UINT                  esp;            /* 00c */
    WORD                  cs;             /* 010 */
    WORD                  ss;             /* 012 */
    WORD                  ds;             /* 014 */
    WORD                  es;             /* 016 */
    WORD                  fs;             /* 018 */
    WORD                  gs;             /* 01a */
    UINT                  eax;            /* 01c */
    UINT                  ebx;            /* 020 */
    UINT                  ecx;            /* 024 */
    UINT                  edx;            /* 028 */
    UINT                  edi;            /* 02c */
    UINT                  esi;            /* 030 */
    UINT                  ebp;            /* 034 */
    void                 *syscall_cfa;    /* 038 */
    struct syscall_frame *prev_frame;     /* 03c */
    union                                 /* 040 */
    {
        XSAVE_FORMAT       xsave;
        FLOATING_SAVE_AREA fsave;
    } u;
    DECLSPEC_ALIGN(64) XSAVE_AREA_HEADER xstate; /* 240 */
};

C_ASSERT( sizeof(struct syscall_frame) == 0x280 );

#define RESTORE_FLAGS_INCOMPLETE_FRAME_CONTEXT 0x00008000

struct x86_thread_data
{
    UINT               fs;            /* 1d4 TEB selector */
    UINT               gs;            /* 1d8 libc selector; update winebuild if you move this! */
    UINT               dr0;           /* 1dc debug registers */
    UINT               dr1;           /* 1e0 */
    UINT               dr2;           /* 1e4 */
    UINT               dr3;           /* 1e8 */
    UINT               dr6;           /* 1ec */
    UINT               dr7;           /* 1f0 */
    UINT               frame_size;    /* 1f4 syscall frame size including xstate */
    UINT               fpcw;          /* 1f8 FPU control word */
};

C_ASSERT( sizeof(struct x86_thread_data) <= sizeof(((struct teb_data *)0)->cpu_data) );
C_ASSERT( offsetof( TEB, GdiTebBatch ) + offsetof( struct x86_thread_data, gs ) == 0x1d8 );
C_ASSERT( offsetof( TEB, GdiTebBatch ) + offsetof( struct x86_thread_data, frame_size ) == 0x1f4 );
C_ASSERT( offsetof( TEB, GdiTebBatch ) + offsetof( struct x86_thread_data, fpcw ) == 0x1f8 );

static unsigned int frame_size;
static unsigned int xstate_size = sizeof(XSAVE_AREA_HEADER);
static UINT64 xstate_extended_features;

static inline struct x86_thread_data *x86_thread_data( struct thread_data *data )
{
    if (!data->teb) return NULL;
    return (struct x86_thread_data *)get_teb_data(data)->cpu_data;
}


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
 * The i386 state in gex's context and the syscall frame
 */

static inline UINT get_eflags( int ctx )
{
    UINT f = 0x202;  /* IF, and bit 1 */
    if (GEX_C8( GEX_CTX_CF )) f |= 0x001;
    if (GEX_C8( GEX_CTX_PF )) f |= 0x004;
    if (GEX_C8( GEX_CTX_AF )) f |= 0x010;
    if (GEX_C8( GEX_CTX_ZF )) f |= 0x040;
    if (GEX_C8( GEX_CTX_NF )) f |= 0x080;
    if (GEX_C8( GEX_CTX_DF )) f |= 0x400;
    if (GEX_C8( GEX_CTX_VF )) f |= 0x800;
    return f;
}

static inline void set_eflags( int ctx, UINT f )
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
        memcpy( &xs->FloatRegisters[i], GEX_P( (uint32_t)ctx + GEX_CTX_X87 + 16 * r ), 10 );
        if (tags & (1u << r)) xs->TagWord |= 1u << r;
    }
    xs->MxCsr = GEX_C32( GEX_CTX_MXCSR );
    xs->MxCsr_Mask = 0x2ffff;
    for (i = 0; i < 8; i++) memcpy( &xs->XmmRegisters[i], GEX_P( (uint32_t)ctx + GEX_CTX_VN(i) ), 16 );
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
        memcpy( GEX_P( (uint32_t)ctx + GEX_CTX_X87 + 16 * r ), &xs->FloatRegisters[i], 10 );
        if (xs->TagWord & (1u << r)) tags |= 1u << r;
    }
    GEX_C32( GEX_CTX_X87_TAGS ) = tags;
    GEX_C32( GEX_CTX_MXCSR ) = xs->MxCsr;
    for (i = 0; i < 8; i++) memcpy( GEX_P( (uint32_t)ctx + GEX_CTX_VN(i) ), &xs->XmmRegisters[i], 16 );
}

/* The program's state, as a system call or a fault finds it, into the
 * frame. */
static void ctx_to_frame( int ctx, struct syscall_frame *frame, UINT eip, UINT esp )
{
    frame->restore_flags = 0;
    frame->eflags = get_eflags( ctx );
    frame->eip = eip;
    frame->esp = esp;
    frame->cs = 0x23;
    frame->ss = frame->ds = frame->es = frame->gs = 0x2b;
    frame->fs = 0x63;
    frame->eax = gp_reg( ctx, R_EAX );
    frame->ebx = gp_reg( ctx, R_EBX );
    frame->ecx = gp_reg( ctx, R_ECX );
    frame->edx = gp_reg( ctx, R_EDX );
    frame->esi = (UINT)GEX_C64( GEX_CTX_XN(6) );
    frame->edi = (UINT)GEX_C64( GEX_CTX_XN(7) );
    frame->ebp = (UINT)GEX_C64( GEX_CTX_XN(5) );
    ctx_to_xsave( ctx, &frame->u.xsave );
    frame->xstate.Mask = XSTATE_MASK_LEGACY;
    frame->xstate.CompactionMask = 0;
}

/* And back, as the frame now says. */
static void frame_to_ctx( int ctx, const struct syscall_frame *frame )
{
    gp_set( ctx, R_EAX, frame->eax );
    gp_set( ctx, R_EBX, frame->ebx );
    gp_set( ctx, R_ECX, frame->ecx );
    gp_set( ctx, R_EDX, frame->edx );
    GEX_C64( GEX_CTX_XN(6) ) = frame->esi;
    GEX_C64( GEX_CTX_XN(7) ) = frame->edi;
    GEX_C64( GEX_CTX_XN(5) ) = frame->ebp;
    gp_set( ctx, R_ESP, frame->esp );
    set_eflags( ctx, frame->eflags );
    if (frame->restore_flags & (CONTEXT_FLOATING_POINT | CONTEXT_EXTENDED_REGISTERS | CONTEXT_XSTATE))
        xsave_to_ctx( ctx, &frame->u.xsave );
    GEX_C64( GEX_CTX_PC ) = frame->eip;
}

/***********************************************************************
 *           signal_set_full_context
 */
NTSTATUS signal_set_full_context( CONTEXT *context )
{
    NTSTATUS status = NtSetContextThread( GetCurrentThread(), context );

    if (!status && (context->ContextFlags & CONTEXT_INTEGER) == CONTEXT_INTEGER)
        get_syscall_frame(get_thread_data())->restore_flags |= LOWORD(CONTEXT_INTEGER);
    return status;
}


/***********************************************************************
 *              get_native_context
 */
void *get_native_context( CONTEXT *context )
{
    return is_old_wow64() ? NULL : context;
}


/***********************************************************************
 *              get_wow_context
 */
void *get_wow_context( CONTEXT *context )
{
    return is_old_wow64() ? context : NULL;
}


/***********************************************************************
 *              NtSetContextThread  (NTDLL.@)
 *              ZwSetContextThread  (NTDLL.@)
 */
NTSTATUS WINAPI NtSetContextThread( HANDLE handle, const CONTEXT *context )
{
    NTSTATUS ret = STATUS_SUCCESS;
    struct thread_data *data = get_thread_data();
    struct syscall_frame *frame = get_syscall_frame( data );
    struct x86_thread_data *x86_data = x86_thread_data( data );
    DWORD flags = context->ContextFlags & ~CONTEXT_i386;
    BOOL self = (handle == GetCurrentThread());

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
        self = (x86_data->dr0 == context->Dr0 &&
                x86_data->dr1 == context->Dr1 &&
                x86_data->dr2 == context->Dr2 &&
                x86_data->dr3 == context->Dr3 &&
                x86_data->dr6 == context->Dr6 &&
                x86_data->dr7 == context->Dr7);

    if (!self)
    {
        ret = set_thread_context( handle, context, &self, IMAGE_FILE_MACHINE_I386 );
        if (ret || !self) return ret;
        if (flags & CONTEXT_DEBUG_REGISTERS)
        {
            x86_data->dr0 = context->Dr0;
            x86_data->dr1 = context->Dr1;
            x86_data->dr2 = context->Dr2;
            x86_data->dr3 = context->Dr3;
            x86_data->dr6 = context->Dr6;
            x86_data->dr7 = context->Dr7;
        }
    }

    if (flags & CONTEXT_INTEGER)
    {
        frame->eax = context->Eax;
        frame->ebx = context->Ebx;
        frame->ecx = context->Ecx;
        frame->edx = context->Edx;
        frame->esi = context->Esi;
        frame->edi = context->Edi;
    }
    if (flags & CONTEXT_CONTROL)
    {
        frame->esp    = context->Esp;
        frame->ebp    = context->Ebp;
        frame->eip    = context->Eip;
        frame->eflags = context->EFlags;
        frame->cs     = context->SegCs;
        frame->ss     = context->SegSs;
    }
    if (flags & CONTEXT_SEGMENTS)
    {
        frame->ds = context->SegDs;
        frame->es = context->SegEs;
        frame->fs = context->SegFs;
        frame->gs = context->SegGs;
    }
    if (flags & CONTEXT_EXTENDED_REGISTERS)
    {
        memcpy( &frame->u.xsave, context->ExtendedRegisters, sizeof(frame->u.xsave) );
        /* reset the current interrupt status */
        frame->u.xsave.StatusWord &= frame->u.xsave.ControlWord | 0xff80;
        frame->xstate.Mask |= XSTATE_MASK_LEGACY;
    }
    else if (flags & CONTEXT_FLOATING_POINT)
    {
        if (user_shared_data->ProcessorFeatures[PF_XMMI_INSTRUCTIONS_AVAILABLE])
        {
            fpu_to_fpux( &frame->u.xsave, &context->FloatSave );
        }
        else
        {
            frame->u.fsave = context->FloatSave;
        }
        frame->xstate.Mask |= XSTATE_MASK_LEGACY_FLOATING_POINT;
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
 *
 * Note: we use a small assembly wrapper to save the necessary registers
 *       in case we are fetching the context of the current thread.
 */
NTSTATUS WINAPI NtGetContextThread( HANDLE handle, CONTEXT *context )
{
    struct thread_data *data = get_thread_data();
    struct syscall_frame *frame = get_syscall_frame( data );
    struct x86_thread_data *x86_data = x86_thread_data( data );
    DWORD needed_flags = context->ContextFlags & ~CONTEXT_i386;
    BOOL self = (handle == GetCurrentThread());
    NTSTATUS ret;

    if (self && !frame) return STATUS_ACCESS_DENIED;

    /* debug registers require a server call */
    if (needed_flags & CONTEXT_DEBUG_REGISTERS) self = FALSE;

    if (!self)
    {
        if ((ret = get_thread_context( handle, context, &self, IMAGE_FILE_MACHINE_I386 ))) return ret;
    }

    if (self)
    {
        if (needed_flags & CONTEXT_INTEGER)
        {
            context->Eax = frame->eax;
            context->Ebx = frame->ebx;
            context->Ecx = frame->ecx;
            context->Edx = frame->edx;
            context->Esi = frame->esi;
            context->Edi = frame->edi;
            context->ContextFlags |= CONTEXT_INTEGER;
        }
        if (needed_flags & CONTEXT_CONTROL)
        {
            context->Esp    = frame->esp;
            context->Ebp    = frame->ebp;
            context->Eip    = frame->eip;
            context->EFlags = frame->eflags;
            context->SegCs  = frame->cs;
            context->SegSs  = frame->ss;
            context->ContextFlags |= CONTEXT_CONTROL;
        }
        if (needed_flags & CONTEXT_SEGMENTS)
        {
            context->SegDs = frame->ds;
            context->SegEs = frame->es;
            context->SegFs = frame->fs;
            context->SegGs = frame->gs;
            context->ContextFlags |= CONTEXT_SEGMENTS;
        }
        if (needed_flags & CONTEXT_FLOATING_POINT)
        {
            if (!user_shared_data->ProcessorFeatures[PF_XMMI_INSTRUCTIONS_AVAILABLE])
            {
                context->FloatSave = frame->u.fsave;
            }
            else if (!user_shared_data->XState.CompactionEnabled ||
                     (frame->xstate.Mask & XSTATE_MASK_LEGACY_FLOATING_POINT))
            {
                fpux_to_fpu( &context->FloatSave, &frame->u.xsave );
            }
            else
            {
                memset( &context->FloatSave, 0, sizeof(context->FloatSave) );
                context->FloatSave.ControlWord = 0x37f;
            }
            context->ContextFlags |= CONTEXT_FLOATING_POINT;
        }
        if (needed_flags & CONTEXT_EXTENDED_REGISTERS)
        {
            XSAVE_FORMAT *xs = (XSAVE_FORMAT *)context->ExtendedRegisters;

            if (!user_shared_data->XState.CompactionEnabled ||
                (frame->xstate.Mask & XSTATE_MASK_LEGACY_FLOATING_POINT))
            {
                memcpy( xs, &frame->u.xsave, FIELD_OFFSET( XSAVE_FORMAT, MxCsr ));
                memcpy( xs->FloatRegisters, frame->u.xsave.FloatRegisters,
                        sizeof( xs->FloatRegisters ));
            }
            else
            {
                memset( xs, 0, FIELD_OFFSET( XSAVE_FORMAT, MxCsr ));
                memset( xs->FloatRegisters, 0, sizeof( xs->FloatRegisters ));
                xs->ControlWord = 0x37f;
            }

            if (!user_shared_data->XState.CompactionEnabled ||
                (frame->xstate.Mask & XSTATE_MASK_LEGACY_SSE))
            {
                memcpy( xs->XmmRegisters, frame->u.xsave.XmmRegisters, sizeof( xs->XmmRegisters ));
                xs->MxCsr      = frame->u.xsave.MxCsr;
                xs->MxCsr_Mask = frame->u.xsave.MxCsr_Mask;
            }
            else
            {
                memset( xs->XmmRegisters, 0, sizeof( xs->XmmRegisters ));
                xs->MxCsr      = 0x1f80;
                xs->MxCsr_Mask = 0x2ffff;
            }

            context->ContextFlags |= CONTEXT_EXTENDED_REGISTERS;
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
                /* copy_xstate may use avx in memcpy, restore xstate not to break the tests. */
                frame->restore_flags |= CONTEXT_XSTATE;
            }
        }
        /* update the cached version of the debug registers */
        if (needed_flags & CONTEXT_DEBUG_REGISTERS)
        {
            x86_data->dr0 = context->Dr0;
            x86_data->dr1 = context->Dr1;
            x86_data->dr2 = context->Dr2;
            x86_data->dr3 = context->Dr3;
            x86_data->dr6 = context->Dr6;
            x86_data->dr7 = context->Dr7;
        }
        set_context_exception_reporting_flags( &context->ContextFlags, CONTEXT_SERVICE_ACTIVE );
    }

    if (context->ContextFlags & (CONTEXT_INTEGER & ~CONTEXT_i386))
        TRACE( "%p: eax=%08x ebx=%08x ecx=%08x edx=%08x esi=%08x edi=%08x\n", handle,
               context->Eax, context->Ebx, context->Ecx, context->Edx, context->Esi, context->Edi );
    if (context->ContextFlags & (CONTEXT_CONTROL & ~CONTEXT_i386))
        TRACE( "%p: ebp=%08x esp=%08x eip=%08x cs=%04x ss=%04x flags=%08x\n", handle,
               context->Ebp, context->Esp, context->Eip, context->SegCs, context->SegSs, context->EFlags );
    if (context->ContextFlags & (CONTEXT_SEGMENTS & ~CONTEXT_i386))
        TRACE( "%p: ds=%04x es=%04x fs=%04x gs=%04x\n", handle,
               context->SegDs, context->SegEs, context->SegFs, context->SegGs );
    if (context->ContextFlags & (CONTEXT_DEBUG_REGISTERS & ~CONTEXT_i386))
        TRACE( "%p: dr0=%08x dr1=%08x dr2=%08x dr3=%08x dr6=%08x dr7=%08x\n", handle,
               context->Dr0, context->Dr1, context->Dr2, context->Dr3, context->Dr6, context->Dr7 );

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
    ULONG esp = context ? context->Esp : frame->esp;
    struct apc_stack_layout *stack = (struct apc_stack_layout *)esp - 1;

    if (flags) FIXME( "flags %#x are not supported.\n", flags );

    if (!context)
    {
        stack->context.ContextFlags = CONTEXT_FULL;
        NtGetContextThread( GetCurrentThread(), &stack->context );
        stack->context.Eax = status;
    }
    else memmove( &stack->context, context, sizeof(stack->context) );

    context_init_xstate( &stack->context, NULL );
    stack->func      = func;
    stack->arg1      = arg1;
    stack->arg2      = arg2;
    stack->arg3      = arg3;
    stack->alertable = TRUE;
    frame->ebp = stack->context.Ebp;
    frame->esp = (ULONG)stack;
    frame->eip = (ULONG)pKiUserApcDispatcher;
    return status;
}


/***********************************************************************
 *           call_raise_user_exception_dispatcher
 */
void call_raise_user_exception_dispatcher( struct thread_data *data )
{
    get_syscall_frame(data)->eip = (DWORD)pKiRaiseUserExceptionDispatcher;
}


/***********************************************************************
 *           call_user_exception_dispatcher
 */
NTSTATUS call_user_exception_dispatcher( struct thread_data *data, EXCEPTION_RECORD *rec, CONTEXT *context )
{
    struct syscall_frame *frame = get_syscall_frame( data );
    struct exc_stack_layout *stack;
    XSAVE_AREA_HEADER *src_xs;

    if (rec->ExceptionCode == EXCEPTION_BREAKPOINT) context->Eip--;

    stack = (struct exc_stack_layout *)((frame->esp - sizeof(*stack) - xstate_size) & ~(ULONG_PTR)63);
    stack->rec_ptr      = &stack->rec;
    stack->context_ptr  = &stack->context;
    stack->rec          = *rec;
    stack->context      = *context;

    if ((src_xs = xstate_from_context( context )))
    {
        XSAVE_AREA_HEADER *dst_xs = (XSAVE_AREA_HEADER *)(stack + 1);

        context_init_xstate( &stack->context, dst_xs );
        assert( !((ULONG_PTR)dst_xs & 63) );
        memset( dst_xs, 0, sizeof(*dst_xs) );
        dst_xs->CompactionMask = user_shared_data->XState.CompactionEnabled ? 0x8000000000000000 | xstate_extended_features : 0;
        copy_xstate( dst_xs, src_xs, src_xs->Mask );
    }
    else
    {
        context_init_xstate( &stack->context, NULL );
    }

    frame->esp = (ULONG)stack;
    frame->eip = (ULONG)pKiUserExceptionDispatcher;
    return STATUS_SUCCESS;
}



/***********************************************************************
 * System calls from the PE side
 *
 * gexrt's __gexw_syscall (libs/gexrt/wasm32.c) lays a call out as i386's
 * stubs leave it -- [esp] the address it comes back to, [esp+4] the same,
 * the argument words from [esp+8] -- with the number in EAX and PC at that
 * address, and calls here. On return EAX holds the status and PC where the
 * program goes on: the same address, unless the call sent it elsewhere
 * (an APC, an exception, NtContinue), which gexrt then runs.
 */

#define A(i) args[i]
#define CALL_0(f)  ((NTSTATUS (*)(void))f)()
#define CALL_4(f)  ((NTSTATUS (*)(UINT))f)(A(0))
#define CALL_8(f)  ((NTSTATUS (*)(UINT,UINT))f)(A(0),A(1))
#define CALL_12(f) ((NTSTATUS (*)(UINT,UINT,UINT))f)(A(0),A(1),A(2))
#define CALL_16(f) ((NTSTATUS (*)(UINT,UINT,UINT,UINT))f)(A(0),A(1),A(2),A(3))
#define CALL_20(f) ((NTSTATUS (*)(UINT,UINT,UINT,UINT,UINT))f)(A(0),A(1),A(2),A(3),A(4))
#define CALL_24(f) ((NTSTATUS (*)(UINT,UINT,UINT,UINT,UINT,UINT))f)(A(0),A(1),A(2),A(3),A(4),A(5))
#define CALL_28(f) ((NTSTATUS (*)(UINT,UINT,UINT,UINT,UINT,UINT,UINT))f)(A(0),A(1),A(2),A(3),A(4),A(5),A(6))
#define CALL_32(f) ((NTSTATUS (*)(UINT,UINT,UINT,UINT,UINT,UINT,UINT,UINT))f)(A(0),A(1),A(2),A(3),A(4),A(5),A(6),A(7))
#define CALL_36(f) ((NTSTATUS (*)(UINT,UINT,UINT,UINT,UINT,UINT,UINT,UINT,UINT))f)(A(0),A(1),A(2),A(3),A(4),A(5),A(6),A(7),A(8))
#define CALL_40(f) ((NTSTATUS (*)(UINT,UINT,UINT,UINT,UINT,UINT,UINT,UINT,UINT,UINT))f)(A(0),A(1),A(2),A(3),A(4),A(5),A(6),A(7),A(8),A(9))
#define CALL_44(f) ((NTSTATUS (*)(UINT,UINT,UINT,UINT,UINT,UINT,UINT,UINT,UINT,UINT,UINT))f)(A(0),A(1),A(2),A(3),A(4),A(5),A(6),A(7),A(8),A(9),A(10))
#define CALL_48(f) ((NTSTATUS (*)(UINT,UINT,UINT,UINT,UINT,UINT,UINT,UINT,UINT,UINT,UINT,UINT))f)(A(0),A(1),A(2),A(3),A(4),A(5),A(6),A(7),A(8),A(9),A(10),A(11))
#define CALL_52(f) ((NTSTATUS (*)(UINT,UINT,UINT,UINT,UINT,UINT,UINT,UINT,UINT,UINT,UINT,UINT,UINT))f)(A(0),A(1),A(2),A(3),A(4),A(5),A(6),A(7),A(8),A(9),A(10),A(11),A(12))
#define CALL_56(f) ((NTSTATUS (*)(UINT,UINT,UINT,UINT,UINT,UINT,UINT,UINT,UINT,UINT,UINT,UINT,UINT,UINT))f)(A(0),A(1),A(2),A(3),A(4),A(5),A(6),A(7),A(8),A(9),A(10),A(11),A(12),A(13))
#define CALL_60(f) ((NTSTATUS (*)(UINT,UINT,UINT,UINT,UINT,UINT,UINT,UINT,UINT,UINT,UINT,UINT,UINT,UINT,UINT))f)(A(0),A(1),A(2),A(3),A(4),A(5),A(6),A(7),A(8),A(9),A(10),A(11),A(12),A(13),A(14))
#define CALL_64(f) ((NTSTATUS (*)(UINT,UINT,UINT,UINT,UINT,UINT,UINT,UINT,UINT,UINT,UINT,UINT,UINT,UINT,UINT,UINT))f)(A(0),A(1),A(2),A(3),A(4),A(5),A(6),A(7),A(8),A(9),A(10),A(11),A(12),A(13),A(14),A(15))
#define CALL_68(f) ((NTSTATUS (*)(UINT,UINT,UINT,UINT,UINT,UINT,UINT,UINT,UINT,UINT,UINT,UINT,UINT,UINT,UINT,UINT,UINT))f)(A(0),A(1),A(2),A(3),A(4),A(5),A(6),A(7),A(8),A(9),A(10),A(11),A(12),A(13),A(14),A(15),A(16))

/* A service table's function `id` (ntdll's, or win32u's once win32u.so has
 * registered it): an indirect call typed by its argument bytes, every
 * argument a 32-bit word (winebuild allows no wider ones in a system call;
 * a call of the wrong type traps, which an unimplemented stub's would). */
static NTSTATUS call_table( const SYSTEM_SERVICE_TABLE *table, UINT id, const UINT *args )
{
    void *f;
    if (id >= table->ServiceLimit) return STATUS_INVALID_SYSTEM_SERVICE;
    f = (void *)table->ServiceTable[id];
    switch (table->ArgumentTable[id])
    {
    case 0: return CALL_0(f);   case 4: return CALL_4(f);   case 8: return CALL_8(f);
    case 12: return CALL_12(f); case 16: return CALL_16(f); case 20: return CALL_20(f);
    case 24: return CALL_24(f); case 28: return CALL_28(f); case 32: return CALL_32(f);
    case 36: return CALL_36(f); case 40: return CALL_40(f); case 44: return CALL_44(f);
    case 48: return CALL_48(f); case 52: return CALL_52(f); case 56: return CALL_56(f);
    case 60: return CALL_60(f); case 64: return CALL_64(f); case 68: return CALL_68(f);
    }
    return STATUS_INVALID_SYSTEM_SERVICE;
}

/* Native code may not be re-entered by a system call made from a callback
 * the call itself made (KeUserModeCallback): each nests a frame. */
void __wine_gex_syscall_body( int ctx )
{
    struct thread_data *data = get_thread_data();
    struct syscall_frame *frame = get_syscall_frame( data );
    UINT esp = gp_reg( ctx, R_ESP ), id = gp_reg( ctx, R_EAX ), table = (id >> 12) & 3;
    const UINT *args = (const UINT *)(ULONG_PTR)(esp + 8);
    NTSTATUS status;

    ctx_to_frame( ctx, frame, *(const UINT *)(ULONG_PTR)esp, esp );
    id &= 0xfff;
    {
        SYSTEM_SERVICE_TABLE *t = &get_teb_data( data )->syscall_table[table];
        status = t->ServiceTable ? call_table( t, id, args ) : STATUS_INVALID_SYSTEM_SERVICE;
    }
    frame = get_syscall_frame( data );
    if (!(frame->restore_flags & CONTEXT_INTEGER)) frame->eax = status;
    frame_to_ctx( ctx, frame );
}

/* The Unix side runs on the thread's own stack, below its syscall frame,
 * as i386's runs on the kernel stack: called from the PE side, the C stack
 * pointer is on the guest's stack, just above the arguments, and what Unix
 * code writes below the guest's stack pointer (a callback's or an APC's
 * frame) would land on its own frames. */
unsigned int __wine_gex_syscall_sp(void)
{
    return ((UINT)(ULONG_PTR)get_syscall_frame( get_thread_data() ) - 256) & ~15u;
}

extern void __wine_gex_syscall_switch( int ctx );
__asm__(
    "\t.globaltype __stack_pointer, i32\n"
    "\t.functype __wine_gex_syscall_sp () -> (i32)\n"
    "\t.functype __wine_gex_syscall_body (i32) -> ()\n"
    "\t.globl __wine_gex_syscall_switch\n"
    "\t.type __wine_gex_syscall_switch,@function\n"
    "__wine_gex_syscall_switch:\n"
    "\t.functype __wine_gex_syscall_switch (i32) -> ()\n"
    "\t.local i32\n"
    "\tglobal.get __stack_pointer\n"
    "\tlocal.set 1\n"
    "\tcall __wine_gex_syscall_sp\n"
    "\tglobal.set __stack_pointer\n"
    "\tlocal.get 0\n"
    "\tcall __wine_gex_syscall_body\n"
    "\tlocal.get 1\n"
    "\tglobal.set __stack_pointer\n"
    "\tend_function\n" );

/* Unix calls from the PE side (__wine_unix_call, through PE ntdll's
 * __wine_unix_call_dispatcher pointer, which load_ntdll_functions sets to
 * this): a unixlib's function table is in the same memory and function
 * table, so the call goes straight through. */
NTSTATUS WINAPI __wine_unix_call_dispatcher( unixlib_handle_t handle, unsigned int code, void *args )
{
    /* The program's registers are in the thread's context while it is in
     * native code (no syscall frame holds them), and every system call the
     * Unix side makes passes its arguments in that context's EBX..EBP
     * (gex's i386 personality): they are put back, as the program expects
     * callee-saved registers to be. */
    int ctx = cur_ctx(), i;
    unsigned long long regs[8], pc = GEX_C64( GEX_CTX_PC );
    NTSTATUS status;

    for (i = 0; i < 8; i++) regs[i] = GEX_C64( GEX_CTX_XN(i) );
    status = ((const unixlib_entry_t *)(UINT_PTR)handle)[code]( args );
    for (i = 0; i < 8; i++) GEX_C64( GEX_CTX_XN(i) ) = regs[i];
    GEX_C64( GEX_CTX_PC ) = pc;
    return status;
}

/* A Unix call from i386 code (a translated builtin DLL's winecrt0 calls
 * __wine_unix_call_dispatcher, which on wasm32 is gex's trampoline: the
 * stdcall arguments are above its two return addresses): gex calls this. */
static void __wine_gex_unix_call( int ctx )
{
    const UINT *w = (const UINT *)(ULONG_PTR)(gp_reg( ctx, R_ESP ) + 8);
    unixlib_handle_t handle = w[0] | ((UINT64)w[1] << 32);
    NTSTATUS status = __wine_unix_call_dispatcher( handle, w[2], (void *)(ULONG_PTR)w[3] );
    gp_set( ctx, R_EAX, status );
}

/* What i386 assembles: in this port it is never jumped to, only named
 * (ntdll's PE side reads its address and gexrt calls __wine_gex_syscall
 * instead). */
void __wine_syscall_dispatcher(void)
{
    abort();
}


/***********************************************************************
 * Faults in the program's code (gex's fault hook)
 */
/* As i386's: a general-protection fault at a privileged instruction. */
static inline DWORD is_privileged_instr( CONTEXT *context )
{
    BYTE instr[16];
    unsigned int i, len, prefix_count = 0;

    len = virtual_uninterrupted_read_memory( (BYTE *)context->Eip, instr, sizeof(instr) );

    for (i = 0; i < len; i++) switch (instr[i])
    {
    /* instruction prefixes */
    case 0x2e:  /* %cs: */
    case 0x36:  /* %ss: */
    case 0x3e:  /* %ds: */
    case 0x26:  /* %es: */
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
        switch(instr[i + 1])
        {
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
    UINT pc = (UINT)GEX_C64( GEX_CTX_PC );

    /* The fault is handled as i386's signal handlers handle one: the state
     * at the fault into a context, the record, and the exception dispatched
     * from the program's stack. A frame is borrowed for it. */
    ctx_to_frame( ctx, frame, pc, gp_reg( ctx, R_ESP ) );
    memset( &xcontext, 0, sizeof(xcontext) );
    xcontext.c.ContextFlags = CONTEXT_FULL | CONTEXT_EXTENDED_REGISTERS;
    NtGetContextThread( GetCurrentThread(), &xcontext.c );
    rec.ExceptionAddress = (void *)(ULONG_PTR)pc;
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
        /* gex resumes after the int3; the address is the int3's own, as
         * i386's SIGTRAP handler has it (the context's Eip goes back one in
         * call_user_exception_dispatcher) */
        rec.ExceptionAddress = (void *)(ULONG_PTR)(pc - 1);
        rec.NumberParameters = 1;
        break;
    }
    call_user_exception_dispatcher( data, &rec, &xcontext.c );
    frame_to_ctx( ctx, frame );
}


/***********************************************************************
 *           KeUserModeCallback
 *
 * The program's (PE side's) KiUserCallbackDispatcher, run nested through
 * gex until it comes back by NtCallbackReturn, which ends the callback's
 * guest run (gexrt unwinds to here).
 */
struct callback_return
{
    void   *ret_ptr;
    ULONG   ret_len;
    NTSTATUS status;
    int     done;
};

NTSTATUS KeUserModeCallback( ULONG id, const void *args, ULONG len, void **ret_ptr, ULONG *ret_len )
{
    struct thread_data *data = get_thread_data();
    struct syscall_frame *frame = get_syscall_frame( data ), *cb_frame;
    ULONG esp = (frame->esp - offsetof(struct callback_stack_layout, args_data[len])) & ~3;
    struct callback_stack_layout *stack = (struct callback_stack_layout *)esp;
    struct callback_return ret = { 0 };
    int ctx = cur_ctx();
    UINT saved_esp = gp_reg( ctx, R_ESP ), saved_pc = (UINT)GEX_C64( GEX_CTX_PC );
    void *saved_list = data->teb->Tib.ExceptionList;

    if ((char *)get_kernel_stack( data ) + min_kernel_stack > (char *)&frame) return STATUS_STACK_OVERFLOW;

    stack->eip  = frame->eip;
    stack->id   = id;
    stack->args = stack->args_data;
    stack->len  = len;
    stack->esp  = frame->esp;
    memcpy( stack->args_data, args, len );

    /* a frame of its own for the system calls the callback makes */
    cb_frame = (struct syscall_frame *)(((ULONG_PTR)&ret - 256 - frame_size) & ~63);  /* below this function's frame */
    cb_frame->prev_frame = frame;
    cb_frame->syscall_cfa = &ret;
    get_teb_data( data )->syscall_frame = cb_frame;

    /* the callback dispatcher is the PE side's: entered with the stack
     * layout at ESP, and it never returns (NtCallbackReturn ends it) */
    gp_set( ctx, R_ESP, esp );
    {
        uint32_t ra = (uint32_t)(ULONG_PTR)pKiUserCallbackDispatcher ^ 1u;
        GEX_C64( GEX_CTX_LAST_TARGET ) = (uint32_t)(ULONG_PTR)pKiUserCallbackDispatcher;
        gex_call_out( ctx, (int64_t)(uint32_t)(ULONG_PTR)pKiUserCallbackDispatcher, ra, esp );
    }
    get_teb_data( data )->syscall_frame = frame;
    data->teb->Tib.ExceptionList = saved_list;
    gp_set( ctx, R_ESP, saved_esp );
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
    frame->eip = (UINT)(ULONG_PTR)pKiUserCallbackDispatcher ^ 1u;
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
    struct x86_thread_data *thread_data = (struct x86_thread_data *)&teb->GdiTebBatch;

    thread_data->fs = 0x63;
    teb->WOW32Reserved = __wine_syscall_dispatcher;
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
 * safepoint: gex-kernel builds i386's frame on the guest's stack and gex
 * runs it through __wine_gex_signal (gex.signal_hook), which calls these
 * handlers with the frame's ucontext, the guest's state. One that arrives
 * in Unix code is taken when a system call returns (libgex), as on i386
 * when inside a system call: the state is the syscall frame's.
 */

/* gex-kernel's i386 frame (Linux's ia32 sigcontext, in the ucontext) */
struct gex_i386_sigcontext
{
    UINT gs, fs, es, ds, edi, esi, ebp, esp, ebx, edx, ecx, eax;
    UINT trapno, err, eip, cs, eflags, esp_at_signal, ss, fpstate, oldmask, cr2;
};
struct gex_i386_ucontext
{
    UINT flags, link, ss_sp, ss_flags, ss_size;
    struct gex_i386_sigcontext sc;
};

extern void __gex_signal_frames( volatile unsigned long long *x, unsigned long long pc );
__attribute__((import_module("gex"), import_name("signal_hook"))) void gex_signal_hook( int idx );

static __thread int in_guest_signal;

static void __wine_gex_signal( int ctx, unsigned int pc )
{
    in_guest_signal++;
    __gex_signal_frames( (volatile unsigned long long *)(ULONG_PTR)ctx, pc );
    in_guest_signal--;
}

static void sc_to_context( const struct gex_i386_sigcontext *sc, CONTEXT *context )
{
    memset( context, 0, sizeof(*context) );
    context->ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER | CONTEXT_SEGMENTS;
    context->Eax = sc->eax; context->Ebx = sc->ebx; context->Ecx = sc->ecx; context->Edx = sc->edx;
    context->Esi = sc->esi; context->Edi = sc->edi; context->Ebp = sc->ebp; context->Esp = sc->esp;
    context->Eip = sc->eip; context->EFlags = sc->eflags;
    context->SegCs = sc->cs; context->SegSs = sc->ss; context->SegDs = sc->ds;
    context->SegEs = sc->es; context->SegFs = sc->fs; context->SegGs = sc->gs;
}

static void context_to_sc( const CONTEXT *context, struct gex_i386_sigcontext *sc )
{
    DWORD flags = context->ContextFlags & ~CONTEXT_i386;
    if (flags & CONTEXT_INTEGER)
    {
        sc->eax = context->Eax; sc->ebx = context->Ebx; sc->ecx = context->Ecx;
        sc->edx = context->Edx; sc->esi = context->Esi; sc->edi = context->Edi;
    }
    if (flags & CONTEXT_CONTROL)
    {
        sc->ebp = context->Ebp; sc->esp = context->Esp; sc->eip = context->Eip;
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
        struct gex_i386_ucontext *uc = ucontext;
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
    gex_signal_hook( (int)(ULONG_PTR)__wine_gex_signal );
    return;
error:
    perror( "sigaction" );
    exit(1);
}

void signal_init_process( TEB *teb )
{
    if (user_shared_data->XState.Size) xstate_size = user_shared_data->XState.Size - sizeof(XSAVE_FORMAT);
    frame_size = offsetof( struct syscall_frame, xstate ) + xstate_size;
    xstate_extended_features = user_shared_data->XState.EnabledFeatures & ~(UINT64)3;

    alloc_syscall_frame( (frame_size + 63) & ~63 );
    signal_alloc_thread( teb );

    /* gex's side: system calls from the PE side, faults in the program */
    gex_unix_dispatcher( 0, (int)(ULONG_PTR)__wine_gex_syscall_switch );
    gex_unix_dispatcher( 2, (int)(ULONG_PTR)__wine_gex_unix_call );
    gex_unix_dispatcher( 4, (int)(ULONG_PTR)__wine_unix_call_dispatcher );
    gex_fault_hook( (int)(ULONG_PTR)__wine_gex_fault );
    install_signal_handlers();
}

/***********************************************************************
 *           signal_start_thread
 *
 * As i386's: the frame set up for LdrInitializeThunk with the initial
 * context (init_syscall_frame), then user mode, run through gex. The
 * thread never comes back here: it ends in NtTerminateThread.
 */
void signal_start_thread( PRTL_THREAD_START_ROUTINE entry, void *arg, TEB *teb )
{
    struct thread_data *data = get_thread_data();
    struct teb_data *tdata = (struct teb_data *)&teb->GdiTebBatch;
    struct syscall_frame *frame;
    struct x86_thread_data *thread_data = (struct x86_thread_data *)tdata->cpu_data;
    CONTEXT *cctx, context = { CONTEXT_ALL };
    DWORD *stack;
    int ctx = cur_ctx();

    if (!tdata->syscall_frame)
        tdata->syscall_frame = (struct syscall_frame *)(((ULONG_PTR)&context - 0x10000 - thread_data->frame_size) & ~63);
    frame = tdata->syscall_frame;
    frame->restore_flags = 0;
    frame->syscall_cfa = NULL;
    frame->prev_frame = NULL;

    /* the program's view of the TEB: %fs's base */
    GEX_C64( GEX_CTX_TPIDR ) = (ULONG_PTR)teb;
    thread_data->gs = 0x2b;

    context.SegCs  = 0x23;
    context.SegDs  = context.SegEs = context.SegSs = 0x2b;
    context.SegFs  = thread_data->fs;
    context.SegGs  = thread_data->gs;
    context.EFlags = 0x202;
    context.Eax    = (DWORD)entry;
    context.Ebx    = (DWORD)arg;
    context.Esp    = (DWORD)teb->Tib.StackBase - 16;
    context.Eip    = (DWORD)pRtlUserThreadStart;
    context.FloatSave.ControlWord = 0x27f;
    ((XSAVE_FORMAT *)context.ExtendedRegisters)->ControlWord = 0x27f;
    ((XSAVE_FORMAT *)context.ExtendedRegisters)->MxCsr = 0x1f80;
    if ((cctx = get_cpu_area( data, IMAGE_FILE_MACHINE_I386 ))) *cctx = context;

    if (data->suspend)
    {
        context.ContextFlags |= CONTEXT_EXCEPTION_REPORTING | CONTEXT_EXCEPTION_ACTIVE;
        wait_suspend( &context );
    }

    cctx = (CONTEXT *)((ULONG_PTR)context.Esp & ~3) - 1;
    *cctx = context;
    cctx->ContextFlags = CONTEXT_FULL | CONTEXT_FLOATING_POINT | CONTEXT_EXTENDED_REGISTERS;
    memset( &frame->xstate, 0, sizeof(frame->xstate) );
    signal_set_full_context( cctx );

    stack = (DWORD *)cctx;
    *(--stack) = 0;
    *(--stack) = 0;
    *(--stack) = 0;
    *(--stack) = (DWORD)cctx;
    *(--stack) = 0xdeadbabe;
    frame->esp = (DWORD)stack;
    frame->eip = (DWORD)pLdrInitializeThunk;
    frame->restore_flags |= CONTEXT_FULL | CONTEXT_EXTENDED_REGISTERS;
    /* (before the context is written: a system call of ours uses its
     * registers) */
    pthread_sigmask( SIG_UNBLOCK, &server_block_set, NULL );
    frame_to_ctx( ctx, frame );

    /* user mode, for good: each call-out runs the program until something
     * leaves it (a resume elsewhere), and the next goes on from there */
    for (;;)
    {
        uint32_t pc = (uint32_t)GEX_C64( GEX_CTX_PC );
        gex_call_out( ctx, (int64_t)pc, 0, 0 );
    }
}

#endif  /* __wasm32__ */
