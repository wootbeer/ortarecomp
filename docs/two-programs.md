# Two programs: Orta and the original Panzer Dragoon

The Orta disc holds two programs: `default.xbe` (Orta) and `pd1.xbe`, the
original Panzer Dragoon that Pandora's Box launches (APPENDIX > PANZER
DRAGOON). Both are recompiled, separately, and link into one executable.
Facts about pd1 itself are in `docs/orta-facts.md`.

## How they fit in one executable

| | Orta | pd1 |
|---|---|---|
| generate | `tools/recomp.py X` | `tools/recomp.py --program pd1 X` |
| work / generated C | `build/recomp-work/`, `gen/` | `build/recomp-work-pd1/`, `gen-pd1/` |
| shared XDK/middleware hooks | `src/hle/` (Orta's addresses) | `src/hle/` again, through `src/pd1/orta_hooks.h` |
| its own game code | `src/guest/` | `src/pd1/` (pd1's addresses) |
| seeds | `tools/pdo_seeds.json` | `tools/pd1_seeds.json` |
| names header (force-included) | `src/guest/orta_names.h` | `gen-pd1/pd1_names.h` |
| CMake target | `pdo_recomp` | `pd1_recomp` + `pd1_hle` (only when `gen-pd1/` exists) |
| run alone | `--boot` | `--boot --program pd1` |

- **Names.** Both programs define `sub_XXXXXXXX`, `recomp_lookup` and the
  rest. pd1's names header renames every global its generated code defines
  to `pd1_...` (written by `recomp.py` from the generated code), so pd1's
  sources keep the usual spelling: `void sub_001A64E3(void)` in `src/pd1/`
  is pd1's function at that address and links as `pd1_sub_001A64E3`.
  Orta's header renames only its dispatch (`orta_recomp_lookup`...). The
  unprefixed `recomp_lookup` and `recomp_lookup_manual`, which the kernel
  layer calls, are `src/guest/program.c`'s: they go to the program running.
- **Host code** (`pdo_host`: renderer, overlay, mixer, vertex programs,
  crash report) is compiled once and serves both.
- **Shared hooks** (`src/hle/`: Direct3D, DirectSound, CRI, input, the wait
  and call tracers) are written once, with Orta's addresses, and compiled
  once per program. For pd1, `src/pd1/orta_hooks.h` maps every
  `sub_XXXXXXXX` there to pd1's function and `src/pd1/hle_names.h` gives
  the globals `src/hle/` defines pd1 names (force-included into all of
  pd1's code, so `src/pd1/` calls pd1's copy). Data addresses are written
  `PROG_VA(orta, pd1)` (`src/hle/program_va.h`); 0 for pd1 marks Orta's own
  game code (a caller check), which then never matches.
- **`tools/program_map.py ORTA.xbe PD1.xbe`** writes both headers from the
  two XBEs, and refuses on anything doubtful: a hook is mapped only to an
  identical function (xbe_match.py) or to the one place in pd1 whose code
  matches with addresses masked and whose addresses all correspond (data
  used alike, the same kernel import, mapped callees); look-alikes it cannot
  tell apart, two hooks on one pd1 function, or a `PROG_VA` value identical
  code does not confirm stop it. Hooks pd1 does not have are listed in its
  `ORTA_ONLY` with the evidence. `--check` verifies without writing.
- **Memory.** Both load at their own addresses into the same guest space,
  one at a time. pd1's image reaches 0x00E60B20, over xboxrecomp's fixed
  kernel data (0x00740000) and stacks (0x00780000): patch 0034 moves both,
  and the heap after them, above the image when it is that big (pd1: kernel
  data 0x00E70000, stack 0x00EB0000, heap 0x016B0000, 41 MB). Orta keeps
  its addresses.

## Working on one program without touching the other

- **Regenerate one**: `recomp.py --program pd1` rewrites only `gen-pd1/`
  (and Orta's only `gen/`). Unchanged chunk files are not rewritten, so
  Ninja recompiles only what changed.
- **Compile one**: `cmake --build build --target pd1_recomp` (or
  `pdo_recomp`) to check a change without linking.
- **Add a hook**: a pd1-only hook goes in `src/pd1/` with pd1's address,
  then `recomp.py --program pd1`. An Orta-only one goes in `src/guest/`.
  Orta's generated code is not affected by pd1's and the other way round.
- **Fix or add an XDK/middleware hook** (D3D, DSound, CRI, input): in
  `src/hle/`, with Orta's address; both programs get it. A new hook, a new
  data address (`PROG_VA`) or a new global there: run
  `tools/program_map.py`, then `recomp.py` for both programs. A fix inside
  an existing hook needs only a build.
- **Run one**: `--boot --program pd1` starts pd1 directly, without going
  through Orta and Pandora's Box. The log says `program: pd1 (pd1.xbe)`.
  The testing switches in `docs/STATE.md` (`PDO_NO_DRAW`, `--log`...) work
  the same.
- **What Orta's hooks become in pd1**: `tools/xbe_match.py` compares any two
  XBEs; with Orta in the second place it reads our own `src/` as "the port":
  ```
  python tools/xbe_match.py pd1.xbe default.xbe --col . --work build/match-pd1
  ```
  `build/match-pd1/report.md` lists each Orta hook/override with its pd1
  address (column "Orta" there is pd1). First run: 193 of 222 found, 3830
  identical functions in all; D3D 250/346, DSOUND 479/566, XPP 183/188,
  WMADEC 138/195 identical. Both are XDK 4928 QFE 1.

## Status

- pd1 generates (about 8 minutes), with 208 of Orta's 213 shared hooks
  mapped (181 identical functions, 27 checked code matches; the other 5:
  three trace-only, `DownloadEffectsImage` and `SetMixBinVolumes`, which
  pd1 lacks).
- `--boot --program pd1` with the disc's `p1media`: Direct3D, DirectSound,
  CRI and input through `src/hle/`; boots at 60 fps through the opening
  narration and the opening movie (ALL_OP.SFD) with its subtitles. No
  failed indirect call.
- pd1 draws its frames in software (one 640x480 picture a frame), so its
  bugs are in recompiled game code, not in the renderer. The HUD gauge and
  radar were drawn off by up to 128 pixels when high: `cwd` was not lifted
  (patch 0035), found by tracing the HUD's polygons in an F12 capture
  (`src/pd1/trace.c`) and a watchpoint on the gauge's pixels. Confirmed
  fixed on Windows. Its far sea, distance fades and see-through radar came
  out solid (issue 7): its span routines draw them through a dither mask
  they `ror` a bit per pixel, skipping the pixel on CF, and rotates never
  set CF (patch 0038). Found by making the band's pixels read-only for one
  frame and logging the writers (a local SIGSEGV/trap-flag watch, not kept).
  The frame itself is composed in Direct3D from one 640x1440 A1R5G5B5
  texture (three 640x480 layers) in about six blended passes.
- pd1-only so far: its frame limiter (`src/pd1/frame.c`; it counts vblanks
  with its vertical-blank callback, which `src/hle/d3d_hle.c` now runs once
  per vblank from its waits, as the console's interrupt does) and two seeds.

## Next

1. **Play pd1 through** (needs a person: input, sound by ear, the stages):
   expect pd1's own game code issues, not the XDK's.
2. **Display > Movies other than Original** (Bars, Stretch, Zoom) do not
   apply to pd1's movies yet; Original, the default, is the game's own
   frame and fine. They need the movie picture marked: Orta's checks
   (`d3d_hle.c` `mark_movie`, draw returning to 0x000688E3; `dsound_hle.c`
   movie open at 0x000687E9) are 0 for pd1. pd1 starts every movie through
   0x00124D99 (cdecl, the .sfd name: an `_enter` hook calling
   `pdo_movie_opened` gives the picture size), but draws every 2D quad, the
   movie's among them, from one list renderer (0x001B6AE0, through the
   draw routine 0x001A8500), so the movie quad needs another tell than the
   caller (its texture, CRI's frame).
3. **Switch in-process** (done for now by restarting; Android wants
   in-process). Today: the kernel calls `xbox_relaunch_hook` at
   HalReturnToFirmware (patch 0037); `src/guest/relaunch.c` reads the launch
   data page XLaunchNewImage filled (path at +8), and for `default.xbe` or
   `pd1.xbe` this build has, saves the 4 KB page to `launch_data.bin` in the
   data folder and starts OrtaRecomp again with `--boot --program NAME
   --launch-data FILE` (Linux: `execv` of /proc/self/exe, the same process;
   Windows: CreateProcess, then exit; Android: `RelaunchActivity` in a
   process of its own starts the activity again once the game's process
   has ended); the new run appends to the log and
   puts the page back (`pdo_relaunch_load`) where XGetLaunchInfo reads it.
   Pandora's Box's PANZER DRAGOON leaves Orta at 0x000120D0 with
   `d:\pd1.xbe`; pd1's title EXIT asks for `\Device\CdRom0;default.xbe`
   (launch data type 0, title 53450007). Tested pd1 -> Orta here (Orta then
   stops for lack of its data in this container); Orta -> pd1 needs the
   disc's files. In-process would replace only `restart_on`: stop the
   guest threads (the guest CPU lock is the choke point), reset the kernel
   (timers, interrupts, objects, handles, heap) and our caches keyed by
   guest addresses, remap from the other XBE.
