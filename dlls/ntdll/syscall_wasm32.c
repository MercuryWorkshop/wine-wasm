/*
 * ntdll's system call stubs on wasm32 (native Wine under gex)
 *
 * Copyright 2026 the gex authors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#ifdef __wasm32__

/* No Windows headers: the stubs take the system calls' names by asm label,
 * with 32-bit word parameters (wine/gex/syscall.h). */
#include "wine/gex/syscall.h"
#include "ntsyscalls.h"

/* Written in signal_wasm32.c: the calls that load a new guest state, the
 * ones with 64-bit parameters, and NtQueryPerformanceCounter (no system
 * call). Their generic stubs get other names. */
#define NtContinue __gexw_generic_NtContinue
#define NtContinueEx __gexw_generic_NtContinueEx
#define NtSetContextThread __gexw_generic_NtSetContextThread
#define NtQueryPerformanceCounter __gexw_generic_NtQueryPerformanceCounter
#define NtWow64AllocateVirtualMemory64 __gexw_generic_NtWow64AllocateVirtualMemory64
#define NtWow64ReadVirtualMemory64 __gexw_generic_NtWow64ReadVirtualMemory64
#define NtWow64WriteVirtualMemory64 __gexw_generic_NtWow64WriteVirtualMemory64

#define SYSCALL_ENTRY(id,name,args) GEXW_SYSCALL_ENTRY(id, name, args)
#undef SYSCALL_ENTRY_NtQueryInformationProcess
#define SYSCALL_ENTRY_NtQueryInformationProcess(id,name,args) GEXW_SYSCALL_ENTRY(id, name, args)
ALL_SYSCALLS

#endif  /* __wasm32__ */
