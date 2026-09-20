# Audio validation - 2026-09-20

The Akaneia GameCube track exposed an AX DSP address-boundary bug. At an ADPCM end address inside a frame header, the previous reader skipped past the end instead of following the accelerator's special wrap behavior. It then decoded unrelated ARAM, repeated about 1.91 seconds of music, and produced large sample jumps. The reader now handles both header-nibble boundaries and prefetches the next frame header before saving the voice parameter block. Ordinary sample ends, streamed predictor history, non-stream loops and PCM wrapping retain their expected behavior.

The boundary rules were checked against [Dolphin's hardware-tested DSP accelerator](https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/Core/DSP/DSPAccelerator.cpp). The focused regression fails against the previous reader and passes after correction. It covers the two special end addresses, ordinary wrap/stop, next-header prefetch, streamed history and PCM. No gameplay timing or input is modified by this decoder correction.

Actual native two-player 1080p widescreen, scale-4 matches ran for 5,400 host frames each. The offline fixture selects the full 16-bit m-ex external stage ID through native SSS exit; an earlier 8-bit forced-stage attempt reached Kongo Jungle and is excluded. Correct stages and tracks were confirmed by the native selection/music logs and active player state.

| Check | Evidence |
| --- | --- |
| GameCube before | External stage 305, gcnrealm.hps; the track loses alignment near its short block at 16.94 seconds. In the analyzed 18-44 second interval: 18,501 adjacent-sample jumps above 20,000 and 715 saturated channel samples. |
| GameCube after | Same stage/track and attacks; the repeated section is gone. Same analyzed interval: zero large jumps and 16 saturated channel samples during mixed effects. |
| Green Hill after | External stage 301, native randomized playlist selected ghz2.hps; zero large jumps in the analyzed 18-44 second interval, clean native exit. The earlier baseline selected tails.hps, so it is not a matched-track A/B comparison. |
| Source comparison | Independent local HPS decoding with [vgmstream r2117](https://github.com/vgmstream/vgmstream/releases/tag/r2117), resampled for comparison with captured 32 kHz guest PCM. Sound effects remain in the native capture, so correlation is not expected to equal one. The decoder, source tracks and generated WAVs are private validation files, not distribution contents. |

Private evidence: `build/performance/audio-gamecube-native-before`, `audio-gamecube-boundary-fix`, `audio-greenhill-native-before`, and `audio-greenhill-boundary-fix`; each includes native logs, PCM, device timing and a report. Analysis/tools are under `build/audio-investigation`.

Device timing is opt-in through MELEE_AUDIO_STATS. Its SDL pull callback only updates atomic counters; producer-side CSV records distinguish playback shortfalls from decoder corruption. The GameCube corrected run had no further device shortfalls after stage loading. Occasional 70-216 ms loading/render stalls can still outlast the existing output queue; this correction does not promise removal of every playback gap or clipping from loud simultaneous effects. Physical speaker/headphone quality was not independently verified.
