/* Container-independent identity using the same RVZ/WIA reader as gameplay.
 * Streams decoded bytes without writing or allocating an uncompressed ISO. */
#include "gxruntime/rvz.h"
#include <openssl/evp.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
int main(int argc, char **argv) {
    if (argc != 2 || !rvz_is_image(argv[1])) {
        fprintf(stderr, "Expected an RVZ or WIA disc image\n"); return 2;
    }
    RvzImage *image = rvz_open(argv[1]);
    if (!image) { fprintf(stderr, "%s\n", rvz_last_error()); return 1; }
    uint64_t size = rvz_disc_size(image);
    EVP_MD_CTX *hash = EVP_MD_CTX_new();
    unsigned char *buffer = malloc(1024 * 1024), digest[EVP_MAX_MD_SIZE];
    unsigned length = 0; int result = 1;
    if (!size || size > UINT64_C(8589934592)) {
        fprintf(stderr, "Invalid decoded disc size: %llu\n", (unsigned long long)size); goto done;
    }
    if (!hash || !buffer || EVP_DigestInit_ex(hash, EVP_sha256(), NULL) != 1) {
        fprintf(stderr, "Could not initialize disc verification\n"); goto done;
    }
    for (uint64_t offset = 0; offset < size;) {
        size_t count = size - offset < 1024 * 1024 ? (size_t)(size - offset) : 1024 * 1024;
        if (!rvz_read(image, offset, buffer, count) || EVP_DigestUpdate(hash, buffer, count) != 1) {
            fprintf(stderr, "Disc verification failed: %s\n", rvz_last_error()); goto done;
        }
        offset += count;
    }
    if (EVP_DigestFinal_ex(hash, digest, &length) != 1 || length != 32) goto done;
    printf("{\"schema\":1,\"size\":%llu,\"sha256\":\"", (unsigned long long)size);
    for (unsigned i = 0; i < length; ++i) printf("%02x", digest[i]);
    puts("\"}"); result = 0;
done:
    free(buffer); EVP_MD_CTX_free(hash); rvz_close(image); return result;
}
