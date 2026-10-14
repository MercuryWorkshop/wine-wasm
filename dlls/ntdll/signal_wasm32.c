/*
 * ntdll on wasm32: native Wine for i386 programs under gex
 *
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
 * The program's code is i386, run by gex; ntdll and the rest of Wine are
 * wasm, sharing its memory, its structures (wasm32 lays them out as MSVC
 * does i386's) and its thread: the TEB is the guest's (FS base), the
 * CONTEXT is i386's, and native code runs on the guest thread's own stack,
 * below the guest's stack pointer (gexrt's __gexw_entry). What differs
 * from signal_i386.c is control flow, as wasm has neither registers to
 * capture nor frames to jump into:
 *
 * - A captured context is the guest's state at the innermost call from the
 *   guest into native code.
 * - Loading a guest state (NtContinue, RtlRestoreContext) drops the native
 *   frames up to that call (gexrt's __gexw_unwind), which then returns 0,
 *   so gex goes on from the context.
 * - An exception raised by native code is dispatched in place; continuing
 *   it returns from RtlRaiseException, unless a handler moved the context.
 * - RtlUnwind calls the unwind handlers and returns; a native __TRY's
 *   target is reached by longjmp (wasm exception handling).
 * - System calls go to gex-kernel's NT layer through gex.syscall
 *   (syscall_wasm32.c, gexrt).
 *
 * See docs/wine-native.md in the gex tree (§3.4, §3.15).
 */

#ifdef __wasm32__

#include <stdarg.h>
#include <stdlib.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winternl.h"
#include "ddk/wdm.h"
#include "ntdll_misc.h"
#include "wine/exception.h"
#include "wine/debug.h"
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

/* ---- gexrt (libs/gexrt) ---- */

extern int gexw_get_ctx(void);
extern int gexw_get_depth(void);
extern unsigned int gexw_get_retaddr(void);
extern void DECLSPEC_NORETURN __gexw_throw_unwind(void);
extern int __gexw_entry( int ctx, int depth, unsigned int need, unsigned long long ret,
                         int (*body)(int, int, unsigned int, unsigned long long) );
extern unsigned long long __gexw_guest_call( UINT target, const UINT *words, UINT n );
extern unsigned long long __gexw_guest_call_regs( UINT target, UINT ecx, UINT edx, const UINT *words, UINT n );
extern unsigned int __gexw_syscall( unsigned int id, const unsigned int *words, unsigned int n );
extern unsigned int __gexw_syscall_resume( unsigned int id, const unsigned int *words, unsigned int n );
extern int __gexw_ret32( int ctx, unsigned long long ret, unsigned int pop, unsigned int eax );

/* The guest context (gex_abi.h): i386 registers in 64-bit slots, eax, ecx,
 * edx, ebx, esp, ebp, esi, edi; the PC at 256; the FS base at 280. */
#define CTX_REG(ctx, n) (*(ULONG *)(ULONG_PTR)((ctx) + 8 * (n)))
#define CTX_PC(ctx)     (*(ULONGLONG *)(ULONG_PTR)((ctx) + 256))
#define R_EAX 0
#define R_ECX 1
#define R_EDX 2
#define R_EBX 3
#define R_ESP 4
#define R_EBP 5
#define R_ESI 6
#define R_EDI 7

static inline int guest_ctx(void)
{
    extern int gex_provider_ctx(void) __attribute__((import_module("gex"), import_name("provider_ctx")));
    int ctx = gexw_get_ctx();
    return ctx ? ctx : gex_provider_ctx();
}

/* Below it, a function pointer is a table index: native code (the ICALL
 * check's test). gex-kernel's NT trampolines are at it. */
#define GEX_NATIVE_FLOOR 0x3f0000

static inline ULONG guest_word( ULONG addr ) { return *(ULONG *)(ULONG_PTR)addr; }


/*******************************************************************
 *         system calls with more to them than a stub
 */
