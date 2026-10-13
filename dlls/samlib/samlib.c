/*
 * Security Account Manager client library
 *
 * Copyright 2026 the Wine project authors (see the file AUTHORS)
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

/*
 * On Windows these calls go to the SAM server in lsass. Wine has no SAM
 * database: its accounts are the ones netapi32 and advapi32 already report --
 * the session's user (RID 1000, as in the wineserver's token), and the
 * built-in Administrator (500) and Guest (501), both disabled -- in the
 * account domain S-1-5-21-0-0-0 named after the computer, with the built-in
 * domain's Administrators, Users and Guests aliases. This serves that
 * database, read only. Layouts follow [MS-SAMR] and the Windows SDK's
 * ntsam.h, which native callers (the shell's shacct.dll) are built against.
 */

#include <stdarg.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#include "lmcons.h"
#include "wine/exception.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(samlib);

static const char *debugstr_us( const UNICODE_STRING *us )
{
    if (!us) return "(null)";
    return debugstr_wn( us->Buffer, us->Length / sizeof(WCHAR) );
}

typedef struct { USHORT UnitsPerWeek; UCHAR *LogonHours; } LOGON_HOURS;
typedef struct { ULONG Length; UCHAR *SecurityDescriptor; } SR_SECURITY_DESCRIPTOR;

typedef struct { ULONG RelativeId; UNICODE_STRING Name; } SAM_RID_ENUMERATION;

enum { SidTypeUser_ = 1, SidTypeAlias_ = 4, SidTypeUnknown_ = 8 };

/* USER_INFORMATION_CLASS */
enum
{
    UserGeneralInformation = 1, UserPreferencesInformation, UserLogonInformation,
    UserLogonHoursInformation, UserAccountInformation, UserNameInformation,
    UserAccountNameInformation, UserFullNameInformation, UserPrimaryGroupInformation,
    UserHomeInformation, UserScriptInformation, UserProfileInformation,
    UserAdminCommentInformation, UserWorkStationsInformation, UserSetPasswordInformation,
    UserControlInformation, UserExpiresInformation, UserInternal1Information,
    UserInternal2Information, UserParametersInformation, UserAllInformation,
    UserExtendedInformation = 28, UserLogonUIInformation = 29,
};

/* ALIAS_INFORMATION_CLASS */
enum { AliasGeneralInformation = 1, AliasNameInformation, AliasAdminCommentInformation };

/* user account control bits */
#define USER_ACCOUNT_DISABLED       0x00000001
#define USER_PASSWORD_NOT_REQUIRED  0x00000004
#define USER_NORMAL_ACCOUNT         0x00000010
#define USER_DONT_EXPIRE_PASSWORD   0x00000200

typedef struct
{
    UNICODE_STRING UserName, FullName;
    ULONG PrimaryGroupId;
    UNICODE_STRING AdminComment, UserComment;
} USER_GENERAL_INFORMATION;

typedef struct
{
    UNICODE_STRING UserComment, Reserved1;
    USHORT CountryCode, CodePage;
} USER_PREFERENCES_INFORMATION;

#pragma pack(push,4)
typedef struct
{
    UNICODE_STRING UserName, FullName;
    ULONG UserId, PrimaryGroupId;
    UNICODE_STRING HomeDirectory, HomeDirectoryDrive, ScriptPath, ProfilePath, WorkStations;
    LARGE_INTEGER LastLogon, LastLogoff, PasswordLastSet, PasswordCanChange, PasswordMustChange;
    LOGON_HOURS LogonHours;
    USHORT BadPasswordCount, LogonCount;
    ULONG UserAccountControl;
} USER_LOGON_INFORMATION;

typedef struct
{
    UNICODE_STRING UserName, FullName;
    ULONG UserId, PrimaryGroupId;
    UNICODE_STRING HomeDirectory, HomeDirectoryDrive, ScriptPath, ProfilePath, AdminComment, WorkStations;
    LARGE_INTEGER LastLogon, LastLogoff;
    LOGON_HOURS LogonHours;
    USHORT BadPasswordCount, LogonCount;
    LARGE_INTEGER PasswordLastSet, AccountExpires;
    ULONG UserAccountControl;
} USER_ACCOUNT_INFORMATION;

typedef struct
{
    LARGE_INTEGER LastLogon, LastLogoff, PasswordLastSet, AccountExpires, PasswordCanChange, PasswordMustChange;
    UNICODE_STRING UserName, FullName, HomeDirectory, HomeDirectoryDrive, ScriptPath, ProfilePath,
                   AdminComment, WorkStations, UserComment, Parameters, LmPassword, NtPassword, PrivateData;
    SR_SECURITY_DESCRIPTOR SecurityDescriptor;
    ULONG UserId, PrimaryGroupId, UserAccountControl, WhichFields;
    LOGON_HOURS LogonHours;
    USHORT BadPasswordCount, LogonCount, CountryCode, CodePage;
    BOOLEAN LmPasswordPresent, NtPasswordPresent, PasswordExpired, PrivateDataSensitive;
} USER_ALL_INFORMATION;
#pragma pack(pop)

