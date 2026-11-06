/*
 * Window theming support
 *
 * Copyright 2022 Zhiyi Zhang for CodeWeavers
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
#include "wingdi.h"
#include "winuser.h"
#include "uxthemedll.h"
#include "vssym32.h"

static void uxtheme_draw_menu_button(HTHEME theme, HWND hwnd, HDC hdc, enum NONCLIENT_BUTTON_TYPE type,
                                     RECT rect, BOOL down, BOOL grayed)
{
    int part, state;

    switch (type)
    {
    case MENU_CLOSE_BUTTON:
        part = WP_MDICLOSEBUTTON;
        break;
    case MENU_MIN_BUTTON:
        part = WP_MDIMINBUTTON;
        break;
    case MENU_RESTORE_BUTTON:
        part = WP_MDIRESTOREBUTTON;
        break;
    case MENU_HELP_BUTTON:
        part = WP_MDIHELPBUTTON;
        break;
    /* There is no WP_MDIMAXBUTTON */
    default:
        user_api.pNonClientButtonDraw(hwnd, hdc, type, rect, down, grayed);
        return;
    }

    if (grayed)
        state = MINBS_DISABLED;
    else if (down)
        state = MINBS_PUSHED;
    else
        state = MINBS_NORMAL;

    if (IsThemeBackgroundPartiallyTransparent(theme, part, state))
        DrawThemeParentBackground(hwnd, hdc, &rect);
    DrawThemeBackground(theme, hdc, part, state, &rect, NULL);
}

void WINAPI UXTHEME_NonClientButtonDraw(HWND hwnd, HDC hdc, enum NONCLIENT_BUTTON_TYPE type,
                                        RECT rect, BOOL down, BOOL grayed)
{
    HTHEME theme;

    theme = OpenThemeDataForDpi(NULL, L"Window", GetDpiForWindow(hwnd));
    if (!theme)
    {
        user_api.pNonClientButtonDraw(hwnd, hdc, type, rect, down, grayed);
        return;
    }

    switch (type)
    {
    case MENU_CLOSE_BUTTON:
    case MENU_MIN_BUTTON:
    case MENU_MAX_BUTTON:
    case MENU_RESTORE_BUTTON:
    case MENU_HELP_BUTTON:
        uxtheme_draw_menu_button(theme, hwnd, hdc, type, rect, down, grayed);
        break;
    default:
        break;
    }

    CloseThemeData(theme);
}

/* A caption button's part: the restore button in place of a maximized window's maximize one and
 * a minimized window's minimize one. */
static int caption_button_part(DWORD style, DWORD ex_style, enum NONCLIENT_BUTTON_TYPE type)
{
    switch (type)
    {
    case NC_THEMED_CLOSE_BUTTON:
        return (ex_style & WS_EX_TOOLWINDOW) ? WP_SMALLCLOSEBUTTON : WP_CLOSEBUTTON;
    case NC_THEMED_MAX_BUTTON:
        return (style & WS_MAXIMIZE) ? WP_RESTOREBUTTON : WP_MAXBUTTON;
    case NC_THEMED_MIN_BUTTON:
        return (style & WS_MINIMIZE) ? WP_RESTOREBUTTON : WP_MINBUTTON;
    default:
        return 0;
    }
}

static int caption_part(HTHEME theme, DWORD style, DWORD ex_style)
{
    if (ex_style & WS_EX_TOOLWINDOW) return WP_SMALLCAPTION;
    if ((style & WS_MAXIMIZE) && IsThemePartDefined(theme, WP_MAXCAPTION, 0)) return WP_MAXCAPTION;
    return WP_CAPTION;
}

/* The caption, from the window's top edge to the caption's bottom, in window coordinates. */
static void caption_rect(HWND hwnd, DWORD style, DWORD ex_style, const RECT *window, RECT *caption,
                         RECT *inside)
{
    RECT adjust = {0, 0, 0, 0};
    int height = GetSystemMetrics((ex_style & WS_EX_TOOLWINDOW) ? SM_CYSMCAPTION : SM_CYCAPTION);

    AdjustWindowRectEx(&adjust, style & ~(WS_HSCROLL | WS_VSCROLL), FALSE, ex_style & ~WS_EX_CLIENTEDGE);
    inside->left = window->left - adjust.left;
    inside->right = window->right - adjust.right;
    inside->top = window->top - adjust.top - height;
    inside->bottom = window->bottom - adjust.bottom;
    *caption = *window;
    caption->bottom = inside->top + height;
}