NTSTATUS WINAPI NtContinue( CONTEXT *context, BOOLEAN alertable )
{
    UINT w[2] = { PtrToUlong( context ), alertable };
    return __gexw_syscall_resume( __id_NtContinue, w, 2 );
}

NTSTATUS WINAPI NtContinueEx( CONTEXT *context, KCONTINUE_ARGUMENT *args )
{
    UINT w[2] = { PtrToUlong( context ), PtrToUlong( args ) };
    return __gexw_syscall_resume( __id_NtContinueEx, w, 2 );
}

NTSTATUS WINAPI NtSetContextThread( HANDLE handle, const CONTEXT *context )
{
    UINT w[2] = { HandleToULong( handle ), PtrToUlong( context ) };
    return __gexw_syscall_resume( __id_NtSetContextThread, w, 2 );
}

/* NtQueryPerformanceCounter without a system call (docs/wine-overhead.md
 * F2 in the gex tree). The Unix side's answer is monotonic_counter():
 * clock_gettime( CLOCK_BOOTTIME ) in 100 ns units, frequency TICKSPERSEC.
 * gex.clock_ns reads that clock as the system call would (the same kernel
 * clock, no lock, no door), so the two agree; games ask hundreds of times a
 * frame (Noita: 795 a world frame on its main thread). */
extern long long gex_clock_ns( unsigned int clk ) __attribute__((import_module("gex"), import_name("clock_ns")));

NTSTATUS WINAPI NtQueryPerformanceCounter( LARGE_INTEGER *counter, LARGE_INTEGER *frequency )
{
    /* One call in QPC_SAFEPOINT still makes the system call: a thread that
     * spins on the counter alone (a loading thread waiting for another)
     * would otherwise never reach gex (no unit installs, no signals, no
     * stop) and could hold up the process for minutes. The counter is
     * shared and racy on purpose: a spinning thread still lands on a
     * multiple of QPC_SAFEPOINT about once in QPC_SAFEPOINT of its calls. */
    enum { QPC_SAFEPOINT = 64 };
    static unsigned int calls;

    if (!(++calls % QPC_SAFEPOINT))
    {
        UINT w[2] = { PtrToUlong( counter ), PtrToUlong( frequency ) };
        return __gexw_syscall( __id_NtQueryPerformanceCounter, w, 2 );
    }
    counter->QuadPart = gex_clock_ns( 7 /* CLOCK_BOOTTIME */ ) / 100;
    if (frequency) frequency->QuadPart = 10000000;  /* TICKSPERSEC */
    return STATUS_SUCCESS;
}

NTSTATUS WINAPI NtWow64AllocateVirtualMemory64( HANDLE process, ULONG64 *ret, ULONG64 zero_bits,
                                                ULONG64 *size_ptr, ULONG type, ULONG protect )
{
    UINT w[7] = { HandleToULong( process ), PtrToUlong( ret ), (UINT)zero_bits, (UINT)(zero_bits >> 32),
                  PtrToUlong( size_ptr ), type, protect };
    return __gexw_syscall( __id_NtWow64AllocateVirtualMemory64, w, 7 );
}

NTSTATUS WINAPI NtWow64ReadVirtualMemory64( HANDLE process, ULONG64 addr, void *buffer,
                                            ULONG64 size, ULONG64 *bytes_read )
{
    UINT w[7] = { HandleToULong( process ), (UINT)addr, (UINT)(addr >> 32), PtrToUlong( buffer ),
                  (UINT)size, (UINT)(size >> 32), PtrToUlong( bytes_read ) };
    return __gexw_syscall( __id_NtWow64ReadVirtualMemory64, w, 7 );
}

NTSTATUS WINAPI NtWow64WriteVirtualMemory64( HANDLE process, ULONG64 addr, const void *buffer,
                                             ULONG64 size, ULONG64 *bytes_written )
{
    UINT w[7] = { HandleToULong( process ), (UINT)addr, (UINT)(addr >> 32), PtrToUlong( buffer ),
                  (UINT)size, (UINT)(size >> 32), PtrToUlong( bytes_written ) };
    return __gexw_syscall( __id_NtWow64WriteVirtualMemory64, w, 7 );
}