typedef struct { BOOLEAN PasswordIsBlank, AccountIsDisabled; } USER_LOGON_UI_INFORMATION;

typedef struct { UNICODE_STRING Name; ULONG MemberCount; UNICODE_STRING AdminComment; } ALIAS_GENERAL_INFORMATION;

/* the fields USER_ALL_INFORMATION carries for a caller that may read them all but the secrets */
#define USER_ALL_READ_GENERAL_MASK      0x0000003f
#define USER_ALL_READ_LOGON_MASK        0x0003ffc0
#define USER_ALL_READ_ACCOUNT_MASK      0x003c0000
#define USER_ALL_READ_PREFERENCES_MASK  0x00c00000

/* The database. */

struct account
{
    ULONG rid;
    const WCHAR *name;          /* NULL: the session's user */
    ULONG control;
    const WCHAR *comment;
};

static const struct account users[] =
{
    { 500, L"Administrator", USER_NORMAL_ACCOUNT | USER_DONT_EXPIRE_PASSWORD | USER_ACCOUNT_DISABLED,
      L"Built-in account for administering the computer/domain" },
    { 501, L"Guest", USER_NORMAL_ACCOUNT | USER_DONT_EXPIRE_PASSWORD | USER_ACCOUNT_DISABLED | USER_PASSWORD_NOT_REQUIRED,
      L"Built-in account for guest access to the computer/domain" },
    { 1000, NULL, USER_NORMAL_ACCOUNT | USER_DONT_EXPIRE_PASSWORD, L"" },
};

/* a member of an alias: a well-known SID (S-1-5-x), or a user of the account domain */
struct member { BOOL account; ULONG rid; };

struct alias
{
    ULONG rid;
    const WCHAR *name;
    const WCHAR *comment;
    struct member members[3];
    unsigned int count;
};

static const struct alias builtin_aliases[] =
{
    { DOMAIN_ALIAS_RID_ADMINS, L"Administrators",
      L"Administrators have complete and unrestricted access to the computer/domain",
      { { TRUE, 500 }, { TRUE, 1000 } }, 2 },
    { DOMAIN_ALIAS_RID_USERS, L"Users",
      L"Users are prevented from making accidental or intentional system-wide changes and can run most applications",
      { { FALSE, SECURITY_INTERACTIVE_RID }, { FALSE, SECURITY_AUTHENTICATED_USER_RID }, { TRUE, 1000 } }, 3 },
    { DOMAIN_ALIAS_RID_GUESTS, L"Guests",
      L"Guests have the same access as members of the Users group by default, except for the Guest account which is further restricted",
      { { TRUE, 501 } }, 1 },
};

enum domain { DOMAIN_ACCOUNT, DOMAIN_BUILTIN };

static const SID_IDENTIFIER_AUTHORITY nt_authority = { SECURITY_NT_AUTHORITY };

/* S-1-5-21-0-0-0, the domain of the user SID S-1-5-21-0-0-0-1000 the wineserver gives the session */
static void account_domain_sid( SID *sid )
{
    sid->Revision = SID_REVISION;
    sid->SubAuthorityCount = 4;
    sid->IdentifierAuthority = nt_authority;
    sid->SubAuthority[0] = SECURITY_NT_NON_UNIQUE;
    sid->SubAuthority[1] = sid->SubAuthority[2] = sid->SubAuthority[3] = 0;
}

static void builtin_domain_sid( SID *sid )
{
    sid->Revision = SID_REVISION;
    sid->SubAuthorityCount = 1;
    sid->IdentifierAuthority = nt_authority;
    sid->SubAuthority[0] = SECURITY_BUILTIN_DOMAIN_RID;
}

static const WCHAR *user_name( const struct account *a, WCHAR *buf )
{
    DWORD len = UNLEN + 1;
    if (a->name) return a->name;
    if (!GetUserNameW( buf, &len )) buf[0] = 0;
    return buf;
}

static const struct account *find_user( ULONG rid )
{
    unsigned int i;
    for (i = 0; i < ARRAY_SIZE(users); i++) if (users[i].rid == rid) return &users[i];
    return NULL;
}

static const struct alias *find_alias( enum domain domain, ULONG rid )
{
    unsigned int i;
    if (domain != DOMAIN_BUILTIN) return NULL;
    for (i = 0; i < ARRAY_SIZE(builtin_aliases); i++) if (builtin_aliases[i].rid == rid) return &builtin_aliases[i];
    return NULL;
}

/* Handles. */

enum object { OBJ_SERVER = 0x53414d31, OBJ_DOMAIN, OBJ_USER, OBJ_ALIAS };

struct handle
{
    enum object type;
    enum domain domain;
    ULONG rid;
};

