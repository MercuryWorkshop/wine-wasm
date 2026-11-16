/*
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

#ifndef _VERSIONHELPERS_H_
#define _VERSIONHELPERS_H_

#include <windows.h>
#include <sdkddkver.h>

#ifdef __cplusplus
#define VERSIONHELPERAPI inline BOOL
#else
#define VERSIONHELPERAPI FORCEINLINE BOOL
#endif

VERSIONHELPERAPI IsWindowsVersionOrGreater(WORD major, WORD minor, WORD servpack)
{
    OSVERSIONINFOEXW info = { sizeof(info) };
    DWORDLONG mask = 0;

    mask = VerSetConditionMask(mask, VER_MAJORVERSION, VER_GREATER_EQUAL);
    mask = VerSetConditionMask(mask, VER_MINORVERSION, VER_GREATER_EQUAL);
    mask = VerSetConditionMask(mask, VER_SERVICEPACKMAJOR, VER_GREATER_EQUAL);
    info.dwMajorVersion = major;
    info.dwMinorVersion = minor;
    info.wServicePackMajor = servpack;
    return VerifyVersionInfoW(&info, VER_MAJORVERSION | VER_MINORVERSION | VER_SERVICEPACKMAJOR, mask);
}

VERSIONHELPERAPI IsWindowsXPOrGreater(void) { return IsWindowsVersionOrGreater(HIBYTE(_WIN32_WINNT_WINXP), LOBYTE(_WIN32_WINNT_WINXP), 0); }
VERSIONHELPERAPI IsWindowsXPSP1OrGreater(void) { return IsWindowsVersionOrGreater(HIBYTE(_WIN32_WINNT_WINXP), LOBYTE(_WIN32_WINNT_WINXP), 1); }
VERSIONHELPERAPI IsWindowsXPSP2OrGreater(void) { return IsWindowsVersionOrGreater(HIBYTE(_WIN32_WINNT_WINXP), LOBYTE(_WIN32_WINNT_WINXP), 2); }
VERSIONHELPERAPI IsWindowsXPSP3OrGreater(void) { return IsWindowsVersionOrGreater(HIBYTE(_WIN32_WINNT_WINXP), LOBYTE(_WIN32_WINNT_WINXP), 3); }
VERSIONHELPERAPI IsWindowsVistaOrGreater(void) { return IsWindowsVersionOrGreater(HIBYTE(_WIN32_WINNT_VISTA), LOBYTE(_WIN32_WINNT_VISTA), 0); }
VERSIONHELPERAPI IsWindowsVistaSP1OrGreater(void) { return IsWindowsVersionOrGreater(HIBYTE(_WIN32_WINNT_VISTA), LOBYTE(_WIN32_WINNT_VISTA), 1); }
VERSIONHELPERAPI IsWindowsVistaSP2OrGreater(void) { return IsWindowsVersionOrGreater(HIBYTE(_WIN32_WINNT_VISTA), LOBYTE(_WIN32_WINNT_VISTA), 2); }
VERSIONHELPERAPI IsWindows7OrGreater(void) { return IsWindowsVersionOrGreater(HIBYTE(_WIN32_WINNT_WIN7), LOBYTE(_WIN32_WINNT_WIN7), 0); }
VERSIONHELPERAPI IsWindows7SP1OrGreater(void) { return IsWindowsVersionOrGreater(HIBYTE(_WIN32_WINNT_WIN7), LOBYTE(_WIN32_WINNT_WIN7), 1); }
VERSIONHELPERAPI IsWindows8OrGreater(void) { return IsWindowsVersionOrGreater(HIBYTE(_WIN32_WINNT_WIN8), LOBYTE(_WIN32_WINNT_WIN8), 0); }
VERSIONHELPERAPI IsWindows8Point1OrGreater(void) { return IsWindowsVersionOrGreater(HIBYTE(_WIN32_WINNT_WINBLUE), LOBYTE(_WIN32_WINNT_WINBLUE), 0); }
VERSIONHELPERAPI IsWindowsThresholdOrGreater(void) { return IsWindowsVersionOrGreater(HIBYTE(_WIN32_WINNT_WINTHRESHOLD), LOBYTE(_WIN32_WINNT_WINTHRESHOLD), 0); }
VERSIONHELPERAPI IsWindows10OrGreater(void) { return IsWindowsVersionOrGreater(HIBYTE(_WIN32_WINNT_WIN10), LOBYTE(_WIN32_WINNT_WIN10), 0); }

VERSIONHELPERAPI IsWindowsServer(void)
{
    OSVERSIONINFOEXW info = { sizeof(info), 0, 0, 0, 0, {0}, 0, 0, 0, VER_NT_WORKSTATION };
    DWORDLONG mask = VerSetConditionMask(0, VER_PRODUCT_TYPE, VER_EQUAL);
    return !VerifyVersionInfoW(&info, VER_PRODUCT_TYPE, mask);
}

#endif /* _VERSIONHELPERS_H_ */
