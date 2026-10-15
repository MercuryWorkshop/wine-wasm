/*
 * Native Wine modules on wasm64 under gex (W64): the run-time half of the
 * spec glue (winebuild's wasm64 entry adapters) and of the ICALL pass's
 * Win64 exit stubs and entry thunks. The code is LLP64 wasm64 lowered to
 * wasm32 (wcc64), so every address is below 4 GiB. See
 * docs/wine-native-x64.md in the gex tree.
 *
 * The guest is x86-64 with Win64's one calling convention: arguments 1-4
 * in RCX, RDX, R8, R9 (XMM0-3 for floating point, by position), the rest
 * on the stack past the return address and a 32-byte home area; a struct
 * that is not 1, 2, 4 or 8 bytes goes by reference; the result in RAX or
 * XMM0; RSP 16-byte aligned at a call.
 *
 * Copyright 2026 the gex authors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#if defined(__wasm64__) && !defined(GEX_MEMORY64)

#include <stdint.h>
#include "wine/gex/gex_abi.h"
#include "wine/gex/gexrt64.h"

/* x86-64 registers in gex's context, numbered as x86 encodes them */
enum { X_RAX, X_RCX, X_RDX, X_RBX, X_RSP, X_RBP, X_RSI, X_RDI, X_R8, X_R9, X_R10 };
#define REG(r) GEX_C64( GEX_CTX_XN(r) )
/* a guest quadword (below 4 GiB: the lowered flavour) */
#define GQ(a) (*(gex_u64 *)GEX_P( (uint32_t)(a) ))

/* Per-thread state: wasm globals of the module instance (gex instantiates
 * a module once per thread), as on wasm32 (wasm32.c), with the guest
 * addresses 64 bits wide:
 *
 * - gexw_ctx_g: the thread's guest context (0 until the first entry);
 * - gexw_depth_g: the call depth of the innermost entry from the guest;
 * - gexw_retaddr_g: that entry's guest return address;
 * - gexw_entry_rsp_g: the guest's RSP at that entry;
 * - gexw_entry_context_g: the CONTEXT that entry was made with, if it was
 *   (ntdll's KiUser*Dispatcher), else 0;
 * - gexw_icall_target_g: the ICALL pass's guest target. */
