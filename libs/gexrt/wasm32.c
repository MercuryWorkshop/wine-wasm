/*
 * Native Wine modules on wasm32 under gex: the run-time half of the spec
 * glue (winebuild's wasm32 entry adapters) and of the ICALL pass's exit
 * stubs. See docs/wine-native.md in the gex tree.
 *
 * Copyright 2026 the gex authors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifdef __wasm32__

#include <stdint.h>
#include "wine/gex/gex_abi.h"
#include "wine/gex/provider.h"

/* Per-thread state: wasm globals of the module instance (gex instantiates
 * a module once per thread). Assembly, because clang cannot address an
 * address_space(1) global in position-independent code.
 *
 * - gexw_ctx_g: the thread's guest context (0 until the first entry);
 * - gexw_depth_g: the call depth of the innermost entry from the guest;
 * - gexw_retaddr_g: that entry's guest return address (what
 *   __builtin_return_address means to Wine on wasm32);
 * - gexw_icall_target_g: the ICALL pass's guest target. */
__asm__(
    "\t.globaltype __stack_pointer, i32\n"
    "\t.globaltype gexw_icall_target_g, i32\n"
    "gexw_icall_target_g:\n"
    "\t.globaltype gexw_ctx_g, i32\n"
    "gexw_ctx_g:\n"
    "\t.globaltype gexw_depth_g, i32\n"
    "gexw_depth_g:\n"
    "\t.globaltype gexw_retaddr_g, i32\n"
    "gexw_retaddr_g:\n"
    /* thrown by native code that has written a new guest state into the
     * context (NtContinue and the like): every native frame up to the
     * innermost entry from the guest is dropped, and that entry returns
     * 0, so its caller goes on from the context's PC (docs/wine-native.md
     * §3.15) */
    "\t.tagtype __gexw_unwind\n"
    "\t.globl __gexw_unwind\n"
    "__gexw_unwind:\n"
    "\t.globl gexw_get_icall_target\n"
    "\t.type gexw_get_icall_target,@function\n"
    "gexw_get_icall_target:\n"
    "\t.functype gexw_get_icall_target () -> (i32)\n"
    "\tglobal.get gexw_icall_target_g\n"
    "\tend_function\n"
    "\t.globl gexw_set_icall_target\n"
    "\t.type gexw_set_icall_target,@function\n"
    "gexw_set_icall_target:\n"
    "\t.functype gexw_set_icall_target (i32) -> ()\n"
    "\tlocal.get 0\n"
    "\tglobal.set gexw_icall_target_g\n"
    "\tend_function\n"
    "\t.globl gexw_get_ctx\n"
    "\t.type gexw_get_ctx,@function\n"
    "gexw_get_ctx:\n"
    "\t.functype gexw_get_ctx () -> (i32)\n"
    "\tglobal.get gexw_ctx_g\n"
    "\tend_function\n"
    "\t.globl gexw_set_ctx\n"
    "\t.type gexw_set_ctx,@function\n"
    "gexw_set_ctx:\n"
    "\t.functype gexw_set_ctx (i32) -> ()\n"
    "\tlocal.get 0\n"
    "\tglobal.set gexw_ctx_g\n"
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
    "\t.functype gexw_get_retaddr () -> (i32)\n"
    "\tglobal.get gexw_retaddr_g\n"
    "\tend_function\n"
    "\t.globl gexw_get_sp\n"
    "\t.type gexw_get_sp,@function\n"
    "gexw_get_sp:\n"
    "\t.functype gexw_get_sp () -> (i32)\n"
    "\tglobal.get __stack_pointer\n"
    "\tend_function\n"
    "\t.globl __gexw_throw_unwind\n"
    "\t.type __gexw_throw_unwind,@function\n"
    "__gexw_throw_unwind:\n"
    "\t.functype __gexw_throw_unwind () -> ()\n"
    "\tthrow __gexw_unwind\n"
    "\tend_function\n"

    /* __gexw_entry(ctx, depth, need, ret, body): an entry from the guest
     * into native code (every winebuild adapter is one). The native code
     * runs on the guest thread's own stack, below the guest's stack
     * pointer, as Windows' does: SEH frames, stack limits and fibers see
     * one stack. The per-thread globals and the C stack pointer are put
     * back on the way out; a __gexw_unwind from inside ends the call
     * with 0 (the context holds the state to go on from). */
    "\t.globl __gexw_entry\n"
    "\t.type __gexw_entry,@function\n"
    "__gexw_entry:\n"
    "\t.functype __gexw_entry (i32, i32, i32, i64, i32) -> (i32)\n"
    "\t.local i32, i32, i32, i32, i32, i32\n"
    "\tglobal.get __stack_pointer\n"
    "\tlocal.set 5\n"
    "\tglobal.get gexw_ctx_g\n"
    "\tlocal.set 6\n"
    "\tglobal.get gexw_depth_g\n"
    "\tlocal.set 7\n"
    "\tglobal.get gexw_retaddr_g\n"
    "\tlocal.set 8\n"
    /* esp, the guest's stack pointer */
    "\tlocal.get 0\n"
    "\ti32.load 32\n"
    "\tlocal.tee 9\n"
    "\ti32.load 0\n"
    "\tglobal.set gexw_retaddr_g\n"
    "\tlocal.get 9\n"
    "\ti32.const 64\n"
    "\ti32.sub\n"
    "\ti32.const -16\n"
    "\ti32.and\n"
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
    "\ti32.const 0\n"
    "\tend_function\n"
);

