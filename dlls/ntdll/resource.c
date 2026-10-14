/*
 * PE file resources
 *
 * Copyright 1995 Thomas Sandford
 * Copyright 1996 Martin von Loewis
 * Copyright 2003 Alexandre Julliard
 *
 * Based on the Win16 resource handling code in loader/resource.c
 * Copyright 1993 Robert J. Amstadt
 * Copyright 1995 Alexandre Julliard
 * Copyright 1997 Marcus Meissner
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

#include <stdarg.h>
#include <stdlib.h>
#include <sys/types.h>

#include "ntstatus.h"
#include "windef.h"
#include "winbase.h"
#include "winnt.h"
#include "winternl.h"
#include "ntdll_misc.h"
#include "wine/asm.h"
#include "wine/exception.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(resource);

#define IS_INTRESOURCE(x)       (((ULONG_PTR)(x) >> 16) == 0)

/**********************************************************************
 *  is_data_file_module
 *
 * Check if a module handle is for a LOAD_LIBRARY_AS_DATAFILE module.
 */
static inline BOOL is_data_file_module( HMODULE hmod )
{
    return (ULONG_PTR)hmod & 1;
}


/**********************************************************************
 *  push_language
 *
 * push a language in the list of languages to try
 */
static inline int push_language( WORD *list, int pos, WORD lang )
{
    int i;
    for (i = 0; i < pos; i++) if (list[i] == lang) return pos;
    list[pos++] = lang;
    return pos;
}


/**********************************************************************
 *  find_first_entry
 *
 * Find the first suitable entry in a resource directory
 */
static const IMAGE_RESOURCE_DIRECTORY *find_first_entry( const IMAGE_RESOURCE_DIRECTORY *dir,
                                                         const void *root, int want_dir )
{
    const IMAGE_RESOURCE_DIRECTORY_ENTRY *entry = (const IMAGE_RESOURCE_DIRECTORY_ENTRY *)(dir + 1);
    int pos;

    for (pos = 0; pos < dir->NumberOfNamedEntries + dir->NumberOfIdEntries; pos++)
    {
        if (!entry[pos].DataIsDirectory == !want_dir)
            return (const IMAGE_RESOURCE_DIRECTORY *)((const char *)root + entry[pos].OffsetToDirectory);
    }
    return NULL;
}


/**********************************************************************
 *  find_entry_by_id
 *
 * Find an entry by id in a resource directory
 */
static const IMAGE_RESOURCE_DIRECTORY *find_entry_by_id( const IMAGE_RESOURCE_DIRECTORY *dir,
                                                         WORD id, const void *root, int want_dir )
{
    const IMAGE_RESOURCE_DIRECTORY_ENTRY *entry;
    int min, max, pos;

    entry = (const IMAGE_RESOURCE_DIRECTORY_ENTRY *)(dir + 1);
    min = dir->NumberOfNamedEntries;
    max = min + dir->NumberOfIdEntries - 1;
    while (min <= max)
    {
        pos = (min + max) / 2;
        if (entry[pos].Id == id)
        {
            if (!entry[pos].DataIsDirectory == !want_dir)
            {
                TRACE("root %p dir %p id %04x ret %p\n",
                      root, dir, id, (const char*)root + entry[pos].OffsetToDirectory);
                return (const IMAGE_RESOURCE_DIRECTORY *)((const char *)root + entry[pos].OffsetToDirectory);
            }
            break;
        }
        if (entry[pos].Id > id) max = pos - 1;
        else min = pos + 1;
    }
    TRACE("root %p dir %p id %04x not found\n", root, dir, id );
    return NULL;
}


/**********************************************************************
 *  find_entry_by_name
 *
 * Find an entry by name in a resource directory
 */