__asm__(
    "\t.globaltype __stack_pointer, i64\n"
    "\t.globaltype gexw_icall_target_g, i64\n"
    "gexw_icall_target_g:\n"
    "\t.globaltype gexw_ctx_g, i32\n"
    "gexw_ctx_g:\n"
    "\t.globaltype gexw_depth_g, i32\n"
    "gexw_depth_g:\n"
    "\t.globaltype gexw_retaddr_g, i64\n"
    "gexw_retaddr_g:\n"
    "\t.globaltype gexw_entry_rsp_g, i64\n"
    "gexw_entry_rsp_g:\n"
    "\t.globaltype gexw_entry_context_g, i64\n"
    "gexw_entry_context_g:\n"
    /* thrown by native code that has written a new guest state into the
     * context: every native frame up to the innermost entry from the
     * guest is dropped, and that entry returns 0 */
    "\t.tagtype __gexw_unwind\n"
    "\t.globl __gexw_unwind\n"
    "__gexw_unwind:\n"
    "\t.globl gexw_get_icall_target\n"
    "\t.type gexw_get_icall_target,@function\n"
    "gexw_get_icall_target:\n"
    "\t.functype gexw_get_icall_target () -> (i64)\n"
    "\tglobal.get gexw_icall_target_g\n"
    "\tend_function\n"
    "\t.globl gexw_set_icall_target\n"
    "\t.type gexw_set_icall_target,@function\n"
    "gexw_set_icall_target:\n"
    "\t.functype gexw_set_icall_target (i64) -> ()\n"
    "\tlocal.get 0\n"
    "\tglobal.set gexw_icall_target_g\n"
    "\tend_function\n"
    "\t.globl gexw_get_ctx\n"
    "\t.type gexw_get_ctx,@function\n"
    "gexw_get_ctx:\n"
    "\t.functype gexw_get_ctx () -> (i32)\n"
    "\tglobal.get gexw_ctx_g\n"
    "\tend_function\n"
    "\t.globl gexw_get_depth\n"
    "\t.type gexw_get_depth,@function\n"
    "gexw_get_depth:\n"
    "\t.functype gexw_get_depth () -> (i32)\n"
    "\tglobal.get gexw_depth_g\n"
    "\tend_function\n"
    "\t.globl gexw_set_depth\n"
    "\t.type gexw_set_depth,@function\n"
    "gexw_set_depth:\n"
    "\t.functype gexw_set_depth (i32) -> ()\n"
    "\tlocal.get 0\n"
    "\tglobal.set gexw_depth_g\n"
    "\tend_function\n"
    "\t.globl gexw_get_retaddr\n"
    "\t.type gexw_get_retaddr,@function\n"
    "gexw_get_retaddr:\n"
    "\t.functype gexw_get_retaddr () -> (i64)\n"
    "\tglobal.get gexw_retaddr_g\n"
    "\tend_function\n"
    "\t.globl gexw_get_entry_rsp\n"
    "\t.type gexw_get_entry_rsp,@function\n"
    "gexw_get_entry_rsp:\n"
    "\t.functype gexw_get_entry_rsp () -> (i64)\n"
    "\tglobal.get gexw_entry_rsp_g\n"
    "\tend_function\n"
    "\t.globl gexw_set_entry_rsp\n"
    "\t.type gexw_set_entry_rsp,@function\n"
    "gexw_set_entry_rsp:\n"
    "\t.functype gexw_set_entry_rsp (i64) -> ()\n"
    "\tlocal.get 0\n"
    "\tglobal.set gexw_entry_rsp_g\n"
    "\tend_function\n"
    "\t.globl gexw_get_entry_context\n"
    "\t.type gexw_get_entry_context,@function\n"
    "gexw_get_entry_context:\n"
    "\t.functype gexw_get_entry_context () -> (i64)\n"
    "\tglobal.get gexw_entry_context_g\n"
    "\tend_function\n"
    "\t.globl gexw_set_entry_context\n"
    "\t.type gexw_set_entry_context,@function\n"
    "gexw_set_entry_context:\n"
    "\t.functype gexw_set_entry_context (i64) -> ()\n"
    "\tlocal.get 0\n"
    "\tglobal.set gexw_entry_context_g\n"
    "\tend_function\n"
    "\t.globl gexw_get_sp\n"
    "\t.type gexw_get_sp,@function\n"
    "gexw_get_sp:\n"
    "\t.functype gexw_get_sp () -> (i64)\n"
    "\tglobal.get __stack_pointer\n"
    "\tend_function\n"
    "\t.globl __gexw_throw_unwind\n"
    "\t.type __gexw_throw_unwind,@function\n"
    "__gexw_throw_unwind:\n"
    "\t.functype __gexw_throw_unwind () -> ()\n"
    "\tthrow __gexw_unwind\n"
    "\tend_function\n"

    /* __gexw_entry_raw(ctx, depth, need, ret, body): an entry from the guest
     * into native code, as on wasm32: the native code runs on the guest
     * thread's stack, below the guest's RSP (Win64 has no red zone); the
     * per-thread globals and the C stack pointer are put back on the way
     * out; a __gexw_unwind from inside ends the call with 0. */
    "\t.globl __gexw_entry_raw\n"
    "\t.type __gexw_entry_raw,@function\n"
    "__gexw_entry_raw:\n"
    "\t.functype __gexw_entry_raw (i32, i32, i32, i64, i64) -> (i32)\n"
    /* 5 sp, 6 ctx, 7 depth, 8 retaddr, 9 rsp, 11 entry rsp, 12 entry
     * context (i64 but 6, 7); 10 the result */
    "\t.local i64, i32, i32, i64, i64, i32, i64, i64\n"
    "\tglobal.get gexw_entry_rsp_g\n"
    "\tlocal.set 11\n"
    "\tglobal.get gexw_entry_context_g\n"
    "\tlocal.set 12\n"
    "\ti64.const 0\n"
    "\tglobal.set gexw_entry_context_g\n"
    "\tglobal.get __stack_pointer\n"
    "\tlocal.set 5\n"
    "\tglobal.get gexw_ctx_g\n"
    "\tlocal.set 6\n"
    "\tglobal.get gexw_depth_g\n"
    "\tlocal.set 7\n"
    "\tglobal.get gexw_retaddr_g\n"
    "\tlocal.set 8\n"
    /* rsp, the guest's stack pointer (X4) */
    "\tlocal.get 0\n"
    "\ti64.extend_i32_u\n"
    "\ti64.load 32\n"
    "\tlocal.tee 9\n"
    "\tglobal.set gexw_entry_rsp_g\n"
    "\tlocal.get 9\n"
    "\ti64.load 0\n"
    "\tglobal.set gexw_retaddr_g\n"
    "\tlocal.get 9\n"
    "\ti64.const 64\n"
    "\ti64.sub\n"
    "\ti64.const -16\n"
    "\ti64.and\n"
    "\tglobal.set __stack_pointer\n"
    "\tlocal.get 0\n"
    "\tglobal.set gexw_ctx_g\n"
    "\tlocal.get 1\n"
    "\tglobal.set gexw_depth_g\n"
    "\tblock\n"
    "\ttry_table (catch __gexw_unwind 0)\n"
    "\tlocal.get 0\n"
    "\tlocal.get 1\n"
    "\tlocal.get 2\n"
    "\tlocal.get 3\n"
    "\tlocal.get 4\n"
    "\tcall_indirect __indirect_function_table, (i32, i32, i32, i64) -> (i32)\n"
    "\tlocal.set 10\n"
    "\tlocal.get 5\n"
    "\tglobal.set __stack_pointer\n"
    "\tlocal.get 6\n"
    "\tglobal.set gexw_ctx_g\n"
    "\tlocal.get 7\n"
    "\tglobal.set gexw_depth_g\n"
    "\tlocal.get 8\n"
    "\tglobal.set gexw_retaddr_g\n"
    "\tlocal.get 11\n"
    "\tglobal.set gexw_entry_rsp_g\n"
    "\tlocal.get 12\n"
    "\tglobal.set gexw_entry_context_g\n"
    "\tlocal.get 10\n"
    "\treturn\n"
    "\tend_try_table\n"
    "\tend_block\n"
    "\tlocal.get 5\n"
    "\tglobal.set __stack_pointer\n"
    "\tlocal.get 6\n"
    "\tglobal.set gexw_ctx_g\n"
    "\tlocal.get 7\n"
    "\tglobal.set gexw_depth_g\n"
    "\tlocal.get 8\n"
    "\tglobal.set gexw_retaddr_g\n"
    "\tlocal.get 11\n"
    "\tglobal.set gexw_entry_rsp_g\n"
    "\tlocal.get 12\n"
    "\tglobal.set gexw_entry_context_g\n"
    "\ti32.const 0\n"
    "\tend_function\n"
);