uint32_t gexw_get_icall_target(void);
int32_t gexw_get_ctx(void);
void gexw_set_ctx(int32_t);
int32_t gexw_get_depth(void);
void gexw_set_depth(int32_t);
uint32_t gexw_get_retaddr(void);
uint32_t gexw_get_sp(void);
void __gexw_throw_unwind(void) __attribute__((noreturn));

__attribute__((import_module("gex"), import_name("provider_ctx"))) int gex_provider_ctx(void);
__attribute__((import_module("gex"), import_name("provider_missing"))) void gex_provider_missing(int addr);

/* The thread's guest context, from inside native code. */
static inline int cur_ctx(void)
{
    int ctx = gexw_get_ctx();
    return ctx ? ctx : gex_provider_ctx();
}

/* NtCurrentTeb(): the guest thread's FS base. */
void *__stdcall __wine_wasm_current_teb(void)
{
    int ctx = cur_ctx();
    return (void *)(uintptr_t)(uint32_t)GEX_C64( GEX_CTX_TPIDR );
}

/* __builtin_return_address(0) in Wine's code: the guest's return address
 * of the innermost call from the guest into native code. */
void *__stdcall __wine_wasm_return_address(void)
{
    return (void *)(uintptr_t)gexw_get_retaddr();
}

/* ---- entry adapters (winebuild) ---- */

/* The stack argument at byte offset `off` past the return address. */
uint32_t __gexw_arg32( int ctx, uint32_t off ) { return GW( gp_reg( ctx, R_ESP ) + 4 + off ); }
uint64_t __gexw_arg64( int ctx, uint32_t off ) { return *(gex_u64 *)GEX_P( gp_reg( ctx, R_ESP ) + 4 + off ); }
float __gexw_argf32( int ctx, uint32_t off ) { return gex_f32( __gexw_arg32( ctx, off )); }
double __gexw_argf64( int ctx, uint32_t off ) { return gex_f64( __gexw_arg64( ctx, off )); }
uint32_t __gexw_reg( int ctx, uint32_t r ) { return gp_reg( ctx, r ); }
/* The guest address of a stack argument: a struct passed by value. */
uint32_t __gexw_argptr( int ctx, uint32_t off ) { return gp_reg( ctx, R_ESP ) + 4 + off; }

