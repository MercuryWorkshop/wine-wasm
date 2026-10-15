/*
 * ntdll on wasm64: native Wine for x86-64 programs under gex (W64)
 *
 * Copyright 1999, 2005 Alexandre Julliard (the parts from signal_x86_64.c)
 * Copyright 2026 the gex authors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

/*
 * The program's code is x86-64, run by gex; ntdll and the rest of Wine's
 * PE side are LLP64 wasm64 (lowered to wasm32), sharing its memory, its
 * structures and its thread: the TEB is the guest's (GS base), the
 * CONTEXT is AMD64's, and native code runs on the guest thread's own
 * stack, below the guest's stack pointer (gexrt64's __gexw_entry). It is
 * signal_x86_64.c's PE half with signal_wasm32.c's control flow:
 *
 * - A captured context is the guest's state at the innermost call from
 *   the guest into native code.
 * - Loading a guest state (NtContinue, RtlRestoreContext) drops the native
 *   frames up to that call (gexrt's __gexw_unwind), which then returns 0,
 *   so gex goes on from the context.
 * - An exception raised by native code is dispatched in place; continuing
 *   it returns from RtlRaiseException, unless a handler moved the context.
 * - x64 exceptions unwind by table. Guest frames unwind from their images'
 *   .pdata. Native frames have none: a guest call from native code returns
 *   to gex's return pad, and the unwinder goes from there to the guest
 *   state native code was entered with (gexrt's chain of call-outs,
 *   wine/gex/gexrt64.h). Native __TRY frames are on the TEB list, called
 *   in stack order with the table-unwound ones.
 * - A native __TRY's RtlUnwind calls the unwind handlers and returns; its
 *   target is reached by longjmp, through gexrt when guest frames stand
 *   between (__gexw_far_longjmp).
 * - System calls go to Wine's Unix side directly (syscall_wasm64.c,
 *   gexrt64's __gexw_syscall64), with the program's registers as the
 *   x86-64 stubs leave them.
 *
 * See docs/wine-native-x64.md in the gex tree (§2, W64.2).
 */

#ifdef __wasm64__

#include <stdarg.h>
#include <stdlib.h>
#include <setjmp.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winternl.h"
#include "ddk/wdm.h"
#include "ntdll_misc.h"
#include "wine/exception.h"
#include "wine/debug.h"
#include "wine/gex/gexrt64.h"
#include "ntsyscalls.h"

WINE_DEFAULT_DEBUG_CHANNEL(seh);
WINE_DECLARE_DEBUG_CHANNEL(relay);

enum syscall_ids
{
#define SYSCALL_ENTRY(id,name,args) __id_##name = id,
#undef SYSCALL_ENTRY_NtQueryInformationProcess
#define SYSCALL_ENTRY_NtQueryInformationProcess SYSCALL_ENTRY
ALL_SYSCALLS
#undef SYSCALL_ENTRY
#undef SYSCALL_ENTRY_NtQueryInformationProcess
    __nb_syscalls
};

/* ---- gexrt64 (libs/gexrt/wasm64.c) ---- */

extern int gexw_get_ctx(void);
extern ULONG64 gexw_get_entry_rsp(void);
extern ULONG64 gexw_get_entry_context(void);
extern void gexw_set_entry_context( ULONG64 context );
extern void DECLSPEC_NORETURN __gexw_throw_unwind(void);
extern int __gexw_entry( int ctx, int depth, unsigned int need, unsigned long long ret,
                         int (*body)(int, int, unsigned int, unsigned long long) );
extern int __gexw_ret_void( int ctx, unsigned long long ret );
extern unsigned int __gexw_syscall64( unsigned int id, const ULONG64 *words, unsigned int n );
extern unsigned int __gexw_syscall64_resume( unsigned int id, const ULONG64 *words, unsigned int n );

/* The guest context (gex_abi.h): x86-64's registers in 64-bit slots,
 * numbered as x86 encodes them; the PC at 256; the flags a byte each from
 * 264; MXCSR at 320; the x87 control word at 1028; XMM0-15 from 384. */
#define CTX_Q(ctx, off)  (*(ULONG64 *)(ULONG_PTR)((ctx) + (off)))
#define CTX_B(ctx, off)  (*(BYTE *)(ULONG_PTR)((ctx) + (off)))
#define CTX_REG(ctx, n)  CTX_Q( ctx, 8 * (n) )
#define CTX_PC(ctx)      CTX_Q( ctx, 256 )
#define CTX_LAST_TARGET(ctx) CTX_Q( ctx, 352 )
enum { R_RAX, R_RCX, R_RDX, R_RBX, R_RSP, R_RBP, R_RSI, R_RDI,
       R_R8, R_R9, R_R10, R_R11, R_R12, R_R13, R_R14, R_R15 };

static inline int guest_ctx(void)
{
    extern int gex_provider_ctx(void) __attribute__((import_module("gex"), import_name("provider_ctx")));
    int ctx = gexw_get_ctx();
    return ctx ? ctx : gex_provider_ctx();
}

/* Below it, a function pointer is a table index: native code (the ICALL
 * check's test). */
#define GEX_NATIVE_FLOOR 0x3f0000

static inline struct gexw_callout *first_callout(void)
{
    return *(struct gexw_callout **)((char *)NtCurrentTeb() + GEXW_TEB_CALLOUTS);
}


/*******************************************************************
 *         system calls with more to them than a stub
 */
NTSTATUS WINAPI NtContinue( CONTEXT *context, BOOLEAN alertable )
{
    const ULONG64 w[2] = { (ULONG_PTR)context, alertable };
    return __gexw_syscall64_resume( __id_NtContinue, w, 2 );
}

NTSTATUS WINAPI NtContinueEx( CONTEXT *context, KCONTINUE_ARGUMENT *args )
{
    const ULONG64 w[2] = { (ULONG_PTR)context, (ULONG_PTR)args };
    return __gexw_syscall64_resume( __id_NtContinueEx, w, 2 );
}

