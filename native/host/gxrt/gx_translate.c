/*
 * Binary-GX to Aurora FIFO translation.
 *
 * Aurora is a source-level GX implementation. Raw GameCube streams therefore
 * need three pieces of information which real GX hardware obtains elsewhere:
 * host pointers for vertex arrays, host pointers/revisions for textures and
 * TLUTs, and nested display-list expansion. This file supplies exactly that
 * compatibility seam while leaving original CP/BP/XF command ordering intact.
 */
#include "gx_translate.h"

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

/* One-shot forensic log for XF memory that Aurora does not model.  This is
 * intentionally opt-in: the translator can run on an asynchronous worker and
 * ordinary play must never pay for stderr synchronization. */
static atomic_uint s_xf_unmodeled_logged = 0;

static int xf_unmodeled_log_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0) {
        const char* env = getenv("GCN_GXT_XF_DIAG");
        enabled = env && *env && *env != '0';
    }
    return enabled;
}

#define GX_AURORA_OPCODE          0x50u
#define GX_AURORA_LOAD_ARRAYBASE  0x0010u
#define GX_AURORA_LOAD_TEXOBJ     0x0030u
#define GX_AURORA_LOAD_TLUT       0x0031u
#define GX_AURORA_DESTROY_COPY_TEX 0x0034u
#define GX_AURORA_LOAD_COPY_SRC   0x0035u
#define GX_AURORA_LOAD_COPY_DST   0x0036u
#define GX_AURORA_LOAD_COPY_DEST  0x0037u
#define GX_AURORA_INVALIDATE_ARRAY 0x0042u

#define GXT_ARRAY_COUNT            16u
#define GXT_TEXTURE_COUNT           8u
#define GXT_MAX_TLUTS              20u
#define GXT_MAX_DL_DEPTH           16u
#define GXT_MAX_COPY_DESTS         64u
#define GXT_ARRAY_FALLBACK_CAP (256u * 1024u)
#define GXT_PAGE_SHIFT              12u
#define GXT_PAGE_SIZE       (1u << GXT_PAGE_SHIFT)
#define GXT_PHYSICAL_SIZE   (1u << 28)
#define GXT_PAGE_COUNT      (GXT_PHYSICAL_SIZE >> GXT_PAGE_SHIFT)
/* Interest filter: one bit per 64 KiB region, set the first time a graphics
 * resource reads that region's epochs.  RAM writers outside every marked
 * region take a single relaxed load instead of the epoch machinery. */
#define GXT_REGION_SHIFT            16u
#define GXT_REGION_COUNT    (GXT_PHYSICAL_SIZE >> GXT_REGION_SHIFT)
#define GXT_REGION_WORDS    (GXT_REGION_COUNT / 64u)

/*
 * Translator-owned snapshot of guest resource bytes.  Aurora's FIFO worker
 * dereferences resource pointers asynchronously, after the guest may already
 * have mutated the range, so every emitted pointer (except EFB-copy
 * destination keys) targets one of these immutable blocks.  A superseded
 * block stays on the retired list until gx_translate_frame_drained().
 */
typedef struct GxtStaged {
    struct GxtStaged* next;       /* replaced/retired chain */
    struct GxtStaged* next_fresh; /* run-local allocation chain */
    u32 size;
} GxtStaged;

typedef struct GxtArray {
    u32 base;
    u32 stride;
    u32 frame_required;
    u32 emitted_base;
    u32 emitted_size;
    u8 base_valid;
    u8 emitted_this_frame;
    u8 metadata_valid;
    u8 force_rebind;
    u64 source_epoch;
    GxtStaged* staged;
    u32 staged_size;
    u32 version;
} GxtArray;

typedef struct GxtTlut {
    u32 tmem;
    u32 phys;
    u16 entries;
    u8 valid;
    u8 dirty;
    u8 revision_pending;
    u8 emitted_valid;
    u32 emitted_format;
    u32 version;
    GxtStaged* data;
} GxtTlut;

/* GX assigns palette format to each texture unit, not to TMEM storage. Aurora
 * assigns it to a TLUT object, so give each of the eight texture units its own
 * view of the shared immutable palette bytes. */
typedef struct GxtTlutBinding {
    u32 tmem;
    u32 source_version;
    u32 format;
    u8 valid;
} GxtTlutBinding;

/*
 * Content-addressed snapshot cache.  Texture and array slots churn every few
 * draws (a match re-points the eight texmaps ~1700 times per frame), so
 * snapshots must be keyed by their guest source, not by the slot that
 * happens to reference them: re-binding unchanged content reuses the
 * existing immutable block with zero copies.  Entries own their blocks;
 * slots only borrow.  A superseded block joins the drain-deferred retired
 * list, so an already-emitted pointer stays readable until Aurora finishes
 * the frame that referenced it.
 */
typedef struct GxtCacheEntry {
    u32 phys;
    u32 size;
    u64 epoch;
    GxtStaged* block;
} GxtCacheEntry;
#define GXT_CACHE_ENTRIES 4096u
#define GXT_CACHE_PROBES    16u

typedef struct GxtState {
    u32 vcd_lo;
    u32 vcd_hi;
    u32 vat[8][3];
    GxtArray arrays[GXT_ARRAY_COUNT];

    u32 tex_mode0[GXT_TEXTURE_COUNT];
    u32 tex_mode1[GXT_TEXTURE_COUNT];
    u32 tex_image0[GXT_TEXTURE_COUNT];
    u32 tex_image3[GXT_TEXTURE_COUNT];
    u32 tex_tlut[GXT_TEXTURE_COUNT];
    u32 tex_version[GXT_TEXTURE_COUNT];
    u64 tex_source_epoch[GXT_TEXTURE_COUNT];
    u8 tex_valid[GXT_TEXTURE_COUNT];
    u8 tex_dirty[GXT_TEXTURE_COUNT];
    u8 tex_revision_pending[GXT_TEXTURE_COUNT];
    GxtStaged* tex_staged[GXT_TEXTURE_COUNT];
    u32 tex_staged_phys[GXT_TEXTURE_COUNT];
    u32 texture_version_clock;

    u32 tlut_src_phys;
    u8 tlut_src_valid;
    GxtTlut tluts[GXT_MAX_TLUTS];
    GxtTlutBinding tlut_bindings[GXT_TEXTURE_COUNT];
    u32 tlut_version_clock;

    /* EFB-copy destinations are Aurora cache keys: a texture bound at one of
     * these addresses must be emitted with the identical live-RAM pointer or
     * Aurora cannot resolve it to the copied GPU texture.  Each entry
     * remembers the content epoch at copy time: a later guest WRITE to the
     * destination means the RAM now holds real texture data, so the entry
     * must be evicted (and Aurora's cache told) or the stale GPU copy
     * aliases the new texture. */
    u32 copy_dests[GXT_MAX_COPY_DESTS];
    u64 copy_dest_epochs[GXT_MAX_COPY_DESTS];
    u64 copy_dest_hashes[GXT_MAX_COPY_DESTS];
    u32 copy_dest_sizes[GXT_MAX_COPY_DESTS];
    u32 copy_dest_count;
    u32 copy_dest_next;

    GxtCacheEntry cache[GXT_CACHE_ENTRIES];
    u32 array_version_clock;

    u32 bp_cache[256];
    u8 bp_valid[256];
    u32 bp_mask;
    u32 current_mtx_row;   /* CP 0x30 bits 0-5 (diagnostic) */
    u32 xf_chan[10];       /* XF 0x1009-0x1012 shadow (diagnostic) */
    u8* ram_base;
} GxtState;

struct GxTranslateContext {
    GxtState state;
    GxTranslateStats stats;
    GxTranslateError last_error;
    u32 last_error_offset;
    u8 last_error_opcode;
    GxtStaged* retired;
    atomic_uint_fast64_t* page_epochs;
    atomic_uint_fast64_t interest[GXT_REGION_WORDS];
    atomic_uint_fast64_t invalidation_epoch;
    atomic_uint_fast64_t full_invalidation_epoch;
    atomic_uint_fast64_t invalidation_notifications;
    atomic_uint_fast64_t invalidation_pages;
};

typedef struct GxtSink {
    u8* out;
    u32 cap;
    u32 len;
    int overflow;
} GxtSink;

typedef struct GxtRun {
    GxTranslateContext* context;
    GxtState* state;
    GxtSink* sink;
    u8* ram;
    u32 ram_size;
    GxTranslateStats delta;
    GxTranslateError error;
    u32 error_offset;
    u8 error_opcode;
    /* Transactional staging: blocks allocated during this run are freed if
     * the run fails; blocks superseded during this run move to the context's
     * retired list only if the run succeeds. */
    GxtStaged* fresh;
    GxtStaged* replaced;
    /* Per-run epoch memoization.  Within one translated stream all resource
     * bytes are read at translation time anyway, so evaluating a resource's
     * page epochs once per stream loses no fidelity while removing the
     * per-draw atomic page scans that dominated draw-heavy frames. */
    u8 tex_epoch_checked[GXT_TEXTURE_COUNT];
    u64 tex_epoch_value[GXT_TEXTURE_COUNT];
    u32 tex_epoch_phys[GXT_TEXTURE_COUNT];
    u32 tex_epoch_size[GXT_TEXTURE_COUNT];
    /* Draws re-evaluate texture dirtiness only after a texture-affecting
     * command; between such commands the eight-slot scan is skipped. */
    u8 tex_scan_pending;
    u8 array_epoch_checked[GXT_ARRAY_COUNT];
    u64 array_epoch_value[GXT_ARRAY_COUNT];
    u32 array_epoch_base[GXT_ARRAY_COUNT];
    u32 array_epoch_extent[GXT_ARRAY_COUNT];
} GxtRun;

typedef struct GxtIndexedAttr {
    u8 active;
    u8 offset;
    u8 index_width;
    u8 index_count;
    u8 cp_index;
    u8 fallback_extent;
    u16 element_size;
} GxtIndexedAttr;

