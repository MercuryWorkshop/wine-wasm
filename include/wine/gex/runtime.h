/*
 * gexrt's interface for native Wine code that works on the guest's i386
 * state (msvcrt's exception handling, setjmp/longjmp, thiscall calls into
 * guest objects): libs/gexrt/wasm32.c, docs/wine-native.md (gex) §3.15.
 */
#ifndef __WINE_GEX_RUNTIME_H
#define __WINE_GEX_RUNTIME_H

#ifdef __wasm32__

#define GEXW_EAX 0
#define GEXW_ECX 1
#define GEXW_EDX 2
#define GEXW_EBX 3
#define GEXW_ESP 4
#define GEXW_EBP 5
#define GEXW_ESI 6
#define GEXW_EDI 7

/* Function pointers below this are native (function-table indices); at or
 * above it, guest addresses. */
#define GEXW_ICALL_FLOOR 0x3f0000u

unsigned int __gexw_guest_reg( unsigned int r );
void __gexw_guest_set_reg( unsigned int r, unsigned int v );
/* Leave native code for the guest at `eip` (registers as set). */
void __gexw_guest_jump( unsigned int eip ) __attribute__((noreturn));
/* Call guest code at `target` with `n` stack words: EDX:EAX back. */
unsigned long long __gexw_guest_call( unsigned int target, const unsigned int *words, unsigned int n );
/* The same with ECX and EDX set (thiscall, fastcall). */
unsigned long long __gexw_guest_call_regs( unsigned int target, unsigned int ecx, unsigned int edx,
                                           const unsigned int *words, unsigned int n );
/* The same with EBP set and the callee-saved registers kept: EAX back. */
unsigned int __gexw_guest_call_ebp( unsigned int target, unsigned int ebp, const unsigned int *words,
                                    unsigned int n );

/* The guest's x87 ST0, popped; its XMM registers' low lanes. */
double __gexw_x87_pop( void );
double __gexw_guest_xmm_f64( unsigned int n );
void __gexw_guest_set_xmm_f64( unsigned int n, double v );
float __gexw_guest_xmm_f32( unsigned int n );
void __gexw_guest_set_xmm_f32( unsigned int n, float v );

static inline int __gexw_is_native( const void *f )
{
    return (unsigned int)(unsigned long)f < GEXW_ICALL_FLOOR;
}

#endif  /* __wasm32__ */

#endif  /* __WINE_GEX_RUNTIME_H */