uint64_t gexw_get_icall_target(void);
int32_t gexw_get_ctx(void);
int32_t gexw_get_depth(void);
uint64_t gexw_get_retaddr(void);
uint64_t gexw_get_sp(void);
uint64_t gexw_get_entry_rsp(void);
void gexw_set_entry_rsp(uint64_t);
void __gexw_throw_unwind(void) __attribute__((noreturn));

__attribute__((import_module("gex"), import_name("provider_ctx"))) int gex_provider_ctx(void);
__attribute__((import_module("gex"), import_name("provider_missing"))) void gex_provider_missing(int addr);

/* The thread's guest context, from inside native code. */
static inline int cur_ctx(void)
{
    int ctx = gexw_get_ctx();
    return ctx ? ctx : gex_provider_ctx();
}

/* NtCurrentTeb(): the guest thread's GS base. */
void *__wine_wasm_current_teb(void)
{
    int ctx = cur_ctx();
    return (void *)(uintptr_t)GEX_C64( GEX_CTX_TPIDRRO );
}

/* __builtin_return_address(0) in Wine's code: the guest's return address
 * of the innermost call from the guest into native code. */
void *__wine_wasm_return_address(void)
{
    return (void *)(uintptr_t)gexw_get_retaddr();
}

/* ---- entry adapters (winebuild) and entry thunks (the ICALL pass) ---- */

static const unsigned char arg_reg[4] = { X_RCX, X_RDX, X_R8, X_R9 };

/* Argument slot `k` as an integer: RCX, RDX, R8, R9, then the stack past
 * the return address and the home area. */
uint64_t __gexw_arg( int ctx, uint32_t k )
{
    if (k < 4) return REG( arg_reg[k] );
    return GQ( REG( X_RSP ) + 8 + 8 * (uint64_t)k );
}

/* The same as floating point: XMM0-3, then the stack. */
double __gexw_argf64( int ctx, uint32_t k )
{
    if (k < 4) return gex_f64( GEX_C64( GEX_CTX_VN( k )) );
    return gex_f64( GQ( REG( X_RSP ) + 8 + 8 * (uint64_t)k ));
}

float __gexw_argf32( int ctx, uint32_t k )
{
    if (k < 4) return gex_f32( GEX_C32( GEX_CTX_VN( k )) );
    return gex_f32( (uint32_t)GQ( REG( X_RSP ) + 8 + 8 * (uint64_t)k ));
}

/* The guest address of argument slot `k`: a 1, 2, 4 or 8-byte struct
 * passed by value. A register argument is written to its home slot first
 * (the callee's to use). */
uint64_t __gexw_argptr( int ctx, uint32_t k )
{
    uint64_t at = REG( X_RSP ) + 8 + 8 * (uint64_t)k;
    if (k < 4) GQ( at ) = REG( arg_reg[k] );
    return at;
}

/* Return to the guest: pop the return address, set PC. */
static inline int ret_to_guest( int ctx, uint64_t ret )
{
    uint64_t sp = REG( X_RSP ), ra = GQ( sp );
    REG( X_RSP ) = sp + 8;
    GEX_C64( GEX_CTX_PC ) = ra;
    return ra == ret;
}

/* The same with RAX. */
int __gexw_ret( int ctx, uint64_t ret, uint64_t rax )
{
    REG( X_RAX ) = rax;
    return ret_to_guest( ctx, ret );
}

int __gexw_ret_void( int ctx, uint64_t ret )
{
    return ret_to_guest( ctx, ret );
}

/* The same with XMM0 (its upper lanes zeroed, as movq/movss loads do). */
int __gexw_ret_f64( int ctx, uint64_t ret, double v )
{
    GEX_C64( GEX_CTX_VN( 0 )) = gex_b64( v );
    GEX_C64( GEX_CTX_VN( 0 ) + 8 ) = 0;
    return ret_to_guest( ctx, ret );
}

int __gexw_ret_f32( int ctx, uint64_t ret, float v )
{
    uint32_t b;
    __builtin_memcpy( &b, &v, 4 );
    GEX_C64( GEX_CTX_VN( 0 )) = b;
    GEX_C64( GEX_CTX_VN( 0 ) + 8 ) = 0;
    return ret_to_guest( ctx, ret );
}

