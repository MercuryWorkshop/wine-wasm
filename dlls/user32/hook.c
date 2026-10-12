/*
 * Windows hook functions
 *
 * Copyright 2002 Alexandre Julliard
 * Copyright 2005 Dmitry Timoshkov
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
 *
 * NOTES:
 *   Status of the various hooks:
 *     WH_MSGFILTER                 OK
 *     WH_JOURNALRECORD             Partially implemented
 *     WH_JOURNALPLAYBACK           Partially implemented
 *     WH_KEYBOARD                  OK
 *     WH_GETMESSAGE	            OK (FIXME: A/W mapping?)
 *     WH_CALLWNDPROC	            OK (FIXME: A/W mapping?)
 *     WH_CBT
 *       HCBT_MOVESIZE              OK
 *       HCBT_MINMAX                OK
 *       HCBT_QS                    OK
 *       HCBT_CREATEWND             OK
 *       HCBT_DESTROYWND            OK
 *       HCBT_ACTIVATE              OK
 *       HCBT_CLICKSKIPPED          OK
 *       HCBT_KEYSKIPPED            OK
 *       HCBT_SYSCOMMAND            OK
 *       HCBT_SETFOCUS              OK
 *     WH_SYSMSGFILTER	            OK
 *     WH_MOUSE	                    OK
 *     WH_HARDWARE	            Not supported in Win32
 *     WH_DEBUG	                    Not implemented
 *     WH_SHELL
 *       HSHELL_WINDOWCREATED       OK
 *       HSHELL_WINDOWDESTROYED     OK
 *       HSHELL_ACTIVATESHELLWINDOW Not implemented
 *       HSHELL_WINDOWACTIVATED     Not implemented
 *       HSHELL_GETMINRECT          Not implemented
 *       HSHELL_REDRAW              Not implemented
 *       HSHELL_TASKMAN             Not implemented
 *       HSHELL_LANGUAGE            Not implemented
 *       HSHELL_SYSMENU             Not implemented
 *       HSHELL_ENDTASK             Not implemented
 *       HSHELL_ACCESSIBILITYSTATE  Not implemented
 *       HSHELL_APPCOMMAND          Not implemented
 *       HSHELL_WINDOWREPLACED      Not implemented
 *       HSHELL_WINDOWREPLACING     Not implemented
 *     WH_FOREGROUNDIDLE            Not implemented
 *     WH_CALLWNDPROCRET            OK (FIXME: A/W mapping?)
 *     WH_KEYBOARD_LL               Implemented but should use SendMessage instead
 *     WH_MOUSE_LL                  Implemented but should use SendMessage instead
 */

#include "ntstatus.h"
#include "user_private.h"
#include "wine/asm.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(hook);
WINE_DECLARE_DEBUG_CHANNEL(relay);

static struct user_api_hook original_user_api =
{
    USER_DefDlgProc,
    USER_NonClientButtonDraw,
    USER_ScrollBarDraw,
    USER_ScrollBarProc,
};
static struct user_api_hook hooked_user_api;
struct user_api_hook *user_api = &original_user_api;

struct hook_info
{
    INT id;
    void *proc;
    void *handle;
    DWORD pid, tid;
    BOOL prev_unicode, next_unicode;
    WCHAR module[MAX_PATH];
};

static CRITICAL_SECTION api_hook_cs;
static CRITICAL_SECTION_DEBUG critsect_debug =
{
    0, 0, &api_hook_cs,
    { &critsect_debug.ProcessLocksList, &critsect_debug.ProcessLocksList },
    0, 0, { (DWORD_PTR)(__FILE__ ": api_hook_cs") }
};
static CRITICAL_SECTION api_hook_cs = { &critsect_debug, -1, 0, 0, 0, 0 };


#define WH_WINEVENT (WH_MAXHOOK+1)

