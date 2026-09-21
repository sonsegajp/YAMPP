// RVZ/WIA disc image reading. See gxruntime/rvz.h for what the format is.
//
// The layout: a header naming the compression and chunk size, a table of raw
// data ranges (a GameCube disc is one or a few), and a table of groups. Each
// group holds one chunk of the disc, compressed. RVZ additionally packs a group
// as a run-length stream of "here are N stored bytes" and "here are N bytes of
// padding, regenerate them from this seed", which is what lets it drop the
// padding that fills most of a disc.
//
// The padding generator and the packed stream layout follow Dolphin's
// DiscIO/LaggedFibonacciGenerator.cpp and DiscIO/WIACompression.cpp, which are
// the reference for this format (CC0 / GPL-2.0-or-later respectively).
#include "gxruntime/rvz.h"

#ifndef _WIN32
#define _fseeki64 fseeko
#endif
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zstd.h>

#define WIA_MAGIC 0x57494101u   // "WIA\1"
#define RVZ_MAGIC 0x52565A01u   // "RVZ\1"

#define COMPRESSION_NONE 0
#define COMPRESSION_PURGE 1
#define COMPRESSION_BZIP2 2
#define COMPRESSION_LZMA 3
#define COMPRESSION_LZMA2 4
#define COMPRESSION_ZSTD 5

#define JUNK_BLOCK_SIZE 0x8000u   // padding restarts on each 32 KiB block

static char s_error[256];

static void fail(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(s_error, sizeof s_error, fmt, args);
    va_end(args);
}

const char* rvz_last_error(void) { return s_error[0] ? s_error : "no error"; }

static uint32_t be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static uint64_t be64(const uint8_t* p) {
    return ((uint64_t)be32(p) << 32) | be32(p + 4);
}
static uint32_t swap32(uint32_t x) {
    return (x >> 24) | ((x >> 8) & 0xFF00u) | ((x << 8) & 0xFF0000u) | (x << 24);
}

// ---------------------------------------------------------------------------
// The padding generator: a lagged Fibonacci generator seeded with 17 words.
// ---------------------------------------------------------------------------
#define LFG_K 521
#define LFG_J 32
#define LFG_SEED_WORDS 17

typedef struct {
    uint32_t buffer[LFG_K];   // held byte-swapped, so the bytes read out are big-endian
    size_t position_bytes;
} Lfg;

static void lfg_step(Lfg* g) {
    for (size_t i = 0; i < LFG_J; ++i)
        g->buffer[i] ^= g->buffer[i + LFG_K - LFG_J];
    for (size_t i = LFG_J; i < LFG_K; ++i)
        g->buffer[i] ^= g->buffer[i - LFG_J];
}

static void lfg_seed(Lfg* g, const uint8_t* seed /* LFG_SEED_WORDS * 4 bytes */) {
    g->position_bytes = 0;
    for (size_t i = 0; i < LFG_SEED_WORDS; ++i)
        g->buffer[i] = be32(seed + i * 4);
    for (size_t i = LFG_SEED_WORDS; i < LFG_K; ++i) {
        g->buffer[i] = (g->buffer[i - 17] << 23) ^ (g->buffer[i - 16] >> 9) ^ g->buffer[i - 1];
    }
    // The generator's output shifts by 18 rather than 16; doing that here keeps
    // handing out bytes simple.
    for (size_t i = 0; i < LFG_K; ++i) {
        uint32_t x = g->buffer[i];
        g->buffer[i] = swap32((x & 0xFF00FFFFu) | ((x >> 2) & 0x00FF0000u));
    }
    for (int i = 0; i < 4; ++i)
        lfg_step(g);
}

static void lfg_skip(Lfg* g, size_t count) {
    g->position_bytes += count;
    while (g->position_bytes >= LFG_K * sizeof(uint32_t)) {
        lfg_step(g);
        g->position_bytes -= LFG_K * sizeof(uint32_t);
    }
}

static void lfg_bytes(Lfg* g, size_t count, uint8_t* out) {
    while (count > 0) {
        size_t length = LFG_K * sizeof(uint32_t) - g->position_bytes;
        if (length > count) length = count;
        memcpy(out, (const uint8_t*)g->buffer + g->position_bytes, length);
        g->position_bytes += length;
        count -= length;
        out += length;
        if (g->position_bytes == LFG_K * sizeof(uint32_t)) {
            lfg_step(g);
            g->position_bytes = 0;
        }
    }
}

// ---------------------------------------------------------------------------