static const u8 kFmtSize[5] = {1, 1, 2, 2, 4};
static const u8 kColSize[8] = {2, 3, 4, 2, 3, 4, 4, 4};

static u32 bits(u32 value, u32 shift, u32 count)
{
    return (value >> shift) & ((1u << count) - 1u);
}

static u16 rd_be16(const u8* p)
{
    return (u16)(((u32)p[0] << 8) | p[1]);
}

static u32 rd_be32(const u8* p)
{
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

static void put(GxtSink* sink, const void* data, u32 size)
{
    if (sink->overflow) return;
    if (size > sink->cap - sink->len) {
        sink->overflow = 1;
        return;
    }
    memcpy(sink->out + sink->len, data, size);
    sink->len += size;
}

static void put_u8(GxtSink* sink, u8 value) { put(sink, &value, 1); }

static void put_be16(GxtSink* sink, u16 value)
{
    const u8 bytes[2] = {(u8)(value >> 8), (u8)value};
    put(sink, bytes, 2);
}

static void put_be32(GxtSink* sink, u32 value)
{
    const u8 bytes[4] = {
        (u8)(value >> 24), (u8)(value >> 16), (u8)(value >> 8), (u8)value,
    };
    put(sink, bytes, 4);
}

static void put_be64(GxtSink* sink, u64 value)
{
    u8 bytes[8];
    unsigned i;
    for (i = 0; i < 8; ++i) bytes[i] = (u8)(value >> (56u - i * 8u));
    put(sink, bytes, 8);
}

static void fail_run(GxtRun* run, GxTranslateError error, u8 opcode, u32 offset)
{
    if (run->error != GXT_ERROR_NONE) return;
    run->error = error;
    run->error_opcode = opcode;
    run->error_offset = offset;
}

static u32 next_nonzero(u32* clock)
{
    ++*clock;
    if (*clock == 0) ++*clock;
    return *clock;
}

static u64 div_ceil_u64(u64 value, u64 divisor)
{
    return value / divisor + (value % divisor != 0);
}

u32 gx_translate_texture_source_size(u32 format, u32 width, u32 height, u32 mip_count)
{
    u64 total = 0;
    u32 mip;
    if (!width || !height || !mip_count) return 0;
    for (mip = 0; mip < mip_count; ++mip) {
        const u64 mip_width = (width >> mip) ? (width >> mip) : 1u;
        const u64 mip_height = (height >> mip) ? (height >> mip) : 1u;
        u64 level = 0;
        switch (format) {
        case 0x0u: /* I4 */
        case 0x8u: /* C4 */
        case 0xEu: /* CMPR */
            level = div_ceil_u64(mip_width, 8) * div_ceil_u64(mip_height, 8) * 32u;
            break;
        case 0x1u: /* I8 */
        case 0x2u: /* IA4 */
        case 0x9u: /* C8 */
            level = div_ceil_u64(mip_width, 8) * div_ceil_u64(mip_height, 4) * 32u;
            break;
        case 0x3u: /* IA8 */
        case 0x4u: /* RGB565 */
        case 0x5u: /* RGB5A3 */
        case 0xAu: /* C14X2 */
            level = div_ceil_u64(mip_width, 4) * div_ceil_u64(mip_height, 4) * 32u;
            break;
        case 0x6u: /* RGBA8 */
            level = div_ceil_u64(mip_width, 4) * div_ceil_u64(mip_height, 4) * 64u;
            break;
        default:
            return 0;
        }
        total += level;
        if (total > UINT32_MAX) return 0;
    }
    return (u32)total;
}

u32 gx_translate_tlut_source_size(u32 entries)
{
    return entries <= UINT32_MAX / 2u ? entries * 2u : 0u;
}

static int allocate_page_epochs(GxTranslateContext* context)
{
    u32 page;
    if (context->page_epochs) return 1;
    context->page_epochs = (atomic_uint_fast64_t*)malloc(GXT_PAGE_COUNT * sizeof(*context->page_epochs));
    if (!context->page_epochs) return 0;
    for (page = 0; page < GXT_PAGE_COUNT; ++page)
        atomic_init(&context->page_epochs[page], 0);
    return 1;
}

static u64 next_invalidation_epoch(GxTranslateContext* context)
{
    /* A 64-bit epoch avoids a global page-array reset while worker threads are
     * active. At one billion notifications per second it still lasts centuries. */
    return atomic_fetch_add_explicit(&context->invalidation_epoch, 1,
                                     memory_order_relaxed) + 1u;
}

static u64 source_epoch(const GxTranslateContext* context, u32 physical, u32 size)
{
    GxTranslateContext* mutable_context = (GxTranslateContext*)(uintptr_t)context;
    u32 first, last, page, region;
    u64 epoch;
    if (!context || !size) return 0;
    epoch = atomic_load_explicit(&context->full_invalidation_epoch, memory_order_relaxed);
    if (physical >= GXT_PHYSICAL_SIZE) return epoch;
    if (size > GXT_PHYSICAL_SIZE - physical) size = GXT_PHYSICAL_SIZE - physical;
    /* Register interest BEFORE reading the epochs so a concurrent writer
     * either sees the bit (and stamps the pages) or its bytes land before
     * the caller's snapshot copy.  The seq_cst ordering closes the common
     * interleavings; the residual TSO window is absorbed by the byte-compare
     * guard at the staging sites. */
    for (region = physical >> GXT_REGION_SHIFT;
         region <= (physical + size - 1u) >> GXT_REGION_SHIFT; ++region) {
        const u64 bit = 1ull << (region & 63u);
        atomic_uint_fast64_t* word = &mutable_context->interest[region >> 6u];
        if (!(atomic_load_explicit(word, memory_order_relaxed) & bit))
            atomic_fetch_or_explicit(word, bit, memory_order_seq_cst);
    }
    first = physical >> GXT_PAGE_SHIFT;
    last = (physical + size - 1u) >> GXT_PAGE_SHIFT;
    if (!context->page_epochs) return epoch;
    for (page = first; page <= last; ++page) {
        const u64 page_epoch = atomic_load_explicit(&context->page_epochs[page],
                                                    memory_order_relaxed);
        if (page_epoch > epoch) epoch = page_epoch;
    }
    return epoch;
}

static u8* staged_bytes(GxtStaged* block)
{
    return (u8*)(block + 1);
}

static GxtStaged* stage_copy(GxtRun* run, const u8* source, u32 size,
                             u8 opcode, u32 offset)
{
    GxtStaged* block = (GxtStaged*)malloc(sizeof(*block) + size);
    if (!block) {
        fail_run(run, GXT_ERROR_OUT_OF_MEMORY, opcode, offset);
        return NULL;
    }
    block->next = NULL;
    block->next_fresh = run->fresh;
    block->size = size;
    memcpy(staged_bytes(block), source, size);
    run->fresh = block;
    run->delta.staged_copies++;
    run->delta.staged_bytes += size;
    return block;
}

/* Supersede *slot with a new block.  The old block joins the run's replaced
 * chain and is freed only after the next reported Aurora drain. */
static void stage_replace(GxtRun* run, GxtStaged** slot, GxtStaged* block)
{
    if (*slot) {
        (*slot)->next = run->replaced;
        run->replaced = *slot;
    }
    *slot = block;
}

static void free_staged_chain(GxtStaged* block)
{
    while (block) {
        GxtStaged* next = block->next;
        free(block);
        block = next;
    }
}

/* Array and texture bindings borrow blocks owned by the shared snapshot
 * cache.  When a cache entry is superseded, every borrower of the old block
 * must be invalidated before that block moves to the frame-retired list.
 * Otherwise its pointer survives in persistent GX state after the drain and
 * the next translation can compare or emit freed memory. */
static void invalidate_cache_borrowers(GxtState* state, GxtStaged* block)
{
    u32 i;
    if (!block) return;
    for (i = 0; i < GXT_ARRAY_COUNT; ++i) {
        GxtArray* array = &state->arrays[i];
        if (array->staged != block) continue;
        array->staged = NULL;
        array->staged_size = 0;
        array->metadata_valid = 0;
        array->force_rebind = 1;
    }
    for (i = 0; i < GXT_TEXTURE_COUNT; ++i) {
        if (state->tex_staged[i] != block) continue;
        state->tex_staged[i] = NULL;
        state->tex_staged_phys[i] = 0xFFFFFFFFu;
        state->tex_dirty[i] = 1;
        state->tex_revision_pending[i] = 1;
    }
}

/*
 * Borrow an immutable snapshot of [phys, phys+size) at the given content
 * epoch.  A cache hit is free; a miss copies once and supersedes any stale
 * entry for the same source.  Returned blocks are owned by the cache; the
 * caller must never free them.
 */
static GxtStaged* cache_acquire(GxtRun* run, u32 phys, u32 size, u64 epoch,
                                u8 opcode, u32 offset)
{
    GxtState* state = run->state;
    const u32 mask = GXT_CACHE_ENTRIES - 1u;
    const u32 hash = (phys >> 5) * 2654435761u;
    GxtCacheEntry* entry = NULL;
    GxtStaged* block;
    u32 probe;
    for (probe = 0; probe < GXT_CACHE_PROBES; ++probe) {
        GxtCacheEntry* candidate = &state->cache[(hash + probe) & mask];
        if (candidate->block && candidate->phys == phys &&
            candidate->size == size) {
            if (candidate->epoch == epoch)
                return candidate->block;
            /* NOTE(bisect 2026-08-26): a byte-verify (memcmp-then-reuse) here
             * was trialed to absorb false-sharing epoch churn and preserve
             * pointer identity, but the first build carrying it regressed
             * whole-run throughput ~3x; it is reverted while the audit-fix
             * batch is bisected one variable at a time.  If re-attempted it
             * must ship together with the invalidation-generation stamp that
             * caps memcmp volume (see runbook, translator-staging audit). */
            entry = candidate;   /* stale content for this source: supersede */
            break;
        }
        if (!entry && !candidate->block) entry = candidate;
    }
    if (!entry)
        entry = &state->cache[hash & mask];
    block = stage_copy(run, run->ram + phys, size, opcode, offset);
    if (!block) return NULL;
    if (entry->block) {
        invalidate_cache_borrowers(state, entry->block);
        entry->block->next = run->replaced;
        run->replaced = entry->block;
    }
    entry->phys = phys;
    entry->size = size;
    entry->epoch = epoch;
    entry->block = block;
    return block;
}

static void free_all_staging(GxTranslateContext* context)
{
    u32 i;
    /* Array/texture slots only borrow from the content cache. */
    for (i = 0; i < GXT_ARRAY_COUNT; ++i)
        context->state.arrays[i].staged = NULL;
    for (i = 0; i < GXT_TEXTURE_COUNT; ++i)
        context->state.tex_staged[i] = NULL;
    for (i = 0; i < GXT_CACHE_ENTRIES; ++i) {
        free(context->state.cache[i].block);
        context->state.cache[i].block = NULL;
    }
    for (i = 0; i < GXT_MAX_TLUTS; ++i) {
        free(context->state.tluts[i].data);
        context->state.tluts[i].data = NULL;
    }
    free_staged_chain(context->retired);
    context->retired = NULL;
}

static int find_copy_dest(const GxtState* state, u32 physical)
{
    u32 i;
    for (i = 0; i < state->copy_dest_count; ++i)
        if (state->copy_dests[i] == physical) return (int)i;
    return -1;
}

/* GPU copies remain authoritative until their covered RAM bytes change.
 * Page epochs also change for unrelated objects sharing an allocation page,
 * so confirm a content change before retiring the GPU image. */
static u64 copy_content_hash(const u8* bytes,u32 size) {
    u64 hash=0x9e3779b185ebca87ull^(u64)size;
    while(size>=8){u64 word;memcpy(&word,bytes,8);hash^=word;hash*=0xc2b2ae3d27d4eb4full;hash=(hash<<31)|(hash>>33);bytes+=8;size-=8;}
    while(size--){hash^=*bytes++;hash*=0x100000001b3ull;}return hash;
}

static int copy_dest_live(GxtRun* run, u32 physical)
{
    GxtState* state = run->state;
    const int index = find_copy_dest(state, physical);
    u64 epoch;
    if (index < 0) return 0;
    epoch = source_epoch(run->context, physical, state->copy_dest_sizes[index]);
    if (epoch == state->copy_dest_epochs[index]) return 1;
    if(copy_content_hash(run->ram+physical,state->copy_dest_sizes[index])==state->copy_dest_hashes[index]) {
        state->copy_dest_epochs[index]=epoch;return 1;
    }
    if(getenv("MELEE_TRACE_COPIES"))fprintf(stderr,"[copy] invalidated %08X size=%u\n",physical,state->copy_dest_sizes[index]);
    put_u8(run->sink, GX_AURORA_OPCODE);
    put_be16(run->sink, GX_AURORA_DESTROY_COPY_TEX);
    put_be64(run->sink, (u64)(uintptr_t)(run->ram + physical));
    state->copy_dest_count--;
    state->copy_dests[index] = state->copy_dests[state->copy_dest_count];
    state->copy_dest_epochs[index] = state->copy_dest_epochs[state->copy_dest_count];
    state->copy_dest_hashes[index] = state->copy_dest_hashes[state->copy_dest_count];
    state->copy_dest_sizes[index] = state->copy_dest_sizes[state->copy_dest_count];
    if (state->copy_dest_next >= state->copy_dest_count)
        state->copy_dest_next = 0;
    return 0;
}

static void note_copy_dest(GxtRun* run, u32 physical, u32 size)
{
    GxtState* state = run->state;
    const u64 epoch = source_epoch(run->context, physical, size);
    int index = find_copy_dest(state, physical);
    if (index < 0) {
        if (state->copy_dest_count < GXT_MAX_COPY_DESTS) {
            index = (int)state->copy_dest_count++;
        } else {
            index = (int)state->copy_dest_next;
            state->copy_dest_next =
                (state->copy_dest_next + 1u) % GXT_MAX_COPY_DESTS;
        }
        state->copy_dests[index] = physical;
    }
    state->copy_dest_epochs[index] = epoch;
    state->copy_dest_hashes[index] = copy_content_hash(run->ram+physical,size);
    state->copy_dest_sizes[index] = size;
}

static void arrays_begin_frame(GxtState* state)
{
    u32 i;
    for (i = 0; i < GXT_ARRAY_COUNT; ++i) {
        state->arrays[i].frame_required = 0;
        state->arrays[i].emitted_this_frame = 0;
    }
}

static void set_array_base_state(GxtState* state, u32 index, u32 physical)
{
    GxtArray* array;
    if (index >= GXT_ARRAY_COUNT) return;
    array = &state->arrays[index];
    physical &= 0x0FFFFFFFu;
    if (!array->base_valid || array->base != physical) {
        array->base = physical;
        array->base_valid = 1;
        array->frame_required = 0;
        array->emitted_this_frame = 0;
    }
    /* A same-base rewrite alone is not a content signal: content changes are
     * observed through page epochs and the explicit INVL_VC command. */
}

static void set_array_stride_state(GxtState* state, u32 index, u32 stride)
{
    GxtArray* array;
    if (index >= GXT_ARRAY_COUNT) return;
    array = &state->arrays[index];
    stride &= 0xFFu;
    if (array->stride != stride) {
        array->stride = stride;
        array->frame_required = 0;
        array->emitted_this_frame = 0;
        array->force_rebind = 1;
    }
}

void gx_translate_set_array_base(GxTranslateContext* context, u32 cp_index, u32 physical)
{
    if (context) set_array_base_state(&context->state, cp_index, physical);
}

void gx_translate_set_array_stride(GxTranslateContext* context, u32 cp_index, u32 stride)
{
    if (context) set_array_stride_state(&context->state, cp_index, stride);
}

static int texture_range(const GxtState* state, u32 id, u32* physical, u32* size)
{
    const u32 required = (1u << 2) | (1u << 3);
    u32 width, height, format, mip_count;
    if (id >= GXT_TEXTURE_COUNT || (state->tex_valid[id] & required) != required) return 0;
    *physical = (state->tex_image3[id] & 0x00FFFFFFu) << 5;
    width = bits(state->tex_image0[id], 0, 10) + 1u;
    height = bits(state->tex_image0[id], 10, 10) + 1u;
    format = bits(state->tex_image0[id], 20, 4);
    mip_count = bits(state->tex_mode1[id], 8, 8) / 16u + 1u;
    *size = gx_translate_texture_source_size(format, width, height, mip_count);
    return *size != 0;
}

static void dirty_textures_using_tlut(GxtState* state, u32 tmem)
{
    u32 id;
    for (id = 0; id < GXT_TEXTURE_COUNT; ++id) {
        const u32 is_ci = state->tex_valid[id] & (1u << 4);
        if (is_ci && bits(state->tex_tlut[id], 0, 10) == tmem)
            state->tex_dirty[id] = 1;
    }
}

void gx_translate_invalidate_ram(GxTranslateContext* context, u32 physical, u32 size)
{
    u32 first, last, page, region;
    int interesting = 0;
    u64 epoch;
    if (!context || !size) return;
    physical &= 0x0FFFFFFFu;
    atomic_fetch_add_explicit(&context->invalidation_notifications, 1, memory_order_relaxed);
    if (physical >= GXT_PHYSICAL_SIZE) return;
    if (size > GXT_PHYSICAL_SIZE - physical) size = GXT_PHYSICAL_SIZE - physical;
    /* Fast reject: RAM writers vastly outnumber graphics consumers (about
     * 100k notifications per frame in-match), so a store outside every
     * region a resource has ever read costs one load per 64 KiB touched. */
    for (region = physical >> GXT_REGION_SHIFT;
         region <= (physical + size - 1u) >> GXT_REGION_SHIFT; ++region) {
        if (atomic_load_explicit(&context->interest[region >> 6u],
                                 memory_order_seq_cst) & (1ull << (region & 63u))) {
            interesting = 1;
            break;
        }
    }
    if (!interesting) return;
    epoch = next_invalidation_epoch(context);
    if (!context->page_epochs) {
        atomic_store_explicit(&context->full_invalidation_epoch, epoch, memory_order_relaxed);
        return;
    }
    first = physical >> GXT_PAGE_SHIFT;
    last = (physical + size - 1u) >> GXT_PAGE_SHIFT;
    for (page = first; page <= last; ++page)
        atomic_store_explicit(&context->page_epochs[page], epoch, memory_order_relaxed);
    atomic_fetch_add_explicit(&context->invalidation_pages, (u64)last - first + 1u,
                              memory_order_relaxed);
}

void gx_translate_invalidate_all_ram(GxTranslateContext* context)
{
    if (!context) return;
    atomic_fetch_add_explicit(&context->invalidation_notifications, 1, memory_order_relaxed);
    atomic_store_explicit(&context->full_invalidation_epoch,
                          next_invalidation_epoch(context), memory_order_relaxed);
}

static int bp_tex_slot(u8 reg, u32* slot, u32* kind)
{
    u32 base;
    if (reg >= 0x80u && reg <= 0x9Bu) base = 0x80u;
    else if (reg >= 0xA0u && reg <= 0xBBu) base = 0xA0u;
    else return 0;
    *slot = ((u32)reg - base) & 3u;
    if (base == 0xA0u) *slot += 4u;
    *kind = (((u32)reg - base) & 0x1Fu) / 4u;
    return 1;
}

static int find_tlut(const GxtState* state, u32 tmem)
{
    u32 i;
    for (i = 0; i < GXT_MAX_TLUTS; ++i)
        if (state->tluts[i].valid && state->tluts[i].tmem == tmem) return (int)i;
    return -1;
}

static int alloc_tlut(GxtState* state, u32 tmem)
{
    u32 i;
    const int found = find_tlut(state, tmem);
    if (found >= 0) return found;
    for (i = 0; i < GXT_MAX_TLUTS; ++i) {
        if (!state->tluts[i].valid) {
            memset(&state->tluts[i], 0, sizeof(state->tluts[i]));
            state->tluts[i].valid = 1;
            state->tluts[i].tmem = tmem;
            return (int)i;
        }
    }
    return -1;
}

/* GCN_GXT_LIVE_ARRAYS=1: A/B bisection knob emitting live guest-RAM array
 * pointers instead of snapshots (the pre-snapshot behavior).  Diagnostic
 * only: it reintroduces the async-mutation hazard on purpose so a visual
 * defect can be attributed to the staging layer or ruled out. */
static int live_arrays_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0) {
        const char* value = getenv("GCN_GXT_LIVE_ARRAYS");
        enabled = value && *value && *value != '0';
    }
    return enabled;
}

