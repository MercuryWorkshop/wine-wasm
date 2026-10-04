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
 * address_space(1) global in position-independent code. */
__asm__(
    "\t.globaltype gexw_icall_target_g, i32\n"
    "gexw_icall_target_g:\n"
    "\t.globaltype gexw_ctx_g, i32\n"
    "gexw_ctx_g:\n"
    "\t.globaltype gexw_depth_g, i32\n"
    "gexw_depth_g:\n"
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
);

uint32_t gexw_get_icall_target(void);
int32_t gexw_get_ctx(void);
void gexw_set_ctx(int32_t);
int32_t gexw_get_depth(void);
void gexw_set_depth(int32_t);

__attribute__((import_module("gex"), import_name("provider_ctx"))) int gex_provider_ctx(void);
__attribute__((import_module("gex"), import_name("provider_missing"))) void gex_provider_missing(int addr);

/* ---- entry adapters (winebuild) ---- */

struct __gexw_frame { int ctx, depth; };

/* An adapter's entry: the thread's guest context (the same value on every
 * entry of a thread) and the call depth, for the exit stubs of any guest
 * call the native code makes. Not restored on return: the context does not
 * change, and a depth left higher by a nested entry only makes the depth
 * guard (a stack-overflow check) trip a little earlier. */
void __gexw_enter( struct __gexw_frame *f, int ctx, int depth )
{
    gexw_set_ctx( ctx );
    gexw_set_depth( depth );
}

void __gexw_leave( struct __gexw_frame *f )
{
}

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

/* ---- guest calls from native code (the ICALL pass's exit stubs) ---- */

/* Call the guest function at `target` with `n` stack words: push them and a
 * return address of our own, run it through gex's function table, and give
 * back EDX:EAX. The guest stack is put back as it was whatever the callee
 * popped (cdecl or stdcall). */
uint64_t __gexw_guest_call( uint32_t target, const uint32_t *words, uint32_t n )
{
    int ctx = gexw_get_ctx();
    uint32_t sp0, sp, ra, i;

    if (!ctx) ctx = gex_provider_ctx();
    sp0 = gp_reg( ctx, R_ESP );
    sp = sp0 - 4 * n - 4;
    ra = target ^ 1u;  /* in the callee's page, so tier-1's call-out can watch it */
    GW( sp ) = ra;
    for (i = 0; i < n; i++) GW( sp + 4 + 4 * i ) = words[i];
    gp_set( ctx, R_ESP, sp );
    GEX_C64( GEX_CTX_LAST_TARGET ) = target;
    if (!GEX_FTAB( gex_ftab_idx( target ))( ctx, gexw_get_depth() + 1, 0xffffffffu, ra ))
        __builtin_trap();  /* the guest left another way: W2 unwinds here */
    gp_set( ctx, R_ESP, sp0 );
    return ((uint64_t)gp_reg( ctx, R_EDX ) << 32) | gp_reg( ctx, R_EAX );
}

/* The same for a float or double result: ST0, popped. */
double __gexw_guest_call_fp( uint32_t target, const uint32_t *words, uint32_t n )
{
    int ctx = gexw_get_ctx();
    uint32_t top, se;
    uint64_t sig, sign;
    int32_t e, de;

    if (!ctx) ctx = gex_provider_ctx();
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