static const IMAGE_RESOURCE_DIRECTORY *find_entry_by_name( const IMAGE_RESOURCE_DIRECTORY *dir,
                                                           LPCWSTR name, const void *root,
                                                           int want_dir )
{
    const IMAGE_RESOURCE_DIRECTORY_ENTRY *entry;
    const IMAGE_RESOURCE_DIR_STRING_U *str;
    int min, max, res, pos, namelen;

    if (IS_INTRESOURCE(name)) return find_entry_by_id( dir, LOWORD(name), root, want_dir );
    entry = (const IMAGE_RESOURCE_DIRECTORY_ENTRY *)(dir + 1);
    namelen = wcslen(name);
    min = 0;
    max = dir->NumberOfNamedEntries - 1;
    while (min <= max)
    {
        pos = (min + max) / 2;
        str = (const IMAGE_RESOURCE_DIR_STRING_U *)((const char *)root + entry[pos].NameOffset);
        res = wcsncmp( name, str->NameString, str->Length );
        if (!res && namelen == str->Length)
        {
            if (!entry[pos].DataIsDirectory == !want_dir)
            {
                TRACE("root %p dir %p name %s ret %p\n",
                      root, dir, debugstr_w(name), (const char*)root + entry[pos].OffsetToDirectory);
                return (const IMAGE_RESOURCE_DIRECTORY *)((const char *)root + entry[pos].OffsetToDirectory);
            }
            break;
        }
        if (res < 0) max = pos - 1;
        else min = pos + 1;
    }
    TRACE("root %p dir %p name %s not found\n", root, dir, debugstr_w(name) );
    return NULL;
}


/**********************************************************************
 *  find_entry
 *
 * Find a resource entry
 */
static NTSTATUS find_entry( HMODULE hmod, const LDR_RESOURCE_INFO *info,
                            ULONG level, const void **ret, int want_dir )
{
    ULONG size;
    const void *root;
    const IMAGE_RESOURCE_DIRECTORY *resdirptr;
    WORD list[9];  /* list of languages to try */
    int i, pos = 0;

    root = RtlImageDirectoryEntryToData( hmod, TRUE, IMAGE_DIRECTORY_ENTRY_RESOURCE, &size );
    if (!root) return STATUS_RESOURCE_DATA_NOT_FOUND;
    if (size < sizeof(*resdirptr)) return STATUS_RESOURCE_DATA_NOT_FOUND;
    resdirptr = root;

    if (!level--) goto done;
    if (!(*ret = find_entry_by_name( resdirptr, (LPCWSTR)info->Type, root, want_dir || level )))
        return STATUS_RESOURCE_TYPE_NOT_FOUND;
    if (!level--) return STATUS_SUCCESS;

    resdirptr = *ret;
    if (!(*ret = find_entry_by_name( resdirptr, (LPCWSTR)info->Name, root, want_dir || level )))
        return STATUS_RESOURCE_NAME_NOT_FOUND;
    if (!level--) return STATUS_SUCCESS;
    if (level) return STATUS_INVALID_PARAMETER;  /* level > 3 */

    /* 1. specified language */
    pos = push_language( list, pos, info->Language );

    /* 2. specified language with neutral sublanguage */
    pos = push_language( list, pos, MAKELANGID( PRIMARYLANGID(info->Language), SUBLANG_NEUTRAL ) );

    /* 3. neutral language with neutral sublanguage */
    pos = push_language( list, pos, MAKELANGID( LANG_NEUTRAL, SUBLANG_NEUTRAL ) );

    /* if no explicitly specified language, try some defaults */
    if (PRIMARYLANGID(info->Language) == LANG_NEUTRAL)
    {
        LANGID user_lang, user_neutral_lang, system_lang;

        get_resource_lcids( &user_lang, &user_neutral_lang, &system_lang );

        /* user defaults, unless SYS_DEFAULT sublanguage specified  */
        if (SUBLANGID(info->Language) != SUBLANG_SYS_DEFAULT)
        {
            /* 4. current thread locale language */
            pos = push_language( list, pos, LANGIDFROMLCID(NtCurrentTeb()->CurrentLocale) );

            /* 5. user locale language */
            pos = push_language( list, pos, user_lang );

            /* 6. user locale language with neutral sublanguage  */
            pos = push_language( list, pos, user_neutral_lang );
        }

        /* 7. system locale language */
        pos = push_language( list, pos, system_lang );

        /* 8. system locale language with neutral sublanguage */
        pos = push_language( list, pos, PRIMARYLANGID( system_lang ));

        /* 9. English */
        pos = push_language( list, pos, MAKELANGID( LANG_ENGLISH, SUBLANG_DEFAULT ) );
    }

    resdirptr = *ret;
    for (i = 0; i < pos; i++)
        if ((*ret = find_entry_by_id( resdirptr, list[i], root, want_dir ))) return STATUS_SUCCESS;

    /* if no explicitly specified language, return the first entry */
    if (PRIMARYLANGID(info->Language) == LANG_NEUTRAL)
    {
        if ((*ret = find_first_entry( resdirptr, root, want_dir ))) return STATUS_SUCCESS;
    }
    return STATUS_RESOURCE_LANG_NOT_FOUND;

done:
    *ret = resdirptr;
    return STATUS_SUCCESS;
}