/* A caption button's state: the CBS_* value, as the theme's inactive images (the second four of
 * eight) for an inactive window. */
static int caption_button_state(HTHEME theme, int part, int state, BOOL active)
{
    int count;

    if (!active && SUCCEEDED(GetThemeInt(theme, part, 0, TMT_IMAGECOUNT, &count)) && count >= 8)
        state += 4;
    return state;
}

static void draw_caption_button(HTHEME theme, HDC hdc, DWORD style, DWORD ex_style, int part, int state,
                                BOOL active, const RECT *caption, const RECT *button)
{
    int cap_state = active ? CS_ACTIVE : CS_INACTIVE;

    /* The caption behind it first: the button's corners are transparent. */
    DrawThemeBackground(theme, hdc, caption_part(theme, style, ex_style), cap_state, caption, button);
    DrawThemeBackground(theme, hdc, part, caption_button_state(theme, part, state, active), button, NULL);
}

static HICON caption_icon(HWND hwnd, DWORD ex_style)
{
    HICON icon = (HICON)SendMessageW(hwnd, WM_GETICON, ICON_SMALL2, 0);

    if (!icon) icon = (HICON)GetClassLongPtrW(hwnd, GCLP_HICONSM);
    if (!icon) icon = (HICON)SendMessageW(hwnd, WM_GETICON, ICON_BIG, 0);
    if (!icon) icon = (HICON)GetClassLongPtrW(hwnd, GCLP_HICON);
    if (!icon && !(ex_style & WS_EX_DLGMODALFRAME))
        icon = LoadImageW(0, (LPCWSTR)IDI_WINLOGO, IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                          GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR | LR_SHARED);
    return icon;
}

/* Whether the maximize or minimize button is disabled: its style missing (win32u says for close). */
static BOOL caption_button_disabled(HWND hwnd, DWORD style, UINT command)
{
    if (command == SC_MAXIMIZE) return !(style & WS_MAXIMIZEBOX);
    if (command == SC_MINIMIZE) return !(style & WS_MINIMIZEBOX);
    return FALSE;
}

/* The window's frame, caption and caption buttons in the visual style, the buttons where win32u's
 * hit-testing finds them (the classic buttons' places). */
