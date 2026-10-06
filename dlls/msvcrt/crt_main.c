/*
 * mainCRTStartup default entry point
 *
 * Copyright 2019 Jacek Caban for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * In addition to the permissions in the GNU Lesser General Public License,
 * the authors give you unlimited permission to link the compiled version
 * of this file with other programs, and to distribute those programs
 * without any restriction coming from the use of this file.  (The GNU
 * Lesser General Public License restrictions do apply in other respects;
 * for example, they cover modification of the file, and distribution when
 * not linked into another program.)
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

#if 0
#pragma makedep implib
#endif

#include <stdarg.h>
#include <stdlib.h>
#include <process.h>

#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#include "crt_init.h"

#ifdef __wasm32__
/* wasm: clang names a two-parameter main __main_argc_argv and a
 * parameterless one __original_main; only a three-parameter one is main
 * (a call of the wrong type would trap) */
int __cdecl main3(int argc, char **argv, char **env) __asm__("main") __attribute__((weak));
int __cdecl __main_argc_argv(int argc, char **argv) __attribute__((weak));
int __cdecl __original_main(void) __attribute__((weak));
#else
int __cdecl main(int argc, char **argv, char **env);
#endif

static const IMAGE_NT_HEADERS *get_nt_header( void )
{
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)NtCurrentTeb()->Peb->ImageBaseAddress;
    return (const IMAGE_NT_HEADERS *)((char *)dos + dos->e_lfanew);
}

int __cdecl mainCRTStartup(void)
{
    int argc, ret;
    char **argv, **env;

#ifdef _UCRT
    _configure_narrow_argv(_crt_argv_unexpanded_arguments);
    _initialize_narrow_environment();
    argc = *__p___argc();
    argv = *__p___argv();
    env = _get_initial_narrow_environment();
#else
    int new_mode =  0;
    __getmainargs(&argc, &argv, &env, 0, &new_mode);
#endif
    _set_app_type(get_nt_header()->OptionalHeader.Subsystem == IMAGE_SUBSYSTEM_WINDOWS_GUI ? _crt_gui_app : _crt_console_app);
    do_global_ctors();

#ifdef __wasm32__
    if (__main_argc_argv) ret = __main_argc_argv(argc, argv);
    else if (__original_main) ret = __original_main();
    else ret = main3(argc, argv, env);
#else
    ret = main(argc, argv, env);
#endif

    exit(ret);
    return ret;
}