/**********************************************************************
 *  MUI resource modules
 *
 * A Windows language-neutral image (one with a "MUI" resource) keeps its
 * localizable resources in <dir>\<language>\<name>.mui. A resource the
 * image does not have is looked up there, as Windows' loader does, and an
 * entry found there is accessed relative to the .mui image.
 */
struct mui_module
{
    struct mui_module *next;
    WCHAR             *ln_name;   /* NT name of the language-neutral file */
    HMODULE            mui;       /* the .mui file mapped as a data file (base | 1), or 0 if none */
    SIZE_T             size;
};

static struct mui_module *mui_modules;
static RTL_CRITICAL_SECTION mui_section;
static RTL_CRITICAL_SECTION_DEBUG mui_section_debug =
{
    0, 0, &mui_section,
    { &mui_section_debug.ProcessLocksList, &mui_section_debug.ProcessLocksList },
      0, 0, { (DWORD_PTR)(__FILE__ ": mui_section") }
};
static RTL_CRITICAL_SECTION mui_section = { &mui_section_debug, -1, 0, 0, 0, 0 };

static BOOL has_mui_resource( HMODULE hmod )
{
    static const WCHAR muiW[] = {'M','U','I',0};
    const IMAGE_RESOURCE_DIRECTORY *root;
    ULONG size;

    if (!(root = RtlImageDirectoryEntryToData( hmod, TRUE, IMAGE_DIRECTORY_ENTRY_RESOURCE, &size )))
        return FALSE;
    if (size < sizeof(*root)) return FALSE;
    return find_entry_by_name( root, muiW, root, TRUE ) != NULL;
}

static HMODULE map_data_file( const WCHAR *name, SIZE_T *size )
{
    UNICODE_STRING str;
    OBJECT_ATTRIBUTES attr;
    IO_STATUS_BLOCK io;
    HANDLE file, mapping;
    void *base = NULL;
    NTSTATUS status;

    RtlInitUnicodeString( &str, name );
    InitializeObjectAttributes( &attr, &str, OBJ_CASE_INSENSITIVE, 0, NULL );
    if (NtOpenFile( &file, GENERIC_READ | SYNCHRONIZE, &attr, &io, FILE_SHARE_READ | FILE_SHARE_DELETE,
                    FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE ))
        return 0;
    status = NtCreateSection( &mapping, STANDARD_RIGHTS_REQUIRED | SECTION_QUERY | SECTION_MAP_READ,
                              NULL, NULL, PAGE_READONLY, SEC_COMMIT, file );
    NtClose( file );
    if (status) return 0;
    *size = 0;
    status = NtMapViewOfSection( mapping, GetCurrentProcess(), &base, 0, 0, NULL, size,
                                 ViewShare, 0, PAGE_READONLY );
    NtClose( mapping );
    if (status) return 0;
    if (!RtlImageNtHeader( base ))
    {
        NtUnmapViewOfSection( GetCurrentProcess(), base );
        return 0;
    }
    return (HMODULE)((ULONG_PTR)base | 1);
}