static void draw_frame(HTHEME theme, HWND hwnd, HDC hdc, const RECT *window, BOOL active, UINT hot)
{
    DWORD style = GetWindowLongW(hwnd, GWL_STYLE), ex_style = GetWindowLongW(hwnd, GWL_EXSTYLE);
    BOOL small = !!(ex_style & WS_EX_TOOLWINDOW);
    int frame_state = active ? FS_ACTIVE : FS_INACTIVE, cap_state = active ? CS_ACTIVE : CS_INACTIVE;
    int cxsize = GetSystemMetrics(SM_CXSIZE), cysize = GetSystemMetrics(SM_CYSIZE);
    RECT caption, inside, r, text;
    WCHAR buffer[256];
    COLORREF color;
    int part, len;

    caption_rect(hwnd, style, ex_style, window, &caption, &inside);
    DrawThemeBackground(theme, hdc, caption_part(theme, style, ex_style), cap_state, &caption, NULL);

    if (inside.left > window->left)
    {
        SetRect(&r, window->left, caption.bottom, inside.left, inside.bottom);
        DrawThemeBackground(theme, hdc, small ? WP_SMALLFRAMELEFT : WP_FRAMELEFT, frame_state, &r, NULL);
    }
    if (inside.right < window->right)
    {
        SetRect(&r, inside.right, caption.bottom, window->right, inside.bottom);
        DrawThemeBackground(theme, hdc, small ? WP_SMALLFRAMERIGHT : WP_FRAMERIGHT, frame_state, &r, NULL);
    }
    if (inside.bottom < window->bottom)
    {
        SetRect(&r, window->left, inside.bottom, window->right, window->bottom);
        DrawThemeBackground(theme, hdc, small ? WP_SMALLFRAMEBOTTOM : WP_FRAMEBOTTOM, frame_state, &r, NULL);
    }

    text = inside;
    text.bottom = caption.bottom;
    if ((style & WS_SYSMENU) && !small)
    {
        HICON icon = caption_icon(hwnd, ex_style);
        int cx = GetSystemMetrics(SM_CXSMICON), cy = GetSystemMetrics(SM_CYSMICON);

        if (icon)
        {
            DrawIconEx(hdc, inside.left + 2, inside.top + (GetSystemMetrics(SM_CYCAPTION) - cy) / 2, icon,
                       cx, cy, 0, 0, DI_NORMAL);
            text.left += cx + 2;
        }
    }

    if (style & WS_SYSMENU)
    {
        BOOL disabled = !!(hot & NC_THEMED_CLOSE_DISABLED);
        int state = disabled ? CBS_DISABLED : LOWORD(hot) == HTCLOSE ? CBS_HOT : CBS_NORMAL;

        if (small)
        {
            int bmp = MulDiv(11, GetDpiForWindow(hwnd), USER_DEFAULT_SCREEN_DPI);
            int height = GetSystemMetrics(SM_CYSMCAPTION);

            r.top = inside.top + (height - 1 - bmp) / 2;
            r.left = inside.right - (height + 1 + bmp) / 2;
            r.bottom = r.top + bmp;
            r.right = r.left + bmp;
        }
        else
            SetRect(&r, inside.right - cxsize, inside.top + 2, inside.right - 2, inside.top + cysize - 2);
        draw_caption_button(theme, hdc, style, ex_style, caption_button_part(style, ex_style, NC_THEMED_CLOSE_BUTTON),
                            state, active, &caption, &r);
        text.right = r.left - 2;

        if (!small && (style & (WS_MAXIMIZEBOX | WS_MINIMIZEBOX)))
        {
            int right = inside.right - cxsize;

            disabled = caption_button_disabled(hwnd, style, SC_MAXIMIZE);
            state = disabled ? CBS_DISABLED : LOWORD(hot) == HTMAXBUTTON ? CBS_HOT : CBS_NORMAL;
            SetRect(&r, right - cxsize, inside.top + 2, right - 2, inside.top + cysize - 2);
            draw_caption_button(theme, hdc, style, ex_style, caption_button_part(style, ex_style, NC_THEMED_MAX_BUTTON),
                                state, active, &caption, &r);

            right -= cxsize - 2;
            disabled = caption_button_disabled(hwnd, style, SC_MINIMIZE);
            state = disabled ? CBS_DISABLED : LOWORD(hot) == HTMINBUTTON ? CBS_HOT : CBS_NORMAL;
            SetRect(&r, right - cxsize, inside.top + 2, right - 2, inside.top + cysize - 2);
            draw_caption_button(theme, hdc, style, ex_style, caption_button_part(style, ex_style, NC_THEMED_MIN_BUTTON),
                                state, active, &caption, &r);
            text.right = r.left - 2;
        }
    }

    len = GetWindowTextW(hwnd, buffer, ARRAY_SIZE(buffer));
    if (len)
    {
        NONCLIENTMETRICSW metrics = {sizeof(metrics)};
        HFONT font, old_font;
        POINT offset;

        part = caption_part(theme, style, ex_style);
        SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, 0, &metrics, 0);
        font = CreateFontIndirectW(small ? &metrics.lfSmCaptionFont : &metrics.lfCaptionFont);
        old_font = SelectObject(hdc, font);
        SetBkMode(hdc, TRANSPARENT);
        text.left += 2;
        if (SUCCEEDED(GetThemeColor(theme, part, cap_state, TMT_TEXTSHADOWCOLOR, &color))
            && SUCCEEDED(GetThemePosition(theme, part, cap_state, TMT_TEXTSHADOWOFFSET, &offset)))
        {
            RECT shadow = text;

            OffsetRect(&shadow, offset.x, offset.y);
            SetTextColor(hdc, color);
            DrawTextW(hdc, buffer, len, &shadow, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_LEFT | DT_END_ELLIPSIS);
        }
        if (FAILED(GetThemeColor(theme, part, cap_state, TMT_TEXTCOLOR, &color)))
            color = GetSysColor(active ? COLOR_CAPTIONTEXT : COLOR_INACTIVECAPTIONTEXT);
        SetTextColor(hdc, color);
        DrawTextW(hdc, buffer, len, &text, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_LEFT | DT_END_ELLIPSIS);
        DeleteObject(SelectObject(hdc, old_font));
    }
}

