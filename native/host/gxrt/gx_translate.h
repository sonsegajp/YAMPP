#ifndef MELEE_RECOMP_GX_TRANSLATE_H
#define MELEE_RECOMP_GX_TRANSLATE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Stateful adapter from a big-endian GameCube GX FIFO stream to Aurora's
 * source-port FIFO dialect.  One context must be kept for each emulated GX
 * command processor because CP/BP state persists between submissions.
 */
typedef struct GxTranslateContext GxTranslateContext;

typedef enum GxTranslateError {
    GXT_ERROR_NONE = 0,
    GXT_ERROR_INVALID_ARGUMENT,
    GXT_ERROR_OUTPUT_OVERFLOW,
    GXT_ERROR_TRUNCATED_COMMAND,
    GXT_ERROR_UNKNOWN_OPCODE,
    GXT_ERROR_INVALID_VERTEX_FORMAT,
    GXT_ERROR_MISSING_ARRAY,
    GXT_ERROR_DISPLAY_LIST_RANGE,
    GXT_ERROR_DISPLAY_LIST_DEPTH,
    GXT_ERROR_RESOURCE_RANGE,
    GXT_ERROR_OUT_OF_MEMORY
} GxTranslateError;

typedef struct GxTranslateStats {
    uint64_t streams;
    uint64_t input_bytes;
    uint64_t output_bytes;
    uint64_t arraybase_emitted;
    uint64_t array_source_bytes;
    uint64_t display_lists_inlined;
    uint64_t display_list_bytes;
    uint64_t draws;
    uint64_t texture_objects_emitted;
    uint64_t texture_source_bytes;
    uint64_t tluts_emitted;
    uint64_t tlut_source_bytes;
    uint64_t display_copies;
    uint64_t texture_copies;
    uint64_t ram_invalidations;
    uint64_t ram_invalidation_pages;
    uint64_t resource_rebinds;
    uint64_t staged_copies;
    uint64_t staged_bytes;
    /* Matrix-model diagnostics: Aurora models only the SDK's PNMTX0-9 /
     * TEXMTX0-9 slices of XF matrix RAM.  These count guest references
     * outside that model so a skinning-corruption report can be tied to a
     * concrete unsupported matrix row. */
    uint64_t xf_unmodeled_writes;  /* XF memory writes Aurora drops */
    uint64_t matrix_index_high;    /* CP/XF current-matrix rows > PNMTX9 (27) */
    uint64_t max_pnmtx_row;        /* highest per-vertex or current PNMTX row */
    uint64_t matrix_row_misaligned;     /* rows not divisible by 3: Aurora's
                                          * idx=row/3 palette applies a matrix
                                          * offset by 1-2 rows (shear/flatten) */
    uint64_t matrix_row_misaligned_max; /* highest such row seen */
} GxTranslateStats;

GxTranslateContext* gx_translate_create(void);
void gx_translate_destroy(GxTranslateContext* context);
void gx_translate_reset(GxTranslateContext* context);

/*
 * Call once at the same boundary as aurora_begin_frame().  This lets the
 * adapter reduce array upload extents again when a new frame references a
 * smaller maximum index than the previous frame.
 */
void gx_translate_begin_frame(GxTranslateContext* context);

/*
 * Emitted commands reference translator-owned snapshots of guest resource
 * bytes, never live guest RAM (the one exception is EFB-copy destinations,
 * which Aurora treats as opaque cache keys).  A snapshot that has been
 * superseded is kept alive until the host reports that Aurora has drained all
 * previously submitted FIFO work.  Call this after aurora_end_frame() (or any
 * other full FIFO drain) returns to release superseded snapshots.
 */
void gx_translate_frame_drained(GxTranslateContext* context);

/*
 * Notify the adapter after guest RAM is modified.  Physical addresses are
 * accepted with or without cached/uncached effective-address high bits.  Any
 * loaded texture or TLUT overlapping the range receives a new Aurora content
 * revision even when its pointer and GX registers did not change. These two
 * invalidation calls are safe to invoke concurrently with translation; all
 * other context operations belong to the single GX/FIFO worker.
 */
void gx_translate_invalidate_ram(GxTranslateContext* context, uint32_t physical_address,
                                 uint32_t size);
void gx_translate_invalidate_all_ram(GxTranslateContext* context);

/* Optional state seeding for integrations which attach after early CP setup. */
void gx_translate_set_array_base(GxTranslateContext* context, uint32_t cp_index,
                                 uint32_t physical_address);
void gx_translate_set_array_stride(GxTranslateContext* context, uint32_t cp_index,
                                   uint32_t stride);

/*
 * Returns the translated byte count.  Zero means failure; inspect
 * gx_translate_last_error().  Translation is transactional: a too-small output
 * buffer may be retried without corrupting persistent GX state or revisions.
 */
uint32_t gx_translate_stream(GxTranslateContext* context,
                             const uint8_t* input, uint32_t input_size,
                             uint8_t* guest_ram, uint32_t guest_ram_size,
                             uint8_t* output, uint32_t output_capacity);

GxTranslateError gx_translate_last_error(const GxTranslateContext* context);
uint32_t gx_translate_last_error_offset(const GxTranslateContext* context);
uint8_t gx_translate_last_error_opcode(const GxTranslateContext* context);
const GxTranslateStats* gx_translate_stats(const GxTranslateContext* context);

/* Exact GameCube tiled-source sizes used by the validator and unit tests. */
uint32_t gx_translate_texture_source_size(uint32_t format, uint32_t width,
                                          uint32_t height, uint32_t mip_count);
uint32_t gx_translate_tlut_source_size(uint32_t entries);

/*
 * Compatibility entry point used by the original gxrt frontend.  New runtimes
 * should own a GxTranslateContext and call gx_translate_stream() directly.
 */
uint32_t gx_translate(const uint8_t* input, uint32_t input_size,
                      uint8_t* guest_ram, uint32_t guest_ram_size,
                      uint8_t* output, uint32_t output_capacity);

#ifdef __cplusplus
}
#endif

#endif
