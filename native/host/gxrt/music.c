/* Stage music: a now-playing bar when a track starts, and optional custom
 * tracks from user/mods/music/stage.
 *
 * Melee starts a background track by id; the id indexes the disc's HPS file
 * table (hps_files, 0x803BC314), which this file reads out of guest memory so
 * the name shown is always the track the game actually started.
 *
 * Custom music, one folder per track, any number of files in it:
 *
 *   user/mods/music/stage/Final Destination/my track.mp3
 *   user/mods/music/stage/corneria/another.flac        (HPS stem also works)
 *
 * Any format Windows can decode is accepted (mp3, m4a/aac, wma, flac, wav,
 * ogg when a codec is installed); decoding goes through Media Foundation, with
 * a built-in RIFF/WAVE reader as the fallback.
 */
#include "abi_recompcore.h"
#include "recomp_funcs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "platform_compat.h"
#ifdef _WIN32
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#else
#include <dirent.h>
#include <strings.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#define _stricmp strcasecmp
#endif

#define MUSIC_MAX_TRACKS 64
#ifndef MUSIC_MAX_FRAMES
#define MUSIC_MAX_FRAMES (48000u * 60u * 12u)   /* twelve minutes at 48 kHz */
#endif
#define HPS_TABLE 0x803BC314u                   /* const char* hps_files[0x62] */
#define HPS_COUNT 0x62
/* Custom tracks are mixed in beside the game's own output, which is quieter
 * than a mastered music file; this keeps them from burying the sound effects.
 * MELEE_MUSIC_GAIN overrides it (percent). */
static float music_gain = 0.60f;
#define MUSIC_GAIN music_gain

extern int aurora_link_music_bar(const char* text, int custom);
extern int music_text_show(Context* ctx, const char* title, int custom);
extern void func_80023F28(Context*);


/* Display names for the HPS files a match can start. Anything missing falls
 * back to the file stem, so a name is never invented. */
/* `stage` marks the tracks a match plays: the bar is for stages, so menu,
   opening and results music never brings it up. */
