// SPDX-License-Identifier: GPL-3.0-or-later
#include "gxruntime/yuyv_decode.h"

// Port of Dolphin TexDecoder_DecodeXFB (TextureDecoder_Common.cpp):
// per pair Y0,U,Y1,V → two RGB via inverse BT.601 float, alpha=255.

static u8 clamp_u8_f(float v) {
    if (v < 0.f)
        return 0;
    if (v > 255.f)
        return 255;
    return (u8)v;
}

void dol_yuyv_pair_to_rgba8(u8 y0, u8 u, u8 y1, u8 v, u8* rgba8) {
    const int Y1 = (int)y0 - 16;
    const int U = (int)u - 128;
    const int Y2 = (int)y1 - 16;
    const int V = (int)v - 128;
    const float fY1 = (float)Y1;
    const float fY2 = (float)Y2;
    const float fU = (float)U;
    const float fV = (float)V;

    rgba8[0] = clamp_u8_f(1.164f * fY1 + 1.596f * fV);
    rgba8[1] = clamp_u8_f(1.164f * fY1 - 0.392f * fU - 0.813f * fV);
    rgba8[2] = clamp_u8_f(1.164f * fY1 + 2.017f * fU);
    rgba8[3] = 255;

    rgba8[4] = clamp_u8_f(1.164f * fY2 + 1.596f * fV);
    rgba8[5] = clamp_u8_f(1.164f * fY2 - 0.392f * fU - 0.813f * fV);
    rgba8[6] = clamp_u8_f(1.164f * fY2 + 2.017f * fU);
    rgba8[7] = 255;
}

u32 dol_yuyv_rgba_byte_size(u32 width, u32 height) {
    return width * height * 4u;
}

bool dol_yuyv_decode_rgba8(const u8* yuyv, u32 width, u32 height,
                           u32 src_stride, u8* rgba) {
    if (yuyv == NULL || rgba == NULL || width == 0u || height == 0u)
        return false;
    if ((width & 1u) != 0u)
        return false;
    if (src_stride < width * 2u)
        return false;
    /* VI overscan can exceed 640; match common NTSC/PAL XFB bounds. */
    if (width > 720u || height > 576u)
        return false;

    for (u32 y = 0; y < height; ++y) {
        const u8* row = yuyv + (size_t)y * src_stride;
        u8* dst = rgba + (size_t)y * (size_t)width * 4u;
        for (u32 x = 0; x + 1u < width; x += 2u) {
            const u8* p = row + x * 2u;
            dol_yuyv_pair_to_rgba8(p[0], p[1], p[2], p[3], dst + x * 4u);
        }
    }
    return true;
}
