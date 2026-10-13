/*
 * Desktop window class.
 *
 * Copyright 1994 Alexandre Julliard
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
#include <string.h>

#define COBJMACROS
#include "windef.h"
#include "winbase.h"
#include "wingdi.h"
#include "winnls.h"
#include "winreg.h"
#include "wincodec.h"
#include "controls.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(desktop);

static HBRUSH hbrushPattern;
static HBITMAP hbitmapWallPaper;
static SIZE bitmapSize;
static BOOL fTileWallPaper;

/* The wallpaper is the session's, as on Windows, where it lives in win32k:
 * every process that paints the desktop (Windows' explorer paints it in its
 * own Progman window) draws the one SPI_SETDESKWALLPAPER named, in the
 * style the user's settings give it. Each process loads it when the path,
 * the style or the desktop size changes. */
static WCHAR wallpaper_path[MAX_PATH];
static int wallpaper_style = -1;
static SIZE wallpaper_screen;
static CRITICAL_SECTION wallpaper_cs;
static CRITICAL_SECTION_DEBUG wallpaper_cs_debug =
{
    0, 0, &wallpaper_cs,
    { &wallpaper_cs_debug.ProcessLocksList, &wallpaper_cs_debug.ProcessLocksList },
    0, 0, { (DWORD_PTR)(__FILE__ ": wallpaper_cs") }
};
static CRITICAL_SECTION wallpaper_cs = { &wallpaper_cs_debug, -1, 0, 0, 0, 0 };

/* Windows 7's WallpaperStyle values (with TileWallpaper=1, tiled) */
enum wallpaper_fit { FIT_CENTER = 0, FIT_STRETCH = 2, FIT_FIT = 6, FIT_FILL = 10, FIT_SPAN = 22, FIT_TILE = 100 };

static int read_wallpaper_style(void)
{
    WCHAR buf[16];
    DWORD size = sizeof(buf);
    int style = FIT_CENTER;
    HKEY key;

    if (RegOpenKeyExW( HKEY_CURRENT_USER, L"Control Panel\\Desktop", 0, KEY_READ, &key )) return style;
    if (!RegQueryValueExW( key, L"WallpaperStyle", NULL, NULL, (BYTE *)buf, &size )) style = wcstol( buf, NULL, 10 );
    size = sizeof(buf);
    if (!RegQueryValueExW( key, L"TileWallpaper", NULL, NULL, (BYTE *)buf, &size ) && wcstol( buf, NULL, 10 ))
        style = FIT_TILE;
    RegCloseKey( key );
    return style;
}

static const GUID wic_bgr32 = {0x6fddc324,0x4e03,0x4bfe,{0xb1,0x85,0x3d,0x77,0x76,0x8d,0xc9,0x0e}};

/* A wallpaper LoadImage cannot read (JPEG, PNG: what Windows 7's own
 * wallpapers are), decoded with the Windows Imaging Component */