static void emit_array_command(GxtRun* run, u32 index, u32 size)
{
    const GxtArray* array = &run->state->arrays[index];
    const u8* source = live_arrays_enabled()
        ? run->ram + array->base
        : staged_bytes(array->staged);
    /* Snapshot allocations can reuse an address retired in an earlier frame.
     * Publish an explicit content revision so Aurora never mistakes different
     * geometry at the same host pointer for an existing GPU upload. */
    put_u8(run->sink, GX_AURORA_OPCODE);
    put_be16(run->sink, GX_AURORA_INVALIDATE_ARRAY);
    put_u8(run->sink, (u8)index);
    put_be32(run->sink, array->version);
    put_u8(run->sink, GX_AURORA_OPCODE);
    put_be16(run->sink, (u16)(GX_AURORA_LOAD_ARRAYBASE + index));
    put_be64(run->sink, (u64)(uintptr_t)source);
    put_be32(run->sink, size);
    put_u8(run->sink, 0u);
    run->delta.arraybase_emitted++;
    run->delta.array_source_bytes += size;
}

static int emit_array_extent(GxtRun* run, u32 index, u32 required, int fallback,
                             u8 opcode, u32 offset)
{
    GxtArray* array;
    u32 available, desired;
    u64 epoch;
    if (index >= GXT_ARRAY_COUNT) {
        fail_run(run, GXT_ERROR_MISSING_ARRAY, opcode, offset);
        return 0;
    }
    array = &run->state->arrays[index];
    if (!array->base_valid || !run->ram || array->base >= run->ram_size) {
        fail_run(run, GXT_ERROR_MISSING_ARRAY, opcode, offset);
        return 0;
    }
    available = run->ram_size - array->base;
    if (fallback)
        required = available < GXT_ARRAY_FALLBACK_CAP ? available : GXT_ARRAY_FALLBACK_CAP;
    if (!required || required > available) {
        fail_run(run, GXT_ERROR_RESOURCE_RANGE, opcode, offset);
        return 0;
    }
    if (required > array->frame_required) array->frame_required = required;
    desired = array->frame_required;
    /* A display list reveals its largest vertex index incrementally. Stage
     * geometrically sized snapshots so every larger primitive does not upload
     * another near-identical prefix of the same array. Only RAM is read here;
     * the draw still indexes exactly its declared vertices. */
    { u32 bucket = 256; while (bucket < desired && bucket <= available / 2) bucket *= 2;
      if (bucket >= desired && bucket <= available) desired = bucket; }

    if (run->array_epoch_checked[index] &&
        run->array_epoch_base[index] == array->base &&
        desired <= run->array_epoch_extent[index]) {
        epoch = run->array_epoch_value[index];
    } else {
        epoch = source_epoch(run->context, array->base, desired);
        run->array_epoch_checked[index] = 1;
        run->array_epoch_value[index] = epoch;
        run->array_epoch_base[index] = array->base;
        run->array_epoch_extent[index] = desired;
    }

    /* Aurora's worker reads the array bytes asynchronously, so any change in
     * source content, extent, or base gets a fresh immutable snapshot.  The
     * changed pointer also serves as Aurora's content-change signal.  A page
     * epoch that moved without changing the referenced bytes (a neighboring
     * store in the same 4 KiB page) is absorbed by comparison instead of
     * re-uploading the array. */
    {
        int need_stage = !array->staged || array->emitted_base != array->base ||
                         array->staged_size < desired || array->force_rebind;
        if (!need_stage && array->source_epoch != epoch) {
            if (memcmp(staged_bytes(array->staged), run->ram + array->base,
                       desired) == 0)
                array->source_epoch = epoch;
            else
                need_stage = 1;
        }
        if (need_stage) {
            GxtStaged* block =
                cache_acquire(run, array->base, desired, epoch, opcode, offset);
            if (!block) return 0;
            if (array->staged) run->delta.resource_rebinds++;
            array->staged = block;   /* borrowed from the content cache */
            array->staged_size = desired;
            array->source_epoch = epoch;
            array->emitted_base = array->base;
            array->version = next_nonzero(&run->state->array_version_clock);
            array->metadata_valid = 0;
            array->force_rebind = 0;
        }
    }
    if (!array->emitted_this_frame || !array->metadata_valid ||
        array->emitted_size != desired) {
        emit_array_command(run, index, desired);
        array->emitted_size = desired;
        array->metadata_valid = 1;
        array->emitted_this_frame = 1;
    }
    return 1;
}

