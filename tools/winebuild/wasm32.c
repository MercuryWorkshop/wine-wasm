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
/* the one module of the whole PE side (pelink.py), by file name */
const char *wasm_shared_module = NULL;

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
    char **types = NULL, **import_names = NULL;
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
                char *field;
                free( wasm_name( &s ));
                field = wasm_name( &s );
                kind = *s.p++;
                switch (kind)
                {
                case 0:
                    func_type = xrealloc( func_type, (nimports + 1) * sizeof(*func_type) );
                    import_names = xrealloc( import_names, (nimports + 1) * sizeof(*import_names) );
                    import_names[nimports] = field;
                    field = NULL;
                    func_type[nimports++] = leb( &s );
                    break;
                case 1: { unsigned int fl; s.p++; fl = leb( &s ); leb( &s ); if (fl & 1) leb( &s ); break; }
                case 2: { unsigned int fl = leb( &s ); leb( &s ); if (fl & 1) leb( &s ); break; }
                case 3: s.p += 2; break;
                case 4: s.p++; leb( &s ); break;
                }
                free( field );
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
                            /* defined here, or called from here (an import: its
                             * type is the caller's view, which is what an
                             * adapter for a function another DLL defines needs) */
                            if (!sym && undefined && idx < nimports) sym = xstrdup( import_names[idx] );
                            if (sym && !local && idx < nimports + nfuncs && func_type[idx] < ntypes &&
                                (!undefined || !find_func_sig( sym )))
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
    for (i = 0; i < nimports; i++) free( import_names[i] );
    free( import_names );
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

/* The C identifier of an export's adapter; its symbol (an asm label) is
 * gexp_<export name>, or gexp_#<ordinal> for an export with no name, which
 * is what gex binds it by. */
static unsigned int adapter_count;

static const char *adapter_symbol( const ORDDEF *odp )
{
    /* the skeleton's export table names it, unless it is -noname */
    if (odp->name && !(odp->flags & FLAG_NONAME)) return strmake( "gexp_%s", odp->name );
    return strmake( "gexp_#%d", odp->ordinal );
}

/* An adapter is a body, which reads the arguments, calls the function and
 * returns to the guest, and the F_body gex binds, which runs the body
 * through gexrt's __gexw_entry (the C stack moved onto the guest's, the
 * per-thread state, and the catch for a guest state loaded underneath). */
static const char *output_adapter_header( const ORDDEF *odp )
{
    const char *ident = strmake( "__gexw_adapter_%u", adapter_count++ );
    output( "static int %s_body(int ctx, int depth, unsigned int need, unsigned long long ret);\n", ident );
    output( "int %s(int ctx, int depth, unsigned int need, unsigned long long ret) __asm__(\"%s\");\n",
            ident, adapter_symbol( odp ));
    output( "int %s(int ctx, int depth, unsigned int need, unsigned long long ret)\n{\n", ident );
    output( "    return __gexw_entry(ctx, depth, need, ret, %s_body);\n}\n", ident );
    output( "static int %s_body(int ctx, int depth, unsigned int need, unsigned long long ret)\n{\n", ident );
    return ident;
}

static int spec_arg_matches( enum arg_type t, char p )
{
    switch (t)
    {
    case ARG_FLOAT:  return p == 'f';
    case ARG_INT64:  return p == 'I';
    case ARG_DOUBLE: return p == 'd';
    case ARG_INT128: return 0;
    default:         return p == 'i';
    }
}

/* Parameters that are a one-word struct passed by value. The .spec spells
 * them as one long, which the counts below cannot tell from an int, but
 * wasm32 C takes the struct by pointer: passing the value made
 * GdiAlphaBlend read its BLENDFUNCTION from the address 0x01ff0000, so
 * every AlphaBlend from i386 code drew nothing. */
static int word_struct_param( const ORDDEF *odp, unsigned int j )
{
    static const struct { const char *name; unsigned int param; } table[] =
    {
        { "GdiAlphaBlend", 10 },  /* BLENDFUNCTION */
    };
    const char *name = odp->name ? odp->name : get_link_name( odp );
    unsigned int i;

    for (i = 0; i < sizeof(table) / sizeof(table[0]); i++)
        if (table[i].param == j && !strcmp( table[i].name, name )) return 1;
    return 0;
}