static const char * const hook_names[WH_WINEVENT - WH_MINHOOK + 1] =
{
    "WH_MSGFILTER",
    "WH_JOURNALRECORD",
    "WH_JOURNALPLAYBACK",
    "WH_KEYBOARD",
    "WH_GETMESSAGE",
    "WH_CALLWNDPROC",
    "WH_CBT",
    "WH_SYSMSGFILTER",
    "WH_MOUSE",
    "WH_HARDWARE",
    "WH_DEBUG",
    "WH_SHELL",
    "WH_FOREGROUNDIDLE",
    "WH_CALLWNDPROCRET",
    "WH_KEYBOARD_LL",
    "WH_MOUSE_LL",
    "WH_WINEVENT"
};


/***********************************************************************
 *		set_windows_hook
 *
 * Implementation of SetWindowsHookExA and SetWindowsHookExW.
 */
static HHOOK set_windows_hook( INT id, HOOKPROC proc, HINSTANCE inst, DWORD tid, BOOL ansi )
{
    WCHAR module[MAX_PATH];
    UNICODE_STRING str;

    if (!inst)
    {
        RtlInitUnicodeString( &str, NULL );
    }
    else
    {
        size_t len = GetModuleFileNameW( inst, module, ARRAYSIZE(module) );
        if (!len || len >= ARRAYSIZE(module))
        {
            SetLastError( ERROR_INVALID_PARAMETER );
            return 0;
        }
        str.Buffer = module;
        str.MaximumLength = str.Length = len * sizeof(WCHAR);
    }

    return NtUserSetWindowsHookEx( inst, &str, tid, id, proc, ansi );
}

#ifdef __i386__
/* Some apps pass a non-stdcall proc to SetWindowsHookExA,
 * so we need a small assembly wrapper to call the proc.
 */
extern LRESULT HOOKPROC_wrapper( HOOKPROC proc,
                                 INT code, WPARAM wParam, LPARAM lParam );
__ASM_GLOBAL_FUNC( HOOKPROC_wrapper,
                   "pushl %ebp\n\t"
                   __ASM_CFI(".cfi_adjust_cfa_offset 4\n\t")
                   __ASM_CFI(".cfi_rel_offset %ebp,0\n\t")
                   "movl %esp,%ebp\n\t"
                   __ASM_CFI(".cfi_def_cfa_register %ebp\n\t")
                   "pushl %edi\n\t"
                   __ASM_CFI(".cfi_rel_offset %edi,-4\n\t")
                   "pushl %esi\n\t"
                   __ASM_CFI(".cfi_rel_offset %esi,-8\n\t")
                   "pushl %ebx\n\t"
                   __ASM_CFI(".cfi_rel_offset %ebx,-12\n\t")
                   "pushl 20(%ebp)\n\t"
                   "pushl 16(%ebp)\n\t"
                   "pushl 12(%ebp)\n\t"
                   "movl 8(%ebp),%eax\n\t"
                   "call *%eax\n\t"
                   "leal -12(%ebp),%esp\n\t"
                   "popl %ebx\n\t"
                   __ASM_CFI(".cfi_same_value %ebx\n\t")
                   "popl %esi\n\t"
                   __ASM_CFI(".cfi_same_value %esi\n\t")
                   "popl %edi\n\t"
                   __ASM_CFI(".cfi_same_value %edi\n\t")
                   "leave\n\t"
                   __ASM_CFI(".cfi_def_cfa %esp,4\n\t")
                   __ASM_CFI(".cfi_same_value %ebp\n\t")
                   "ret" )
#else
static inline LRESULT HOOKPROC_wrapper( HOOKPROC proc,
                                 INT code, WPARAM wParam, LPARAM lParam )
{
    return proc( code, wParam, lParam );
}
#endif  /* __i386__ */


/***********************************************************************
 *		call_hook_AtoW
 */
