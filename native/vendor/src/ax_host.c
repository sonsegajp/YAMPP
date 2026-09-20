// SPDX-License-Identifier: GPL-3.0-or-later
#include "gxruntime/ax_host.h"

#include "gxruntime/aram.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static u32 hilo(CPUState* cpu, u32 address) {
    return ((u32)mem_read16(cpu, address) << 16) | mem_read16(cpu, address + 2u);
}

static void write_s32(CPUState* cpu, u32 address, s32 value) {
    mem_write32(cpu, address, (u32)value);
}

static s32 read_s32(CPUState* cpu, u32 address) {
    return (s32)mem_read32(cpu, address);
}

static s16 clamp_s16(s64 value) {
    if (value > INT16_MAX)
        return INT16_MAX;
    if (value < INT16_MIN)
        return INT16_MIN;
    return (s16)value;
}

typedef struct {
    CPUState* cpu;
    u32 pb;
    u32 current;
    u32 end;
    u32 loop;
    u16 format;
    u16 loop_flag;
    u16 stream;
    u16 running;
    u16 pred_scale;
    s16 yn1;
    s16 yn2;
    s16 coef[16];
    u16 loop_pred_scale;
    s16 loop_yn1;
    s16 loop_yn2;
    u16 loop_counter;
    bool legacy;
} SampleReader;

static void sample_reader_load(SampleReader* reader, CPUState* cpu, u32 pb, bool legacy) {
    memset(reader, 0, sizeof *reader);
    reader->cpu = cpu;
    reader->pb = pb;
    reader->legacy = legacy;
    reader->running = mem_read16(cpu, pb + 0x0Eu);
    reader->stream = mem_read16(cpu, pb + 0x10u);
    reader->loop_flag = mem_read16(cpu, pb + 0x6Eu);
    reader->format = mem_read16(cpu, pb + 0x70u);
    reader->loop = hilo(cpu, pb + 0x72u);
    reader->end = hilo(cpu, pb + 0x76u);
    reader->current = hilo(cpu, pb + 0x7Au);
    for (u32 i = 0; i < 16; i++)
        reader->coef[i] = (s16)mem_read16(cpu, pb + 0x7Eu + i * 2u);
    reader->pred_scale = mem_read16(cpu, pb + 0xA0u);
    reader->yn1 = (s16)mem_read16(cpu, pb + 0xA2u);
    reader->yn2 = (s16)mem_read16(cpu, pb + 0xA4u);
    reader->loop_pred_scale = mem_read16(cpu, pb + 0xB4u);
    reader->loop_yn1 = (s16)mem_read16(cpu, pb + 0xB6u);
    reader->loop_yn2 = (s16)mem_read16(cpu, pb + 0xB8u);
    reader->loop_counter = legacy ? 0 : mem_read16(cpu, pb + 0xC2u);
}

static void sample_reader_store(const SampleReader* reader) {
    CPUState* cpu = reader->cpu;
    u32 pb = reader->pb;
    mem_write16(cpu, pb + 0x0Eu, reader->running);
    mem_write16(cpu, pb + 0x7Au, (u16)(reader->current >> 16));
    mem_write16(cpu, pb + 0x7Cu, (u16)reader->current);
    mem_write16(cpu, pb + 0xA0u, reader->pred_scale);
    mem_write16(cpu, pb + 0xA2u, (u16)reader->yn1);
    mem_write16(cpu, pb + 0xA4u, (u16)reader->yn2);
    if (!reader->legacy) mem_write16(cpu, pb + 0xC2u, reader->loop_counter);
}

static void sample_reader_finish_or_loop(SampleReader* reader, u32 step) {
    if (reader->current != reader->end+step-1u)
        return;
    if (!reader->loop_flag) {
        if(getenv("MELEE_AUDIO_TRACE")) fprintf(stderr,"[voice-end] pb=%08X cur=%08X end=%08X loop=%08X running=%u\n",reader->pb,reader->current,reader->end,reader->loop,reader->running);
        reader->running = 0;
        return;
    }
    reader->current = reader->loop;
    reader->pred_scale = reader->loop_pred_scale;
    if (reader->stream == 1) {
        // Stream voices: loop advances the ring counter; yn state stays so the
        // next ARAM refill continues ADPCM prediction across buffer wraps.
        reader->loop_counter++;
    } else {
        reader->yn1 = reader->loop_yn1;
        reader->yn2 = reader->loop_yn2;
    }
}

