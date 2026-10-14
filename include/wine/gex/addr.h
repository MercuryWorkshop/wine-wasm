/*
 * The address gex's imports take for a native pointer.
 *
 * On wasm32 a pointer is that address. On wasm64 (W64, gex
 * docs/wine-native-x64.md) the code is LP64. By default it is lowered to
 * wasm32, so pointers are 64 bits wide but every address is below 4 GiB
 * and the address is the low half. Every pointer passed to a gex import
 * goes through GEX_ADDR, so this is the one place the lowering's address
 * step is made (and where a dual heap would check it).
 *
 * A memory64 flavour (GEX_MEMORY64, wcc64 -mgex-memory64: not built yet,
 * docs/wine-native-x64.md §0.1) keeps the full 64-bit address, and its gex
 * imports take i64 addresses.
 */
#ifndef __WINE_GEX_ADDR_H
#define __WINE_GEX_ADDR_H

#include <stdint.h>

#ifdef GEX_MEMORY64
typedef long long gex_addr_t;
#define GEX_ADDR(p) ((gex_addr_t)(uintptr_t)(p))
#else
typedef int gex_addr_t;
#define GEX_ADDR(p) ((gex_addr_t)(uint32_t)(uintptr_t)(p))
#endif

#endif