static HBITMAP load_wic_bitmap( const WCHAR *path )
{
    HRESULT (WINAPI *create_factory)( UINT, IWICImagingFactory ** );
    IWICImagingFactory *factory = NULL;
    IWICBitmapDecoder *decoder = NULL;
    IWICBitmapFrameDecode *frame = NULL;
    IWICFormatConverter *converter = NULL;
    BITMAPINFO info = {{ sizeof(info.bmiHeader) }};
    HBITMAP bitmap = 0;
    HMODULE wic;
    UINT width, height;
    void *bits;

    if (!(wic = LoadLibraryW( L"windowscodecs.dll" ))) return 0;
    if (!(create_factory = (void *)GetProcAddress( wic, "WICCreateImagingFactory_Proxy" ))) goto done;
    if (FAILED(create_factory( WINCODEC_SDK_VERSION, &factory ))) goto done;
    if (FAILED(IWICImagingFactory_CreateDecoderFromFilename( factory, path, NULL, GENERIC_READ,
                                                            WICDecodeMetadataCacheOnDemand, &decoder )))
        goto done;
    if (FAILED(IWICBitmapDecoder_GetFrame( decoder, 0, &frame ))) goto done;
    if (FAILED(IWICImagingFactory_CreateFormatConverter( factory, &converter ))) goto done;
    if (FAILED(IWICFormatConverter_Initialize( converter, (IWICBitmapSource *)frame, &wic_bgr32,
                                               WICBitmapDitherTypeNone, NULL, 0, WICBitmapPaletteTypeCustom )))
        goto done;
    if (FAILED(IWICFormatConverter_GetSize( converter, &width, &height ))) goto done;
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -(int)height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    if (!(bitmap = CreateDIBSection( 0, &info, DIB_RGB_COLORS, &bits, 0, 0 ))) goto done;
    if (FAILED(IWICFormatConverter_CopyPixels( converter, NULL, width * 4, width * height * 4, bits )))
    {
        DeleteObject( bitmap );
        bitmap = 0;
    }
done:
    if (converter) IWICFormatConverter_Release( converter );
    if (frame) IWICBitmapFrameDecode_Release( frame );
    if (decoder) IWICBitmapDecoder_Release( decoder );
    if (factory) IWICImagingFactory_Release( factory );
    if (!bitmap) WARN( "could not decode %s\n", debugstr_w(path) );
    return bitmap;
}

/* the picture drawn at the desktop's size in its style, background around
 * it, so painting is one copy; a tiled one stays as it is */
static HBITMAP fit_wallpaper( HBITMAP picture, int style, SIZE screen )
{
    BITMAP bm;
    HDC screen_dc, src, dst;
    HBITMAP fitted, old_src, old_dst;
    HBRUSH background;
    RECT rect = { 0, 0, screen.cx, screen.cy };
    int x, y, w, h;
    double sx, sy, scale;

    if (style == FIT_TILE || !GetObjectW( picture, sizeof(bm), &bm ) || !bm.bmWidth || !bm.bmHeight)
        return picture;
    w = bm.bmWidth;
    h = bm.bmHeight;
    sx = (double)screen.cx / w;
    sy = (double)screen.cy / h;
    switch (style)
    {
    case FIT_STRETCH: w = screen.cx; h = screen.cy; break;
    case FIT_FIT:  scale = sx < sy ? sx : sy; w = w * scale + 0.5; h = h * scale + 0.5; break;
    case FIT_FILL:
    case FIT_SPAN: scale = sx > sy ? sx : sy; w = w * scale + 0.5; h = h * scale + 0.5; break;
    default: break;  /* centred */
    }
    x = (screen.cx - w) / 2;
    y = (screen.cy - h) / 2;

    screen_dc = GetDC( 0 );
    fitted = CreateCompatibleBitmap( screen_dc, screen.cx, screen.cy );
    src = CreateCompatibleDC( screen_dc );
    dst = CreateCompatibleDC( screen_dc );
    ReleaseDC( 0, screen_dc );
    if (!fitted || !src || !dst)
    {
        if (fitted) DeleteObject( fitted );
        if (src) DeleteDC( src );
        if (dst) DeleteDC( dst );
        return picture;
    }
    old_src = SelectObject( src, picture );
    old_dst = SelectObject( dst, fitted );
    background = GetSysColorBrush( COLOR_BACKGROUND );
    FillRect( dst, &rect, background );
    SetStretchBltMode( dst, HALFTONE );
    SetBrushOrgEx( dst, 0, 0, NULL );
    StretchBlt( dst, x, y, w, h, src, 0, 0, bm.bmWidth, bm.bmHeight, SRCCOPY );
    SelectObject( src, old_src );
    SelectObject( dst, old_dst );
    DeleteDC( src );
    DeleteDC( dst );
    DeleteObject( picture );
    return fitted;
}


/***********************************************************************
 *           DESKTOP_LoadBitmap
 */
