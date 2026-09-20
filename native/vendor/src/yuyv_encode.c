// SPDX-License-Identifier: GPL-3.0-or-later
#include "gxruntime/yuyv_encode.h"

// Port of Dolphin SWEfbInterface::ConvertColorToYUV + EncodeXFB 4:2:2 pack
// (VideoBackends/Software/SWEfbInterface.cpp). BT.601 with HW-tested coeffs.
// Layout per pixel pair: Y0, U, Y1, V (yuv422_packed {Y,UV} twice).

void dol_yuyv_rgb_to_yuv_raw(u8 r, u8 g, u8 b, u8* y_raw, s8* u_s, s8* v_s) {
    const u16 y = (u16)(66 * r + 129 * g + 25 * b);
    const s16 u = (s16)(-38 * r + -74 * g + 112 * b);
    const s16 v = (s16)(112 * r + -94 * g + -18 * b);
    if (y_raw != NULL)
        *y_raw = (u8)((y >> 8) + ((y >> 7) & 1));
    if (u_s != NULL)
        *u_s = (s8)((u >> 8) + ((u >> 7) & 1));
    if (v_s != NULL)
        *v_s = (s8)((v >> 8) + ((v >> 7) & 1));
}

u32 dol_yuyv_byte_size(u32 width, u32 height) {
    return width * height * 2u;
}

static void pack_row_yuyv(const u8* y_raw, const s8* u_s, const s8* v_s,
                          u32 width, u8* dest) {
    /* Dolphin EncodeXFB: for each pair, U and V both filter around the first
     * pixel of the pair (scanline i-1,i,i+1 with border clamp). */
    for (u32 x = 0; x + 1u < width; x += 2u) {
        const s16 u_l = (s16)(x == 0u ? u_s[0] : u_s[x - 1u]);
        const s16 u_m = (s16)u_s[x];
        const s16 u_r = (s16)(x + 1u < width ? u_s[x + 1u] : u_s[x]);
        const s16 v_l = (s16)(x == 0u ? v_s[0] : v_s[x - 1u]);
        const s16 v_m = (s16)v_s[x];
        const s16 v_r = (s16)(x + 1u < width ? v_s[x + 1u] : v_s[x]);

        dest[x * 2u + 0u] = (u8)(y_raw[x] + 16u);
        dest[x * 2u + 1u] = (u8)(128 + ((u_l + (u_m << 1) + u_r) >> 2));
        dest[(x + 1u) * 2u + 0u] = (u8)(y_raw[x + 1u] + 16u);
        dest[(x + 1u) * 2u + 1u] =
            (u8)(128 + ((v_l + (v_m << 1) + v_r) >> 2));
    }
}

bool dol_yuyv_encode_rgba8(const u8* rgba, u32 width, u32 height, u8* dest,
                           u32 dest_stride) {
    if (rgba == NULL || dest == NULL || width == 0u || height == 0u)
        return false;
    if ((width & 1u) != 0u)
        return false;
    if (dest_stride < width * 2u)
        return false;
    if (width > 640u)
        return false;

    u8 y_raw[640];
    s8 u_s[640];
    s8 v_s[640];

    for (u32 y = 0; y < height; ++y) {
        const u8* row = rgba + (size_t)y * (size_t)width * 4u;
        for (u32 x = 0; x < width; ++x) {
            const u8* p = row + x * 4u;
            dol_yuyv_rgb_to_yuv_raw(p[0], p[1], p[2], &y_raw[x], &u_s[x],
                                    &v_s[x]);
        }
        pack_row_yuyv(y_raw, u_s, v_s, width,
                      dest + (size_t)y * dest_stride);
    }
    return true;
}

bool dol_yuyv_encode_rgba8_rect(const u8* rgba, u32 rgba_w, u32 rgba_h,
                                u32 src_x, u32 src_y, u32 width, u32 height,
                                u8* dest, u32 dest_stride) {
    if (rgba == NULL || dest == NULL || width == 0u || height == 0u)
        return false;
    if ((width & 1u) != 0u)
        return false;
    if (src_x + width > rgba_w || src_y + height > rgba_h)
        return false;
    if (dest_stride == 0u)
        dest_stride = width * 2u;
    if (dest_stride < width * 2u)
        return false;
    if (width > 640u)
        return false;

    u8 y_raw[640];
    s8 u_s[640];
    s8 v_s[640];

    for (u32 y = 0; y < height; ++y) {
        const u8* row =
            rgba + ((size_t)(src_y + y) * (size_t)rgba_w + src_x) * 4u;
        for (u32 x = 0; x < width; ++x) {
            const u8* p = row + x * 4u;
            dol_yuyv_rgb_to_yuv_raw(p[0], p[1], p[2], &y_raw[x], &u_s[x],
                                    &v_s[x]);
        }
        pack_row_yuyv(y_raw, u_s, v_s, width,
                      dest + (size_t)y * dest_stride);
    }
    return true;
}

