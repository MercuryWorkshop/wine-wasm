/*
 * winebuild for Wine as native wasm64 under gex (W64)
 *
 * A DLL's code is LLP64 wasm64 lowered to wasm32 (gex's wcc64); gex binds
 * its entry adapters at the export addresses of a PE32+ skeleton, as for
 * wasm32 (wasm32.c, docs/wine-native-x64.md §1.2 in the gex tree). The
 * caller is x86-64 code with Win64's one calling convention, so an
 * adapter needs no .spec convention: argument k is slot k (RCX, RDX, R8,
 * R9 or XMM0-3, then the stack), a struct that is not 1, 2, 4 or 8 bytes
 * comes by reference, a small one by value in its slot, and the result
 * goes in RAX or XMM0.
 *
 * The types come from the ICALL pass's gexw.sig64 section in the objects
 * (pointer or integer, a byval struct's size, a struct return's size),
 * else from the wasm signature.
 *
 * Copyright 2026 the gex authors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "config.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "build.h"

/* One parameter or result: its kind (v i I p f d X b s) and, for b and s,
 * the struct's size. */
struct kind64 { char k; unsigned int size; };

struct sig64
{
    struct kind64 ret;
    struct kind64 params[MAX_ARGUMENTS + 1];
    unsigned int nparams;
    int varargs;
};

static int parse_kind( const char **p, struct kind64 *out )
{
    char k = **p;
    if (!k || !strchr( "viIpfdXbs", k )) return 0;
    (*p)++;
    out->k = k;
    out->size = 0;
    if (k == 'b' || k == 's') out->size = strtoul( *p, (char **)p, 10 );
    return 1;
}

/* gexw.sig64's string, or a wasm signature ("i:ii") taken as one */
static int parse_sig64( const char *str, struct sig64 *s )
{
    const char *p = str;
    memset( s, 0, sizeof(*s) );
    if (!parse_kind( &p, &s->ret ) || *p++ != ':') return 0;
    while (*p && *p != '.')
    {
        if (s->nparams > MAX_ARGUMENTS || !parse_kind( &p, &s->params[s->nparams] )) return 0;
        if (s->params[s->nparams].k == 's' || s->params[s->nparams].k == 'v') return 0;
        s->nparams++;
    }
    s->varargs = *p == '.';
    return 1;
}

static int small_struct( unsigned int n ) { return n == 1 || n == 2 || n == 4 || n == 8; }

static const char *c_type64( const struct kind64 *k )
{
    switch (k->k)
    {
    case 'i': return "int";
    case 'I': return "long long";
    case 'p': case 'b': return "void *";
    case 'f': return "float";
    case 'd': return "double";
    case 'X': return "__int128";
    case 'v': case 's': return "void";
    }
    return "long long";
}

void output_wasm64_prelude(void)
{
    output( "unsigned long long __gexw_arg(int, unsigned int);\n" );
    output( "double __gexw_argf64(int, unsigned int);\n" );
    output( "float __gexw_argf32(int, unsigned int);\n" );
    output( "unsigned long long __gexw_argptr(int, unsigned int);\n" );
    output( "unsigned long long __gexw_va_start(int, unsigned int);\n" );
    output( "unsigned int __gexw_va_printf_size(unsigned long long, const void *, int);\n" );
    output( "void __gexw_va_printf_copy(unsigned long long, const void *, int, void *);\n" );
    output( "int __gexw_ret(int, unsigned long long, unsigned long long);\n" );
    output( "int __gexw_ret_void(int, unsigned long long);\n" );
    output( "int __gexw_ret_f64(int, unsigned long long, double);\n" );
    output( "int __gexw_ret_f32(int, unsigned long long, float);\n" );
}

