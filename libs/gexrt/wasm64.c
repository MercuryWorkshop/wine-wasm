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

/* x86-64 registers in gex's context, numbered as x86 encodes them */
enum { X_RAX, X_RCX, X_RDX, X_RBX, X_RSP, X_RBP, X_RSI, X_RDI, X_R8, X_R9 };
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

    /* __gexw_entry(ctx, depth, need, ret, body): an entry from the guest
     * into native code, as on wasm32: the native code runs on the guest
     * thread's stack, below the guest's RSP (Win64 has no red zone); the
     * per-thread globals and the C stack pointer are put back on the way
     * out; a __gexw_unwind from inside ends the call with 0. */
    "\t.globl __gexw_entry\n"
    "\t.type __gexw_entry,@function\n"
    "__gexw_entry:\n"
    "\t.functype __gexw_entry (i32, i32, i32, i64, i64) -> (i32)\n"
    /* 5 sp, 6 ctx, 7 depth, 8 retaddr, 9 rsp, 11 entry rsp (i64 but 6, 7);
     * 10 the result */
    "\t.local i64, i32, i32, i64, i64, i32, i64\n"
    "\tglobal.get gexw_entry_rsp_g\n"
    "\tlocal.set 11\n"
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

/* The return address of a guest call from native code: gex-kernel's
 * NATIVE_RETURN_PAD (the last byte of its trampoline page, int3 filler). */
#define GEXW_RETURN_PAD 0x3ffff1u

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
 * no dispatch entry until a call-out translates it.) When the call-out
 * cannot be made or is abandoned (an outer one returned first: the guest
 * left these frames), every native frame up to the innermost entry goes. */
uint64_t __gexw_guest_call64( uint64_t target, const uint64_t *slots, uint32_t n, uint32_t fp )
{
    int ctx = cur_ctx();
    uint64_t sp0 = REG( X_RSP ), pc0 = GEX_C64( GEX_CTX_PC ), sp, ra = GEXW_RETURN_PAD;
    uint32_t i, room = n < 4 ? 4 : n;

    /* 16-aligned at the call: the return address goes just below */
    sp = ((gexw_get_sp() - 64) & ~15ull) - 8 * (uint64_t)((room + 1) & ~1u);
    for (i = 0; i < room; i++) GQ( sp + 8 * i ) = i < n ? slots[i] : 0;
    for (i = 0; i < 4 && i < n; i++)
    {
        REG( arg_reg[i] ) = slots[i];
        GEX_C64( GEX_CTX_VN( i )) = slots[i];
        GEX_C64( GEX_CTX_VN( i ) + 8 ) = 0;
    }
    sp -= 8;
    GQ( sp ) = ra;
    REG( X_RSP ) = sp;
    GEX_C64( GEX_CTX_PC ) = target;
    GEX_C64( GEX_CTX_LAST_TARGET ) = target;
    if (!gex_call_out( ctx, (int64_t)target, (int64_t)ra, (int64_t)(sp + 8) )) __gexw_throw_unwind();
    REG( X_RSP ) = sp0;
    GEX_C64( GEX_CTX_PC ) = pc0;
    return fp ? GEX_C64( GEX_CTX_VN( 0 )) : REG( X_RAX );
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

/* The ICALL pass's entry thunks: { function, thunk } pairs of table
 * indices (8 bytes each) in the data section gexw_entries, as on wasm32. */
extern const char __start_gexw_entries[], __stop_gexw_entries[];
static const void *const gexw_entries_anchor[2] __attribute__((used, section("gexw_entries")));
const void *const __gexw_entries[2] __attribute__((used)) = { __start_gexw_entries, __stop_gexw_entries };
extern const char __start_gexw_entries_decl[], __stop_gexw_entries_decl[];
static const void *const gexw_entries_decl_anchor[2] __attribute__((used, section("gexw_entries_decl")));
const void *const __gexw_entries_decl[2] __attribute__((used)) = { __start_gexw_entries_decl, __stop_gexw_entries_decl };

#endif /* __wasm64__ && !GEX_MEMORY64 */