/* the .mui file for a language-neutral file, in the first UI language that has one */
static HMODULE load_mui_file( const WCHAR *ln_name, SIZE_T *size )
{
    static const WCHAR muiW[] = {'.','m','u','i',0};
    LANGID langs[3];
    const WCHAR *base = wcsrchr( ln_name, '\\' );
    WCHAR locale[LOCALE_NAME_MAX_LENGTH], *path;
    UNICODE_STRING str;
    HMODULE ret = 0;
    unsigned int i, n = 0;
    SIZE_T dir_len;

    if (!base) return 0;
    base++;
    dir_len = base - ln_name;
    NtQueryDefaultUILanguage( &langs[n++] );
    langs[n++] = MAKELANGID( LANG_ENGLISH, SUBLANG_ENGLISH_US );

    if (!(path = RtlAllocateHeap( GetProcessHeap(), 0,
                                  (dir_len + LOCALE_NAME_MAX_LENGTH + wcslen( base ) + 6) * sizeof(WCHAR) )))
        return 0;
    for (i = 0; i < n && !ret; i++)
    {
        if (i && langs[i] == langs[0]) continue;
        str.Buffer = locale;
        str.MaximumLength = sizeof(locale);
        if (RtlLcidToLocaleName( langs[i], &str, 0, FALSE )) continue;
        locale[str.Length / sizeof(WCHAR)] = 0;
        memcpy( path, ln_name, dir_len * sizeof(WCHAR) );
        path[dir_len] = 0;
        wcscat( path, locale );
        wcscat( path, L"\\" );
        wcscat( path, base );
        wcscat( path, muiW );
        ret = map_data_file( path, size );
        TRACE( "%s -> %p\n", debugstr_w(path), ret );
    }
    RtlFreeHeap( GetProcessHeap(), 0, path );
    return ret;
}

/* the .mui module of a language-neutral module, mapped on first use */
static HMODULE get_mui_module( HMODULE hmod )
{
    struct mui_module *mod;
    MEMORY_SECTION_NAME *name;
    SIZE_T len = sizeof(*name) + 1024 * sizeof(WCHAR);
    HMODULE ret = 0;

    if (!has_mui_resource( hmod )) return 0;
    if (!(name = RtlAllocateHeap( GetProcessHeap(), 0, len ))) return 0;
    if (NtQueryVirtualMemory( GetCurrentProcess(), (void *)((ULONG_PTR)hmod & ~3),
                              MemoryMappedFilenameInformation, name, len, NULL ))
    {
        RtlFreeHeap( GetProcessHeap(), 0, name );
        return 0;
    }

    RtlEnterCriticalSection( &mui_section );
    for (mod = mui_modules; mod; mod = mod->next)
        if (!wcsicmp( mod->ln_name, name->SectionFileName.Buffer )) break;
    if (!mod && (mod = RtlAllocateHeap( GetProcessHeap(), 0, sizeof(*mod) )))
    {
        mod->ln_name = RtlAllocateHeap( GetProcessHeap(), 0, name->SectionFileName.Length + sizeof(WCHAR) );
        if (mod->ln_name)
        {
            memcpy( mod->ln_name, name->SectionFileName.Buffer, name->SectionFileName.Length + sizeof(WCHAR) );
            mod->mui = load_mui_file( mod->ln_name, &mod->size );
            mod->next = mui_modules;
            mui_modules = mod;
        }
        else
        {
            RtlFreeHeap( GetProcessHeap(), 0, mod );
            mod = NULL;
        }
    }
    if (mod) ret = mod->mui;
    RtlLeaveCriticalSection( &mui_section );
    RtlFreeHeap( GetProcessHeap(), 0, name );
    return ret;
}

/* the .mui module an entry found by find_resource() is in, if it is in one */
static HMODULE mui_module_from_entry( const void *entry )
{
    struct mui_module *mod;
    HMODULE ret = 0;

    RtlEnterCriticalSection( &mui_section );
    for (mod = mui_modules; mod; mod = mod->next)
    {
        const char *base = (const char *)((ULONG_PTR)mod->mui & ~3);
        if (mod->mui && (const char *)entry >= base && (const char *)entry < base + mod->size)
        {
            ret = mod->mui;
            break;
        }
    }
    RtlLeaveCriticalSection( &mui_section );
    return ret;
}