static int emit_tlut(GxtRun* run, u32 texture_id, u32 tmem, u32 format, u32* out_slot,
                     u8 opcode, u32 offset)
{
    const int index = find_tlut(run->state, tmem);
    GxtTlut* tlut;
    *out_slot = 0;
    if (index < 0) {
        fail_run(run, GXT_ERROR_RESOURCE_RANGE, opcode, offset);
        return 0;
    }
    tlut = &run->state->tluts[index];
    *out_slot = texture_id;
    /* TLUT bytes were snapshotted at the LOADTLUT trigger, matching hardware
     * TMEM semantics: later guest writes to the source RAM are invisible
     * until the palette is loaded again. */
    if (!tlut->data) {
        fail_run(run, GXT_ERROR_RESOURCE_RANGE, opcode, offset);
        return 0;
    }
    if (tlut->revision_pending || !tlut->version) {
        tlut->version = next_nonzero(&run->state->tlut_version_clock);
        tlut->revision_pending = 0;
    }
    GxtTlutBinding* binding = &run->state->tlut_bindings[texture_id];
    if (binding->valid && binding->tmem == tmem && binding->format == format &&
        binding->source_version == tlut->version) return 1;

    put_u8(run->sink, GX_AURORA_OPCODE);
    put_be16(run->sink, GX_AURORA_LOAD_TLUT);
    put_u8(run->sink, (u8)texture_id);
    put_be64(run->sink, (u64)(uintptr_t)staged_bytes(tlut->data));
    put_be32(run->sink, format);
    put_be16(run->sink, tlut->entries);
    put_be32(run->sink, texture_id + 1u);
    put_be32(run->sink, next_nonzero(&run->state->tlut_version_clock));
    binding->valid = 1;
    binding->tmem = tmem;
    binding->format = format;
    binding->source_version = tlut->version;
    tlut->dirty = 0;
    run->delta.tluts_emitted++;
    run->delta.tlut_source_bytes += tlut->data->size;
    return 1;
}

static int emit_dirty_texobjs(GxtRun* run, u8 opcode, u32 offset)
{
    u32 id;
    if (!run->tex_scan_pending) return 1;
    for (id = 0; id < GXT_TEXTURE_COUNT; ++id) {
        u32 phys, source_size, image0, width, height, format, mip_count;
        u64 source_rev;
        u32 tlut_slot = 0;
        const u8* source_ptr;
        if (!texture_range(run->state, id, &phys, &source_size)) {
            if (!run->state->tex_dirty[id]) continue;
            fail_run(run, GXT_ERROR_RESOURCE_RANGE, opcode, offset);
            return 0;
        }
        if (run->tex_epoch_checked[id] && run->tex_epoch_phys[id] == phys &&
            run->tex_epoch_size[id] == source_size) {
            source_rev = run->tex_epoch_value[id];
        } else {
            source_rev = source_epoch(run->context, phys, source_size);
            run->tex_epoch_checked[id] = 1;
            run->tex_epoch_value[id] = source_rev;
            run->tex_epoch_phys[id] = phys;
            run->tex_epoch_size[id] = source_size;
            if (run->state->tex_source_epoch[id] != source_rev) {
                GxtStaged* staged = run->state->tex_staged[id];
                if (staged && !run->state->tex_dirty[id] &&
                    run->state->tex_staged_phys[id] == phys &&
                    staged->size == source_size && run->ram &&
                    phys < run->ram_size && source_size <= run->ram_size - phys &&
                    memcmp(staged_bytes(staged), run->ram + phys, source_size) == 0) {
                    /* Page epoch moved but the texture bytes did not (false
                     * sharing inside the 4 KiB pages): absorb without a
                     * re-upload/re-conversion. */
                    run->state->tex_source_epoch[id] = source_rev;
                } else {
                    run->state->tex_dirty[id] = 1;
                    run->state->tex_revision_pending[id] = 1;
                    run->delta.resource_rebinds++;
                }
            }
        }
        image0 = run->state->tex_image0[id];
        width = bits(image0, 0, 10) + 1u;
        height = bits(image0, 10, 10) + 1u;
        format = bits(image0, 20, 4);
        mip_count = bits(run->state->tex_mode1[id], 8, 8) / 16u + 1u;
        if (!run->state->tex_dirty[id]) continue;
        if (!run->ram || phys >= run->ram_size || source_size > run->ram_size - phys) {
            fail_run(run, GXT_ERROR_RESOURCE_RANGE, opcode, offset);
            return 0;
        }
        if (format >= 8u && format <= 10u) {
            const u32 required = 1u << 4;
            const u32 raw_tlut = run->state->tex_tlut[id];
            if ((run->state->tex_valid[id] & required) != required ||
                !emit_tlut(run, id, bits(raw_tlut, 0, 10), bits(raw_tlut, 10, 2),
                           &tlut_slot, opcode, offset)) {
                if (run->error == GXT_ERROR_NONE)
                    fail_run(run, GXT_ERROR_RESOURCE_RANGE, opcode, offset);
                return 0;
            }
        }
        if (copy_dest_live(run, phys)) {
            /* Aurora resolves this texture from its EFB-copy cache by pointer
             * identity; the pointed-to guest bytes are never read. */
            source_ptr = run->ram + phys;
            run->state->tex_staged[id] = NULL;   /* cache-owned; just unbind */
        } else {
            GxtStaged* block =
                cache_acquire(run, phys, source_size, source_rev, opcode, offset);
            if (!block) return 0;
            run->state->tex_staged[id] = block;  /* borrowed */
            run->state->tex_staged_phys[id] = phys;
            source_ptr = staged_bytes(block);
        }
        /* Optional forensic dump of small indexed texture/palette pairs. */
        if (format == 8u && source_size <= 2048u) {
            static FILE* capture; static int checked; static u32 captured;
            if (!checked) { const char* path = getenv("GCN_GXT_PALETTE_DUMP");
                if (path && *path) capture = fopen(path, "wb"); checked = 1; }
            if (capture && captured < 32768u) {
                const GxtTlut* palette = &run->state->tluts[find_tlut(run->state, bits(run->state->tex_tlut[id], 0, 10))];
                const u32 header[9] = {0x50414C31u, phys, width, height, source_size,
                    bits(run->state->tex_tlut[id], 10, 2), palette->phys,
                    (u32)palette->data->size, run->state->tex_mode1[id]};
                fwrite(header, sizeof header, 1, capture);
                fwrite(source_ptr, source_size, 1, capture);
                fwrite(staged_bytes(palette->data), palette->data->size, 1, capture);
                ++captured;
            }
        }
        if (run->state->tex_revision_pending[id] || !run->state->tex_version[id])
            run->state->tex_version[id] = next_nonzero(&run->state->texture_version_clock);
        put_u8(run->sink, GX_AURORA_OPCODE);
        put_be16(run->sink, GX_AURORA_LOAD_TEXOBJ);
        put_u8(run->sink, (u8)id);
        put_be64(run->sink, (u64)(uintptr_t)source_ptr);
        put_be32(run->sink, width);
        put_be32(run->sink, height);
        put_be32(run->sink, format);
        put_be32(run->sink, tlut_slot);
        put_u8(run->sink, (u8)(mip_count > 1u));
        put_be32(run->sink, id + 1u);
        put_be32(run->sink, run->state->tex_version[id]);
        run->state->tex_dirty[id] = 0;
        run->state->tex_revision_pending[id] = 0;
        run->state->tex_source_epoch[id] = source_rev;
        run->delta.texture_objects_emitted++;
        run->delta.texture_source_bytes += source_size;
    }
    run->tex_scan_pending = 0;
    return 1;
}

