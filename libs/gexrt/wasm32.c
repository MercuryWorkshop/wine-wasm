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
    /* the guest's stack pointer at the innermost entry: guest code that
     * moves above it (a catch block continuing in an outer frame, a
     * longjmp) has left every native frame since */
    "\t.globaltype gexw_entry_esp_g, i32\n"
    "gexw_entry_esp_g:\n"
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
    "\t.globl gexw_get_entry_esp\n"
    "\t.type gexw_get_entry_esp,@function\n"
    "gexw_get_entry_esp:\n"
    "\t.functype gexw_get_entry_esp () -> (i32)\n"
    "\tglobal.get gexw_entry_esp_g\n"
    "\tend_function\n"
    "\t.globl gexw_set_entry_esp\n"
    "\t.type gexw_set_entry_esp,@function\n"
    "gexw_set_entry_esp:\n"
    "\t.functype gexw_set_entry_esp (i32) -> ()\n"
    "\tlocal.get 0\n"
    "\tglobal.set gexw_entry_esp_g\n"
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
    "\t.globl __gexw_entry_raw\n"
    "\t.type __gexw_entry_raw,@function\n"
    "__gexw_entry_raw:\n"
    "\t.functype __gexw_entry_raw (i32, i32, i32, i64, i32) -> (i32)\n"
    "\t.local i32, i32, i32, i32, i32, i32, i32\n"
    "\tglobal.get gexw_entry_esp_g\n"
    "\tlocal.set 11\n"
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
    "\tglobal.set gexw_entry_esp_g\n"
    "\tlocal.get 9\n"
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
    "\tlocal.get 11\n"
    "\tglobal.set gexw_entry_esp_g\n"
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
    "\tglobal.set gexw_entry_esp_g\n"
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
uint32_t gexw_get_entry_esp(void);
void gexw_set_entry_esp(uint32_t);
int __gexw_entry_raw(int, int, unsigned int, unsigned long long, int (*)(int, int, unsigned int, unsigned long long));
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

/* Return to the guest with every register as it is (an entry that must
 * keep EAX, ECX and EDX: msvcrt's _chkesp). */
