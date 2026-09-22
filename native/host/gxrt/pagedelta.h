/* Incremental snapshots of the large emulated memories.
 *
 * Rollback needs a complete image of guest RAM and ARAM at every simulated
 * frame, because any of them may be the one a late input rewinds to. Copying
 * those images whole costs 40 MB per saved frame and another 40 MB per frame
 * replayed after a correction -- several gigabytes a second of pure memcpy,
 * which is felt as a stutter and which evicts the entire cache the emulation
 * was about to use.
 *
 * Almost none of those 40 MB change between two frames. This module keeps the
 * per-slot images whole, so a restore is still a straight copy of a complete
 * state, but copies only the pages that actually differ. The hardware already
 * records which pages were written: the memories are reserved with
 * MEM_WRITE_WATCH, and GetWriteWatch reports and clears that record. Nothing
 * is added to the write path itself.
 *
 * Where write watching is unavailable the module reports that, and callers
 * copy whole images exactly as before.
 *
 * The one thing hardware watching does not see is a write the kernel performs
 * on the process's behalf -- a file read landing straight in guest RAM. Those
 * few callers say so with pd_mark. */
#ifndef YAMPP_PAGEDELTA_H
#define YAMPP_PAGEDELTA_H
#include <stddef.h>

/* Reserve `size` bytes whose writes can be tracked, or fall back to an
 * ordinary allocation. Used for guest RAM and ARAM; pd_free releases it. */
void* pd_alloc(size_t size);
void pd_free(void* base, size_t size);

/* Describe the memories to snapshot. Called before pd_init; the regions must
 * stay at the same address and size for the life of the snapshot set. */
void pd_clear_regions(void);
int pd_add_region(void* base, size_t size);

/* Allocate `slots` complete images of every registered region. Returns 0 if
 * the images do not fit, in which case nothing is tracked and the caller must
 * snapshot those memories itself. */
int pd_init(unsigned slots);
void pd_shutdown(void);

/* Whether pd_save/pd_restore are carrying the registered regions. False means
 * the caller owns them. */
int pd_active(void);

/* Make slot `slot` a complete image of the regions as they are now. */
void pd_save(unsigned slot);
/* Put the regions back exactly as slot `slot` recorded them. The slot must
 * have been saved; the caller establishes that from its own frame bookkeeping. */
void pd_restore(unsigned slot);

/* Bytes actually copied by the last pd_save, for the diagnostics that report
 * what a connection is costing. */
size_t pd_last_saved_bytes(void);

/* A write this process made without going through its own address space --
 * a file read whose destination was inside a tracked region. Writes made by
 * ordinary stores, including from the recompiled guest and from the m-ex
 * engine, are recorded by the hardware and must not be reported here. */
void pd_mark(const void* address, size_t length);

#endif