static NTSTATUS new_handle( enum object type, enum domain domain, ULONG rid, void **ret )
{
    struct handle *h;
    if (!ret) return STATUS_INVALID_PARAMETER;
    if (!(h = HeapAlloc( GetProcessHeap(), 0, sizeof(*h) ))) return STATUS_INSUFFICIENT_RESOURCES;
    h->type = type;
    h->domain = domain;
    h->rid = rid;
    *ret = h;
    return STATUS_SUCCESS;
}

static struct handle *get_handle( void *handle, enum object type )
{
    struct handle *h = handle;
    if (!h) return NULL;
    __TRY
    {
        if (h->type != type) h = NULL;
    }
    __EXCEPT_PAGE_FAULT
    {
        h = NULL;
    }
    __ENDTRY
    return h;
}

/* Returned memory: one block per call, freed with SamFreeMemory. Strings
 * are packed after the structures that point to them. */

struct block
{
    BYTE *base, *strings;
    SIZE_T size;
};

static void *block_alloc( struct block *b, SIZE_T fixed, SIZE_T strings )
{
    fixed = (fixed + 7) & ~7;
    if (!(b->base = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY, fixed + strings ))) return NULL;
    b->strings = b->base + fixed;
    b->size = fixed + strings;
    return b->base;
}

static void block_string( struct block *b, UNICODE_STRING *us, const WCHAR *s )
{
    SIZE_T len = s ? wcslen( s ) * sizeof(WCHAR) : 0;
    us->Length = len;
    us->MaximumLength = len + sizeof(WCHAR);
    us->Buffer = (WCHAR *)b->strings;
    if (len) memcpy( b->strings, s, len );
    ((WCHAR *)b->strings)[len / sizeof(WCHAR)] = 0;
    b->strings += len + sizeof(WCHAR);
}

static SIZE_T string_size( const WCHAR *s )
{
    return ((s ? wcslen( s ) : 0) + 1) * sizeof(WCHAR);
}

/* never, as an unsigned 64-bit time */
static void never( LARGE_INTEGER *t )
{
    t->QuadPart = 0x7fffffffffffffff;
}

