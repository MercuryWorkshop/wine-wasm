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
 * and a Unix call. */
#define GEXW_SYSCALL_ENTRY   0x7ffe1010ull
#define GEXW_UNIX_CALL_ENTRY 0x7ffe1020ull

/* Per-thread state every native module shares, in TEB->SystemReserved1
 * (which 64-bit Wine leaves unused: it is 16-bit and WoW64 state):
 *
 * - [29], the innermost guest call from native code (a struct
 *   gexw_callout), each linking to the next outer one;
 * - [25..27], a longjmp to a native frame that guest frames and the
 *   call-outs holding them stand between (__gexw_far_longjmp): the
 *   call-out it waits for, the jmp_buf, the value. */
#define GEXW_TEB_CALLOUTS 0x278
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
};

#endif  /* __WINE_WINE_GEX_GEXRT64_H */
