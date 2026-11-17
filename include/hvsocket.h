/*
 * Hyper-V sockets
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

#ifndef __HVSOCKET_H__
#define __HVSOCKET_H__

#include <ws2def.h>

#define HVSOCKET_CONNECT_TIMEOUT       0x01
#define HVSOCKET_CONNECT_TIMEOUT_MAX   300000
#define HVSOCKET_CONTAINER_PASSTHRU    0x02
#define HVSOCKET_CONNECTED_SUSPEND     0x04

#define HV_PROTOCOL_RAW 1

DEFINE_GUID(HV_GUID_ZERO, 0x00000000, 0x0000, 0x0000, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00);
DEFINE_GUID(HV_GUID_BROADCAST, 0xffffffff, 0xffff, 0xffff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff);
#define HV_GUID_WILDCARD HV_GUID_ZERO
DEFINE_GUID(HV_GUID_CHILDREN, 0x90db8b89, 0x0d35, 0x4f79, 0x8c, 0xe9, 0x49, 0xea, 0x0a, 0xc8, 0xb7, 0xcd);
DEFINE_GUID(HV_GUID_LOOPBACK, 0xe0e16197, 0xdd56, 0x4a10, 0x91, 0x95, 0x5e, 0xe7, 0xa1, 0x55, 0xa8, 0x38);
DEFINE_GUID(HV_GUID_PARENT, 0xa42e7cda, 0xd03f, 0x480c, 0x9c, 0xc2, 0xa4, 0xde, 0x20, 0xab, 0xb8, 0x78);
DEFINE_GUID(HV_GUID_SILOHOST, 0x36bd0c5c, 0x7276, 0x4223, 0x88, 0xba, 0x7d, 0x03, 0xb6, 0x54, 0xc5, 0x68);
DEFINE_GUID(HV_GUID_VSOCK_TEMPLATE, 0x00000000, 0xfacb, 0x11e6, 0xbd, 0x58, 0x64, 0x00, 0x6a, 0x79, 0x86, 0xd3);

typedef struct _SOCKADDR_HV
{
    ADDRESS_FAMILY Family;
    USHORT Reserved;
    GUID VmId;
    GUID ServiceId;
} SOCKADDR_HV, *PSOCKADDR_HV;

#define HVSOCKET_ADDRESS_FLAG_PASSTHRU 0x00000001

typedef struct _HVSOCKET_ADDRESS_INFO
{
    GUID SystemId;
    GUID VirtualMachineId;
    GUID SiloId;
    ULONG Flags;
} HVSOCKET_ADDRESS_INFO, *PHVSOCKET_ADDRESS_INFO;

#endif /* __HVSOCKET_H__ */