/* Return to the guest: EDX:EAX, pop the return address and `pop` bytes. */
int __gexw_ret( int ctx, uint64_t ret, uint32_t pop, uint64_t edx_eax )
{
    gp_set( ctx, R_EDX, (uint32_t)(edx_eax >> 32) );
    return gp_return_eax( ctx, ret, pop, (uint32_t)edx_eax );
}

/* The same with a 32-bit (or no) result: EAX only. */
int __gexw_ret32( int ctx, uint64_t ret, uint32_t pop, uint32_t eax )
{
    return gp_return_eax( ctx, ret, pop, eax );
}

/* The same with a float result in ST0. */
int __gexw_ret_fp( int ctx, uint64_t ret, uint32_t pop, double v )
{
    gp_push_st0( ctx, v );
    return gp_return( ctx, ret, pop );
}

void __gexw_missing( int ctx )
{
    gex_provider_missing( (int)GP_SELF_PC() );
}

/* ---- running guest code from native code ---- */

/* Run the guest from the context until it reaches `stop` (a return
 * address of ours, or a syscall's), as gex's own dispatch loop does: an
 * F_body that returns 0 has written where to go on into the context. */
static void run_until( int ctx, uint32_t stop )
{
    int depth = gexw_get_depth() + 1;
    for (;;)
    {
        uint32_t pc = (uint32_t)GEX_C64( GEX_CTX_PC );
        if (pc == stop) return;
        GEX_C64( GEX_CTX_LAST_TARGET ) = pc;
        if (GEX_FTAB( gex_ftab_idx( pc ))( ctx, depth, 0xffffffffu, stop )) return;
    }
}

/* Call the guest function at `target` with `n` stack words: push them and a
 * return address of our own below the native stack, run it through gex's
 * function table, and give back EDX:EAX. The guest stack pointer is put
 * back as it was whatever the callee popped (cdecl or stdcall). */
uint64_t __gexw_guest_call( uint32_t target, const uint32_t *words, uint32_t n )
{
    int ctx = cur_ctx();
    uint32_t sp0, sp, ra, i;

    sp0 = gp_reg( ctx, R_ESP );
    /* below everything native code has live */
    sp = ((gexw_get_sp() - 64) & ~15u) - 4 * n - 4;
    ra = target ^ 1u;  /* in the callee's page, so tier-1's call-out can watch it */
    GW( sp ) = ra;
    for (i = 0; i < n; i++) GW( sp + 4 + 4 * i ) = words[i];
    gp_set( ctx, R_ESP, sp );
    GEX_C64( GEX_CTX_PC ) = target;
    GEX_C64( GEX_CTX_LAST_TARGET ) = target;
    if (!GEX_FTAB( gex_ftab_idx( target ))( ctx, gexw_get_depth() + 1, 0xffffffffu, ra ))
        run_until( ctx, ra );
    gp_set( ctx, R_ESP, sp0 );
    return ((uint64_t)gp_reg( ctx, R_EDX ) << 32) | gp_reg( ctx, R_EAX );
}

/* The same with ECX and EDX set: an i386 fastcall or thiscall callee. */
uint64_t __gexw_guest_call_regs( uint32_t target, uint32_t ecx, uint32_t edx, const uint32_t *words, uint32_t n )
{
    int ctx = cur_ctx();
    gp_set( ctx, R_ECX, ecx );
    gp_set( ctx, R_EDX, edx );
    return __gexw_guest_call( target, words, n );
}

/* ---- NT system calls from native code ---- */

/* gex-kernel's NT layer reads an i386 system call's arguments above two
 * return addresses, as Wine's i386 stubs leave them. The `pc` passed is
 * marked native (GEX_NATIVE_SYSCALL): a call that would resume the guest
 * elsewhere (an APC delivered by an alertable wait, a user-mode callback's
 * end) writes that state into the context and returns normally, so the
 * native frames below it survive. */
#define GEX_NATIVE_SYSCALL 0x2000000000000000ull