/* One entry adapter, or NULL when the .spec and the code do not agree.
 *
 * The .spec's arguments are what the i386 caller pushed; the wasm
 * parameters are what the C function takes. They are matched in order:
 * an argument whose kind matches the parameter is passed as is; otherwise
 * the parameter is an i32 pointing at a struct passed by value (wasm32
 * passes those by pointer), made of as many of the caller's arguments as
 * the counts leave (LARGE_INTEGER is one int64, a D2D1_POINT_2F two
 * floats), so the adapter passes the address of the guest's copy.
 * A small struct returned by value is a hidden first pointer parameter on
 * wasm32 (and a void result); i386 returns it in EDX:EAX. */
static const char *output_adapter( const ORDDEF *odp, const char *sig )
{
    const char *link = get_link_name( odp );
    const char *params = strchr( sig, ':' ) + 1;
    char ret = sig[0];
    unsigned int nargs = odp->u.func.nb_args, nparams;
    unsigned int words = spec_words( odp ), i, j, off = 0, regs = 0, pop;
    unsigned int first[MAX_ARGUMENTS + 1], count[MAX_ARGUMENTS + 1];
    int sret = 0, va_param = -1, va_wide = 0;
    unsigned int va_off = 0, va_fmt_off = 0;
    const char *p, *ident;

    if (strlen( params ) == nargs + 1 && ret == 'v' && params[0] == 'i' &&
        (!nargs || !spec_arg_matches( odp->u.func.args[0], 'i' ) || nargs + 1 == strlen( params )))
    {
        /* hidden struct-return pointer, unless the counts say otherwise */
        sret = 1;
        params++;
    }
    nparams = strlen( params );
    if (nparams > MAX_ARGUMENTS) return NULL;

    /* match each parameter with the run of caller arguments it takes */
    for (i = j = 0; j < nparams; j++)
    {
        unsigned int left_args, left_params = nparams - j - 1;
        if (i >= nargs) return NULL;
        first[j] = i;
        left_args = nargs - i - 1;
        if (spec_arg_matches( odp->u.func.args[i], params[j] ) && left_args >= left_params)
        {
            count[j] = 1;
            i++;
            continue;
        }
        /* a 64-bit parameter the .spec spells as two words (ntdll's
         * NtWow64ReadVirtualMemory64: long long for each ULONG64) */
        if (params[j] == 'I' && i + 1 < nargs && left_args >= left_params + 1 &&
            spec_arg_matches( odp->u.func.args[i], 'i' ) && spec_arg_matches( odp->u.func.args[i + 1], 'i' ))
        {
            count[j] = 2;
            i += 2;
            continue;
        }
        if (params[j] != 'i') return NULL;
        /* a struct by value: as many arguments as leave one per remaining parameter */
        if (nargs - i < left_params + 1) return NULL;
        count[j] = nargs - i - left_params;
        i += count[j];
    }
    if (i != nargs) return NULL;

    /* thiscall: `this` in ECX; fastcall: the first two words in ECX, EDX */
    if (odp->flags & FLAG_THISCALL) regs = 1;
    else if (odp->flags & FLAG_FASTCALL) regs = words < 2 ? words : 2;
    pop = (odp->type == TYPE_STDCALL) ? 4 * (words - regs) : 0;

    /* a printf taking a va_list (vsnprintf): the guest's va_list is its
     * argument words, laid out again from the format as for varargs */
    {
        const char *name = odp->name ? odp->name : link;
        unsigned int k, o = 0, fo = 0;
        va_param = -1;
        if (strstr( name, "printf" ) && nparams == nargs && nargs >= 2 &&
            odp->u.func.args[nargs - 1] == ARG_PTR)
        {
            for (k = 0; k + 1 < nargs; k++)
            {
                if (odp->u.func.args[k] == ARG_STR || odp->u.func.args[k] == ARG_WSTR)
                {
                    va_param = nargs - 1;
                    fo = o;
                    va_wide = odp->u.func.args[k] == ARG_WSTR;
                }
                o += spec_arg_size( odp->u.func.args[k] );
            }
            va_off = o;
            va_fmt_off = fo;
        }
    }
    output( "%s %s();\n", c_type( sret ? 'v' : ret ), link );
    ident = output_adapter_header( odp );
    if (sret) output( "    unsigned long long sr __attribute__((aligned(8))) = 0;\n" );
    if (va_param >= 0)
    {
        output( "    unsigned int va = __gexw_arg32(ctx, %u);\n", va_off );
        output( "    const void *fmt = (const void *)__gexw_arg32(ctx, %u);\n", va_fmt_off );
        output( "    void *vabuf = __builtin_alloca(__gexw_va_printf_size(va, fmt, %d));\n", va_wide );
        output( "    __gexw_va_printf_copy(va, fmt, %d, vabuf);\n", va_wide );
    }
    output( "    " );
    if (ret != 'v') output( "%s r = ", c_type( ret ));
    output( "((%s (*)(", c_type( ret ));
    if (sret) output( "void *%s", *params ? ", " : "" );
    for (p = params; *p; p++) output( "%s%s", p == params ? "" : ", ", c_type( *p ));
    if (!*params && !sret) output( "void" );
    output( "))%s)(", link );
    if (sret) output( "&sr%s", *params ? ", " : "" );
    for (j = 0; j < nparams; j++)
    {
        unsigned int bytes = 0, k;
        for (k = first[j]; k < first[j] + count[j]; k++) bytes += spec_arg_size( odp->u.func.args[k] );
        if (j) output( ", " );
        if (j < regs) output( "(int)__gexw_reg(ctx, %u)", j == 0 ? 1 : 2 );  /* ECX, EDX */
        else
        {
            if ((int)j == va_param)
                output( "(int)vabuf" );
            else if (params[j] == 'I' && count[j] == 2)
                output( "(long long)__gexw_arg64(ctx, %u)", off );
            else if (count[j] > 1 || !spec_arg_matches( odp->u.func.args[first[j]], params[j] ) ||
                     word_struct_param( odp, j ))
                output( "(int)__gexw_argptr(ctx, %u)", off );  /* a struct by value */
            else switch (params[j])
            {
            case 'i': output( "(int)__gexw_arg32(ctx, %u)", off ); break;
            case 'f': output( "__gexw_argf32(ctx, %u)", off ); break;
            case 'I': output( "(long long)__gexw_arg64(ctx, %u)", off ); break;
            case 'd': output( "__gexw_argf64(ctx, %u)", off ); break;
            }
            off += bytes;
        }
    }
    output( ");\n" );
    if (sret) ret = 's';
    switch (ret)
    {
    case 's': output( "    return __gexw_ret(ctx, ret, %u, sr);\n", pop ); break;
    case 'v': output( "    return __gexw_ret32(ctx, ret, %u, 0);\n", pop ); break;
    case 'i': output( "    return __gexw_ret32(ctx, ret, %u, (unsigned int)r);\n", pop ); break;
    case 'I': output( "    return __gexw_ret(ctx, ret, %u, (unsigned long long)r);\n", pop ); break;
    case 'f':
    case 'd': output( "    return __gexw_ret_fp(ctx, ret, %u, r);\n", pop ); break;
    }
    output( "}\n\n" );
    return ident;
}

