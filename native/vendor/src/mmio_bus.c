// SPDX-License-Identifier: GPL-3.0-or-later
#include "gxruntime/mmio_bus.h"

#include "gxruntime/gap_report.h"

#include <stdio.h>
#include <string.h>

// Classify a guest MMIO effective address into its GameCube hardware block, for
// the structured gap report (C2). Mirrors the offset map in the demand
// certificate's MMIO lane so live tripwires diff against the static poke list.
static const char* mmio_block_name(u32 ea) {
    // Uncached EFB window (physical 0x08000000..0x0BFFFFFF) — real HW, not
    // off-map. Claimed by dol_efb_access when registered; this label only
    // appears if a host forgets to register the region.
    if (ea >= 0xC8000000u && ea < 0xCC000000u)
        return "EFB";
    if (ea < 0xCC000000u || ea >= 0xCC010000u)
        return "off-map";
    u32 off = ea - 0xCC000000u;
    if (off < 0x1000u) return "CP";
    if (off < 0x2000u) return "PE";
    if (off < 0x3000u) return "VI";
    if (off < 0x4000u) return "PI";
    if (off < 0x5000u) return "MI";
    if (off < 0x6000u) return "DSP/AI";
    if (off < 0x6400u) return "DI";
    if (off < 0x6800u) return "SI";
    if (off < 0x6C00u) return "EXI";
    if (off < 0x7000u) return "AI-stream";
    return "GX-FIFO/WPAR";
}

static void note_unmapped_mmio(CPUState* cpu, const char* kind, u32 ea) {
    char key[16];
    snprintf(key, sizeof key, "0x%08X", ea);
    gap_report_note("mmio", kind, key, cpu != NULL ? cpu->pc : 0u,
                    mmio_block_name(ea));
}

static bool range_valid(u32 base, u32 size) {
    return size != 0u && base <= UINT32_MAX - (size - 1u);
}

static bool range_contains(u32 base, u32 size, u32 ea, u8 access_size) {
    if (!range_valid(base, size) || access_size == 0u)
        return false;
    if (ea < base)
        return false;
    const u32 off = ea - base;
    return off < size && (u32)access_size <= size - off;
}

void dol_mmio_bus_init(DolMmioBus* bus) {
    if (bus != NULL)
        memset(bus, 0, sizeof(*bus));
}

bool dol_mmio_bus_register(DolMmioBus* bus, u32 base, u32 size,
                           DolMmioReadFn read, DolMmioWriteFn write,
                           void* user) {
    if (bus == NULL || !range_valid(base, size) ||
        (read == NULL && write == NULL))
        return false;

    for (u32 i = 0; i < DOL_MMIO_BUS_MAX_REGIONS; i++) {
        DolMmioRegion* region = &bus->regions[i];
        if (!region->active) {
            region->base = base;
            region->size = size;
            region->read = read;
            region->write = write;
            region->user = user;
            region->active = true;
            return true;
        }
    }
    return false;
}

bool dol_mmio_bus_contains(const DolMmioBus* bus, u32 ea) {
    if (bus == NULL)
        return false;
    for (u32 i = 0; i < DOL_MMIO_BUS_MAX_REGIONS; i++) {
        const DolMmioRegion* region = &bus->regions[i];
        if (region->active && range_contains(region->base, region->size, ea, 1))
            return true;
    }
    return false;
}

bool dol_mmio_bus_read(const DolMmioBus* bus, CPUState* cpu, u32 ea, u8 size,
                       u64* value) {
    if (bus == NULL || size == 0u)
        return false;
    for (u32 i = 0; i < DOL_MMIO_BUS_MAX_REGIONS; i++) {
        const DolMmioRegion* region = &bus->regions[i];
        if (!region->active || region->read == NULL ||
            !range_contains(region->base, region->size, ea, size))
            continue;
        if (region->read(region->user, cpu, ea, size, value))
            return true;
    }
    note_unmapped_mmio(cpu, "unknown-read", ea);
    return false;
}

bool dol_mmio_bus_write(const DolMmioBus* bus, CPUState* cpu, u32 ea, u8 size,
                        u64 value) {
    if (bus == NULL || size == 0u)
        return false;
    for (u32 i = 0; i < DOL_MMIO_BUS_MAX_REGIONS; i++) {
        const DolMmioRegion* region = &bus->regions[i];
        if (!region->active || region->write == NULL ||
            !range_contains(region->base, region->size, ea, size))
            continue;
        if (region->write(region->user, cpu, ea, size, value))
            return true;
    }
    note_unmapped_mmio(cpu, "unknown-write", ea);
    return false;
}