typedef struct {
    uint64_t data_offset;
    uint64_t data_size;
    uint32_t group_index;
    uint32_t number_of_groups;
} RawDataEntry;

typedef struct {
    uint32_t data_offset;      // in 4-byte units
    uint32_t data_size;        // RVZ: bit 31 set when the group is compressed
    uint32_t rvz_packed_size;  // RVZ only, 0 when the group is not packed
} GroupEntry;

struct RvzImage {
    FILE* file;
    bool is_rvz;
    uint32_t compression;
    uint32_t chunk_size;
    uint64_t disc_size;

    RawDataEntry* raw;
    uint32_t raw_count;
    GroupEntry* groups;
    uint32_t group_count;

    uint8_t* chunk;          // the group currently decoded
    uint64_t chunk_group;    // which group that is, or UINT64_MAX
    size_t chunk_length;

    uint8_t* scratch;        // compressed bytes, then unpacked bytes
    size_t scratch_size;
};

static bool read_at(FILE* f, uint64_t offset, void* dst, size_t size) {
    return _fseeki64(f, (long long)offset, SEEK_SET) == 0 && fread(dst, 1, size, f) == size;
}

// Decompresses `in` into `out`, or copies it when the file is uncompressed.
static bool inflate_into(const RvzImage* image, const uint8_t* in, size_t in_size,
                         uint8_t* out, size_t out_size) {
    if (image->compression == COMPRESSION_NONE) {
        if (in_size < out_size) return false;
        memcpy(out, in, out_size);
        return true;
    }
    size_t got = ZSTD_decompress(out, out_size, in, in_size);
    if (ZSTD_isError(got)) {
        fail("compressed data could not be read (%s)", ZSTD_getErrorName(got));
        return false;
    }
    return got == out_size;
}

// Reads a compressed table (the raw-data and group lists are stored this way).
static uint8_t* read_table(RvzImage* image, uint64_t offset, uint32_t stored, size_t expanded) {
    uint8_t* packed = (uint8_t*)malloc(stored ? stored : 1);
    uint8_t* out = (uint8_t*)malloc(expanded ? expanded : 1);
    if (!packed || !out) { free(packed); free(out); fail("out of memory"); return NULL; }
    if (!read_at(image->file, offset, packed, stored) ||
        !inflate_into(image, packed, stored, out, expanded)) {
        free(packed); free(out);
        fail("the image's tables could not be read");
        return NULL;
    }
    free(packed);
    return out;
}

bool rvz_is_image(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    uint8_t magic[4] = {0};
    bool ok = fread(magic, 1, 4, f) == 4;
    fclose(f);
    if (!ok) return false;
    uint32_t value = be32(magic);
    return value == WIA_MAGIC || value == RVZ_MAGIC;
}