NTSTATUS WINAPI NtSetContextThread( HANDLE handle, const CONTEXT *context )
{
    const ULONG64 w[2] = { (ULONG_PTR)handle, (ULONG_PTR)context };
    return __gexw_syscall64_resume( __id_NtSetContextThread, w, 2 );
}

/* NtQueryPerformanceCounter without a system call, as on wasm32
 * (signal_wasm32.c): the Unix side's answer is monotonic_counter(),
 * clock_gettime( CLOCK_BOOTTIME ) in 100 ns units, which gex.clock_ns
 * reads as the system call would. One call in QPC_SAFEPOINT still makes
 * the system call, so a thread spinning on the counter alone reaches the
 * Unix side now and then. */
extern long long gex_clock_ns( unsigned int clk ) __attribute__((import_module("gex"), import_name("clock_ns")));

NTSTATUS WINAPI NtQueryPerformanceCounter( LARGE_INTEGER *counter, LARGE_INTEGER *frequency )
{
    enum { QPC_SAFEPOINT = 64 };
    static unsigned int calls;

    if (!(++calls % QPC_SAFEPOINT))
    {
        const ULONG64 w[2] = { (ULONG_PTR)counter, (ULONG_PTR)frequency };
        return __gexw_syscall64( __id_NtQueryPerformanceCounter, w, 2 );
    }
    counter->QuadPart = gex_clock_ns( 7 /* CLOCK_BOOTTIME */ ) / 100;
    if (frequency) frequency->QuadPart = 10000000;  /* TICKSPERSEC */
    return STATUS_SUCCESS;
}


/***********************************************************************
 *		capture_guest_context
 *
 * The guest's state at the innermost call from the guest into native
 * code, as returned to (Win64's callee pops nothing): or, inside an entry
 * made with a CONTEXT (the KiUser*Dispatcher ones), that CONTEXT.
 */
static void capture_guest_context( CONTEXT *context )
{
    int ctx = guest_ctx();
    ULONG64 rsp = gexw_get_entry_rsp(), at = gexw_get_entry_context();
    ULONG f;
    int i;

    if (at)
    {
        *context = *(const CONTEXT *)(ULONG_PTR)at;
        context->ContextFlags &= ~0x40;  /* no xstate */
        return;
    }
    memset( context, 0, sizeof(*context) );
    context->ContextFlags = CONTEXT_FULL;
    context->Rax = CTX_REG( ctx, R_RAX );
    context->Rcx = CTX_REG( ctx, R_RCX );
    context->Rdx = CTX_REG( ctx, R_RDX );
    context->Rbx = CTX_REG( ctx, R_RBX );
    context->Rbp = CTX_REG( ctx, R_RBP );
    context->Rsi = CTX_REG( ctx, R_RSI );
    context->Rdi = CTX_REG( ctx, R_RDI );
    context->R8  = CTX_REG( ctx, R_R8 );
    context->R9  = CTX_REG( ctx, R_R9 );
    context->R10 = CTX_REG( ctx, R_R10 );
    context->R11 = CTX_REG( ctx, R_R11 );
    context->R12 = CTX_REG( ctx, R_R12 );
    context->R13 = CTX_REG( ctx, R_R13 );
    context->R14 = CTX_REG( ctx, R_R14 );
    context->R15 = CTX_REG( ctx, R_R15 );
    context->Rip = rsp ? *(ULONG64 *)(ULONG_PTR)rsp : CTX_PC( ctx );
    context->Rsp = rsp ? rsp + 8 : CTX_REG( ctx, R_RSP );
    f = 0x202;
    if (CTX_B( ctx, 266 )) f |= 0x001;  /* CF */
    if (CTX_B( ctx, 268 )) f |= 0x004;  /* PF */
    if (CTX_B( ctx, 269 )) f |= 0x010;  /* AF */
    if (CTX_B( ctx, 265 )) f |= 0x040;  /* ZF */
    if (CTX_B( ctx, 264 )) f |= 0x080;  /* SF */
    if (CTX_B( ctx, 270 )) f |= 0x400;  /* DF */
    if (CTX_B( ctx, 267 )) f |= 0x800;  /* OF */
    context->EFlags = f;
    context->SegCs = 0x33;
    context->SegDs = context->SegEs = context->SegGs = context->SegSs = 0x2b;
    context->SegFs = 0x53;
    context->MxCsr = context->FltSave.MxCsr = *(ULONG *)(ULONG_PTR)(ctx + 320);
    context->FltSave.MxCsr_Mask = 0x2ffff;
    context->FltSave.ControlWord = *(USHORT *)(ULONG_PTR)(ctx + 1028);
    for (i = 0; i < 16; i++) memcpy( &context->FltSave.XmmRegisters[i], (char *)(ULONG_PTR)ctx + 384 + 16 * i, 16 );
}


/***********************************************************************
 *		RtlCaptureContext (NTDLL.@)
 */
void WINAPI RtlCaptureContext( CONTEXT *context )
{
    capture_guest_context( context );
}


/***********************************************************************
 *           unwind_native_frames
 *
 * The unwinder at gex's return pad: native frames, which have no table,
 * stand between it and the guest state native code was entered with.
 */
static BOOL unwind_native_frames( CONTEXT *context )
{
    struct gexw_callout *rec, *best = NULL;

    for (rec = first_callout(); rec; rec = (struct gexw_callout *)(ULONG_PTR)rec->prev)
    {
        if (rec->pad_rsp == context->Rsp) { best = rec; break; }
        if (!best && rec->pad_rsp > context->Rsp) best = rec;
    }
    if (!best)
    {
        WARN( "no native caller for the return pad at rsp %p\n", (void *)context->Rsp );
        return FALSE;
    }
    if (best->context)
    {
        DWORD flags = context->ContextFlags;
        *context = *(const CONTEXT *)(ULONG_PTR)best->context;
        context->ContextFlags = flags;
    }
    else
    {
        context->Rip = *(ULONG64 *)(ULONG_PTR)best->entry_rsp;
        context->Rsp = best->entry_rsp + 8;
    }
    return TRUE;
}