static u32 attr_desc(const GxtState* state, u32 cp_index)
{
    if (cp_index < 4u) return bits(state->vcd_lo, 9u + cp_index * 2u, 2);
    return bits(state->vcd_hi, (cp_index - 4u) * 2u, 2);
}

static void attr_format(const GxtState* state, u32 fmt, u32 cp_index,
                        u32* component_count, u32* component_format,
                        u32* packed_size, u32* nbt3)
{
    const u32 a = state->vat[fmt & 7u][0];
    const u32 b = state->vat[fmt & 7u][1];
    const u32 c = state->vat[fmt & 7u][2];
    *packed_size = 0;
    *nbt3 = 0;
    switch (cp_index) {
    case 0:
        *component_count = bits(a, 0, 1) ? 3u : 2u;
        *component_format = bits(a, 1, 3);
        break;
    case 1:
        *nbt3 = bits(a, 31, 1);
        *component_count = bits(a, 9, 1) ? 9u : 3u;
        *component_format = bits(a, 10, 3);
        break;
    case 2:
        *component_count = 1;
        *component_format = bits(a, 14, 3);
        *packed_size = *component_format <= 5u ? kColSize[*component_format] : 0u;
        break;
    case 3:
        *component_count = 1;
        *component_format = bits(a, 18, 3);
        *packed_size = *component_format <= 5u ? kColSize[*component_format] : 0u;
        break;
    default: {
        const u32 tex = cp_index - 4u;
        const u32 regs[8] = {a, b, b, b, b, c, c, c};
        const u8 count_bits[8] = {21, 0, 9, 18, 27, 5, 14, 23};
        const u32 reg = regs[tex];
        *component_count = bits(reg, count_bits[tex], 1) ? 2u : 1u;
        *component_format = bits(reg, (u32)count_bits[tex] + 1u, 3);
        break;
    }
    }
}

static int build_vertex_layout(const GxtState* state, u32 fmt,
                               GxtIndexedAttr indexed[GXT_ARRAY_COUNT], u32* vertex_size)
{
    u32 cp_index, size = 0, i;
    memset(indexed, 0, sizeof(GxtIndexedAttr) * GXT_ARRAY_COUNT);
    for (i = 0; i < 9u; ++i)
        if (bits(state->vcd_lo, i, 1)) ++size;

    for (cp_index = 0; cp_index < 12u; ++cp_index) {
        const u32 desc = attr_desc(state, cp_index);
        u32 count, format, packed, nbt3, element_size = 0;
        if (!desc) continue;
        attr_format(state, fmt, cp_index, &count, &format, &packed, &nbt3);
        if (packed) element_size = packed;
        else if (format < sizeof(kFmtSize)) {
            if (cp_index == 1u && nbt3) count = 3u;
            element_size = count * kFmtSize[format];
        }

        if (desc == 1u) {
            u32 direct_count = count;
            if (cp_index == 1u && nbt3) direct_count = 9u;
            if (packed) element_size = packed;
            else if (format < sizeof(kFmtSize)) element_size = direct_count * kFmtSize[format];
            else return 0;
            size += element_size;
        } else {
            GxtIndexedAttr* item = &indexed[cp_index];
            item->active = 1;
            item->offset = (u8)size;
            item->index_width = desc == 2u ? 1u : 2u;
            item->index_count = (u8)((cp_index == 1u && nbt3) ? 3u : 1u);
            item->cp_index = (u8)cp_index;
            item->element_size = (u16)element_size;
            item->fallback_extent = (u8)(element_size == 0);
            size += item->index_width * item->index_count;
        }
        if (size > 255u) return 0;
    }
    *vertex_size = size;
    return size != 0;
}

static int emit_draw_arrays(GxtRun* run, const u8* vertices, u32 count, u32 vertex_size,
                            const GxtIndexedAttr indexed[GXT_ARRAY_COUNT],
                            u8 opcode, u32 offset)
{
    u32 attr;
    for (attr = 0; attr < 12u; ++attr) {
        const GxtIndexedAttr* item = &indexed[attr];
        u32 vertex, max_index = 0;
        u64 required;
        if (!item->active) continue;
        for (vertex = 0; vertex < count; ++vertex) {
            const u8* source = vertices + vertex * vertex_size + item->offset;
            u32 n;
            for (n = 0; n < item->index_count; ++n) {
                const u32 index = item->index_width == 1u
                                    ? source[n]
                                    : rd_be16(source + n * 2u);
                if (index > max_index) max_index = index;
            }
        }
        if (item->fallback_extent) {
            if (!emit_array_extent(run, item->cp_index, 0, 1, opcode, offset)) return 0;
            continue;
        }
        required = (u64)max_index * run->state->arrays[item->cp_index].stride +
                   item->element_size;
        if (!required || required > UINT32_MAX) {
            fail_run(run, GXT_ERROR_RESOURCE_RANGE, opcode, offset);
            return 0;
        }
        if (!emit_array_extent(run, item->cp_index, (u32)required, 0, opcode, offset)) return 0;
    }
    return 1;
}

static void note_texture_register(GxtState* state, u32 slot, u32 kind, u32 value)
{
    u32* destination = NULL;
    u32 old = 0;
    u8 valid_bit = 0;
    if (kind == 0u) { destination = &state->tex_mode0[slot]; valid_bit = 1u << 0; }
    else if (kind == 1u) { destination = &state->tex_mode1[slot]; valid_bit = 1u << 1; }
    else if (kind == 2u) { destination = &state->tex_image0[slot]; valid_bit = 1u << 2; }
    else if (kind == 5u) { destination = &state->tex_image3[slot]; valid_bit = 1u << 3; }
    else if (kind == 6u) { destination = &state->tex_tlut[slot]; valid_bit = 1u << 4; }
    if (!destination) return;
    old = *destination;
    if (!(state->tex_valid[slot] & valid_bit) || *destination != value) {
        *destination = value;
        state->tex_valid[slot] |= valid_bit;
        /* Mode0 is fully represented by the original BP command. Mode1 only
         * affects Aurora metadata when the source mip count changes. */
        if (kind != 0u && (kind != 1u || bits(old, 8, 8) / 16u != bits(value, 8, 8) / 16u)) {
            state->tex_dirty[slot] = 1;
            state->tex_revision_pending[slot] = 1;
        }
    }
}

/*
 * Aurora consumes EFB-to-texture copies through the metadata its own SDK layer
 * would have queued (GXSetTexCopySrc/GXSetTexCopyDst/GXCopyTex), not from the
 * raw BP copy registers.  Rebuild that metadata from the masked BP state so a
 * raw BP 0x52 trigger performs the same resolve Aurora's GXCopyTex would.
 *
 * The 4-bit BP target field stores the real EFB copy format as
 * real = target / 2 + (target & 1) * 8 (Dolphin UPE_Copy::tp_realFormat).
 * Combined with the intensity bit (BP 0x52 bit 15) and a Z24 EFB pixel format
 * (BP 0x43), it reconstructs the GXTexFmt GXSetTexCopyDst was called with.
 */
