#include "gx_translate.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../platform_compat.h"

#define ARRAY_COUNT(x) (sizeof(x) / sizeof((x)[0]))

typedef struct Buffer {
    uint8_t data[4096];
    uint32_t size;
} Buffer;

static int failures;

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
        ++failures; \
        return; \
    } \
} while (0)

static void push_u8(Buffer* b, uint8_t value) { b->data[b->size++] = value; }

static void push_be16(Buffer* b, uint16_t value)
{
    push_u8(b, (uint8_t)(value >> 8));
    push_u8(b, (uint8_t)value);
}

static void push_be32(Buffer* b, uint32_t value)
{
    push_u8(b, (uint8_t)(value >> 24));
    push_u8(b, (uint8_t)(value >> 16));
    push_u8(b, (uint8_t)(value >> 8));
    push_u8(b, (uint8_t)value);
}

static uint16_t read_be16(const uint8_t* p)
{
    return (uint16_t)(((uint32_t)p[0] << 8) | p[1]);
}

static uint32_t read_be32(const uint8_t* p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static uint64_t read_be64(const uint8_t* p)
{
    return ((uint64_t)read_be32(p) << 32) | read_be32(p + 4);
}

static void cp(Buffer* b, uint8_t reg, uint32_t value)
{
    push_u8(b, 0x08);
    push_u8(b, reg);
    push_be32(b, value);
}

static void bp(Buffer* b, uint8_t reg, uint32_t value)
{
    push_u8(b, 0x61);
    push_be32(b, ((uint32_t)reg << 24) | (value & 0x00FFFFFFu));
}

static void draw(Buffer* b, uint8_t opcode, const uint8_t* vertices,
                 uint16_t count, uint32_t bytes)
{
    push_u8(b, opcode);
    push_be16(b, count);
    memcpy(b->data + b->size, vertices, bytes);
    b->size += bytes;
}

static void call_dl(Buffer* b, uint32_t address, uint32_t size)
{
    push_u8(b, 0x40);
    push_be32(b, address);
    push_be32(b, size);
}

static int find_aurora(const uint8_t* bytes, uint32_t size, uint16_t subcommand,
                       uint32_t start)
{
    uint32_t i;
    for (i = start; i + 3u <= size; ++i) {
        if (bytes[i] == 0x50 && read_be16(bytes + i + 1) == subcommand)
            return (int)i;
    }
    return -1;
}

static int count_aurora(const uint8_t* bytes, uint32_t size, uint16_t subcommand)
{
    int count = 0;
    uint32_t start = 0;
    int found;
    while ((found = find_aurora(bytes, size, subcommand, start)) >= 0) {
        ++count;
        start = (uint32_t)found + 3u;
    }
    return count;
}

static int count_texture_id(const uint8_t* bytes, uint32_t size, uint8_t id)
{
    int count = 0;
    uint32_t start = 0;
    int found;
    while ((found = find_aurora(bytes, size, 0x0030, start)) >= 0) {
        if ((uint32_t)found + 4u <= size && bytes[found + 3] == id)
            ++count;
        start = (uint32_t)found + 3u;
    }
    return count;
}

static void setup_direct_position(Buffer* b)
{
    cp(b, 0x50, 1u << 9);                /* POS direct */
    cp(b, 0x70, 1u | (4u << 1));         /* XYZ, F32 */
}

static void append_direct_point(Buffer* b)
{
    static const uint8_t xyz[12] = {0};
    draw(b, 0xB8, xyz, 1, sizeof(xyz));  /* GX_POINTS | VTXFMT0 */
}

static void setup_c8_texture(Buffer* b, uint32_t texture_phys,
                             uint32_t tlut_phys, uint32_t tmem)
{
    bp(b, 0x64, tlut_phys >> 5);
    bp(b, 0x65, tmem | (1u << 10)); /* one 32-byte line = 16 entries */
    bp(b, 0x80, 0);
    bp(b, 0x84, 32u << 8); /* max LOD 2.0 -> three source levels */
    bp(b, 0x88, 7u | (7u << 10) | (9u << 20));
    bp(b, 0x94, texture_phys >> 5);
    bp(b, 0x98, tmem | (1u << 10));
}

static void test_source_sizes(void)
{
    CHECK(gx_translate_texture_source_size(0x0, 1, 1, 1) == 32);
    CHECK(gx_translate_texture_source_size(0x1, 9, 5, 1) == 128);
    CHECK(gx_translate_texture_source_size(0x6, 5, 5, 1) == 256);
    CHECK(gx_translate_texture_source_size(0xE, 8, 8, 1) == 32);
    CHECK(gx_translate_texture_source_size(0x9, 8, 8, 3) == 128);
    CHECK(gx_translate_texture_source_size(0x7, 8, 8, 1) == 0);
    CHECK(gx_translate_tlut_source_size(16) == 32);
}

static void test_exact_indexed_array(void)
{
    /* End RAM at the exact required extent: upload capacity must not cross it. */
    uint8_t ram[0x1000 + 72] = {0};
    uint8_t output[4096];
    const uint8_t indices[3] = {2, 5, 1};
    const uint8_t smaller[1] = {1};
    Buffer fifo = {{0}, 0};
    Buffer next = {{0}, 0};
    GxTranslateContext* context = gx_translate_create();
    uint32_t size;
    int command;
    const GxTranslateStats* stats;
    CHECK(context != NULL);

    cp(&fifo, 0x50, 2u << 9);              /* POS INDEX8 */
    cp(&fifo, 0x70, 1u | (4u << 1));       /* XYZ F32: 12 bytes */
    cp(&fifo, 0xB0, 12);
    cp(&fifo, 0xA0, 0x1000);
    draw(&fifo, 0x90, indices, 3, sizeof(indices));
    size = gx_translate_stream(context, fifo.data, fifo.size, ram, sizeof(ram),
                               output, sizeof(output));
    CHECK(size != 0);
    command = find_aurora(output, size, 0x0010, 0);
    CHECK(command >= 0);
    CHECK(read_be32(output + command + 11) == 72); /* 5 * 12 + 12 */
    stats = gx_translate_stats(context);
    CHECK(stats->array_source_bytes == 72);

    gx_translate_begin_frame(context);
    draw(&next, 0x90, smaller, 1, sizeof(smaller));
    size = gx_translate_stream(context, next.data, next.size, ram, sizeof(ram),
                               output, sizeof(output));
    CHECK(size != 0);
    command = find_aurora(output, size, 0x0010, 0);
    CHECK(command >= 0);
    CHECK(read_be32(output + command + 11) == 24); /* 1 * 12 + 12 */

    ram[0x1000] ^= 0xFF;   /* the guard absorbs invalidations of unchanged bytes */
    gx_translate_invalidate_ram(context, 0x1000, 24);
    size = gx_translate_stream(context, next.data, next.size, ram, sizeof(ram),
                               output, sizeof(output));
    CHECK(size != 0);
    /* Content changes surface as a fresh snapshot pointer, not a zero-size
     * rebind toggle. */
    CHECK(count_aurora(output, size, 0x0010) == 1);
    command = find_aurora(output, size, 0x0010, 0);
    CHECK(command >= 0);
    CHECK(read_be32(output + command + 11) == 24);
    CHECK(read_be64(output + command + 3) != (uint64_t)(uintptr_t)(ram + 0x1000));
    gx_translate_destroy(context);
}

static void test_nbt3_and_indexed_xf(void)
{
    uint8_t ram[0x10000] = {0};
    uint8_t output[4096];
    const uint8_t nbt_indices[3] = {1, 4, 2};
    Buffer fifo = {{0}, 0};
    GxTranslateContext* context = gx_translate_create();
    uint32_t size;
    int command;
    CHECK(context != NULL);
    /* Limit visible RAM to the required indexed source, including NBT3 tails. */

    cp(&fifo, 0x50, 2u << 11);             /* NRM INDEX8 */
    cp(&fifo, 0x70, (1u << 31) | (4u << 10)); /* NBT3 F32 */
    cp(&fifo, 0xB1, 12);
    cp(&fifo, 0xA1, 0x1800);
    draw(&fifo, 0x90, nbt_indices, 1, sizeof(nbt_indices));
    size = gx_translate_stream(context, fifo.data, fifo.size, ram, 0x1800 + 60,
                               output, sizeof(output));
    CHECK(size != 0);
    command = find_aurora(output, size, 0x0011, 0);
    CHECK(command >= 0);
    CHECK(read_be32(output + command + 11) == 60); /* 4 * 12 + XYZ */
    gx_translate_destroy(context);

    context = gx_translate_create();
    memset(&fifo, 0, sizeof(fifo));
    cp(&fifo, 0xBC, 16);
    cp(&fifo, 0xAC, 0x2000);
    push_u8(&fifo, 0x20);
    push_be16(&fifo, 3);
    push_be16(&fifo, (uint16_t)((2u - 1u) << 12));
    size = gx_translate_stream(context, fifo.data, fifo.size, ram, 0x2000 + 56,
                               output, sizeof(output));
    CHECK(size != 0);
    command = find_aurora(output, size, 0x001C, 0);
    CHECK(command >= 0);
    CHECK(read_be32(output + command + 11) == 56); /* 3 * 16 + 2 words */
    CHECK((uint32_t)command + 16u < size && output[command + 16] == 0x20);
    gx_translate_destroy(context);
}

static void test_nested_display_list(void)
{
    uint8_t ram[0x10000] = {0};
    uint8_t output[4096];
    const uint8_t indices[2] = {3, 7};
    Buffer nested = {{0}, 0};
    Buffer top = {{0}, 0};
    GxTranslateContext* context = gx_translate_create();
    uint32_t size;
    int command;
    const GxTranslateStats* stats;
    CHECK(context != NULL);
    cp(&nested, 0x50, 2u << 9);
    cp(&nested, 0x70, 1u | (4u << 1));
    cp(&nested, 0xB0, 12);
    cp(&nested, 0xA0, sizeof(ram) - 96); /* exact source at the RAM boundary */
    draw(&nested, 0x90, indices, 2, sizeof(indices));
    memcpy(ram + 0x3000, nested.data, nested.size);
    call_dl(&top, 0x3000, nested.size);
    size = gx_translate_stream(context, top.data, top.size, ram, sizeof(ram),
                               output, sizeof(output));
    CHECK(size != 0);
    command = find_aurora(output, size, 0x0010, 0);
    CHECK(command >= 0);
    CHECK(read_be32(output + command + 11) == 96);
    stats = gx_translate_stats(context);
    CHECK(stats->display_lists_inlined == 1);
    CHECK(stats->display_list_bytes == nested.size);
    CHECK(stats->draws == 1);
    gx_translate_destroy(context);
}

static void test_texture_tlut_and_revisions(void)
{
    uint8_t ram[0x10000] = {0};
    uint8_t output[4096];
    Buffer first = {{0}, 0};
    Buffer point = {{0}, 0};
    GxTranslateContext* context = gx_translate_create();
    const GxTranslateStats* stats;
    uint32_t size, version1, version2, tlut_version1, tlut_version2;
    int texture, tlut;
    CHECK(context != NULL);
    setup_direct_position(&first);
    setup_c8_texture(&first, 0x2000, 0x1800, 7);
    append_direct_point(&first);
    size = gx_translate_stream(context, first.data, first.size, ram, sizeof(ram),
                               output, sizeof(output));
    CHECK(size != 0);
    tlut = find_aurora(output, size, 0x0031, 0);
    texture = find_aurora(output, size, 0x0030, 0);
    CHECK(tlut >= 0 && texture >= 0 && tlut < texture);
    CHECK(read_be16(output + tlut + 16) == 16);
    CHECK(read_be32(output + texture + 12) == 8);
    CHECK(read_be32(output + texture + 16) == 8);
    CHECK(read_be32(output + texture + 20) == 9);
    CHECK(output[texture + 28] == 1);
    tlut_version1 = read_be32(output + tlut + 22);
    version1 = read_be32(output + texture + 33);
    stats = gx_translate_stats(context);
    CHECK(stats->texture_source_bytes == 128);
    CHECK(stats->tlut_source_bytes == 32);

    setup_direct_position(&point);
    append_direct_point(&point);
    size = gx_translate_stream(context, point.data, point.size, ram, sizeof(ram),
                               output, sizeof(output));
    CHECK(size != 0);
    CHECK(find_aurora(output, size, 0x0030, 0) < 0);

    ram[0x2000] ^= 0xFF;
    gx_translate_invalidate_ram(context, 0x2000, 1);
    size = gx_translate_stream(context, point.data, point.size, ram, sizeof(ram),
                               output, sizeof(output));
    CHECK(size != 0);
    texture = find_aurora(output, size, 0x0030, 0);
    CHECK(texture >= 0);
    version2 = read_be32(output + texture + 33);
    CHECK(version2 > version1);
    CHECK(find_aurora(output, size, 0x0031, 0) < 0);

    /* A CPU write to the palette source is invisible until the next load
     * trigger: the TLUT models TMEM, which snapshotted at BP 0x65. */
    gx_translate_invalidate_ram(context, 0x1800, 2);
    size = gx_translate_stream(context, point.data, point.size, ram, sizeof(ram),
                               output, sizeof(output));
    CHECK(size != 0);
    CHECK(find_aurora(output, size, 0x0031, 0) < 0);

    memset(&point, 0, sizeof(point));
    bp(&point, 0x64, 0x1800u >> 5);
    bp(&point, 0x65, 7u | (1u << 10));
    setup_direct_position(&point);
    append_direct_point(&point);
    size = gx_translate_stream(context, point.data, point.size, ram, sizeof(ram),
                               output, sizeof(output));
    CHECK(size != 0);
    tlut = find_aurora(output, size, 0x0031, 0);
    texture = find_aurora(output, size, 0x0030, 0);
    CHECK(tlut >= 0 && texture >= 0);
    tlut_version2 = read_be32(output + tlut + 22);
    CHECK(tlut_version2 > tlut_version1);
    gx_translate_destroy(context);
}

/* One TMEM palette can be interpreted differently by two GX texture units.
 * The unused/stale unit must never recolor the live unit (Adventure ReDead). */
static void test_shared_palette_distinct_formats(void)
{
    uint8_t ram[0x10000] = {0};
    uint8_t output[4096];
    Buffer fifo = {{0}, 0};
    GxTranslateContext* context = gx_translate_create();
    CHECK(context != NULL);
    setup_direct_position(&fifo);
    setup_c8_texture(&fifo, 0x2000, 0x1800, 7);
    bp(&fifo, 0x81, 0);
    bp(&fifo, 0x85, 0);
    bp(&fifo, 0x89, 7u | (7u << 10) | (9u << 20));
    bp(&fifo, 0x95, 0x3000u >> 5);
    bp(&fifo, 0x99, 7u | (2u << 10));
    append_direct_point(&fifo);
    uint32_t size = gx_translate_stream(context, fifo.data, fifo.size, ram, sizeof(ram), output, sizeof(output));
    CHECK(size != 0);
    int a = find_aurora(output, size, 0x0031, 0);
    int b = find_aurora(output, size, 0x0031, (uint32_t)a + 26u);
    CHECK(a >= 0 && b >= 0);
    CHECK(output[a + 3] != output[b + 3]);
    CHECK(read_be32(output + a + 12) == 1);
    CHECK(read_be32(output + b + 12) == 2);
    int ta = find_aurora(output, size, 0x0030, 0);
    int tb = find_aurora(output, size, 0x0030, (uint32_t)ta + 37u);
    CHECK(ta >= 0 && tb >= 0);
    CHECK(read_be32(output + ta + 24) == output[a + 3]);
    CHECK(read_be32(output + tb + 24) == output[b + 3]);
    uint32_t va = read_be32(output + a + 22), vb = read_be32(output + b + 22);
    memset(&fifo, 0, sizeof(fifo));
    bp(&fifo, 0x64, 0x1800u >> 5);
    bp(&fifo, 0x65, 7u | (1u << 10));
    append_direct_point(&fifo);
    size = gx_translate_stream(context, fifo.data, fifo.size, ram, sizeof(ram), output, sizeof(output));
    a = find_aurora(output, size, 0x0031, 0);
    b = find_aurora(output, size, 0x0031, (uint32_t)a + 26u);
    CHECK(a >= 0 && b >= 0 && output[a + 3] != output[b + 3]);
    CHECK(read_be32(output + a + 22) > va && read_be32(output + b + 22) > vb);
    gx_translate_destroy(context);
}

static void test_bp_mask_and_transactional_retry(void)
{
    uint8_t ram[0x10000] = {0};
    uint8_t output[4096];
    uint8_t tiny[8];
    Buffer first = {{0}, 0};
    Buffer masked = {{0}, 0};
    GxTranslateContext* context = gx_translate_create();
    const GxTranslateStats* stats;
    uint32_t size;
    int texture;
    CHECK(context != NULL);
    setup_direct_position(&first);
    setup_c8_texture(&first, 0x2000, 0x1800, 7);
    append_direct_point(&first);

    size = gx_translate_stream(context, first.data, first.size, ram, sizeof(ram),
                               tiny, sizeof(tiny));
    CHECK(size == 0);
    CHECK(gx_translate_last_error(context) == GXT_ERROR_OUTPUT_OVERFLOW);
    CHECK(gx_translate_stats(context)->streams == 0);
    size = gx_translate_stream(context, first.data, first.size, ram, sizeof(ram),
                               output, sizeof(output));
    CHECK(size != 0);
    stats = gx_translate_stats(context);
    CHECK(stats->streams == 1);
    CHECK(stats->texture_objects_emitted == 1);
    CHECK(stats->tluts_emitted == 1);

    bp(&masked, 0xFE, 0x000003FFu);       /* update width bits only */
    bp(&masked, 0x88, 15u);               /* width = 16 */
    setup_direct_position(&masked);
    append_direct_point(&masked);
    size = gx_translate_stream(context, masked.data, masked.size, ram, sizeof(ram),
                               output, sizeof(output));
    CHECK(size != 0);
    texture = find_aurora(output, size, 0x0030, 0);
    CHECK(texture >= 0);
    CHECK(read_be32(output + texture + 12) == 16);
    CHECK(read_be32(output + texture + 16) == 8);
    CHECK(read_be32(output + texture + 20) == 9);
    gx_translate_destroy(context);
}

static void test_malformed_stream_is_rejected(void)
{
    uint8_t ram[256] = {0};
    uint8_t output[256];
    const uint8_t truncated_cp[2] = {0x08, 0x50};
    const uint8_t bad_opcode[1] = {0x7F};
    GxTranslateContext* context = gx_translate_create();
    CHECK(context != NULL);
    CHECK(gx_translate_stream(context, truncated_cp, sizeof(truncated_cp), ram, sizeof(ram),
                              output, sizeof(output)) == 0);
    CHECK(gx_translate_last_error(context) == GXT_ERROR_TRUNCATED_COMMAND);
    CHECK(gx_translate_last_error_opcode(context) == 0x08);
    CHECK(gx_translate_stream(context, bad_opcode, sizeof(bad_opcode), ram, sizeof(ram),
                              output, sizeof(output)) == 0);
    CHECK(gx_translate_last_error(context) == GXT_ERROR_UNKNOWN_OPCODE);
    gx_translate_destroy(context);
}

static void test_retail_single_byte_commands_and_zero_draw(void)
{
    uint8_t ram[256] = {0};
    uint8_t output[256];
    const uint8_t fifo[] = {0x44, 0x90, 0x00, 0x00};
    GxTranslateContext* context = gx_translate_create();
    uint32_t size;
    CHECK(context != NULL);
    size = gx_translate_stream(context, fifo, sizeof(fifo), ram, sizeof(ram),
                               output, sizeof(output));
    CHECK(size == sizeof(fifo));
    CHECK(output[0] == 0x00); /* metrics rewritten so Aurora cannot see CALL_DL */
    CHECK(output[1] == 0x90 && output[2] == 0 && output[3] == 0);
    CHECK(gx_translate_stats(context)->draws == 0);
    gx_translate_destroy(context);

    context = gx_translate_create();
    CHECK(context != NULL);
    {
        Buffer empty_dl = {{0}, 0};
        call_dl(&empty_dl, 0x0FFFFFFFu, 0);
        push_u8(&empty_dl, 0x00);
        size = gx_translate_stream(context, empty_dl.data, empty_dl.size,
                                   ram, sizeof(ram), output, sizeof(output));
        CHECK(size == 1 && output[0] == 0x00);
    }
    gx_translate_destroy(context);
}

static void test_array_snapshot_immutability(void)
{
    uint8_t ram[0x10000];
    uint8_t output[4096];
    const uint8_t idx[1] = {1};
    Buffer fifo = {{0}, 0};
    Buffer redraw = {{0}, 0};
    GxTranslateContext* context = gx_translate_create();
    uint32_t size;
    int command;
    uint64_t pointer1, pointer2;
    const uint8_t* snapshot;
    CHECK(context != NULL);
    memset(ram, 0xAA, sizeof(ram));

    cp(&fifo, 0x50, 2u << 9);              /* POS INDEX8 */
    cp(&fifo, 0x70, 1u | (4u << 1));       /* XYZ F32 */
    cp(&fifo, 0xB0, 12);
    cp(&fifo, 0xA0, 0x1000);
    draw(&fifo, 0x90, idx, 1, sizeof(idx));
    size = gx_translate_stream(context, fifo.data, fifo.size, ram, sizeof(ram),
                               output, sizeof(output));
    CHECK(size != 0);
    command = find_aurora(output, size, 0x0010, 0);
    CHECK(command >= 0);
    pointer1 = read_be64(output + command + 3);
    CHECK(pointer1 != (uint64_t)(uintptr_t)(ram + 0x1000));
    snapshot = (const uint8_t*)(uintptr_t)pointer1;
    CHECK(snapshot[0] == 0xAA);

    /* Mutating the source must leave the emitted snapshot intact and produce
     * a fresh snapshot with the new bytes on the next translation. */
    memset(ram + 0x1000, 0x55, 32);
    gx_translate_invalidate_ram(context, 0x1000, 32);
    draw(&redraw, 0x90, idx, 1, sizeof(idx));
    size = gx_translate_stream(context, redraw.data, redraw.size, ram, sizeof(ram),
                               output, sizeof(output));
    CHECK(size != 0);
    command = find_aurora(output, size, 0x0010, 0);
    CHECK(command >= 0);
    pointer2 = read_be64(output + command + 3);
    CHECK(pointer2 != pointer1);
    CHECK(((const uint8_t*)(uintptr_t)pointer2)[0] == 0x55);
    CHECK(snapshot[0] == 0xAA);   /* superseded block lives until the drain */
    gx_translate_frame_drained(context);
    gx_translate_destroy(context);
}

static void test_texture_snapshot_immutability(void)
{
    uint8_t ram[0x10000] = {0};
    uint8_t output[4096];
    Buffer fifo = {{0}, 0};
    Buffer redraw = {{0}, 0};
    GxTranslateContext* context = gx_translate_create();
    uint32_t size, version1, version2;
    int texture;
    uint64_t pointer1, pointer2;
    const uint8_t* snapshot;
    CHECK(context != NULL);
    memset(ram + 0x2000, 0x33, 128);

    setup_direct_position(&fifo);
    setup_c8_texture(&fifo, 0x2000, 0x1800, 7);
    append_direct_point(&fifo);
    size = gx_translate_stream(context, fifo.data, fifo.size, ram, sizeof(ram),
                               output, sizeof(output));
    CHECK(size != 0);
    texture = find_aurora(output, size, 0x0030, 0);
    CHECK(texture >= 0);
    pointer1 = read_be64(output + texture + 4);
    version1 = read_be32(output + texture + 33);
    CHECK(pointer1 != (uint64_t)(uintptr_t)(ram + 0x2000));
    snapshot = (const uint8_t*)(uintptr_t)pointer1;
    CHECK(snapshot[0] == 0x33);

    memset(ram + 0x2000, 0x44, 128);
    gx_translate_invalidate_ram(context, 0x2000, 128);
    setup_direct_position(&redraw);
    append_direct_point(&redraw);
    size = gx_translate_stream(context, redraw.data, redraw.size, ram, sizeof(ram),
                               output, sizeof(output));
    CHECK(size != 0);
    texture = find_aurora(output, size, 0x0030, 0);
    CHECK(texture >= 0);
    pointer2 = read_be64(output + texture + 4);
    version2 = read_be32(output + texture + 33);
    CHECK(pointer2 != pointer1);
    CHECK(version2 > version1);
    CHECK(((const uint8_t*)(uintptr_t)pointer2)[0] == 0x44);
    CHECK(snapshot[0] == 0x33);
    gx_translate_frame_drained(context);
    gx_translate_destroy(context);
}

static void test_false_sharing_invalidation_absorbed(void)
{
    uint8_t ram[0x10000] = {0};
    uint8_t output[4096];
    const uint8_t idx[1] = {1};
    Buffer fifo = {{0}, 0};
    Buffer redraw = {{0}, 0};
    GxTranslateContext* context = gx_translate_create();
    uint32_t size;
    uint64_t staged_before;
    CHECK(context != NULL);

    setup_c8_texture(&fifo, 0x2000, 0x1800, 7);
    cp(&fifo, 0x50, 2u << 9);              /* POS INDEX8 */
    cp(&fifo, 0x70, 1u | (4u << 1));       /* XYZ F32 */
    cp(&fifo, 0xB0, 12);
    cp(&fifo, 0xA0, 0x1000);
    draw(&fifo, 0x90, idx, 1, sizeof(idx));
    size = gx_translate_stream(context, fifo.data, fifo.size, ram, sizeof(ram),
                               output, sizeof(output));
    CHECK(size != 0);
    staged_before = gx_translate_stats(context)->staged_copies;

    /* Invalidate the texture and array pages WITHOUT changing any bytes: a
     * neighboring-store epoch bump must not re-upload anything. */
    gx_translate_invalidate_ram(context, 0x1000, 32);
    gx_translate_invalidate_ram(context, 0x2000, 128);
    draw(&redraw, 0x90, idx, 1, sizeof(idx));
    size = gx_translate_stream(context, redraw.data, redraw.size, ram, sizeof(ram),
                               output, sizeof(output));
    CHECK(size != 0);
    CHECK(find_aurora(output, size, 0x0030, 0) < 0);   /* no texture re-emit */
    CHECK(gx_translate_stats(context)->staged_copies == staged_before);
    gx_translate_destroy(context);
}

static void test_cache_eviction_invalidates_texture_borrower(void)
{
    enum {
        RAM_SIZE = 0x900000,
        CACHE_COLLISION_PERIOD = 0x20000,
        COLLIDERS = 64
    };
    uint8_t* ram = (uint8_t*)calloc(1, RAM_SIZE);
    uint8_t output[4096];
    Buffer first = {{0}, 0};
    GxTranslateContext* context = gx_translate_create();
    uint32_t i, size;
    int saw_texture0_rebind = 0;
    CHECK(ram != NULL);
    CHECK(context != NULL);

    /* These physical addresses have the same cache bucket. Texture 0 keeps
     * borrowing the first snapshot while texture 1 fills and rolls the probe
     * window. Evicting texture 0's cache entry must dirty that binding so it
     * is rebound before the retired snapshot is freed. */
    setup_direct_position(&first);
    bp(&first, 0x88, 0); /* texture 0 image0: 1x1 I4 */
    bp(&first, 0x94, 0x1000u >> 5);
    bp(&first, 0x89, 0); /* texture 1 image0: 1x1 I4 */
    bp(&first, 0x95, (0x1000u + CACHE_COLLISION_PERIOD) >> 5);
    append_direct_point(&first);
    size = gx_translate_stream(context, first.data, first.size, ram, RAM_SIZE,
                               output, sizeof(output));
    CHECK(size != 0);
    CHECK(count_texture_id(output, size, 0) == 1);
    CHECK(count_texture_id(output, size, 1) == 1);
    gx_translate_frame_drained(context);

    for (i = 2; i <= COLLIDERS; ++i) {
        Buffer step = {{0}, 0};
        const uint32_t phys = 0x1000u + i * CACHE_COLLISION_PERIOD;
        bp(&step, 0x95, phys >> 5);
        append_direct_point(&step);
        size = gx_translate_stream(context, step.data, step.size, ram, RAM_SIZE,
                                   output, sizeof(output));
        CHECK(size != 0);
        if (count_texture_id(output, size, 0) != 0)
            saw_texture0_rebind = 1;
        gx_translate_frame_drained(context);
    }
    CHECK(saw_texture0_rebind);
    gx_translate_destroy(context);
    free(ram);
}

static void test_tlut_snapshot_at_load_trigger(void)
{
    uint8_t ram[0x10000] = {0};
    uint8_t output[4096];
    Buffer fifo = {{0}, 0};
    Buffer redraw = {{0}, 0};
    Buffer reload = {{0}, 0};
    GxTranslateContext* context = gx_translate_create();
    uint32_t size;
    int tlut;
    uint64_t pointer1, pointer2;
    CHECK(context != NULL);
    memset(ram + 0x1800, 0x11, 32);

    setup_direct_position(&fifo);
    setup_c8_texture(&fifo, 0x2000, 0x1800, 7);
    append_direct_point(&fifo);
    size = gx_translate_stream(context, fifo.data, fifo.size, ram, sizeof(ram),
                               output, sizeof(output));
    CHECK(size != 0);
    tlut = find_aurora(output, size, 0x0031, 0);
    CHECK(tlut >= 0);
    pointer1 = read_be64(output + tlut + 4);
    CHECK(pointer1 != (uint64_t)(uintptr_t)(ram + 0x1800));
    CHECK(((const uint8_t*)(uintptr_t)pointer1)[0] == 0x11);

    /* Without a reload, the mutation stays invisible (TMEM semantics). */
    memset(ram + 0x1800, 0x22, 32);
    gx_translate_invalidate_ram(context, 0x1800, 32);
    setup_direct_position(&redraw);
    append_direct_point(&redraw);
    size = gx_translate_stream(context, redraw.data, redraw.size, ram, sizeof(ram),
                               output, sizeof(output));
    CHECK(size != 0);
    CHECK(find_aurora(output, size, 0x0031, 0) < 0);
    CHECK(((const uint8_t*)(uintptr_t)pointer1)[0] == 0x11);

    /* Reloading snapshots the new palette bytes at the trigger. */
    bp(&reload, 0x64, 0x1800u >> 5);
    bp(&reload, 0x65, 7u | (1u << 10));
    setup_direct_position(&reload);
    append_direct_point(&reload);
    size = gx_translate_stream(context, reload.data, reload.size, ram, sizeof(ram),
                               output, sizeof(output));
    CHECK(size != 0);
    tlut = find_aurora(output, size, 0x0031, 0);
    CHECK(tlut >= 0);
    pointer2 = read_be64(output + tlut + 4);
    CHECK(pointer2 != pointer1);
    CHECK(((const uint8_t*)(uintptr_t)pointer2)[0] == 0x22);
    gx_translate_frame_drained(context);
    gx_translate_destroy(context);
}

static void setup_texture_copy_state(Buffer* b, uint32_t dest_phys)
{
    bp(b, 0x43, 0);                    /* EFB pixel format RGB8_Z24 */
    bp(b, 0x49, 2u | (4u << 10));      /* source left=2 top=4 */
    bp(b, 0x4A, 15u | (7u << 10));     /* source 16x8 */
    bp(b, 0x4B, dest_phys >> 5);
    bp(b, 0x4D, 64u >> 5);             /* two 32-byte I8 tiles per row */
}

static void test_efb_texture_copy_metadata(void)
{
    uint8_t ram[0x10000] = {0};
    uint8_t output[4096];
    Buffer fifo = {{0}, 0};
    Buffer bind = {{0}, 0};
    GxTranslateContext* context = gx_translate_create();
    const GxTranslateStats* stats;
    uint32_t size;
    int copy_src, copy_dst, copy_dest, texture;
    uint64_t dest_pointer;
    CHECK(context != NULL);

    setup_texture_copy_state(&fifo, 0x4000);
    /* I8 copy: target field 2 encodes real format 1, intensity set. */
    bp(&fifo, 0x52, (2u << 3) | (1u << 15));
    size = gx_translate_stream(context, fifo.data, fifo.size, ram, sizeof(ram),
                               output, sizeof(output));
    CHECK(size != 0);
    copy_src = find_aurora(output, size, 0x0035, 0);
    copy_dst = find_aurora(output, size, 0x0036, 0);
    copy_dest = find_aurora(output, size, 0x0037, 0);
    CHECK(copy_src >= 0 && copy_dst > copy_src && copy_dest > copy_dst);
    CHECK(read_be32(output + copy_src + 3) == 2);
    CHECK(read_be32(output + copy_src + 7) == 4);
    CHECK(read_be32(output + copy_src + 11) == 16);
    CHECK(read_be32(output + copy_src + 15) == 8);
    CHECK(read_be32(output + copy_dst + 3) == 16);
    CHECK(read_be32(output + copy_dst + 7) == 8);
    CHECK(read_be32(output + copy_dst + 11) == 0x1); /* GX_TF_I8 */
    CHECK(output[copy_dst + 15] == 0);               /* no mipmap */
    dest_pointer = read_be64(output + copy_dest + 3);
    CHECK(dest_pointer == (uint64_t)(uintptr_t)(ram + 0x4000));
    /* The raw BP trigger must directly follow the metadata. */
    CHECK(output[copy_dest + 11] == 0x61 && output[copy_dest + 12] == 0x52);
    stats = gx_translate_stats(context);
    CHECK(stats->texture_copies == 1);
    CHECK(stats->display_copies == 0);

    /* Binding the copy destination must reuse the exact same host pointer so
     * Aurora resolves it from its copy-texture cache. */
    setup_direct_position(&bind);
    bp(&bind, 0x80, 0);
    bp(&bind, 0x84, 0);
    bp(&bind, 0x88, 15u | (7u << 10) | (1u << 20)); /* I8 16x8 */
    bp(&bind, 0x94, 0x4000u >> 5);
    append_direct_point(&bind);
    size = gx_translate_stream(context, bind.data, bind.size, ram, sizeof(ram),
                               output, sizeof(output));
    CHECK(size != 0);
    texture = find_aurora(output, size, 0x0030, 0);
    CHECK(texture >= 0);
    CHECK(read_be64(output + texture + 4) == dest_pointer);
    gx_translate_destroy(context);
}

static void test_efb_copy_half_scale_and_masked_format(void)
{
    uint8_t ram[0x10000] = {0};
    uint8_t output[4096];
    Buffer fifo = {{0}, 0};
    GxTranslateContext* context = gx_translate_create();
    uint32_t size;
    int copy_dst;
    CHECK(context != NULL);

    setup_texture_copy_state(&fifo, 0x6000);
    /* RGBA8 copy at half scale: target field 12 encodes real format 6.  The
     * format bits arrive through a BP mask write to prove masked state feeds
     * the metadata. */
    bp(&fifo, 0x52, 1u << 9);
    bp(&fifo, 0xFE, 0x000078u);
    bp(&fifo, 0x52, (12u << 3) | (1u << 9));
    size = gx_translate_stream(context, fifo.data, fifo.size, ram, sizeof(ram),
                               output, sizeof(output));
    CHECK(size != 0);
    copy_dst = find_aurora(output, size, 0x0036, 0);
    CHECK(copy_dst >= 0);
    copy_dst = find_aurora(output, size, 0x0036, (uint32_t)copy_dst + 3u);
    CHECK(copy_dst >= 0);                            /* second trigger */
    CHECK(read_be32(output + copy_dst + 3) == 8);    /* 16x8 halved */
    CHECK(read_be32(output + copy_dst + 7) == 4);
    CHECK(read_be32(output + copy_dst + 11) == 0x6); /* GX_TF_RGBA8 */
    CHECK(output[copy_dst + 15] == 1);
    CHECK(gx_translate_stats(context)->texture_copies == 2);
    gx_translate_destroy(context);
}

static void test_display_copy_is_dropped(void)
{
    uint8_t ram[0x10000] = {0};
    uint8_t output[4096];
    Buffer fifo = {{0}, 0};
    GxTranslateContext* context = gx_translate_create();
    uint32_t size, offset;
    CHECK(context != NULL);

    bp(&fifo, 0x4A, 15u | (7u << 10));
    bp(&fifo, 0x4B, 0x4000u >> 5);
    bp(&fifo, 0x52, 1u << 14);
    size = gx_translate_stream(context, fifo.data, fifo.size, ram, sizeof(ram),
                               output, sizeof(output));
    CHECK(size == fifo.size);
    CHECK(find_aurora(output, size, 0x0035, 0) < 0);
    CHECK(find_aurora(output, size, 0x0037, 0) < 0);
    for (offset = 0; offset + 1u < size; ++offset)
        CHECK(!(output[offset] == 0x61 && output[offset + 1] == 0x52));
    CHECK(gx_translate_stats(context)->display_copies == 1);
    CHECK(gx_translate_stats(context)->texture_copies == 0);
    gx_translate_destroy(context);
}

static void test_copy_dest_evicts_when_overwritten(void)
{
    uint8_t ram[0x10000] = {0};
    uint8_t output[4096];
    Buffer fifo = {{0}, 0};
    Buffer bind = {{0}, 0};
    Buffer rebind = {{0}, 0};
    GxTranslateContext* context = gx_translate_create();
    uint32_t size;
    int texture, destroy;
    CHECK(context != NULL);

    /* EFB-copy to 0x4000, then bind it: live-RAM pointer (cache key). */
    setup_texture_copy_state(&fifo, 0x4000);
    bp(&fifo, 0x52, (2u << 3) | (1u << 15));
    setup_direct_position(&bind);
    bp(&bind, 0x80, 0);
    bp(&bind, 0x84, 0);
    bp(&bind, 0x88, 15u | (7u << 10) | (1u << 20)); /* I8 16x8 */
    bp(&bind, 0x94, 0x4000u >> 5);
    append_direct_point(&bind);
    CHECK(gx_translate_stream(context, fifo.data, fifo.size, ram, sizeof(ram),
                              output, sizeof(output)) != 0);
    size = gx_translate_stream(context, bind.data, bind.size, ram, sizeof(ram),
                               output, sizeof(output));
    CHECK(size != 0);
    texture = find_aurora(output, size, 0x0030, 0);
    CHECK(texture >= 0);
    CHECK(read_be64(output + texture + 4) == (uint64_t)(uintptr_t)(ram + 0x4000));

    /* An unrelated object shares the copy's 4 KiB page. Updating it must
     * retain the GPU image, even though the page epoch changes. */
    ram[0x4800] = 0xAB;
    gx_translate_invalidate_ram(context, 0x4800, 1);
    setup_direct_position(&rebind);
    append_direct_point(&rebind);
    size = gx_translate_stream(context, rebind.data, rebind.size, ram, sizeof(ram),
                               output, sizeof(output));
    CHECK(size != 0);
    CHECK(find_aurora(output, size, 0x0034, 0) < 0);
    texture = find_aurora(output, size, 0x0030, 0);
    if (texture >= 0)
        CHECK(read_be64(output + texture + 4) == (uint64_t)(uintptr_t)(ram + 0x4000));
    rebind.size = 0;

    /* The guest overwrites the destination with real texture data: the copy
     * cache entry must be destroyed and the bind must switch to a snapshot,
     * or the stale GPU copy aliases the new texture (black stage geometry). */
    memset(ram + 0x4000, 0x77, 128);
    gx_translate_invalidate_ram(context, 0x4000, 128);
    setup_direct_position(&rebind);
    append_direct_point(&rebind);
    size = gx_translate_stream(context, rebind.data, rebind.size, ram, sizeof(ram),
                               output, sizeof(output));
    CHECK(size != 0);
    destroy = find_aurora(output, size, 0x0034, 0);
    texture = find_aurora(output, size, 0x0030, 0);
    CHECK(destroy >= 0);
    CHECK(read_be64(output + destroy + 3) == (uint64_t)(uintptr_t)(ram + 0x4000));
    CHECK(texture > destroy);
    CHECK(read_be64(output + texture + 4) != (uint64_t)(uintptr_t)(ram + 0x4000));
    CHECK(((const uint8_t*)(uintptr_t)read_be64(output + texture + 4))[0] == 0x77);
    gx_translate_destroy(context);
}

static void test_invalidation_interest_filter(void)
{
    static uint8_t ram[0x200000];
    uint8_t output[4096];
    memset(ram, 0, sizeof(ram));
    const uint8_t idx[1] = {1};
    Buffer fifo = {{0}, 0};
    GxTranslateContext* context = gx_translate_create();
    const GxTranslateStats* stats;
    uint64_t pages_before;
    CHECK(context != NULL);

    /* A store far from every consumed region takes the fast-reject path and
     * stamps no page epochs. */
    gx_translate_invalidate_ram(context, 0x100000, 64);
    stats = gx_translate_stats(context);
    CHECK(stats->ram_invalidations == 1);
    CHECK(stats->ram_invalidation_pages == 0);

    /* Consuming an array in that region registers interest, after which the
     * same store stamps epochs again. */
    cp(&fifo, 0x50, 2u << 9);
    cp(&fifo, 0x70, 1u | (4u << 1));
    cp(&fifo, 0xB0, 12);
    cp(&fifo, 0xA0, 0x100000);
    draw(&fifo, 0x90, idx, 1, sizeof(idx));
    CHECK(gx_translate_stream(context, fifo.data, fifo.size, ram, sizeof(ram),
                              output, sizeof(output)) != 0);
    pages_before = gx_translate_stats(context)->ram_invalidation_pages;
    gx_translate_invalidate_ram(context, 0x100000, 64);
    CHECK(gx_translate_stats(context)->ram_invalidation_pages > pages_before);
    gx_translate_destroy(context);
}

static void test_texture_copy_without_destination_fails(void)
{
    uint8_t ram[0x10000] = {0};
    uint8_t output[4096];
    Buffer fifo = {{0}, 0};
    GxTranslateContext* context = gx_translate_create();
    CHECK(context != NULL);

    bp(&fifo, 0x4A, 15u | (7u << 10));
    bp(&fifo, 0x52, 2u << 3);          /* no BP 0x4B destination loaded */
    CHECK(gx_translate_stream(context, fifo.data, fifo.size, ram, sizeof(ram),
                              output, sizeof(output)) == 0);
    CHECK(gx_translate_last_error(context) == GXT_ERROR_RESOURCE_RANGE);
    CHECK(gx_translate_last_error_opcode(context) == 0x61);
    gx_translate_destroy(context);
}

typedef struct InvalidationThreadArgs {
    GxTranslateContext* context;
    uint32_t iterations;
    uint32_t address;
    volatile LONG* completed;
} InvalidationThreadArgs;

static DWORD
#ifdef _WIN32
WINAPI
#endif
invalidation_thread(void* opaque)
{
    InvalidationThreadArgs* args = (InvalidationThreadArgs*)opaque;
    uint32_t i;
    for (i = 0; i < args->iterations; ++i)
        gx_translate_invalidate_ram(args->context, args->address + (i & 31u), 1);
    InterlockedIncrement(args->completed);
    return 0;
}

static void test_concurrent_invalidation(void)
{
    enum { THREADS = 4, ITERATIONS = 25000 };
    uint8_t ram[0x10000] = {0};
    uint8_t output[4096];
    Buffer first = {{0}, 0};
    Buffer point = {{0}, 0};
    GxTranslateContext* context = gx_translate_create();
    InvalidationThreadArgs args[THREADS];
#ifdef _WIN32
    HANDLE threads[THREADS];
#else
    pthread_t threads[THREADS];
#endif
    volatile LONG completed = 0;
    const GxTranslateStats* stats;
    uint32_t i, size;
    int translation_failed = 0;
    CHECK(context != NULL);
    setup_direct_position(&first);
    setup_c8_texture(&first, 0x2000, 0x1800, 7);
    append_direct_point(&first);
    CHECK(gx_translate_stream(context, first.data, first.size, ram, sizeof(ram),
                              output, sizeof(output)) != 0);
    setup_direct_position(&point);
    append_direct_point(&point);

    for (i = 0; i < THREADS; ++i) {
        args[i].context = context;
        args[i].iterations = ITERATIONS;
        args[i].address = (i & 1u) ? 0x1800u : 0x2000u;
        args[i].completed = &completed;
#ifdef _WIN32
        threads[i] = CreateThread(NULL, 0, invalidation_thread, &args[i], 0, NULL);
        CHECK(threads[i] != NULL);
#else
        threads[i] = CreateThreadSimple(0, invalidation_thread, &args[i]);
        CHECK(threads[i] != 0);
#endif
    }
    while (InterlockedCompareExchange(&completed, 0, 0) != THREADS) {
        size = gx_translate_stream(context, point.data, point.size, ram, sizeof(ram),
                                   output, sizeof(output));
        if (!size) translation_failed = 1;
        Sleep(0);
    }
#ifdef _WIN32
    CHECK(WaitForMultipleObjects(THREADS, threads, TRUE, INFINITE) == WAIT_OBJECT_0);
    for (i = 0; i < THREADS; ++i) CloseHandle(threads[i]);
#else
    for (i = 0; i < THREADS; ++i) CHECK(platform_thread_join(threads[i], INFINITE) == WAIT_OBJECT_0);
#endif
    CHECK(!translation_failed);
    CHECK(gx_translate_stream(context, point.data, point.size, ram, sizeof(ram),
                              output, sizeof(output)) != 0);
    stats = gx_translate_stats(context);
    CHECK(stats->ram_invalidations == (uint64_t)THREADS * ITERATIONS);
    CHECK(stats->ram_invalidation_pages == (uint64_t)THREADS * ITERATIONS);
    /* The threads never changed the bytes, so their invalidations are
     * absorbed; a real content change still forces a rebind. */
    ram[0x2000] ^= 0xFF;
    gx_translate_invalidate_ram(context, 0x2000, 1);
    CHECK(gx_translate_stream(context, point.data, point.size, ram, sizeof(ram),
                              output, sizeof(output)) != 0);
    CHECK(gx_translate_stats(context)->resource_rebinds != 0);
    gx_translate_destroy(context);
}

int main(void)
{
    test_source_sizes();
    test_exact_indexed_array();
    test_nbt3_and_indexed_xf();
    test_nested_display_list();
    test_texture_tlut_and_revisions();
    test_shared_palette_distinct_formats();
    test_bp_mask_and_transactional_retry();
    test_malformed_stream_is_rejected();
    test_retail_single_byte_commands_and_zero_draw();
    test_array_snapshot_immutability();
    test_texture_snapshot_immutability();
    test_false_sharing_invalidation_absorbed();
    test_cache_eviction_invalidates_texture_borrower();
    test_tlut_snapshot_at_load_trigger();
    test_efb_texture_copy_metadata();
    test_efb_copy_half_scale_and_masked_format();
    test_display_copy_is_dropped();
    test_copy_dest_evicts_when_overwritten();
    test_invalidation_interest_filter();
    test_texture_copy_without_destination_fails();
    test_concurrent_invalidation();
    if (failures) {
        fprintf(stderr, "%d gx_translate test(s) failed\n", failures);
        return 1;
    }
    puts("gx_translate tests passed");
    return 0;
}