static NTSTATUS user_info( const struct account *a, ULONG class, void **ret )
{
    WCHAR namebuf[UNLEN + 1], profile[MAX_PATH] = L"";
    const WCHAR *name = user_name( a, namebuf );
    struct block b;
    DWORD len;

    if (!a->name && (len = GetEnvironmentVariableW( L"USERPROFILE", profile, ARRAY_SIZE(profile) )) >= ARRAY_SIZE(profile))
        profile[0] = 0;

    switch (class)
    {
    case UserGeneralInformation:
    {
        USER_GENERAL_INFORMATION *i;
        if (!(i = block_alloc( &b, sizeof(*i), string_size( name ) + 2 * string_size( NULL ) + string_size( a->comment ) )))
            return STATUS_INSUFFICIENT_RESOURCES;
        block_string( &b, &i->UserName, name );
        block_string( &b, &i->FullName, NULL );
        i->PrimaryGroupId = DOMAIN_GROUP_RID_USERS;
        block_string( &b, &i->AdminComment, a->comment );
        block_string( &b, &i->UserComment, NULL );
        break;
    }
    case UserPreferencesInformation:
    {
        USER_PREFERENCES_INFORMATION *i;
        if (!(i = block_alloc( &b, sizeof(*i), 2 * string_size( NULL ) ))) return STATUS_INSUFFICIENT_RESOURCES;
        block_string( &b, &i->UserComment, NULL );
        block_string( &b, &i->Reserved1, NULL );
        break;
    }
    case UserLogonInformation:
    {
        USER_LOGON_INFORMATION *i;
        if (!(i = block_alloc( &b, sizeof(*i), string_size( name ) + string_size( profile ) + 5 * string_size( NULL ) )))
            return STATUS_INSUFFICIENT_RESOURCES;
        block_string( &b, &i->UserName, name );
        block_string( &b, &i->FullName, NULL );
        i->UserId = a->rid;
        i->PrimaryGroupId = DOMAIN_GROUP_RID_USERS;
        block_string( &b, &i->HomeDirectory, NULL );
        block_string( &b, &i->HomeDirectoryDrive, NULL );
        block_string( &b, &i->ScriptPath, NULL );
        block_string( &b, &i->ProfilePath, profile );
        block_string( &b, &i->WorkStations, NULL );
        never( &i->PasswordMustChange );
        i->UserAccountControl = a->control;
        break;
    }
    case UserAccountInformation:
    {
        USER_ACCOUNT_INFORMATION *i;
        if (!(i = block_alloc( &b, sizeof(*i), string_size( name ) + string_size( profile ) + string_size( a->comment )
                               + 5 * string_size( NULL ) )))
            return STATUS_INSUFFICIENT_RESOURCES;
        block_string( &b, &i->UserName, name );
        block_string( &b, &i->FullName, NULL );
        i->UserId = a->rid;
        i->PrimaryGroupId = DOMAIN_GROUP_RID_USERS;
        block_string( &b, &i->HomeDirectory, NULL );
        block_string( &b, &i->HomeDirectoryDrive, NULL );
        block_string( &b, &i->ScriptPath, NULL );
        block_string( &b, &i->ProfilePath, profile );
        block_string( &b, &i->AdminComment, a->comment );
        block_string( &b, &i->WorkStations, NULL );
        never( &i->AccountExpires );
        i->UserAccountControl = a->control;
        break;
    }
    case UserNameInformation:
    {
        struct { UNICODE_STRING UserName, FullName; } *i;
        if (!(i = block_alloc( &b, sizeof(*i), string_size( name ) + string_size( NULL ) ))) return STATUS_INSUFFICIENT_RESOURCES;
        block_string( &b, &i->UserName, name );
        block_string( &b, &i->FullName, NULL );
        break;
    }
    case UserAccountNameInformation:
    case UserFullNameInformation:
    case UserHomeInformation:
    case UserScriptInformation:
    case UserProfileInformation:
    case UserAdminCommentInformation:
    case UserWorkStationsInformation:
    case UserParametersInformation:
    {
        /* one string (two for the home: directory and drive) */
        const WCHAR *s = class == UserAccountNameInformation ? name : class == UserProfileInformation ? profile
                         : class == UserAdminCommentInformation ? a->comment : NULL;
        UNICODE_STRING *i;
        if (!(i = block_alloc( &b, 2 * sizeof(*i), string_size( s ) + string_size( NULL ) ))) return STATUS_INSUFFICIENT_RESOURCES;
        block_string( &b, &i[0], s );
        if (class == UserHomeInformation) block_string( &b, &i[1], NULL );
        break;
    }
    case UserPrimaryGroupInformation:
    case UserControlInformation:
    {
        ULONG *i;
        if (!(i = block_alloc( &b, sizeof(*i), 0 ))) return STATUS_INSUFFICIENT_RESOURCES;
        *i = class == UserControlInformation ? a->control : DOMAIN_GROUP_RID_USERS;
        break;
    }
    case UserExpiresInformation:
    {
        LARGE_INTEGER *i;
        if (!(i = block_alloc( &b, sizeof(*i), 0 ))) return STATUS_INSUFFICIENT_RESOURCES;
        never( i );
        break;
    }
    case UserLogonHoursInformation:
    {
        LOGON_HOURS *i;
        if (!(i = block_alloc( &b, sizeof(*i), 0 ))) return STATUS_INSUFFICIENT_RESOURCES;
        break;
    }
    case UserAllInformation:
    {
        USER_ALL_INFORMATION *i;
        if (!(i = block_alloc( &b, sizeof(*i), string_size( name ) + string_size( profile ) + string_size( a->comment )
                               + 10 * string_size( NULL ) )))
            return STATUS_INSUFFICIENT_RESOURCES;
        never( &i->AccountExpires );
        never( &i->PasswordMustChange );
        block_string( &b, &i->UserName, name );
        block_string( &b, &i->FullName, NULL );
        block_string( &b, &i->HomeDirectory, NULL );
        block_string( &b, &i->HomeDirectoryDrive, NULL );
        block_string( &b, &i->ScriptPath, NULL );
        block_string( &b, &i->ProfilePath, profile );
        block_string( &b, &i->AdminComment, a->comment );
        block_string( &b, &i->WorkStations, NULL );
        block_string( &b, &i->UserComment, NULL );
        block_string( &b, &i->Parameters, NULL );
        i->UserId = a->rid;
        i->PrimaryGroupId = DOMAIN_GROUP_RID_USERS;
        i->UserAccountControl = a->control;
        i->WhichFields = USER_ALL_READ_GENERAL_MASK | USER_ALL_READ_LOGON_MASK | USER_ALL_READ_ACCOUNT_MASK
                         | USER_ALL_READ_PREFERENCES_MASK;
        break;
    }
    case UserExtendedInformation:
    {
        /* ExtendedWhichFields first, then the user tile (the account
         * picture), the password hint, whether logon UI hides the account,
         * and the shell's properties: none of them is set for Wine's
         * accounts, which is ExtendedWhichFields 0 and the rest zero. Sized
         * past the structure, so a reader of any of its fields reads zero. */
        if (!block_alloc( &b, 64, 0 )) return STATUS_INSUFFICIENT_RESOURCES;
        break;
    }
    case UserLogonUIInformation:
    {
        /* Wine's accounts have no passwords: logging on asks for none */
        USER_LOGON_UI_INFORMATION *i;
        if (!(i = block_alloc( &b, sizeof(*i), 0 ))) return STATUS_INSUFFICIENT_RESOURCES;
        i->PasswordIsBlank = TRUE;
        i->AccountIsDisabled = !!(a->control & USER_ACCOUNT_DISABLED);
        break;
    }
    default:
        FIXME( "information class %lu not supported\n", class );
        return STATUS_INVALID_INFO_CLASS;
    }
    *ret = b.base;
    return STATUS_SUCCESS;
}