/*******************************************************************
 *         EXC_CallHandler
 *
 * Call an exception handler (guest code, or native: the call goes through
 * the ICALL check) with a nested frame registered, as i386's does.
 */
typedef struct
{
    EXCEPTION_REGISTRATION_RECORD frame;
    EXCEPTION_REGISTRATION_RECORD *prevFrame;
} EXC_NESTED_FRAME;

static DWORD EXC_CallHandler( EXCEPTION_RECORD *record, EXCEPTION_REGISTRATION_RECORD *frame,
                              CONTEXT *context, EXCEPTION_REGISTRATION_RECORD **dispatcher,
                              PEXCEPTION_HANDLER handler, PEXCEPTION_HANDLER nested_handler )
{
    EXC_NESTED_FRAME nested;
    DWORD ret;

    nested.frame.Handler = nested_handler;
    nested.prevFrame = frame;
    __wine_push_frame( &nested.frame );
    ret = handler( record, frame, context, dispatcher );
    __wine_pop_frame( &nested.frame );
    return ret;
}


/*******************************************************************
 *         raise_handler
 *
 * Handler for exceptions happening inside a handler.
 */
static DWORD WINAPI raise_handler( EXCEPTION_RECORD *rec, EXCEPTION_REGISTRATION_RECORD *frame,
                                   CONTEXT *context, EXCEPTION_REGISTRATION_RECORD **dispatcher )
{
    if (rec->ExceptionFlags & (EXCEPTION_UNWINDING | EXCEPTION_EXIT_UNWIND))
        return ExceptionContinueSearch;
    /* We shouldn't get here so we store faulty frame in dispatcher */
    *dispatcher = ((EXC_NESTED_FRAME*)frame)->prevFrame;
    return ExceptionNestedException;
}


/*******************************************************************
 *         unwind_handler
 *
 * Handler for exceptions happening inside an unwind handler.
 */
static DWORD WINAPI unwind_handler( EXCEPTION_RECORD *rec, EXCEPTION_REGISTRATION_RECORD *frame,
                                    CONTEXT *context, EXCEPTION_REGISTRATION_RECORD **dispatcher )
{
    if (!(rec->ExceptionFlags & (EXCEPTION_UNWINDING | EXCEPTION_EXIT_UNWIND)))
        return ExceptionContinueSearch;
    /* We shouldn't get here so we store faulty frame in dispatcher */
    *dispatcher = ((EXC_NESTED_FRAME*)frame)->prevFrame;
    return ExceptionCollidedUnwind;
}


/**********************************************************************
 *           call_seh_handlers
 *
 * Call the SEH handlers chain.
 */
NTSTATUS call_seh_handlers( EXCEPTION_RECORD *rec, CONTEXT *context )
{
    EXCEPTION_REGISTRATION_RECORD *frame, *dispatch, *nested_frame;
    DWORD res;

    frame = NtCurrentTeb()->Tib.ExceptionList;
    nested_frame = NULL;
    while (frame != (EXCEPTION_REGISTRATION_RECORD*)~0UL)
    {
        /* Check frame address */
        if (!is_valid_frame( (ULONG_PTR)frame ))
        {
            rec->ExceptionFlags |= EXCEPTION_STACK_INVALID;
            break;
        }

        /* Call handler */
        TRACE( "calling handler at %p code=%lx flags=%lx\n",
               frame->Handler, rec->ExceptionCode, rec->ExceptionFlags );
        res = EXC_CallHandler( rec, frame, context, &dispatch, frame->Handler, raise_handler );
        TRACE( "handler at %p returned %lx\n", frame->Handler, res );

        if (frame == nested_frame)
        {
            /* no longer nested */
            nested_frame = NULL;
            rec->ExceptionFlags &= ~EXCEPTION_NESTED_CALL;
        }

        switch(res)
        {
        case ExceptionContinueExecution:
            if (!(rec->ExceptionFlags & EXCEPTION_NONCONTINUABLE)) return STATUS_SUCCESS;
            return STATUS_NONCONTINUABLE_EXCEPTION;
        case ExceptionContinueSearch:
            break;
        case ExceptionNestedException:
            if (nested_frame < dispatch) nested_frame = dispatch;
            rec->ExceptionFlags |= EXCEPTION_NESTED_CALL;
            break;
        default:
            return STATUS_INVALID_DISPOSITION;
        }
        frame = frame->Prev;
    }
    return STATUS_UNHANDLED_EXCEPTION;
}


