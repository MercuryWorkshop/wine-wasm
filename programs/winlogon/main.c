/*
 * winlogon: the session's logon process, as far as Windows components need one
 *
 * Windows' winlogon.exe cannot run here (it needs LSA, smss, LogonUI and a
 * real logon). What Windows components need from it is its part in their
 * session and logon sequence, which is done here the way winlogon does it:
 *
 * - The Windows 7 theme client, UXInit.dll: ThemesOnWinlogonSetup and
 *   ThemesOnCreateSession at session start, then ThemesOnLogon(0) and
 *   ThemesOnLogon(1) around the logon, impersonating the user. The Themes
 *   service (themeservice.dll) then injects threads into this process to
 *   install the theme hooks (RegisterUserApiHook), which it does only from a
 *   process in the SYSTEM logon session: this process becomes a system
 *   process once it has a copy of the user's token.
 * - Starting the user's shell, with the user's token (the command line, if
 *   any, or nothing), once the theme is loaded, as Windows' winlogon starts
 *   it only after ThemesOnLogon: the shell's controls then open their theme
 *   data from their first paint. The shell's process is created first and
 *   runs once the theme is loaded, since a session whose only process is a
 *   system process ends.
 *
 * It then stays until the session ends (wineserver's shutdown event).
 *
 * Copyright 2026 the gex project
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
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>
#include <winternl.h>

#define ProcessWineMakeProcessSystem 1000

typedef NTSTATUS (WINAPI *set_info_fn)( HANDLE, PROCESSINFOCLASS, void *, ULONG );

/* UXInit.dll's exports, by ordinal (it has no names) */
#define UXINIT_THEMES_ON_CREATE_SESSION  7
#define UXINIT_THEMES_ON_TERMINATE_SESSION 8
#define UXINIT_THEMES_ON_LOGON           9
#define UXINIT_THEMES_ON_LOGOFF          10
#define UXINIT_THEMES_ON_WINLOGON_SETUP  14

static BOOL verbose;

static void trace( const char *fmt, ... )
{
    va_list args;
    if (!verbose) return;
    va_start( args, fmt );
    fprintf( stderr, "winlogon: " );
    vfprintf( stderr, fmt, args );
    va_end( args );
}

static void enable_privilege( HANDLE token, const WCHAR *name )
{
    TOKEN_PRIVILEGES privs;

    privs.PrivilegeCount = 1;
    privs.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    if (!LookupPrivilegeValueW( NULL, name, &privs.Privileges[0].Luid ) ||
        !AdjustTokenPrivileges( token, FALSE, &privs, 0, NULL, NULL ) || GetLastError())
        trace( "could not enable %ls: %lu\n", name, GetLastError() );
}

/* How long the service manager gives a service to start: ServicesPipeTimeout,
 * read as services.exe reads it. */
static DWORD service_start_timeout(void)
{
    WCHAR buffer[16];
    DWORD size = sizeof(buffer), type, value = 10000;
    HKEY key;

    if (RegOpenKeyExW( HKEY_LOCAL_MACHINE, L"System\\CurrentControlSet\\Control", 0, KEY_READ, &key )) return value;
    if (!RegQueryValueExW( key, L"ServicesPipeTimeout", NULL, &type, (BYTE *)buffer, &size ))
    {
        if (type == REG_SZ && wcstoul( buffer, NULL, 10 )) value = wcstoul( buffer, NULL, 10 );
        else if (type == REG_DWORD && *(DWORD *)buffer) value = *(DWORD *)buffer;
    }
    RegCloseKey( key );
    return value;
}

/* the user's shell: what follows "winlogon.exe" on the command line, if anything */
/* The Themes service, when it starts by itself: the theme logon below talks to
 * it, and without it the session's theme is Windows Classic. The service
 * manager starts it beside the logon (its auto-start services run after the
 * session's first process goes on), one service after another, and gives each
 * the service start timeout; so wait for it as long as the service manager
 * would: the timeout while it waits its turn, and again once it is starting. A
 * fixed 30 s gave up on a cold first visit, where the service's DLL comes late,
 * and the session came up in Windows Classic. */
static void wait_for_themes(void)
{
    SC_HANDLE scm, svc;
    QUERY_SERVICE_CONFIGW *config;
    SERVICE_STATUS status;
    DWORD size = 0, start = GetTickCount(), since = start, state = 0, timeout = service_start_timeout();
    BOOL automatic = FALSE;

    if (!(scm = OpenSCManagerW( NULL, NULL, SC_MANAGER_CONNECT ))) return;
    if (!(svc = OpenServiceW( scm, L"Themes", SERVICE_QUERY_STATUS | SERVICE_QUERY_CONFIG )))
    {
        CloseServiceHandle( scm );
        return;
    }
    QueryServiceConfigW( svc, NULL, 0, &size );
    if (size && (config = malloc( size )))
    {
        if (QueryServiceConfigW( svc, config, size, &size ))
            automatic = config->dwStartType <= SERVICE_AUTO_START;
        free( config );
    }
    while (automatic && QueryServiceStatus( svc, &status ) && status.dwCurrentState != SERVICE_RUNNING)
    {
        if (status.dwCurrentState != state)
        {
            state = status.dwCurrentState;
            since = GetTickCount();
        }
        if (GetTickCount() - since >= timeout) break;
        Sleep( 20 );
    }
    trace( "Themes service %s after %lu ms\n", automatic ? "waited for" : "not automatic", GetTickCount() - start );
    CloseServiceHandle( svc );
    CloseServiceHandle( scm );
}

