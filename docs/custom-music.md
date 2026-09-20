# Custom music

Put tracks in a folder under `user/mods/music/stage`. The runtime first looks for the current track's friendly name, then its original HPS filename without `.hps`. For example:

```text
user/mods/music/stage/Corneria/My song.mp3
user/mods/music/stage/corneria/Another song.flac
user/mods/music/stage/izumi/Fountain theme.wav
user/mods/music/stage/saria/My alternate Great Bay song.wav
```

Use one naming style for a track: a nonempty friendly-name folder takes precedence over the HPS-stem folder. Match the HPS stem when a track has no friendly-name mapping. Folder contents are scanned when the game starts that music track, so restart the track or leave and re-enter the stage after adding files. Filenames become the displayed song titles. Up to 64 candidate files are scanned; subfolders are not scanned. A time-based choice selects one candidate when music starts, and that track loops. There is no playlist editor or sequential shuffle queue.

On Windows, decoding uses Media Foundation. MP3, M4A/AAC, WMA, FLAC and WAV depend on the installed Windows decoders; Ogg requires a compatible decoder. Linux builds use the system FFmpeg libraries and their available codecs, converting audio to signed 16-bit PCM. The built-in fallback supports PCM WAV, mono/stereo, 8- or 16-bit samples, 8-96 kHz. Stereo 16-bit PCM WAV is a useful troubleshooting format. Files are decoded asynchronously with a 34,560,000-frame limit (12 minutes at 48 kHz; duration varies with sample rate). A decode failure is logged rather than converted automatically.

Custom playback defaults to 60% gain relative to full-scale PCM. Set `MELEE_MUSIC_GAIN` to a percentage from 1 to 200 before launch to override it. Overall volume remains in F1 settings. Avoid clipping in the source audio. Text, Markdown, INI and JSON files are ignored in track folders.

**Online sessions use the original game music.** Substituting local files currently changes the guest's audio path, so the runtime disables substitution during synchronized play. A custom music folder is local preference data, not an Online costume package, and is never downloaded with a room.

Do not place music, HPS files or other game data in the source repository. Keep personal music in `user/mods/music`, which is ignored by Git. Share only files you are entitled to distribute.

## Latest native check

The Great Bay alternate track now visibly displays **Saria's Song**. An isolated `saria/YAMPP Test Tone.wav` fixture was selected, decoded as stereo 32 kHz / 96,000 frames, and displayed its filename in the actual gameplay banner. Evidence: `build/performance/saria-label-final-ui` and `saria-custom-final-v3`. Two earlier custom-music attempts stopped at character select and are excluded. These checks establish file selection/decoding and visible labeling, not physical speaker output.
