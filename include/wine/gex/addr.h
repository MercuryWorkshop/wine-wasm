/*
 * The 32-bit address gex's imports take for a native pointer.
 *
 * On wasm32 a pointer is that address. On wasm64 (W64, gex
 * docs/wine-native-x64.md) the code is LP64 lowered to wasm32: pointers
 * are 64 bits wide but every address is below 4 GiB, so the address is the
 * low half. Every pointer passed to a gex import goes through GEX_ADDR, so
 * this is the one place the lowering's address step is made (and where a
 * dual heap would check it).
 */
#ifndef __WINE_GEX_ADDR_H
#define __WINE_GEX_ADDR_H

#include <stdint.h>

#define GEX_ADDR(p) ((int)(uint32_t)(uintptr_t)(p))

#endif