typedef struct { const char* stem; const char* name; int stage; } TrackName;
static const TrackName track_names[] = {
  {"bigblue", "Big Blue", 1}, {"castle", "Princess Peach's Castle", 1}, {"corneria", "Corneria", 1},
  {"flatzone", "Flat Zone", 1}, {"fourside", "Fourside", 1}, {"greatbay", "Great Bay", 1},
  {"greens", "Green Greens", 1}, {"icemt", "Icicle Mountain", 1}, {"izumi", "Fountain of Dreams", 1},
  {"kongo", "Kongo Jungle", 1}, {"kraid", "Brinstar Depths", 1}, {"mutecity", "Mute City", 1},
  {"onetto", "Onett", 1}, {"onetto2", "Onett", 1}, {"pokesta", "Pokemon Stadium", 1},
  {"pstadium", "Pokemon Stadium", 1}, {"rcruise", "Rainbow Cruise", 1}, {"venom", "Venom", 1},
  {"ystory", "Yoshi's Story", 1}, {"yorster", "Yoshi's Island", 1}, {"zebes", "Brinstar", 1},
  {"shrine", "Temple", 1}, {"saria", "Saria's Song", 1}, {"garden", "Kongo Jungle", 1},
  {"inis1_01", "Mushroom Kingdom", 1}, {"inis1_02", "Mushroom Kingdom", 1},
  {"inis2_01", "Mushroom Kingdom II", 1}, {"inis2_02", "Mushroom Kingdom II", 1},
  {"old_dk", "Kongo Jungle N64", 1}, {"old_kb", "Dream Land N64", 1}, {"old_ys", "Yoshi's Island N64", 1},
  {"target", "Target Test", 1}, {"hyaku", "Multi-Man Melee", 1}, {"hyaku2", "Multi-Man Melee", 1},
  {"swm_15min", "15-Minute Melee", 1}, {"mrider", "Mach Rider", 1}, {"smari3", "Super Mario Bros. 3", 1},
  {"docmari", "Dr. Mario", 1}, {"akaneia", "Fire Emblem", 1}, {"baloon", "Balloon Fight", 1},
  {"famidemo", "Famicom Medley", 1}, {"pura", "Poke Floats", 1}, {"menu01", "Menu", 0},
  {"menu02", "Menu", 0}, {"menu3", "Menu", 0}, {"opening", "Opening", 0}, {"ending", "Ending", 0},
  {"gameover", "Game Over", 0}, {"continue", "Continue", 0}, {"1p_qk", "Trophy Collection", 0},
  {"s_select", "Character Select", 0}, {"target", "Target Test", 1},
  /* Added tracks from the official Akaneia release; audio remains user-supplied. */
  {"mcavern", "Metal Mario Fight", 1},
  {"old_castle", "Peach's Castle 64", 1},
  {"old_hyrule", "Hyrule Castle 64", 1},
  {"old_mush", "Mushroom Kingdom 64", 1},
  {"old_saffron", "Saffron City 64", 1},
  {"old_zebes", "Planet Zebes 64", 1},
  {"starwolf", "Star Wolf", 1},
  {"sonic", "Open Your Heart", 1},
  {"ac_title", "Title (Animal Crossing)", 1},
  {"kk_go", "Go K.K. Rider! (Live)", 1},
  {"kk_urban", "K.K. Cruisin' (Live)", 1},
  {"kk_west", "K.K. Western (Live)", 1},
  {"kk_new", "K.K. Gumbo (Live)", 1},
  {"kk_rock", "Rockin' K.K. (Live)", 1},
  {"kk_euro", "DJ K.K. (Live)", 1},
  {"kk_peru", "K.K. Condor (Live)", 1},
  {"ghz", "Green Hill Zone", 1},
  {"delfino", "Delfino Plaza", 1},
  {"ricco", "Ricco Harbor", 1},
  {"mansion", "Luigi's Mansion (Main Theme)", 1},
  {"1am", "1AM (Dawn)", 1},
  {"7am", "7AM (Day)", 1},
  {"2pm", "2PM (Evening)", 1},
  {"5pm", "5PM (Night)", 1},
  {"pgym", "Gym Theme", 1},
  {"doltitle", "Dolphin Park/Title", 1},
  {"dedede", "King Dedede's Theme", 1},
  {"ff_sonic", "Sonic the Hedgehog Fanfare", 0},
  {"kouhi", "You've Come Far (Coffee Break)", 1},
  {"gcnrealm", "Gamecube Realm", 1},
  {"area6", "Area 6", 1},
  {"mtdedede", "Mt. Dedede", 1},
  {"goldenland", "Dark Golden Land", 1},
  {"flippant", "Flippant Foe", 1},
  {"castleisland", "Castle Theme (Yoshi's Island)", 1},
  {"fichina", "Fichina", 1},
  {"sonic2", "It Doesn't Matter", 1},
  {"tails", "Believe In Myself", 1},
  {"old_sectorz", "Sector Z", 1},
  {"violet", "Violet City", 1},
  {"ghz2", "Green Hill Zone (Alt)", 1},
};

typedef struct { char path[MAX_PATH]; char name[96]; } Track;
static Track tracks[MUSIC_MAX_TRACKS];
static int track_count;
static char folder_root[MAX_PATH];
static int media_foundation_ready;

/* Playback state, read by the audio mixer on the guest thread. */
static short* pcm;
static unsigned pcm_frames, pcm_channels, pcm_rate;
/* Decoding a whole track takes long enough to be seen as a hitch on the frame
 * that starts it, so it happens on its own thread and is handed over here when
 * it is done. The generation counter drops a decode the game has moved past. */
static CRITICAL_SECTION pcm_lock;
static char decode_path[MAX_PATH];
static volatile LONG decode_generation;
static double cursor;
static int playing;
static char now_playing[128];
static unsigned bar_frames;

static void stem_of(const char* file, char* out, unsigned cap) {
  snprintf(out, cap, "%s", file);
  char* dot = strrchr(out, '.');
  if (dot) *dot = 0;
}