static s16 sample_reader_next(SampleReader* reader) {
    if (reader->running != 1)
        return 0;


    s16 sample = 0;
    u32 step = 2;
    switch (reader->format) {
    case DOL_AX_FMT_ADPCM: {
        // Nintendo DSP ADPCM; current/end/loop are nibble addresses in ARAM.
        u8 packed = (u8)aram_read(reader->current >> 1, 1);
        s32 nibble = (reader->current & 1u) ? (packed & 0xFu) : (packed >> 4);
        if (nibble >= 8)
            nibble -= 16;
        u32 predictor = (reader->pred_scale >> 4) & 7u;
        u32 shift = reader->pred_scale & 0xFu;
        s64 value = (s64)nibble * ((s64)1 << shift) * 2048 +
                    (s64)reader->coef[predictor * 2u] * reader->yn1 +
                    (s64)reader->coef[predictor * 2u + 1u] * reader->yn2 +
                    1024;
        sample = clamp_s16(value >> 11);
        reader->yn2 = reader->yn1;
        reader->yn1 = sample;
        reader->current++;
        /* DSP accelerator header-edge behavior (Dolphin DSPAccelerator): an
         * end address in either header nibble wraps without resetting ADPCM
         * prediction. Prefetch the next header before storing PB state. */
        if ((reader->end & 15u) == 0 && reader->current == reader->end) {
            reader->current = reader->loop + 1u;
        } else if ((reader->end & 15u) == 1 && reader->current == reader->end - 1u) {
            reader->current = reader->loop;
        } else if ((reader->current & 15u) == 0) {
            reader->pred_scale = (u16)aram_read(reader->current >> 1, 1);
            reader->current += 2;
            step += 2;
        }
        break;
    }
    case DOL_AX_FMT_PCM16:
        sample = (s16)aram_read(reader->current * 2u, 2);
        reader->current++;
        break;
    case DOL_AX_FMT_PCM8:
        sample = (s16)((s8)aram_read(reader->current, 1) << 8);
        reader->current++;
        break;
    default:
        reader->running = 0;
        break;
    }

    sample_reader_finish_or_loop(reader,step);
    return sample;
}

static void read_resampled_samples(DolAxHost* ax, CPUState* cpu, u32 pb,
                                   s16 output[DOL_AX_SAMPLES_PER_MS]) {
    SampleReader reader;
    sample_reader_load(&reader, cpu, pb, ax->legacy_ax);

    u16 src_type = mem_read16(cpu, pb + 0x08u);
    u32 ratio = hilo(cpu, pb + 0xA6u);
    u32 position = mem_read16(cpu, pb + 0xAAu);
    s16 history[4];
    for (u32 i = 0; i < 4; i++)
        history[i] = (s16)mem_read16(cpu, pb + 0xACu + i * 2u);

    if (src_type == 2) {
        for (u32 i = 0; i < DOL_AX_SAMPLES_PER_MS; i++)
            output[i] = sample_reader_next(&reader);
        for (u32 i = 0; i < 4; i++)
            history[i] = output[DOL_AX_SAMPLES_PER_MS - 4u + i];
    } else {
        // MusyX source type 0 requests the DSP's polyphase filter. Coefficients
        // live in DSP ROM, so the portable backend uses linear mode as a
        // deterministic fallback.
        s16 ring[4];
        memcpy(ring, history, sizeof ring);
        u32 index = 4;
        for (u32 i = 0; i < DOL_AX_SAMPLES_PER_MS; i++) {
            position += ratio;
            while (position >= 0x10000u) {
                ring[index++ & 3u] = sample_reader_next(&reader);
                position -= 0x10000u;
            }

            u16 fraction = (u16)position;
            if (fraction) {
                s32 first = ring[index++ & 3u];
                s32 second = ring[index++ & 3u];
                output[i] = (s16)((first * (u16)-fraction + second * fraction) >>
                                  16);
                index += 2;
            } else {
                output[i] = ring[index++ & 3u];
                index += 3;
            }
        }
        history[3] = ring[--index & 3u];
        history[2] = ring[--index & 3u];
        history[1] = ring[--index & 3u];
        history[0] = ring[--index & 3u];
    }

    mem_write16(cpu, pb + 0xAAu, (u16)position);
    for (u32 i = 0; i < 4; i++)
        mem_write16(cpu, pb + 0xACu + i * 2u, (u16)history[i]);
    sample_reader_store(&reader);
}

