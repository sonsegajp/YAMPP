#ifndef YAMPP_COSTUME_ART_H
#define YAMPP_COSTUME_ART_H
#include "abi_recompcore.h"
void costume_art_gx_link(Context*);
RecFn costume_art_lookup(uint32_t);
void costume_art_frame_done(Context*);
void costume_art_reset(Context*);
#endif
