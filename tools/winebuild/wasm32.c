/*
 * winebuild for Wine as native wasm32 under gex
 *
 * A DLL's code is a position-independent wasm module; gex binds it at the
 * export addresses of a PE skeleton (docs/wine-native.md §3.2, §3.3 in the
 * gex tree). This file makes the module's spec glue: one entry adapter per
 * export, which reads the i386 caller's arguments from the guest stack,
 * calls the C function and returns the way the .spec says the export
 * returns (stdcall pops its arguments; EAX, EDX:EAX or ST0). The argument
 * and return types are the function's wasm signature, read from the
 * compiled objects, since a .spec has no return types.
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

const char *wasm_module_file = NULL;

/* ---- reading wasm objects, archives and modules ------------------------- */

struct wasm_buf { const unsigned char *p, *end; };

static unsigned int leb( struct wasm_buf *b )
{
    unsigned int r = 0, s = 0;
    unsigned char x;
    do
    {
        if (b->p >= b->end) fatal_error( "truncated wasm file\n" );
        x = *b->p++;
        if (s < 32) r |= (unsigned int)(x & 0x7f) << s;
        s += 7;
    } while (x & 0x80);
    return r;
}

static int sleb( struct wasm_buf *b )
{
    int r = 0, s = 0;
    unsigned char x;
    do
    {
        x = *b->p++;
        r |= (int)(x & 0x7f) << s;
        s += 7;
    } while (x & 0x80);
    if (s < 32 && (x & 0x40)) r |= -1 << s;
    return r;
}

static char *wasm_name( struct wasm_buf *b )
{
    unsigned int n = leb( b );
    char *s = xmalloc( n + 1 );
    memcpy( s, b->p, n );
    s[n] = 0;
    b->p += n;
    return s;
}

/* A function signature as a string: results, ':', params; i = i32,
 * I = i64, f = f32, d = f64 (so "i:ii" is int f(int, int)). */
struct wasm_func { char *name; char *sig; };

static struct
{
    struct wasm_func *funcs;
    unsigned int count, size;
} wasm_funcs;

static char valtype( unsigned char t )
{
    switch (t)
    {
    case 0x7f: return 'i';
    case 0x7e: return 'I';
    case 0x7d: return 'f';
    case 0x7c: return 'd';
    }
    return '?';
}

static void add_func( const char *name, const char *sig )
{
    if (wasm_funcs.count == wasm_funcs.size)
    {
        wasm_funcs.size = wasm_funcs.size ? 2 * wasm_funcs.size : 256;
        wasm_funcs.funcs = xrealloc( wasm_funcs.funcs, wasm_funcs.size * sizeof(*wasm_funcs.funcs) );
    }
    wasm_funcs.funcs[wasm_funcs.count].name = xstrdup( name );
    wasm_funcs.funcs[wasm_funcs.count].sig = xstrdup( sig );
    wasm_funcs.count++;
}

static const char *find_func_sig( const char *name )
{
    unsigned int i;
    for (i = 0; i < wasm_funcs.count; i++)
        if (!strcmp( wasm_funcs.funcs[i].name, name )) return wasm_funcs.funcs[i].sig;
    return NULL;
}

