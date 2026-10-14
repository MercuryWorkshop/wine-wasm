/*
 * Dwmapi
 *
 * Copyright 2007 Andras Kovacs
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
 */

#include <stdarg.h>

#include "winternl.h"
#define COBJMACROS
#include "windef.h"
#include "winbase.h"
#include "wingdi.h"
#include "winuser.h"
#include "winreg.h"
#include "dwmapi.h"
#include "ntuser.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(dwmapi);

/* Whether the display driver draws DWM's glass (gex's compositor does):
 * then composition is on, whatever version is reported. */
static BOOL driver_draws_glass(void)
{
    static LONG state = -1;

    if (state == -1) state = NtUserSetWindowGlass( NULL, NULL ) ? 1 : 0;
    return state;
}

/* Windows 7's uxtheme takes a window's frame back from DWM (when it has a
 * window region, or its theme is detached) with this private attribute. */
#define DWMWA_NCRENDERING_EXILED 0x10001

/* Whether DWM draws the window's frame: the policy the application set
 * (DWMWA_NCRENDERING_POLICY) and whether uxtheme exiled it, kept with the
 * window. */
static const WCHAR ncrendering_policy_prop[] = L"__wine_dwm_ncrendering_policy";
static const WCHAR ncrendering_exiled_prop[] = L"__wine_dwm_ncrendering_exiled";

static BOOL ncrendering_enabled( HWND hwnd )
{
    DWORD style = GetWindowLongW( hwnd, GWL_STYLE );

    if (!driver_draws_glass()) return FALSE;
    if ((style & WS_CAPTION) != WS_CAPTION || (style & WS_CHILD)) return FALSE;
    if ((UINT_PTR)GetPropW( hwnd, ncrendering_policy_prop ) == DWMNCRP_DISABLED + 1) return FALSE;
    return !GetPropW( hwnd, ncrendering_exiled_prop );
}

static void update_ncrendering( HWND hwnd )
{
    struct set_window_glass_params params = { .mask = WINDOW_GLASS_FRAME };

    params.frame = ncrendering_enabled( hwnd );
    NtUserSetWindowGlass( hwnd, &params );
}

/* The colorization, as Windows 7 keeps it; its default is "sky". */
struct colorization
{
    DWORD argb, balance, blur_balance, afterglow, afterglow_balance;
};

