#ifndef MELEE_PC_NATIVE_PRELUDE_H
#define MELEE_PC_NATIVE_PRELUDE_H
/* Use the host CRT, not MSL's 32-bit size_t/uintptr_t definitions. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <dolphin/mtx.h>
typedef Vec Vec3;
typedef Quaternion Vec4;
typedef S16Vec S16Vec3;
typedef struct { float x, y; } Vec2, *Vec2Ptr, Point2d, *Point2dPtr;
typedef struct { int8_t x, y, z; } S8Vec3, S8Vec;
typedef struct { uint8_t x, y, z, w; } U8Vec4;
typedef struct { int x, y; } IntVec2;
typedef struct { int32_t x, y; } S32Vec2;
typedef struct { int x, y, z; } IntVec3;
typedef struct { int32_t x, y, z; } S32Vec, S32Vec3;

#include <dolphin/gx.h>
#include <dolphin/pad.h>
typedef enum { GX_TC_LINEAR, GX_TC_GE, GX_TC_EQ, GX_TC_LE } GXTevClampMode;
#define PAD_STICK_UP (1u << 16)
#define PAD_STICK_DOWN (1u << 17)
#define PAD_STICK_LEFT (1u << 18)
#define PAD_STICK_RIGHT (1u << 19)
#define PAD_SUBSTICK_UP (1u << 20)
#define PAD_SUBSTICK_DOWN (1u << 21)
#define PAD_SUBSTICK_LEFT (1u << 22)
#define PAD_SUBSTICK_RIGHT (1u << 23)
#define PAD_TRIGGER_LR (1u << 31)
#define PAD_CONFIRM (1ull << 32)
#define PAD_CANCEL (1ull << 33)
#define PAD_LR_START (1ull << 34)
#define PAD_LRA_START (1ull << 35)
#define PAD_ANY_UP (1ull << 36)
#define PAD_ANY_DOWN (1ull << 37)
#define PAD_ANY_LEFT (1ull << 38)
#define PAD_ANY_RIGHT (1ull << 39)

#undef ATTRIBUTE_ALIGN
#define ATTRIBUTE_ALIGN(n) __attribute__((aligned(n)))
#endif