/***********************************************************************
 *              SamConnect (SAMLIB.@)
 */
NTSTATUS WINAPI SamConnect( UNICODE_STRING *server, void **ret, ACCESS_MASK access, OBJECT_ATTRIBUTES *attr )
{
    TRACE( "(%s, %p, %#lx, %p)\n", debugstr_us( server ), ret, access, attr );
    if (server && server->Length)
    {
        WCHAR name[MAX_COMPUTERNAME_LENGTH + 1];
        DWORD len = ARRAY_SIZE(name);
        const WCHAR *s = server->Buffer;
        USHORT n = server->Length / sizeof(WCHAR);
        while (n && *s == '\\') { s++; n--; }
        if (!GetComputerNameW( name, &len ) || n != len || wcsnicmp( s, name, n ))
            return RPC_NT_SERVER_UNAVAILABLE;
    }
    return new_handle( OBJ_SERVER, DOMAIN_ACCOUNT, 0, ret );
}

/***********************************************************************
 *              SamCloseHandle (SAMLIB.@)
 */
NTSTATUS WINAPI SamCloseHandle( void *handle )
{
    struct handle *h = handle;
    TRACE( "(%p)\n", handle );
    if (!get_handle( h, OBJ_SERVER ) && !get_handle( h, OBJ_DOMAIN ) && !get_handle( h, OBJ_USER )
        && !get_handle( h, OBJ_ALIAS ))
        return STATUS_INVALID_HANDLE;
    h->type = 0;
    HeapFree( GetProcessHeap(), 0, h );
    return STATUS_SUCCESS;
}

/***********************************************************************
 *              SamFreeMemory (SAMLIB.@)
 */
NTSTATUS WINAPI SamFreeMemory( void *buffer )
{
    TRACE( "(%p)\n", buffer );
    if (buffer) HeapFree( GetProcessHeap(), 0, buffer );
    return STATUS_SUCCESS;
}

/***********************************************************************
 *              SamLookupDomainInSamServer (SAMLIB.@)
 */