static const WCHAR *shell_command_line(void)
{
    const WCHAR *p = GetCommandLineW();
    BOOL quoted = FALSE;

    for (; *p && (quoted || (*p != ' ' && *p != '\t')); p++) if (*p == '"') quoted = !quoted;
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

int main( void )
{
    typedef void (WINAPI *void_fn)(void);
    typedef BOOL (WINAPI *bool_fn)(void);
    typedef BOOL (WINAPI *logon_fn)(BOOL);
    HANDLE process_token, user_token = 0, shell_token = 0, shutdown = 0;
    set_info_fn set_info;
    HMODULE uxinit;
    HANDLE shell_thread = 0;
    const WCHAR *shell;
    NTSTATUS status;

    verbose = GetEnvironmentVariableW( L"WINLOGON_VERBOSE", NULL, 0 ) > 0;

    /* the user's token, before this process leaves the user's logon session */
    if (!OpenProcessToken( GetCurrentProcess(), TOKEN_ALL_ACCESS, &process_token ))
    {
        fprintf( stderr, "winlogon: no process token (%lu)\n", GetLastError() );
        return 1;
    }
    if (!DuplicateTokenEx( process_token, TOKEN_ALL_ACCESS, NULL, SecurityImpersonation,
                           TokenImpersonation, &user_token ) ||
        !DuplicateTokenEx( process_token, TOKEN_ALL_ACCESS, NULL, SecurityImpersonation,
                           TokenPrimary, &shell_token ))
        fprintf( stderr, "winlogon: could not copy the user's token (%lu)\n", GetLastError() );

    /* winlogon is the session's trusted computing base */
    enable_privilege( process_token, L"SeTcbPrivilege" );
    enable_privilege( process_token, L"SeDebugPrivilege" );

    if (*(shell = shell_command_line()))
    {
        STARTUPINFOW si = { sizeof(si) };
        PROCESS_INFORMATION pi;
        WCHAR *cmd = _wcsdup( shell );

        if (CreateProcessAsUserW( shell_token, NULL, cmd, NULL, NULL, FALSE, CREATE_SUSPENDED,
                                  NULL, NULL, &si, &pi ))
        {
            trace( "created %ls\n", cmd );
            shell_thread = pi.hThread;
            CloseHandle( pi.hProcess );
        }
        else fprintf( stderr, "winlogon: could not start %ls (%lu)\n", cmd, GetLastError() );
        free( cmd );
    }

    /* the session's desktop, before anything runs on it (Windows' winlogon
     * creates it before it starts the shell): a shell starting first raced
     * this process to start the desktop process, and the loser of two
     * desktop processes opened a browser window. The shell's process is
     * created (suspended, so nothing of it runs) before, so that its start
     * goes on beside the desktop's. */
    GetDesktopWindow();

    /* then leave the user's logon session: from here this is a system
     * process, which no longer keeps the session alive by itself */
    set_info = (set_info_fn)GetProcAddress( GetModuleHandleW( L"ntdll.dll" ), "NtSetInformationProcess" );
    if (set_info && (status = set_info( GetCurrentProcess(), ProcessWineMakeProcessSystem, &shutdown, sizeof(HANDLE *) )))
        fprintf( stderr, "winlogon: could not become a system process (%#lx)\n", status );

    wait_for_themes();
    if ((uxinit = LoadLibraryW( L"UXInit.dll" )))
    {
        void_fn setup = (void_fn)GetProcAddress( uxinit, MAKEINTRESOURCEA(UXINIT_THEMES_ON_WINLOGON_SETUP) );
        bool_fn create = (bool_fn)GetProcAddress( uxinit, MAKEINTRESOURCEA(UXINIT_THEMES_ON_CREATE_SESSION) );
        logon_fn logon = (logon_fn)GetProcAddress( uxinit, MAKEINTRESOURCEA(UXINIT_THEMES_ON_LOGON) );

        if (setup) setup();
        trace( "ThemesOnCreateSession -> %d (%lu)\n", create ? create() : -1, GetLastError() );
        if (logon && user_token && SetThreadToken( NULL, user_token ))
        {
            trace( "ThemesOnLogon(0) -> %d\n", logon( FALSE ));
            trace( "ThemesOnLogon(1) -> %d\n", logon( TRUE ));
            SetThreadToken( NULL, NULL );
        }
    }
    else trace( "no UXInit.dll (%lu)\n", GetLastError() );

    /* the theme is loaded: the shell runs */
    if (shell_thread)
    {
        trace( "starting the shell\n" );
        ResumeThread( shell_thread );
        CloseHandle( shell_thread );
    }

    /* threads the Themes service injects run here until the session ends */
    if (shutdown) WaitForSingleObject( shutdown, INFINITE );
    else Sleep( INFINITE );
    return 0;
}