static LRESULT call_hook_AtoW( HOOKPROC proc, INT id, INT code, WPARAM wparam, LPARAM lparam )
{
    LRESULT ret;
    UNICODE_STRING usBuffer;
    if (id != WH_CBT || code != HCBT_CREATEWND)
        ret = HOOKPROC_wrapper( proc, code, wparam, lparam );
    else
    {
        CBT_CREATEWNDA *cbtcwA = (CBT_CREATEWNDA *)lparam;
        CBT_CREATEWNDW cbtcwW;
        CREATESTRUCTW csW;
        LPWSTR nameW = NULL;
        LPWSTR classW = NULL;
        WCHAR name_buf[3];

        cbtcwW.lpcs = &csW;
        cbtcwW.hwndInsertAfter = cbtcwA->hwndInsertAfter;
        csW = *(CREATESTRUCTW *)cbtcwA->lpcs;

        if (!IS_INTRESOURCE(cbtcwA->lpcs->lpszName))
        {
            if (cbtcwA->lpcs->lpszName[0] != '\xff')
            {
                RtlCreateUnicodeStringFromAsciiz( &usBuffer, cbtcwA->lpcs->lpszName );
                csW.lpszName = nameW = usBuffer.Buffer;
            }
            else
            {
                name_buf[0] = 0xffff;
                name_buf[1] = MAKEWORD( cbtcwA->lpcs->lpszName[1], cbtcwA->lpcs->lpszName[2] );
                name_buf[2] = 0;
                csW.lpszName = name_buf;
            }
        }
        if (!IS_INTRESOURCE(cbtcwA->lpcs->lpszClass))
        {
            RtlCreateUnicodeStringFromAsciiz(&usBuffer,cbtcwA->lpcs->lpszClass);
            csW.lpszClass = classW = usBuffer.Buffer;
        }
        ret = HOOKPROC_wrapper( proc, code, wparam, (LPARAM)&cbtcwW );
        cbtcwA->hwndInsertAfter = cbtcwW.hwndInsertAfter;
        HeapFree( GetProcessHeap(), 0, nameW );
        HeapFree( GetProcessHeap(), 0, classW );
    }
    return ret;
}


/***********************************************************************
 *		call_hook_WtoA
 */
static LRESULT call_hook_WtoA( HOOKPROC proc, INT id, INT code, WPARAM wparam, LPARAM lparam )
{
    LRESULT ret;

    if (id != WH_CBT || code != HCBT_CREATEWND)
        ret = HOOKPROC_wrapper( proc, code, wparam, lparam );
    else
    {
        CBT_CREATEWNDW *cbtcwW = (CBT_CREATEWNDW *)lparam;
        CBT_CREATEWNDA cbtcwA;
        CREATESTRUCTA csA;
        int len;
        LPSTR nameA = NULL;
        LPSTR classA = NULL;
        char name_buf[4];

        cbtcwA.lpcs = &csA;
        cbtcwA.hwndInsertAfter = cbtcwW->hwndInsertAfter;
        csA = *(CREATESTRUCTA *)cbtcwW->lpcs;

        if (!IS_INTRESOURCE(cbtcwW->lpcs->lpszName))
        {
            if (cbtcwW->lpcs->lpszName[0] != 0xffff)
            {
                len = WideCharToMultiByte( CP_ACP, 0, cbtcwW->lpcs->lpszName, -1, NULL, 0, NULL, NULL );
                nameA = HeapAlloc( GetProcessHeap(), 0, len*sizeof(CHAR) );
                WideCharToMultiByte( CP_ACP, 0, cbtcwW->lpcs->lpszName, -1, nameA, len, NULL, NULL );
                csA.lpszName = nameA;
            }
            else
            {
                name_buf[0] = '\xff';
                name_buf[1] = cbtcwW->lpcs->lpszName[1];
                name_buf[2] = cbtcwW->lpcs->lpszName[1] >> 8;
                name_buf[3] = 0;
                csA.lpszName = name_buf;
            }
        }

        if (!IS_INTRESOURCE(cbtcwW->lpcs->lpszClass)) {
            len = WideCharToMultiByte( CP_ACP, 0, cbtcwW->lpcs->lpszClass, -1, NULL, 0, NULL, NULL );
            classA = HeapAlloc( GetProcessHeap(), 0, len*sizeof(CHAR) );
            WideCharToMultiByte( CP_ACP, 0, cbtcwW->lpcs->lpszClass, -1, classA, len, NULL, NULL );
            csA.lpszClass = classA;
        }

        ret = HOOKPROC_wrapper( proc, code, wparam, (LPARAM)&cbtcwA );
        cbtcwW->hwndInsertAfter = cbtcwA.hwndInsertAfter;
        HeapFree( GetProcessHeap(), 0, nameA );
        HeapFree( GetProcessHeap(), 0, classA );
    }
    return ret;
}