/***********************************************************************
 *           virtual_unwind
 */
static NTSTATUS virtual_unwind( ULONG type, DISPATCHER_CONTEXT *dispatch, CONTEXT *context )
{
    dispatch->ImageBase = 0;
    dispatch->ScopeIndex = 0;
    dispatch->ControlPc = context->Rip;

    if (context->Rip == GEXW_RETURN_PAD)
    {
        dispatch->FunctionEntry = NULL;
        dispatch->LanguageHandler = NULL;
        dispatch->HandlerData = NULL;
        dispatch->EstablisherFrame = context->Rsp;
        if (!unwind_native_frames( context ))
        {
            /* nothing to go on to: the end of the stack */
            context->Rip = 0;
            context->Rsp = (ULONG_PTR)NtCurrentTeb()->Tib.StackBase;
        }
        return STATUS_SUCCESS;
    }

    dispatch->FunctionEntry = RtlLookupFunctionEntry( context->Rip, &dispatch->ImageBase,
                                                      dispatch->HistoryTable );
    return RtlVirtualUnwind2( type, dispatch->ImageBase, context->Rip, dispatch->FunctionEntry,
                              context, NULL, &dispatch->HandlerData, &dispatch->EstablisherFrame,
                              NULL, NULL, NULL, &dispatch->LanguageHandler, 0 );
}


struct unwind_exception_frame
{
    EXCEPTION_REGISTRATION_RECORD frame;
    char dummy[0x10];
    DISPATCHER_CONTEXT *dispatch;
};


/*******************************************************************
 *         nested_exception_handler
 */
static EXCEPTION_DISPOSITION WINAPI nested_exception_handler( EXCEPTION_RECORD *rec, EXCEPTION_REGISTRATION_RECORD *frame,
                                                              CONTEXT *context, EXCEPTION_REGISTRATION_RECORD **dispatcher )
{
    struct unwind_exception_frame *unwind_frame = (struct unwind_exception_frame *)frame;
    DISPATCHER_CONTEXT *dispatch = (DISPATCHER_CONTEXT *)dispatcher;

    if (rec->ExceptionFlags & (EXCEPTION_UNWINDING | EXCEPTION_EXIT_UNWIND)) return ExceptionContinueSearch;
    dispatch->EstablisherFrame = unwind_frame->dispatch->EstablisherFrame;
    return ExceptionNestedException;
}


/***********************************************************************
 *		call_seh_handler
 *
 * A handler (guest code, or native: the call goes through the ICALL
 * check) with a nested frame registered.
 */
static DWORD call_seh_handler( EXCEPTION_RECORD *rec, ULONG_PTR frame,
                               CONTEXT *context, void *dispatch, PEXCEPTION_ROUTINE handler )
{
    struct unwind_exception_frame wrapper_frame;
    DWORD res;

    wrapper_frame.frame.Handler = (PEXCEPTION_HANDLER)nested_exception_handler;
    wrapper_frame.dispatch = dispatch;
    __wine_push_frame( &wrapper_frame.frame );
    res = handler( rec, (void *)frame, context, dispatch );
    __wine_pop_frame( &wrapper_frame.frame );
    return res;
}


/**********************************************************************
 *           call_seh_handlers
 *
 * Call the SEH handlers chain: the table-unwound frames' language
 * handlers and the TEB list's (native __TRY) frames, in stack order.
 */