int __gexw_ret_only( int ctx, uint64_t ret, uint32_t pop )
{
    return gp_return( ctx, ret, pop );
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

/* The return address of a guest call from native code: the last word of
 * gex-kernel's NT trampoline page (int3 filler). */
#define GEXW_RETURN_PAD 0x3ffff1u

/* Run the guest from the context until it reaches `stop` (a return
 * address of ours, or a syscall's), as gex's own dispatch loop does: an
 * F_body that returns 0 has written where to go on into the context. */
static void run_until( int ctx, uint32_t stop, uint32_t limit )
{
    int depth = gexw_get_depth() + 1;
    for (;;)
    {
        uint32_t pc = (uint32_t)GEX_C64( GEX_CTX_PC );
        if (pc == stop) return;
        /* The guest went on above our entry's stack pointer (a catch block
         * continuing in an outer frame, a longjmp): every native frame
         * since that entry is dead. Drop them; the entry returns 0 and the
         * guest goes on from the context. */
        if (limit && gp_reg( ctx, R_ESP ) >= limit) __gexw_throw_unwind();
        GEX_C64( GEX_CTX_LAST_TARGET ) = pc;
        if (GEX_FTAB( gex_ftab_idx( pc ))( ctx, depth, 0xffffffffu, stop )) return;
        /* a trap through native frames inside it skipped their entries'
         * restores */
        gexw_set_entry_esp( limit );
    }
}

/* ---- entering native code ---- */

#define GEX_NR_NATIVE_LEFT 0x7f000003u
#define GEXW_JMP_EAX 0x3f0080u
/* gex-kernel's NATIVE_SYSCALL mark on a system call's pc (see below) */
#define GEX_NATIVE_SYSCALL 0x2000000000000000ull

/* Every entry from the guest (winebuild's adapters, the entry thunks). Guest
 * code entering from above the innermost entry's stack has left that
 * entry's native frames (a catch block continuing in an outer frame) and is
 * running nested inside them: gex abandons the innermost call-out, its
 * native caller drops its frames (run_until), and the guest re-enters here
 * from the context, one level flatter, until it is not nested. */
int __gexw_entry( int ctx, int depth, unsigned int need, unsigned long long ret,
                  int (*body)(int, int, unsigned int, unsigned long long) )
{
    uint32_t e = gexw_get_entry_esp();
    /* (at or above: a live entry's nested callees run below its native
     * frames, which are below its own stack pointer) */
    if (e && gp_reg( ctx, R_ESP ) >= e)
    {
        uint32_t eax = gp_reg( ctx, R_EAX );
        /* re-entered from the guest's side, `jmp eax` to this function's
         * address (gex-kernel's tramp::JMP_EAX_32): no x86 convention passes
         * an argument in EAX */
        GEX_C64( GEX_CTX_PC ) = GEXW_JMP_EAX;
        gp_set( ctx, R_EAX, GEX_NR_NATIVE_LEFT );
        /* stale if no call-out was left (a trap skipped a restore): then
         * the re-entry just goes on */
        gexw_set_entry_esp( 0 );
        /* this function's address, for EAX when the host leaves (ctx::BALLAST,
         * which nothing reads) */
        GEX_C64( 344u ) = GP_SELF_PC();
        gex_syscall( ctx, (int64_t)(GEX_NATIVE_SYSCALL | GEXW_JMP_EAX) );
        /* reached only when no call-out was left: as if nothing */
        gp_set( ctx, R_EAX, eax );
    }
    return __gexw_entry_raw( ctx, depth, need, ret, body );
}

/* Call the guest function at `target` with `n` stack words: push them and a
 * return address of our own below the native stack, run it through gex's
 * function table, and give back EDX:EAX. The guest stack pointer is put
 * back as it was whatever the callee popped (cdecl or stdcall). */
uint64_t __gexw_guest_call( uint32_t target, const uint32_t *words, uint32_t n )
{
    int ctx = cur_ctx();
    uint32_t sp0, sp, ra, i, limit;

    sp0 = gp_reg( ctx, R_ESP );
    /* below everything native code has live */
    sp = ((gexw_get_sp() - 64) & ~15u) - 4 * n - 4;
    /* An address no code has, in a page with a dispatch table (gex-kernel's
     * NT trampolines, which run before any of this), so tier-1's call-out
     * can watch it. Not in the callee's page: a watched slot there could
     * be one the callee's own code needs (its table's indices are the
     * page's addresses). */
    ra = GEXW_RETURN_PAD;
    GW( sp ) = ra;
    for (i = 0; i < n; i++) GW( sp + 4 + 4 * i ) = words[i];
    gp_set( ctx, R_ESP, sp );
    GEX_C64( GEX_CTX_PC ) = target;
    GEX_C64( GEX_CTX_LAST_TARGET ) = target;
    /* the guest's stack pointer while native code runs is the innermost
     * entry's (native code moves it only around its guest calls, and puts
     * it back): what the guest leaves the native frames by going above */
    limit = sp0;
    if (!GEX_FTAB( gex_ftab_idx( target ))( ctx, gexw_get_depth() + 1, 0xffffffffu, ra ))
    {
        gexw_set_entry_esp( limit );
        run_until( ctx, ra, limit );
    }
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

/* ---- guest state for the i386 C runtime (msvcrt's except_i386.c on wasm32) ---- */

/* The guest's register `r` (R_EAX..R_EDI) as it is now: at an entry, the
 * caller's. */
uint32_t __gexw_guest_reg( unsigned r )
{
    int ctx = cur_ctx();
    return gp_reg( ctx, r );
}

void __gexw_guest_set_reg( unsigned r, uint32_t v )
{
    int ctx = cur_ctx();
    gp_set( ctx, r, v );
}

/* Leave native code for the guest at `eip`, with the registers as they are
 * in the context (an i386 catch block's continuation, longjmp): every
 * native frame since the innermost entry goes (__gexw_unwind), that entry
 * returns 0, and the guest goes on from the context; outer native frames
 * the guest has left go too (run_until, NATIVE_LEFT). */
void __gexw_guest_jump( uint32_t eip )
{
    int ctx = cur_ctx();
    GEX_C64( GEX_CTX_PC ) = eip;
    __gexw_throw_unwind();
}

/* Call guest code that addresses its caller's frame through EBP (a catch
 * block, a filter or a __finally of MSVC's i386 code) with EBP = `ebp`; the
 * callee-saved registers are put back after, as the i386 helpers' pushes
 * and pops do. EAX comes back. */
uint32_t __gexw_guest_call_ebp( uint32_t target, uint32_t ebp, const uint32_t *words, uint32_t n )
{
    int ctx = cur_ctx();
    uint32_t sbp = gp_reg( ctx, R_EBP ), sbx = gp_reg( ctx, R_EBX ), ssi = gp_reg( ctx, R_ESI ),
             sdi = gp_reg( ctx, R_EDI ), r;
    gp_set( ctx, R_EBP, ebp );
    r = (uint32_t)__gexw_guest_call( target, words, n );
    gp_set( ctx, R_EBP, sbp );
    gp_set( ctx, R_EBX, sbx );
    gp_set( ctx, R_ESI, ssi );
    gp_set( ctx, R_EDI, sdi );
    return r;
}

/* CALL_VTBL_FUNC (msvcrt's cxx.h) on an object that may be the guest's: a
 * native entry as is (called as C, `this` first); a guest one through
 * gex-kernel's tramp::VTBL_WRAPPERS_32 entry for the offset, x86 that
 * moves the `this` the caller pushed into ECX and jumps through the vtable
 * (i386 Wine's vtbl_wrapper_N). */
#define GEXW_VTBL_WRAPPERS 0x3f0100u
void *__gexw_vtbl_target( void *this, unsigned int off )
{
    void *entry = (*(void ***)this)[off / 4];
    if ((uint32_t)(uintptr_t)entry < 0x3f0000u || off / 4 >= 64) return entry;
    return (void *)(uintptr_t)(GEXW_VTBL_WRAPPERS + 16u * (off / 4));
}

/* ---- varargs from the guest (winebuild's varargs adapters) ---- */

/* i386 passes every variadic argument in 4-byte stack slots (8 bytes for a
 * double or an int64, 4-aligned); wasm32's va_list is a buffer with each
 * argument at its natural alignment. They differ only for the 8-byte ones,
 * which a printf format names: walk it, and lay the guest's arguments out
 * as wasm32 does (`out` null: just the size). */
static uint32_t va_printf( uint32_t va, const void *fmt, int wide, char *out )
{
    const unsigned char *f8 = fmt;
    const unsigned short *f16 = fmt;
    uint32_t in = 0, o = 0, i = 0;
#define FCH(k) (wide ? (unsigned int)f16[k] : (unsigned int)f8[k])
#define TAKE(n) do { \
        if ((n) == 8) o = (o + 7) & ~7u; \
        if (out) __builtin_memcpy( out + o, (const void *)(uintptr_t)(va + in), (n) ); \
        o += (n); in += (n); } while (0)
    if (!fmt) return 0;
    for (;;)
    {
        unsigned int c = FCH(i++), size = 4;
        if (!c) break;
        if (c != '%') continue;
        if (FCH(i) == '%') { i++; continue; }
        /* positional (%n$): left alone, read in order */
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
            else if (c == 'j' || c == 'q') { size = 8; i++; }
            else if (c == 'h' || c == 'l' || c == 'L' || c == 'w' || c == 'z' || c == 't' || c == 'I' || c == 'T') i++;
            else break;
        }
        c = FCH(i);
        if (!c) break;
        i++;
        switch (c)
        {
        case 'e': case 'E': case 'f': case 'F': case 'g': case 'G': case 'a': case 'A':
            TAKE(8);  /* a double (a float is promoted; MSVC's long double is one) */
            break;
        case 'd': case 'i': case 'o': case 'u': case 'x': case 'X':
            TAKE(size);
            break;
        default:  /* c C s S Z p n: a word */
            TAKE(4);
            break;
        }
    }
#undef TAKE
#undef FCH
    return o + 64;  /* (and room for what the format does not name) */
}