RvzImage* rvz_open(const char* path) {
    s_error[0] = 0;
    FILE* f = fopen(path, "rb");
    if (!f) { fail("could not open %s", path); return NULL; }

    uint8_t head1[0x48];
    if (!read_at(f, 0, head1, sizeof head1)) { fclose(f); fail("file is too short"); return NULL; }
    uint32_t magic = be32(head1);
    if (magic != WIA_MAGIC && magic != RVZ_MAGIC) { fclose(f); fail("not an RVZ or WIA image"); return NULL; }

    uint32_t head2_size = be32(head1 + 12);
    if (head2_size < 0xDC) { fclose(f); fail("image header is damaged"); return NULL; }
    uint8_t head2[0xDC];
    if (!read_at(f, sizeof head1, head2, sizeof head2)) { fclose(f); fail("file is too short"); return NULL; }

    RvzImage* image = (RvzImage*)calloc(1, sizeof *image);
    if (!image) { fclose(f); fail("out of memory"); return NULL; }
    image->file = f;
    image->is_rvz = magic == RVZ_MAGIC;
    /* WIAHeader1: four u32 fields, then the 20-byte header-2 SHA-1. */
    image->disc_size = be64(head1 + 0x24);
    image->compression = be32(head2 + 4);
    image->chunk_size = be32(head2 + 12);
    image->chunk_group = UINT64_MAX;

    uint32_t disc_type = be32(head2);
    if (disc_type != 1) {
        rvz_close(image);
        fail("this is a %s image; a GameCube disc is needed", disc_type == 2 ? "Wii" : "blank");
        return NULL;
    }
    if (image->compression != COMPRESSION_NONE && image->compression != COMPRESSION_ZSTD) {
        static const char* names[] = { "none", "purge", "bzip2", "LZMA", "LZMA2", "zstd" };
        const char* name = image->compression < 6 ? names[image->compression] : "an unknown method";
        rvz_close(image);
        fail("this image is compressed with %s; only zstd images can be read. "
             "Convert it with Dolphin (RVZ, zstd) and try again", name);
        return NULL;
    }
    if (image->chunk_size == 0 || image->chunk_size > 0x4000000u) {
        rvz_close(image);
        fail("image header is damaged (chunk size)");
        return NULL;
    }

    // Offsets within WIAHeader2. The partition fields and their hash sit between
    // the disc header and these, so they start at 0xB4, not 0x9C.
    image->raw_count = be32(head2 + 0xB4);
    uint64_t raw_offset = be64(head2 + 0xB8);
    uint32_t raw_stored = be32(head2 + 0xC0);
    image->group_count = be32(head2 + 0xC4);
    uint64_t group_offset = be64(head2 + 0xC8);
    uint32_t group_stored = be32(head2 + 0xD0);

    const size_t group_entry_size = image->is_rvz ? 12 : 8;
    if (!image->raw_count || !image->group_count) {
        rvz_close(image); fail("image contains no data"); return NULL;
    }

    uint8_t* raw_bytes = read_table(image, raw_offset, raw_stored, (size_t)image->raw_count * 24);
    if (!raw_bytes) { rvz_close(image); return NULL; }
    image->raw = (RawDataEntry*)calloc(image->raw_count, sizeof(RawDataEntry));
    if (!image->raw) { free(raw_bytes); rvz_close(image); fail("out of memory"); return NULL; }
    for (uint32_t i = 0; i < image->raw_count; ++i) {
        const uint8_t* p = raw_bytes + (size_t)i * 24;
        image->raw[i].data_offset = be64(p);
        image->raw[i].data_size = be64(p + 8);
        image->raw[i].group_index = be32(p + 16);
        image->raw[i].number_of_groups = be32(p + 20);
    }
    free(raw_bytes);

    uint8_t* group_bytes = read_table(image, group_offset, group_stored,
                                      (size_t)image->group_count * group_entry_size);
    if (!group_bytes) { rvz_close(image); return NULL; }
    image->groups = (GroupEntry*)calloc(image->group_count, sizeof(GroupEntry));
    if (!image->groups) { free(group_bytes); rvz_close(image); fail("out of memory"); return NULL; }
    for (uint32_t i = 0; i < image->group_count; ++i) {
        const uint8_t* p = group_bytes + (size_t)i * group_entry_size;
        image->groups[i].data_offset = be32(p);
        image->groups[i].data_size = be32(p + 4);
        image->groups[i].rvz_packed_size = image->is_rvz ? be32(p + 8) : 0;
    }
    free(group_bytes);

    image->chunk = (uint8_t*)malloc(image->chunk_size);
    if (!image->chunk) { rvz_close(image); fail("out of memory"); return NULL; }
    return image;
}

void rvz_close(RvzImage* image) {
    if (!image) return;
    if (image->file) fclose(image->file);
    free(image->raw);
    free(image->groups);
    free(image->chunk);
    free(image->scratch);
    free(image);
}

uint64_t rvz_disc_size(const RvzImage* image) { return image ? image->disc_size : 0; }

static bool reserve_scratch(RvzImage* image, size_t size) {
    if (image->scratch_size >= size) return true;
    uint8_t* grown = (uint8_t*)realloc(image->scratch, size);
    if (!grown) { fail("out of memory"); return false; }
    image->scratch = grown;
    image->scratch_size = size;
    return true;
}

