// SPDX-License-Identifier: GPL-3.0-or-later
#include "gxruntime/gap_report.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Fixed-capacity store: gaps are, by definition, the rare unhandled cases; a
// clean run holds zero and even a badly-broken run holds a bounded taxonomy of
// distinct (lane, kind, key) tuples. Overflow past the cap is itself recorded
// as a single synthetic entry rather than dropped silently.
#define GAP_MAX_ENTRIES 1024
#define GAP_STR 64

typedef struct {
    char lane[GAP_STR];
    char kind[GAP_STR];
    char key[GAP_STR];
    char notes[128];
    uint32_t first_pc;
    uint64_t count;
} GapEntry;

static GapEntry g_entries[GAP_MAX_ENTRIES];
static size_t g_count = 0;
static uint64_t g_overflow = 0;

static void copy_str(char* dst, size_t cap, const char* src) {
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    size_t n = strlen(src);
    if (n >= cap)
        n = cap - 1u;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

const char* gap_report_class_name(GapReportClass c) {
    switch (c) {
    case GAP_CLASS_POLICY:
        return "policy";
    case GAP_CLASS_DIAGNOSTIC:
        return "diagnostic";
    case GAP_CLASS_GAP:
    default:
        return "gap";
    }
}

// Documented diagnostic GapCounter kinds — not defects. Expand only with a
// cited KNOWLEDGE/handoff rationale; do not hide real capability gaps here.
//
// gxcore/normals_ignored: has_normal && !lit_valid (legitimate unlit path).
//   KNOWLEDGE/aurora-runtime.md; handoff normals_ignored_brief_impl-c.
static int is_documented_diagnostic(const char* lane, const char* kind) {
    if (lane == NULL || kind == NULL)
        return 0;
    if (strcmp(lane, "gxcore") == 0 && strcmp(kind, "normals_ignored") == 0)
        return 1;
    return 0;
}

static int has_policy_tag(const char* s) {
    return s != NULL && strstr(s, "policy/") != NULL;
}

GapReportClass gap_report_classify(const char* lane, const char* kind,
                                  const char* key, const char* notes) {
    (void)key;
    // kind prefix "policy/..." or notes carrying "policy/<name>"
    if (has_policy_tag(kind) || has_policy_tag(notes))
        return GAP_CLASS_POLICY;
    if (is_documented_diagnostic(lane, kind))
        return GAP_CLASS_DIAGNOSTIC;
    return GAP_CLASS_GAP;
}

void gap_report_reset(void) {
    g_count = 0;
    g_overflow = 0;
}

void gap_report_add(const char* lane, const char* kind, const char* key,
                    uint32_t first_pc, const char* notes, uint64_t occurrences) {
    if (lane == NULL || kind == NULL || key == NULL || occurrences == 0u)
        return;
    for (size_t i = 0; i < g_count; ++i) {
        GapEntry* e = &g_entries[i];
        if (strncmp(e->lane, lane, GAP_STR) == 0 &&
            strncmp(e->kind, kind, GAP_STR) == 0 &&
            strncmp(e->key, key, GAP_STR) == 0) {
            e->count += occurrences;
            return;
        }
    }
    if (g_count >= GAP_MAX_ENTRIES) {
        g_overflow += 1u;
        return;
    }
    GapEntry* e = &g_entries[g_count++];
    copy_str(e->lane, GAP_STR, lane);
    copy_str(e->kind, GAP_STR, kind);
    copy_str(e->key, GAP_STR, key);
    copy_str(e->notes, sizeof e->notes, notes);
    e->first_pc = first_pc;
    e->count = occurrences;
}

void gap_report_note(const char* lane, const char* kind, const char* key,
                     uint32_t first_pc, const char* notes) {
    gap_report_add(lane, kind, key, first_pc, notes, 1u);
}

size_t gap_report_size(void) { return g_count; }

uint64_t gap_report_total(void) {
    uint64_t total = 0;
    for (size_t i = 0; i < g_count; ++i)
        total += g_entries[i].count;
    return total + g_overflow;
}

static GapReportClass entry_class(const GapEntry* e) {
    return gap_report_classify(e->lane, e->kind, e->key, e->notes);
}

size_t gap_report_acceptance_size(void) {
    size_t n = 0;
    for (size_t i = 0; i < g_count; ++i) {
        if (entry_class(&g_entries[i]) == GAP_CLASS_GAP)
            n += 1u;
    }
    // Overflow is always a real gap (store integrity).
    if (g_overflow != 0u)
        n += 1u;
    return n;
}

uint64_t gap_report_acceptance_total(void) {
    uint64_t total = 0;
    for (size_t i = 0; i < g_count; ++i) {
        if (entry_class(&g_entries[i]) == GAP_CLASS_GAP)
            total += g_entries[i].count;
    }
    return total + g_overflow;
}

static int cmp_entries(const void* a, const void* b) {
    const GapEntry* x = (const GapEntry*)a;
    const GapEntry* y = (const GapEntry*)b;
    int c = strncmp(x->lane, y->lane, GAP_STR);
    if (c != 0)
        return c;
    c = strncmp(x->kind, y->kind, GAP_STR);
    if (c != 0)
        return c;
    return strncmp(x->key, y->key, GAP_STR);
}

// Minimal JSON string escaping (quotes and backslashes; the fields are short
// ASCII register/opcode/symbol names, no control chars expected).
static void write_escaped(FILE* f, const char* s) {
    for (; *s != '\0'; ++s) {
        if (*s == '"' || *s == '\\')
            fputc('\\', f);
        fputc(*s, f);
    }
}

static void write_entry(FILE* f, const GapEntry* e, int first) {
    char pcbuf[16];
    if (e->first_pc != 0u)
        snprintf(pcbuf, sizeof pcbuf, "0x%08X", e->first_pc);
    else
        pcbuf[0] = '\0';
    const char* cls = gap_report_class_name(entry_class(e));
    fprintf(f, "%s\n {\"lane\":\"", first ? "" : ",");
    write_escaped(f, e->lane);
    fputs("\",\"kind\":\"", f);
    write_escaped(f, e->kind);
    fputs("\",\"key\":\"", f);
    write_escaped(f, e->key);
    fprintf(f, "\",\"count\":%llu,\"first_pc\":\"%s\",\"notes\":\"",
            (unsigned long long)e->count, pcbuf);
    write_escaped(f, e->notes);
    fprintf(f, "\",\"class\":\"%s\"}", cls);
}

int gap_report_write_json_ex(const char* path, int acceptance_only) {
    // Sort a copy so serialisation order is stable without disturbing the store.
    static GapEntry sorted[GAP_MAX_ENTRIES];
    memcpy(sorted, g_entries, g_count * sizeof(GapEntry));
    qsort(sorted, g_count, sizeof(GapEntry), cmp_entries);

    FILE* f = (path == NULL || strcmp(path, "-") == 0) ? stdout : fopen(path, "w");
    if (f == NULL)
        return 1;
    fputs("[", f);
    int first = 1;
    for (size_t i = 0; i < g_count; ++i) {
        const GapEntry* e = &sorted[i];
        if (acceptance_only && entry_class(e) != GAP_CLASS_GAP)
            continue;
        write_entry(f, e, first);
        first = 0;
    }
    // Overflow is always acceptance-relevant.
    if (g_overflow != 0u) {
        fprintf(f, "%s\n {\"lane\":\"gap_report\",\"kind\":\"overflow\","
                   "\"key\":\"entries\",\"count\":%llu,\"first_pc\":\"\","
                   "\"notes\":\"distinct-tuple cap %d exceeded\","
                   "\"class\":\"gap\"}",
                first ? "" : ",", (unsigned long long)g_overflow, GAP_MAX_ENTRIES);
        first = 0;
    }
    // Empty store/acceptance → "[]\n"; non-empty → "[\n {...}\n]\n".
    if (first)
        fputs("]\n", f);
    else
        fputs("\n]\n", f);
    if (f != stdout)
        fclose(f);
    return 0;
}

int gap_report_write_json(const char* path) {
    return gap_report_write_json_ex(path, 0);
}