/* The defined function symbols of one wasm object file, with signatures. */
static void read_wasm_object( const unsigned char *data, size_t size )
{
    struct wasm_buf b = { data + 8, data + size };
    char **types = NULL;
    unsigned int *func_type = NULL, ntypes = 0, nimports = 0, nfuncs = 0, i;

    if (size < 8 || memcmp( data, "\0asm", 4 )) return;
    while (b.p < b.end)
    {
        unsigned char id = *b.p++;
        unsigned int len = leb( &b );
        struct wasm_buf s = { b.p, b.p + len };
        b.p += len;

        if (id == 1)  /* types */
        {
            ntypes = leb( &s );
            types = xmalloc( ntypes * sizeof(*types) );
            for (i = 0; i < ntypes; i++)
            {
                unsigned int np, nr, k;
                char buf[256], *q = buf;
                if (*s.p++ != 0x60) fatal_error( "unexpected wasm type form\n" );
                np = leb( &s );
                const unsigned char *params = s.p;
                s.p += np;
                nr = leb( &s );
                for (k = 0; k < nr && k < 1; k++) *q++ = valtype( s.p[k] );
                s.p += nr;
                if (!nr) *q++ = 'v';
                *q++ = ':';
                for (k = 0; k < np && q < buf + sizeof(buf) - 1; k++) *q++ = valtype( params[k] );
                *q = 0;
                types[i] = xstrdup( buf );
            }
        }
        else if (id == 2)  /* imports */
        {
            unsigned int n = leb( &s );
            for (i = 0; i < n; i++)
            {
                unsigned char kind;
                free( wasm_name( &s ));
                free( wasm_name( &s ));
                kind = *s.p++;
                switch (kind)
                {
                case 0:
                    func_type = xrealloc( func_type, (nimports + 1) * sizeof(*func_type) );
                    func_type[nimports++] = leb( &s );
                    break;
                case 1: { unsigned int fl; s.p++; fl = leb( &s ); leb( &s ); if (fl & 1) leb( &s ); break; }
                case 2: { unsigned int fl = leb( &s ); leb( &s ); if (fl & 1) leb( &s ); break; }
                case 3: s.p += 2; break;
                case 4: s.p++; leb( &s ); break;
                }
            }
        }
        else if (id == 3)  /* functions */
        {
            nfuncs = leb( &s );
            func_type = xrealloc( func_type, (nimports + nfuncs) * sizeof(*func_type) );
            for (i = 0; i < nfuncs; i++) func_type[nimports + i] = leb( &s );
        }
        else if (id == 0)  /* custom: the linking section's symbol table */
        {
            char *name = wasm_name( &s );
            if (!strcmp( name, "linking" ))
            {
                leb( &s );  /* version */
                while (s.p < s.end)
                {
                    unsigned char sub = *s.p++;
                    unsigned int sublen = leb( &s );
                    struct wasm_buf t = { s.p, s.p + sublen };
                    s.p += sublen;
                    if (sub != 8) continue;  /* WASM_SYMBOL_TABLE */
                    unsigned int n = leb( &t );
                    for (i = 0; i < n; i++)
                    {
                        unsigned char kind = *t.p++;
                        unsigned int flags = leb( &t );
                        int undefined = flags & 0x10, local = flags & 0x02, explicit_name = flags & 0x40;
                        switch (kind)
                        {
                        case 0:  /* function */
                        {
                            unsigned int idx = leb( &t );
                            char *sym = (!undefined || explicit_name) ? wasm_name( &t ) : NULL;
                            if (sym && !undefined && !local && idx < nimports + nfuncs && func_type[idx] < ntypes)
                                add_func( sym, types[func_type[idx]] );
                            free( sym );
                            break;
                        }
                        case 1:  /* data */
                            free( wasm_name( &t ));
                            if (!undefined) { leb( &t ); leb( &t ); leb( &t ); }
                            break;
                        case 2: case 4: case 5:  /* global, event, table */
                            leb( &t );
                            if (!undefined || explicit_name) free( wasm_name( &t ));
                            break;
                        case 3:  /* section */
                            leb( &t );
                            break;
                        }
                    }
                }
            }
            free( name );
        }
    }
    for (i = 0; i < ntypes; i++) free( types[i] );
    free( types );
    free( func_type );
}

static unsigned char *read_whole_file( const char *name, size_t *size )
{
    FILE *f = fopen( name, "rb" );
    unsigned char *data;
    if (!f) fatal_error( "cannot open %s\n", name );
    fseek( f, 0, SEEK_END );
    *size = ftell( f );
    fseek( f, 0, SEEK_SET );
    data = xmalloc( *size ? *size : 1 );
    if (*size && fread( data, *size, 1, f ) != 1) fatal_error( "cannot read %s\n", name );
    fclose( f );
    return data;
}

/* An object file or an ar archive of them. */
static void read_wasm_input( const char *name )
{
    size_t size;
    unsigned char *data = read_whole_file( name, &size );

    if (size > 8 && !memcmp( data, "!<arch>\n", 8 ))
    {
        size_t pos = 8;
        while (pos + 60 <= size)
        {
            char hdr[61];
            size_t len;
            memcpy( hdr, data + pos, 60 );
            hdr[60] = 0;
            len = strtoul( hdr + 48, NULL, 10 );
            if (pos + 60 + len > size) break;
            read_wasm_object( data + pos + 60, len );
            pos += 60 + len + (len & 1);
        }
    }
    else read_wasm_object( data, size );
    free( data );
}

/* The memory size and alignment (log2) of a linked side module, and the
 * memory-base-relative address of each exported data symbol. */