NTSTATUS call_seh_handlers( EXCEPTION_RECORD *rec, CONTEXT *orig_context )
{
    EXCEPTION_REGISTRATION_RECORD *teb_frame = NtCurrentTeb()->Tib.ExceptionList;
    UNWIND_HISTORY_TABLE table;
    DISPATCHER_CONTEXT dispatch;
    CONTEXT context;
    NTSTATUS status;
    ULONG_PTR frame, nested_frame, limit;
    DWORD res;

    context = *orig_context;
    context.ContextFlags &= ~0x40; /* Clear xstate flag. */

    dispatch.TargetIp      = 0;
    dispatch.ContextRecord = &context;
    dispatch.HistoryTable  = &table;
    nested_frame = 0;
    for (;;)
    {
        status = virtual_unwind( UNW_FLAG_EHANDLER, &dispatch, &context );
        if (status != STATUS_SUCCESS) return status;

    unwind_done:
        if (!dispatch.EstablisherFrame) break;

        if (!is_valid_frame( dispatch.EstablisherFrame ))
        {
            ERR( "invalid frame %p (%p-%p)\n", (void *)dispatch.EstablisherFrame,
                 NtCurrentTeb()->Tib.StackLimit, NtCurrentTeb()->Tib.StackBase );
            rec->ExceptionFlags |= EXCEPTION_STACK_INVALID;
            break;
        }

        /* the native frames' handlers below this frame first */
        limit = dispatch.LanguageHandler ? dispatch.EstablisherFrame : context.Rsp;
        while (is_valid_frame( (ULONG_PTR)teb_frame ) && (ULONG_PTR)teb_frame < limit)
        {
            TRACE( "calling TEB handler %p (rec=%p frame=%p context=%p dispatch=%p) sp=%I64x\n",
                   teb_frame->Handler, rec, teb_frame, orig_context, &dispatch, context.Rsp );
            res = call_seh_handler( rec, (ULONG_PTR)teb_frame, orig_context,
                                    &dispatch, (PEXCEPTION_ROUTINE)teb_frame->Handler );
            if (dispatch.EstablisherFrame == nested_frame)
                rec->ExceptionFlags &= EXCEPTION_NONCONTINUABLE;
            TRACE( "TEB handler at %p returned %lu\n", teb_frame->Handler, res );

            switch (res)
            {
            case ExceptionContinueExecution:
                if (rec->ExceptionFlags & EXCEPTION_NONCONTINUABLE) return STATUS_NONCONTINUABLE_EXCEPTION;
                return STATUS_SUCCESS;
            case ExceptionContinueSearch:
                break;
            case ExceptionNestedException:
                rec->ExceptionFlags |= EXCEPTION_NESTED_CALL;
                nested_frame = dispatch.EstablisherFrame;
                TRACE( "nested exception\n" );
                break;
            case ExceptionCollidedUnwind:
                RtlVirtualUnwind( UNW_FLAG_NHANDLER, dispatch.ImageBase, dispatch.ControlPc,
                                  dispatch.FunctionEntry, &context, NULL, &frame, NULL );
                teb_frame = teb_frame->Prev;
                goto unwind_done;
            default:
                return STATUS_INVALID_DISPOSITION;
            }
            teb_frame = teb_frame->Prev;
        }

        if (dispatch.LanguageHandler)
        {
            TRACE( "calling handler %p (rec=%p, frame=%I64x context=%p, dispatch=%p)\n",
                   dispatch.LanguageHandler, rec, dispatch.EstablisherFrame, orig_context, &dispatch );
            res = call_seh_handler( rec, dispatch.EstablisherFrame, orig_context,
                                    &dispatch, dispatch.LanguageHandler );
            if (dispatch.EstablisherFrame == nested_frame)
                rec->ExceptionFlags &= EXCEPTION_NONCONTINUABLE;
            TRACE( "handler at %p returned %lu\n", dispatch.LanguageHandler, res );

            switch (res)
            {
            case ExceptionContinueExecution:
                if (rec->ExceptionFlags & EXCEPTION_NONCONTINUABLE) return STATUS_NONCONTINUABLE_EXCEPTION;
                return STATUS_SUCCESS;
            case ExceptionContinueSearch:
                break;
            case ExceptionNestedException:
                rec->ExceptionFlags |= EXCEPTION_NESTED_CALL;
                nested_frame = dispatch.EstablisherFrame;
                TRACE( "nested exception\n" );
                break;
            case ExceptionCollidedUnwind:
                RtlVirtualUnwind( UNW_FLAG_NHANDLER, dispatch.ImageBase, dispatch.ControlPc,
                                  dispatch.FunctionEntry, &context, NULL, &frame, NULL );
                goto unwind_done;
            default:
                return STATUS_INVALID_DISPOSITION;
            }
        }

        if (!context.Rip || context.Rsp == (ULONG64)NtCurrentTeb()->Tib.StackBase) break;
    }
    return STATUS_UNHANDLED_EXCEPTION;
}


/* Drop the call-outs a guest state at `rsp` has left (their native frames
 * are gone, or going: a trap can skip gexrt's own bookkeeping). */
static void prune_callouts( ULONG64 rsp )
{
    struct gexw_callout **head = (struct gexw_callout **)((char *)NtCurrentTeb() + GEXW_TEB_CALLOUTS);

    while (*head && (*head)->pad_rsp <= rsp) *head = (struct gexw_callout *)(ULONG_PTR)(*head)->prev;
}


/* An entry with a CONTEXT (the KiUser*Dispatchers): the guest state it
 * leaves with may be this very entry again (NtContinueEx delivering the
 * next APC, a fault in a handler), which gex's bridge would take for an
 * entry that ran nothing and hand to tier-1's translation of the
 * skeleton's bytes: run it again here instead. */
static int dispatcher_entry( int ctx, int depth, unsigned int need, unsigned long long ret,
                             int (*body)( int, int, unsigned int, unsigned long long ) )
{
    ULONG64 self = CTX_LAST_TARGET( ctx );
    int r;

    do r = __gexw_entry( ctx, depth, need, ret, body );
    while (!r && CTX_Q( ctx, 256 ) == self);
    return r;
}


/*******************************************************************
 *		KiUserExceptionDispatcher (NTDLL.@)
 *
 * Entered by the Unix side with no return address: the CONTEXT at RSP,
 * the record at RSP + 0x4f0 (exc_stack_layout).
 */
static int ki_user_exception_dispatcher( int ctx, int depth, unsigned int need, unsigned long long ret )
{
    ULONG64 rsp = CTX_REG( ctx, R_RSP );
    CONTEXT *context = (CONTEXT *)(ULONG_PTR)rsp;

    gexw_set_entry_context( rsp );
    prune_callouts( context->Rsp );
    dispatch_exception( (EXCEPTION_RECORD *)(ULONG_PTR)(rsp + 0x4f0), context );
    return 0;  /* dispatch_exception does not return */
}

int gexp_KiUserExceptionDispatcher( int ctx, int depth, unsigned int need, unsigned long long ret )
{
    return dispatcher_entry( ctx, depth, need, ret, ki_user_exception_dispatcher );
}

/* (its address is what LdrSystemDllInitBlock records) */
void WINAPI KiUserExceptionDispatcher( EXCEPTION_RECORD *rec, CONTEXT *context )
{
    dispatch_exception( rec, context );
}


/*******************************************************************
 *		KiUserApcDispatcher (NTDLL.@)
 *
 * Entered by the Unix side with no return address: the CONTEXT at RSP,
 * with the function and its arguments in its home slots, the continue
 * argument at RSP + 0x4f0 (apc_stack_layout).
 */
static int ki_user_apc_dispatcher( int ctx, int depth, unsigned int need, unsigned long long ret )
{
    ULONG64 rsp = CTX_REG( ctx, R_RSP );
    CONTEXT *context = (CONTEXT *)(ULONG_PTR)rsp;
    PNTAPCFUNC func = (PNTAPCFUNC)(ULONG_PTR)context->P4Home;

    gexw_set_entry_context( rsp );
    func( context->P1Home, context->P2Home, context->P3Home );
    NtContinueEx( context, (KCONTINUE_ARGUMENT *)(ULONG_PTR)(rsp + 0x4f0) );
    return 0;
}