/* The HPS file this track id plays, read from the game's own table. */
static int hps_file(Context* ctx, unsigned id, char* out, unsigned cap) {
  out[0] = 0;
  uint32_t table=HPS_TABLE,count=HPS_COUNT;
  if (g_mex_active) {
    /* m-ex Header.s: rtoc + 0x94 is BGMFileNames, +0x15C is BGMCount.
     * Read the live relocated table; added IDs exceed Melee's 98 entries. */
    uint32_t toc=ctx->gpr[2];
    if (toc<0x80000000u || toc>0x81800000u-0x160u) return 0;
    table=mem_read32(ctx,toc+0x94u);count=mem_read32(ctx,toc+0x15Cu);
    if (!count || count>4096u || table<0x80000000u || table>0x81800000u-count*4u) return 0;
  }
  if (id >= count) return 0;
  uint32_t p = mem_read32(ctx, table + id * 4u);
  if (p < 0x80000000u || p >= 0x81800000u) return 0;
  unsigned n = 0;
  while (n + 1 < cap && p + n < 0x81800000u) {
    unsigned char c = mem_read8(ctx, p + n);
    if (!c) break;
    out[n++] = (char)c;
  }
  out[n] = 0;
  return n > 0;
}

static const TrackName* track_entry(const char* stem) {
  for (unsigned i = 0; i < sizeof track_names / sizeof *track_names; i++)
    if (!_stricmp(track_names[i].stem, stem)) return &track_names[i];
  return NULL;
}
static const char* display_name(const char* stem) {
  const TrackName* entry = track_entry(stem);
  return entry ? entry->name : NULL;
}
/* Only a track a stage plays gets a bar. An unknown stem is not assumed to be
   one, so nothing pops up over the menus. */
static int is_stage_track(const char* stem) {
  const TrackName* entry = track_entry(stem);
  return entry && entry->stage;
}

/* ---- decoding -------------------------------------------------------------- */
static int read_u32le(const unsigned char* p) { return (int)(p[0] | (p[1] << 8) | (p[2] << 16) | ((unsigned)p[3] << 24)); }
static int read_u16le(const unsigned char* p) { return p[0] | (p[1] << 8); }

/* Fallback for plain PCM WAV, used when Media Foundation is unavailable. */
typedef struct { short* samples; unsigned frames, channels, rate; } DecodeResult;

static int load_wav(const char* path, DecodeResult* out) {
  FILE* f = fopen(path, "rb");
  if (!f) return 0;
  unsigned char header[12];
  if (fread(header, 1, 12, f) != 12 || memcmp(header, "RIFF", 4) || memcmp(header + 8, "WAVE", 4)) { fclose(f); return 0; }
  unsigned channels = 0, rate = 0, bits = 0;
  for (;;) {
    unsigned char chunk[8];
    if (fread(chunk, 1, 8, f) != 8) break;
    unsigned size = (unsigned)read_u32le(chunk + 4);
    if (!memcmp(chunk, "fmt ", 4)) {
      unsigned char fmt[16];
      if (size < 16 || fread(fmt, 1, 16, f) != 16) break;
      if (read_u16le(fmt) != 1) break;
      channels = (unsigned)read_u16le(fmt + 2);
      rate = (unsigned)read_u32le(fmt + 4);
      bits = (unsigned)read_u16le(fmt + 14);
      if (size > 16) fseek(f, (long)(size - 16), SEEK_CUR);
    } else if (!memcmp(chunk, "data", 4)) {
      if (!channels || channels > 2 || (bits != 8 && bits != 16) || rate < 8000 || rate > 96000) break;
      unsigned frames = size / (channels * (bits / 8));
      if (!frames || frames > MUSIC_MAX_FRAMES) break;
      short* samples = (short*)malloc((size_t)frames * channels * sizeof(short));
      if (!samples) break;
      if (bits == 16) {
        if (fread(samples, sizeof(short), (size_t)frames * channels, f) != (size_t)frames * channels) { free(samples); break; }
      } else {
        unsigned char* bytes = (unsigned char*)malloc(size);
        if (!bytes) { free(samples); break; }
        if (fread(bytes, 1, size, f) != size) { free(bytes); free(samples); break; }
        for (unsigned i = 0; i < frames * channels; i++) samples[i] = (short)((bytes[i] - 128) << 8);
        free(bytes);
      }
      fclose(f);
      out->samples = samples; out->frames = frames; out->channels = channels; out->rate = rate;
      return 1;
    } else {
      fseek(f, (long)((size + 1u) & ~1u), SEEK_CUR);
    }
  }
  fclose(f);
  return 0;
}

