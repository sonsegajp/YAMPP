// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef GXRUNTIME_YUYV_DECODE_H
#define GXRUNTIME_YUYV_DECODE_H

#include "core/types.h"

// E7: Decode GameCube XFB YUYV 4:2:2 → tightly packed RGBA8 (R,G,B,A).
// Inverse of dol_yuyv_encode_* / Dolphin TexDecoder_DecodeXFB (BT.601 float).

#ifdef __cplusplus
extern "C" {
#endif

// Decode one YUYV pair (Y0,U,Y1,V) into two RGBA pixels (8 bytes).
void dol_yuyv_pair_to_rgba8(u8 y0, u8 u, u8 y1, u8 v, u8* rgba8);

// Decode width*height YUYV rows (src_stride bytes/row, >= width*2) into
// tightly packed RGBA8 dest (width*4 bytes/row). width must be even.
// Returns false on null/zero/odd-width/stride-too-small/oversized.
bool dol_yuyv_decode_rgba8(const u8* yuyv, u32 width, u32 height,
                           u32 src_stride, u8* rgba);

// Byte size of tightly packed RGBA8 for width*height.
u32 dol_yuyv_rgba_byte_size(u32 width, u32 height);

#ifdef __cplusplus
}
#endif

#endif
