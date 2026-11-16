/*
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

#ifndef _HANDLEAPI_H_
#define _HANDLEAPI_H_

#include <windef.h>
#include <apisetcconv.h>

#ifdef __cplusplus
extern "C" {
#endif

WINBASEAPI BOOL WINAPI CloseHandle(HANDLE);
WINBASEAPI BOOL WINAPI DuplicateHandle(HANDLE,HANDLE,HANDLE,HANDLE*,DWORD,BOOL,DWORD);
WINBASEAPI BOOL WINAPI GetHandleInformation(HANDLE,LPDWORD);
WINBASEAPI BOOL WINAPI SetHandleInformation(HANDLE,DWORD,DWORD);

#ifdef __cplusplus
}
#endif

#endif /* _HANDLEAPI_H_ */