bool dol_yuyv_encode_rgba8_rect_yscale(const u8* rgba, u32 rgba_w, u32 rgba_h,
                                       u32 src_x, u32 src_y, u32 width,
                                       u32 height, u32 dst_height, u8* dest,
                                       u32 dest_stride) {
    if (dst_height == 0u || dst_height == height)
        return dol_yuyv_encode_rgba8_rect(rgba, rgba_w, rgba_h, src_x, src_y,
                                          width, height, dest, dest_stride);
    if (rgba == NULL || dest == NULL || width == 0u || height == 0u)
        return false;
    if ((width & 1u) != 0u)
        return false;
    if (src_x + width > rgba_w || src_y + height > rgba_h)
        return false;
    if (dest_stride == 0u)
        dest_stride = width * 2u;
    if (dest_stride < width * 2u)
        return false;
    if (width > 640u)
        return false;

    u8 y_raw[640];
    s8 u_s[640];
    s8 v_s[640];

    /* Nearest-neighbor vertical resample (SW CopyRegion y_ratio). */
    for (u32 dy = 0; dy < dst_height; ++dy) {
        u32 sy = (u32)(((u64)dy * (u64)height + (u64)(dst_height / 2u)) /
                       (u64)dst_height);
        if (sy >= height)
            sy = height - 1u;
        const u8* row =
            rgba + ((size_t)(src_y + sy) * (size_t)rgba_w + src_x) * 4u;
        for (u32 x = 0; x < width; ++x) {
            const u8* p = row + x * 4u;
            dol_yuyv_rgb_to_yuv_raw(p[0], p[1], p[2], &y_raw[x], &u_s[x],
                                    &v_s[x]);
        }
        pack_row_yuyv(y_raw, u_s, v_s, width,
                      dest + (size_t)dy * dest_stride);
    }
    return true;
}

u32 dol_xfb_height_from_yscale(u32 efb_height, u32 i_scale, bool scale_invert) {
    if (efb_height == 0u)
        return 1u;
    u32 isc = i_scale & 0x1FFu;
    if (isc == 0u)
        isc = 256u;

    u32 xfb_h;
    if (scale_invert) {
        /* SDK path: iScale = 256/yScale written to BP; yScale = 256/iScale.
         * Dolphin BPStructs: yScale = 256.0f / dispcopyyscale when scale_invert.
         * num = 1 + (efbH-1) * yScale  with copyTexSrcWH.y = efbH-1. */
        if (isc == 256u) {
            xfb_h = efb_height;
        } else {
            /* Integer form of 1 + (efbH-1)*256/iScale, matching __GXGetNumXfbLines
             * for the common case (no odd-scale bump). */
            const u32 count = (efb_height - 1u) * 256u;
            xfb_h = (count / isc) + 1u;
            u32 isc_d = isc;
            if (isc_d > 0x80u && isc_d < 0x100u) {
                while ((isc_d % 2u) == 0u)
                    isc_d /= 2u;
                if ((efb_height % isc_d) == 0u)
                    xfb_h += 1u;
            }
        }
    } else {
        /* yScale = iScale/256; num = 1 + (efbH-1)*(iScale/256). */
        if (isc == 256u) {
            xfb_h = efb_height;
        } else {
            const u32 count = (efb_height - 1u) * isc;
            xfb_h = (count / 256u) + 1u;
        }
    }
    if (xfb_h == 0u)
        xfb_h = 1u;
    if (xfb_h > 0x400u)
        xfb_h = 0x400u;
    return xfb_h;
}

bool dol_yuyv_encode_argb32(const u32* argb, u32 width, u32 height, u8* dest,
                            u32 dest_stride) {
    if (argb == NULL || dest == NULL || width == 0u || height == 0u)
        return false;
    if ((width & 1u) != 0u)
        return false;
    if (dest_stride < width * 2u)
        return false;
    if (width > 640u)
        return false;

    u8 y_raw[640];
    s8 u_s[640];
    s8 v_s[640];

    for (u32 y = 0; y < height; ++y) {
        const u32* row = argb + (size_t)y * (size_t)width;
        for (u32 x = 0; x < width; ++x) {
            const u32 c = row[x];
            const u8 r = (u8)((c >> 16) & 0xFFu);
            const u8 g = (u8)((c >> 8) & 0xFFu);
            const u8 b = (u8)(c & 0xFFu);
            dol_yuyv_rgb_to_yuv_raw(r, g, b, &y_raw[x], &u_s[x], &v_s[x]);
        }
        pack_row_yuyv(y_raw, u_s, v_s, width,
                      dest + (size_t)y * dest_stride);
    }
    return true;
}