uint32_t __gexw_va_printf_size( uint32_t va, const void *fmt, int wide )
{
    return va_printf( va, fmt, wide, 0 );
}

void __gexw_va_printf_copy( uint32_t va, const void *fmt, int wide, void *out )
{
    uint32_t n = va_printf( va, fmt, wide, out );
    /* (slack past what the format names: zeros) */
    __builtin_memset( (char *)out + n - 64, 0, 64 );
}

/* ---- NT system calls from native code ---- */

/* gex-kernel's NT layer reads an i386 system call's arguments above two
 * return addresses, as Wine's i386 stubs leave them. The `pc` passed is
 * marked native (GEX_NATIVE_SYSCALL): a call that would resume the guest
 * elsewhere (an APC delivered by an alertable wait, a user-mode callback's
 * end) writes that state into the context and returns normally, so the
 * native frames below it survive. */

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
    if ((uint32_t)GEX_C64( GEX_CTX_PC ) != stop) run_until( ctx, stop, esp0 );
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

/* Pop the guest's ST0 as a double (the i386 ABI's floating-point result,
 * and the _CI* functions' arguments). */
double __gexw_x87_pop( void )
{
    int ctx = cur_ctx();
    uint32_t top, se;
    uint64_t sig, sign;
    int32_t e, de;

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

/* The same for a float or double result: ST0, popped. */
double __gexw_guest_call_fp( uint32_t target, const uint32_t *words, uint32_t n )
{
    (void)__gexw_guest_call( target, words, n );
    return __gexw_x87_pop();
}

/* The guest's FP control and status words: 0 the x87 control word, 1 its
 * status word, 2 MXCSR. `set`: write `v`; the value (old, or read) back. */
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

/* The guest's XMM registers (the SSE2 libm entry points' arguments). */
double __gexw_guest_xmm_f64( unsigned n )
{
    int ctx = cur_ctx();
    return gp_xmm_f64( ctx, n );
}

void __gexw_guest_set_xmm_f64( unsigned n, double v )
{
    int ctx = cur_ctx();
    gp_set_xmm_f64( ctx, n, v );
}

float __gexw_guest_xmm_f32( unsigned n )
{
    int ctx = cur_ctx();
    return gex_f32( (uint32_t)GEX_C64( GEX_CTX_VN( n )) );
}

void __gexw_guest_set_xmm_f32( unsigned n, float v )
{
    int ctx = cur_ctx();
    uint32_t b;
    __builtin_memcpy( &b, &v, 4 );
    GEX_C64( GEX_CTX_VN( n )) = (GEX_C64( GEX_CTX_VN( n )) & ~0xffffffffull) | b;
}

/* The ICALL pass's entry thunks: { function, thunk } pairs of table
 * indices in the data section gexw_entries (one run per DLL object). gex
 * reads these bounds (exported by pelink) once the module's data is
 * relocated, and binds each thunk at its function's index as a guest
 * address: i386 code that calls a native function pointer runs it. */
extern const char __start_gexw_entries[], __stop_gexw_entries[];
/* a null pair, so the section (and its bounds) exist in every module */
static const void *const gexw_entries_anchor[2] __attribute__((used, section("gexw_entries")));
const void *const __gexw_entries[2] __attribute__((used)) = { __start_gexw_entries, __stop_gexw_entries };

#endif /* __wasm32__ */