#ifdef _WIN32
/* Any format Windows has a decoder for, via Media Foundation. */
static int load_media(const char* path, DecodeResult* out) {
  if (!media_foundation_ready) return 0;
  wchar_t wide[MAX_PATH];
  if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, wide, MAX_PATH)) return 0;
  IMFSourceReader* reader = NULL;
  if (FAILED(MFCreateSourceReaderFromURL(wide, NULL, &reader)) || !reader) return 0;

  int ok = 0;
  IMFMediaType* want = NULL;
  short* samples = NULL;
  unsigned frames = 0, capacity = 0, channels = 0, rate = 0;
  if (SUCCEEDED(MFCreateMediaType(&want))) {
    want->lpVtbl->SetGUID(want, &MF_MT_MAJOR_TYPE, &MFMediaType_Audio);
    want->lpVtbl->SetGUID(want, &MF_MT_SUBTYPE, &MFAudioFormat_PCM);
    want->lpVtbl->SetUINT32(want, &MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    if (SUCCEEDED(reader->lpVtbl->SetCurrentMediaType(reader, (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, NULL, want))) {
      IMFMediaType* actual = NULL;
      if (SUCCEEDED(reader->lpVtbl->GetCurrentMediaType(reader, (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, &actual)) && actual) {
        UINT32 c = 0, r = 0;
        actual->lpVtbl->GetUINT32(actual, &MF_MT_AUDIO_NUM_CHANNELS, &c);
        actual->lpVtbl->GetUINT32(actual, &MF_MT_AUDIO_SAMPLES_PER_SECOND, &r);
        channels = c; rate = r;
        actual->lpVtbl->Release(actual);
      }
      reader->lpVtbl->SetStreamSelection(reader, (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE);
      if (channels >= 1 && channels <= 2 && rate >= 8000 && rate <= 192000) {
        for (;;) {
          DWORD flags = 0;
          IMFSample* sample = NULL;
          if (FAILED(reader->lpVtbl->ReadSample(reader, (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, NULL, &flags, NULL, &sample))) break;
          if (flags & MF_SOURCE_READERF_ENDOFSTREAM) { if (sample) sample->lpVtbl->Release(sample); ok = frames > 0; break; }
          if (!sample) continue;
          IMFMediaBuffer* buffer = NULL;
          if (SUCCEEDED(sample->lpVtbl->ConvertToContiguousBuffer(sample, &buffer)) && buffer) {
            BYTE* data = NULL; DWORD length = 0;
            if (SUCCEEDED(buffer->lpVtbl->Lock(buffer, &data, NULL, &length))) {
              unsigned got = length / (unsigned)(channels * sizeof(short));
              if (frames + got > MUSIC_MAX_FRAMES) got = frames < MUSIC_MAX_FRAMES ? MUSIC_MAX_FRAMES - frames : 0;
              if (got) {
                if (frames + got > capacity) {
                  unsigned grow = capacity ? capacity * 2 : (rate * channels);
                  while (grow < frames + got) grow *= 2;
                  short* bigger = (short*)realloc(samples, (size_t)grow * channels * sizeof(short));
                  if (!bigger) { buffer->lpVtbl->Unlock(buffer); buffer->lpVtbl->Release(buffer); sample->lpVtbl->Release(sample); goto done; }
                  samples = bigger; capacity = grow;
                }
                memcpy(samples + (size_t)frames * channels, data, (size_t)got * channels * sizeof(short));
                frames += got;
              }
              buffer->lpVtbl->Unlock(buffer);
            }
            buffer->lpVtbl->Release(buffer);
          }
          sample->lpVtbl->Release(sample);
          if (frames >= MUSIC_MAX_FRAMES) { ok = 1; break; }
        }
      }
    }
  }
done:
  if (want) want->lpVtbl->Release(want);
  reader->lpVtbl->Release(reader);
  if (ok && frames) {
    out->samples = samples; out->frames = frames; out->channels = channels; out->rate = rate;
    return 1;
  }
  free(samples);
  return 0;
}
#else /* Linux: decode via FFmpeg/libav */
/* PCM capacity is measured in frames, and never exceeds the decode limit. */
static int media_reserve(short** samples, unsigned* capacity, unsigned needed,
                         unsigned channels, unsigned rate) {
  if (needed > MUSIC_MAX_FRAMES) return 0;
  if (needed <= *capacity) return 1;
  unsigned grow = *capacity ? *capacity : rate * channels;
  if (grow > MUSIC_MAX_FRAMES) grow = MUSIC_MAX_FRAMES;
  while (grow < needed)
    grow = grow > MUSIC_MAX_FRAMES / 2u ? MUSIC_MAX_FRAMES : grow * 2u;
  short* bigger = (short*)realloc(*samples, (size_t)grow * channels * sizeof(short));
  if (!bigger) return 0;
  *samples = bigger;
  *capacity = grow;
  return 1;
}
static int load_media(const char* path, DecodeResult* out) {
  AVFormatContext* fmt_ctx = NULL;
  const AVCodec* codec = NULL;
  AVCodecContext* codec_ctx = NULL;
  SwrContext* swr = NULL;
  AVPacket* pkt = NULL;
  AVFrame* frame = NULL;
  short* samples = NULL;
  unsigned total_frames = 0, capacity = 0, channels = 0, rate = 0;
  int stream_idx, ok = 0;

  if (avformat_open_input(&fmt_ctx, path, NULL, NULL) < 0) return 0;
  if (avformat_find_stream_info(fmt_ctx, NULL) < 0) goto cleanup;
  stream_idx = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
  if (stream_idx < 0) goto cleanup;

  codec_ctx = avcodec_alloc_context3(codec);
  if (!codec_ctx) goto cleanup;
  avcodec_parameters_to_context(codec_ctx, fmt_ctx->streams[stream_idx]->codecpar);
  if (avcodec_open2(codec_ctx, codec, NULL) < 0) goto cleanup;

  channels = codec_ctx->ch_layout.nb_channels;
  rate = (unsigned)codec_ctx->sample_rate;
  if (channels < 1 || channels > 2 || rate < 8000 || rate > 192000) goto cleanup;

  /* Resample to stereo s16 */
  {
    AVChannelLayout out_layout = channels == 1
      ? (AVChannelLayout)AV_CHANNEL_LAYOUT_MONO
      : (AVChannelLayout)AV_CHANNEL_LAYOUT_STEREO;
    if (swr_alloc_set_opts2(&swr, &out_layout, AV_SAMPLE_FMT_S16, (int)rate,
                            &codec_ctx->ch_layout, codec_ctx->sample_fmt,
                            codec_ctx->sample_rate, 0, NULL) < 0) goto cleanup;
  }
  if (swr_init(swr) < 0) goto cleanup;

  pkt = av_packet_alloc();
  frame = av_frame_alloc();
  if (!pkt || !frame) goto cleanup;

  while (av_read_frame(fmt_ctx, pkt) >= 0) {
    if (pkt->stream_index != stream_idx) { av_packet_unref(pkt); continue; }
    if (avcodec_send_packet(codec_ctx, pkt) < 0) { av_packet_unref(pkt); break; }
    av_packet_unref(pkt);
    while (avcodec_receive_frame(codec_ctx, frame) >= 0) {
      int out_samples = swr_get_out_samples(swr, frame->nb_samples);
      if (out_samples <= 0) continue;
      if ((unsigned)out_samples > MUSIC_MAX_FRAMES - total_frames)
        out_samples = (int)(MUSIC_MAX_FRAMES - total_frames);
      if (out_samples <= 0) { ok = total_frames > 0; goto cleanup; }
      if (!media_reserve(&samples, &capacity, total_frames + (unsigned)out_samples,
                         channels, rate)) goto cleanup;
      uint8_t* out_buf = (uint8_t*)(samples + (size_t)total_frames * channels);
      int converted = swr_convert(swr, &out_buf, out_samples,
                                  (const uint8_t**)frame->extended_data, frame->nb_samples);
      if (converted > 0) total_frames += (unsigned)converted;
      if (total_frames >= MUSIC_MAX_FRAMES) { ok = 1; goto cleanup; }
    }
  }
  /* Flush decoder */
  avcodec_send_packet(codec_ctx, NULL);
  while (avcodec_receive_frame(codec_ctx, frame) >= 0) {
    int out_samples = swr_get_out_samples(swr, frame->nb_samples);
    if (out_samples <= 0) continue;
    if ((unsigned)out_samples > MUSIC_MAX_FRAMES - total_frames)
      out_samples = (int)(MUSIC_MAX_FRAMES - total_frames);
    if (out_samples <= 0) { ok = total_frames > 0; goto cleanup; }
    if (!media_reserve(&samples, &capacity, total_frames + (unsigned)out_samples,
                       channels, rate)) goto cleanup;
    uint8_t* out_buf = (uint8_t*)(samples + (size_t)total_frames * channels);
    int converted = swr_convert(swr, &out_buf, out_samples,
                                (const uint8_t**)frame->extended_data, frame->nb_samples);
    if (converted > 0) total_frames += (unsigned)converted;
    if (total_frames >= MUSIC_MAX_FRAMES) { ok = 1; goto cleanup; }
  }
  ok = total_frames > 0;

cleanup:
  if (frame) av_frame_free(&frame);
  if (pkt) av_packet_free(&pkt);
  if (swr) swr_free(&swr);
  if (codec_ctx) avcodec_free_context(&codec_ctx);
  if (fmt_ctx) avformat_close_input(&fmt_ctx);
  if (ok && total_frames) {
    out->samples = samples; out->frames = total_frames; out->channels = channels; out->rate = rate;
    return 1;
  }
  free(samples);
  return 0;
}
#endif

static int load_any(const char* path, DecodeResult* out) {
  if (load_media(path, out)) return 1;
  return load_wav(path, out);
}

/* ---- track folders --------------------------------------------------------- */
#ifdef _WIN32
static void scan_folder(const char* folder) {
  static const char* skip[] = {".txt", ".md", ".ini", ".json"};
  char pattern[MAX_PATH];
  snprintf(pattern, sizeof pattern, "%s\\*.*", folder);
  WIN32_FIND_DATAA found;
  HANDLE handle = FindFirstFileA(pattern, &found);
  if (handle == INVALID_HANDLE_VALUE) return;
  do {
    if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
    const char* dot = strrchr(found.cFileName, '.');
    if (!dot) continue;
    int ignored = 0;
    for (unsigned i = 0; i < sizeof skip / sizeof *skip; i++) if (!_stricmp(dot, skip[i])) ignored = 1;
    if (ignored || track_count >= MUSIC_MAX_TRACKS) continue;
    Track* t = &tracks[track_count++];
    snprintf(t->path, sizeof t->path, "%s\\%s", folder, found.cFileName);
    stem_of(found.cFileName, t->name, sizeof t->name);
  } while (FindNextFileA(handle, &found));
  FindClose(handle);
}
#else
static void scan_folder(const char* folder) {
  static const char* skip[] = {".txt", ".md", ".ini", ".json"};
  DIR* dir = opendir(folder);
  if (!dir) return;
  struct dirent* entry;
  while ((entry = readdir(dir)) != NULL) {
    if (entry->d_type == DT_DIR) continue;
    const char* dot = strrchr(entry->d_name, '.');
    if (!dot) continue;
    int ignored = 0;
    for (unsigned i = 0; i < sizeof skip / sizeof *skip; i++) if (!strcasecmp(dot, skip[i])) ignored = 1;
    if (ignored || track_count >= MUSIC_MAX_TRACKS) continue;
    Track* t = &tracks[track_count++];
    snprintf(t->path, sizeof t->path, "%s/%s", folder, entry->d_name);
    stem_of(entry->d_name, t->name, sizeof t->name);
  }
  closedir(dir);
}
#endif

void music_init(void) {
  InitializeCriticalSection(&pcm_lock);
  const char* mods = getenv("MELEE_MOD_REGISTRY");
  char base[MAX_PATH];
  if (mods && *mods) {
    snprintf(base, sizeof base, "%s", mods);
    char* slash = strrchr(base, '/');
#ifdef _WIN32
    if (!slash) slash = strrchr(base, '\\');
#endif
    if (slash) *slash = 0;
  } else {
#ifdef _WIN32
    snprintf(base, sizeof base, "user\\mods");
#else
    snprintf(base, sizeof base, "user/mods");
#endif
  }
#ifdef _WIN32
  snprintf(folder_root, sizeof folder_root, "%s\\music\\stage", base);
#else
  snprintf(folder_root, sizeof folder_root, "%s/music/stage", base);
#endif
  const char* gain = getenv("MELEE_MUSIC_GAIN");
  if (gain && *gain) { int percent = atoi(gain); if (percent > 0 && percent <= 200) music_gain = percent / 100.f; }
#ifdef _WIN32
  media_foundation_ready = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE));
  fprintf(stderr, "[music] custom tracks from %s (decoder: %s)\n", folder_root,
          media_foundation_ready ? "any format Windows can play" : "WAV only");
#else
  media_foundation_ready = 1; /* FFmpeg always available */
  fprintf(stderr, "[music] custom tracks from %s (decoder: FFmpeg)\n", folder_root);
#endif
}

static DWORD
#ifdef _WIN32
WINAPI
#endif
decode_worker(LPVOID param) {
  LONG generation = (LONG)(intptr_t)param;
  char path[MAX_PATH];
  EnterCriticalSection(&pcm_lock);
  snprintf(path, sizeof path, "%s", decode_path);
  LeaveCriticalSection(&pcm_lock);
  DecodeResult result = {0};
  int ok = load_any(path, &result);
  EnterCriticalSection(&pcm_lock);
  if (!ok) fprintf(stderr, "[music] cannot decode %s\n", path);
  else if (generation != decode_generation) { free(result.samples); }
  else {
    free(pcm);
    pcm = result.samples; pcm_frames = result.frames; pcm_channels = result.channels; pcm_rate = result.rate;
    cursor = 0; playing = 1;
    fprintf(stderr, "[music] custom track ready: %s (%u Hz, %u ch, %u frames)\n", now_playing, pcm_rate, pcm_channels, pcm_frames);
  }
  LeaveCriticalSection(&pcm_lock);
  return 0;
}

#ifndef _WIN32
void* music_decode_thread(void* arg) {
  LONG gen = ((struct { LONG gen; }*)arg)->gen;
  free(arg);
  decode_worker((LPVOID)(intptr_t)gen);
  return NULL;
}
#endif

/* Pick a custom track for this HPS file, by display name or by file stem. */
#ifdef _WIN32
static HANDLE decoder_threads[64];
#endif
static int choose_custom(const char* stem, const char* name) {
  track_count = 0;
  if (!folder_root[0]) return 0;
  char folder[MAX_PATH];
#ifdef _WIN32
  if (name) { snprintf(folder, sizeof folder, "%s\\%s", folder_root, name); scan_folder(folder); }
  if (!track_count) { snprintf(folder, sizeof folder, "%s\\%s", folder_root, stem); scan_folder(folder); }
#else
  if (name) { snprintf(folder, sizeof folder, "%s/%s", folder_root, name); scan_folder(folder); }
  if (!track_count) { snprintf(folder, sizeof folder, "%s/%s", folder_root, stem); scan_folder(folder); }
#endif
  if (!track_count) return 0;
  Track* pick = &tracks[(unsigned)(GetTickCount() / 7u) % (unsigned)track_count];
  EnterCriticalSection(&pcm_lock);
  snprintf(decode_path, sizeof decode_path, "%s", pick->path);
  snprintf(now_playing, sizeof now_playing, "%s", pick->name);
  free(pcm); pcm = NULL; pcm_frames = 0; playing = 0;
  LONG generation = InterlockedIncrement(&decode_generation);
  LeaveCriticalSection(&pcm_lock);
#ifdef _WIN32
  unsigned slot=64;
  for(unsigned i=0;i<64;i++){
    if(decoder_threads[i]&&WaitForSingleObject(decoder_threads[i],0)==WAIT_OBJECT_0){CloseHandle(decoder_threads[i]);decoder_threads[i]=NULL;}
    if(!decoder_threads[i]&&slot==64)slot=i;
  }
  if(slot==64)return 0;
  decoder_threads[slot]=CreateThread(NULL,0,decode_worker,(LPVOID)(LONG_PTR)generation,0,NULL);
  if(!decoder_threads[slot])return 0;
#else
  {
    pthread_t thread;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    struct { LONG gen; } *dw_arg = malloc(sizeof *dw_arg);
    if (!dw_arg) { pthread_attr_destroy(&attr); return 0; }
    dw_arg->gen = generation;
    extern void* music_decode_thread(void*);
    if (pthread_create(&thread, &attr, music_decode_thread, dw_arg) != 0) {
      free(dw_arg);
      pthread_attr_destroy(&attr);
      return 0;
    }
    pthread_attr_destroy(&attr);
  }
#endif
  fprintf(stderr, "[music] custom track for %s: %s\n", name ? name : stem, pick->path);
  return 1;
}

/* lbAudioAx_80023F28(track id): the game starting a background track. */
/* Replacing a track means not starting the disc's own, which is a different path
 * through the guest than a player without that file takes. Outside a netplay
 * session that is harmless, but during one it would mean two machines running
 * different code, so custom music stands aside and everyone hears the disc.
 * lbAudioAx_800236DC is the stream stop (AXDriverStop). */
#define FN_BGM_STOP 0x800236DCu
extern int netplay_session_active(void);
static int stopping_for_custom;

static void stop_disc_track(Context* ctx) {
  RecFn f = lookup_function(FN_BGM_STOP);
  if (lookup_is_stub(f)) return;
  stopping_for_custom = 1;
  Context saved = *ctx;
  f(ctx);
  uint64_t tb = ctx->timebase;
  *ctx = saved;
  ctx->timebase = tb;
  stopping_for_custom = 0;
}

void music_silence_disc(Context* ctx) { (void)ctx; }

void music_bgm_play(Context* ctx) {
  extern int netplay_replaying(void);
  if (netplay_replaying()) { func_80023F28(ctx); return; }
  unsigned id = ctx->gpr[3];
  char file[64], stem[64];
  playing = 0;
  if (hps_file(ctx, id, file, sizeof file)) {
    stem_of(file, stem, sizeof stem);
    const char* name = display_name(stem);
    if (!netplay_session_active() && choose_custom(stem, name)) {
      stop_disc_track(ctx);
      playing = 1;
      aurora_link_music_bar(now_playing, 1); bar_frames = 480;
      fprintf(stderr, "[music] track %02X %s -> custom: %s\n", id, file, now_playing);
      ctx->gpr[3] = 1;          /* the disc track is not started */
      return;
    }
    if (netplay_session_active()) playing = 0;   /* no substitution during a match */
    snprintf(now_playing, sizeof now_playing, "%s", name ? name : stem);
    if (is_stage_track(stem)) { aurora_link_music_bar(now_playing, 0); bar_frames = 480; }
    fprintf(stderr, "[music] track %02X %s -> %s (bar %s)\n", id, file, now_playing,
            is_stage_track(stem) ? "yes" : "no");
  }
  func_80023F28(ctx);
}

/* lbAudioAx_80023694(): the game stopping the background track. */
void music_bgm_stop(void) {
  extern int netplay_replaying(void);
  if (netplay_replaying()) return;
  if (stopping_for_custom) return;      /* our own stop, not the game's */
  EnterCriticalSection(&pcm_lock);
  InterlockedIncrement(&decode_generation);   /* a decode still running is now stale */
  playing = 0;
  free(pcm); pcm = NULL; pcm_frames = 0;
  now_playing[0] = 0;
  LeaveCriticalSection(&pcm_lock);
}

/* Mixed into the host output beside the game's own audio (audio.c). */
void music_mix(short* out, unsigned frames, unsigned rate) {
  if (!playing || !pcm || !pcm_frames || !rate) return;
  EnterCriticalSection(&pcm_lock);
  if (!playing || !pcm || !pcm_frames) { LeaveCriticalSection(&pcm_lock); return; }
  double step = (double)pcm_rate / (double)rate;
  for (unsigned i = 0; i < frames; i++) {
    unsigned at = (unsigned)cursor;
    if (at >= pcm_frames) { cursor = 0; at = 0; }
    unsigned next = at + 1 < pcm_frames ? at + 1 : 0;
    double blend = cursor - (double)at;
    for (unsigned c = 0; c < 2; c++) {
      unsigned channel = pcm_channels == 1 ? 0 : c;
      double a = pcm[(size_t)at * pcm_channels + channel];
      double b = pcm[(size_t)next * pcm_channels + channel];
      int value = out[i * 2 + c] + (int)((a + (b - a) * blend) * MUSIC_GAIN);
      out[i * 2 + c] = (short)(value > 32767 ? 32767 : value < -32768 ? -32768 : value);
    }
    cursor += step;
  }
  LeaveCriticalSection(&pcm_lock);
}

void music_frame(void) {
  if (bar_frames && --bar_frames == 0) aurora_link_music_bar(NULL, 0);
}

void music_release(void){
 InterlockedIncrement(&decode_generation);
#ifdef _WIN32
 for(unsigned i=0;i<64;i++)if(decoder_threads[i]){WaitForSingleObject(decoder_threads[i],INFINITE);CloseHandle(decoder_threads[i]);decoder_threads[i]=NULL;}
#endif
 free(pcm);pcm=NULL;DeleteCriticalSection(&pcm_lock);
#ifdef _WIN32
 if(media_foundation_ready)MFShutdown();
#endif
}