struct wasm_module_info
{
    unsigned int mem_size, mem_align;
    unsigned int nexports;
    char **export_names;
    unsigned int *export_offsets;
    unsigned char *bytes;
    size_t size;
};

static void read_wasm_module( const char *name, struct wasm_module_info *info )
{
    struct wasm_buf b;
    unsigned int nglobals_imported = 0, nglobals = 0, i;
    int *global_init = NULL;

    memset( info, 0, sizeof(*info) );
    info->bytes = read_whole_file( name, &info->size );
    if (info->size < 8 || memcmp( info->bytes, "\0asm", 4 )) fatal_error( "%s is not a wasm module\n", name );
    b.p = info->bytes + 8;
    b.end = info->bytes + info->size;
    while (b.p < b.end)
    {
        unsigned char id = *b.p++;
        unsigned int len = leb( &b );
        struct wasm_buf s = { b.p, b.p + len };
        b.p += len;

        if (id == 0)
        {
            char *sec = wasm_name( &s );
            if (!strcmp( sec, "dylink.0" ))
            {
                while (s.p < s.end)
                {
                    unsigned char kind = *s.p++;
                    unsigned int sublen = leb( &s );
                    const unsigned char *next = s.p + sublen;
                    if (kind == 1)
                    {
                        info->mem_size = leb( &s );
                        info->mem_align = leb( &s );
                    }
                    s.p = next;
                }
            }
            free( sec );
        }
        else if (id == 2)
        {
            unsigned int n = leb( &s );
            for (i = 0; i < n; i++)
            {
                unsigned char kind;
                free( wasm_name( &s ));
                free( wasm_name( &s ));
                kind = *s.p++;
                switch (kind)
                {
                case 0: leb( &s ); break;
                case 1: { unsigned int fl; s.p++; fl = leb( &s ); leb( &s ); if (fl & 1) leb( &s ); break; }
                case 2: { unsigned int fl = leb( &s ); leb( &s ); if (fl & 1) leb( &s ); break; }
                case 3: s.p += 2; nglobals_imported++; break;
                case 4: s.p++; leb( &s ); break;
                }
            }
        }
        else if (id == 6)  /* globals: exported data symbols are i32.const offsets */
        {
            nglobals = leb( &s );
            global_init = xmalloc( (nglobals ? nglobals : 1) * sizeof(*global_init) );
            for (i = 0; i < nglobals; i++)
            {
                s.p += 2;  /* valtype, mutability */
                global_init[i] = -1;
                if (*s.p == 0x41) { s.p++; global_init[i] = sleb( &s ); }
                else if (*s.p == 0x23) { s.p++; leb( &s ); }  /* global.get */
                else if (*s.p == 0x42) { s.p++; sleb( &s ); }
                while (s.p < s.end && *s.p != 0x0b) s.p++;  /* skip to end */
                s.p++;
            }
        }
        else if (id == 7)
        {
            unsigned int n = leb( &s );
            info->export_names = xmalloc( (n ? n : 1) * sizeof(char *) );
            info->export_offsets = xmalloc( (n ? n : 1) * sizeof(unsigned int) );
            for (i = 0; i < n; i++)
            {
                char *ename = wasm_name( &s );
                unsigned char kind = *s.p++;
                unsigned int idx = leb( &s );
                if (kind == 3 && idx >= nglobals_imported && idx - nglobals_imported < nglobals &&
                    global_init[idx - nglobals_imported] >= 0)
                {
                    info->export_names[info->nexports] = ename;
                    info->export_offsets[info->nexports++] = global_init[idx - nglobals_imported];
                }
                else free( ename );
            }
        }
    }
    free( global_init );
}

int wasm_module_data_export( const struct wasm_module_info *info, const char *name, unsigned int *offset )
{
    unsigned int i;
    for (i = 0; i < info->nexports; i++)
        if (!strcmp( info->export_names[i], name )) { *offset = info->export_offsets[i]; return 1; }
    return 0;
}

/* ---- the spec glue: entry adapters ------------------------------------- */

static const char *c_type( char t )
{
    switch (t)
    {
    case 'i': return "int";
    case 'I': return "long long";
    case 'f': return "float";
    case 'd': return "double";
    case 'v': return "void";
    }
    return "int";
}

/* The wasm signature the .spec implies, when the code has none to read
 * (an export the objects do not define): ints and pointers are i32, int64
 * and double take their own types, and the result is taken to be an int. */
