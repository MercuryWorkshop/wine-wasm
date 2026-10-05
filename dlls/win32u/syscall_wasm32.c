/*
 * win32u's system call stubs on wasm32 (native Wine under gex)
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

/* As ntdll's (syscall_wasm32.c there): no Windows headers, the stubs take
 * the system calls' names by asm label with 32-bit word parameters. */
#include "wine/gex/syscall.h"
#include "win32syscalls.h"

#define SYSCALL_ENTRY(id,name,args) GEXW_SYSCALL_ENTRY(id, name, args)
ALL_SYSCALLS

#endif  /* __wasm32__ */