/***********************************************************************
 *		call_hook_proc
 */
static LRESULT call_hook_proc( HOOKPROC proc, INT id, INT code, WPARAM wparam, LPARAM lparam,
                               BOOL prev_unicode, BOOL next_unicode )
{
    LRESULT ret;

    TRACE_(relay)( "\1Call hook proc %p (id=%s,code=%x,wp=%08Ix,lp=%08Ix)\n",
                   proc, hook_names[id-WH_MINHOOK], code, wparam, lparam );

    if (!prev_unicode == !next_unicode) ret = proc( code, wparam, lparam );
    else if (prev_unicode) ret = call_hook_WtoA( proc, id, code, wparam, lparam );
    else ret = call_hook_AtoW( proc, id, code, wparam, lparam );

    TRACE_(relay)( "\1Ret  hook proc %p (id=%s,code=%x,wp=%08Ix,lp=%08Ix) retval=%08Ix\n",
                   proc, hook_names[id-WH_MINHOOK], code, wparam, lparam, ret );

    return ret;
}


/***********************************************************************
 *		get_hook_proc
 *
 * Retrieve the hook procedure real value for a module-relative proc
 */
void *get_hook_proc( void *proc, const WCHAR *module, HMODULE *free_module )
{
    HMODULE mod;

    GetModuleHandleExW( 0, module, &mod );
    *free_module = mod;
    if (!mod)
    {
        TRACE( "loading %s\n", debugstr_w(module) );
        /* FIXME: the library will never be freed */
        if (!(mod = LoadLibraryExW(module, NULL, LOAD_WITH_ALTERED_SEARCH_PATH))) return NULL;
    }
    return (char *)mod + (ULONG_PTR)proc;
}


/***********************************************************************
 *		SetWindowsHookA (USER32.@)
 */
HHOOK WINAPI SetWindowsHookA( INT id, HOOKPROC proc )
{
    return SetWindowsHookExA( id, proc, 0, GetCurrentThreadId() );
}


/***********************************************************************
 *		SetWindowsHookW (USER32.@)
 */
HHOOK WINAPI SetWindowsHookW( INT id, HOOKPROC proc )
{
    return SetWindowsHookExW( id, proc, 0, GetCurrentThreadId() );
}


/***********************************************************************
 *		SetWindowsHookExA (USER32.@)
 */
HHOOK WINAPI SetWindowsHookExA( INT id, HOOKPROC proc, HINSTANCE inst, DWORD tid )
{
    return set_windows_hook( id, proc, inst, tid, TRUE );
}

/***********************************************************************
 *		SetWindowsHookExW (USER32.@)
 */
HHOOK WINAPI SetWindowsHookExW( INT id, HOOKPROC proc, HINSTANCE inst, DWORD tid )
{
    return set_windows_hook( id, proc, inst, tid, FALSE );
}


/***********************************************************************
 *           SetWinEventHook                            [USER32.@]
 *
 * Set up an event hook for a set of events.
 *
 * PARAMS
 *  event_min [I] Lowest event handled by pfnProc
 *  event_max [I] Highest event handled by pfnProc
 *  inst      [I] DLL containing pfnProc
 *  proc      [I] Callback event hook function
 *  pid       [I] Process to get events from, or 0 for all processes
 *  tid       [I] Thread to get events from, or 0 for all threads
 *  flags     [I] Flags indicating the status of pfnProc
 *
 * RETURNS
 *  Success: A handle representing the hook.
 *  Failure: A NULL handle.
 */
