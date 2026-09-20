"""Game config: Super Smash Bros. Melee (GALE01, NTSC 1.02).

Symbols and TU splits come from the doldecomp/melee project's decomp-toolkit config,
which gives exact function extents for 19,829 functions -- so we never have to guess
function boundaries in text1. Only text0 (the CodeWarrior bootstrap, which the decomp
config also covers) and unmapped call targets need discovery.
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]

NAME     = "Super Smash Bros. Melee"
ID       = "GALE01"
DECOMP   = str(ROOT / "upstream/melee")
DOL      = str(ROOT / "data/GALE01/sys/main.dol")
SYMBOLS  = DECOMP + "/config/GALE01/symbols.txt"
SPLITS   = DECOMP + "/config/GALE01/splits.txt"
SYMFMT   = "dtk"
DOL_SHA1 = "08e0bf20134dfcb260699671004527b2d6bb1a45"

# Recompiled functions replaced by hand-written HLE (host/hle_gale01.c).
# Each entry is a place the recompiled body cannot work as-is, with the reason.
OVERRIDES = {
    # VIWaitForRetrace: sleeps on the retrace queue until a VI interrupt bumps
    # the retrace count. We have neither async interrupts nor stack-switching
    # threads yet, so the HLE drives __VIRetraceHandler synchronously instead --
    # which is also what commits the pending VI_TFBL write, so it is what makes
    # any picture possible.
    # VIWaitForRetrace: removal was TRIED and REVERTED. Real interrupt delivery
    # now works (delivered=239/240 frames), so this looked redundant -- but
    # removing it collapsed boot from 97M guest calls to 942, distinct 597 -> 123,
    # and zero disc reads. The real function blocks on a retrace count that our
    # delivery path does not advance the way this HLE does; delivery working is
    # necessary but not sufficient to retire it.
    #
    # Recorded rather than retried: "delivery works, therefore the retrace
    # override is redundant" is a reasonable inference and a wrong one.
    0x8034F314,

    # Guest thread layer -> host-thread scheduler (host/hle_thread.c).
    # Recompiled code runs on the host call stack, so OSLoadContext cannot resume
    # another thread -- its `rfi` is a plain return and SelectThread falls back to
    # its caller. Result: threads get marked runnable and never execute. Each of
    # these hands off between host threads instead.
    0x8034B25C,   # OSCreateThread
    0x8034B61C,   # OSResumeThread
    0x8034B8A4,   # OSSuspendThread
    0x8034BA14,   # OSSleepThread
    0x8034BB00,   # OSWakeupThread
    0x8034B37C,   # OSExitThread
    0x8034B22C,   # __OSReschedule
    0x80345174,   # OSLoadContext -- restored a garbage r1 from an unpopulated
                  # OSContext; the host scheduler owns context switching

    # PADRead: the recompiled body polls an SI handshake we do not model, so its
    # err field never clears. db_GetGameLaunchButtonState loops on that field,
    # which is what actually pinned boot. Report attached, neutral controllers.
    0x8034DA00,   # PADRead -- override RESTORED. A/B measured: removing it
                  # regresses boot from 20.87M guest calls to 1,553 and from 204
                  # disc reads to 0. The recompiled body polls an SI handshake we
                  # do not model, so its err field never clears and
                  # db_GetGameLaunchButtonState pins boot -- exactly what the
                  # original note said. The HLE must instead feed HSD's raw queue.

    # __write_console: the stdio sink OSReport/__assert/OSPanic funnel through.
    # Routes the game's own diagnostics to stderr so an assertion tells us what
    # it is unhappy about instead of vanishing.
    0x80325F20,

    # __ARChecksize: probes ARAM size by marker write/read-back across address
    # aliases. We allocate the ARAM, so we define its size (16 MB) rather than
    # let it discover it from absent-memory aliasing we do not model.
    0x80351010,

    # DSP handshake only. The earlier, wider cut at lbAudioAx_8002838C skipped
    # the whole audio subsystem -- including the synth bank-table setup that
    # synth.c later depends on, which showed up as
    #   "invalid bankID = 0; filename = /audio/us/main.ssm"
    # Letting AX/synth init run for real and stubbing just the DSP conversation
    # keeps those tables populated. Costs audio output, not audio *state*.
    0x80336820,   # __DSP_boot_task -- uploads AX microcode, then talks to it
    # ARQPostRequest: the ARAM request queue. Its head at r13-16856 is corrupted
    # by a wild write at call 3761 -- not by the poster (both posts arrive with a
    # valid request and a valid callback), not by DI and not by the ARAM engine;
    # all four were excluded by direct logging. The poster then reads a non-null
    # head, takes the `tail->next = req` path with tail == req, self-links the
    # node, and the queue never drains, so the audio load never completes.
    #
    # The queue is SDK code, which is the layer this project HLEs anyway. Doing
    # the transfer and invoking the callback synchronously removes the queue, the
    # in-flight marker and the interrupt round-trip from the boot path at once.
    # The wild write is still worth finding -- the low-memory and ABI guards stay
    # armed for it -- but it is not on the path to pixels.
    # VIGetRetraceCount: the HSD video layer polls this in a tight spin without
    # ever calling VIWaitForRetrace, so nothing advanced the count and boot
    # stopped at the first frame. The HLE advances it on host vblanks and runs
    # __VIRetraceHandler, which is what commits the pending VI_TFBL write.
    0x8035017C,   # VIGetRetraceCount

    # lb_800195D0: the guest's own poll while waiting for the SFX load. Faithful
    # reimplementation (it just calls lb_800192A8(0x8002955C) then lb_8001CC84),
    # overridden solely to drain deferred ARQ completions from a point outside
    # any DVD/ARAM completion -- the property that keeps DevCom from re-entering
    # itself and double-advancing hsd_SynthSFXBank[0].
    # 0x800195D0,  # lb_800195D0 -- override REMOVED for the GXRuntime port.
    #   It existed to drain OUR deferred ARAM/DI completions from a point
    #   outside any handler -- machinery that does not exist here, since
    #   GXRuntime services those devices and the ARQ path is HLE'd
    #   synchronously. Its drain calls are inert and its comments describe the
    #   old host's arm/fire measurements as if they still applied.
    #
    #   Boot never reaches HSD_Init in this build while the old host did, and
    #   the divergence is in exactly this stretch. An override that suppresses
    #   real guest code is the obvious way to lose a boot step, so the honest
    #   test is to let the real function run.

    # 0x80352114,  # ARQPostRequest -- override REMOVED. A/B measured: with and
    #   without it, boot reaches the same point (4 disc reads / 52,096 bytes) and
    #   HSD_DevComDVDARAMEndCallback is reached in neither. The real SDK ARQ ISR
    #   path is the more faithful of two equivalents, so it stays.
    #   it short-circuits DevCom's ARAM-side dispatch, so a staging entry never
    #   enters the ARAM queue. The deadlock that motivated it may have been a
    #   symptom of the completion-ordering bug since fixed.
    0x803360D4,   # DSPAddTask      -- queues a task on a core we do not run
    0x803360CC,   # DSPCheckInit    -- report the DSP initialised

    # __OSInitAudioSystem: polls the DSP control/status register at 0xCC00500A
    # waiting on a handshake that never completes -- measured at 85,403,830
    # reads of a constant 0x8A5 over 120 frames, with boot frozen at 368 guest
    # calls. This is not a gap in GXRuntime: its audio_dma.c states plainly that
    # the host-audio path (MusyX HLE) does not run DSP-LLE and that mailbox
    # traffic is expected residual. A runtime that deliberately omits DSP-LLE
    # cannot answer an LLE handshake, so the SDK layer that waits on one belongs
    # in frontend HLE -- which is exactly where DSPCheckInit and DSPAddTask
    # already sit. This is the lower SDK layer they did not cover.
    0x80344534,   # __OSInitAudioSystem

    # DVDReadAsyncPrio: the SDK's async read. Measured, this configuration can
    # signal completion in neither of the two ways the SDK supports: the guest
    # never sleeps (OSSleepThread=0 across a full run) so there is nothing to
    # wake, and __DVDInterruptHandler cannot be entered from an arbitrary point
    # because it goes through the scheduler and wedges in OSSetCurrentContext.
    # Result: one read issued, never acknowledged, and the HSD structure at
    # 0x80433318 stays zero while func_8001CC84 polls it 161M times.
    #
    # So do the read synchronously and invoke the caller's callback directly --
    # exactly how VIWaitForRetrace HLEs the wait rather than driving the
    # interrupt. Same pattern, same reason, and it needs neither a sleeper nor a
    # safe interrupt entry point.
    # 0x80337CF8,  # DVDReadAsyncPrio -- override REMOVED, boundary moved down.
    #   HLE-ing here delivered correct bytes and fired the callback (3 -> 1,188
    #   reads) but skipped every piece of SDK bookkeeping the layer maintains:
    #   DVDGetDriveStatus polls three small-data globals (0x804D72B0/A8/98)
    #   that stayed zero and were never written once across a full run, while
    #   DVDInit/__DVDFSInit had both run correctly. Replicating that bookkeeping
    #   from outside would be guesswork about a state machine we do not own.
    #
    #   Instead, HLE the *lowest* layer -- DVDLowRead, the hardware transfer --
    #   and let the real DVDReadAsyncPrio, command queue and completion path run
    #   as guest code. The SDK then maintains its own state by construction, and
    #   we supply only the bytes, which is the one thing the runtime cannot.
    #   RESULT of moving the boundary down: it does not work here, and the
    #   reason is a constraint already measured elsewhere in this port. With the
    #   real DVDReadAsyncPrio running, the SDK issues exactly ONE request and
    #   never another (ReadAsyncPrio=1, ARQPostRequest=0) -- its async state
    #   machine must advance between requests, and that needs an interrupt-driven
    #   completion this frontend cannot deliver (OSSleepThread=0, so no sleeper
    #   to wake; entering a handler from an arbitrary point wedges in
    #   OSSetCurrentContext).
    #
    #   So the higher boundary worked BECAUSE it bypasses that machinery. The
    #   principle "HLE at the lowest layer only the host can service" is wrong
    #   for this runtime; the measured rule is "HLE at whatever layer avoids the
    #   mechanisms the frontend cannot implement".
    #
    #   Reverted to the arrangement that streams: 1,188 reads / 1,187 ARAM
    #   transfers vs 1 read. DVDLowRead's HLE stays in hle_gale01.c, unused, as
    #   the record of why.
    # 0x80337CF8,  # DVDReadAsyncPrio -- override REMOVED, second attempt.
    #   It was added because this frontend could not deliver an async completion.
    #   That is no longer true: real interrupt delivery now works (239 in 240
    #   frames, gmMain 0 -> 239). With the HLE in place, di_execute never runs
    #   and DI is never raised, so the DI half of delivery has nothing to carry.
    #
    #   The 15.5M-call spin has exactly one driver, lbDvd_800189EC, waiting on a
    #   DVD completion. Letting the real path run should produce a real DI
    #   interrupt that delivery can now carry to the real handler.
    #
    #   Pass condition, set before running: DI reads > 0 AND guest calls stay in
    #   the 100M range. The VIWaitForRetrace removal failed exactly here --
    #   delivery working was necessary but not sufficient -- so a collapse to a
    #   few thousand calls means revert, as that one did.

    # DVDGetDriveStatus: reports the drive state from three small-data globals
    # (0x804D72B0/A8/98) that the SDK's async completion path maintains. That
    # path is bypassed by the DVDReadAsyncPrio HLE above -- necessarily, since
    # this frontend cannot deliver an interrupt-driven completion -- so the
    # globals stay zero and the poll never settles.
    #
    # With reads served synchronously the drive genuinely IS idle at every point
    # the guest can observe it, so DVD_STATE_END (0) is the truthful answer
    # rather than a placate. Reporting it here is the same shape as
    # DSPCheckInit: state the frontend owns because the layer beneath it was
    # deliberately replaced.
    # DVDReadAsyncPrio: removal tried TWICE (boundary-move, and again after
    # interrupt delivery started working). Both collapsed boot -- 108M guest
    # calls -> 16,011, gmMain 239 -> 0. The second attempt did fire a real DI
    # interrupt end-to-end (di reads=1, delivered=1), so delivery works on DI as
    # well as VI; what fails is the SDK's async state machine, which needs
    # completions arriving repeatedly rather than once.
    0x80337CF8,

    # lbDvd_800189EC: the sole driver of the 15.5M-call spin (widened LR capture
    # showed exactly one caller). It waits on a DVD completion.
    #
    # Option (b) -- let the real DVD path raise a real DI interrupt -- was tried
    # and reverted: boot collapsed 108M -> 16,011 calls. A real DI interrupt DID
    # fire end-to-end, so delivery works; the SDK's async state machine needs
    # completions arriving repeatedly, which this frontend cannot yet sustain.
    #
    # So this override is the pragmatic path, taken with that measurement behind
    # it rather than as a first resort. It should be retired once the async
    # paths complete continuously -- the note above records what that requires.
    # 0x800189EC,  # lbDvd_800189EC -- TRIED, REVERTED.
    #   Reached the most new code of any configuration (distinct 597 -> 616) and
    #   simultaneously wedged boot: gmMain 239 -> 1, calls 94.5M -> 35,862.
    #   Advancing into new code and sustaining the frame loop are different
    #   things, and this traded the second for the first.
    #
    #   Measured alongside it: the game issues only 2,240 bytes of GX traffic in
    #   240 frames -- GX init and nothing else. So no configuration tried so far
    #   has reached Melee's render path at all; the absence of graphics is not a
    #   graphics problem.

    0x80339B4C,   # DVDGetDriveStatus

    # HSD_SynthSFXWaitForLoadCompletion: THE stall. Boot spins here calling
    # lb_800195D0 21.7M times waiting for a Synth SFX bank load that never
    # completes, and everything downstream follows from it -- lb_80433318 stays
    # zero, HSD_Init and gmMain never run, no framebuffer, no frame.
    #
    # The load completes through the DevCom/ARAM path, whose completion this
    # frontend cannot signal (guest never sleeps, so nothing to wake; entering a
    # handler from an arbitrary point wedges the scheduler). The transfers
    # themselves DO happen -- 1,188 DVD reads and 1,187 ARAM transfers, all
    # synchronous through the HLEs above -- so by the time this is called the
    # data is already in place and the wait has nothing left to wait for.
    #
    # Same pattern as VIWaitForRetrace and DVDReadAsyncPrio, at the same layer,
    # for the same measured reason.
    0x80388B0C,   # HSD_SynthSFXWaitForLoadCompletion

    # ARQPostRequest: the ARAM leg, and the same problem as DVDReadAsyncPrio one
    # layer along. Measured: ARQPostRequest=2, ARStartDMA=1,
    # __ARQInterruptServiceRoutine=0 -- transfers are issued and never
    # acknowledged, because this configuration can deliver a completion by
    # neither route (the guest never sleeps, and the handler cannot be entered
    # from an arbitrary point without wedging OSSetCurrentContext).
    #
    # So perform the ARAM copy synchronously and invoke the request's callback
    # inline. This is the third instance of the same pattern -- VIWaitForRetrace,
    # DVDReadAsyncPrio, and now this -- which is what a runtime that models
    # devices but leaves signalling to the frontend actually requires.
    0x80352114,   # ARQPostRequest


    # __DSP_boot_task: the AX microcode handshake needs a real DSP to answer.
    # Stubbed as "started" so boot reaches the render loop; costs audio, and is
    # the override the DSP LLE core will replace.
}


# Mid-function probe points (guest addresses). The tracer hooks function entries
# only; these give full register visibility at a chosen instruction, which is
# what the DVD-side investigation needs -- code says HSD_DevComDVDWakeUp reads
# its queue head from 0x804C6320, but sampling that address at every wake reads
# zero even for wakes that demonstrably dispatched. Probing immediately after
# the load settles which is wrong.
PROBES = tuple(range(0x8038F4F0, 0x8038F6C0, 4))
# Every instruction in HSD_DevComDVDWakeUp for one run. Two explanations for
# how control reaches 0x8038F5F0 without executing the DVDReadAsyncPrio call
# at 0x8038F5EC were disproven (no direct branch targets it; the probe log
# did not overflow -- 29 records, 0 dropped). Reading the actual execution
# path is the way to find the real predecessor instead of guessing a third
# time. An indirect branch (bctr/blr) is the standing candidate.
