// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef GXRUNTIME_GAP_REPORT_H
#define GXRUNTIME_GAP_REPORT_H

// C2 tripwire totality: ONE structured gap report per run, every lane.
//
// Every place the runtime silently does-not-implement or does-not-recognise
// something calls gap_report_note(); at the end of a run the driver writes the
// aggregated report as JSON. A clean, fully-supported run emits an EMPTY report
// (zero entries) — that empty file is the machine-checkable meaning of "nothing
// failed silently", and it is diffable against the per-game static certificate.
//
// Empty-report *acceptance* (C2) is stricter than the raw store: it ignores
// documented non-defects so residual ranking is not dominated by known noise.
// Classification is machine-readable on every JSON row via "class":
//   "gap"         — real residual (counts toward acceptance)
//   "policy"      — intentional host-owned residual (kind or notes "policy/*")
//   "diagnostic"  — documented non-defect GapCounter (e.g. normals_ignored)
// gap_report_acceptance_size/total() count only class=="gap". Full reports
// still list every entry (no silent drops).
//
// Schema (one JSON object per distinct (lane, kind, key) tuple):
//   { "lane": str,      // which subsystem: mmio | exi | vi | dsp | hle |
//                       //   frontend | gxcore | dl | efb | ...
//     "kind": str,      // the category of miss within the lane.
//                       // Kinds beginning with "policy/" are intentional
//                       // host-owned residuals (e.g. policy/dsp_host).
//     "key":  str,      // the specific thing (register addr, opcode, name, …)
//     "count": u64,     // how many times it fired
//     "first_pc": str,  // guest PC at the first occurrence ("0x........" or "")
//     "notes": str,     // free-form context; may carry "policy/<name>"
//     "class": str }    // "gap" | "policy" | "diagnostic" (acceptance filter)
//
// Entries aggregate by (lane, kind, key): the first occurrence records first_pc
// and notes; later hits only bump count. The store is process-global and single
// -threaded by contract (these events fire on the guest-exec / decode thread).
//
// C linkage so the C runtime (GXRuntime/src, StrikersRecomp host) and the C++
// tools (frontend replay) share one report.

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Entry classification for empty-report acceptance (see file header).
typedef enum GapReportClass {
    GAP_CLASS_GAP = 0,
    GAP_CLASS_POLICY = 1,
    GAP_CLASS_DIAGNOSTIC = 2,
} GapReportClass;

// Stable string for GapReportClass ("gap" | "policy" | "diagnostic").
const char* gap_report_class_name(GapReportClass c);

// Classify one (lane,kind,key,notes) tuple. Pure function of the strings;
// does not touch the store. Used by write_json and acceptance counters.
GapReportClass gap_report_classify(const char* lane, const char* kind,
                                  const char* key, const char* notes);

// Drop all accumulated entries. Call at the start of a run/replay.
void gap_report_reset(void);

// Record one occurrence. lane/kind/key must be non-NULL, stable short strings;
// notes may be NULL. first_pc==0 is serialised as "" (no PC context). Strings
// longer than the internal buffers are truncated, never overflowed.
void gap_report_note(const char* lane, const char* kind, const char* key,
                     uint32_t first_pc, const char* notes);

// Like gap_report_note but adds `occurrences` to the count in one call — for
// lanes that already hold an aggregate tally (e.g. gxcore GapCounters, where the
// counter value IS the fire-count). occurrences==0 records nothing.
void gap_report_add(const char* lane, const char* kind, const char* key,
                    uint32_t first_pc, const char* notes, uint64_t occurrences);

// Number of distinct (lane, kind, key) entries currently held (all classes).
size_t gap_report_size(void);

// Sum of all entry counts (total occurrences across every lane, all classes).
uint64_t gap_report_total(void);

// Acceptance residual: distinct entries / total occurrences with class=="gap".
// Empty acceptance_size means empty-report PASS after policy+diagnostic filter.
size_t gap_report_acceptance_size(void);
uint64_t gap_report_acceptance_total(void);

// Serialise to <path> as a JSON array (stable-sorted by lane, kind, key).
// Every object includes "class". Returns 0 on success, non-zero on I/O error.
// An empty store writes "[]".
int gap_report_write_json(const char* path);

// Like gap_report_write_json but if acceptance_only!=0 omits policy/diagnostic
// rows (still tags remaining rows with "class":"gap"). Full inventory is always
// available via gap_report_write_json.
int gap_report_write_json_ex(const char* path, int acceptance_only);

#ifdef __cplusplus
}
#endif

#endif /* GXRUNTIME_GAP_REPORT_H */
