/*
 * Typed system-call wrappers for win32u's Unix side in 64-bit native Wine
 * under gex (W64.4, gex docs/wine-native-x64.md), as ntdll's
 * (dlls/ntdll/unix/syscall_wasm64.cpp): the program's x86-64 code passes a
 * system call's arguments as 64-bit words, and in wasm an indirect call
 * must have the callee's exact type, so each of win32u's system calls gets
 * a wrapper that converts the words to its parameters, and returns its
 * result as the whole of rax (handles, pointers). __wine_unix_lib_init
 * registers this table as win32u's service table (syscall.c), and
 * signal_wasm64.c's dispatcher calls its entries.
 *
 * Copyright 2026 the gex authors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#if 0
#pragma makedep unix
#endif

#ifdef __wasm64__

/* the headers are C: handles as plain pointers (the same i64 in wasm),
 * and the parameter and variable names C++ reserves renamed */
#define NO_STRICT
#define class class_
#define virtual virtual_
extern "C" {
#include "config.h"
#include <stdarg.h>
#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winnt.h"
#include "winbase.h"
#include "winternl.h"
#include "ntgdi.h"
#include "ntuser.h"
#include "ddk/d3dkmthk.h"
#include "win32syscalls.h"
}
#undef class
#undef virtual

namespace {

template<unsigned...> struct seq {};
template<unsigned N, unsigned... I> struct make_seq : make_seq<N - 1, N - 1, I...> {};
template<unsigned... I> struct make_seq<0, I...> { typedef seq<I...> type; };

template<class T> inline T word( ULONG64 v ) { return (T)v; }

/* win32u's calls return handles, pointers and ULONG_PTRs too: the whole
 * register */
template<class R> struct ret
{
    template<class F, class... A> static ULONG64 call( F f, A... a ) { return (ULONG64)(LONG_PTR)f( a... ); }
};
template<> struct ret<void>
{
    template<class F, class... A> static ULONG64 call( F f, A... a ) { f( a... ); return 0; }
};

template<class R, class... P, unsigned... I>
inline ULONG64 invoke( R (*f)(P...), const ULONG64 *a, seq<I...> )
{
    return ret<R>::call( f, word<P>( a[I] )... );
}

template<class R, class... P>
inline ULONG64 invoke( R (*f)(P...), const ULONG64 *a )
{
    return invoke( f, a, typename make_seq<sizeof...(P)>::type() );
}

}

extern "C" void __wine_wasm64_win32u_stub( const char *name );

/* the system calls Wine does not implement (syscall.c's stubs) */
#define SYSCALL_STUB(name) \
    static ULONG_PTR name##_stub( void ) { __wine_wasm64_win32u_stub( #name ); return 0; }
ALL_SYSCALL_STUBS
#undef SYSCALL_STUB

namespace stubs {
#define SYSCALL_STUB(name) static ULONG_PTR (* const name)( void ) = name##_stub;
ALL_SYSCALL_STUBS
#undef SYSCALL_STUB
}
using namespace stubs;

typedef ULONG64 (*wasm64_syscall)( const ULONG64 *args );

extern "C" const wasm64_syscall __wine_wasm64_win32u_syscalls[] =
{
#define SYSCALL_ENTRY(id,name,args) [](const ULONG64 *a) -> ULONG64 { return invoke( name, a ); },
    ALL_SYSCALLS
#undef SYSCALL_ENTRY
};

#endif  /* __wasm64__ */