int gexp_KiUserApcDispatcher( int ctx, int depth, unsigned int need, unsigned long long ret )
{
    return dispatcher_entry( ctx, depth, need, ret, ki_user_apc_dispatcher );
}

void WINAPI KiUserApcDispatcher( CONTEXT *context, ULONG_PTR arg1, ULONG_PTR arg2, ULONG_PTR arg3,
                                 PNTAPCFUNC func )
{
    func( arg1, arg2, arg3 );
    NtContinue( context, TRUE );
}


/*******************************************************************
 *		KiUserCallbackDispatcher (NTDLL.@)
 *
 * Entered by the Unix side (KeUserModeCallback) with no return address:
 * the arguments' address at RSP + 0x20, their length at 0x28, the id at
 * 0x2c (callback_stack_layout). It never returns: NtCallbackReturn ends
 * the Unix side's call-out.
 */
static int ki_user_callback_dispatcher( int ctx, int depth, unsigned int need, unsigned long long ret )
{
    ULONG64 rsp = CTX_REG( ctx, R_RSP );
    NTSTATUS status;

    status = dispatch_user_callback( *(void **)(ULONG_PTR)(rsp + 0x20), *(ULONG *)(ULONG_PTR)(rsp + 0x28),
                                     *(ULONG *)(ULONG_PTR)(rsp + 0x2c) );
    status = NtCallbackReturn( NULL, 0, status );
    for (;;) RtlRaiseStatus( status );
}

int gexp_KiUserCallbackDispatcher( int ctx, int depth, unsigned int need, unsigned long long ret )
{
    return dispatcher_entry( ctx, depth, need, ret, ki_user_callback_dispatcher );
}

void WINAPI KiUserCallbackDispatcher( ULONG id, void *args, ULONG len )
{
    NTSTATUS status = dispatch_user_callback( args, len, id );
    status = NtCallbackReturn( NULL, 0, status );
    for (;;) RtlRaiseStatus( status );
}


/**************************************************************************
 *              RtlIsEcCode (NTDLL.@)
 */
BOOLEAN WINAPI RtlIsEcCode( ULONG_PTR ptr )
{
    return FALSE;
}


/**********************************************************************
 *           unwind_exception_handler
 *
 * Handler for exceptions happening while calling an unwind handler.
 */
static DWORD WINAPI unwind_exception_handler( EXCEPTION_RECORD *rec, EXCEPTION_REGISTRATION_RECORD *frame,
                                              CONTEXT *context, EXCEPTION_REGISTRATION_RECORD **dispatcher )
{
    struct unwind_exception_frame *unwind_frame = (struct unwind_exception_frame *)frame;
    DISPATCHER_CONTEXT *dispatch = (DISPATCHER_CONTEXT *)dispatcher;

    /* copy the original dispatcher into the current one, except for the TargetIp */
    dispatch->ControlPc        = unwind_frame->dispatch->ControlPc;
    dispatch->ImageBase        = unwind_frame->dispatch->ImageBase;
    dispatch->FunctionEntry    = unwind_frame->dispatch->FunctionEntry;
    dispatch->EstablisherFrame = unwind_frame->dispatch->EstablisherFrame;
    dispatch->LanguageHandler  = unwind_frame->dispatch->LanguageHandler;
    dispatch->HandlerData      = unwind_frame->dispatch->HandlerData;
    dispatch->HistoryTable     = unwind_frame->dispatch->HistoryTable;
    dispatch->ScopeIndex       = unwind_frame->dispatch->ScopeIndex;
    *dispatch->ContextRecord   = *unwind_frame->dispatch->ContextRecord;
    dispatch->ContextRecord->ContextFlags &= ~0x40;
    TRACE( "detected collided unwind\n" );
    return ExceptionCollidedUnwind;
}


/**********************************************************************
 *           call_unwind_handler
 */
static DWORD call_unwind_handler( EXCEPTION_RECORD *rec, ULONG_PTR frame,
                                  CONTEXT *context, void *dispatch, PEXCEPTION_ROUTINE handler )
{
    struct unwind_exception_frame wrapper_frame;
    DWORD res;

    wrapper_frame.frame.Handler = unwind_exception_handler;
    wrapper_frame.dispatch = dispatch;
    __wine_push_frame( &wrapper_frame.frame );
    res = handler( rec, (void *)frame, context, dispatch );
    __wine_pop_frame( &wrapper_frame.frame );
    return res;
}


/*******************************************************************
 *              RtlRestoreContext (NTDLL.@)
 */
void CDECL RtlRestoreContext( CONTEXT *context, EXCEPTION_RECORD *rec )
{
    EXCEPTION_REGISTRATION_RECORD *teb_frame = NtCurrentTeb()->Tib.ExceptionList;

    if (rec && rec->ExceptionCode == STATUS_LONGJUMP && rec->NumberParameters >= 1)
    {
        struct _JUMP_BUFFER *jmp = (struct _JUMP_BUFFER *)rec->ExceptionInformation[0];
        context->Rbx   = jmp->Rbx;
        context->Rsp   = jmp->Rsp;
        context->Rbp   = jmp->Rbp;
        context->Rsi   = jmp->Rsi;
        context->Rdi   = jmp->Rdi;
        context->R12   = jmp->R12;
        context->R13   = jmp->R13;
        context->R14   = jmp->R14;
        context->R15   = jmp->R15;
        context->Rip   = jmp->Rip;
        context->MxCsr = jmp->MxCsr;
        context->FltSave.MxCsr = jmp->MxCsr;
        context->FltSave.ControlWord = jmp->FpCsr;
        memcpy( &context->Xmm6, &jmp->Xmm6, 10 * sizeof(M128A) );
    }
    else if (rec && rec->ExceptionCode == STATUS_UNWIND_CONSOLIDATE && rec->NumberParameters >= 1)
    {
        /* x86-64 calls it from a frame made to look like the context's,
         * for an unwind it starts itself; here it is called directly */
        PVOID (CALLBACK *consolidate)(EXCEPTION_RECORD *) = (void *)rec->ExceptionInformation[0];
        TRACE( "calling consolidate callback %p (rec=%p)\n", consolidate, rec );
        context->Rip = (ULONG64)consolidate( rec );
    }

    /* remove no longer accessible TEB frames */
    while (is_valid_frame( (ULONG_PTR)teb_frame ) && (ULONG64)teb_frame < context->Rsp)
    {
        TRACE( "removing TEB frame: %p\n", teb_frame );
        teb_frame = __wine_pop_frame( teb_frame );
    }

    TRACE( "returning to %p stack %p\n", (void *)context->Rip, (void *)context->Rsp );
    NtContinue( context, FALSE );
}