void __gexw_missing( int ctx )
{
    gex_provider_missing( (int)GEX_C64( GEX_CTX_LAST_TARGET ));
}

/* ---- running guest code from native code ---- */

uint64_t gexw_get_entry_context(void);
void gexw_set_entry_context(uint64_t);
extern void longjmp( void *, int ) __attribute__((noreturn));

__attribute__((import_module("gex"), import_name("unix_dispatcher"))) int gex_unix_dispatcher( int op, int idx );

/* the TEB (GS base) and its quadword at `off` */
#define TEBQ(off) GQ( GEX_C64( GEX_CTX_TPIDRRO ) + (off) )

/* Ops 0 and 4 of gex.unix_dispatcher under W64: the Unix side's entries for
 * a system call and a Unix call from native PE code, each void(int ctx),
 * the program's state set up as the x86-64 stubs leave it (signal_wasm64.c
 * on the Unix side registers them). 0: none; -1 until asked. Process-wide:
 * the module's data is the process's. */
static int unix_syscall_entry = -1, unix_call_entry = -1;

static inline void callout_push( int ctx, struct gexw_callout *rec, uint64_t pad_rsp )
{
    rec->prev = TEBQ( GEXW_TEB_CALLOUTS );
    rec->pad_rsp = pad_rsp;
    rec->entry_rsp = gexw_get_entry_rsp();
    rec->context = gexw_get_entry_context();
    TEBQ( GEXW_TEB_CALLOUTS ) = (uintptr_t)rec;
}

/* Run the guest from the context's PC until it comes back to the return
 * pad with RSP at `pad_rsp`. When it cannot be made or is abandoned (an
 * outer call-out returned first, or the guest went on above this one's
 * frames: gex leaves a call-out whose stack the guest has unwound past),
 * every native frame up to the innermost entry goes. A __gexw_far_longjmp
 * waiting for this call-out is made here, in the frame whose native caller
 * it jumps to. */
int __gexw_entry_raw( int ctx, int depth, unsigned int need, unsigned long long ret,
                      int (*body)( int, int, unsigned int, unsigned long long ) );

/* gex's NATIVE_LEFT (gex_nr), its `jmp rax` (the trampoline page's
 * JMP_EAX_32: ff e0 is jmp rax in 64-bit code), the mark on a system call
 * from native code, the context's spare slot (ctx::BALLAST), and how far
 * above a native caller's call-out the guest must be to have left it
 * (NATIVE_UNWOUND) */
#define GEX_NR_NATIVE_LEFT 0x7f000003u
#define GEXW_JMP_RAX 0x3f0080ull
#define GEX_NATIVE_SYSCALL 0x2000000000000000ull
#define GEX_CTX_BALLAST 344u
#define GEXW_NATIVE_UNWOUND 512u
/* the TEB's NtTib.StackBase and DeallocationStack: the stack the guest is
 * on (a fiber switch changes it) */
#define TEB_STACK_BASE 0x08
#define TEB_DEALLOCATION_STACK 0x1478

/* Whether the guest, at its stack pointer now, is above the call-out
 * `head` on the same stack: it has left the native frames that made it. */
static int left_callout( int ctx, uint64_t teb, uint64_t head )
{
    uint64_t pad, rsp = REG( X_RSP );

    if (!teb || !head) return 0;
    pad = ((const struct gexw_callout *)(uintptr_t)head)->pad_rsp;
    if (pad < GQ( teb + TEB_DEALLOCATION_STACK ) || pad >= GQ( teb + TEB_STACK_BASE )) return 0;
    return rsp > pad + GEXW_NATIVE_UNWOUND;
}

/* An entry from the guest into native code (the adapters' and the
 * dispatchers'). Its native frames are gone when it ends, by returning or
 * by a __gexw_unwind (NtContinue, an abandoned call-out): so are the
 * call-outs they made, whose records were on them. The chain goes back to
 * what it was at entry, less the call-outs the guest state it leaves with
 * is above (a trap or a jump out of their guest code); a record left on it
 * would be stack that native code reuses, read as a call-out. After a
 * fiber switch it is left as it is: the stack is another one. */