static void apply_updates(CPUState* cpu, u32 pb, u32 millisecond,
                          u32* update_index) {
    u16 count = mem_read16(cpu, pb + 0x44u + millisecond * 2u);
    u32 updates = hilo(cpu, pb + 0x4Eu);
    for (u16 i = 0; i < count; i++, (*update_index)++) {
        u32 entry = updates + *update_index * 4u;
        u16 word_offset = mem_read16(cpu, entry);
        u16 value = mem_read16(cpu, entry + 2u);
        mem_write16(cpu, pb + (u32)word_offset * 2u, value);
    }
}

static void mix_channel(DolAxHost* ax, CPUState* cpu, u32 pb, const s16* input,
                        u32 buffer, u32 mix_offset, u32 dpop_offset, bool enabled,
                        bool ramp, u32 frame_offset) {
    u16 volume = mem_read16(cpu, pb + mix_offset);
    u16 delta = ramp ? mem_read16(cpu, pb + mix_offset + 2u) : 0;
    s16 last = 0;
    if (enabled) {
        for (u32 i = 0; i < DOL_AX_SAMPLES_PER_MS; i++) {
            s16 mixed = clamp_s16(((s64)input[i] * volume) >> 15);
            ax->mix[buffer][frame_offset + i] += mixed;
            volume = (u16)(volume + delta);
            last = mixed;
        }
    }
    mem_write16(cpu, pb + mix_offset, volume);
    mem_write16(cpu, pb + dpop_offset, (u16)last);
}

static void process_voice_ms(DolAxHost* ax, CPUState* cpu, u32 pb,
                             u32 frame_offset) {
    if (mem_read16(cpu, pb + 0x0Eu) != 1)
        return;

    s16 samples[DOL_AX_SAMPLES_PER_MS];
    read_resampled_samples(ax, cpu, pb, samples);

    s16 envelope = (s16)mem_read16(cpu, pb + 0x64u);
    s16 envelope_delta = (s16)mem_read16(cpu, pb + 0x66u);
    for (u32 i = 0; i < DOL_AX_SAMPLES_PER_MS; i++) {
        samples[i] = clamp_s16(((s64)samples[i] * envelope) >> 15);
        envelope = (s16)(envelope + envelope_delta);
    }
    mem_write16(cpu, pb + 0x64u, (u16)envelope);

    if (!ax->legacy_ax && mem_read16(cpu, pb + 0xBAu)) {
        s16 history = (s16)mem_read16(cpu, pb + 0xBCu);
        u16 a0 = mem_read16(cpu, pb + 0xBEu);
        s16 b0 = (s16)mem_read16(cpu, pb + 0xC0u);
        for (u32 i = 0; i < DOL_AX_SAMPLES_PER_MS; i++) {
            history = samples[i] =
                clamp_s16(((s64)a0 * samples[i] + (s64)b0 * history) >> 15);
        }
        mem_write16(cpu, pb + 0xBCu, (u16)history);
    }

    u16 control = mem_read16(cpu, pb + 0x0Cu);
    if (ax->legacy_ax) {
        /* Early AX (Melee, ucode 4e8a8b21): main L/R are unconditional;
         * bits 0..2 enable AUX/surround and bit 3 ramps all channels.
         * Matches Dolphin AXUCode::ConvertMixerControl. */
        u16 old=control;
        control=3;
        if (old&0x10) {
            if (!(old&6)) control|=0x600;
            if ((old&7)==1) control|=0xb0;
        } else {
            if (old&1) control|=0x30;
            if (old&2) control|=0x600;
            if (old&4) {
                control|=4;
                if(old&1) control|=0x80;
                if(old&2) control|=0x1000;
            }
        }
        if(old&8) control|=0x2948;
    }
    mix_channel(ax, cpu, pb, samples, DOL_AX_MAIN_L, 0x12u, 0x52u,
                (control & 0x0001u) != 0, (control & 0x0008u) != 0, frame_offset);
    mix_channel(ax, cpu, pb, samples, DOL_AX_MAIN_R, 0x16u, 0x58u,
                (control & 0x0002u) != 0, (control & 0x0008u) != 0, frame_offset);
    mix_channel(ax, cpu, pb, samples, DOL_AX_MAIN_S, 0x2Eu, 0x5Eu,
                (control & 0x0004u) != 0, (control & 0x0008u) != 0, frame_offset);
    mix_channel(ax, cpu, pb, samples, DOL_AX_AUXA_L, 0x1Au, 0x54u,
                (control & 0x0010u) != 0, (control & 0x0040u) != 0, frame_offset);
    mix_channel(ax, cpu, pb, samples, DOL_AX_AUXA_R, 0x1Eu, 0x5Au,
                (control & 0x0020u) != 0, (control & 0x0040u) != 0, frame_offset);
    mix_channel(ax, cpu, pb, samples, DOL_AX_AUXA_S, 0x32u, 0x60u,
                (control & 0x0080u) != 0, (control & 0x0100u) != 0, frame_offset);
    mix_channel(ax, cpu, pb, samples, DOL_AX_AUXB_L, 0x22u, 0x56u,
                (control & 0x0200u) != 0, (control & 0x0800u) != 0, frame_offset);
    mix_channel(ax, cpu, pb, samples, DOL_AX_AUXB_R, 0x26u, 0x5Cu,
                (control & 0x0400u) != 0, (control & 0x0800u) != 0, frame_offset);
    mix_channel(ax, cpu, pb, samples, DOL_AX_AUXB_S, 0x2Au, 0x62u,
                (control & 0x1000u) != 0, (control & 0x2000u) != 0, frame_offset);
}