NTSTATUS WINAPI SamLookupDomainInSamServer( void *server, UNICODE_STRING *name, PSID *ret )
{
    WCHAR computer[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD len = ARRAY_SIZE(computer);
    SID *sid;

    TRACE( "(%p, %s, %p)\n", server, debugstr_us( name ), ret );
    if (!get_handle( server, OBJ_SERVER )) return STATUS_INVALID_HANDLE;
    if (!name || !ret) return STATUS_INVALID_PARAMETER;
    if (!(sid = HeapAlloc( GetProcessHeap(), 0, GetSidLengthRequired( 4 ) ))) return STATUS_INSUFFICIENT_RESOURCES;
    if (name->Length == 7 * sizeof(WCHAR) && !wcsnicmp( name->Buffer, L"Builtin", 7 ))
        builtin_domain_sid( sid );
    else if (GetComputerNameW( computer, &len ) && name->Length == len * sizeof(WCHAR)
             && !wcsnicmp( name->Buffer, computer, len ))
        account_domain_sid( sid );
    else
    {
        HeapFree( GetProcessHeap(), 0, sid );
        return STATUS_NO_SUCH_DOMAIN;
    }
    *ret = sid;
    return STATUS_SUCCESS;
}

/***********************************************************************
 *              SamOpenDomain (SAMLIB.@)
 */
NTSTATUS WINAPI SamOpenDomain( void *server, ACCESS_MASK access, PSID domain_sid, void **ret )
{
    BYTE buf[SECURITY_MAX_SID_SIZE];
    SID *sid = (SID *)buf;

    TRACE( "(%p, %#lx, %p, %p)\n", server, access, domain_sid, ret );
    if (!get_handle( server, OBJ_SERVER )) return STATUS_INVALID_HANDLE;
    if (!domain_sid || !IsValidSid( domain_sid )) return STATUS_INVALID_PARAMETER;
    account_domain_sid( sid );
    if (EqualSid( sid, domain_sid )) return new_handle( OBJ_DOMAIN, DOMAIN_ACCOUNT, 0, ret );
    builtin_domain_sid( sid );
    if (EqualSid( sid, domain_sid )) return new_handle( OBJ_DOMAIN, DOMAIN_BUILTIN, 0, ret );
    return STATUS_NO_SUCH_DOMAIN;
}

/***********************************************************************
 *              SamRidToSid (SAMLIB.@)
 */
NTSTATUS WINAPI SamRidToSid( void *handle, ULONG rid, PSID *ret )
{
    struct handle *h;
    SID *sid;

    TRACE( "(%p, %lu, %p)\n", handle, rid, ret );
    if (!(h = get_handle( handle, OBJ_DOMAIN )) && !(h = get_handle( handle, OBJ_USER ))
        && !(h = get_handle( handle, OBJ_ALIAS )))
        return STATUS_INVALID_HANDLE;
    if (!ret) return STATUS_INVALID_PARAMETER;
    if (!(sid = HeapAlloc( GetProcessHeap(), 0, GetSidLengthRequired( 5 ) ))) return STATUS_INSUFFICIENT_RESOURCES;
    if (h->domain == DOMAIN_BUILTIN) builtin_domain_sid( sid );
    else account_domain_sid( sid );
    sid->SubAuthority[sid->SubAuthorityCount++] = rid;
    *ret = sid;
    return STATUS_SUCCESS;
}

/* the name of an object of a domain, and its use; FALSE if there is none */
static BOOL name_of( enum domain domain, ULONG rid, const WCHAR **name, WCHAR *buf, ULONG *use )
{
    const struct account *a;
    const struct alias *al;

    if (domain == DOMAIN_ACCOUNT && (a = find_user( rid )))
    {
        *name = user_name( a, buf );
        *use = SidTypeUser_;
        return TRUE;
    }
    if ((al = find_alias( domain, rid )))
    {
        *name = al->name;
        *use = SidTypeAlias_;
        return TRUE;
    }
    return FALSE;
}

/***********************************************************************
 *              SamLookupNamesInDomain (SAMLIB.@)
 */
NTSTATUS WINAPI SamLookupNamesInDomain( void *handle, ULONG count, UNICODE_STRING *names, ULONG **rids, ULONG **uses )
{
    static const ULONG all_rids[] = { 500, 501, 1000, DOMAIN_ALIAS_RID_ADMINS, DOMAIN_ALIAS_RID_USERS, DOMAIN_ALIAS_RID_GUESTS };
    struct handle *h;
    ULONG i, j, found = 0;

    TRACE( "(%p, %lu, %p, %p, %p)\n", handle, count, names, rids, uses );
    if (!(h = get_handle( handle, OBJ_DOMAIN ))) return STATUS_INVALID_HANDLE;
    if (!names || !rids || !uses) return STATUS_INVALID_PARAMETER;
    *rids = *uses = NULL;
    if (!count) return STATUS_SUCCESS;
    if (count > 1000) return STATUS_INSUFFICIENT_RESOURCES;
    if (!(*rids = HeapAlloc( GetProcessHeap(), 0, count * sizeof(ULONG) ))
        || !(*uses = HeapAlloc( GetProcessHeap(), 0, count * sizeof(ULONG) )))
    {
        HeapFree( GetProcessHeap(), 0, *rids );
        *rids = NULL;
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    for (i = 0; i < count; i++)
    {
        (*rids)[i] = 0;
        (*uses)[i] = SidTypeUnknown_;
        for (j = 0; j < ARRAY_SIZE(all_rids); j++)
        {
            WCHAR buf[UNLEN + 1];
            const WCHAR *name;
            ULONG use;
            if (!name_of( h->domain, all_rids[j], &name, buf, &use )) continue;
            if (names[i].Length != wcslen( name ) * sizeof(WCHAR)
                || wcsnicmp( names[i].Buffer, name, names[i].Length / sizeof(WCHAR) )) continue;
            TRACE( "%s -> %lu\n", debugstr_us( &names[i] ), all_rids[j] );
            (*rids)[i] = all_rids[j];
            (*uses)[i] = use;
            found++;
            break;
        }
    }
    if (found) return found == count ? STATUS_SUCCESS : STATUS_SOME_NOT_MAPPED;
    HeapFree( GetProcessHeap(), 0, *rids );
    HeapFree( GetProcessHeap(), 0, *uses );
    *rids = *uses = NULL;
    return STATUS_NONE_MAPPED;
}

/***********************************************************************
 *              SamLookupIdsInDomain (SAMLIB.@)
 */
NTSTATUS WINAPI SamLookupIdsInDomain( void *handle, ULONG count, ULONG *rids, UNICODE_STRING **names, ULONG **uses )
{
    WCHAR buf[UNLEN + 1];
    const WCHAR *name;
    struct handle *h;
    struct block b;
    SIZE_T size = 0;
    ULONG i, use, found = 0;
    UNICODE_STRING *out;

    TRACE( "(%p, %lu, %p, %p, %p)\n", handle, count, rids, names, uses );
    if (!(h = get_handle( handle, OBJ_DOMAIN ))) return STATUS_INVALID_HANDLE;
    if (!rids || !names) return STATUS_INVALID_PARAMETER;
    *names = NULL;
    if (uses) *uses = NULL;
    if (!count) return STATUS_SUCCESS;
    if (count > 1000) return STATUS_INSUFFICIENT_RESOURCES;
    for (i = 0; i < count; i++)
        size += name_of( h->domain, rids[i], &name, buf, &use ) ? string_size( name ) : string_size( NULL );
    if (!(out = block_alloc( &b, count * sizeof(*out), size ))) return STATUS_INSUFFICIENT_RESOURCES;
    if (uses && !(*uses = HeapAlloc( GetProcessHeap(), 0, count * sizeof(ULONG) )))
    {
        HeapFree( GetProcessHeap(), 0, out );
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    for (i = 0; i < count; i++)
    {
        if (name_of( h->domain, rids[i], &name, buf, &use )) found++;
        else
        {
            name = NULL;
            use = SidTypeUnknown_;
        }
        block_string( &b, &out[i], name );
        if (!name) out[i].MaximumLength = 0;
        if (uses) (*uses)[i] = use;
    }
    if (!found)
    {
        HeapFree( GetProcessHeap(), 0, out );
        if (uses) HeapFree( GetProcessHeap(), 0, *uses ), *uses = NULL;
        return STATUS_NONE_MAPPED;
    }
    *names = out;
    return found == count ? STATUS_SUCCESS : STATUS_SOME_NOT_MAPPED;
}

/***********************************************************************
 *              SamOpenUser (SAMLIB.@)
 */
NTSTATUS WINAPI SamOpenUser( void *domain, ACCESS_MASK access, ULONG rid, void **ret )
{
    struct handle *h;

    TRACE( "(%p, %#lx, %lu, %p)\n", domain, access, rid, ret );
    if (!(h = get_handle( domain, OBJ_DOMAIN ))) return STATUS_INVALID_HANDLE;
    if (h->domain != DOMAIN_ACCOUNT || !find_user( rid )) return STATUS_NO_SUCH_USER;
    return new_handle( OBJ_USER, DOMAIN_ACCOUNT, rid, ret );
}

/***********************************************************************
 *              SamQueryInformationUser (SAMLIB.@)
 */
NTSTATUS WINAPI SamQueryInformationUser( void *user, ULONG class, void **buffer )
{
    struct handle *h;

    TRACE( "(%p, %lu, %p)\n", user, class, buffer );
    if (!(h = get_handle( user, OBJ_USER ))) return STATUS_INVALID_HANDLE;
    if (!buffer) return STATUS_INVALID_PARAMETER;
    return user_info( find_user( h->rid ), class, buffer );
}

/* The accounts or aliases of a domain, from *context on. */
static NTSTATUS enumerate( struct handle *h, BOOL aliases, ULONG filter, ULONG *context, void **buffer,
                           ULONG max_length, ULONG *count )
{
    struct { ULONG rid; const WCHAR *name; } list[ARRAY_SIZE(users) + ARRAY_SIZE(builtin_aliases)];
    WCHAR buf[UNLEN + 1];
    SAM_RID_ENUMERATION *out;
    struct block b;
    SIZE_T size = 0;
    ULONG i, n = 0;

    if (!context || !buffer || !count) return STATUS_INVALID_PARAMETER;
    *buffer = NULL;
    *count = 0;
    if (aliases)
    {
        if (h->domain == DOMAIN_BUILTIN)
            for (i = 0; i < ARRAY_SIZE(builtin_aliases); i++)
            {
                list[n].rid = builtin_aliases[i].rid;
                list[n++].name = builtin_aliases[i].name;
            }
    }
    else if (h->domain == DOMAIN_ACCOUNT)
    {
        for (i = 0; i < ARRAY_SIZE(users); i++)
        {
            if (filter && !(users[i].control & filter)) continue;
            list[n].rid = users[i].rid;
            list[n++].name = users[i].name ? users[i].name : user_name( &users[i], buf );
        }
    }
    /* all at once: the list is tiny, whatever max_length asks */
    if (*context >= n) return STATUS_NO_MORE_ENTRIES;
    for (i = *context; i < n; i++) size += string_size( list[i].name );
    if (!(out = block_alloc( &b, (n - *context) * sizeof(*out), size ))) return STATUS_INSUFFICIENT_RESOURCES;
    for (i = *context; i < n; i++)
    {
        out[i - *context].RelativeId = list[i].rid;
        block_string( &b, &out[i - *context].Name, list[i].name );
    }
    *count = n - *context;
    *context = n;
    *buffer = out;
    return STATUS_SUCCESS;
}

/***********************************************************************
 *              SamEnumerateUsersInDomain (SAMLIB.@)
 */
NTSTATUS WINAPI SamEnumerateUsersInDomain( void *domain, ULONG *context, ULONG filter, void **buffer,
                                           ULONG max_length, ULONG *count )
{
    struct handle *h;

    TRACE( "(%p, %p, %#lx, %p, %lu, %p)\n", domain, context, filter, buffer, max_length, count );
    if (!(h = get_handle( domain, OBJ_DOMAIN ))) return STATUS_INVALID_HANDLE;
    return enumerate( h, FALSE, filter, context, buffer, max_length, count );
}

/***********************************************************************
 *              SamEnumerateAliasesInDomain (SAMLIB.@)
 */
NTSTATUS WINAPI SamEnumerateAliasesInDomain( void *domain, ULONG *context, void **buffer, ULONG max_length,
                                             ULONG *count )
{
    struct handle *h;

    TRACE( "(%p, %p, %p, %lu, %p)\n", domain, context, buffer, max_length, count );
    if (!(h = get_handle( domain, OBJ_DOMAIN ))) return STATUS_INVALID_HANDLE;
    return enumerate( h, TRUE, 0, context, buffer, max_length, count );
}

/***********************************************************************
 *              SamOpenAlias (SAMLIB.@)
 */
NTSTATUS WINAPI SamOpenAlias( void *domain, ACCESS_MASK access, ULONG rid, void **ret )
{
    struct handle *h;

    TRACE( "(%p, %#lx, %lu, %p)\n", domain, access, rid, ret );
    if (!(h = get_handle( domain, OBJ_DOMAIN ))) return STATUS_INVALID_HANDLE;
    if (!find_alias( h->domain, rid )) return STATUS_NO_SUCH_ALIAS;
    return new_handle( OBJ_ALIAS, h->domain, rid, ret );
}

/***********************************************************************
 *              SamQueryInformationAlias (SAMLIB.@)
 */
NTSTATUS WINAPI SamQueryInformationAlias( void *alias, ULONG class, void **buffer )
{
    const struct alias *al;
    struct handle *h;
    struct block b;

    TRACE( "(%p, %lu, %p)\n", alias, class, buffer );
    if (!(h = get_handle( alias, OBJ_ALIAS ))) return STATUS_INVALID_HANDLE;
    if (!buffer) return STATUS_INVALID_PARAMETER;
    al = find_alias( h->domain, h->rid );
    switch (class)
    {
    case AliasGeneralInformation:
    {
        ALIAS_GENERAL_INFORMATION *i;
        if (!(i = block_alloc( &b, sizeof(*i), string_size( al->name ) + string_size( al->comment ) )))
            return STATUS_INSUFFICIENT_RESOURCES;
        block_string( &b, &i->Name, al->name );
        i->MemberCount = al->count;
        block_string( &b, &i->AdminComment, al->comment );
        break;
    }
    case AliasNameInformation:
    case AliasAdminCommentInformation:
    {
        UNICODE_STRING *i;
        const WCHAR *s = class == AliasNameInformation ? al->name : al->comment;
        if (!(i = block_alloc( &b, sizeof(*i), string_size( s ) ))) return STATUS_INSUFFICIENT_RESOURCES;
        block_string( &b, i, s );
        break;
    }
    default:
        FIXME( "information class %lu not supported\n", class );
        return STATUS_INVALID_INFO_CLASS;
    }
    *buffer = b.base;
    return STATUS_SUCCESS;
}

/***********************************************************************
 *              SamGetMembersInAlias (SAMLIB.@)
 */
NTSTATUS WINAPI SamGetMembersInAlias( void *alias, PSID **members, ULONG *count )
{
    const struct alias *al;
    struct handle *h;
    struct block b;
    PSID *out;
    ULONG i;

    TRACE( "(%p, %p, %p)\n", alias, members, count );
    if (!(h = get_handle( alias, OBJ_ALIAS ))) return STATUS_INVALID_HANDLE;
    if (!members || !count) return STATUS_INVALID_PARAMETER;
    al = find_alias( h->domain, h->rid );
    if (!(out = block_alloc( &b, al->count * sizeof(*out), al->count * GetSidLengthRequired( 5 ) )))
        return STATUS_INSUFFICIENT_RESOURCES;
    for (i = 0; i < al->count; i++)
    {
        SID *sid = (SID *)b.strings;
        if (al->members[i].account)
        {
            account_domain_sid( sid );
            sid->SubAuthority[sid->SubAuthorityCount++] = al->members[i].rid;
        }
        else
        {
            sid->Revision = SID_REVISION;
            sid->SubAuthorityCount = 1;
            sid->IdentifierAuthority = nt_authority;
            sid->SubAuthority[0] = al->members[i].rid;
        }
        out[i] = sid;
        b.strings += GetSidLengthRequired( 5 );
    }
    *members = out;
    *count = al->count;
    return STATUS_SUCCESS;
}

/***********************************************************************
 *              SamRegisterObjectChangeNotification (SAMLIB.@)
 *
 * The database never changes, so the event is never set.
 */
NTSTATUS WINAPI SamRegisterObjectChangeNotification( ULONG type, HANDLE event )
{
    TRACE( "(%lu, %p)\n", type, event );
    return STATUS_SUCCESS;
}

/***********************************************************************
 *              SamUnregisterObjectChangeNotification (SAMLIB.@)
 */
NTSTATUS WINAPI SamUnregisterObjectChangeNotification( ULONG type, HANDLE event )
{
    TRACE( "(%lu, %p)\n", type, event );
    return STATUS_SUCCESS;
}