// Expands one group into image->chunk. `chunk_length` is how much of the disc
// this group covers, which is smaller than the chunk size for the last group.
static bool load_group(RvzImage* image, uint64_t group_index, uint64_t group_offset_in_data,
                       size_t chunk_length) {
    if (image->chunk_group == group_index && image->chunk_length == chunk_length) return true;
    if (group_index >= image->group_count) { fail("image refers to a group it does not have"); return false; }

    const GroupEntry* group = &image->groups[group_index];
    uint32_t stored = group->data_size;
    bool compressed = true;
    uint32_t packed_size = group->rvz_packed_size;
    if (image->is_rvz) {
        compressed = (stored & 0x80000000u) != 0;
        stored &= 0x7FFFFFFFu;
    }

    image->chunk_group = UINT64_MAX;
    if (stored == 0) {
        // A group with nothing stored is a stretch of zeroes.
        memset(image->chunk, 0, chunk_length);
        image->chunk_group = group_index;
        image->chunk_length = chunk_length;
        return true;
    }

    if (!reserve_scratch(image, stored)) return false;
    uint64_t at = (uint64_t)group->data_offset << 2;
    if (!read_at(image->file, at, image->scratch, stored)) { fail("image ends early"); return false; }

    // Where the group's bytes end up: straight into the chunk when the group is
    // not packed, otherwise into a holding buffer that is then unpacked.
    size_t plain_size = packed_size ? packed_size : chunk_length;
    uint8_t* plain = image->chunk;
    uint8_t* holding = NULL;
    if (packed_size) {
        holding = (uint8_t*)malloc(plain_size);
        if (!holding) { fail("out of memory"); return false; }
        plain = holding;
    }

    bool ok;
    if (!compressed) {
        ok = stored >= plain_size;
        if (ok) memcpy(plain, image->scratch, plain_size);
    } else {
        ok = inflate_into(image, image->scratch, stored, plain, plain_size);
    }
    if (!ok) {
        free(holding);
        if (!s_error[0]) fail("a group could not be expanded");
        return false;
    }

    if (packed_size) {
        // The packed stream: a length, its top bit saying whether those bytes are
        // stored here or are padding to regenerate from the seed that follows.
        Lfg lfg;
        size_t in = 0, out = 0;
        uint64_t data_offset = group_offset_in_data;
        bool bad = false;
        while (out < chunk_length) {
            if (in + 4 > plain_size) { bad = true; break; }
            uint32_t size = be32(plain + in);
            in += 4;
            bool junk = (size & 0x80000000u) != 0;
            size &= 0x7FFFFFFFu;
            if (junk) {
                if (in + LFG_SEED_WORDS * 4 > plain_size) { bad = true; break; }
                lfg_seed(&lfg, plain + in);
                in += LFG_SEED_WORDS * 4;
                lfg_skip(&lfg, (size_t)(data_offset % JUNK_BLOCK_SIZE));
            }
            size_t n = size;
            if (n > chunk_length - out) n = chunk_length - out;
            if (junk) {
                lfg_bytes(&lfg, n, image->chunk + out);
            } else {
                if (in + n > plain_size) { bad = true; break; }
                memcpy(image->chunk + out, plain + in, n);
                in += n;
            }
            out += n;
            data_offset += n;
            if (size == 0) { bad = true; break; }   // would never finish
        }
        free(holding);
        if (bad) { fail("a group's packed data is damaged"); return false; }
    }

    image->chunk_group = group_index;
    image->chunk_length = chunk_length;
    return true;
}

bool rvz_read(RvzImage* image, uint64_t offset, void* dst, size_t size) {
    if (!image) return false;
    uint8_t* out = (uint8_t*)dst;
    while (size > 0) {
        const RawDataEntry* found = NULL;
        for (uint32_t i = 0; i < image->raw_count; ++i) {
            const RawDataEntry* entry = &image->raw[i];
            // A range can start part way into a chunk; the groups still begin on
            // a chunk boundary, so round the start down the way the writer did.
            uint64_t skipped = entry->data_offset % image->chunk_size;
            uint64_t start = entry->data_offset - skipped;
            uint64_t length = entry->data_size + skipped;
            if (offset >= start && offset < start + length) { found = entry; break; }
        }
        if (!found) {
            // Not covered by any range: the disc reads as zeroes there.
            memset(out, 0, size);
            return true;
        }

        uint64_t skipped = found->data_offset % image->chunk_size;
        uint64_t start = found->data_offset - skipped;
        uint64_t length = found->data_size + skipped;
        uint64_t into = offset - start;
        uint64_t group = into / image->chunk_size;
        uint64_t group_offset_in_data = group * image->chunk_size;
        size_t chunk_length = (size_t)image->chunk_size;
        if (group_offset_in_data + chunk_length > length)
            chunk_length = (size_t)(length - group_offset_in_data);

        if (group >= found->number_of_groups) { fail("read past the end of a data range"); return false; }
        if (!load_group(image, (uint64_t)found->group_index + group, group_offset_in_data, chunk_length))
            return false;

        size_t in_chunk = (size_t)(into - group_offset_in_data);
        size_t take = chunk_length - in_chunk;
        if (take > size) take = size;
        if (take == 0) { fail("read made no progress"); return false; }
        memcpy(out, image->chunk + in_chunk, take);
        out += take;
        offset += take;
        size -= take;
    }
    return true;
}
