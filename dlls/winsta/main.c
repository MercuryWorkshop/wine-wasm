/*
 * Winstation library implementation
 *
 * Copyright 2011 Austin English
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

#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#include "wine/debug.h"
#include "winsta.h"

WINE_DEFAULT_DEBUG_CHANNEL(winsta);

BOOLEAN WINAPI WinStationQueryInformationA( HANDLE server, ULONG logon_id, WINSTATIONINFOCLASS class,
                                            void *info, ULONG len, ULONG *ret_len )
{
    FIXME( "%p %lu %u %p %lu %p\n", server, logon_id, class, info, len, ret_len );
    SetLastError( ERROR_CALL_NOT_IMPLEMENTED );
    return FALSE;
}

BOOLEAN WINAPI WinStationQueryInformationW( HANDLE server, ULONG logon_id, WINSTATIONINFOCLASS class,
                                            void *info, ULONG len, ULONG *ret_len )
{
    FIXME( "%p %lu %u %p %lu %p\n", server, logon_id, class, info, len, ret_len );
    SetLastError( ERROR_CALL_NOT_IMPLEMENTED );
    return FALSE;
}

BOOLEAN WINAPI WinStationRegisterConsoleNotification( HANDLE server, HWND hwnd, ULONG flags )
{
    FIXME( "%p %p 0x%lx\n", server, hwnd, flags );
    SetLastError( ERROR_CALL_NOT_IMPLEMENTED );
    return FALSE;
}

BOOLEAN WINAPI WinStationUnRegisterConsoleNotification( HANDLE server, HWND hwnd )
{
    FIXME( "%p %p\n", server, hwnd );
    SetLastError( ERROR_CALL_NOT_IMPLEMENTED );
    return FALSE;
}

BOOLEAN WINAPI WinStationGetAllProcesses( HANDLE server, ULONG level,
                                          ULONG *process_count, PTS_ALL_PROCESSES_INFO *info )
{
    FIXME( "%p %lu %p %p\n", server, level, process_count, info );
    *process_count = 0;
    SetLastError( ERROR_CALL_NOT_IMPLEMENTED );
    return FALSE;
}

BOOLEAN WINAPI WinStationGetProcessSid( HANDLE server, ULONG process_id, FILETIME process_start_time,
                                        PVOID process_user_sid, PULONG sid_size )
{
    FIXME( "(%p, %ld, %I64x, %p, %p): stub\n", server, process_id,
           ((UINT64)process_start_time.dwHighDateTime << 32) | process_start_time.dwLowDateTime,
           process_user_sid, sid_size);
    SetLastError( ERROR_CALL_NOT_IMPLEMENTED );
    return FALSE;
}

BOOL WINAPI WinStationVirtualOpen( PVOID a, PVOID b, PVOID c )
{
    FIXME( "%p %p %p\n", a, b, c );
    SetLastError( ERROR_CALL_NOT_IMPLEMENTED );
    return FALSE;
}

/* The local server: Wine has one session, the console, in which every
 * process runs. */
#define LOCAL_SERVER ((HANDLE)(ULONG_PTR)0x5752)

HANDLE WINAPI WinStationOpenServerW( const WCHAR *name )
{
    TRACE( "%s\n", debugstr_w(name) );
    if (name && *name)
    {
        FIXME( "remote server %s not supported\n", debugstr_w(name) );
        SetLastError( ERROR_CALL_NOT_IMPLEMENTED );
        return NULL;
    }
    return LOCAL_SERVER;
}

BOOLEAN WINAPI WinStationCloseServer( HANDLE server )
{
    TRACE( "%p\n", server );
    return TRUE;
}

BOOLEAN WINAPI WinStationFreeMemory( void *mem )
{
    TRACE( "%p\n", mem );
    HeapFree( GetProcessHeap(), 0, mem );
    return TRUE;
}

BOOLEAN WINAPI WinStationEnumerateW( HANDLE server, PSESSIONIDW *sessionids, ULONG *count )
{
    SESSIONIDW *session;

    TRACE( "%p %p %p\n", server, sessionids, count );
    if (server && server != LOCAL_SERVER)
    {
        SetLastError( ERROR_INVALID_HANDLE );
        return FALSE;
    }
    if (!(session = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*session) )))
    {
        SetLastError( ERROR_NOT_ENOUGH_MEMORY );
        return FALSE;
    }
    session->SessionId = NtCurrentTeb()->Peb->SessionId;
    lstrcpyW( session->WinStationName, L"Console" );
    session->State = State_Active;
    *sessionids = session;
    *count = 1;
    return TRUE;
}

/* Connection properties come from a remote desktop client: the console
 * session has none. */
BOOLEAN WINAPI WinStationGetConnectionProperty( ULONG session, const GUID *type, void **value )
{
    TRACE( "%ld %s %p\n", session, debugstr_guid(type), value );
    if (value) *value = NULL;
    SetLastError( ERROR_NOT_FOUND );
    return FALSE;
}

BOOLEAN WINAPI WinStationFreePropertyValue( void *value )
{
    TRACE( "%p\n", value );
    HeapFree( GetProcessHeap(), 0, value );
    return TRUE;
}