static char *spec_sig( const ORDDEF *odp )
{
    char *sig = xmalloc( 4 + odp->u.func.nb_args ), *q = sig;
    int i;
    *q++ = (odp->flags & FLAG_RET64) ? 'I' : 'i';
    *q++ = ':';
    for (i = 0; i < odp->u.func.nb_args; i++)
    {
        switch (odp->u.func.args[i])
        {
        case ARG_INT64:  *q++ = 'I'; break;
        case ARG_DOUBLE: *q++ = 'd'; break;
        case ARG_FLOAT:  *q++ = 'f'; break;
        default:         *q++ = 'i'; break;
        }
    }
    *q = 0;
    return sig;
}

static unsigned int spec_words( const ORDDEF *odp )
{
    unsigned int i, words = 0;
    for (i = 0; i < odp->u.func.nb_args; i++)
    {
        switch (odp->u.func.args[i])
        {
        case ARG_INT64:
        case ARG_DOUBLE: words += 2; break;
        case ARG_INT128: words += 4; break;
        default:         words += 1; break;
        }
    }
    return words;
}

static const char *adapter_name( const ORDDEF *odp )
{
    return strmake( "gexp_%s", odp->name ? odp->name : odp->export_name );
}

/* One entry adapter. */
static unsigned int spec_arg_size( enum arg_type t )
{
    switch (t)
    {
    case ARG_INT64:
    case ARG_DOUBLE: return 8;
    case ARG_INT128: return 16;
    default:         return 4;
    }
}

/* One entry adapter. The .spec's arguments are what the i386 caller
 * pushed; the wasm parameters are what the C function takes. They are
 * matched one by one:
 *   - a 4-byte argument is an i32 or f32 parameter;
 *   - an 8-byte one (int64, double) is an i64 or f64 parameter, or an i32:
 *     a struct passed by value (LARGE_INTEGER), which wasm32 passes by
 *     pointer, so the adapter passes the address of the guest's copy;
 *   - a 16-byte one is always such a pointer.
 * A small struct returned by value is a hidden first pointer parameter on
 * wasm32 (and a void result); i386 returns it in EDX:EAX. */
static void output_adapter( const ORDDEF *odp, const char *sig )
{
    const char *link = get_link_name( odp );
    const char *params = strchr( sig, ':' ) + 1;
    char ret = sig[0];
    unsigned int nparams = strlen( params ), nargs = odp->u.func.nb_args;
    unsigned int words = spec_words( odp ), k, off = 0, regs = 0, pop;
    int sret = 0;
    const char *p;

    if (nparams == nargs + 1 && ret == 'v' && params[0] == 'i')
    {
        sret = 1;
        params++;
        nparams--;
    }
    if (nparams != nargs)
        fatal_error( "%s: the .spec gives %u arguments, the code takes %u\n",
                     odp->name ? odp->name : link, nargs, nparams );
    for (k = 0; k < nargs; k++)
    {
        unsigned int sz = spec_arg_size( odp->u.func.args[k] );
        char t = params[k];
        if ((sz == 4 && t != 'i' && t != 'f') || (sz == 8 && t == 'f') || (sz == 16 && t != 'i'))
            fatal_error( "%s: argument %u is %u bytes in the .spec but '%c' in the code\n",
                         odp->name ? odp->name : link, k + 1, sz, t );
    }

    /* thiscall: `this` in ECX; fastcall: the first two words in ECX, EDX */
    if (odp->flags & FLAG_THISCALL) regs = 1;
    else if (odp->flags & FLAG_FASTCALL) regs = words < 2 ? words : 2;
    pop = (odp->type == TYPE_STDCALL) ? 4 * (words - regs) : 0;

    output( "int %s(int ctx, int depth, unsigned int need, unsigned long long ret)\n{\n", adapter_name( odp ));
    output( "    struct __gexw_frame f;\n" );
    output( "    __gexw_enter(&f, ctx, depth);\n" );
    if (sret) output( "    unsigned long long sr __attribute__((aligned(8))) = 0;\n" );
    output( "    " );
    if (ret != 'v') output( "%s r = ", c_type( ret ));
    output( "((%s (*)(", c_type( ret ));
    if (sret) output( "void *%s", *params ? ", " : "" );
    for (p = params; *p; p++) output( "%s%s", p == params ? "" : ", ", c_type( *p ));
    if (!*params && !sret) output( "void" );
    output( "))%s)(", link );
    if (sret) output( "&sr%s", *params ? ", " : "" );
    for (p = params, k = 0; *p; p++, k++)
    {
        unsigned int sz = spec_arg_size( odp->u.func.args[k] );
        if (k) output( ", " );
        if (k < regs) output( "(int)__gexw_reg(ctx, %u)", k == 0 ? 1 : 2 );  /* ECX, EDX */
        else
        {
            switch (*p)
            {
            case 'i':
                if (sz == 4) output( "(int)__gexw_arg32(ctx, %u)", off );
                else output( "(int)__gexw_argptr(ctx, %u)", off );  /* a struct by value */
                break;
            case 'f': output( "__gexw_argf32(ctx, %u)", off ); break;
            case 'I': output( "(long long)__gexw_arg64(ctx, %u)", off ); break;
            case 'd': output( "__gexw_argf64(ctx, %u)", off ); break;
            }
            off += sz;
        }
    }
    output( ");\n" );
    output( "    __gexw_leave(&f);\n" );
    if (sret) ret = 's';
    switch (ret)
    {
    case 's': output( "    return __gexw_ret(ctx, ret, %u, sr);\n", pop ); break;
    case 'v': output( "    return __gexw_ret(ctx, ret, %u, 0);\n", pop ); break;
    case 'i': output( "    return __gexw_ret(ctx, ret, %u, (unsigned int)r);\n", pop ); break;
    case 'I': output( "    return __gexw_ret(ctx, ret, %u, (unsigned long long)r);\n", pop ); break;
    case 'f':
    case 'd': output( "    return __gexw_ret_fp(ctx, ret, %u, r);\n", pop ); break;
    }
    output( "}\n\n" );
}