static HBITMAP DESKTOP_LoadBitmap( const WCHAR *filename )
{
    HBITMAP hbitmap;

    if (!filename[0]) return 0;
    hbitmap = LoadImageW( 0, filename, IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION | LR_LOADFROMFILE );
    if (!hbitmap) hbitmap = load_wic_bitmap( filename );
    if (!hbitmap)
    {
        WCHAR buffer[MAX_PATH];
        UINT len = GetWindowsDirectoryW( buffer, MAX_PATH - 2 );
        if (buffer[len - 1] != '\\') buffer[len++] = '\\';
        lstrcpynW( buffer + len, filename, MAX_PATH - len );
        hbitmap = LoadImageW( 0, buffer, IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION | LR_LOADFROMFILE );
        if (!hbitmap) hbitmap = load_wic_bitmap( buffer );
    }
    return hbitmap;
}

/***********************************************************************
 *           init_wallpaper
 */
static void init_wallpaper( const WCHAR *wallpaper )
{
    HBITMAP hbitmap = DESKTOP_LoadBitmap( wallpaper );
    RECT rect;
    SIZE screen;
    int style = read_wallpaper_style();

    GetClientRect( GetDesktopWindow(), &rect );
    screen.cx = rect.right;
    screen.cy = rect.bottom;
    if (hbitmap && screen.cx && screen.cy) hbitmap = fit_wallpaper( hbitmap, style, screen );

    EnterCriticalSection( &wallpaper_cs );
    if (hbitmapWallPaper) DeleteObject( hbitmapWallPaper );
    hbitmapWallPaper = hbitmap;
    lstrcpynW( wallpaper_path, wallpaper, MAX_PATH );
    wallpaper_style = style;
    wallpaper_screen = screen;
    if (hbitmap)
    {
	BITMAP bmp;
	GetObjectA( hbitmap, sizeof(bmp), &bmp );
	bitmapSize.cx = (bmp.bmWidth != 0) ? bmp.bmWidth : 1;
	bitmapSize.cy = (bmp.bmHeight != 0) ? bmp.bmHeight : 1;
        fTileWallPaper = style == FIT_TILE;
    }
    LeaveCriticalSection( &wallpaper_cs );
}

/* bring this process's copy of the session's wallpaper up to date */
static void sync_wallpaper(void)
{
    WCHAR path[MAX_PATH];
    RECT rect;

    if (!NtUserSystemParametersInfo( SPI_GETDESKWALLPAPER, ARRAYSIZE(path), path, 0 )) return;
    GetClientRect( GetDesktopWindow(), &rect );
    if (!wcscmp( path, wallpaper_path ) && wallpaper_screen.cx == rect.right &&
        wallpaper_screen.cy == rect.bottom && wallpaper_style == read_wallpaper_style())
        return;
    TRACE( "%s, style %d, %ldx%ld\n", debugstr_w(path), read_wallpaper_style(), rect.right, rect.bottom );
    init_wallpaper( path );
}

/***********************************************************************
 *           DesktopWndProcA
 */
LRESULT WINAPI DesktopWndProcA( HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam )
{
    switch (message)
    {
    case WM_NCCREATE:
    case WM_NCCALCSIZE:
    case WM_PARENTNOTIFY:
    case WM_DISPLAYCHANGE:
        return NtUserMessageCall( hwnd, message, wParam, lParam, 0, NtUserDesktopWindowProc, TRUE );

    default:
        if (message < WM_USER)
            return DefWindowProcA( hwnd, message, wParam, lParam );
        return NtUserMessageCall( hwnd, message, wParam, lParam, 0, NtUserDesktopWindowProc, TRUE );
    }
}

/***********************************************************************
 *           DesktopWndProcW
 */
LRESULT WINAPI DesktopWndProcW( HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam )
{
    switch (message)
    {
    case WM_NCCREATE:
    case WM_NCCALCSIZE:
    case WM_PARENTNOTIFY:
    case WM_DISPLAYCHANGE:
        return NtUserMessageCall( hwnd, message, wParam, lParam, 0, NtUserDesktopWindowProc, FALSE );

    default:
        if (message < WM_USER)
            return DefWindowProcW( hwnd, message, wParam, lParam );
        return NtUserMessageCall( hwnd, message, wParam, lParam, 0, NtUserDesktopWindowProc, FALSE );
    }
}