static u32 copy_dest_format(u32 real_format, u32 intensity, u32 depth)
{
    /* GX_CTF_R4, invalid, GX_CTF_RA4, GX_CTF_RA8, GX_TF_RGB565, GX_TF_RGB5A3,
     * GX_TF_RGBA8, GX_CTF_A8, GX_CTF_R8, GX_CTF_G8, GX_CTF_B8, GX_CTF_RG8,
     * GX_CTF_GB8 */
    static const u8 color_map[13] = {0x20, 0xFF, 0x22, 0x23, 0x04, 0x05, 0x06,
                                     0x27, 0x28, 0x29, 0x2A, 0x2B, 0x2C};
    if (depth) {
        switch (real_format) {
        case 0u:  return 0x30u; /* GX_CTF_Z4 */
        case 1u:  return 0x11u; /* GX_TF_Z8 */
        case 3u:  return 0x13u; /* GX_TF_Z16 */
        case 6u:  return 0x16u; /* GX_TF_Z24X8 */
        case 9u:  return 0x39u; /* GX_CTF_Z8M */
        case 10u: return 0x3Au; /* GX_CTF_Z8L */
        case 12u: return 0x3Cu; /* GX_CTF_Z16L */
        default:  return 0xFFFFFFFFu;
        }
    }
    if (intensity) {
        switch (real_format) {
        case 0u: return 0x00u;  /* GX_TF_I4 */
        case 1u: return 0x01u;  /* GX_TF_I8 */
        case 2u: return 0x02u;  /* GX_TF_IA4 */
        case 3u: return 0x03u;  /* GX_TF_IA8 */
        case 6u: return 0x26u;  /* GX_CTF_YUVA8 */
        default: return 0xFFFFFFFFu;
        }
    }
    if (real_format < 13u && color_map[real_format] != 0xFFu)
        return color_map[real_format];
    return 0xFFFFFFFFu;
}

/* Guest bytes the copy engine would tile into RAM at the destination; used
 * only to validate the destination pointer stays inside guest RAM. */
static u32 copy_dest_size(u32 real_format, u32 width, u32 height)
{
    u32 tile_w = 4u, tile_h = 4u, tile_bytes = 32u;
    if (real_format == 0u) { tile_w = 8u; tile_h = 8u; }
    else if (real_format == 1u || real_format == 2u || real_format == 7u ||
             real_format == 8u || real_format == 9u || real_format == 10u)
        tile_w = 8u;
    else if (real_format == 6u)
        tile_bytes = 64u;
    return ((width + tile_w - 1u) / tile_w) *
           ((height + tile_h - 1u) / tile_h) * tile_bytes;
}

static int emit_texture_copy(GxtRun* run, u32 trigger, u8 opcode, u32 offset)
{
    GxtState* state = run->state;
    const u32 src_reg = state->bp_valid[0x49] ? state->bp_cache[0x49] : 0u;
    const u32 half_scale = bits(trigger, 9, 1);
    const u32 intensity = bits(trigger, 15, 1);
    const u32 depth = state->bp_valid[0x43] &&
                      bits(state->bp_cache[0x43], 0, 3) == 3u;
    u32 left, top, src_w, src_h, out_w, out_h;
    u32 target, real_format, format, dest, dest_size;
    if (!state->bp_valid[0x4A] || !state->bp_valid[0x4B]) {
        fail_run(run, GXT_ERROR_RESOURCE_RANGE, opcode, offset);
        return 0;
    }
    left = bits(src_reg, 0, 10);
    top = bits(src_reg, 10, 10);
    src_w = bits(state->bp_cache[0x4A], 0, 10) + 1u;
    src_h = bits(state->bp_cache[0x4A], 10, 10) + 1u;
    out_w = half_scale ? (src_w + 1u) >> 1 : src_w;
    out_h = half_scale ? (src_h + 1u) >> 1 : src_h;
    target = bits(trigger, 3, 4);
    real_format = target / 2u + (target & 1u) * 8u;
    format = copy_dest_format(real_format, intensity, depth);
    dest = (state->bp_cache[0x4B] << 5) & 0x1FFFFFFFu;
    dest_size = copy_dest_size(real_format, out_w, out_h);
    if (format == 0xFFFFFFFFu || !dest_size || !run->ram ||
        dest >= run->ram_size || dest_size > run->ram_size - dest) {
        fail_run(run, GXT_ERROR_RESOURCE_RANGE, opcode, offset);
        return 0;
    }

    put_u8(run->sink, GX_AURORA_OPCODE);
    put_be16(run->sink, GX_AURORA_LOAD_COPY_SRC);
    put_be32(run->sink, left);
    put_be32(run->sink, top);
    put_be32(run->sink, src_w);
    put_be32(run->sink, src_h);

    put_u8(run->sink, GX_AURORA_OPCODE);
    put_be16(run->sink, GX_AURORA_LOAD_COPY_DST);
    put_be32(run->sink, out_w);
    put_be32(run->sink, out_h);
    put_be32(run->sink, format);
    put_u8(run->sink, (u8)half_scale);

    put_u8(run->sink, GX_AURORA_OPCODE);
    put_be16(run->sink, GX_AURORA_LOAD_COPY_DEST);
    put_be64(run->sink, (u64)(uintptr_t)(run->ram + dest));

    if(getenv("MELEE_TRACE_COPIES")&&out_w>300)fprintf(stderr,"[copy] created %08X size=%u %ux%u fmt=%u clear=%u\n",dest,dest_size,out_w,out_h,format,bits(trigger,11,1));
    note_copy_dest(run, dest, dest_size);
    /* A texture already bound at the destination must re-resolve so Aurora
     * picks up the fresh copy; the copy itself does not touch guest RAM, so
     * page epochs cannot signal this. */
    {
        u32 id;
        for (id = 0; id < GXT_TEXTURE_COUNT; ++id) {
            u32 tex_phys, tex_size;
            if (texture_range(state, id, &tex_phys, &tex_size) &&
                tex_phys == dest)
                state->tex_dirty[id] = 1;
        }
    }
    run->tex_scan_pending = 1;
    return 1;
}

static u32 apply_bp_mask(GxtState* state, u8 reg, u32 value)
{
    const u32 old = state->bp_valid[reg] ? state->bp_cache[reg] : 0u;
    const u32 effective = (old & ~state->bp_mask) | (value & state->bp_mask);
    state->bp_mask = 0x00FFFFFFu;
    state->bp_cache[reg] = effective & 0x00FFFFFFu;
    state->bp_valid[reg] = 1;
    return effective & 0x00FFFFFFu;
}