/* A varargs export (cdecl). On wasm32 a variadic function takes its fixed
 * arguments and then a pointer to a buffer holding the rest at their
 * natural alignments; i386 leaves them in 4-byte stack slots after the
 * fixed ones. Words read the same either way, so the guest's stack is
 * passed as the buffer, except for a printf (by name), whose format can
 * name 8-byte arguments: gexrt lays those out again (__gexw_va_printf_*).
 * NULL when the code's signature is not the fixed arguments plus one. */
static const char *output_varargs_adapter( const ORDDEF *odp, const char *sig )
{
    const char *link = get_link_name( odp ), *params = strchr( sig, ':' ) + 1, *ident, *callee;
    const char *name = odp->name ? odp->name : link;
    char ret = sig[0];
    unsigned int nargs = odp->u.func.nb_args, j, off = 0, fmt_off = 0;
    int fmt = -1, wide = 0;

    if (strlen( params ) != nargs + 1 || params[nargs] != 'i') return NULL;
    for (j = 0; j < nargs; j++)
        if (!spec_arg_matches( odp->u.func.args[j], params[j] )) return NULL;
    if (strstr( name, "printf" ) || strstr( name, "PRINTF" ))
    {
        for (j = 0; j < nargs; j++)
        {
            if (odp->u.func.args[j] == ARG_STR || odp->u.func.args[j] == ARG_WSTR)
            {
                fmt = j;
                fmt_off = off;
                wide = odp->u.func.args[j] == ARG_WSTR;
            }
            off += spec_arg_size( odp->u.func.args[j] );
        }
    }
    else for (j = 0; j < nargs; j++) off += spec_arg_size( odp->u.func.args[j] );

    /* (declared under a name of its own: another adapter may have
     * declared the link name already, without the wasm signature) */
    output( "%s __gexw_va_%u(", c_type( ret ), adapter_count );
    for (j = 0; j <= nargs; j++) output( "%s%s", j ? ", " : "", c_type( params[j] ));
    output( ") __asm__(\"%s\");\n", link );
    callee = strmake( "__gexw_va_%u", adapter_count );
    ident = output_adapter_header( odp );
    output( "    unsigned int va = __gexw_argptr(ctx, %u);\n", off );
    output( "    void *buf = (void *)va;\n" );
    if (fmt >= 0)
    {
        output( "    const void *fmt = (const void *)__gexw_arg32(ctx, %u);\n", fmt_off );
        output( "    unsigned int n = __gexw_va_printf_size(va, fmt, %d);\n", wide );
        output( "    buf = __builtin_alloca(n);\n" );
        output( "    __gexw_va_printf_copy(va, fmt, %d, buf);\n", wide );
    }
    output( "    " );
    if (ret != 'v') output( "%s r = ", c_type( ret ));
    output( "%s(", callee );
    for (j = 0, off = 0; j < nargs; j++)
    {
        switch (params[j])
        {
        case 'i': output( "(int)__gexw_arg32(ctx, %u), ", off ); break;
        case 'f': output( "__gexw_argf32(ctx, %u), ", off ); break;
        case 'I': output( "(long long)__gexw_arg64(ctx, %u), ", off ); break;
        case 'd': output( "__gexw_argf64(ctx, %u), ", off ); break;
        }
        off += spec_arg_size( odp->u.func.args[j] );
    }
    output( "(int)buf);\n" );
    switch (ret)
    {
    case 'v': output( "    return __gexw_ret32(ctx, ret, 0, 0);\n" ); break;
    case 'i': output( "    return __gexw_ret32(ctx, ret, 0, (unsigned int)r);\n" ); break;
    case 'I': output( "    return __gexw_ret(ctx, ret, 0, (unsigned long long)r);\n" ); break;
    case 'f':
    case 'd': output( "    return __gexw_ret_fp(ctx, ret, 0, r);\n" ); break;
    }
    output( "}\n\n" );
    return ident;
}

