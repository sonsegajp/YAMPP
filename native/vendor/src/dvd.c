// SPDX-License-Identifier: GPL-3.0-or-later
// Host-backed GameCube DVD layer. See dvd.h.
#include "gxruntime/dvd.h"
#include "gxruntime/rvz.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// GameCube disc header fields (big-endian, in the first sector).
#define DISC_MAGIC_OFFSET 0x1Cu
#define DISC_MAGIC        0xC2339F3Du
#define DISC_FST_OFFSET   0x424u  // byte offset of the FST on disc
#define DISC_FST_SIZE     0x428u  // byte size of the FST

// One FST entry is 12 bytes: a flags/string-offset word, then two words whose
// meaning depends on whether the entry is a file or a directory.
#define FST_ENTRY_SIZE 12u

static FILE* g_iso;
static RvzImage* g_rvz;   /* set instead of g_iso for a compressed image */
static u8*   g_fst;          // raw FST blob (entries followed by the string table)
static u32   g_fst_size;
static u32   g_num_entries;  // total entries; equals root entry's "next" field
static const char* g_strings;  // string table, inside g_fst
static bool  g_ready;

static u32 be32(const u8* p) {
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | (u32)p[3];
}

// ---------------------------------------------------------------------------
// FST entry accessors (mirror dvdfs.c macros)
// ---------------------------------------------------------------------------
static const u8* fst_entry(u32 i) { return g_fst + (size_t)i * FST_ENTRY_SIZE; }
static bool entry_is_dir(u32 i)   { return (be32(fst_entry(i)) & 0xFF000000u) != 0; }
static u32  entry_stroff(u32 i)   { return be32(fst_entry(i)) & 0x00FFFFFFu; }
static u32  entry_parent(u32 i)   { return be32(fst_entry(i) + 4); }  // dir: parent index
static u32  entry_next(u32 i)     { return be32(fst_entry(i) + 8); }  // dir: first index past it
static u32  entry_filepos(u32 i)  { return be32(fst_entry(i) + 4); }  // file: disc offset
static u32  entry_filelen(u32 i)  { return be32(fst_entry(i) + 8); }  // file: byte length

// Case-insensitive prefix match: true when all of `string` matches `path` and
// the path component ends there ('/' or NUL). Mirrors dvdfs.c isSame().
static bool name_matches(const char* path, const char* string) {
    while (*string != '\0') {
        if (tolower((unsigned char)*path) != tolower((unsigned char)*string))
            return false;
        path++;
        string++;
    }
    return *path == '/' || *path == '\0';
}

// ---------------------------------------------------------------------------
// Image loading
// ---------------------------------------------------------------------------

// Reads straight from an ISO, or through the decompressor for an RVZ/WIA.
static bool image_read(u64 offset, void* dst, size_t length) {
    if (g_rvz)
        return rvz_read(g_rvz, offset, dst, length);
    if (!g_iso)
        return false;
    return fseeko(g_iso, (off_t)offset, SEEK_SET) == 0 && fread(dst, 1, length, g_iso) == length;
}

static bool load_from(const char* path) {
    FILE* f = NULL;
    if (rvz_is_image(path)) {
        g_rvz = rvz_open(path);
        if (!g_rvz) {
            fprintf(stderr, "[dvd] %s\n", rvz_last_error());
            return false;
        }
    } else {
        f = fopen(path, "rb");
        if (!f)
            return false;
        g_iso = f;
    }

    u8 header[0x440];
    if (!image_read(0, header, sizeof header)) {
        dvd_close_image();
        return false;
    }
    if (be32(header + DISC_MAGIC_OFFSET) != DISC_MAGIC) {
        dvd_close_image();  // not a GameCube disc image
        return false;
    }

    u32 fst_off = be32(header + DISC_FST_OFFSET);
    u32 fst_size = be32(header + DISC_FST_SIZE);
    if (fst_size < FST_ENTRY_SIZE || fst_size > 0x4000000u) {
        dvd_close_image();
        return false;
    }

    u8* fst = (u8*)malloc(fst_size);
    if (!fst) {
        dvd_close_image();
        return false;
    }
    if (!image_read(fst_off, fst, fst_size)) {
        free(fst);
        dvd_close_image();
        return false;
    }

    u32 num_entries = be32(fst + 8);  // root entry's "next" field = total count
    if ((u64)num_entries * FST_ENTRY_SIZE > fst_size) {
        free(fst);
        dvd_close_image();
        return false;
    }

    g_fst = fst;
    g_fst_size = fst_size;
    g_num_entries = num_entries;
    g_strings = (const char*)(fst + (size_t)num_entries * FST_ENTRY_SIZE);
    g_ready = true;
    fprintf(stderr, "[dvd] disc image: %s (%u FST entries)\n", path, num_entries);
    return true;
}