static uint32_t syscall_enter( int ctx, uint32_t id, const uint32_t *words, uint32_t n, uint32_t *stop )
{
    uint32_t sp = ((gexw_get_sp() - 64) & ~15u) - 4 * n - 8, i;
    for (i = 0; i < n; i++) GW( sp + 8 + 4 * i ) = words[i];
    /* the state a nested dispatch (an APC) comes back to: below the
     * frame, an address no code has */
    *stop = sp - 16;
    GW( sp ) = *stop;
    GW( sp + 4 ) = *stop;
    gp_set( ctx, R_ESP, sp );
    gp_set( ctx, R_EAX, id );
    GEX_C64( GEX_CTX_PC ) = *stop;
    gex_syscall( ctx, (int64_t)(GEX_NATIVE_SYSCALL | *stop) );
    return sp;
}

/* A system call that returns to its caller. If the kernel sent the thread
 * elsewhere first (KiUserApcDispatcher), that runs here, nested, until it
 * comes back to the call's own state (NtContinue to the saved context). */
uint32_t __gexw_syscall( uint32_t id, const uint32_t *words, uint32_t n )
{
    int ctx = cur_ctx();
    uint32_t esp0 = gp_reg( ctx, R_ESP ), pc0 = (uint32_t)GEX_C64( GEX_CTX_PC ), stop, ret;

    syscall_enter( ctx, id, words, n, &stop );
    if ((uint32_t)GEX_C64( GEX_CTX_PC ) != stop) run_until( ctx, stop );
    ret = gp_reg( ctx, R_EAX );
    gp_set( ctx, R_ESP, esp0 );
    GEX_C64( GEX_CTX_PC ) = pc0;
    return ret;
}

/* A system call that loads a new guest state (NtContinue, NtSetContextThread
 * on the thread itself): when it does, every native frame up to the
 * innermost entry from the guest goes, and the guest carries on from it. */
uint32_t __gexw_syscall_resume( uint32_t id, const uint32_t *words, uint32_t n )
{
    int ctx = cur_ctx();
    uint32_t esp0 = gp_reg( ctx, R_ESP ), pc0 = (uint32_t)GEX_C64( GEX_CTX_PC ), stop, ret;

    syscall_enter( ctx, id, words, n, &stop );
    if ((uint32_t)GEX_C64( GEX_CTX_PC ) != stop) __gexw_throw_unwind();
    ret = gp_reg( ctx, R_EAX );
    gp_set( ctx, R_ESP, esp0 );
    GEX_C64( GEX_CTX_PC ) = pc0;
    return ret;
}

/* The same for a float or double result: ST0, popped. */
double __gexw_guest_call_fp( uint32_t target, const uint32_t *words, uint32_t n )
{
    int ctx = cur_ctx();
    uint32_t top, se;
    uint64_t sig, sign;
    int32_t e, de;

    (void)__gexw_guest_call( target, words, n );
    top = GEX_C32( GEX_CTX_X87_TOP ) & 7u;
    sig = GEX_C64( GEX_CTX_X87 + 16u * top );
    se = GEX_C16( GEX_CTX_X87 + 16u * top + 8u );
    GEX_C32( GEX_CTX_X87_TAGS ) &= ~(1u << top);
    GEX_C32( GEX_CTX_X87_TOP ) = (top + 1u) & 7u;
    sign = (uint64_t)(se >> 15) << 63;
    e = (int32_t)(se & 0x7fff);
    if (e == 0 && sig == 0) return gex_f64( sign );
    if (e == 0x7fff) return gex_f64( sign | 0x7ff0000000000000ull | ((sig << 1) >> 12) );
    de = e - 16383 + 1023;
    if (de >= 0x7ff) return gex_f64( sign | 0x7ff0000000000000ull );
    if (de <= 0) return gex_f64( 12 - de >= 64 ? sign : sign | (sig >> (12 - de)) );
    return gex_f64( sign | ((uint64_t)de << 52) | ((sig << 1) >> 12) );
}

#endif /* __wasm32__ */