/***********************************************************************
 *           PaintDesktop   (USER32.@)
 *
 */
BOOL WINAPI PaintDesktop(HDC hdc)
{
    HWND hwnd = GetDesktopWindow();

    /* check for an owning thread; otherwise don't paint anything (non-desktop mode) */
    if (GetWindowThreadProcessId( hwnd, NULL ))
    {
        RECT rect;

        sync_wallpaper();
        EnterCriticalSection( &wallpaper_cs );

        GetClientRect( hwnd, &rect );

        /* Paint desktop pattern (only if wall paper does not cover everything) */

        if (!hbitmapWallPaper ||
            (!fTileWallPaper && ((bitmapSize.cx < rect.right) || (bitmapSize.cy < rect.bottom))))
        {
            HBRUSH brush = hbrushPattern;
            if (!brush) brush = (HBRUSH)GetClassLongPtrW( hwnd, GCLP_HBRBACKGROUND );
            /* Set colors in case pattern is a monochrome bitmap */
            SetBkColor( hdc, RGB(0,0,0) );
            SetTextColor( hdc, GetSysColor(COLOR_BACKGROUND) );
            FillRect( hdc, &rect, brush );
        }

        /* Paint wall paper */

        if (hbitmapWallPaper)
        {
            INT x, y;
            HDC hMemDC = CreateCompatibleDC( hdc );

            SelectObject( hMemDC, hbitmapWallPaper );

            if (fTileWallPaper)
            {
                for (y = 0; y < rect.bottom; y += bitmapSize.cy)
                    for (x = 0; x < rect.right; x += bitmapSize.cx)
                        BitBlt( hdc, x, y, bitmapSize.cx, bitmapSize.cy, hMemDC, 0, 0, SRCCOPY );
            }
            else
            {
                x = (rect.left + rect.right - bitmapSize.cx) / 2;
                y = (rect.top + rect.bottom - bitmapSize.cy) / 2;
                if (x < 0) x = 0;
                if (y < 0) y = 0;
                BitBlt( hdc, x, y, bitmapSize.cx, bitmapSize.cy, hMemDC, 0, 0, SRCCOPY );
            }
            DeleteDC( hMemDC );
        }
        LeaveCriticalSection( &wallpaper_cs );
    }
    return TRUE;
}

/***********************************************************************
 *           SetDeskWallpaper   (USER32.@)
 */
BOOL WINAPI SetDeskWallpaper( const char *filename )
{
    return SystemParametersInfoA( SPI_SETDESKWALLPAPER, MAX_PATH, (void *)filename, SPIF_UPDATEINIFILE );
}

/***********************************************************************
 *           update_wallpaper
 */
BOOL update_wallpaper( const WCHAR *wallpaper, const WCHAR *pattern )
{
    int pat[8];

    if (hbrushPattern) DeleteObject( hbrushPattern );
    hbrushPattern = 0;
    memset( pat, 0, sizeof(pat) );
    if (pattern)
    {
        char buffer[64];
        WideCharToMultiByte( CP_ACP, 0, pattern, -1, buffer, sizeof(buffer), NULL, NULL );
        if (sscanf( buffer, " %d %d %d %d %d %d %d %d",
                    &pat[0], &pat[1], &pat[2], &pat[3],
                    &pat[4], &pat[5], &pat[6], &pat[7] ))
        {
            WORD ptrn[8];
            HBITMAP hbitmap;
            int i;

            for (i = 0; i < 8; i++) ptrn[i] = pat[i] & 0xffff;
            hbitmap = CreateBitmap( 8, 8, 1, 1, ptrn );
            hbrushPattern = CreatePatternBrush( hbitmap );
            DeleteObject( hbitmap );
        }
    }
    init_wallpaper( wallpaper );
    NtUserRedrawWindow( GetDesktopWindow(), 0, 0, RDW_INVALIDATE | RDW_ERASE | RDW_NOCHILDREN );
    return TRUE;
}