static int walk_stream(GxtRun* run, const u8* input, u32 length, u32 depth)
{
    u32 i = 0;
    if (depth > GXT_MAX_DL_DEPTH) {
        fail_run(run, GXT_ERROR_DISPLAY_LIST_DEPTH, 0x40u, 0);
        return 0;
    }
    while (i < length && run->error == GXT_ERROR_NONE && !run->sink->overflow) {
        const u8 opcode = input[i];
        if (opcode == 0x00u || opcode == 0x01u || opcode == 0x44u || opcode == 0x48u) {
            if (opcode == 0x48u) {
                u32 array_index;
                for (array_index = 0; array_index < GXT_ARRAY_COUNT; ++array_index)
                    if (run->state->arrays[array_index].base_valid)
                        run->state->arrays[array_index].force_rebind = 1;
            }
            /* Aurora masks opcodes with 0xF8, so retail's one-byte 0x44
             * metrics command would otherwise be misread as a nine-byte
             * CALL_DL. Translate that unsupported metrics opcode to a NOP. */
            put_u8(run->sink, opcode == 0x44u ? 0x00u : opcode);
            ++i;
            continue;
        }
        if (opcode == 0x08u) {
            u8 reg;
            u32 value;
            if (length - i < 6u) {
                fail_run(run, GXT_ERROR_TRUNCATED_COMMAND, opcode, i);
                return 0;
            }
            reg = input[i + 1];
            value = rd_be32(input + i + 2);
            if (reg == 0x30u || reg == 0x40u) {
                const u32 row = bits(value, 0, 6);
                if (reg == 0x30u) run->state->current_mtx_row = row;
                if (row > run->delta.max_pnmtx_row) run->delta.max_pnmtx_row = row;
                if (row > 27u) run->delta.matrix_index_high++;
                /* Hardware indexes matrix RAM by ROW; Aurora's palette model
                 * assumes 3-aligned starts (idx = row/3).  A misaligned row
                 * silently applies a matrix offset by 1-2 rows — sheared or
                 * flattened geometry.  Count it loudly. */
                if (row % 3u) {
                    run->delta.matrix_row_misaligned++;
                    if (row > run->delta.matrix_row_misaligned_max)
                        run->delta.matrix_row_misaligned_max = row;
                }
            }
            if (reg == 0x50u) run->state->vcd_lo = value;
            else if (reg == 0x60u) run->state->vcd_hi = value;
            else if (reg >= 0x70u && reg <= 0x77u) run->state->vat[reg & 7u][0] = value;
            else if (reg >= 0x80u && reg <= 0x87u) run->state->vat[reg & 7u][1] = value;
            else if (reg >= 0x90u && reg <= 0x97u) run->state->vat[reg & 7u][2] = value;
            else if ((reg & 0xF0u) == 0xB0u) set_array_stride_state(run->state, reg & 0x0Fu, value);
            if ((reg & 0xF0u) == 0xA0u) {
                set_array_base_state(run->state, reg & 0x0Fu, value);
                put_u8(run->sink, 0x00u);
            } else {
                put(run->sink, input + i, 6u);
            }
            i += 6u;
            continue;
        }
        if (opcode == 0x61u) {
            u32 raw, value, slot, kind;
            u8 reg;
            if (length - i < 5u) {
                fail_run(run, GXT_ERROR_TRUNCATED_COMMAND, opcode, i);
                return 0;
            }
            raw = rd_be32(input + i + 1);
            reg = (u8)(raw >> 24);
            value = raw & 0x00FFFFFFu;
            if (reg == 0xFEu) {
                run->state->bp_mask = value;
                run->state->bp_cache[reg] = value;
                run->state->bp_valid[reg] = 1;
            } else {
                value = apply_bp_mask(run->state, reg, value);
                /* GCN_GXT_NO_FOG=1: A/B bisection knob clearing the fog
                 * function select (BP 0xF1 bits 21-23) so fogged draws render
                 * unfogged.  Diagnostic only. */
                if (reg == 0xF1u) {
                    static int s_no_fog = -1;
                    if (s_no_fog < 0) {
                        const char* env = getenv("GCN_GXT_NO_FOG");
                        s_no_fog = env && *env && *env != '0';
                    }
                    if (s_no_fog) {
                        value &= ~0x00E00000u;
                        run->state->bp_cache[reg] = value;
                        put_u8(run->sink, 0x61u);
                        put_be32(run->sink, ((u32)reg << 24) | value);
                        i += 5u;
                        continue;
                    }
                }
                if (reg == 0x52u) {
                    if (bits(value, 14, 1)) {
                        run->delta.display_copies++;
                        arrays_begin_frame(run->state);
                        /* Aurora stubs display copies with a per-call warning
                         * and presents its own framebuffer at end-of-frame;
                         * the copy's EFB clear is honored by the clearColor
                         * load op at the next recording start.  Drop the
                         * trigger so a 60 Hz warn loop cannot form. */
                        put(run->sink, "\x00\x00\x00\x00\x00", 5u);
                        i += 5u;
                        continue;
                    }
                    if (!emit_texture_copy(run, value, opcode, i)) return 0;
                    run->delta.texture_copies++;
                }
                if (reg == 0x64u) {
                    run->state->tlut_src_phys = (value << 5) & 0x01FFFFFFu;
                    run->state->tlut_src_valid = 1;
                } else if (reg == 0x65u) {
                    const u32 tmem = bits(value, 0, 10);
                    const u32 lines = bits(value, 10, 11);
                    const u32 entries = lines * 16u; /* 32-byte TMEM lines */
                    if (lines && run->state->tlut_src_valid) {
                        /* Hardware snapshots the palette into TMEM at the
                         * load trigger; copy now so later guest writes to the
                         * source RAM cannot alter the loaded palette. */
                        const u32 source_size = gx_translate_tlut_source_size(entries);
                        const u32 phys = run->state->tlut_src_phys;
                        const int index = alloc_tlut(run->state, tmem);
                        GxtStaged* block;
                        GxtTlut* tlut;
                        if (index < 0 || !run->ram || !source_size ||
                            phys >= run->ram_size ||
                            source_size > run->ram_size - phys) {
                            fail_run(run, GXT_ERROR_RESOURCE_RANGE, opcode, i);
                            return 0;
                        }
                        block = stage_copy(run, run->ram + phys, source_size,
                                           opcode, i);
                        if (!block) return 0;
                        tlut = &run->state->tluts[index];
                        stage_replace(run, &tlut->data, block);
                        tlut->phys = phys;
                        tlut->entries = (u16)entries;
                        tlut->dirty = 1;
                        tlut->revision_pending = 1;
                        dirty_textures_using_tlut(run->state, tmem);
                        run->tex_scan_pending = 1;
                    }
                }
                if (bp_tex_slot(reg, &slot, &kind)) {
                    note_texture_register(run->state, slot, kind, value);
                    run->tex_scan_pending = 1;
                }
            }
            put(run->sink, input + i, 5u);
            i += 5u;
            continue;
        }
        if (opcode == 0x20u || opcode == 0x28u || opcode == 0x30u || opcode == 0x38u) {
            u32 array_index, source_index, count, required;
            u64 required64;
            if (length - i < 5u) {
                fail_run(run, GXT_ERROR_TRUNCATED_COMMAND, opcode, i);
                return 0;
            }
            array_index = 12u + ((u32)opcode - 0x20u) / 8u;
            source_index = rd_be16(input + i + 1);
            count = (rd_be16(input + i + 3) >> 12) + 1u;
            required64 = (u64)source_index * run->state->arrays[array_index].stride + count * 4u;
            if (!required64 || required64 > UINT32_MAX) {
                fail_run(run, GXT_ERROR_RESOURCE_RANGE, opcode, i);
                return 0;
            }
            required = (u32)required64;
            if (!emit_array_extent(run, array_index, required, 0, opcode, i)) return 0;
            put(run->sink, input + i, 5u);
            i += 5u;
            continue;
        }
        if (opcode == 0x10u) {
            u32 header, address, count, step;
            if (length - i < 5u) {
                fail_run(run, GXT_ERROR_TRUNCATED_COMMAND, opcode, i);
                return 0;
            }
            header = rd_be32(input + i + 1);
            address = header & 0xFFFFu;
            count = ((header >> 16) & 0x0Fu) + 1u;
            step = 5u + count * 4u;
            if (step > length - i) {
                fail_run(run, GXT_ERROR_TRUNCATED_COMMAND, opcode, i);
                return 0;
            }
            /* Diagnostics for Aurora's partial XF-memory model: writes to
             * matrix RAM outside its modeled slices (pos/tex rows 0-62,
             * normal rows, post-transform, lights) are silently dropped by
             * Aurora's release build.  The final 0x5F4 identity load is the
             * SDK's GX_PTIDENTITY slot and is synthesized by Aurora, so it is
             * intentionally excluded from the missing-state census. */
            if (address < 0x1000u &&
                !(address < 0x0FCu) &&
                !(address >= 0x400u && address < 0x45Au) &&
                !(address >= 0x500u && address < 0x5F0u) &&
                !(address == 0x5F4u && count == 12u) &&
                !(address >= 0x600u && address < 0x680u)) {
                u32 word;
                run->delta.xf_unmodeled_writes++;
                if (xf_unmodeled_log_enabled() &&
                    !atomic_exchange_explicit(&s_xf_unmodeled_logged, 1u,
                                              memory_order_relaxed)) {
                    fprintf(stderr,
                            "gx_translate: first unmodeled XF write address=%04X count=%u values=",
                            address, count);
                    for (word = 0; word < count; ++word)
                        fprintf(stderr, "%s%08X", word ? "," : "",
                                rd_be32(input + i + 5u + word * 4u));
                    fputc('\n', stderr);
                }
            }
            if (address == 0x1018u || address == 0x1019u) {
                const u32 row = bits(rd_be32(input + i + 5), 0, 6);
                if (row > run->delta.max_pnmtx_row) run->delta.max_pnmtx_row = row;
                if (row > 27u) run->delta.matrix_index_high++;
                if (row % 3u) {
                    run->delta.matrix_row_misaligned++;
                    if (row > run->delta.matrix_row_misaligned_max)
                        run->delta.matrix_row_misaligned_max = row;
                }
            }
            if (address >= 0x1009u && address < 0x1009u + 10u) {
                u32 word;
                for (word = 0; word < count &&
                     address + word < 0x1009u + 10u; ++word)
                    run->state->xf_chan[address + word - 0x1009u] =
                        rd_be32(input + i + 5 + word * 4u);
            }
            put(run->sink, input + i, step);
            i += step;
            continue;
        }
        if (opcode == 0x40u) {
            u32 address, bytes;
            if (length - i < 9u) {
                fail_run(run, GXT_ERROR_TRUNCATED_COMMAND, opcode, i);
                return 0;
            }
            address = rd_be32(input + i + 1) & 0x0FFFFFFFu;
            bytes = rd_be32(input + i + 5);
            i += 9u;
            if (!bytes) continue;
            if (!run->ram || address >= run->ram_size || bytes > run->ram_size - address) {
                fail_run(run, GXT_ERROR_DISPLAY_LIST_RANGE, opcode, i - 9u);
                return 0;
            }
            run->delta.display_lists_inlined++;
            run->delta.display_list_bytes += bytes;
            if (!walk_stream(run, run->ram + address, bytes, depth + 1u)) return 0;
            continue;
        }
        if (opcode >= 0x80u && opcode <= 0xBFu) {
            /* GCN_GXT_DRAWDUMP=<path>: forensic per-draw register snapshot
             * (blend/z/TEV/dst-alpha state at each draw) for visual-parity
             * hunts.  Opt-in only; never part of a production run. */
            static FILE* s_drawdump;
            static int s_drawdump_checked;
            static u64 s_drawdump_seq;
            GxtIndexedAttr indexed[GXT_ARRAY_COUNT];
            u32 count, vertex_size, step;
            if (!s_drawdump_checked) {
                const char* path = getenv("GCN_GXT_DRAWDUMP");
                if (path && *path) s_drawdump = fopen(path, "w");
                s_drawdump_checked = 1;
            }
            if (length - i < 3u) {
                fail_run(run, GXT_ERROR_TRUNCATED_COMMAND, opcode, i);
                return 0;
            }
            count = rd_be16(input + i + 1);
            if (count == 0u) {
                put(run->sink, input + i, 3u);
                i += 3u;
                continue;
            }
            if (!build_vertex_layout(run->state, opcode & 7u, indexed, &vertex_size)) {
                fail_run(run, GXT_ERROR_INVALID_VERTEX_FORMAT, opcode, i);
                return 0;
            }
            if (count > (length - i - 3u) / vertex_size) {
                fail_run(run, GXT_ERROR_TRUNCATED_COMMAND, opcode, i);
                return 0;
            }
            step = 3u + count * vertex_size;
            if (s_drawdump) {
                const GxtState* st = run->state;
                fprintf(s_drawdump,
                        "draw=%llu op=%02X n=%u vcd=%06X genmode=%06X zmode=%06X "
                        "blend=%06X dstalpha=%06X acmp=%06X tref0=%06X "
                        "tev0c=%06X tev0a=%06X ksel0=%06X pnrow=%u "
                        "nchan=%08X amb0=%08X mat0=%08X ctrl0=%08X ctrla0=%08X\n",
                        (unsigned long long)s_drawdump_seq++, opcode, count,
                        st->vcd_lo, st->bp_cache[0x00], st->bp_cache[0x40],
                        st->bp_cache[0x41], st->bp_cache[0x42], st->bp_cache[0xF3],
                        st->bp_cache[0x28], st->bp_cache[0xC0], st->bp_cache[0xC1],
                        st->bp_cache[0xF6], st->current_mtx_row,
                        st->xf_chan[0], st->xf_chan[1], st->xf_chan[3],
                        st->xf_chan[5], st->xf_chan[7]);
            }
            if (bits(run->state->vcd_lo, 0, 1)) {
                /* Direct per-vertex PNMTXIDX rows (matrix-palette skinning). */
                u32 vertex;
                for (vertex = 0; vertex < count; ++vertex) {
                    const u32 row = input[i + 3u + vertex * vertex_size];
                    if (row > run->delta.max_pnmtx_row)
                        run->delta.max_pnmtx_row = row;
                    if (row > 27u) run->delta.matrix_index_high++;
                }
            }
            if (!emit_draw_arrays(run, input + i + 3u, count, vertex_size,
                                  indexed, opcode, i)) return 0;
            if (!emit_dirty_texobjs(run, opcode, i)) return 0;
            put(run->sink, input + i, step);
            i += step;
            run->delta.draws++;
            continue;
        }
        fail_run(run, GXT_ERROR_UNKNOWN_OPCODE, opcode, i);
        return 0;
    }
    return run->error == GXT_ERROR_NONE && !run->sink->overflow;
}

