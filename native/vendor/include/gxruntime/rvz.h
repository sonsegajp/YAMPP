// Reading RVZ and WIA disc images, the compressed formats Dolphin writes.
//
// An ISO can simply be seeked into; these cannot. The disc is split into groups
// which are compressed separately, and stretches the compressor recognised as
// padding are not stored at all - they are regenerated from a seed. So a read
// means finding the group a disc offset falls in, decompressing it, and in RVZ's
// case unpacking the mix of stored bytes and regenerated padding inside it.
#ifndef GXRUNTIME_RVZ_H
#define GXRUNTIME_RVZ_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct RvzImage RvzImage;

// Returns NULL when the file is not RVZ/WIA, or uses a compression this build
// cannot read; rvz_last_error() then says which.
RvzImage* rvz_open(const char* path);
void rvz_close(RvzImage* image);

// Reads `size` bytes from the uncompressed disc at `offset`. Returns false if
// the read runs past the end of the disc or the image is damaged.
bool rvz_read(RvzImage* image, uint64_t offset, void* dst, size_t size);

uint64_t rvz_disc_size(const RvzImage* image);
const char* rvz_last_error(void);

// True if the first bytes of this file are an RVZ or WIA signature.
bool rvz_is_image(const char* path);

#endif