static void get_colorization( struct colorization *c )
{
    static const struct { const WCHAR *name; size_t offset; } values[] =
    {
        { L"ColorizationColor", offsetof(struct colorization, argb) },
        { L"ColorizationColorBalance", offsetof(struct colorization, balance) },
        { L"ColorizationBlurBalance", offsetof(struct colorization, blur_balance) },
        { L"ColorizationAfterglow", offsetof(struct colorization, afterglow) },
        { L"ColorizationAfterglowBalance", offsetof(struct colorization, afterglow_balance) },
    };
    DWORD size, i;
    HKEY key;

    c->argb = c->afterglow = 0x6b74b8fc;
    c->balance = 8;
    c->blur_balance = 49;
    c->afterglow_balance = 43;
    if (RegOpenKeyExW( HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\DWM", 0, KEY_READ, &key )) return;
    for (i = 0; i < ARRAY_SIZE(values); i++)
    {
        size = sizeof(DWORD);
        RegQueryValueExW( key, values[i].name, NULL, NULL, (BYTE *)c + values[i].offset, &size );
    }
    RegCloseKey( key );
}

static BOOL set_glass( HWND hwnd, struct set_window_glass_params *params )
{
    struct colorization c;

    get_colorization( &c );
    params->mask |= WINDOW_GLASS_COLORIZATION;
    params->argb = c.argb;
    params->balance = min( c.balance, 100 );
    params->blur_balance = min( c.blur_balance, 100 );
    params->afterglow = c.afterglow;
    params->afterglow_balance = min( c.afterglow_balance, 100 );
    return NtUserSetWindowGlass( hwnd, params );
}


/**********************************************************************
 *           DwmIsCompositionEnabled         (DWMAPI.@)
 */
HRESULT WINAPI DwmIsCompositionEnabled(BOOL *enabled)
{
    RTL_OSVERSIONINFOEXW version;

    TRACE("%p\n", enabled);

    if (!enabled)
        return E_INVALIDARG;

    *enabled = FALSE;
    version.dwOSVersionInfoSize = sizeof(version);
    if (!RtlGetVersion(&version))
        *enabled = (version.dwMajorVersion > 6 || (version.dwMajorVersion == 6 && version.dwMinorVersion >= 3));
    if (!*enabled) *enabled = driver_draws_glass();

    return S_OK;
}

/**********************************************************************
 *           DwmpStartupViaUserInit         (DWMAPI.115)
 *
 * userinit.exe has the session's Desktop Window Manager started (waiting
 * up to timeout ms) before it starts the shell. There is no DWM process
 * to start: where composition is on, it is part of the system, and where
 * it is off (Windows 7, the Basic theme), Windows starts none either.
 */
HRESULT WINAPI DwmpStartupViaUserInit(DWORD timeout)
{
    BOOL enabled;

    TRACE("%lu\n", timeout);

    if (DwmIsCompositionEnabled(&enabled) == S_OK && !enabled)
        return DWM_E_COMPOSITIONDISABLED;
    return S_OK;
}

/**********************************************************************
 *           DwmEnableComposition         (DWMAPI.102)
 */
HRESULT WINAPI DwmEnableComposition(UINT uCompositionAction)
{
    FIXME("(%d) stub\n", uCompositionAction);

    return S_OK;
}

/**********************************************************************
 *           DwmExtendFrameIntoClientArea    (DWMAPI.@)
 */
HRESULT WINAPI DwmExtendFrameIntoClientArea(HWND hwnd, const MARGINS* margins)
{
    struct set_window_glass_params params = {.mask = WINDOW_GLASS_MARGINS};

    TRACE("(%p, %p)\n", hwnd, margins);

    if (!margins) return E_INVALIDARG;
    if (!driver_draws_glass())
    {
        FIXME("(%p, %p) stub\n", hwnd, margins);
        return S_OK;
    }
    params.margins[0] = margins->cxLeftWidth;
    params.margins[1] = margins->cxRightWidth;
    params.margins[2] = margins->cyTopHeight;
    params.margins[3] = margins->cyBottomHeight;
    return set_glass( hwnd, &params ) ? S_OK : E_HANDLE;
}

/**********************************************************************
 *           DwmGetColorizationColor      (DWMAPI.@)
 */
HRESULT WINAPI DwmGetColorizationColor(DWORD *colorization, BOOL *opaque_blend)
{
    struct colorization c;

    TRACE("(%p, %p)\n", colorization, opaque_blend);

    if (!colorization || !opaque_blend) return E_INVALIDARG;
    if (!driver_draws_glass())
    {
        FIXME("(%p, %p) stub\n", colorization, opaque_blend);
        return E_NOTIMPL;
    }
    get_colorization( &c );
    *colorization = c.argb;
    *opaque_blend = FALSE;
    return S_OK;
}

/**********************************************************************
 *        DwmInvalidateIconicBitmaps      (DWMAPI.@)
 */
HRESULT WINAPI DwmInvalidateIconicBitmaps(HWND hwnd)
{
    static BOOL once;

    if (!once++) FIXME("(%p) stub\n", hwnd);

    return E_NOTIMPL;
}

/**********************************************************************
 *           DwmSetWindowAttribute         (DWMAPI.@)
 */
HRESULT WINAPI DwmSetWindowAttribute(HWND hwnd, DWORD attributenum, LPCVOID attribute, DWORD size)
{
    static BOOL once;

    TRACE("(%p, %lx, %p, %lx)\n", hwnd, attributenum, attribute, size);

    switch (attributenum)
    {
    case DWMWA_NCRENDERING_POLICY:
    {
        DWORD policy;

        if (!attribute || size < sizeof(DWORD)) return E_INVALIDARG;
        policy = *(const DWORD *)attribute;
        if (policy >= DWMNCRP_LAST) return E_INVALIDARG;
        if (!IsWindow( hwnd )) return E_HANDLE;
        SetPropW( hwnd, ncrendering_policy_prop, (HANDLE)(UINT_PTR)(policy + 1) );
        if (driver_draws_glass()) update_ncrendering( hwnd );
        return S_OK;
    }
    case DWMWA_NCRENDERING_EXILED:
        if (!attribute || size < sizeof(BOOL)) return E_INVALIDARG;
        if (!IsWindow( hwnd )) return E_HANDLE;
        if (*(const BOOL *)attribute) SetPropW( hwnd, ncrendering_exiled_prop, (HANDLE)1 );
        else RemovePropW( hwnd, ncrendering_exiled_prop );
        if (driver_draws_glass()) update_ncrendering( hwnd );
        return S_OK;
    }

    if (!once++) FIXME("(%p, %lx, %p, %lx) stub\n", hwnd, attributenum, attribute, size);

    return S_OK;
}

/**********************************************************************
 *           DwmGetGraphicsStreamClient         (DWMAPI.@)
 */
HRESULT WINAPI DwmGetGraphicsStreamClient(UINT uIndex, UUID *pClientUuid)
{
    FIXME("(%d, %p) stub\n", uIndex, pClientUuid);

    return E_NOTIMPL;
}

/**********************************************************************
 *           DwmGetTransportAttributes         (DWMAPI.@)
 */
HRESULT WINAPI DwmGetTransportAttributes(BOOL *pfIsRemoting, BOOL *pfIsConnected, DWORD *pDwGeneration)
{
    FIXME("(%p, %p, %p) stub\n", pfIsRemoting, pfIsConnected, pDwGeneration);

    return DWM_E_COMPOSITIONDISABLED;
}

/**********************************************************************
 *           DwmUnregisterThumbnail         (DWMAPI.@)
 */
HRESULT WINAPI DwmUnregisterThumbnail(HTHUMBNAIL thumbnail)
{
    FIXME("(%p) stub\n", thumbnail);

    return E_NOTIMPL;
}

/**********************************************************************
 *           DwmEnableMMCSS         (DWMAPI.@)
 */
HRESULT WINAPI DwmEnableMMCSS(BOOL enableMMCSS)
{
    FIXME("(%d) stub\n", enableMMCSS);

    return S_OK;
}

/**********************************************************************
 *           DwmGetGraphicsStreamTransformHint         (DWMAPI.@)
 */
HRESULT WINAPI DwmGetGraphicsStreamTransformHint(UINT uIndex, MilMatrix3x2D *pTransform)
{
    FIXME("(%d, %p) stub\n", uIndex, pTransform);

    return E_NOTIMPL;
}

/**********************************************************************
 *           DwmEnableBlurBehindWindow         (DWMAPI.@)
 */
HRESULT WINAPI DwmEnableBlurBehindWindow(HWND hWnd, const DWM_BLURBEHIND *pBlurBuf)
{
    struct set_window_glass_params params = {.mask = WINDOW_GLASS_BLUR};

    TRACE("%p %p\n", hWnd, pBlurBuf);

    if (!pBlurBuf) return E_INVALIDARG;
    if (!driver_draws_glass())
    {
        FIXME("%p %p\n", hWnd, pBlurBuf);
        return E_NOTIMPL;
    }
    /* Only what the flags name changes; the rest stays as it was. */
    if (!(pBlurBuf->dwFlags & DWM_BB_ENABLE)) return S_OK;
    params.blur = pBlurBuf->fEnable;
    if ((pBlurBuf->dwFlags & DWM_BB_BLURREGION) && pBlurBuf->hRgnBlur)
        GetRgnBox( pBlurBuf->hRgnBlur, &params.blur_rect );
    return set_glass( hWnd, &params ) ? S_OK : E_HANDLE;
}

/**********************************************************************
 *           DwmDefWindowProc         (DWMAPI.@)
 */
BOOL WINAPI DwmDefWindowProc(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam, LRESULT *plResult)
{
    static int i;

    if (!i++) FIXME("stub\n");

    return FALSE;
}

/**********************************************************************
 *           DwmGetWindowAttribute         (DWMAPI.@)
 */
HRESULT WINAPI DwmGetWindowAttribute(HWND hwnd, DWORD attribute, PVOID pv_attribute, DWORD size)
{
    BOOL enabled = FALSE;
    HRESULT hr;

    TRACE("(%p %ld %p %ld)\n", hwnd, attribute, pv_attribute, size);

    if (DwmIsCompositionEnabled(&enabled) == S_OK && !enabled)
        return E_HANDLE;
    if (!IsWindow(hwnd))
        return E_HANDLE;

    switch (attribute) {
    case DWMWA_NCRENDERING_ENABLED:
        if (!pv_attribute) return E_INVALIDARG;
        if (size < sizeof(BOOL)) return E_NOT_SUFFICIENT_BUFFER;
        *(BOOL *)pv_attribute = ncrendering_enabled( hwnd );
        hr = S_OK;
        break;
    case DWMWA_EXTENDED_FRAME_BOUNDS:
    {
        RECT *rect = (RECT *)pv_attribute;
        DPI_AWARENESS_CONTEXT context;

        if (!rect)
            return E_INVALIDARG;
        if (size < sizeof(*rect))
            return E_NOT_SUFFICIENT_BUFFER;
        if (GetWindowLongW(hwnd, GWL_STYLE) & WS_CHILD)
            return E_HANDLE;

        /* DWM frame bounds are always in physical coords */
        context = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE);
        if (GetWindowRect(hwnd, rect))
            hr = S_OK;
        else
            hr = HRESULT_FROM_WIN32(GetLastError());

        SetThreadDpiAwarenessContext(context);
        break;
    }
    default:
        FIXME("attribute %ld not implemented.\n", attribute);
        hr = E_NOTIMPL;
        break;
    }

    return hr;
}

/**********************************************************************
 *           DwmRegisterThumbnail         (DWMAPI.@)
 */
HRESULT WINAPI DwmRegisterThumbnail(HWND dest, HWND src, PHTHUMBNAIL thumbnail_id)
{
    FIXME("(%p %p %p) stub\n", dest, src, thumbnail_id);

    return E_NOTIMPL;
}

static int get_display_frequency(void)
{
    DEVMODEW mode;
    BOOL ret;

    memset(&mode, 0, sizeof(mode));
    mode.dmSize = sizeof(mode);
    ret = EnumDisplaySettingsExW(NULL, ENUM_CURRENT_SETTINGS, &mode, 0);
    if (ret && mode.dmFields & DM_DISPLAYFREQUENCY && mode.dmDisplayFrequency)
    {
        return mode.dmDisplayFrequency;
    }
    else
    {
        WARN("Failed to query display frequency, returning a fallback value.\n");
        return 60;
    }
}

/**********************************************************************
 *           DwmGetCompositionTimingInfo         (DWMAPI.@)
 */
HRESULT WINAPI DwmGetCompositionTimingInfo(HWND hwnd, DWM_TIMING_INFO *info)
{
    LARGE_INTEGER performance_frequency, qpc;
    static int i, display_frequency;

    if (!info)
        return E_INVALIDARG;

    if (info->cbSize != sizeof(DWM_TIMING_INFO))
        return MILERR_MISMATCHED_SIZE;

    if(!i++) FIXME("(%p %p)\n", hwnd, info);

    memset(info, 0, info->cbSize);
    info->cbSize = sizeof(DWM_TIMING_INFO);

    display_frequency = get_display_frequency();
    info->rateRefresh.uiNumerator = display_frequency;
    info->rateRefresh.uiDenominator = 1;
    info->rateCompose.uiNumerator = display_frequency;
    info->rateCompose.uiDenominator = 1;

    QueryPerformanceFrequency(&performance_frequency);
    info->qpcRefreshPeriod = performance_frequency.QuadPart / display_frequency;

    QueryPerformanceCounter(&qpc);
    info->qpcVBlank = (qpc.QuadPart / info->qpcRefreshPeriod) * info->qpcRefreshPeriod;

    return S_OK;
}

/**********************************************************************
 *                  DwmFlush              (DWMAPI.@)
 */
HRESULT WINAPI DwmFlush(void)
{
    LARGE_INTEGER qpf, qpc, delay;
    LONG64 qpc_refresh_period;
    int display_frequency;
    static BOOL once;

    if (!once++)
        FIXME("stub.\n");
    else
        TRACE("stub.\n");

    display_frequency = get_display_frequency();
    NtQueryPerformanceCounter(&qpc, &qpf);
    qpc_refresh_period = qpf.QuadPart / display_frequency;
    delay.QuadPart = (qpc.QuadPart - ((qpc.QuadPart + qpc_refresh_period - 1) / qpc_refresh_period) * qpc_refresh_period)
            * 10000000 / qpf.QuadPart;
    NtDelayExecution(FALSE, &delay);

    return S_OK;
}

/**********************************************************************
 *           DwmAttachMilContent         (DWMAPI.@)
 */
HRESULT WINAPI DwmAttachMilContent(HWND hwnd)
{
    FIXME("(%p) stub\n", hwnd);
    return E_NOTIMPL;
}

/**********************************************************************
 *           DwmDetachMilContent         (DWMAPI.@)
 */
HRESULT WINAPI DwmDetachMilContent(HWND hwnd)
{
    FIXME("(%p) stub\n", hwnd);
    return E_NOTIMPL;
}

/**********************************************************************
 *           DwmUpdateThumbnailProperties         (DWMAPI.@)
 */
HRESULT WINAPI DwmUpdateThumbnailProperties(HTHUMBNAIL thumbnail, const DWM_THUMBNAIL_PROPERTIES *props)
{
    FIXME("(%p, %p) stub\n", thumbnail, props);
    return E_NOTIMPL;
}

/**********************************************************************
 *           DwmSetPresentParameters         (DWMAPI.@)
 */
HRESULT WINAPI DwmSetPresentParameters(HWND hwnd, DWM_PRESENT_PARAMETERS *params)
{
    FIXME("(%p %p) stub\n", hwnd, params);
    return S_OK;
};

/**********************************************************************
 *           DwmSetIconicLivePreviewBitmap         (DWMAPI.@)
 */
HRESULT WINAPI DwmSetIconicLivePreviewBitmap(HWND hwnd, HBITMAP hbmp, POINT *pos, DWORD flags)
{
    FIXME("(%p %p %p %lx) stub\n", hwnd, hbmp, pos, flags);
    return S_OK;
};

/**********************************************************************
 *           DwmSetIconicThumbnail         (DWMAPI.@)
 */
HRESULT WINAPI DwmSetIconicThumbnail(HWND hwnd, HBITMAP hbmp, DWORD flags)
{
    FIXME("(%p %p %lx) stub\n", hwnd, hbmp, flags);
    return S_OK;
};

/**********************************************************************
 *           DwmpGetColorizationParameters         (DWMAPI.@)
 */
HRESULT WINAPI DwmpGetColorizationParameters(void *params)
{
    /* Windows 7's DWM_COLORIZATION_PARAMS, as the personalization page reads it */
    struct
    {
        COLORREF color;
        COLORREF afterglow;
        UINT intensity;
        UINT afterglow_balance;
        UINT blur_balance;
        UINT reflection_intensity;
        BOOL opaque;
    } *out = params;
    struct colorization c;

    TRACE("(%p)\n", params);

    if (!out) return E_INVALIDARG;
    if (!driver_draws_glass())
    {
        FIXME("(%p) stub\n", params);
        return E_NOTIMPL;
    }
    get_colorization( &c );
    out->color = c.argb;
    out->afterglow = c.afterglow;
    out->intensity = c.balance;
    out->afterglow_balance = c.afterglow_balance;
    out->blur_balance = c.blur_balance;
    out->reflection_intensity = 0;
    out->opaque = FALSE;
    return S_OK;
}

/**********************************************************************
 *           DwmShowContact         (DWMAPI.@)
 */
HRESULT WINAPI DwmShowContact(DWORD pointer_id, enum DWM_SHOWCONTACT showcontact)
{
    FIXME("pointer_id %#lx, showcontact %#x stub\n", pointer_id, showcontact);
    return S_OK;
}