/*******************************************************************
 *		RtlUnwindEx (NTDLL.@)
 *
 * As x86-64's, with the native frames' TEB handlers called in stack order
 * with the table-unwound frames'. An unwind to a native __TRY (its end
 * frame a TEB frame, its target a native function) returns once that
 * frame is reached, as on wasm32: the caller longjmps (winecrt0).
 */
void WINAPI RtlUnwindEx( PVOID end_frame, PVOID target_ip, EXCEPTION_RECORD *rec,
                         PVOID retval, CONTEXT *context, UNWIND_HISTORY_TABLE *table )
{
    EXCEPTION_REGISTRATION_RECORD *teb_frame = NtCurrentTeb()->Tib.ExceptionList;
    EXCEPTION_RECORD record;
    DISPATCHER_CONTEXT dispatch;
    CONTEXT new_context;
    NTSTATUS status;
    ULONG_PTR frame, limit;
    DWORD i, res;

    RtlCaptureContext( context );
    new_context = *context;

    /* build an exception record, if we do not have one */
    if (!rec)
    {
        record.ExceptionCode    = STATUS_UNWIND;
        record.ExceptionFlags   = 0;
        record.ExceptionRecord  = NULL;
        record.ExceptionAddress = (void *)context->Rip;
        record.NumberParameters = 0;
        rec = &record;
    }

    rec->ExceptionFlags |= EXCEPTION_UNWINDING | (end_frame ? 0 : EXCEPTION_EXIT_UNWIND);

    TRACE( "code=%lx flags=%lx end_frame=%p target_ip=%p\n",
           rec->ExceptionCode, rec->ExceptionFlags, end_frame, target_ip );
    for (i = 0; i < min( EXCEPTION_MAXIMUM_PARAMETERS, rec->NumberParameters ); i++)
        TRACE( " info[%ld]=%016I64x\n", i, rec->ExceptionInformation[i] );
    TRACE_CONTEXT( context );

    dispatch.EstablisherFrame = context->Rsp;
    dispatch.TargetIp         = (ULONG64)target_ip;
    dispatch.ContextRecord    = context;
    dispatch.HistoryTable     = table;

    for (;;)
    {
        status = virtual_unwind( UNW_FLAG_UHANDLER, &dispatch, &new_context );
        if (status != STATUS_SUCCESS) raise_status( status, rec );

    unwind_done:
        if (!dispatch.EstablisherFrame) break;

        if (!is_valid_frame( dispatch.EstablisherFrame ))
        {
            ERR( "invalid frame %p (%p-%p)\n", (void *)dispatch.EstablisherFrame,
                 NtCurrentTeb()->Tib.StackLimit, NtCurrentTeb()->Tib.StackBase );
            rec->ExceptionFlags |= EXCEPTION_STACK_INVALID;
            break;
        }

        /* the native frames' handlers below this frame first */
        limit = dispatch.LanguageHandler ? dispatch.EstablisherFrame : new_context.Rsp;
        while (is_valid_frame( (ULONG_PTR)teb_frame ) && (ULONG_PTR)teb_frame < limit)
        {
            if (teb_frame == end_frame) goto done;
            TRACE( "calling TEB handler %p (rec=%p, frame=%p context=%p, dispatch=%p)\n",
                   teb_frame->Handler, rec, teb_frame, dispatch.ContextRecord, &dispatch );
            res = call_unwind_handler( rec, (ULONG_PTR)teb_frame, dispatch.ContextRecord, &dispatch,
                                       (PEXCEPTION_ROUTINE)teb_frame->Handler );
            TRACE( "handler at %p returned %lu\n", teb_frame->Handler, res );
            teb_frame = __wine_pop_frame( teb_frame );

            switch (res)
            {
            case ExceptionContinueSearch:
                rec->ExceptionFlags &= ~EXCEPTION_COLLIDED_UNWIND;
                break;
            case ExceptionCollidedUnwind:
                context->ContextFlags &= ~0x40; /* clear xstate flag */
                new_context = *context;
                RtlVirtualUnwind( UNW_FLAG_NHANDLER, dispatch.ImageBase,
                                  dispatch.ControlPc, dispatch.FunctionEntry,
                                  &new_context, &dispatch.HandlerData,
                                  &frame, NULL );
                rec->ExceptionFlags |= EXCEPTION_COLLIDED_UNWIND;
                goto unwind_done;
            default:
                raise_status( STATUS_INVALID_DISPOSITION, rec );
                break;
            }
        }

        if (dispatch.LanguageHandler)
        {
            if (end_frame && (dispatch.EstablisherFrame > (ULONG64)end_frame))
            {
                ERR( "invalid end frame %p/%p\n", (void *)dispatch.EstablisherFrame, end_frame );
                raise_status( STATUS_INVALID_UNWIND_TARGET, rec );
            }
            if (dispatch.EstablisherFrame == (ULONG64)end_frame) rec->ExceptionFlags |= EXCEPTION_TARGET_UNWIND;
            TRACE( "calling handler %p (rec=%p, frame=%I64x context=%p, dispatch=%p)\n",
                   dispatch.LanguageHandler, rec, dispatch.EstablisherFrame,
                   dispatch.ContextRecord, &dispatch );
            res = call_unwind_handler( rec, dispatch.EstablisherFrame, dispatch.ContextRecord,
                                       &dispatch, dispatch.LanguageHandler );
            TRACE( "handler %p returned %lx\n", dispatch.LanguageHandler, res );

            switch (res)
            {
            case ExceptionContinueSearch:
                rec->ExceptionFlags &= ~EXCEPTION_COLLIDED_UNWIND;
                break;
            case ExceptionCollidedUnwind:
                context->ContextFlags &= ~0x40; /* clear xstate flag */
                new_context = *context;
                RtlVirtualUnwind( UNW_FLAG_NHANDLER, dispatch.ImageBase,
                        dispatch.ControlPc, dispatch.FunctionEntry,
                        &new_context, NULL, &frame, NULL );
                rec->ExceptionFlags |= EXCEPTION_COLLIDED_UNWIND;
                goto unwind_done;
            default:
                raise_status( STATUS_INVALID_DISPOSITION, rec );
                break;
            }
        }

        if (dispatch.EstablisherFrame == (ULONG64)end_frame) break;
        *context = new_context;
        if (!context->Rip || context->Rsp == (ULONG64)NtCurrentTeb()->Tib.StackBase) break;
    }

done:
    /* a native __TRY's target: its caller jumps there */
    if ((ULONG_PTR)target_ip < GEX_NATIVE_FLOOR) return;

    context->Rax = (ULONG64)retval;
    if (rec->ExceptionCode != STATUS_UNWIND_CONSOLIDATE) context->Rip = (ULONG64)target_ip;
    RtlRestoreContext(context, rec);
}