/* The window's shape in the visual style: the caption's transparent corners cut away, as XP's
 * rounded top corners. Set once per window size; a region the program set itself is kept, and a
 * maximized window has none. */
static void update_window_region(HTHEME theme, HWND hwnd, DWORD style, DWORD ex_style, const RECT *window,
                                 int cap_state)
{
    static const WCHAR prop[] = L"__wine_uxtheme_region";
    int cx = window->right - window->left, cy = window->bottom - window->top;
    ULONG_PTR want = (style & WS_MAXIMIZE) ? 0 : MAKELONG(cx, cy);
    ULONG_PTR have = (ULONG_PTR)GetPropW(hwnd, prop);
    RECT caption, inside;
    HRGN region, rest;

    if (have == want) return;
    if (!have)
    {
        HRGN current = CreateRectRgn(0, 0, 0, 0);
        int kind = GetWindowRgn(hwnd, current);

        DeleteObject(current);
        if (kind != ERROR) return;
    }
    if (!want)
    {
        RemovePropW(hwnd, prop);
        SetWindowRgn(hwnd, NULL, TRUE);
        return;
    }

    caption_rect(hwnd, style, ex_style, window, &caption, &inside);
    if (FAILED(GetThemeBackgroundRegion(theme, NULL, caption_part(theme, style, ex_style), cap_state, &caption,
                                        &region)))
        return;
    rest = CreateRectRgn(window->left, caption.bottom, window->right, window->bottom);
    CombineRgn(region, region, rest, RGN_OR);
    DeleteObject(rest);
    SetPropW(hwnd, prop, (HANDLE)want);
    if (!SetWindowRgn(hwnd, region, TRUE)) DeleteObject(region);
}

/* A captioned window's frame part in the visual style (win32u's NC_THEMED_* calls): FALSE when no
 * style is on, or the program asked for classic frames, win32u then drawing those. */
BOOL WINAPI UXTHEME_NonClientThemedDraw(HWND hwnd, HDC hdc, enum NONCLIENT_BUTTON_TYPE type,
                                        RECT rect, BOOL down, BOOL grayed)
{
    DWORD style, ex_style;
    HTHEME theme;
    RECT caption, inside;

    if (!IsThemeActive() || !(GetThemeAppProperties() & STAP_ALLOW_NONCLIENT))
        return FALSE;
    if (!(theme = OpenThemeDataForDpi(NULL, L"Window", GetDpiForWindow(hwnd))))
        return FALSE;

    style = GetWindowLongW(hwnd, GWL_STYLE);
    ex_style = GetWindowLongW(hwnd, GWL_EXSTYLE);
    if (type == NC_THEMED_FRAME)
    {
        draw_frame(theme, hwnd, hdc, &rect, down, grayed);
        update_window_region(theme, hwnd, style, ex_style, &rect, down ? CS_ACTIVE : CS_INACTIVE);
    }
    else
    {
        RECT window;

        GetWindowRect(hwnd, &window);
        OffsetRect(&window, -window.left, -window.top);
        caption_rect(hwnd, style, ex_style, &window, &caption, &inside);
        draw_caption_button(theme, hdc, style, ex_style, caption_button_part(style, ex_style, type), down, grayed,
                            &caption, &rect);
    }
    CloseThemeData(theme);
    return TRUE;
}