HWINEVENTHOOK WINAPI SetWinEventHook(DWORD event_min, DWORD event_max,
                                     HMODULE inst, WINEVENTPROC proc,
                                     DWORD pid, DWORD tid, DWORD flags)
{
    WCHAR module[MAX_PATH];
    UNICODE_STRING str;
    DWORD len = 0;

    TRACE("%ld,%ld,%p,%p,%08lx,%04lx,%08lx\n", event_min, event_max, inst,
          proc, pid, tid, flags);

    if (inst && (!(len = GetModuleFileNameW( inst, module, MAX_PATH )) || len >= MAX_PATH))
    {
        inst = 0;
        len = 0;
    }
    str.Buffer = module;
    str.Length = str.MaximumLength = len * sizeof(WCHAR);
    return NtUserSetWinEventHook( event_min, event_max, inst, &str, proc, pid, tid, flags );
}

NTSTATUS WINAPI User32CallWinEventHook( void *args, ULONG size )
{
    const struct win_event_hook_params *params = args;
    WINEVENTPROC proc = params->proc;
    HMODULE free_module = 0;

    if (params->module[0] && !(proc = get_hook_proc( proc, params->module, &free_module )))
        return STATUS_INVALID_PARAMETER;

    TRACE_(relay)( "\1Call winevent hook proc %p (hhook=%p,event=%lx,hwnd=%p,object_id=%lx,child_id=%lx,tid=%04lx,time=%lx)\n",
                   proc, params->handle, params->event, params->hwnd, params->object_id,
                   params->child_id, params->tid, params->time );

    proc( params->handle, params->event, params->hwnd, params->object_id, params->child_id,
          params->tid, params->time );

    TRACE_(relay)( "\1Ret  winevent hook proc %p (hhook=%p,event=%lx,hwnd=%p,object_id=%lx,child_id=%lx,tid=%04lx,time=%lx)\n",
                   proc, params->handle, params->event, params->hwnd, params->object_id,
                   params->child_id, params->tid, params->time );

    if (free_module) FreeLibrary( free_module );
    return STATUS_SUCCESS;
}

NTSTATUS WINAPI User32CallWindowsHook( void *args, ULONG size )
{
    struct win_hook_params *params = args;
    HOOKPROC proc = params->proc;
    HMODULE free_module = 0;
    void *ret_ptr = NULL;
    CBT_CREATEWNDW cbtc;
    UINT ret_size = 0;
    size_t lparam_offset;
    LRESULT ret;

    lparam_offset = FIELD_OFFSET( struct win_hook_params, module[wcslen( params->module ) + 1]);

    if (lparam_offset < size)
    {
        lparam_offset = (lparam_offset + 15) & ~15; /* align */
        ret_size = size - lparam_offset;
        ret_ptr = (char *)params + lparam_offset;
        params->lparam = (LPARAM)ret_ptr;

        switch (params->id)
        {
        case WH_CBT:
            if (params->code == HCBT_CREATEWND)
            {
                cbtc.hwndInsertAfter = HWND_TOP;
                unpack_message( (HWND)params->wparam, WM_CREATE, NULL, (LPARAM *)&cbtc.lpcs,
                                ret_ptr, FALSE );
                params->lparam = (LPARAM)&cbtc;
                ret_size = sizeof(*cbtc.lpcs);
            }
            break;
        case WH_CALLWNDPROC:
            if (ret_size > sizeof(CWPSTRUCT))
            {
                CWPSTRUCT *cwp = (CWPSTRUCT *)params->lparam;
                size_t offset = (lparam_offset + sizeof(*cwp) + 15) & ~15;

                unpack_message( cwp->hwnd, cwp->message, &cwp->wParam, &cwp->lParam,
                                (char *)params + offset, !params->prev_unicode );
                ret_size = 0;
                break;
            }
        case WH_CALLWNDPROCRET:
            if (ret_size > sizeof(CWPRETSTRUCT))
            {
                CWPRETSTRUCT *cwpret = (CWPRETSTRUCT *)params->lparam;
                size_t offset = (lparam_offset + sizeof(*cwpret) + 15) & ~15;

                unpack_message( cwpret->hwnd, cwpret->message, &cwpret->wParam, &cwpret->lParam,
                                (char *)params + offset, !params->prev_unicode );
                ret_size = 0;
                break;
            }
        }
    }
    if (params->module[0] && !(proc = get_hook_proc( proc, params->module, &free_module )))
        return FALSE;

    ret = call_hook_proc( proc, params->id, params->code, params->wparam, params->lparam,
                          params->prev_unicode, params->next_unicode );

    if (free_module) FreeLibrary( free_module );

    if (ret_size)
    {
        LRESULT *result_ptr = (LRESULT *)ret_ptr - 1;
        *result_ptr = ret;
        return NtCallbackReturn( result_ptr, sizeof(*result_ptr) + ret_size, STATUS_SUCCESS );
    }
    return NtCallbackReturn( &ret, sizeof(ret), STATUS_SUCCESS );
}