/***********************************************************************
 *		RtlRaiseException (NTDLL.@)
 *
 * An exception raised by native code, dispatched in place: continuing it
 * returns, unless a handler moved the context elsewhere.
 */
void WINAPI RtlRaiseException( EXCEPTION_RECORD *rec )
{
    CONTEXT context;
    NTSTATUS status;
    ULONG64 rip, rsp, rbp;

    capture_guest_context( &context );
    rec->ExceptionAddress = (void *)context.Rip;
    rip = context.Rip;
    rsp = context.Rsp;
    rbp = context.Rbp;

    TRACE( "code=%lx flags=%lx addr=%p\n", rec->ExceptionCode, rec->ExceptionFlags, rec->ExceptionAddress );

    if (!NtCurrentTeb()->Peb->BeingDebugged)
    {
        status = dispatch_exception_in_place( rec, &context );
        if (status == STATUS_SUCCESS)
        {
            /* a handler that resumes elsewhere (its own frame) */
            if (context.Rip != rip || context.Rsp != rsp || context.Rbp != rbp) NtContinue( &context, FALSE );
            return;
        }
        if (status != STATUS_UNHANDLED_EXCEPTION) RtlRaiseStatus( status );
    }
    RtlRaiseStatus( NtRaiseException( rec, &context, FALSE ));
}


/*************************************************************************
 *		RtlGetNativeSystemInformation (NTDLL.@)
 */
NTSTATUS WINAPI RtlGetNativeSystemInformation( SYSTEM_INFORMATION_CLASS class,
                                               void *info, ULONG size, ULONG *ret_size )
{
    return NtQuerySystemInformation( class, info, size, ret_size );
}


/***********************************************************************
 *           RtlIsProcessorFeaturePresent [NTDLL.@]
 */
BOOLEAN WINAPI RtlIsProcessorFeaturePresent( UINT feature )
{
    return feature < PROCESSOR_FEATURE_MAX && user_shared_data->ProcessorFeatures[feature];
}


/*************************************************************************
 *		RtlWalkFrameChain (NTDLL.@)
 *
 * The guest's frames from the innermost call into native code, past the
 * native frames between.
 */
ULONG WINAPI RtlWalkFrameChain( void **buffer, ULONG count, ULONG flags )
{
    UNWIND_HISTORY_TABLE table;
    RUNTIME_FUNCTION *func;
    PEXCEPTION_ROUTINE handler;
    ULONG_PTR frame, base;
    CONTEXT context;
    void *data;
    ULONG i, skip = flags >> 8, num_entries = 0;

    capture_guest_context( &context );
    if (count && !skip) buffer[num_entries++] = (void *)context.Rip;
    for (i = 1; i < count; i++)
    {
        if (context.Rip == GEXW_RETURN_PAD)
        {
            if (!unwind_native_frames( &context )) break;
        }
        else
        {
            func = RtlLookupFunctionEntry( context.Rip, &base, &table );
            if (!func) break;
            if (RtlVirtualUnwind2( UNW_FLAG_NHANDLER, base, context.Rip, func, &context, NULL,
                                   &data, &frame, NULL, NULL, NULL, &handler, 0 ))
                break;
            if (!frame || !is_valid_frame( frame )) break;
        }
        if (!context.Rip) break;
        if (context.Rsp == (ULONG_PTR)NtCurrentTeb()->Tib.StackBase) break;
        if (context.Rip == GEXW_RETURN_PAD) { i--; continue; }
        if (i >= skip) buffer[num_entries++] = (void *)context.Rip;
    }
    return num_entries;
}


/***********************************************************************
 *           call_thread_func
 */
static void call_thread_func( PRTL_THREAD_START_ROUTINE entry, void *arg )
{
    __TRY
    {
        /* a native entry (RtlCreateUserThread's wrapper: a function-table
         * index) is called here, not by kernel32's BaseThreadInitThunk,
         * which may be guest code */
        if ((ULONG_PTR)entry < 0x3f0000)
            ((LPTHREAD_START_ROUTINE)entry)( arg );
        else
            pBaseThreadInitThunk( 0, (LPTHREAD_START_ROUTINE)entry, arg );
    }
    __EXCEPT(call_unhandled_exception_filter)
    {
        NtTerminateProcess( GetCurrentProcess(), GetExceptionCode() );
    }
    __ENDTRY
}