static void process_pb_list(DolAxHost* ax, CPUState* cpu, u32 first_pb) {
    u32 pb = first_pb;
    for (u32 voice = 0; pb && voice < DOL_AX_MAX_VOICES; voice++) {
        if (mem_read16(cpu, pb + 0x0Eu) == 1)
            ax->active_voices++;
        u32 update_index = 0;
        for (u32 ms = 0; ms < DOL_AX_FRAME_MS; ms++) {
            apply_updates(cpu, pb, ms, &update_index);
            process_voice_ms(ax, cpu, pb, ms * DOL_AX_SAMPLES_PER_MS);
        }
        pb = hilo(cpu, pb);
    }
}

static void setup_processing(DolAxHost* ax, CPUState* cpu, u32 address) {
    memset(ax->mix, 0, sizeof ax->mix);
    for (u32 channel = 0; channel < DOL_AX_CHANNELS; channel++) {
        s32 value = (s32)mem_read32(cpu, address + channel * 6u);
        s16 delta = (s16)mem_read16(cpu, address + channel * 6u + 4u);
        if (!value)
            continue;
        for (u32 sample = 0; sample < DOL_AX_FRAME_SAMPLES; sample++) {
            ax->mix[channel][sample] = value;
            value += delta;
        }
    }
}

static void download_and_mix(DolAxHost* ax, CPUState* cpu, u32 address,
                             u16 main_volume, u16 auxa_volume, u16 auxb_volume) {
    const u16 volumes[3] = {main_volume, auxa_volume, auxb_volume};
    const u32 channels[3][3] = {
        {DOL_AX_MAIN_L, DOL_AX_MAIN_R, DOL_AX_MAIN_S},
        {DOL_AX_AUXA_L, DOL_AX_AUXA_R, DOL_AX_AUXA_S},
        {DOL_AX_AUXB_L, DOL_AX_AUXB_R, DOL_AX_AUXB_S},
    };
    u32 cursor = address;
    for (u32 group = 0; group < 3; group++) {
        for (u32 component = 0; component < 3; component++) {
            s32* destination = ax->mix[channels[group][component]];
            for (u32 i = 0; i < DOL_AX_FRAME_SAMPLES; i++, cursor += 4u)
                destination[i] +=
                    (s32)(((s64)read_s32(cpu, cursor) * volumes[group]) >> 15);
        }
    }
}

static void upload_three(DolAxHost* ax, CPUState* cpu, u32 address,
                         u32 first_channel) {
    for (u32 channel = 0; channel < 3; channel++)
        for (u32 i = 0; i < DOL_AX_FRAME_SAMPLES; i++)
            write_s32(cpu, address + (channel * DOL_AX_FRAME_SAMPLES + i) * 4u,
                      ax->mix[first_channel + channel][i]);
}

static void mix_aux(DolAxHost* ax, CPUState* cpu, u32 aux_channel,
                    u32 write_address, u32 read_address) {
    if (write_address)
        upload_three(ax, cpu, write_address, aux_channel);
    for (u32 channel = 0; channel < 3; channel++)
        for (u32 i = 0; i < DOL_AX_FRAME_SAMPLES; i++)
            ax->mix[DOL_AX_MAIN_L + channel][i] +=
                read_s32(cpu, read_address +
                                  (channel * DOL_AX_FRAME_SAMPLES + i) * 4u);
}

