/*
 * Typed system-call wrappers for 64-bit native Wine under gex (W64, gex
 * docs/wine-native-x64.md).
 *
 * The program's x86-64 code passes a system call's arguments as 64-bit
 * words. Wine's Unix side is wasm, where an indirect call must have the
 * callee's exact type: NtClose is (i64) -> i32, NtWaitForSingleObject
 * (i64, i32, i64) -> i32. So each system call gets a wrapper that takes
 * the words and converts each to its parameter's type (a BOOLEAN keeps its
 * low byte, as x86-64's zero-extension wrappers in syscall.c do).
 * signal_wasm64.c's dispatcher calls __wine_wasm64_syscalls[id].
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

extern "C" {
#include "config.h"
#include <stdarg.h>
#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winnt.h"
#include "winbase.h"
#include "winternl.h"
#include "ddk/wdm.h"
#include "ntsyscalls.h"
}

namespace {

template<unsigned...> struct seq {};
template<unsigned N, unsigned... I> struct make_seq : make_seq<N - 1, N - 1, I...> {};
template<unsigned... I> struct make_seq<0, I...> { typedef seq<I...> type; };

template<class T> inline T word( ULONG64 v ) { return (T)v; }

template<class R> struct ret
{
    template<class F, class... A> static NTSTATUS call( F f, A... a ) { return (NTSTATUS)f( a... ); }
};
template<> struct ret<void>
{
    template<class F, class... A> static NTSTATUS call( F f, A... a ) { f( a... ); return STATUS_SUCCESS; }
};

template<class R, class... P, unsigned... I>
inline NTSTATUS invoke( R (*f)(P...), const ULONG64 *a, seq<I...> )
{
    return ret<R>::call( f, word<P>( a[I] )... );
}

template<class R, class... P>
inline NTSTATUS invoke( R (*f)(P...), const ULONG64 *a )
{
    return invoke( f, a, typename make_seq<sizeof...(P)>::type() );
}

}

extern "C" void __wine_wasm64_stub_syscall( const char *name );

/* the system calls Wine does not implement (syscall.c's stubs) */
#define SYSCALL_STUB(name) \
    static NTSTATUS name##_stub( void ) { __wine_wasm64_stub_syscall( #name ); return STATUS_NOT_IMPLEMENTED; }
ALL_SYSCALL_STUBS
#undef SYSCALL_STUB

namespace stubs {
#define SYSCALL_STUB(name) static NTSTATUS (* const name)( void ) = name##_stub;
ALL_SYSCALL_STUBS
#undef SYSCALL_STUB
}
using namespace stubs;

typedef NTSTATUS (*wasm64_syscall)( const ULONG64 *args );

extern "C" const wasm64_syscall __wine_wasm64_syscalls[] =
{
#define SYSCALL_ENTRY(id,name,args) [](const ULONG64 *a) -> NTSTATUS { return invoke( name, a ); },
    ALL_SYSCALLS
#undef SYSCALL_ENTRY
};

extern "C" const unsigned int __wine_wasm64_syscall_count = sizeof(__wine_wasm64_syscalls) / sizeof(__wine_wasm64_syscalls[0]);

#endif  /* __wasm64__ */