/* An unimplemented export: raise what Wine raises. */
static void output_stub_adapter( DLLSPEC *spec, const ORDDEF *odp )
{
    output( "int %s(int ctx, int depth, unsigned int need, unsigned long long ret)\n{\n", adapter_name( odp ));
    output( "    __wine_spec_unimplemented_stub(\"%s\", \"%s\");\n", spec->file_name,
            odp->name ? odp->name : odp->export_name ? odp->export_name : "?" );
    output( "    return 0;\n}\n\n" );
}

/*******************************************************************
 *         output_wasm_spec
 *
 * The C glue of a wasm32 DLL module, compiled to output_file_name: an
 * entry adapter per exported function (gexp_<name>), gexp__entry for the
 * entry point, gexp__missing, and a list of the data exports for the
 * link (output_file_name with .rsp: wasm-ld options exporting them, so
 * the skeleton can point at them).
 */
void output_wasm_spec( DLLSPEC *spec, struct strarray files )
{
    struct exports *exports = &spec->exports;
    struct strarray names = empty_strarray, data_syms = empty_strarray;
    const char *c_file;
    FILE *rsp;
    int i;

    STRARRAY_FOR_EACH( file, &files )
        if (strendswith( file, ".o" ) || strendswith( file, ".a" )) read_wasm_input( file );

    c_file = output_file_name ? open_temp_output_file( ".c" ) : NULL;
    if (!c_file) output_file = stdout;

    output( "/* File generated automatically from %s by winebuild (wasm32); do not edit! */\n\n",
            spec_file_name ? spec_file_name : spec->file_name );
    output( "struct __gexw_frame { int ctx, depth; };\n" );
    output( "void __gexw_enter(struct __gexw_frame *, int, int);\n" );
    output( "void __gexw_leave(struct __gexw_frame *);\n" );
    output( "unsigned int __gexw_arg32(int, unsigned int);\n" );
    output( "unsigned long long __gexw_arg64(int, unsigned int);\n" );
    output( "float __gexw_argf32(int, unsigned int);\n" );
    output( "double __gexw_argf64(int, unsigned int);\n" );
    output( "unsigned int __gexw_reg(int, unsigned int);\n" );
    output( "unsigned int __gexw_argptr(int, unsigned int);\n" );
    output( "int __gexw_ret(int, unsigned long long, unsigned int, unsigned long long);\n" );
    output( "int __gexw_ret_fp(int, unsigned long long, unsigned int, double);\n" );
    output( "void __gexw_missing(int);\n" );
    output( "void __wine_spec_unimplemented_stub(const char *, const char *);\n\n" );

    for (i = 0; i < exports->nb_entry_points; i++)
    {
        ORDDEF *odp = exports->entry_points[i];
        const char *sig;

        if (odp->flags & FLAG_FORWARD) continue;  /* the skeleton forwards it */
        switch (odp->type)
        {
        case TYPE_EXTERN:
            strarray_add( &data_syms, get_link_name( odp ));
            continue;
        case TYPE_STUB:
            output_stub_adapter( spec, odp );
            break;
        case TYPE_VARARGS:
            /* i386 varargs (4-byte stack slots) are not wasm32's va_list
             * (8-aligned doubles): left out until they have an adapter */
            warning( "%s: varargs export %s has no wasm32 adapter yet\n", spec->file_name,
                     odp->name ? odp->name : get_link_name( odp ));
            continue;
        case TYPE_STDCALL:
        case TYPE_CDECL:
            if (odp->flags & FLAG_SYSCALL)
            {
                /* ntdll's system calls: a direct call into the Unix side on
                 * wasm32 (W2); no adapter until then */
                continue;
            }
            if (!(sig = find_func_sig( get_link_name( odp ))))
            {
                warning( "%s: %s is not defined in the objects; using the .spec's signature\n",
                         spec->file_name, get_link_name( odp ));
                sig = spec_sig( odp );
                output( "%s %s();\n", c_type( sig[0] ), get_link_name( odp ));
            }
            else output( "%s %s();\n", c_type( sig[0] ), get_link_name( odp ));
            output_adapter( odp, sig );
            break;
        default:
            continue;
        }
        strarray_add( &names, adapter_name( odp ));
    }

    /* the entry point: DllMain(hinst, reason, reserved), stdcall */
    output( "int gexp__entry(int ctx, int depth, unsigned int need, unsigned long long ret)\n{\n" );
    if (spec->init_func && find_func_sig( spec->init_func ))
    {
        output( "    int %s(int, int, int);\n", spec->init_func );
        output( "    struct __gexw_frame f;\n" );
        output( "    __gexw_enter(&f, ctx, depth);\n" );
        output( "    int r = %s((int)__gexw_arg32(ctx, 0), (int)__gexw_arg32(ctx, 4), (int)__gexw_arg32(ctx, 8));\n",
                spec->init_func );
        output( "    __gexw_leave(&f);\n" );
        output( "    return __gexw_ret(ctx, ret, 12, (unsigned int)r);\n}\n\n" );
    }
    else output( "    return __gexw_ret(ctx, ret, 12, 1);\n}\n\n" );
    output( "int gexp__missing(int ctx, int depth, unsigned int need, unsigned long long ret)\n{\n" );
    output( "    __gexw_missing(ctx);\n    return 0;\n}\n\n" );
    strarray_add( &names, "gexp__entry" );
    strarray_add( &names, "gexp__missing" );

    /* in the element segment, where gex finds them by name */
    output( "int (*const __gexw_adapters_%s[])(int, int, unsigned int, unsigned long long) __attribute__((used)) = {\n",
            spec->c_name );
    STRARRAY_FOR_EACH( name, &names ) output( "    %s,\n", name );
    output( "};\n" );

    if (!c_file) return;
    fclose( output_file );
    output_file = NULL;

    /* compile it */
    {
        struct strarray args = empty_strarray;
        strarray_addall( &args, cc_command );
        strarray_add( &args, "-fno-stack-protector" );
        strarray_add( &args, "-c" );
        strarray_add( &args, "-o" );
        strarray_add( &args, output_file_name );
        strarray_add( &args, c_file );
        spawn( args );
    }

    /* the data exports, for the link */
    if (!(rsp = fopen( strmake( "%s.rsp", output_file_name ), "w" )))
        fatal_error( "cannot create %s.rsp\n", output_file_name );
    STRARRAY_FOR_EACH( sym, &data_syms ) fprintf( rsp, "--export-if-defined=%s\n", sym );
    fclose( rsp );
}

/*******************************************************************
 *         wasm module access for the skeleton (spec32.c)
 */
void *read_wasm_module_info( const char *name )
{
    struct wasm_module_info *info = xmalloc( sizeof(*info) );
    read_wasm_module( name, info );
    return info;
}

void wasm_module_layout( const void *ptr, unsigned int *mem_size, unsigned int *mem_align,
                         const unsigned char **bytes, size_t *size )
{
    const struct wasm_module_info *info = ptr;
    *mem_size = info->mem_size;
    *mem_align = info->mem_align;
    *bytes = info->bytes;
    *size = info->size;
}