/***********************************************************************
 *           IsWinEventHookInstalled                       [USER32.@]
 *
 * Determine if an event hook is installed for an event.
 *
 * PARAMS
 *  dwEvent  [I] Id of the event
 *
 * RETURNS
 *  TRUE,  If there are any hooks installed for the event.
 *  FALSE, Otherwise.
 *
 * BUGS
 *  Not implemented.
 */
BOOL WINAPI IsWinEventHookInstalled(DWORD dwEvent)
{
    /* FIXME: Needed by Office 2007 installer */
    WARN("(%ld)-stub!\n", dwEvent);
    return TRUE;
}

/* Wine's own user API hook, for its builtin uxtheme */
BOOL WINAPI __wine_register_user_api_hook(const struct user_api_hook *new_hook, struct user_api_hook *old_hook)
{
    if (!new_hook)
        return FALSE;

    EnterCriticalSection( &api_hook_cs );
    hooked_user_api = *new_hook;
    user_api = &hooked_user_api;
    if (old_hook)
        *old_hook = original_user_api;
    LeaveCriticalSection( &api_hook_cs );
    return TRUE;
}

void WINAPI __wine_unregister_user_api_hook(void)
{
    InterlockedExchangePointer((void **)&user_api, &original_user_api);
}


/***********************************************************************
 * Windows' user API hook
 *
 * RegisterUserApiHook names a DLL and a function in it. Every GUI process
 * of the session loads that DLL, here when it first creates a window or
 * calls DefWindowProc, and again when it gets WM_UAHINIT (which the hook's
 * owner posts after changing it). The function gets a USERAPIHOOK holding
 * user32's own functions and replaces those it wants; the hook then
 * stays in place until it is unregistered.
 */

typedef struct
{
    DWORD        size;
    const WCHAR *dll1;
    const WCHAR *func1;
    const WCHAR *dll2;
    const WCHAR *func2;
} USERAPIHOOKINFO;

enum uah_state { UAH_LOAD_INIT, UAH_STOP, UAH_SHUTDOWN };
typedef BOOL (CALLBACK *uah_init_func)( enum uah_state, USERAPIHOOK * );

USERAPIHOOK *uah;
static USERAPIHOOK uah_table;
static uah_init_func uah_init;
static HMODULE uah_module;
static unsigned int uah_serial;
static BOOL uah_checked;
static DWORD uah_thread;    /* the thread loading the hook */