/* The wasm signature of an export the objects neither define nor call
 * (user32's AttachThreadInput is win32u's NtUserAttachThreadInput): from
 * the .spec, every argument a word but int64, float and double; NULL when
 * that is a guess too far (a 128-bit argument). The link then checks it
 * against the definition. */
static const char *spec_sig( const ORDDEF *odp )
{
    char buf[MAX_ARGUMENTS + 4];
    unsigned int i, n = 0;

    if (strchr( get_link_name( odp ), '.' )) return NULL;
    buf[n++] = (odp->flags & FLAG_RET64) ? 'I' : 'i';
    buf[n++] = ':';
    for (i = 0; i < odp->u.func.nb_args; i++)
    {
        switch (odp->u.func.args[i])
        {
        case ARG_INT64:  buf[n++] = 'I'; break;
        case ARG_FLOAT:  buf[n++] = 'f'; break;
        case ARG_DOUBLE: buf[n++] = 'd'; break;
        case ARG_INT128: return NULL;
        default:         buf[n++] = 'i'; break;
        }
    }
    buf[n] = 0;
    return xstrdup( buf );
}

/* An unimplemented export: raise what Wine raises. */
static const char *output_stub_adapter( DLLSPEC *spec, const ORDDEF *odp )
{
    const char *ident = output_adapter_header( odp );
    output( "    __wine_spec_unimplemented_stub(\"%s\", \"%s\");\n", spec->file_name,
            odp->name ? odp->name : odp->export_name ? odp->export_name : strmake( "#%d", odp->ordinal ));
    output( "    return 0;\n}\n\n" );
    return ident;
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
    output( "int __gexw_entry(int, int, unsigned int, unsigned long long, int (*)(int, int, unsigned int, unsigned long long));\n" );
    output( "unsigned int __gexw_arg32(int, unsigned int);\n" );
    output( "unsigned long long __gexw_arg64(int, unsigned int);\n" );
    output( "float __gexw_argf32(int, unsigned int);\n" );
    output( "double __gexw_argf64(int, unsigned int);\n" );
    output( "unsigned int __gexw_reg(int, unsigned int);\n" );
    output( "unsigned int __gexw_argptr(int, unsigned int);\n" );
    output( "unsigned int __gexw_va_printf_size(unsigned int, const void *, int);\n" );
    output( "void __gexw_va_printf_copy(unsigned int, const void *, int, void *);\n" );
    output( "int __gexw_ret(int, unsigned long long, unsigned int, unsigned long long);\n" );
    output( "int __gexw_ret32(int, unsigned long long, unsigned int, unsigned int);\n" );
    output( "int __gexw_ret_fp(int, unsigned long long, unsigned int, double);\n" );
    output( "void __gexw_missing(int);\n" );
    output( "void __wine_spec_unimplemented_stub(const char *, const char *);\n\n" );

    for (i = 0; i < exports->nb_entry_points; i++)
    {
        ORDDEF *odp = exports->entry_points[i];
        const char *sig, *ident = NULL;

        if (odp->flags & FLAG_FORWARD) continue;  /* the skeleton forwards it */
        switch (odp->type)
        {
        case TYPE_EXTERN:
            strarray_add( &data_syms, get_link_name( odp ));
            continue;
        case TYPE_STUB:
            ident = output_stub_adapter( spec, odp );
            break;
        case TYPE_VARARGS:
            if (odp->flags & FLAG_IMPORT) continue;
            if (find_func_sig( adapter_symbol( odp ))) goto handwritten;
            if (!(sig = find_func_sig( get_link_name( odp ))))
            {
                warning( "%s: %s is not defined in the objects; no adapter\n",
                         spec->file_name, get_link_name( odp ));
                continue;
            }
            if (!(ident = output_varargs_adapter( odp, sig )))
                warning( "%s: varargs %s: the .spec's arguments do not fit the code's (%s); no adapter\n",
                         spec->file_name, odp->name ? odp->name : get_link_name( odp ), sig );
            break;
        case TYPE_STDCALL:
        case TYPE_CDECL:
            /* written by hand in the objects (ntdll's KiUser*Dispatcher,
             * entered with no return address) */
            if (find_func_sig( adapter_symbol( odp )))
            {
            handwritten:
                /* in the element segment like the others */
                ident = strmake( "__gexw_adapter_%u", adapter_count++ );
                output( "int %s(int, int, unsigned int, unsigned long long) __asm__(\"%s\");\n",
                        ident, adapter_symbol( odp ));
                break;
            }
            /* re-exported from a DLL it imports (-import): the owner's
             * adapter serves it (gex binds the same slot), unless it is
             * there under another name (user32's GetKeyState is win32u's
             * NtUserGetKeyState): then an adapter of its own */
            if ((odp->flags & FLAG_IMPORT) && (!odp->name || !strcmp( odp->name, get_link_name( odp ))))
                continue;
            if (!(sig = find_func_sig( get_link_name( odp ))) &&
                !(sig = spec_sig( odp )))
            {
                /* not in the objects (an i386 export written in assembly
                 * there, for one): no adapter, gex binds gexp__missing */
                warning( "%s: %s is not defined in the objects; no adapter\n",
                         spec->file_name, get_link_name( odp ));
                continue;
            }
            if (!(ident = output_adapter( odp, sig )))
                warning( "%s: %s: the .spec's arguments do not fit the code's (%s); no adapter\n",
                         spec->file_name, odp->name ? odp->name : get_link_name( odp ), sig );
            break;
        default:
            continue;
        }
        if (ident) strarray_add( &names, ident );
    }

    /* a program's entry point: mainCRTStartup(void), cdecl (its caller
     * keeps its own stack pointer) */
    if (spec->init_func && !(spec->characteristics & IMAGE_FILE_DLL))
    {
        output( "int %s(void);\n", spec->init_func );
        output( "static int gexp__entry_body(int ctx, int depth, unsigned int need, unsigned long long ret)\n{\n" );
        output( "    return __gexw_ret32(ctx, ret, 0, (unsigned int)%s());\n}\n\n", spec->init_func );
        output( "int gexp__entry(int ctx, int depth, unsigned int need, unsigned long long ret)\n{\n" );
        output( "    return __gexw_entry(ctx, depth, need, ret, gexp__entry_body);\n}\n\n" );
    }
    /* the entry point: DllMain(hinst, reason, reserved), stdcall */
    else if (spec->init_func && find_func_sig( spec->init_func ))
    {
        output( "int %s(int, int, int);\n", spec->init_func );
        output( "static int gexp__entry_body(int ctx, int depth, unsigned int need, unsigned long long ret)\n{\n" );
        output( "    int r = %s((int)__gexw_arg32(ctx, 0), (int)__gexw_arg32(ctx, 4), (int)__gexw_arg32(ctx, 8));\n",
                spec->init_func );
        output( "    return __gexw_ret32(ctx, ret, 12, (unsigned int)r);\n}\n\n" );
        output( "int gexp__entry(int ctx, int depth, unsigned int need, unsigned long long ret)\n{\n" );
        output( "    return __gexw_entry(ctx, depth, need, ret, gexp__entry_body);\n}\n\n" );
    }
    else
    {
        output( "int gexp__entry(int ctx, int depth, unsigned int need, unsigned long long ret)\n{\n" );
        output( "    return __gexw_ret32(ctx, ret, 12, 1);\n}\n\n" );
    }
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

    /* every exported link name, for the one-module PE side (pesym.py) */
    if (!(rsp = fopen( strmake( "%s.exports", output_file_name ), "w" )))
        fatal_error( "cannot create %s.exports\n", output_file_name );
    /* winecrt0's base of this DLL, which gex writes (wasmdll) */
    fprintf( rsp, "__gexw_image_base data\n" );
    for (i = 0; i < exports->nb_entry_points; i++)
    {
        ORDDEF *odp = exports->entry_points[i];
        if (odp->flags & FLAG_FORWARD)
        {
            /* another DLL's export: what callers of this name reach */
            if (odp->name) fprintf( rsp, "%s forward %s\n", odp->name, odp->link_name );
            continue;
        }
        if (odp->flags & FLAG_IMPORT)
        {
            /* defined elsewhere; under another name there (user32's
             * ClipCursor is win32u's NtUserClipCursor): a forward to it */
            if (odp->name && strcmp( odp->name, get_link_name( odp )))
            {
                char *base = xstrdup( spec->file_name ), *dot = strrchr( base, '.' );
                if (dot) *dot = 0;
                fprintf( rsp, "%s forward %s.%s\n", odp->name, base, get_link_name( odp ));
                free( base );
            }
            continue;
        }
        if (odp->type == TYPE_STUB || odp->type == TYPE_VARIABLE) continue;
        fprintf( rsp, "%s %s\n", get_link_name( odp ), odp->type == TYPE_EXTERN ? "data" : "func" );
        /* exported under another name than its code's (kernelbase's
         * FreeEnvironmentStringsA is FreeEnvironmentStringsW): callers of
         * that name reach it as a forward to this DLL */
        if (odp->name && odp->type != TYPE_EXTERN && strcmp( odp->name, get_link_name( odp )))
        {
            char *base = xstrdup( spec->file_name ), *dot = strrchr( base, '.' );
            if (dot) *dot = 0;
            fprintf( rsp, "%s forward %s.%s\n", odp->name, base, get_link_name( odp ));
            free( base );
        }
    }
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
