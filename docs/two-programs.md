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
| its own sources (overrides, hooks) | `src/guest/` (all `src/` but `src/pd1/`) | `src/pd1/` |
| seeds | `tools/pdo_seeds.json` | `tools/pd1_seeds.json` |
| names header (force-included) | `src/guest/orta_names.h` | `gen-pd1/pd1_names.h` |
| CMake target | `pdo_recomp` | `pd1_recomp` (only when `gen-pd1/` exists) |
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
- **Add a hook**: a pd1 hook goes in `src/pd1/` with pd1's address, then
  `recomp.py --program pd1`. Orta's generated code is not affected and
  needs no regeneration, and the other way round.
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

- pd1 generates (16060 functions, about 8 minutes), compiles and links.
- `--boot --program pd1` with only the XBEs present: CRT start, threads,
  XInput's USB stack, then Direct3D on the guest's own code (no HLE yet:
  `out dx, al` and `wbinvd` reached as no-ops) and stops opening
  `D:\P1MEDIA\TNRoman.bin`, its first data file. One indirect call failed:
  0x002723C7 (in XPP; a seed candidate, or an input hook as in Orta).

## Next

1. **Share Orta's library hooks with pd1** rather than copying them. Most of
   `d3d_hle.c`, `dsound_hle.c`, `input_hle.c`, `cri_hle.c` and `vsh.c` hook
   XDK and CRI code identical in both. Plan: compile those files a second
   time into `pd1_recomp`, with a header mapping each Orta hook name to its
   pd1 counterpart (`#define sub_002AF710 sub_0021B...`, generated from the
   match report and committed: addresses only), and data addresses
   (`D3D__pDevice`, `D3D__RenderState`...) taken from a per-program header
   instead of literals. `recomp.py --program pd1` applies the same map when
   it scans for overrides. A hook with no pd1 counterpart must stop the
   generation with an error rather than silently hook whatever pd1 has at
   Orta's address. Game-specific code (HUD squeeze, cutscene bars,
   widescreen, intro skip, unlocks, launch guard, caller-address checks
   such as `0x0006A24B`) moves to Orta-only files first. Then a fix in a
   shared hook reaches both programs, and each file still builds per
   program.
2. **Boot pd1 alone to its title** with the disc's files, on (1).
3. **Switch in-process.** Orta's launch path (0x000120D0, then
   XLaunchNewImageA 0x0022CE3C with `d:\pd1.xbe`) ends the program: leave
   the title thread back to `pdo_guest_run`, stop the other guest threads
   (a reboot generation the guest CPU lock checks), reset the kernel
   objects, handles and heap, remap the address space from pd1.xbe with the
   launch data page kept, `pdo_program_select(PDO_PROGRAM_PD1)`, enter at
   0x001A64E3. pd1 leaves with `d:\default.xbe`; check whether Orta reads
   the launch data to return to Pandora's Box. Fallback if the soft reboot
   is too fragile: restart the process with `--program`.