/* The C expression for parameter `p` from argument slot `k`. */
static void output_arg64( const struct kind64 *p, unsigned int k )
{
    switch (p->k)
    {
    case 'i': output( "(int)__gexw_arg(ctx, %u)", k ); break;
    case 'I': output( "(long long)__gexw_arg(ctx, %u)", k ); break;
    case 'p': output( "(void *)__gexw_arg(ctx, %u)", k ); break;
    case 'f': output( "__gexw_argf32(ctx, %u)", k ); break;
    case 'd': output( "__gexw_argf64(ctx, %u)", k ); break;
    case 'X': output( "*(__int128 *)__gexw_arg(ctx, %u)", k ); break;  /* by reference */
    case 'b':
        /* a small struct by value is in its slot; any other by reference */
        if (small_struct( p->size )) output( "(void *)__gexw_argptr(ctx, %u)", k );
        else output( "(void *)__gexw_arg(ctx, %u)", k );
        break;
    }
}

/* A printf's format argument: (index, wide), or -1. */
static int printf_format( const ORDDEF *odp, const char *link, int *wide )
{
    const char *name = odp->name ? odp->name : link;
    int j;
    if (!strstr( name, "printf" ) && !strstr( name, "PRINTF" )) return -1;
    for (j = 0; j < odp->u.func.nb_args; j++)
        if (odp->u.func.args[j] == ARG_STR || odp->u.func.args[j] == ARG_WSTR)
        {
            *wide = odp->u.func.args[j] == ARG_WSTR;
            return j;
        }
    return -1;
}

/* One entry adapter (varargs or not), or NULL when the code's signature
 * has no Win64 shape here. `wasm_sig` is the function's wasm signature,
 * used when the objects have no gexw.sig64 line for it. */
const char *output_adapter64( const ORDDEF *odp, const char *wasm_sig )
{
    const char *link = get_link_name( odp ), *ident, *str = wasm_find_sig64( link );
    struct sig64 s;
    static unsigned int count;
    unsigned int j, k = 0, nfixed, n = count++;
    int sret_reg = 0, sret_mem = 0, fmt = -1, wide = 0, va_list_param = -1;

    if (!parse_sig64( str ? str : wasm_sig, &s )) return NULL;
    if (s.ret.k == 'X') return NULL;
    sret_reg = s.ret.k == 's' && small_struct( s.ret.size );
    sret_mem = s.ret.k == 's' && !sret_reg;
    nfixed = s.nparams;
    if (!str && odp->type == TYPE_VARARGS)
    {
        /* a variadic function's wasm signature ends with its buffer */
        if (!nfixed) return NULL;
        s.varargs = 1;
        nfixed--;
    }
    if (odp->type == TYPE_VARARGS && !s.varargs) return NULL;
    if (s.varargs || odp->type == TYPE_VARARGS) fmt = printf_format( odp, link, &wide );
    /* a printf taking a va_list (vsnprintf): the guest's is a pointer to
     * 8-byte slots, laid out again from the format */
    else if ((fmt = printf_format( odp, link, &wide )) >= 0 && nfixed == odp->u.func.nb_args &&
             odp->u.func.args[nfixed - 1] == ARG_PTR && fmt < (int)nfixed - 1)
        va_list_param = nfixed - 1;
    else fmt = -1;

    /* the function, under a name of its own (another adapter may have
     * declared the link name with other types) */
    output( "%s __gexw_f64_%u(", sret_reg || sret_mem ? "void" : c_type64( &s.ret ), n );
    if (sret_reg || sret_mem) output( "void *%s", nfixed || s.varargs ? ", " : "" );
    for (j = 0; j < nfixed; j++) output( "%s%s", j ? ", " : "", c_type64( &s.params[j] ));
    if (s.varargs) output( "%svoid *", nfixed ? ", " : "" );
    if (!nfixed && !s.varargs && !sret_reg && !sret_mem) output( "void" );
    output( ") __asm__(\"%s\");\n", link );
    ident = output_adapter_header( odp );

    if (sret_reg) output( "    unsigned long long sr = 0;\n" );
    if (sret_mem) output( "    void *sr = (void *)__gexw_arg(ctx, %u);\n", k++ );
    if (s.varargs)
    {
        output( "    unsigned long long va = __gexw_va_start(ctx, %u);\n", k + nfixed );
        output( "    void *buf = (void *)va;\n" );
    }
    else if (va_list_param >= 0)
    {
        output( "    unsigned long long va = __gexw_arg(ctx, %u);\n", k + va_list_param );
        output( "    void *buf;\n" );
    }
    if (fmt >= 0)
    {
        output( "    const void *fmt = (const void *)__gexw_arg(ctx, %u);\n", k + fmt );
        output( "    buf = __builtin_alloca(__gexw_va_printf_size(va, fmt, %d));\n", wide );
        output( "    __gexw_va_printf_copy(va, fmt, %d, buf);\n", wide );
    }
    output( "    " );
    if (!sret_reg && !sret_mem && s.ret.k != 'v') output( "%s r = ", c_type64( &s.ret ));
    output( "__gexw_f64_%u(", n );
    if (sret_reg) output( "&sr%s", nfixed || s.varargs ? ", " : "" );
    if (sret_mem) output( "sr%s", nfixed || s.varargs ? ", " : "" );
    for (j = 0; j < nfixed; j++)
    {
        if (j) output( ", " );
        if ((int)j == va_list_param) output( "buf" );
        else output_arg64( &s.params[j], k + j );
    }
    if (s.varargs) output( "%sbuf", nfixed ? ", " : "" );
    output( ");\n" );
    if (sret_reg) output( "    return __gexw_ret(ctx, ret, sr);\n" );
    else if (sret_mem) output( "    return __gexw_ret(ctx, ret, (unsigned long long)sr);\n" );
    else switch (s.ret.k)
    {
    case 'v': output( "    return __gexw_ret_void(ctx, ret);\n" ); break;
    case 'i': output( "    return __gexw_ret(ctx, ret, (unsigned int)r);\n" ); break;
    case 'I': output( "    return __gexw_ret(ctx, ret, (unsigned long long)r);\n" ); break;
    case 'p': output( "    return __gexw_ret(ctx, ret, (unsigned long long)r);\n" ); break;
    case 'f': output( "    return __gexw_ret_f32(ctx, ret, r);\n" ); break;
    case 'd': output( "    return __gexw_ret_f64(ctx, ret, r);\n" ); break;
    }
    output( "}\n\n" );
    return ident;
}