int __gexw_entry( int ctx, int depth, unsigned int need, unsigned long long ret,
                  int (*body)( int, int, unsigned int, unsigned long long ) )
{
    uint64_t teb = GEX_C64( GEX_CTX_TPIDRRO ), head = teb ? GQ( teb + GEXW_TEB_CALLOUTS ) : 0, rsp;
    uint64_t stack = teb ? GQ( teb + TEB_STACK_BASE ) : 0;
    int r;

    /* Guest code entering from above the innermost call-out's frame has
     * left the native frames that made it (a catch block continuing in an
     * outer frame, a longjmp) and is running nested inside them: one more
     * level of host and native frames per throw that way. As on wasm32
     * (NATIVE_LEFT): gex abandons that call-out, its native caller drops its
     * frames (run_callout), and the guest re-enters this function from
     * gex's `jmp rax` (an address of the trampoline page's, which the bridge
     * does not take for this function's "ran nothing"), one level flatter,
     * until it is not nested. gex checks the mark (NATIVE_UNWOUND). */
    if (left_callout( ctx, teb, head ))
    {
        uint64_t rax = REG( X_RAX ), pc = GEX_C64( GEX_CTX_PC );

        GEX_C64( GEX_CTX_PC ) = GEXW_JMP_RAX;
        REG( X_RAX ) = GEX_NR_NATIVE_LEFT;
        GEX_C64( GEX_CTX_BALLAST ) = GEX_C64( GEX_CTX_LAST_TARGET );  /* this function's address, for RAX */
        gex_syscall( ctx, (int64_t)(GEX_NATIVE_SYSCALL | GEXW_JMP_RAX) );
        /* reached only when no call-out was left: as if nothing */
        REG( X_RAX ) = rax;
        GEX_C64( GEX_CTX_PC ) = pc;
    }
    r = __gexw_entry_raw( ctx, depth, need, ret, body );

    /* The same at the end: an entry made inside a call-out that leaves the
     * guest above it (NtContinue to a catch block's continuation in an
     * outer frame, a longjmp) would have the guest go on nested in the
     * native frames that made the call-out, a level more per C++ throw.
     * gex abandons the call-out now, and the guest goes on from the state
     * this entry left (RAX back from the context's spare slot). Not after
     * a fiber switch (the TEB's stack changed): the guest is on another
     * stack, and the call-outs stay the other fiber's. */
    if (teb && stack == GQ( teb + TEB_STACK_BASE ) && left_callout( ctx, teb, head ))
    {
        uint64_t rax = REG( X_RAX );

        GEX_C64( GEX_CTX_BALLAST ) = rax;
        REG( X_RAX ) = GEX_NR_NATIVE_LEFT;
        gex_syscall( ctx, (int64_t)(GEX_NATIVE_SYSCALL | GEX_C64( GEX_CTX_PC )) );
        REG( X_RAX ) = rax;
    }

    teb = GEX_C64( GEX_CTX_TPIDRRO );
    if (teb && stack == GQ( teb + TEB_STACK_BASE ))
    {
        rsp = REG( X_RSP );
        while (head && ((const struct gexw_callout *)(uintptr_t)head)->pad_rsp <= rsp)
            head = ((const struct gexw_callout *)(uintptr_t)head)->prev;
        GQ( teb + GEXW_TEB_CALLOUTS ) = head;
    }
    return r;
}

static void run_callout( int ctx, uint64_t pad_rsp )
{
    struct gexw_callout rec;
    uint64_t prev;
    int ok;

    callout_push( ctx, &rec, pad_rsp );
    prev = rec.prev;
    ok = gex_call_out( ctx, (int64_t)GEX_C64( GEX_CTX_PC ), (int64_t)GEXW_RETURN_PAD, (int64_t)pad_rsp );
    TEBQ( GEXW_TEB_CALLOUTS ) = prev;
    if (!ok) __gexw_throw_unwind();
    if (TEBQ( GEXW_TEB_JUMP_TO ) == (uintptr_t)&rec)
    {
        void *buf = (void *)(uintptr_t)TEBQ( GEXW_TEB_JUMP_BUF );
        int val = (int)TEBQ( GEXW_TEB_JUMP_VAL );
        TEBQ( GEXW_TEB_JUMP_TO ) = 0;
        longjmp( buf, val );
    }
}

/* After a call into the Unix side that returns to the pad with RSP at
 * `pad_rsp`: when the Unix side sent the thread elsewhere first (an APC,
 * KiUserApcDispatcher), the guest runs that under a call-out until it
 * comes back here; at the pad with another stack pointer (NtCallbackReturn
 * ending a user callback: KeUserModeCallback's call-out waits for it), the
 * state is an outer call-out's, and every native frame up to the innermost
 * entry goes. */
static void after_call( int ctx, uint64_t pad_rsp )
{
    if (GEX_C64( GEX_CTX_PC ) != GEXW_RETURN_PAD) run_callout( ctx, pad_rsp );
    else if (REG( X_RSP ) != pad_rsp) __gexw_throw_unwind();
}

/* A Unix call through PE ntdll's __wine_unix_call_dispatcher pointer
 * (GEXW_UNIX_CALL_ENTRY) from native code: to the Unix side's entry
 * directly, the registers as an x86-64 caller leaves them; 0 when there is
 * none. */
static int direct_unix_call( int ctx, const uint64_t *slots, uint64_t *ret )
{
    uint64_t sp0 = REG( X_RSP ), pc0 = GEX_C64( GEX_CTX_PC ), sp;

    if (unix_call_entry < 0) unix_call_entry = gex_unix_dispatcher( 5, 0 );
    if (unix_call_entry <= 0) return 0;
    sp = ((gexw_get_sp() - 64) & ~15ull) - 0x30 - 8;
    GQ( sp ) = GEXW_RETURN_PAD;
    REG( X_RCX ) = slots[0];
    REG( X_RDX ) = slots[1];
    REG( X_R8 ) = slots[2];
    REG( X_RSP ) = sp;
    GEX_C64( GEX_CTX_PC ) = GEXW_UNIX_CALL_ENTRY;
    ((void (*)(int))(uintptr_t)unix_call_entry)( ctx );
    after_call( ctx, sp + 8 );
    *ret = REG( X_RAX );
    REG( X_RSP ) = sp0;
    GEX_C64( GEX_CTX_PC ) = pc0;
    return 1;
}