bool dvd_open_image(const char* path) {
    if (g_ready)
        return true;
    if (path != NULL && path[0] != '\0' && load_from(path))
        return true;
    fprintf(stderr, "[dvd] no readable GameCube disc image at '%s'\n",
            path != NULL ? path : "(null)");
    return false;
}

void dvd_close_image(void) {
    if (g_rvz)
        rvz_close(g_rvz);
    g_rvz = NULL;
    if (g_iso)
        fclose(g_iso);
    free(g_fst);
    g_iso = NULL;
    g_fst = NULL;
    g_strings = NULL;
    g_ready = false;
}

bool dvd_image_ready(void) {
    return g_ready;
}

// ---------------------------------------------------------------------------
// Path resolution (mirrors DVDConvertPathToEntrynum; current dir is always the
// root, since the game never issues DVDChangeDir).
// ---------------------------------------------------------------------------
s32 dvd_path_to_entrynum(const char* path) {
    if (!g_ready || !path)
        return -1;

    u32 dir = 0;  // currentDirectory == root
    while (1) {
        if (*path == '\0')
            return (s32)dir;
        if (*path == '/') {
            dir = 0;
            path++;
            continue;
        }
        if (*path == '.') {
            if (path[1] == '.') {
                if (path[2] == '/') {
                    dir = entry_parent(dir);
                    path += 3;
                    continue;
                }
                if (path[2] == '\0')
                    return (s32)entry_parent(dir);
            } else if (path[1] == '/') {
                path += 2;
                continue;
            } else if (path[1] == '\0') {
                return (s32)dir;
            }
        }

        const char* end = path;
        while (*end != '\0' && *end != '/')
            end++;
        bool want_dir = (*end != '\0');
        u32 length = (u32)(end - path);

        u32 i;
        bool found = false;
        for (i = dir + 1; i < entry_next(dir);
             i = entry_is_dir(i) ? entry_next(i) : (i + 1)) {
            if (!entry_is_dir(i) && want_dir)
                continue;
            if (name_matches(path, g_strings + entry_stroff(i))) {
                found = true;
                break;
            }
        }
        if (!found)
            return -1;
        if (!want_dir)
            return (s32)i;

        dir = i;
        path += length + 1;
    }
}

bool dvd_entry_info(s32 entrynum, u32* start, u32* length) {
    if (!g_ready || entrynum < 0 || (u32)entrynum >= g_num_entries)
        return false;
    if (entry_is_dir((u32)entrynum))
        return false;
    if (start)
        *start = entry_filepos((u32)entrynum);
    if (length)
        *length = entry_filelen((u32)entrynum);
    {extern int mods_dvd_info(uint32_t*,uint32_t*);if(start&&length)mods_dvd_info(start,length);}
    return true;
}

// ---------------------------------------------------------------------------
// Disc read -> guest RAM
// ---------------------------------------------------------------------------
void dvd_read_to_guest(CPUState* cpu, u32 guest_addr, u32 disc_off, u32 length) {
    if (!g_ready || length == 0)
        return;

    {extern int mods_disc_read(CPUState*,uint32_t,uint32_t*,uint32_t);if(mods_disc_read(cpu,guest_addr,&disc_off,length))return;}
    // Fast path: the destination lives entirely in cached or uncached MEM1, so
    // copy disc bytes straight into the flat guest RAM (both are big-endian raw
    // bytes -- no swizzling, exactly as a real DI DMA would land them).
    u8* dst = NULL;
    if (guest_addr >= GC_RAM_BASE && guest_addr + length <= GC_RAM_BASE + cpu->ram_size)
        dst = cpu->ram + (guest_addr - GC_RAM_BASE);
    else if (guest_addr >= GC_RAM_UNCACHED &&
             guest_addr + length <= GC_RAM_UNCACHED + cpu->ram_size)
        dst = cpu->ram + (guest_addr - GC_RAM_UNCACHED);

    if (dst) {
        if (!image_read(disc_off, dst, length))
            memset(dst, 0, length);  // zero-fill past image end
        return;
    }

    // Fallback for a non-MEM1 target: route each byte through the CPU so any
    // external region (VM window, etc.) is honored. Not expected in practice.
    for (u32 k = 0; k < length; k++) {
        u8 b = 0;
        (void)image_read(disc_off + k, &b, 1);
        mem_write8(cpu, guest_addr + k, b);
    }
}