static CRITICAL_SECTION uah_cs;
static CRITICAL_SECTION_DEBUG uah_cs_debug =
{
    0, 0, &uah_cs,
    { &uah_cs_debug.ProcessLocksList, &uah_cs_debug.ProcessLocksList },
    0, 0, { (DWORD_PTR)(__FILE__ ": uah_cs") }
};
static CRITICAL_SECTION uah_cs = { &uah_cs_debug, -1, 0, 0, 0, 0 };

static INT WINAPI real_SetWindowRgn( HWND hwnd, HRGN rgn, BOOL redraw )
{
    return NtUserSetWindowRgn( hwnd, rgn, redraw );
}

static INT WINAPI real_SetScrollInfo( HWND hwnd, INT bar, const SCROLLINFO *info, BOOL redraw )
{
    return NtUserSetScrollInfo( hwnd, bar, info, redraw );
}

static BOOL WINAPI real_EnableScrollBar( HWND hwnd, UINT bar, UINT flags )
{
    return NtUserEnableScrollBar( hwnd, bar, flags );
}

static void WINAPI real_MDIRedrawFrame( HWND hwnd, DWORD flags )
{
    NtUserSetWindowPos( hwnd, 0, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOSIZE | SWP_NOMOVE |
                        SWP_NOACTIVATE | SWP_NOZORDER );
}

static HWND WINAPI real_GetRealWindowOwner( HWND hwnd )
{
    return GetWindow( hwnd, GW_OWNER );
}

/* stop the hook in this process; uah_cs held */
static void uah_stop(void)
{
    uah_init_func init = uah_init;

    uah = NULL;
    uah_init = NULL;
    uah_module = 0;   /* never unloaded: other threads may still be in it */
    if (init) init( UAH_STOP, NULL );
}

static BOOL WINAPI real_ForceResetUserApiHook( HINSTANCE module )
{
    BOOL ret = FALSE;

    EnterCriticalSection( &uah_cs );
    if (uah_module && uah_module == module)
    {
        uah_stop();
        ret = TRUE;
    }
    LeaveCriticalSection( &uah_cs );
    return ret;
}

static void uah_fill_table( USERAPIHOOK *table )
{
    memset( table, 0, sizeof(*table) );
    table->size                  = sizeof(*table);
    table->DefWindowProcA        = real_DefWindowProcA;
    table->DefWindowProcW        = real_DefWindowProcW;
    table->GetScrollInfo         = real_GetScrollInfo;
    table->SetScrollInfo         = real_SetScrollInfo;
    table->EnableScrollBar       = real_EnableScrollBar;
    table->AdjustWindowRectEx    = real_AdjustWindowRectEx;
    table->SetWindowRgn          = real_SetWindowRgn;
    table->GetSystemMetrics      = real_GetSystemMetrics;
    table->SystemParametersInfoA = real_SystemParametersInfoA;
    table->SystemParametersInfoW = real_SystemParametersInfoW;
    table->ForceResetUserApiHook = real_ForceResetUserApiHook;
    table->DrawFrameControl      = real_DrawFrameControl;
    table->DrawCaption           = real_DrawCaption;
    table->MDIRedrawFrame        = real_MDIRedrawFrame;
    table->GetRealWindowOwner    = real_GetRealWindowOwner;
}

/* load the hook DLL and start the hook; uah_cs held */
static void uah_load( const struct user_api_hook_names *names )
{
    char func[ARRAY_SIZE(names->func)];
    uah_init_func init;
    HMODULE module;

    if (!WideCharToMultiByte( CP_ACP, 0, names->func, -1, func, sizeof(func), NULL, NULL )) return;
    if (!(module = LoadLibraryW( names->dll )))
    {
        WARN( "could not load %s (%lu)\n", debugstr_w(names->dll), GetLastError() );
        return;
    }
    if (!(init = (uah_init_func)GetProcAddress( module, func )))
    {
        WARN( "no %s in %s\n", debugstr_a(func), debugstr_w(names->dll) );
        FreeLibrary( module );
        return;
    }
    uah_fill_table( &uah_table );
    if (!init( UAH_LOAD_INIT, &uah_table ))
    {
        WARN( "%s!%s failed\n", debugstr_w(names->dll), debugstr_a(func) );
        FreeLibrary( module );
        return;
    }
    TRACE( "%s!%s installed\n", debugstr_w(names->dll), debugstr_a(func) );
    uah_module = module;
    uah_init = init;
    uah = &uah_table;
}

