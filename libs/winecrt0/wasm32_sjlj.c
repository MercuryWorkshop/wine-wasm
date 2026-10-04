/*
 * Native Wine modules on wasm32 under gex: the runtime of clang's SjLj
 * lowering (setjmp/longjmp onto wasm exception handling). An object of its
 * own, so only a module that uses setjmp links it: a module with an
 * exception tag needs an engine with exception handling. See docs/wine-native.md in the gex tree.
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

__asm__(
    /* the tag the lowering throws and catches (wasi-libc's) */
    "\t.tagtype __c_longjmp i32\n"
    "\t.globl __c_longjmp\n"
    "__c_longjmp:\n"
);

/* ---- setjmp/longjmp: the runtime of clang's wasm SjLj lowering ---- */

/* setjmp and longjmp calls are rewritten by -mllvm -wasm-enable-sjlj into
 * these, with longjmp a throw of the __c_longjmp tag (wasm exception
 * handling). The layout is wasi-libc's. */
struct __gexw_jmp_buf
{
    void *func_invocation_id;
    uint32_t label;
    struct { void *env; int val; } arg;
};

void __wasm_setjmp( void *env, uint32_t label, void *func_invocation_id )
{
    struct __gexw_jmp_buf *jb = env;
    jb->func_invocation_id = func_invocation_id;
    jb->label = label;
}

uint32_t __wasm_setjmp_test( void *env, void *func_invocation_id )
{
    struct __gexw_jmp_buf *jb = env;
    return jb->func_invocation_id == func_invocation_id ? jb->label : 0;
}

void __wasm_longjmp( void *env, int val )
{
    struct __gexw_jmp_buf *jb = env;
    jb->arg.env = env;
    jb->arg.val = val ? val : 1;
    __builtin_wasm_throw( 1, &jb->arg );
}

#endif /* __wasm32__ */
