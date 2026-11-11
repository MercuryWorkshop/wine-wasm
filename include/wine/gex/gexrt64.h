/*
 * Native Wine on wasm64 under gex (W64): what gexrt64 (libs/gexrt/wasm64.c)
 * shares with ntdll's PE side (signal_wasm64.c). See
 * docs/wine-native-x64.md in the gex tree.
 *
 * No Windows headers: gexrt includes this too.
 */
#ifndef __WINE_WINE_GEX_GEXRT64_H
#define __WINE_WINE_GEX_GEXRT64_H

/* The return address of a guest call from native code: gex-kernel's
 * NATIVE_RETURN_PAD (the last byte of its trampoline page, int3 filler). */
#define GEXW_RETURN_PAD 0x3ffff1ull

/* The two entries on the page after the USD (dlls/ntdll/unix/unix_private.h):
 * a `ud2` each, which the Unix side's fault hook takes for a system call
 * and a Unix call. The USD is where gex places it (0x7ffe0000, or low in a
 * compact space): __gexw_entry_page is the page after it. */
extern unsigned long long __gexw_entry_page(void);
#define GEXW_SYSCALL_ENTRY   (__gexw_entry_page() + 0x10)
#define GEXW_UNIX_CALL_ENTRY (__gexw_entry_page() + 0x20)

/* Per-thread state every native module shares, in TEB->SystemReserved1
 * (which 64-bit Wine leaves unused: it is 16-bit and WoW64 state):
 *
 * - [29], the innermost guest call from native code (a struct
 *   gexw_callout), each linking to the next outer one;
 * - [25..27], a longjmp to a native frame that guest frames and the
 *   call-outs holding them stand between (__gexw_far_longjmp): the
 *   call-out it waits for, the jmp_buf, the value;
 * - [28], under GEXW_DEBUG_ENTRY with ntdll's gexctx channel on: the
 *   CONTEXT a consolidate callback runs with (RtlRestoreContext), which a
 *   guest call made inside it should record as its entry's. */
#define GEXW_TEB_CALLOUTS 0x278
#define GEXW_TEB_DEBUG_CONTEXT 0x270
#define GEXW_TEB_JUMP_TO  0x268
#define GEXW_TEB_JUMP_BUF 0x260
#define GEXW_TEB_JUMP_VAL 0x258

/* A guest call from native code, live while it runs (on the native stack).
 * The x64 unwinder (signal_wasm64.c) reaches the return pad with RSP at
 * pad_rsp; the caller there is the guest state native code was entered
 * with: the return address at entry_rsp and RSP above it, or, for an
 * entry made with a CONTEXT (KiUserExceptionDispatcher and the other
 * dispatchers, entered with no return address), that CONTEXT. */
struct gexw_callout
{
    unsigned long long prev;
    unsigned long long pad_rsp;
    unsigned long long entry_rsp;
    unsigned long long context;
#ifdef GEXW_DEBUG_ENTRY
    unsigned long long writer;   /* gexw_get_entry_writer() at the call */
    unsigned long long retaddr;  /* the innermost entry's return address */
#endif
};

/* GEXW_DEBUG_ENTRY (a debugging build switch, off by default): the last
 * write of the entry context, the kind in the top four bits and a guest
 * address below: */
#ifdef GEXW_DEBUG_ENTRY
#define GEXW_WRITER_ENTER   1ull  /* an entry cleared it; its return address */
#define GEXW_WRITER_RETURN  2ull  /* an entry returned; its return address */
#define GEXW_WRITER_UNWOUND 3ull  /* an entry ended by __gexw_unwind; the same */
#define GEXW_WRITER_SET     4ull  /* gexw_set_entry_context; the value */
extern unsigned long long gexw_get_entry_writer(void);
extern unsigned long long gexw_get_entry_writer_prev(void);
#endif

/* WoW64 (wow64cpu as native code): where gex stops 32-bit code, int3
 * filler on its trampoline page that is never run (gex-kernel's
 * tramp::WOW64_STOP_SYSCALL, WOW64_STOP_UNIX, WOW64_RETURN_MARK): the system
 * call thunk (BTCpuGetBopCode), the unix call one (__wine_get_unix_opcode),
 * and the return address of a system call native 32-bit ntdll makes. */
#define WOW64_STOP_SYSCALL 0x3ffff4u
#define WOW64_STOP_UNIX    0x3ffff7u
#define WOW64_RETURN_MARK  0x3ffffau

/* WoW64: where native 64-bit code called from native 32-bit code (wow64cpu's
 * __wine_gex_syscall32) puts its C stack: below the 64-bit frames of the
 * innermost BTCpuSimulate, in TEB->SystemReserved1[24]. Only the native
 * wow64cpu writes it, before each __gexw_run32: anything else reading it
 * between runs may find a stale value. */
#define GEXW_TEB_WOW64_SP 0x250

/* The thread in 64-bit mode (CS 0x33) for a direct call from native 32-bit
 * code; returns the mode to put back with __gexw_mode64_leave. */
extern unsigned int __gexw_mode64_enter(void);
extern void __gexw_mode64_leave( unsigned int cs );

/* FN(ARG) with the C stack pointer at SP, put back after. */
extern unsigned long long __gexw_call_on_stack( unsigned long long sp, unsigned long long (*fn)( void * ), void *arg );
extern unsigned long long gexw_get_sp(void);

/* FN(ARG) under gex.native_try (wasm64.c), where __gexw_native_try_on
 * (gex's debugging switch GEX_NATIVE_TRY): what FN returned (0 for a void
 * FN), or GEXW_TRY_FAULT when it trapped on a memory access out of bounds.
 * FN is a table index that stays in the table; with GEXW_TRY_WIDE it takes
 * a pointer, whose low half ARG is. An FN that returns STATUS_ACCESS_VIOLATION
 * itself reads as a fault: the status is the same, and restoring the stack
 * pointer after a balanced return changes nothing. */
#define GEXW_TRY_WIDE  0x80000000u
#define GEXW_TRY_FAULT 0xc0000005u  /* STATUS_ACCESS_VIOLATION */
extern int __gexw_native_try_on(void);
extern unsigned int __gexw_native_try( int fn, unsigned int arg );

/* Runs the 32-bit state in AREA (an I386_CONTEXT: wow64cpu's CPU area) until
 * it reaches STOPS[0] or STOPS[1] (WOW64_STOP_SYSCALL and WOW64_STOP_UNIX,
 * the pair gex knows), stores it back and returns the index of the one it
 * reached. The x87 and SSE registers are the thread's one register
 * file, as on x86-64: neither direction copies them. */
extern unsigned int __gexw_run32( void *area, const unsigned int stops[2] );
/* The gex context's 32-bit state into AREA, and AREA's into it. */
extern void __gexw_ctx_to_i386( void *area );
extern void __gexw_i386_to_ctx( const void *area );

#endif  /* __WINE_WINE_GEX_GEXRT64_H */
