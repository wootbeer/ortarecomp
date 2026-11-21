# Current state

v0.1.16 (beta, Windows and Linux/Steam Deck; Android a preview, `android/`): Panzer Dragoon Orta plays
through all ten episodes and the ending, with music, movies, saves, bosses
and Pandora's Box, the original Panzer Dragoon included. Changes are
listed in `CHANGELOG.md`.
History and detail: `docs/archive/state-to-v0.1.md` (to the release) and
`docs/archive/state-bring-up.md`; facts about the game: `docs/orta-facts.md`.

## How it fits together

- `tools/recomp.py`: the USA `default.xbe` -> `gen/` through xboxrecomp
  v0.13.1 and patches 0001-0045 (`patches/xboxrecomp/`), with seeds for
  functions the sweep misses (`tools/pdo_seeds.json`; every failed indirect
  call seen in play is seeded). 42 lifter jumps still go to stubs, all from
  data decoded as code.
- Two programs in one executable: Orta and pd1 (Open 7), each with its own
  generated folder, sources and CMake target (`docs/two-programs.md`).
- `src/hle/` (shared with pd1) and `src/guest/`: Direct3D, DirectSound, CRI (movies) and input in place of
  the Xbox's (`d3d_hle.c`, `dsound_hle.c`, `cri_hle.c`, `input_hle.c`),
  vertex programs compiled for the CPU (`vsh.c`), option hooks
  (`widescreen.c`, `unlocks.c`, `intro_skip.c`), crash and hang reports
  (`crash_report.c`, `wait_trace.c`).
- `src/gfx/renderer.c`: the game records frames on its thread; the main
  thread owns the window and draws them on the SDL GPU API (Vulkan), with
  vertex programs and fixed-function vertices on the GPU. Each vertex
  program and each fragment setup becomes its own shader (`vsh_spec.c`,
  glslang at run time, cached in `shaders/`), their pipelines made in the
  background by three threads; the driver's compiled pipelines are kept
  in `pipelines.vkcache`. Until a program's is made it is drawn from the
  CPU's vertices. On a slow driver (Turnip, or any whose first pipeline
  takes over 50 ms) draws wait for their own pipelines (`fs_wait`) instead
  of the interpreters' (seconds each there). Frame interpolation pairs the
  draws of the last two frames (`match_frames`, on a thread of its own)
  and blends them; vertex programs run once a game frame on compute (the
  vertex pre-pass, `vout_prepass`; `PDO_VOUT=0` turns it off), the draws
  blending their outputs. See the archive for what is and is not blended.
  Movies (Display > Movies, Original by default): `d3d_hle.c` marks a
  movie's picture (its size from the .sfd), the window pass shows it alone
  with subtitles laid over its bottom (`movie_only`).
- Widescreen (`widescreen.c`): 16:9 is the game's own mode; 21:9 widens
  its view after the display setup, 16:9 <-> 21:9 mid-game from Swap, 4:3
  <-> wide at the next start. In `d3d_hle.c` the HUD routine's sprites are
  squeezed back (`hud_squeeze`; HUD Edges sends corner pieces to the
  sides, but not with the cutscene bars or 20 frames of a full-screen
  backdrop, `hud_menu`: lightning flashes last 11-14) and the cutscene
  bars are dropped (`cutscene_bars`). The log says when Edges stops and
  why, and what became of the bars. Fit Zoom crops the HUD with the rest.
  Rumble is passed through (XInputSetState).
- Lossless Scaling frame generation (experimental): `lib/lsfg` (lsfg-vk and
  DXVK's DXBC compiler, from the BattleTanx port), run on SDL's own device
  through `patches/SDL/0001` (local, applied by CMake), host side in
  `src/gfx/lsfg_present.cpp`; needs SPIR-V headers to build and the
  player's Lossless.dll to run. F1 > Advanced > Frame gen.
- `src/audio/`: mixer, I3DL2 reverb, output Stereo / Headphones / 5.1 (the
  six speaker bins out as they are, 3D voices panned around the corners).
  3D voices go through the game's own HRTF tables, on 5.1 shared between
  the front and back pairs as DirectSound does (Sound > 3D sound; Off: a
  pan, the model head on Headphones, the corners on 5.1). With 5.1, GetSpeakerConfig reports a Dolby Digital console
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
   two slow seconds. Pairing runs on its own thread; on the RP6 it is
   136-191 ms a second at 120 Hz.
2. A full play on the release build with no `[ICALL] Failed` in the log.
   The title's main thread runs without the guest CPU lock (run inline by
   PsCreateSystemThreadEx, the lock given up by that kernel call). Mad Dash
   Recomp makes it take the lock (its own patch 0045); not taken here: Orta plays
   through as it is, and it would serialise the main thread with the sound,
   movie and file threads (speed, and hangs where a loop waits without a
   kernel call). Its 0044 (IO status order) is in.
