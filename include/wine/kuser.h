/*
 * Where KUSER_SHARED_DATA is mapped in a process
 *
 * One definition for ntdll (both sides), kernel32, kernelbase, win32u and
 * ntoskrnl. Windows has it at 0x7ffe0000; gex's W3 (wasm32) has it in the
 * low space Wine reserves below gex's stack, so a WebAssembly memory need
 * not reach 2 GiB (the address is gex's GEX_WINE32_KUSER, which ntdll's unix
 * side checks; i386 code that reads the Windows address by constant is
 * folded to this one by gex's translator).
 */

#ifndef __WINE_WINE_KUSER_H
#define __WINE_WINE_KUSER_H

#ifdef __wasm32__
#define WINE_USER_SHARED_DATA 0x04ff0000
#else
#define WINE_USER_SHARED_DATA 0x7ffe0000
#endif

#endif /* __WINE_WINE_KUSER_H */