static void output_samples(DolAxHost* ax, CPUState* cpu, u32 stereo_address,
                           u32 surround_address) {
    u32 peak = 0;
    for (u32 i = 0; i < DOL_AX_FRAME_SAMPLES; i++) {
        write_s32(cpu, surround_address + i * 4u, ax->mix[DOL_AX_MAIN_S][i]);
        s16 left = clamp_s16(ax->mix[DOL_AX_MAIN_L][i]);
        s16 right = clamp_s16(ax->mix[DOL_AX_MAIN_R][i]);
        // GameCube AI DMA is interleaved right/left. SDL receives conventional
        // left/right ordering at the AID drain site.
        mem_write16(cpu, stereo_address + i * 4u, (u16)right);
        mem_write16(cpu, stereo_address + i * 4u + 2u, (u16)left);
        u32 left_magnitude = left == INT16_MIN ? 32768u : (u32)abs(left);
        u32 right_magnitude = right == INT16_MIN ? 32768u : (u32)abs(right);
        if (left_magnitude > peak)
            peak = left_magnitude;
        if (right_magnitude > peak)
            peak = right_magnitude;
    }
    ax->stats.frames++;
    ax->stats.active_voices = ax->active_voices;
    if (ax->active_voices > ax->stats.peak_voices)
        ax->stats.peak_voices = ax->active_voices;
    if (peak > ax->stats.peak_mix)
        ax->stats.peak_mix = peak;
    if (peak > 0u)
        ax->stats.nonzero_frames++;
    if (ax->log && (ax->stats.frames <= 10u || ax->stats.frames % 100u == 0u))
        fprintf(stderr, "[audio] frame %llu: voices=%u peak=%u\n",
                (unsigned long long)ax->stats.frames, ax->active_voices, peak);
}

static void mix_auxb_lr(DolAxHost* ax, CPUState* cpu, u32 upload_address,
                        u32 download_address) {
    for (u32 channel = 0; channel < 2; channel++)
        for (u32 i = 0; i < DOL_AX_FRAME_SAMPLES; i++)
            write_s32(cpu,
                      upload_address + (channel * DOL_AX_FRAME_SAMPLES + i) * 4u,
                      ax->mix[DOL_AX_AUXB_L + channel][i]);
    for (u32 channel = 0; channel < 2; channel++) {
        for (u32 i = 0; i < DOL_AX_FRAME_SAMPLES; i++) {
            s32 value = read_s32(
                cpu, download_address + (channel * DOL_AX_FRAME_SAMPLES + i) * 4u);
            ax->mix[DOL_AX_AUXB_L + channel][i] = value;
            ax->mix[DOL_AX_MAIN_L + channel][i] += value;
        }
    }
}

static void set_opposite_lr(DolAxHost* ax, CPUState* cpu, u32 address) {
    for (u32 i = 0; i < DOL_AX_FRAME_SAMPLES; i++) {
        s32 value = read_s32(cpu, address + i * 4u);
        ax->mix[DOL_AX_MAIN_L][i] = -value;
        ax->mix[DOL_AX_MAIN_R][i] = value;
        ax->mix[DOL_AX_MAIN_S][i] = 0;
    }
}

static void run_compressor(DolAxHost* ax, CPUState* cpu, u16 threshold,
                           u16 frames, u32 table) {
    (void)frames;
    bool triggered = false;
    for (u32 i = 0; i < DOL_AX_FRAME_SAMPLES; i++) {
        s32 left = ax->mix[DOL_AX_MAIN_L][i];
        s32 right = ax->mix[DOL_AX_MAIN_R][i];
        if (labs(left) > threshold || labs(right) > threshold) {
            triggered = true;
            break;
        }
    }
    if (!triggered)
        return;
    for (u32 i = 0; i < DOL_AX_FRAME_SAMPLES; i++) {
        u16 coefficient = mem_read16(cpu, table + i * 2u);
        ax->mix[DOL_AX_MAIN_L][i] =
            (s32)(((s64)ax->mix[DOL_AX_MAIN_L][i] * coefficient) >> 15);
        ax->mix[DOL_AX_MAIN_R][i] =
            (s32)(((s64)ax->mix[DOL_AX_MAIN_R][i] * coefficient) >> 15);
    }
}

void dol_ax_host_init(DolAxHost* ax) {
    if (ax == NULL)
        return;
    memset(ax, 0, sizeof *ax);
}

