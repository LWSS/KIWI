# Miles Sound System 9.3b (RAD Game Tools, 12-Dec-2012)

Replaces the Miles 7.2e drop that KIWI originally shipped (retail CoD4 era).
Source install: `C:\Miles9` (full SDK incl. `src/sdk`, examples, tools, pdbs, .rar).
That external install is not required to build or run KIWI; the build uses this directory.
Only what the engine needs is vendored here:

| path | contents |
|---|---|
| `mss.h`, `rrcore.h` | public API header + RAD core types (mss.h includes rrCore.h) |
| `mss32.lib` | x86 import lib for `mss32.dll` (linked by scripts/pre_build.cmake) |
| `dlls/mss32.dll` | x86 runtime, copied next to the exe by scripts/post_build.cmake |
| `dlls/miles/mssmp3.asi` | MP3 decoder. In Miles 9 MP3 is a separate add-on (not in mss32.dll); CoD4 streams music/ambience as .mp3, so without it AIL_open_stream fails with "Error getting sound format". Source: milesss-v9.3b `win/mp3/redist`. |
| `dlls/miles/*.flt` | stock x86 pipeline filters (dolby/ds3d/dsp/eax/srs) — KIWI opens none of them today, kept so the redist dir layout matches AIL_set_redist_directory("miles") |
| `x64/` | mss64.lib / mss64.dll / 64-bit .flt set + mss64mp3.asi. NOT wired into the build yet (x64 port is a separate task). |
| `help/*.chm` | API reference + user guide |

Dropped vs 7.2e: `mssvoice.asi` (unused),
`milesEq.flt` (custom "3 Band Parm Eq" — removed from the tree, never worked).

API deltas that touched KIWI code (all in src/sound): `AIL_set_room_type` /
`AIL_set_digital_master_reverb_levels` take a `bus_index`, `AIL_set_DirectSound_HWND` became
`AIL_platform_property(dig, WIN32_HWND, ...)`, `HPROENUM/HPROENUM_FIRST` became
`HMSSENUM/MSS_FIRST`, and the `FAR` macro no longer exists.

Integration fixes validated by user playback on 2026-09-06:
- `SND_SetHWND` supplies the mutex scope missing from Miles 9's `WIN32_HWND`
  property setter; without it, the mixer stalls.
- `MSS_SetMixerPreferences` in `src/sound/snd_mss.cpp` targets 24 ms of mix-ahead.
  DirectSound rounds fragments to 256 frames: 1/2/4 fragments at
  11025/22050/44100 Hz give 23.22 ms. Both driver-open paths apply the setting.
  This is the Miles queue duration, not measured end-to-end output latency.
