/* See pagedelta.h for what this is and why it exists.
 *
 * The bookkeeping is one bitmap per slot, `stale`, holding the pages where the
 * live memory may differ from that slot's stored image. It is a superset, never
 * a subset: over-copying costs time, under-copying would hand rollback a state
 * that never existed. Every operation begins by folding the hardware's record
 * of new writes into every slot, which is what keeps that true. */
#include "pagedelta.h"
#include "platform_compat.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PD_MAX_REGIONS 4

typedef struct { unsigned char* base; size_t size, pages, first; } PdRegion;

static PdRegion pd_regions[PD_MAX_REGIONS];
static unsigned pd_region_count;
static size_t pd_page = 4096;
static size_t pd_total_pages, pd_total_bytes, pd_words;
static unsigned char* pd_images;
static uint32_t* pd_stale;          /* slots x pd_words */
static uint32_t* pd_fresh;          /* pages written since the last fold */
static unsigned pd_slots;
static int pd_tracking;             /* the hardware is reporting writes */
static int pd_ready;                /* images exist and are being maintained */
static size_t pd_saved_bytes;
#ifdef _WIN32
static void** pd_watch;
static size_t pd_watch_cap;
#endif

static void bit_set(uint32_t* bits, size_t index) { bits[index >> 5] |= 1u << (index & 31u); }
static int bit_get(const uint32_t* bits, size_t index) { return (bits[index >> 5] >> (index & 31u)) & 1u; }
static void bits_or(uint32_t* into, const uint32_t* from, size_t words) {
  for (size_t i = 0; i < words; ++i) into[i] |= from[i];
}
static void bits_fill(uint32_t* bits, size_t words) { memset(bits, 0xFF, words * sizeof *bits); }
static void bits_clear(uint32_t* bits, size_t words) { memset(bits, 0, words * sizeof *bits); }

void* pd_alloc(size_t size) {
#ifdef _WIN32
  void* memory = VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, PAGE_READWRITE);
  if (memory) return memory;
  /* Write watching is a reservation flag, so losing it means losing the
   * allocation too. An ordinary one still runs, just without the tracking. */
  return VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
  return calloc(1, size);
#endif
}

void pd_free(void* base, size_t size) {
  (void)size;
  if (!base) return;
#ifdef _WIN32
  VirtualFree(base, 0, MEM_RELEASE);
#else
  free(base);
#endif
}

void pd_clear_regions(void) { pd_region_count = 0; }

int pd_add_region(void* base, size_t size) {
  if (!base || !size || pd_region_count >= PD_MAX_REGIONS) return 0;
  pd_regions[pd_region_count].base = (unsigned char*)base;
  pd_regions[pd_region_count].size = size;
  ++pd_region_count;
  return 1;
}

/* Does the hardware report writes to every registered region? A region that
 * was allocated without the flag answers with a failure here, and one region
 * without tracking disqualifies the whole set: a partially tracked image is
 * silently wrong, where an untracked one is merely slow. */
static int probe_tracking(void) {
#ifdef _WIN32
  for (unsigned r = 0; r < pd_region_count; ++r) {
    void* addresses[4];
    ULONG_PTR count = 4;
    ULONG granularity = 0;
    if (GetWriteWatch(0, pd_regions[r].base, pd_regions[r].size,
                      addresses, &count, &granularity) != 0) return 0;
    if (granularity && granularity != pd_page) return 0;
  }
  return 1;
#else
  return 0;
#endif
}

int pd_init(unsigned slots) {
  pd_shutdown();
  if (!slots || !pd_region_count) return 0;
#ifdef _WIN32
  { SYSTEM_INFO info; GetSystemInfo(&info); if (info.dwPageSize) pd_page = info.dwPageSize; }
#endif
  pd_total_pages = pd_total_bytes = 0;
  for (unsigned r = 0; r < pd_region_count; ++r) {
    pd_regions[r].first = pd_total_pages;
    pd_regions[r].pages = (pd_regions[r].size + pd_page - 1) / pd_page;
    pd_total_pages += pd_regions[r].pages;
    pd_total_bytes += pd_regions[r].size;
  }
  pd_words = (pd_total_pages + 31) / 32;
  pd_tracking = probe_tracking();
  if (!pd_tracking) return 0;

#ifdef _WIN32
  pd_watch_cap = 0;
  for (unsigned r = 0; r < pd_region_count; ++r)
    if (pd_regions[r].pages > pd_watch_cap) pd_watch_cap = pd_regions[r].pages;
  pd_watch = (void**)malloc(pd_watch_cap * sizeof *pd_watch);
  if (!pd_watch) { pd_shutdown(); return 0; }
#endif
  pd_images = (unsigned char*)malloc(pd_total_bytes * slots);
  pd_stale = (uint32_t*)malloc(pd_words * sizeof *pd_stale * slots);
  pd_fresh = (uint32_t*)malloc(pd_words * sizeof *pd_fresh);
  if (!pd_images || !pd_stale || !pd_fresh) { pd_shutdown(); return 0; }
  pd_slots = slots;
  /* No slot holds anything yet, so every page of every slot is stale. */
  for (unsigned s = 0; s < slots; ++s) bits_fill(pd_stale + (size_t)s * pd_words, pd_words);
  bits_clear(pd_fresh, pd_words);
  pd_ready = 1;
  pd_saved_bytes = 0;
  fprintf(stderr, "[rollback] tracked snapshots: %zu MB per slot in %u slots, %zu KB page map\n",
          pd_total_bytes >> 20, slots, (pd_words * sizeof *pd_stale * (slots + 1)) >> 10);
  return 1;
}