void dol_ax_host_set_log(DolAxHost* ax, bool enabled) {
    if (ax != NULL)
        ax->log = enabled;
}

void dol_ax_host_reset_stats(DolAxHost* ax) {
    if (ax == NULL)
        return;
    memset(&ax->stats, 0, sizeof ax->stats);
    ax->active_voices = 0;
}

const DolAxHostStats* dol_ax_host_stats(const DolAxHost* ax) {
    return ax != NULL ? &ax->stats : NULL;
}

void dol_ax_host_process_command_list(DolAxHost* ax, CPUState* cpu, u32 address) {
    if (ax == NULL || cpu == NULL)
        return;

    u32 cursor = address;
    u32 pb = 0;
    ax->active_voices = 0;
    for (u32 command_count = 0; command_count < DOL_AX_MAX_COMMANDS;
         command_count++) {
        u16 command = mem_read16(cpu, cursor);
        cursor += 2u;
        switch (command) {
        case 0:
            setup_processing(ax, cpu, hilo(cpu, cursor));
            cursor += 4u;
            break;
        case 1: {
            u32 source = hilo(cpu, cursor);
            u16 main_volume = mem_read16(cpu, cursor + 4u);
            u16 auxa_volume = mem_read16(cpu, cursor + 6u);
            u16 auxb_volume = mem_read16(cpu, cursor + 8u);
            cursor += 10u;
            download_and_mix(ax, cpu, source, main_volume, auxa_volume,
                             auxb_volume);
            break;
        }
        case 2:
            pb = hilo(cpu, cursor);
            cursor += 4u;
            break;
        case 3:
            process_pb_list(ax, cpu, pb);
            break;
        case 4:
        case 5: {
            u32 upload = hilo(cpu, cursor);
            u32 download = hilo(cpu, cursor + 4u);
            cursor += 8u;
            mix_aux(ax, cpu, command == 4 ? DOL_AX_AUXA_L : DOL_AX_AUXB_L,
                    upload, download);
            break;
        }
        case 6:
            upload_three(ax, cpu, hilo(cpu, cursor), DOL_AX_MAIN_L);
            cursor += 4u;
            break;
        case 7: {
            u32 source = hilo(cpu, cursor);
            cursor += 4u;
            for (u32 i = 0; i < DOL_AX_FRAME_SAMPLES; i++) {
                s32 value = read_s32(cpu, source + i * 4u);
                ax->mix[DOL_AX_MAIN_L][i] = value;
                ax->mix[DOL_AX_MAIN_R][i] = value;
                ax->mix[DOL_AX_MAIN_S][i] = 0;
            }
            break;
        }
        case 8:
            cursor += 20u;
            break;
        case 9:
            mix_aux(ax, cpu, DOL_AX_AUXB_L, 0, hilo(cpu, cursor));
            cursor += 4u;
            break;
        case 10:
        case 11:
        case 12:
            break;
        case 13:
            cursor = hilo(cpu, cursor);
            // Size word describes this new chunk for hardware DMA. Host memory
            // is directly accessible, so no copy is needed.
            break;
        case 14: {
            u32 surround = hilo(cpu, cursor);
            u32 stereo = hilo(cpu, cursor + 4u);
            cursor += 8u;
            output_samples(ax, cpu, stereo, surround);
            break;
        }
        case 15:
            return;
        case 16: {
            u32 upload = hilo(cpu, cursor);
            u32 download = hilo(cpu, cursor + 4u);
            cursor += 8u;
            mix_auxb_lr(ax, cpu, upload, download);
            break;
        }
        case 17:
            set_opposite_lr(ax, cpu, hilo(cpu, cursor));
            cursor += 4u;
            break;
        case 18: {
            u16 threshold = mem_read16(cpu, cursor);
            u16 frames = mem_read16(cpu, cursor + 2u);
            u32 table = hilo(cpu, cursor + 4u);
            cursor += 8u;
            run_compressor(ax, cpu, threshold, frames, table);
            break;
        }
        default:
            ax->stats.last_unknown_cmd = command;
            ax->stats.unknown_cmd_count++;
            if (ax->log)
                fprintf(stderr, "[audio] unknown AX command 0x%04X at 0x%08X\n",
                        command, cursor - 2u);
            return;
        }
    }
    if (ax->log)
        fprintf(stderr, "[audio] AX command list exceeded safety limit\n");
}