/*******************************************************************
 *		KiUserExceptionDispatcher (NTDLL.@)
 *
 * Entered by gex-kernel with no return address: the record's and the
 * context's addresses on the stack.
 */
static int ki_user_exception_dispatcher( int ctx, int depth, unsigned int need, unsigned long long ret )
{
    ULONG esp = CTX_REG( ctx, R_ESP );
    dispatch_exception( ULongToPtr( guest_word( esp )), ULongToPtr( guest_word( esp + 4 )));
    return 0;  /* dispatch_exception does not return */
}

int gexp_KiUserExceptionDispatcher( int ctx, int depth, unsigned int need, unsigned long long ret )
{
    return __gexw_entry( ctx, depth, need, ret, ki_user_exception_dispatcher );
}

/* (its address is what LdrSystemDllInitBlock records) */
void WINAPI KiUserExceptionDispatcher( EXCEPTION_RECORD *rec, CONTEXT *context )
{
    dispatch_exception( rec, context );
}


/*******************************************************************
 *		KiUserApcDispatcher (NTDLL.@)
 *
 * Entered by gex-kernel with no return address: the function, its three
 * arguments, the alertable flag, then the CONTEXT to go back to.
 */
static int ki_user_apc_dispatcher( int ctx, int depth, unsigned int need, unsigned long long ret )
{
    ULONG esp = CTX_REG( ctx, R_ESP );
    PNTAPCFUNC func = ULongToPtr( guest_word( esp ));
    CONTEXT *context = ULongToPtr( esp + 0x14 );

    func( guest_word( esp + 4 ), guest_word( esp + 8 ), guest_word( esp + 12 ));
    NtContinue( context, guest_word( esp + 16 ));
    return 0;
}

int gexp_KiUserApcDispatcher( int ctx, int depth, unsigned int need, unsigned long long ret )
{
    return __gexw_entry( ctx, depth, need, ret, ki_user_apc_dispatcher );
}

/* (its address is what LdrSystemDllInitBlock records) */
void WINAPI KiUserApcDispatcher( CONTEXT *context, ULONG_PTR arg1, ULONG_PTR arg2, ULONG_PTR arg3,
                                 PNTAPCFUNC func )
{
    func( arg1, arg2, arg3 );
    NtContinue( context, TRUE );
}


/*******************************************************************
 *		KiUserCallbackDispatcher (NTDLL.@)
 */
void WINAPI KiUserCallbackDispatcher( ULONG id, void *args, ULONG len )
{
    NTSTATUS status = dispatch_user_callback( args, len, id );
    status = NtCallbackReturn( NULL, 0, status );
    for (;;) RtlRaiseStatus( status );
}


/***********************************************************************
 *		capture_guest_context
 *
 * The guest's state at the innermost call from the guest into native
 * code, as returned to: `pop` bytes of arguments popped.
 */
