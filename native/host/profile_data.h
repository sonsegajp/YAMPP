#pragma once
/* Fixed-size, decoded pixels only. No file paths or image decoders on the relay. */
#define YAMPP_AVATAR_SIDE 96
#define YAMPP_AVATAR_BYTES (YAMPP_AVATAR_SIDE * YAMPP_AVATAR_SIDE * 4)
#define YAMPP_AVATAR_CHUNK 2048
#define YAMPP_AVATAR_PARTS (YAMPP_AVATAR_BYTES / YAMPP_AVATAR_CHUNK)