/* Call the guest function at `target` with `n` argument slots (the ICALL
 * pass's exit stubs): the first four go in RCX, RDX, R8, R9 and XMM0-3
 * alike (a callee reads the ones its types name; a variadic one finds the
 * floating-point ones in both, as Win64 wants), every slot also on the
 * stack (the first four in the home area), below everything native code
 * has live. Back: RAX, or XMM0's low 64 bits when `fp`. RSP is put back as
 * it was.
 *
 * The guest runs under gex.call_out, as the Unix side's callbacks do
 * (signal_wasm64.c): the host runs it until it returns to the return pad
 * with RSP above the call, translating the pad's page first. (wasm32.c
 * runs the function table itself, run_until; under W64 the pad's page has
 * no dispatch entry until a call-out translates it.) */
uint64_t __gexw_guest_call64( uint64_t target, const uint64_t *slots, uint32_t n, uint32_t fp )
{
    int ctx = cur_ctx();
    uint64_t sp0 = REG( X_RSP ), pc0 = GEX_C64( GEX_CTX_PC ), sp, r;
    uint32_t i, room = n < 4 ? 4 : n;

    if (target == GEXW_UNIX_CALL_ENTRY && n == 3 && direct_unix_call( ctx, slots, &r )) return r;

    /* 16-aligned at the call: the return address goes just below */
    sp = ((gexw_get_sp() - 64 - sizeof(struct gexw_callout)) & ~15ull) - 8 * (uint64_t)((room + 1) & ~1u);
    for (i = 0; i < room; i++) GQ( sp + 8 * i ) = i < n ? slots[i] : 0;
    for (i = 0; i < 4 && i < n; i++)
    {
        REG( arg_reg[i] ) = slots[i];
        GEX_C64( GEX_CTX_VN( i )) = slots[i];
        GEX_C64( GEX_CTX_VN( i ) + 8 ) = 0;
    }
    sp -= 8;
    GQ( sp ) = GEXW_RETURN_PAD;
    REG( X_RSP ) = sp;
    GEX_C64( GEX_CTX_PC ) = target;
    GEX_C64( GEX_CTX_LAST_TARGET ) = target;
    run_callout( ctx, sp + 8 );
    REG( X_RSP ) = sp0;
    GEX_C64( GEX_CTX_PC ) = pc0;
    return fp ? GEX_C64( GEX_CTX_VN( 0 )) : REG( X_RAX );
}

/* longjmp to `buf`, set up in the native frame that registered the TEB
 * exception frame `frame` (winecrt0's __TRY), from native code that guest
 * frames may stand between: those are under a call-out of a native
 * caller's. When they do, the innermost entry is left with the guest at
 * the return pad of the call-out just below `frame` (the call-outs between
 * are abandoned, their native frames dropped), and that call-out makes the
 * jump (run_callout). A wasm longjmp cannot cross gex.call_out's host
 * frame itself. */
void __gexw_far_longjmp( void *buf, int val, uint64_t frame )
{
    int ctx = cur_ctx();
    uint64_t r = TEBQ( GEXW_TEB_CALLOUTS ), to = 0;

    while (r && ((const struct gexw_callout *)(uintptr_t)r)->pad_rsp < frame)
    {
        to = r;
        r = ((const struct gexw_callout *)(uintptr_t)r)->prev;
    }
    if (!to) longjmp( buf, val );
    TEBQ( GEXW_TEB_JUMP_TO ) = to;
    TEBQ( GEXW_TEB_JUMP_BUF ) = (uintptr_t)buf;
    TEBQ( GEXW_TEB_JUMP_VAL ) = (uint32_t)val;
    REG( X_RSP ) = ((const struct gexw_callout *)(uintptr_t)to)->pad_rsp;
    GEX_C64( GEX_CTX_PC ) = GEXW_RETURN_PAD;
    __gexw_throw_unwind();
}

/* ---- NT system calls from native code ---- */

/* The program's state set up as Wine's x86-64 stubs leave it at the call
 * through [0x7ffe1000] (signal_wasm64.c's syscall_body on the Unix side
 * reads it): the number in RAX, the words in R10 (and RCX), RDX, R8, R9
 * and from RSP + 0x30, the return pad at [RSP] and [RSP + 8], below the
 * native stack. The Unix side's entry is called directly (gex's op 1 under
 * W64); without one, the `ud2` there runs under a call-out and its fault
 * hook takes it. */