static void capture_guest_context( CONTEXT *context, ULONG pop )
{
    int ctx = guest_ctx();
    ULONG esp = CTX_REG( ctx, R_ESP );

    memset( context, 0, sizeof(*context) );
    context->ContextFlags = CONTEXT_FULL;
    context->Eax = CTX_REG( ctx, R_EAX );
    context->Ecx = CTX_REG( ctx, R_ECX );
    context->Edx = CTX_REG( ctx, R_EDX );
    context->Ebx = CTX_REG( ctx, R_EBX );
    context->Ebp = CTX_REG( ctx, R_EBP );
    context->Esi = CTX_REG( ctx, R_ESI );
    context->Edi = CTX_REG( ctx, R_EDI );
    context->Eip = guest_word( esp );
    context->Esp = esp + 4 + pop;
    context->EFlags = 0x202;
    context->SegCs = 0x23;
    context->SegDs = context->SegEs = context->SegSs = context->SegGs = 0x2b;
    context->SegFs = 0x63;
}


/***********************************************************************
 *		RtlCaptureContext (NTDLL.@)
 */
void WINAPI RtlCaptureContext( CONTEXT *context )
{
    capture_guest_context( context, 0 );
}


/*******************************************************************
 *              RtlRestoreContext (NTDLL.@)
 */
void CDECL RtlRestoreContext( CONTEXT *context, EXCEPTION_RECORD *rec )
{
    TRACE( "returning to %p stack %p\n", (void *)context->Eip, (void *)context->Esp );
    NtContinue( context, FALSE );
}


/*******************************************************************
 *		RtlUnwind (NTDLL.@)
 *
 * Calls the unwind handlers of the frames above `end_frame` and returns
 * (i386's goes back to its caller through NtContinue, the same place).
 */