3. First-time pipeline stalls (kept in `pipelines.bin` after); programs'
   own are made in the background; the driver's own compile is kept in
   `pipelines.vkcache` (Vulkan pipeline cache, `patches/SDL/0002`).
   Issue 10 (dirty disc at the episode 7 boss after a long session): heap
   overflow area (patch 0041), to confirm in a full play on Windows.
4. Steam Deck movies at 20 fps (issue 11). On Windows the same was MSVC
   leaving the huge generated functions (the movie decoder) unoptimised,
   fixed with /d2OptimizeHugeFunctions (v0.1.6). The Deck builds with GCC,
   which has no such limit: if it persists, F11 during a movie shows which
   thread is busy.
5. Android (`android/`, `android/PLAN.md`): touch controls, pd1, other
   GPUs' fallbacks and a custom GPU driver (Turnip, libadrenotools) are in;
   on the RP6 with Turnip, 4x with 4x MSAA holds 60 fps, and interpolation
   at 120 Hz mostly holds 120 (the vertex pre-pass, PDO_VOUT). Lossless Scaling
   was tried and taken out (too slow on Adreno). The game file picker now
   draws in the launcher's window. Released with v0.1.13 (a preview). Next: which of the
   Android renderer work helps on PC (setups' own fragment shaders,
   background pipelines, the CPU stand-in), and later Concept of Love.
6. Gunfire (issue 15): the mixbins' -6 dB headroom, the reverb send by
   distance and the HRTF filters (taps / 512, chosen by ear against the
   console) are in; up to 12 machine-gun copies at once are the game's own.
   Not reproduced: the dsstdfx.bin effects on the 3D mixbins.
7. The original Panzer Dragoon (Pandora's Box, `pd1.xbe`): a second
   program, generated on its own into `gen-pd1/` (`recomp.py --program
   pd1`), linked in beside Orta, run alone with `--boot --program pd1`.
   Shares Orta's D3D/DSound/CRI/input hooks (`src/hle/`, mapped by
   `tools/program_map.py`); playable. Pandora's Box launches it and its
   title's EXIT returns, by restarting OrtaRecomp with the launch data
   (`src/guest/relaunch.c`, patch 0037). Next: pd1 play-testing; an
   in-process switch (for Android). All in `docs/two-programs.md`.
8. Wishlist: the European (PAL) disc. Only the USA default.xbe is
   recognised now (its hash in `src/xbe.c`, `tools/recomp.py`); PAL is a
   different XBE, so its own recompile, addresses and hooks. Asked for by
   a player; the owner has the ISO.
   Dual-stick aiming, as an option: the right stick turning the view all
   the way round and the left moving, as modern games do. The camera
   spring in 0x0007B5D0 can be driven directly (`docs/orta-facts.md`,
   Input and the reticle); taking its damping out made the camera follow
   the aim at once, which is the start of it. Needs the yaw's 360 wrap
   handled (it shook) and a choice of how the game's own camera moves
   with it.

## Testing

- Cloud: `PDO_NO_DRAW=1 PDO_FAST=4`, `PDO_SCREENSHOT=dir`, `PDO_CAPTURE=frame`
  (+ `PDO_CAPTURE_INTERP`, `_SEQ`), `PDO_INPUT_SCRIPT` for buttons (the
  standard script reaches episode 1 by frame ~11000). Interpolation never
  runs offscreen.
- Windows: build from a Developer PowerShell; `--log run.log`, not
  PowerShell redirection. F12 captures a frame, F11 dumps threads.