/* find_entry(), falling back to the module's .mui file */
static NTSTATUS find_resource( HMODULE hmod, const LDR_RESOURCE_INFO *info,
                               ULONG level, const void **ret, int want_dir )
{
    NTSTATUS status = find_entry( hmod, info, level, ret, want_dir );
    HMODULE mui;

    if (status != STATUS_RESOURCE_TYPE_NOT_FOUND && status != STATUS_RESOURCE_NAME_NOT_FOUND &&
        status != STATUS_RESOURCE_LANG_NOT_FOUND)
        return status;
    if (!(mui = get_mui_module( hmod ))) return status;
    if (find_entry( mui, info, level, ret, want_dir )) return status;
    return STATUS_SUCCESS;
}


/**********************************************************************
 *	LdrFindResourceDirectory_U  (NTDLL.@)
 */
NTSTATUS WINAPI DECLSPEC_HOTPATCH LdrFindResourceDirectory_U( HMODULE hmod, const LDR_RESOURCE_INFO *info,
                                            ULONG level, const IMAGE_RESOURCE_DIRECTORY **dir )
{
    const void *res;
    NTSTATUS status;

    __TRY
    {
	if (info) TRACE( "module %p type %s name %s lang %04lx level %ld\n",
                     hmod, debugstr_w((LPCWSTR)info->Type),
                     level > 1 ? debugstr_w((LPCWSTR)info->Name) : "",
                     level > 2 ? info->Language : 0, level );

        status = find_resource( hmod, info, level, &res, TRUE );
        if (status == STATUS_SUCCESS) *dir = res;
    }
    __EXCEPT_PAGE_FAULT
    {
        return GetExceptionCode();
    }
    __ENDTRY;
    return status;
}


/**********************************************************************
 *	LdrFindResource_U  (NTDLL.@)
 */
NTSTATUS WINAPI DECLSPEC_HOTPATCH LdrFindResource_U( HMODULE hmod, const LDR_RESOURCE_INFO *info,
                                   ULONG level, const IMAGE_RESOURCE_DATA_ENTRY **entry )
{
    const void *res;
    NTSTATUS status;

    __TRY
    {
	if (info) TRACE( "module %p type %s name %s lang %04lx level %ld\n",
                     hmod, debugstr_w((LPCWSTR)info->Type),
                     level > 1 ? debugstr_w((LPCWSTR)info->Name) : "",
                     level > 2 ? info->Language : 0, level );

        status = find_resource( hmod, info, level, &res, FALSE );
        if (status == STATUS_SUCCESS) *entry = res;
    }
    __EXCEPT_PAGE_FAULT
    {
        return GetExceptionCode();
    }
    __ENDTRY;
    return status;
}


/**********************************************************************
 *	LdrFindResourceEx_U  (NTDLL.@)
 *
 * LdrFindResource_U with flags first (Windows 7's lpk.dll imports it); no flag
 * changes what is found.
 */
NTSTATUS WINAPI LdrFindResourceEx_U( ULONG flags, HMODULE hmod, const LDR_RESOURCE_INFO *info,
                                     ULONG level, const IMAGE_RESOURCE_DATA_ENTRY **entry )
{
    if (flags) FIXME( "flags %#lx ignored\n", flags );
    return LdrFindResource_U( hmod, info, level, entry );
}

/**********************************************************************
 *	LdrResFindResource  (NTDLL.@)
 *
 * A resource's data and size by type, name and language in one call
 * (Windows 7's gameux.dll imports it). The culture outputs and the flags
 * are not used: no MUI fallback beyond LdrFindResource_U's.
 */
NTSTATUS WINAPI LdrResFindResource( HMODULE hmod, ULONG_PTR type, ULONG_PTR name, ULONG_PTR lang,
                                    void **data, ULONG *size, void *culture, ULONG *culture_len,
                                    ULONG flags )
{
    const IMAGE_RESOURCE_DATA_ENTRY *entry;
    LDR_RESOURCE_INFO info;
    NTSTATUS status;
    ULONG len;
    void *ptr;

    info.Type = type;
    info.Name = name;
    info.Language = lang;
    if ((status = LdrFindResource_U( hmod, &info, 3, &entry ))) return status;
    if ((status = LdrAccessResource( hmod, entry, &ptr, &len ))) return status;
    if (data) *data = ptr;
    if (size) *size = len;
    if (culture_len) *culture_len = 0;
    return STATUS_SUCCESS;
}