void WINAPI RtlUnwind( void *end_frame, void *target_ip, EXCEPTION_RECORD *rec, void *retval )
{
    EXCEPTION_REGISTRATION_RECORD *pEndFrame = end_frame;
    EXCEPTION_RECORD record;
    EXCEPTION_REGISTRATION_RECORD *frame, *dispatch;
    CONTEXT context;
    DWORD res;

    capture_guest_context( &context, 16 );
    context.Eax = PtrToUlong( retval );

    /* build an exception record, if we do not have one */
    if (!rec)
    {
        record.ExceptionCode    = STATUS_UNWIND;
        record.ExceptionFlags   = 0;
        record.ExceptionRecord  = NULL;
        record.ExceptionAddress = (void *)context.Eip;
        record.NumberParameters = 0;
        rec = &record;
    }

    rec->ExceptionFlags |= EXCEPTION_UNWINDING | (pEndFrame ? 0 : EXCEPTION_EXIT_UNWIND);

    TRACE( "code=%lx flags=%lx\n", rec->ExceptionCode, rec->ExceptionFlags );
    TRACE_CONTEXT( &context );

    /* get chain of exception frames */
    frame = NtCurrentTeb()->Tib.ExceptionList;
    while ((frame != (EXCEPTION_REGISTRATION_RECORD*)~0UL) && (frame != pEndFrame))
    {
        /* Check frame address */
        if (pEndFrame && (frame > pEndFrame))
            raise_status( STATUS_INVALID_UNWIND_TARGET, rec );

        if (!is_valid_frame( (ULONG_PTR)frame )) raise_status( STATUS_BAD_STACK, rec );

        /* Call handler */
        TRACE( "calling handler at %p code=%lx flags=%lx\n",
               frame->Handler, rec->ExceptionCode, rec->ExceptionFlags );
        res = EXC_CallHandler( rec, frame, &context, &dispatch, frame->Handler, unwind_handler );
        TRACE( "handler at %p returned %lx\n", frame->Handler, res );

        switch(res)
        {
        case ExceptionContinueSearch:
            break;
        case ExceptionCollidedUnwind:
            frame = dispatch;
            break;
        default:
            raise_status( STATUS_INVALID_DISPOSITION, rec );
            break;
        }
        frame = __wine_pop_frame( frame );
    }
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
    DWORD eip, esp, ebp;

    capture_guest_context( &context, 4 );
    rec->ExceptionAddress = (void *)context.Eip;
    eip = context.Eip;
    esp = context.Esp;
    ebp = context.Ebp;

    TRACE( "code=%lx flags=%lx addr=%p\n", rec->ExceptionCode, rec->ExceptionFlags, rec->ExceptionAddress );

    status = dispatch_exception_in_place( rec, &context );

    if (status == STATUS_SUCCESS)
    {
        /* a handler that resumes elsewhere (its own frame) */
        if (context.Eip != eip || context.Esp != esp || context.Ebp != ebp) NtContinue( &context, FALSE );
        return;
    }
    if (status != STATUS_UNHANDLED_EXCEPTION) RtlRaiseStatus( status );
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
 * The guest's EBP chain from the innermost call into native code.
 */
ULONG WINAPI RtlWalkFrameChain( void **buffer, ULONG count, ULONG flags )
{
    CONTEXT context;
    ULONG *frame;
    ULONG i, skip = flags >> 8, pos = 0;

    capture_guest_context( &context, 0 );
    if (count && !skip) buffer[pos++] = (void *)context.Eip;
    for (i = 1; i < count; i++)
    {
        if (!is_valid_frame( context.Ebp )) break;
        frame = (ULONG *)context.Ebp;
        context.Ebp = frame[0];
        context.Eip = frame[1];
        if (i >= skip) buffer[pos++] = (void *)context.Eip;
    }
    return pos;
}


/***********************************************************************
 *           call_thread_func
 */
void call_thread_func( PRTL_THREAD_START_ROUTINE entry, void *arg )
{
    __TRY
    {
        /* kernel32's BaseThreadInitThunk is reached at its guest address,
         * a fastcall: (0, entry) in ECX and EDX; ntdll's own default is
         * native code (a function table index) */
        if (PtrToUlong( pBaseThreadInitThunk ) < GEX_NATIVE_FLOOR)
            pBaseThreadInitThunk( 0, (LPTHREAD_START_ROUTINE)entry, arg );
        else
        {
            UINT w[1] = { PtrToUlong( arg ) };
            __gexw_guest_call_regs( PtrToUlong( pBaseThreadInitThunk ), 0, PtrToUlong( entry ), w, 1 );
        }
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
 * Entered from a CONTEXT (no return address), with the entry point in EAX
 * and its argument in EBX.
 */
static int rtl_user_thread_start( int ctx, int depth, unsigned int need, unsigned long long ret )
{
    call_thread_func( ULongToPtr( CTX_REG( ctx, R_EAX )), ULongToPtr( CTX_REG( ctx, R_EBX )));
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
    if (!wasm32_ntdll_address)
        wasm32_ntdll_address = ULongToPtr( (ULONG)*(ULONGLONG *)(ULONG_PTR)(guest_ctx() + 352) );
    loader_init( context, (void **)&context->Eax );
    TRACE_(relay)( "\1Starting thread proc %p (arg=%p)\n", (void *)context->Eax, (void *)context->Ebx );
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


/**********************************************************************
 *           NtCurrentTeb   (NTDLL.@)
 */
TEB * WINAPI ntdll_NtCurrentTeb(void) __asm__("NtCurrentTeb");
TEB * WINAPI ntdll_NtCurrentTeb(void)
{
    return __wine_wasm_current_teb();
}


/**************************************************************************
 *           _chkstk   (NTDLL.@)
 *           _alloca_probe   (NTDLL.@)
 *
 * Guest code only (EAX bytes off the stack, the return address moved):
 * adapters of their own.
 */
static int chkstk( int ctx, unsigned long long ret )
{
    ULONG esp = CTX_REG( ctx, R_ESP ), ra = guest_word( esp ), sp = esp - CTX_REG( ctx, R_EAX );

    *(ULONG *)ULongToPtr( sp ) = ra;
    CTX_REG( ctx, R_ESP ) = sp + 4;
    CTX_PC( ctx ) = ra;
    return ra == ret;
}

int gexp__chkstk( int ctx, int depth, unsigned int need, unsigned long long ret )
{
    return chkstk( ctx, ret );
}

int gexp__alloca_probe( int ctx, int depth, unsigned int need, unsigned long long ret )
{
    return chkstk( ctx, ret );
}

#endif  /* __wasm32__ */
