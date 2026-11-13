# Current state

v0.1.5 (beta, Windows and Linux/Steam Deck): Panzer Dragoon Orta plays
through all ten episodes and the ending, with music, movies, saves, bosses
and Pandora's Box. Changes are listed in `CHANGELOG.md`.
History and detail: `docs/archive/state-to-v0.1.md` (to the release) and
`docs/archive/state-bring-up.md`; facts about the game: `docs/orta-facts.md`.

## How it fits together

- `tools/recomp.py`: the USA `default.xbe` -> `gen/` through xboxrecomp
  v0.13.1 and patches 0001-0033 (`patches/xboxrecomp/`), with seeds for
  functions the sweep misses (`tools/pdo_seeds.json`; every failed indirect
  call seen in play is seeded). 42 lifter jumps still go to stubs, all from
  data decoded as code.
- `src/guest/`: Direct3D, DirectSound, CRI (movies) and input in place of
  the Xbox's (`d3d_hle.c`, `dsound_hle.c`, `cri_hle.c`, `input_hle.c`),
  vertex programs compiled for the CPU (`vsh.c`), option hooks
  (`widescreen.c`, `unlocks.c`, `intro_skip.c`), crash and hang reports
  (`crash_report.c`, `wait_trace.c`).
- `src/gfx/renderer.c`: the game records frames on its thread; the main
  thread owns the window and draws them on the SDL GPU API (Vulkan), with
  vertex programs and fixed-function vertices on the GPU. Frame
  interpolation pairs the draws of the last two frames (`match_frames`) and
  blends them; see the archive for what is and is not blended, and why.
- Lossless Scaling frame generation (experimental): `lib/lsfg` (lsfg-vk and
  DXVK's DXBC compiler, from the BattleTanx port), run on SDL's own device
  through `patches/SDL/0001` (local, applied by CMake), host side in
  `src/gfx/lsfg_present.cpp`; needs SPIR-V headers to build and the
  player's Lossless.dll to run. F1 > Advanced > Frame gen.
- `src/audio/`: mixer, I3DL2 reverb, output Stereo / Headphones (a model
  head) / 5.1 (the six speaker bins out as they are, 3D voices panned around
  the corners). With 5.1, GetSpeakerConfig reports a Dolby Digital console
  and movies with an .aix (opening_bs, ins1_bs, ins2_bs: three stereo ADX
  streams) play it: CRI routes a buffer per speaker pair, and we write our
  own decoding (`src/audio/aix.c`) over its chunks, since CRI's fills only
  some channels under us (`dsound_hle.c`, "5.1 movie soundtracks").
- `src/gfx/overlay.c`: the launcher and the F1 menu; settings in
  `ortarecomp.ini` (`src/config.c`); save files 1-3 are `saves/E`, `E2`,
  `E3`.

## Release

`tools/package.py` builds `build-release/` (no console, static C runtime,
icon from `tools/make_icon.py`) into `dist/`: a zip on Windows, a tar.gz on
Linux (stripped; needs only libc; build on Ubuntu 22.04 for SteamOS). Release builds
always log to `ortarecomp.log` in the data folder (the run before kept as
`ortarecomp-previous.log`); a crash (Windows: exception filter and a
message; Linux: fatal-signal handler) is reported there with executable
offsets and the nearest game function (`sub_XXXXXXXX`), no symbols needed.

## Open

1. Frame interpolation: the main source of reports to expect (beta). A
   frame's pairing logs its stages when over 8 ms; blending holds off after
   two slow seconds. On weaker PCs, next: reuse group distances in the
   wrong-pair check, lazy bounds.
2. A full play on the release build with no `[ICALL] Failed` in the log.
3. First-time pipeline stalls (kept in `pipelines.bin` after).
4. Android (Vulkan/Turnip): measure the recompiled code and the guest CPU
   lock on ARM; Linux's WaitForMultipleObjects polls with sleeps.
5. Gunfire sounds stacked and loud (issue 15). Fixed so far: the mixbins'
   -6 dB headroom and the reverb send following distance. Left: up to 12
   machine-gun copies play at once, started by the sound library at
   0x225417 (`PDO_TEST_NEWEST=1` keeps only the newest and sounds closer to
   the Xbox); find how often it really starts them. HRTF filters (Headphones
   output) are not used: their tap scale is unknown (`docs/orta-facts.md`).
6. Wishlist: the European (PAL) disc. Only the USA default.xbe is
   recognised now (its hash in `src/xbe.c`, `tools/recomp.py`); PAL is a
   different XBE, so its own recompile, addresses and hooks. Asked for by
   a player; the owner has the ISO.
7. Wishlist (issue 13): ultrawide for movies and real-time cutscenes, both
   letterboxed by the game. Movies: find the bars in the picture and zoom
   on wide windows without cutting subtitles. Cutscenes: the game draws the
   bars itself, so its cutscene state has to be found first. Gameplay
   ultrawide is not planned. Rumble is passed through (XInputSetState); the
   log says when the game first sends it.

## Testing

- Cloud: `PDO_NO_DRAW=1 PDO_FAST=4`, `PDO_SCREENSHOT=dir`, `PDO_CAPTURE=frame`
  (+ `PDO_CAPTURE_INTERP`, `_SEQ`), `PDO_INPUT_SCRIPT` for buttons (the
  standard script reaches episode 1 by frame ~11000). Interpolation never
  runs offscreen.
- Windows: build from a Developer PowerShell; `--log run.log`, not
  PowerShell redirection. F12 captures a frame, F11 dumps threads.