static void init_state(GxtState* state)
{
    memset(state, 0, sizeof(*state));
    state->bp_mask = 0x00FFFFFFu;
}

static void init_context(GxTranslateContext* context)
{
    u32 word;
    init_state(&context->state);
    for (word = 0; word < GXT_REGION_WORDS; ++word)
        atomic_init(&context->interest[word], 0);
    atomic_init(&context->invalidation_epoch, 0);
    atomic_init(&context->full_invalidation_epoch, 0);
    atomic_init(&context->invalidation_notifications, 0);
    atomic_init(&context->invalidation_pages, 0);
    (void)allocate_page_epochs(context);
}

GxTranslateContext* gx_translate_create(void)
{
    GxTranslateContext* context = (GxTranslateContext*)calloc(1, sizeof(*context));
    if (context) init_context(context);
    return context;
}

void gx_translate_destroy(GxTranslateContext* context)
{
    if (!context) return;
    free_all_staging(context);
    free(context->page_epochs);
    free(context);
}

void gx_translate_reset(GxTranslateContext* context)
{
    if (!context) return;
    free_all_staging(context);
    free(context->page_epochs);
    memset(context, 0, sizeof(*context));
    init_context(context);
}

void gx_translate_begin_frame(GxTranslateContext* context)
{
    if (context) arrays_begin_frame(&context->state);
}

void gx_translate_frame_drained(GxTranslateContext* context)
{
    if (!context) return;
    free_staged_chain(context->retired);
    context->retired = NULL;
}

GxTranslateError gx_translate_last_error(const GxTranslateContext* context)
{
    return context ? context->last_error : GXT_ERROR_INVALID_ARGUMENT;
}

u32 gx_translate_last_error_offset(const GxTranslateContext* context)
{
    return context ? context->last_error_offset : 0;
}

u8 gx_translate_last_error_opcode(const GxTranslateContext* context)
{
    return context ? context->last_error_opcode : 0;
}

const GxTranslateStats* gx_translate_stats(const GxTranslateContext* context)
{
    GxTranslateContext* mutable_context;
    if (!context) return NULL;
    mutable_context = (GxTranslateContext*)(uintptr_t)context;
    mutable_context->stats.ram_invalidations =
        atomic_load_explicit(&context->invalidation_notifications, memory_order_relaxed);
    mutable_context->stats.ram_invalidation_pages =
        atomic_load_explicit(&context->invalidation_pages, memory_order_relaxed);
    return &context->stats;
}

static void add_stats(GxTranslateStats* destination, const GxTranslateStats* source)
{
#define GXT_ADD(field) destination->field += source->field
    GXT_ADD(streams);
    GXT_ADD(input_bytes);
    GXT_ADD(output_bytes);
    GXT_ADD(arraybase_emitted);
    GXT_ADD(array_source_bytes);
    GXT_ADD(display_lists_inlined);
    GXT_ADD(display_list_bytes);
    GXT_ADD(draws);
    GXT_ADD(texture_objects_emitted);
    GXT_ADD(texture_source_bytes);
    GXT_ADD(tluts_emitted);
    GXT_ADD(tlut_source_bytes);
    GXT_ADD(display_copies);
    GXT_ADD(texture_copies);
    GXT_ADD(resource_rebinds);
    GXT_ADD(staged_copies);
    GXT_ADD(staged_bytes);
    GXT_ADD(xf_unmodeled_writes);
    GXT_ADD(matrix_index_high);
    GXT_ADD(matrix_row_misaligned);
    if (source->max_pnmtx_row > destination->max_pnmtx_row)
        destination->max_pnmtx_row = source->max_pnmtx_row;
    if (source->matrix_row_misaligned_max > destination->matrix_row_misaligned_max)
        destination->matrix_row_misaligned_max = source->matrix_row_misaligned_max;
#undef GXT_ADD
}

u32 gx_translate_stream(GxTranslateContext* context,
                        const u8* input, u32 input_size,
                        u8* guest_ram, u32 guest_ram_size,
                        u8* output, u32 output_capacity)
{
    GxtState working;
    GxtSink sink;
    GxtRun run;
    if (!context || !input || !input_size || !output || !output_capacity) {
        if (context) context->last_error = GXT_ERROR_INVALID_ARGUMENT;
        return 0;
    }
    working = context->state;
    if (working.ram_base != guest_ram) {
        u32 i;
        working.ram_base = guest_ram;
        for (i = 0; i < GXT_ARRAY_COUNT; ++i) {
            working.arrays[i].emitted_this_frame = 0;
            working.arrays[i].metadata_valid = 0;
            working.arrays[i].force_rebind = 1;
        }
        for (i = 0; i < GXT_TEXTURE_COUNT; ++i) {
            if (working.tex_valid[i]) {
                working.tex_dirty[i] = 1;
                working.tex_revision_pending[i] = 1;
            }
            /* Old snapshots came from the previous RAM mapping. */
            working.tex_staged_phys[i] = 0xFFFFFFFFu;
        }
        for (i = 0; i < GXT_MAX_TLUTS; ++i) {
            if (working.tluts[i].valid) {
                /* TLUT snapshots model TMEM and stay valid across a RAM
                 * remap; only the emitted binding must be refreshed. */
                working.tluts[i].dirty = 1;
                working.tluts[i].revision_pending = 1;
                working.tluts[i].emitted_valid = 0;
            }
        }
    }
    sink.out = output;
    sink.cap = output_capacity;
    sink.len = 0;
    sink.overflow = 0;
    memset(&run, 0, sizeof(run));
    run.context = context;
    run.state = &working;
    run.sink = &sink;
    run.ram = guest_ram;
    run.ram_size = guest_ram_size;
    run.tex_scan_pending = 1;
    run.delta.streams = 1;
    run.delta.input_bytes = input_size;
    if (!walk_stream(&run, input, input_size, 0) || sink.overflow) {
        /* Snapshots taken during a failed run are referenced only by the
         * discarded working state; the replaced chain is dropped unfreed
         * because the persistent state still owns those blocks. */
        GxtStaged* block = run.fresh;
        while (block) {
            GxtStaged* next = block->next_fresh;
            free(block);
            block = next;
        }
        context->last_error = sink.overflow ? GXT_ERROR_OUTPUT_OVERFLOW : run.error;
        context->last_error_offset = run.error_offset;
        context->last_error_opcode = run.error_opcode;
        return 0;
    }
    run.delta.output_bytes = sink.len;
    context->state = working;
    if (run.replaced) {
        GxtStaged* tail = run.replaced;
        while (tail->next) tail = tail->next;
        tail->next = context->retired;
        context->retired = run.replaced;
    }
    add_stats(&context->stats, &run.delta);
    context->last_error = GXT_ERROR_NONE;
    context->last_error_offset = 0;
    context->last_error_opcode = 0;
    return sink.len;
}

long g_gxt_arraybase_emitted = 0;
long g_gxt_dl_inlined = 0;
long g_gxt_dl_bytes = 0;
long g_gxt_draws = 0;
long g_gxt_texobjs_emitted = 0;
long g_gxt_tluts_emitted = 0;
long g_gxt_display_copies = 0;
long g_gxt_texture_copies = 0;
long g_gxt_stop_op = -1;
long g_gxt_stop_at = -1;

static GxTranslateContext context;
static int initialized;
u32 gx_translate(const u8* input, u32 input_size,
                 u8* guest_ram, u32 guest_ram_size,
                 u8* output, u32 output_capacity)
{
    const GxTranslateStats* stats;
    u32 result;
    if (!initialized) {
        memset(&context, 0, sizeof(context));
        init_context(&context);
        initialized = 1;
    }
    gx_translate_frame_drained(&context);
    gx_translate_invalidate_all_ram(&context);
    result = gx_translate_stream(&context, input, input_size, guest_ram, guest_ram_size,
                                 output, output_capacity);
    stats = gx_translate_stats(&context);
    g_gxt_arraybase_emitted = (long)stats->arraybase_emitted;
    g_gxt_dl_inlined = (long)stats->display_lists_inlined;
    g_gxt_dl_bytes = (long)stats->display_list_bytes;
    g_gxt_draws = (long)stats->draws;
    g_gxt_texobjs_emitted = (long)stats->texture_objects_emitted;
    g_gxt_tluts_emitted = (long)stats->tluts_emitted;
    g_gxt_display_copies = (long)stats->display_copies;
    g_gxt_texture_copies = (long)stats->texture_copies;
    if (context.last_error == GXT_ERROR_NONE) {
        g_gxt_stop_op = -1;
        g_gxt_stop_at = -1;
    } else {
        g_gxt_stop_op = context.last_error_opcode;
        g_gxt_stop_at = (long)context.last_error_offset;
    }
    return result;
}

void gx_translate_release(void){if(initialized){free_all_staging(&context);free(context.page_epochs);context.page_epochs=NULL;initialized=0;}}