void pd_shutdown(void) {
  free(pd_images); pd_images = NULL;
  free(pd_stale); pd_stale = NULL;
  free(pd_fresh); pd_fresh = NULL;
#ifdef _WIN32
  free(pd_watch); pd_watch = NULL; pd_watch_cap = 0;
#endif
  pd_slots = 0; pd_ready = 0; pd_tracking = 0; pd_saved_bytes = 0;
}

int pd_active(void) { return pd_ready; }
size_t pd_last_saved_bytes(void) { return pd_saved_bytes; }

/* Collect the pages written since the previous call and clear the hardware's
 * record of them. Every slot's image predates those writes, so all of them
 * gain the pages. Called before any copying, and after any copying this module
 * performs into the live regions, so the record never runs ahead of the maps. */
static void fold_new_writes(void) {
  bits_clear(pd_fresh, pd_words);
#ifdef _WIN32
  for (unsigned r = 0; r < pd_region_count; ++r) {
    ULONG_PTR count = pd_watch_cap;
    ULONG granularity = 0;
    if (GetWriteWatch(WRITE_WATCH_FLAG_RESET, pd_regions[r].base, pd_regions[r].size,
                      pd_watch, &count, &granularity) != 0) {
      /* The record is gone; assume the whole region moved. Correct, and slow
       * only for the one frame it happens on. */
      for (size_t p = 0; p < pd_regions[r].pages; ++p) bit_set(pd_fresh, pd_regions[r].first + p);
      continue;
    }
    for (ULONG_PTR i = 0; i < count; ++i) {
      size_t offset = (size_t)((unsigned char*)pd_watch[i] - pd_regions[r].base);
      bit_set(pd_fresh, pd_regions[r].first + offset / pd_page);
    }
  }
#endif
  for (unsigned s = 0; s < pd_slots; ++s) bits_or(pd_stale + (size_t)s * pd_words, pd_fresh, pd_words);
}

/* Drop the hardware's record without folding it in. Used directly after this
 * module writes into the live regions itself, where the affected pages have
 * already been accounted for exactly. */
static void discard_new_writes(void) {
#ifdef _WIN32
  for (unsigned r = 0; r < pd_region_count; ++r) {
    ULONG_PTR count = pd_watch_cap;
    ULONG granularity = 0;
    GetWriteWatch(WRITE_WATCH_FLAG_RESET, pd_regions[r].base, pd_regions[r].size,
                  pd_watch, &count, &granularity);
  }
#endif
}

/* Copy every page named by `bits`, between the live regions and slot image
 * `image`. Consecutive pages are copied as one run, which is what most of
 * these are: a frame touches contiguous stretches of a heap, not scattered
 * single pages. */
static size_t copy_pages(unsigned char* image, const uint32_t* bits, int to_image) {
  size_t copied = 0;
  size_t image_offset = 0;
  for (unsigned r = 0; r < pd_region_count; ++r) {
    PdRegion* region = &pd_regions[r];
    unsigned char* slot = image + image_offset;
    for (size_t p = 0; p < region->pages; ) {
      if (!bit_get(bits, region->first + p)) { ++p; continue; }
      size_t run = 1;
      while (p + run < region->pages && bit_get(bits, region->first + p + run)) ++run;
      size_t at = p * pd_page;
      size_t length = run * pd_page;
      if (at + length > region->size) length = region->size - at;
      if (to_image) memcpy(slot + at, region->base + at, length);
      else memcpy(region->base + at, slot + at, length);
      copied += length;
      p += run;
    }
    image_offset += region->size;
  }
  return copied;
}

void pd_save(unsigned slot) {
  if (!pd_ready || slot >= pd_slots) return;
  fold_new_writes();
  uint32_t* bits = pd_stale + (size_t)slot * pd_words;
  pd_saved_bytes = copy_pages(pd_images + (size_t)slot * pd_total_bytes, bits, 1);
  /* The image now matches the live regions everywhere. */
  bits_clear(bits, pd_words);
}

void pd_restore(unsigned slot) {
  if (!pd_ready || slot >= pd_slots) return;
  fold_new_writes();
  uint32_t* bits = pd_stale + (size_t)slot * pd_words;
  copy_pages(pd_images + (size_t)slot * pd_total_bytes, bits, 0);
  /* The live regions just changed on exactly those pages, so every other
   * slot's image may now differ on them. Recorded before this slot's map is
   * cleared, because this slot's map is the list of pages that changed. */
  for (unsigned s = 0; s < pd_slots; ++s)
    if (s != slot) bits_or(pd_stale + (size_t)s * pd_words, bits, pd_words);
  bits_clear(bits, pd_words);
  /* Those writes are already in the maps above; letting the hardware report
   * them again would only make the next save copy them a second time. */
  discard_new_writes();
}

void pd_mark(const void* address, size_t length) {
  if (!pd_ready || !address || !length) return;
  const unsigned char* at = (const unsigned char*)address;
  for (unsigned r = 0; r < pd_region_count; ++r) {
    PdRegion* region = &pd_regions[r];
    const unsigned char* first = at > region->base ? at : region->base;
    const unsigned char* last = at + length < region->base + region->size
                              ? at + length : region->base + region->size;
    if (first >= last) continue;
    size_t begin = (size_t)(first - region->base) / pd_page;
    size_t end = (size_t)(last - 1 - region->base) / pd_page;
    for (size_t p = begin; p <= end; ++p)
      for (unsigned s = 0; s < pd_slots; ++s)
        bit_set(pd_stale + (size_t)s * pd_words, region->first + p);
  }
}