/***********************************************************************
 *           RtlUserThreadStart (NTDLL.@)
 *
 * Entered from a CONTEXT (no return address), with the entry point in RCX
 * and its argument in RDX.
 */
static int rtl_user_thread_start( int ctx, int depth, unsigned int need, unsigned long long ret )
{
    call_thread_func( (PRTL_THREAD_START_ROUTINE)(ULONG_PTR)CTX_REG( ctx, R_RCX ),
                      (void *)(ULONG_PTR)CTX_REG( ctx, R_RDX ));
    RtlExitUserThread( 0 );
}

int gexp_RtlUserThreadStart( int ctx, int depth, unsigned int need, unsigned long long ret )
{
    return __gexw_entry( ctx, depth, need, ret, rtl_user_thread_start );
}

void WINAPI RtlUserThreadStart( PRTL_THREAD_START_ROUTINE entry, void *arg )
{
    call_thread_func( entry, arg );
    RtlExitUserThread( 0 );
}


/***********************************************************************
 *           signal_start_thread
 */
static void DECLSPEC_NORETURN signal_start_thread( CONTEXT *ctx )
{
    NtContinue( ctx, TRUE );
    for (;;) RtlRaiseStatus( STATUS_INVALID_PARAMETER );
}


/******************************************************************
 *		LdrInitializeThunk (NTDLL.@)
 */
void *wasm32_ntdll_address;

void WINAPI LdrInitializeThunk( CONTEXT *context, ULONG_PTR unk2, ULONG_PTR unk3, ULONG_PTR unk4 )
{
    /* the guest address it was entered at, inside ntdll's image (its own
     * functions' addresses are function table indices) */
    if (!wasm32_ntdll_address) wasm32_ntdll_address = (void *)(ULONG_PTR)CTX_LAST_TARGET( guest_ctx() );
    loader_init( context, (void **)&context->Rcx );
    TRACE_(relay)( "\1Starting thread proc %p (arg=%p)\n", (void *)context->Rcx, (void *)context->Rdx );
    signal_start_thread( context );
}


/***********************************************************************
 *           raise_breakpoint
 */
static void raise_breakpoint(void)
{
    EXCEPTION_RECORD rec;

    rec.ExceptionCode    = EXCEPTION_BREAKPOINT;
    rec.ExceptionFlags   = 0;
    rec.ExceptionRecord  = NULL;
    rec.NumberParameters = 0;
    RtlRaiseException( &rec );
}


/***********************************************************************
 *           process_breakpoint
 */
void WINAPI process_breakpoint(void)
{
    __TRY
    {
        raise_breakpoint();
    }
    __EXCEPT_ALL
    {
        /* do nothing */
    }
    __ENDTRY
}


/***********************************************************************
 *		DbgUiRemoteBreakin   (NTDLL.@)
 */
void WINAPI DbgUiRemoteBreakin( void *arg )
{
    if (NtCurrentTeb()->Peb->BeingDebugged)
    {
        __TRY
        {
            DbgBreakPoint();
        }
        __EXCEPT_ALL
        {
            /* do nothing */
        }
        __ENDTRY
    }
    RtlExitUserThread( STATUS_SUCCESS );
}


/**********************************************************************
 *		DbgBreakPoint   (NTDLL.@)
 */
void WINAPI DbgBreakPoint(void)
{
    raise_breakpoint();
}

/**********************************************************************
 *		DbgUserBreakPoint   (NTDLL.@)
 */
void WINAPI DbgUserBreakPoint(void)
{
    raise_breakpoint();
}


/***********************************************************************
 *           _setjmp (NTDLL.@)
 *           _setjmpex (NTDLL.@)
 *
 * The guest's: its state as returned to, from the innermost entry.
 */
int CDECL NTDLL__setjmpex( _JUMP_BUFFER *buf, ULONG64 frame )
{
    CONTEXT context;

    capture_guest_context( &context );
    buf->Frame = frame;
    buf->Rbx   = context.Rbx;
    buf->Rsp   = context.Rsp;
    buf->Rbp   = context.Rbp;
    buf->Rsi   = context.Rsi;
    buf->Rdi   = context.Rdi;
    buf->R12   = context.R12;
    buf->R13   = context.R13;
    buf->R14   = context.R14;
    buf->R15   = context.R15;
    buf->Rip   = context.Rip;
    buf->MxCsr = context.MxCsr;
    buf->FpCsr = context.FltSave.ControlWord;
    buf->Spare = 0;
    memcpy( &buf->Xmm6, &context.Xmm6, 10 * sizeof(M128A) );
    return 0;
}


/*******************************************************************
 *		longjmp (NTDLL.@)
 */
void CDECL NTDLL_longjmp( _JUMP_BUFFER *buf, int retval )
{
    EXCEPTION_RECORD rec;
    CONTEXT context;

    if (!retval) retval = 1;
    rec.ExceptionCode = STATUS_LONGJUMP;
    rec.ExceptionFlags = 0;
    rec.ExceptionRecord = NULL;
    rec.ExceptionAddress = NULL;
    rec.NumberParameters = 1;
    rec.ExceptionInformation[0] = (DWORD_PTR)buf;
    if (buf->Frame) RtlUnwind( (void *)buf->Frame, (void *)buf->Rip, &rec, IntToPtr(retval) );
    capture_guest_context( &context );
    context.Rax = retval;
    RtlRestoreContext( &context, &rec );
}


/**************************************************************************
 *		__chkstk (NTDLL.@)
 *
 * Guest code only, and x86-64's touches nothing (as signal_x86_64.c's):
 * an adapter of its own that returns, every register as it was.
 */
int gexp___chkstk( int ctx, int depth, unsigned int need, unsigned long long ret )
{
    return __gexw_ret_void( ctx, ret );
}

#endif  /* __wasm64__ */