static uint64_t syscall_enter( int ctx, uint32_t id, const uint64_t *w, uint32_t n )
{
    uint32_t i, extra = n > 4 ? n - 4 : 0;
    uint64_t sp = ((gexw_get_sp() - 64 - sizeof(struct gexw_callout)) & ~15ull) -
                  ((0x30 + 8 * (uint64_t)extra + 15) & ~15ull) - 8;

    GQ( sp ) = GEXW_RETURN_PAD;
    GQ( sp + 8 ) = GEXW_RETURN_PAD;
    for (i = 4; i < n; i++) GQ( sp + 0x30 + 8 * (uint64_t)(i - 4) ) = w[i];
    REG( X_RAX ) = id;
    REG( X_R10 ) = REG( X_RCX ) = n > 0 ? w[0] : 0;
    REG( X_RDX ) = n > 1 ? w[1] : 0;
    REG( X_R8 ) = n > 2 ? w[2] : 0;
    REG( X_R9 ) = n > 3 ? w[3] : 0;
    REG( X_RSP ) = sp;
    GEX_C64( GEX_CTX_PC ) = GEXW_SYSCALL_ENTRY;
    if (unix_syscall_entry < 0) unix_syscall_entry = gex_unix_dispatcher( 1, 0 );
    if (unix_syscall_entry > 0) ((void (*)(int))(uintptr_t)unix_syscall_entry)( ctx );
    else if (!gex_call_out( ctx, (int64_t)GEXW_SYSCALL_ENTRY, (int64_t)GEXW_RETURN_PAD, (int64_t)(sp + 8) ))
        __gexw_throw_unwind();
    return sp;
}

/* A system call that returns to its caller. If the Unix side sent the
 * thread elsewhere first (KiUserApcDispatcher), that runs here, under a
 * call-out, until it comes back to the call's own state (NtContinue to the
 * saved context). */
uint32_t __gexw_syscall64( uint32_t id, const uint64_t *w, uint32_t n )
{
    int ctx = cur_ctx();
    uint64_t sp0 = REG( X_RSP ), pc0 = GEX_C64( GEX_CTX_PC ), sp;
    uint32_t ret;

    sp = syscall_enter( ctx, id, w, n );
    after_call( ctx, sp + 8 );
    ret = (uint32_t)REG( X_RAX );
    REG( X_RSP ) = sp0;
    GEX_C64( GEX_CTX_PC ) = pc0;
    return ret;
}

/* A system call that loads a new guest state (NtContinue, NtSetContextThread
 * on the thread itself): when it does, every native frame up to the
 * innermost entry from the guest goes, and the guest carries on from it. */
uint32_t __gexw_syscall64_resume( uint32_t id, const uint64_t *w, uint32_t n )
{
    int ctx = cur_ctx();
    uint64_t sp0 = REG( X_RSP ), pc0 = GEX_C64( GEX_CTX_PC ), sp;
    uint32_t ret;

    sp = syscall_enter( ctx, id, w, n );
    if (GEX_C64( GEX_CTX_PC ) != GEXW_RETURN_PAD || REG( X_RSP ) != sp + 8) __gexw_throw_unwind();
    ret = (uint32_t)REG( X_RAX );
    REG( X_RSP ) = sp0;
    GEX_C64( GEX_CTX_PC ) = pc0;
    return ret;
}

/* ---- 16-byte compare-and-exchange ---- */

/* wasm has no 16-byte atomic: x86-64's cmpxchg16b, which Wine's SList
 * code asks for (InterlockedCompareExchange128), under one process-wide
 * lock. Native callers agree with each other; guest code's cmpxchg16b on
 * the same memory does not take the lock. */
static volatile int cas16_lock;

int gexw_cas16( volatile void *ptr, void *expected, unsigned __int128 desired, int success, int failure )
    __asm__("__atomic_compare_exchange_16");
int gexw_cas16( volatile void *ptr, void *expected, unsigned __int128 desired, int success, int failure )
{
    unsigned __int128 cur;
    int ok;

    while (__atomic_exchange_n( &cas16_lock, 1, __ATOMIC_ACQUIRE )) __builtin_wasm_memory_atomic_wait32( (int *)&cas16_lock, 1, 1000 );
    __builtin_memcpy( &cur, (const void *)ptr, 16 );
    /* (compared as a value, not by memcmp: gexrt has no C runtime, and in
     * the one PE module ntdll's memcmp is ntdll's own) */
    if ((ok = cur == *(unsigned __int128 *)expected)) __builtin_memcpy( (void *)ptr, &desired, 16 );
    else __builtin_memcpy( expected, &cur, 16 );
    __atomic_store_n( &cas16_lock, 0, __ATOMIC_RELEASE );
    __builtin_wasm_memory_atomic_notify( (int *)&cas16_lock, 1 );
    return ok;
}

/* ---- varargs from the guest (winebuild's varargs adapters) ---- */

/* Win64 passes every variadic argument in an 8-byte slot; wasm's va_list
 * is a buffer with each argument at its natural size and alignment. They
 * differ for anything narrower than 8 bytes, which a printf format names:
 * walk it, and lay the guest's slots out as wasm does (`out` null: just
 * the size). LLP64's sizes: long is 4 bytes, size_t, ptrdiff_t, pointers
 * and %I are 8; MSVC's long double is a double. */
