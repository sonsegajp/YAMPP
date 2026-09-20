#!/usr/bin/env python3
"""
Addresses of recompiled functions we replace with hand-written HLE (runtime/hle_*.c).
The code generator SKIPS emitting a body for these; the HLE file provides func_<ADDR>.
Forward declarations and the dispatch table still reference them, so links resolve to
the HLE version.
"""
OVERRIDES = {
    # --- OS thread scheduler (host-thread-backed cooperative scheduler, hle_thread.c) ---
    0x8030803C,  # OSCreateThread
    0x803086A4,  # OSResumeThread
    0x8030892C,  # OSSuspendThread
    0x80308A9C,  # OSSleepThread
    0x80308B88,  # OSWakeupThread
    0x80308224,  # OSExitThread
    # --- diagnostics (hle_os.c) ---
    0x8032E2CC,  # __write_console — capture console/OSReport/panic output to stderr
    0x80006950,  # OSReport — print format string (diagnostics)
    0x80006C4C,  # OSPanic  — print file:line + message (assertion diagnostics)
    # --- input (hle_os.c) ---
    0x80315A20,  # PADRead — report a neutral controller (SI/PAD not modeled)
    # --- OS alarms (hle_os.c): host-side list + fake-time firing. The recompiled path can't
    # work: alarms fire from the decrementer exception we don't model, so set-without-fire
    # corrupts/loops the alarm queue (InsertAlarm infinite walk). ---
    0x803021A4,  # OSSetAlarm
    0x8030220C,  # OSSetPeriodicAlarm
    0x80302288,  # OSCancelAlarm
    # (reverted 0x802B5E0C JKRAramPiece::sync override — it corrupted a REL / broke the logo scene;
    #  restoring the real body to re-baseline, then fix the ARAM completion routing properly.)
}
