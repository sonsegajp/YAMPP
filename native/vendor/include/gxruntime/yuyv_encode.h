// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef GXRUNTIME_YUYV_ENCODE_H
#define GXRUNTIME_YUYV_ENCODE_H

#include "core/types.h"

// E7: GameCube XFB is YUYV 4:2:2 (Dolphin EFBCopyFormat::XFB / EncodeXFB).
// Pure software encode from tightly packed RGBA8 (top-left) into guest-RAM
// layout: per pixel pair [Y0, U, Y1, V] with BT.601 hardware-tested coeffs
// (SWEfbInterface::ConvertColorToYUV + 4:2:2 downsample). No GPU dependency.

#ifdef __cplusplus
extern "C" {
#endif

// Convert one RGB pixel to pre-offset Y and signed U/V (not yet +16/+128).
// Out: y_raw 0..255-ish, u_s/v_s signed chroma before 128 bias.
void dol_yuyv_rgb_to_yuv_raw(u8 r, u8 g, u8 b, u8* y_raw, s8* u_s, s8* v_s);

// Encode width*height RGBA8 pixels (4 bytes/pixel, tightly packed rows) into
// YUYV at dest. dest_stride is bytes per output row (must be >= width*2 and
// even). width must be even (hardware assumes YU-aligned start). Returns false
// on null/zero/odd-width/stride-too-small.
bool dol_yuyv_encode_rgba8(const u8* rgba, u32 width, u32 height,
                           u8* dest, u32 dest_stride);

// Same encode from ARGB8888 host words (DolEfbAccess color store layout).
bool dol_yuyv_encode_argb32(const u32* argb, u32 width, u32 height,
                            u8* dest, u32 dest_stride);

// Encode an EFB source rect from a full-frame RGBA8 buffer into YUYV dest.
// rgba is tightly packed rgba_w*rgba_h; rect is [src_x,src_y)+[width,height).
// dest_stride defaults to width*2 when 0. width must be even.
bool dol_yuyv_encode_rgba8_rect(const u8* rgba, u32 rgba_w, u32 rgba_h,
                                u32 src_x, u32 src_y, u32 width, u32 height,
                                u8* dest, u32 dest_stride);

// Like rect encode, but vertically scales EFB height → dst_height with
// nearest-neighbor row pick (Dolphin SWEfbInterface EncodeXFB + CopyRegion).
// dst_height == 0 or == height falls through to unscaled rect encode.
bool dol_yuyv_encode_rgba8_rect_yscale(const u8* rgba, u32 rgba_w, u32 rgba_h,
                                       u32 src_x, u32 src_y, u32 width,
                                       u32 height, u32 dst_height, u8* dest,
                                       u32 dest_stride);

// Byte size of a tightly packed YUYV buffer for width*height pixels.
u32 dol_yuyv_byte_size(u32 width, u32 height);

// XFB line count from EFB height + BP 0x4E iScale + UPE scale_invert (bit10).
// Dolphin: num_xfb_lines = 1 + (efbH-1) * yScale; SDK __GXGetNumXfbLines when
// scale_invert. i_scale 0 → treat as 256 (1.0). Caps at 1024.
u32 dol_xfb_height_from_yscale(u32 efb_height, u32 i_scale, bool scale_invert);

#ifdef __cplusplus
}
#endif

#endif