static uint32_t va_printf( uint64_t va, const void *fmt, int wide, char *out )
{
    const unsigned char *f8 = fmt;
    const unsigned short *f16 = fmt;
    uint32_t o = 0, i = 0;
    uint64_t in = 0;
#define FCH(k) (wide ? (unsigned int)f16[k] : (unsigned int)f8[k])
#define TAKE(n) do { \
        o = (o + (n) - 1) & ~((n) - 1u); \
        if (out) __builtin_memcpy( out + o, (const void *)(uintptr_t)(va + in), (n) ); \
        o += (n); in += 8; } while (0)
    if (!fmt) return 0;
    for (;;)
    {
        unsigned int c = FCH(i++), size = 4;
        if (!c) break;
        if (c != '%') continue;
        if (FCH(i) == '%') { i++; continue; }
        while ((c = FCH(i)) && (c == '-' || c == '+' || c == ' ' || c == '#' || c == '0' || c == '\'' ||
                                (c >= '1' && c <= '9') || c == '$'))
            i++;
        if (FCH(i) == '*') { TAKE(4); i++; }
        while ((c = FCH(i)) >= '0' && c <= '9') i++;
        if (FCH(i) == '.')
        {
            i++;
            if (FCH(i) == '*') { TAKE(4); i++; }
            while ((c = FCH(i)) >= '0' && c <= '9') i++;
        }
        for (;;)
        {
            c = FCH(i);
            if (c == 'I' && FCH(i + 1) == '6' && FCH(i + 2) == '4') { size = 8; i += 3; }
            else if (c == 'I' && FCH(i + 1) == '3' && FCH(i + 2) == '2') { size = 4; i += 3; }
            else if (c == 'l' && FCH(i + 1) == 'l') { size = 8; i += 2; }
            else if (c == 'j' || c == 'q' || c == 'z' || c == 't' || c == 'I') { size = 8; i++; }
            else if (c == 'h' || c == 'l' || c == 'L' || c == 'w' || c == 'T') i++;
            else break;
        }
        c = FCH(i);
        if (!c) break;
        i++;
        switch (c)
        {
        case 'e': case 'E': case 'f': case 'F': case 'g': case 'G': case 'a': case 'A':
            TAKE(8);
            break;
        case 'd': case 'i': case 'o': case 'u': case 'x': case 'X':
            if (size == 8) TAKE(8);
            else TAKE(4);
            break;
        case 'c': case 'C':
            TAKE(4);
            break;
        default:  /* s S Z p n: a pointer */
            TAKE(8);
            break;
        }
    }
#undef TAKE
#undef FCH
    return o + 64;  /* (and room for what the format does not name) */
}

uint32_t __gexw_va_printf_size( uint64_t va, const void *fmt, int wide )
{
    return va_printf( va, fmt, wide, 0 );
}

void __gexw_va_printf_copy( uint64_t va, const void *fmt, int wide, void *out )
{
    uint32_t n = va_printf( va, fmt, wide, out );
    __builtin_memset( (char *)out + n - 64, 0, 64 );
}

/* A varargs adapter's guest va_list: the register arguments written to
 * their home slots, then the address of slot `k`, the first variadic one. */
uint64_t __gexw_va_start( int ctx, uint32_t k )
{
    uint32_t i;
    for (i = 0; i < 4; i++) __gexw_argptr( ctx, i );
    return REG( X_RSP ) + 8 + 8 * (uint64_t)k;
}

/* The guest's FP control and status words (wine/gex/runtime.h), as on
 * wasm32: 0 the x87 control word, 1 its status word, 2 MXCSR. */
unsigned int __gexw_guest_fpu( unsigned int which, unsigned int set, unsigned int v )
{
    int ctx = cur_ctx();
    unsigned int off = which == 0 ? GEX_CTX_X87_CW : which == 1 ? GEX_CTX_X87_SW : GEX_CTX_MXCSR;
    unsigned int old = which == 2 ? GEX_C32( off ) : GEX_C16( off );
    if (set)
    {
        if (which == 2) GEX_C32( off ) = v;
        else GEX_C16( off ) = (uint16_t)v;
    }
    return old;
}

/* The ICALL pass's entry thunks: { function, thunk } pairs of table
 * indices (8 bytes each) in the data section gexw_entries, as on wasm32. */
extern const char __start_gexw_entries[], __stop_gexw_entries[];
static const void *const gexw_entries_anchor[2] __attribute__((used, section("gexw_entries")));
const void *const __gexw_entries[2] __attribute__((used)) = { __start_gexw_entries, __stop_gexw_entries };
extern const char __start_gexw_entries_decl[], __stop_gexw_entries_decl[];
static const void *const gexw_entries_decl_anchor[2] __attribute__((used, section("gexw_entries_decl")));
const void *const __gexw_entries_decl[2] __attribute__((used)) = { __start_gexw_entries_decl, __stop_gexw_entries_decl };

/* The DLLs a native module imports, for ntdll's loader (its skeleton has
 * no import table): pelink.py's moduleimports.c in the one PE module; none
 * for a DLL linked as its own module. */
__attribute__((weak)) const char *__gexw_module_imports( const unsigned short *name )
{
    return 0;
}

#endif /* __wasm64__ && !GEX_MEMORY64 */
