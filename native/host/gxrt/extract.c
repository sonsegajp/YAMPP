/* Unpacking a disc image to disk.
 *
 * The first-run setup does this for an ISO by itself, but an RVZ is compressed
 * and cannot be read without the decompressor, so both kinds are handled here
 * and the setup calls the game with --extract. */
#include "gxruntime/rvz.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#else
#include <sys/types.h>
#define _mkdir(d) mkdir((d), 0755)
#define _fseeki64 fseeko
#endif

typedef struct {
    FILE* iso;
    RvzImage* rvz;
} Image;

static int image_read(Image* image, uint64_t offset, void* dst, size_t size) {
    if (image->rvz) return rvz_read(image->rvz, offset, dst, size) ? 1 : 0;
    return _fseeki64(image->iso, (long long)offset, SEEK_SET) == 0 &&
           fread(dst, 1, size, image->iso) == size;
}

static uint32_t be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static void make_parents(const char* path) {
    char folder[1024];
    snprintf(folder, sizeof folder, "%s", path);
    for (char* p = folder + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            char saved = *p;
            *p = 0;
            _mkdir(folder);
            *p = saved;
        }
    }
}

static int write_range(Image* image, uint64_t offset, uint64_t size, const char* target) {
    make_parents(target);
    FILE* out = fopen(target, "wb");
    if (!out) { fprintf(stderr, "cannot write %s\n", target); return 0; }
    static unsigned char buffer[1 << 20];
    int ok = 1;
    while (size > 0) {
        size_t want = size < sizeof buffer ? (size_t)size : sizeof buffer;
        if (!image_read(image, offset, buffer, want)) { ok = 0; break; }
        if (fwrite(buffer, 1, want, out) != want) { ok = 0; break; }
        offset += want;
        size -= want;
    }
    fclose(out);
    if (!ok) fprintf(stderr, "failed while writing %s\n", target);
    return ok;
}

/* Writes sys/main.dol, sys/fst.bin and every file on the disc under `out`. */
int extract_disc_image(const char* image_path, const char* out)
{
    Image image = {0};
    if (rvz_is_image(image_path)) {
        image.rvz = rvz_open(image_path);
        if (!image.rvz) { fprintf(stderr, "%s\n", rvz_last_error()); return 1; }
    } else {
        image.iso = fopen(image_path, "rb");
        if (!image.iso) { fprintf(stderr, "cannot open %s\n", image_path); return 1; }
    }

    unsigned char header[0x440];
    if (!image_read(&image, 0, header, sizeof header)) { fprintf(stderr, "image is too short\n"); return 1; }
    if (be32(header + 0x1c) != 0xc2339f3du) { fprintf(stderr, "not a GameCube disc image\n"); return 1; }

    uint32_t dol_offset = be32(header + 0x420);
    uint32_t fst_offset = be32(header + 0x424);
    uint32_t fst_size = be32(header + 0x428);

    /* The executable's length is the furthest its sections reach. */
    unsigned char dol_head[0x100];
    if (!image_read(&image, dol_offset, dol_head, sizeof dol_head)) { fprintf(stderr, "image is too short\n"); return 1; }
    uint32_t dol_size = 0;
    for (int i = 0; i < 18; i++) {
        uint32_t reach = be32(dol_head + i * 4) + be32(dol_head + 0x90 + i * 4);
        if (reach > dol_size) dol_size = reach;
    }

    char path[1024];
    snprintf(path, sizeof path, "%s/sys/main.dol", out);
    if (!write_range(&image, dol_offset, dol_size, path)) return 1;
    snprintf(path, sizeof path, "%s/sys/fst.bin", out);
    if (!write_range(&image, fst_offset, fst_size, path)) return 1;

    if (fst_size < 12) { fprintf(stderr, "file table too small\n"); return 1; }
    unsigned char* fst = (unsigned char*)malloc(fst_size);
    if (!fst || !image_read(&image, fst_offset, fst, fst_size)) { fprintf(stderr, "cannot read the file table\n"); return 1; }
    uint32_t count = be32(fst + 8);
    if (count < 1 || (uint64_t)count * 12 > fst_size) { fprintf(stderr, "the file table is damaged\n"); return 1; }
    const char* names = (const char*)(fst + (size_t)count * 12);
    uint32_t names_end = fst_size - (uint32_t)(count * 12);

    /* Directory entries give the index they end at, so a stack tracks the path. */
    struct { uint32_t end; char path[768]; } stack[16];
    int depth = 0;
    stack[0].end = count;
    snprintf(stack[0].path, sizeof stack[0].path, "%s/files", out);

    unsigned written = 0;
    for (uint32_t i = 1; i < count; i++) {
        while (depth > 0 && i >= stack[depth].end) depth--;
        uint32_t flags = be32(fst + i * 12);
        uint32_t offset = be32(fst + i * 12 + 4);
        uint32_t size = be32(fst + i * 12 + 8);
        uint32_t name_off = flags & 0xffffff;
        if (name_off >= names_end) { fprintf(stderr, "name offset out of range in the file table\n"); return 1; }
        if (!memchr(names + name_off, 0, names_end - name_off)) { fprintf(stderr, "unterminated name in the file table\n"); return 1; }
        const char* name = names + name_off;
        if (strchr(name, '/') || strchr(name, '\\') || !name[0] || !strcmp(name, ".") || !strcmp(name, "..")) { fprintf(stderr, "bad name in the file table\n"); return 1; }
        char full[1024];
        snprintf(full, sizeof full, "%s/%s", stack[depth].path, name);
        if (flags >> 24) {
            if (depth + 1 >= (int)(sizeof stack / sizeof *stack)) { fprintf(stderr, "file table nests too deeply\n"); return 1; }
            depth++;
            stack[depth].end = size;
            snprintf(stack[depth].path, sizeof stack[depth].path, "%s", full);
            make_parents(full);
            _mkdir(full);
        } else {
            if (!write_range(&image, offset, size, full)) return 1;
            if (++written % 200 == 0) { printf("    %u files...\n", written); fflush(stdout); }
        }
    }
    printf("  Unpacked %u files.\n", written);

    free(fst);
    if (image.rvz) rvz_close(image.rvz);
    if (image.iso) fclose(image.iso);
    return 0;
}