/* The entry point: a program's mainCRTStartup(void), a DLL's
 * DllMain(hinst, reason, reserved). */
void output_entry64( DLLSPEC *spec, int has_init )
{
    if (spec->init_func && !(spec->characteristics & IMAGE_FILE_DLL))
    {
        output( "int %s(void);\n", spec->init_func );
        output( "static int gexp__entry_body(int ctx, int depth, unsigned int need, unsigned long long ret)\n{\n" );
        output( "    return __gexw_ret(ctx, ret, (unsigned int)%s());\n}\n\n", spec->init_func );
    }
    else if (has_init)
    {
        output( "int %s(void *, unsigned int, void *);\n", spec->init_func );
        output( "static int gexp__entry_body(int ctx, int depth, unsigned int need, unsigned long long ret)\n{\n" );
        output( "    int r = %s((void *)__gexw_arg(ctx, 0), (unsigned int)__gexw_arg(ctx, 1), (void *)__gexw_arg(ctx, 2));\n",
                spec->init_func );
        output( "    return __gexw_ret(ctx, ret, (unsigned int)r);\n}\n\n" );
    }
    else
    {
        output( "int gexp__entry(int ctx, int depth, unsigned int need, unsigned long long ret)\n{\n" );
        output( "    return __gexw_ret(ctx, ret, 1);\n}\n\n" );
        return;
    }
    output( "int gexp__entry(int ctx, int depth, unsigned int need, unsigned long long ret)\n{\n" );
    output( "    return __gexw_entry(ctx, depth, need, ret, gexp__entry_body);\n}\n\n" );
}