/* don't penalize other platforms with stuff needed on i386 for compatibility */
#ifdef __i386__
NTSTATUS WINAPI access_resource( HMODULE hmod, const IMAGE_RESOURCE_DATA_ENTRY *entry,
                                 void **ptr, ULONG *size )
#else
static inline NTSTATUS access_resource( HMODULE hmod, const IMAGE_RESOURCE_DATA_ENTRY *entry,
                                        void **ptr, ULONG *size )
#endif
{
    NTSTATUS status;

    __TRY
    {
        ULONG dirsize;
        HMODULE mui = mui_module_from_entry( entry );

        if (mui) hmod = mui;
        if (!RtlImageDirectoryEntryToData( hmod, TRUE, IMAGE_DIRECTORY_ENTRY_RESOURCE, &dirsize ))
            status = STATUS_RESOURCE_DATA_NOT_FOUND;
        else
        {
            if (ptr)
            {
                BOOL is_data_file = is_data_file_module(hmod);
                hmod = (HMODULE)((ULONG_PTR)hmod & ~3);
                if (is_data_file)
                    *ptr = RtlImageRvaToVa( RtlImageNtHeader(hmod), hmod, entry->OffsetToData, NULL );
                else
                    *ptr = (char *)hmod + entry->OffsetToData;
            }
            if (size) *size = entry->Size;
            status = STATUS_SUCCESS;
        }
    }
    __EXCEPT_PAGE_FAULT
    {
        return GetExceptionCode();
    }
    __ENDTRY;
    return status;
}

/**********************************************************************
 *	LdrAccessResource  (NTDLL.@)
 *
 * NOTE
 * On x86, Shrinker, an executable compressor, depends on the
 * "call access_resource" instruction being there.
 */
#ifdef __i386__
__ASM_STDCALL_FUNC( LdrAccessResource, 16,
    "pushl %ebp\n\t"
    "movl %esp, %ebp\n\t"
    "subl $4,%esp\n\t"
    "pushl 24(%ebp)\n\t"
    "pushl 20(%ebp)\n\t"
    "pushl 16(%ebp)\n\t"
    "pushl 12(%ebp)\n\t"
    "pushl 8(%ebp)\n\t"
    "call " __ASM_STDCALL("access_resource",16) "\n\t"
    "leave\n\t"
    "ret $16"
)
#else
NTSTATUS WINAPI LdrAccessResource( HMODULE hmod, const IMAGE_RESOURCE_DATA_ENTRY *entry,
                                   void **ptr, ULONG *size )
{
    return access_resource( hmod, entry, ptr, size );
}
#endif

/**********************************************************************
 *	RtlFindMessage  (NTDLL.@)
 */
NTSTATUS WINAPI RtlFindMessage( HMODULE hmod, ULONG type, ULONG lang,
                                ULONG msg_id, const MESSAGE_RESOURCE_ENTRY **ret )
{
    const MESSAGE_RESOURCE_DATA *data;
    const MESSAGE_RESOURCE_BLOCK *block;
    const IMAGE_RESOURCE_DATA_ENTRY *rsrc;
    LDR_RESOURCE_INFO info;
    NTSTATUS status;
    void *ptr;
    unsigned int i;

    info.Type     = type;
    info.Name     = 1;
    info.Language = lang;

    if ((status = LdrFindResource_U( hmod, &info, 3, &rsrc )) != STATUS_SUCCESS)
        return status;
    if ((status = LdrAccessResource( hmod, rsrc, &ptr, NULL )) != STATUS_SUCCESS)
        return status;

    data = ptr;
    block = data->Blocks;
    for (i = 0; i < data->NumberOfBlocks; i++, block++)
    {
        if (msg_id >= block->LowId && msg_id <= block->HighId)
        {
            const MESSAGE_RESOURCE_ENTRY *entry;

            entry = (const MESSAGE_RESOURCE_ENTRY *)((const char *)data + block->OffsetToEntries);
            for (i = msg_id - block->LowId; i > 0; i--)
                entry = (const MESSAGE_RESOURCE_ENTRY *)((const char *)entry + entry->Length);
            *ret = entry;
            return STATUS_SUCCESS;
        }
    }
    return STATUS_MESSAGE_NOT_FOUND;
}