/***********************************************************************
 *		uah_check
 *
 * Bring this process up to date with the session's user API hook: once
 * (force FALSE), or whenever the hook may have changed (force TRUE).
 */
void uah_check( BOOL force )
{
    struct user_api_hook_names names;

    if (!force && uah_checked) return;
    if (uah_thread == GetCurrentThreadId()) return;   /* the hook DLL itself, while loading */
    if (!TryEnterCriticalSection( &uah_cs )) return;  /* another thread is loading it */
    if (force || !uah_checked)
    {
        uah_checked = TRUE;
        uah_thread = GetCurrentThreadId();
        if (NtUserCallTwoParam( (UINT_PTR)&names, 0, NtUserCallTwoParam_GetUserApiHook ) &&
            names.serial != uah_serial)
        {
            uah_serial = names.serial;
            if (uah_init) uah_stop();
            if (names.dll[0]) uah_load( &names );
        }
        uah_thread = 0;
    }
    LeaveCriticalSection( &uah_cs );
}

/***********************************************************************
 *		uah_call_owp
 *
 * Call a window or dialog procedure through the hook's procedures for the
 * messages it wants to see: pre may handle the message itself, post sees
 * the procedure's result.
 */
LRESULT uah_call_owp( UAH_OWP pre, UAH_OWP post, const struct uah_msg_array *array,
                      HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                      LRESULT (*call)( HWND, UINT, WPARAM, LPARAM, void * ), void *arg )
{
    LRESULT result = 0;
    void *data = NULL;

    if (!pre || !uah_is_override( array, msg )) return call( hwnd, msg, wp, lp, arg );
    if (pre( hwnd, msg, wp, lp, &result, &data )) return result;
    result = call( hwnd, msg, wp, lp, arg );
    if (post) post( hwnd, msg, wp, lp, &result, &data );
    return result;
}

/***********************************************************************
 *		RegisterUserApiHook (USER32.@)
 */
BOOL WINAPI RegisterUserApiHook( const USERAPIHOOKINFO *info )
{
    struct user_api_hook_names names;

    TRACE( "%p\n", info );
    if (!info || info->size < sizeof(*info) || !info->dll1 || !info->func1 ||
        wcslen( info->dll1 ) >= ARRAY_SIZE(names.dll) || wcslen( info->func1 ) >= ARRAY_SIZE(names.func))
    {
        SetLastError( ERROR_INVALID_PARAMETER );
        return FALSE;
    }
    memset( &names, 0, sizeof(names) );
    wcscpy( names.dll, info->dll1 );
    wcscpy( names.func, info->func1 );
    TRACE( "%s!%s\n", debugstr_w(names.dll), debugstr_w(names.func) );
    return NtUserCallTwoParam( (UINT_PTR)&names, 0, NtUserCallTwoParam_SetUserApiHook ) != 0;
}

/***********************************************************************
 *		UnregisterUserApiHook (USER32.@)
 */
BOOL WINAPI UnregisterUserApiHook(void)
{
    TRACE( "\n" );
    return NtUserCallTwoParam( 0, 0, NtUserCallTwoParam_SetUserApiHook ) != 0;
}

/***********************************************************************
 *		SetWindowRgn (USER32.@)
 */
INT WINAPI SetWindowRgn( HWND hwnd, HRGN rgn, BOOL redraw )
{
    USERAPIHOOK *hook = uah;
    return hook ? hook->SetWindowRgn( hwnd, rgn, redraw ) : NtUserSetWindowRgn( hwnd, rgn, redraw );
}
