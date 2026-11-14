# Orta Recomp: notes for Claude sessions

Static recompilation of Panzer Dragoon Orta (Xbox, NTSC-U) on xboxrecomp,
SDL3 throughout so the same code later runs on Android. Built on what
Concept of Love (JSRF, wootbeer/conceptoflove) did: read its
`docs/porting-another-title.md` once for the plan and the lessons.

**Start each session by reading `docs/STATE.md` (one page).** Do not read
`docs/archive/` or `gen/` wholesale: generated C is ~2M lines; grep it for
one function by address or name instead.

## Rules

- **Nothing from the game is ever committed**: no XBE, ISO, extracted files,
  generated C, screenshots, captures, listings, match reports or hashes
  beyond the dump hashes in `src/xbe.c`/`tools/recomp.py`. Players supply
  their own game files. Check `git status` before every commit. No personal
  names, paths or emails in anything committed.
- **Never edit generated C.** Fix the lifter (patch in `patches/xboxrecomp/`,
  to send upstream), the inputs to `tools/recomp.py`, or override a function
  in `src/guest/`. Regeneration must stay deterministic.
- **xboxrecomp patches must not overlap.** `recomp.py` and CMake decide a
  patch is applied with `git apply --reverse --check`, one patch at a time.
  To change lines an earlier patch added, edit that patch instead.
  There are 34 now (0001-0034).
- **Third-party projects** (Cxbx-Reloaded, xemu, the JSRF Decompilation,
  other ports): facts only, no code. Symbol databases and listings are read
  from a local clone at run time, never committed. Our own GPL code from
  Concept of Love may be copied.
- Graphics/audio/input are ours, on SDL3 (GPU API, audio, gamepad);
  xboxrecomp is used for the recompiler and the kernel layer only. No
  NV2A/APU emulation.
- Accuracy over speed; keep fixes minimal and explain them in commit messages.
- Keep `docs/STATE.md` to one page: update it at the end of a session and
  move finished detail into `docs/archive/`. Facts about the game go in
  `docs/orta-facts.md`.

## Overriding game functions

`tools/recomp.py` passes the program's own `.c` files to the recompiler:
Orta's are everything under `src/` except `src/pd1/`; pd1's (`--program
pd1`) are `src/pd1/` (pd1's addresses) and `src/hle/` mapped to pd1 by
`src/pd1/orta_hooks.h`. XDK and middleware hooks go in `src/hle/`, with
Orta's addresses and data as `PROG_VA(orta, pd1)`: both programs get them
(after a new hook, address or global there, run `tools/program_map.py`).
Orta's own game code goes in `src/guest/`. It reads these forms
(literally, at the start of a line):

- `void sub_XXXXXXXX(void)` replaces the function for every caller, direct
  calls included. Declare `extern void sub_XXXXXXXX_gen(void);` to keep the
  original callable (wrapping).
- `void sub_XXXXXXXX_enter(void)` runs at the top of the generated function
  (no trailing comment on that line).

After adding or removing one, rerun `tools/recomp.py` for that program only.
Guest registers are
`g_eax`... and memory is `MEM32(va)` from `gen/recomp_types.h`. Guest ABI:
stdcall, arguments at `g_esp+4+4n`. Name hooks by address; XDK API names in
comments are fine (public API, factual mapping). Put the API name in the
comment just above the function: `tools/xbe_match.py` reads it from there.

## Two programs: Orta and pd1

The disc's second program, the original Panzer Dragoon (`pd1.xbe`, from
Pandora's Box), is recompiled separately into `gen-pd1/` and linked into the
same executable; its names get a `pd1_` prefix through a force-included
header, so its sources still say `sub_XXXXXXXX`. Read
`docs/two-programs.md` before working on it: layout, how to rebuild and run
one program alone, and how Orta's hooks are to be shared with pd1.

## Porting from Concept of Love

Its `src/guest/d3d_hle.c` (ours: `src/hle/`), `dsound_hle.c`, `input_hle.c`, `cri_hle.c` are
tied to JSRF addresses (XDK 4134). Orta is XDK 4928: re-find every function
and table, and re-derive render-state indices from Orta's own setters or
tables, never from a 4134 table. Sources of names, all read locally:
`tools/xbe_match.py` (what Orta shares with JSRF), Cxbx-Reloaded's
XbSymbolDatabase CLI built from a local clone (XDK symbols), disassembly.
Before replacing a function, check its `ret N` and argument use in Orta's
own code.

## Layout

- `tools/xbe_info.py`: identify an XBE (title ID, XDK build, sections)
- `tools/xbe_match.py`: functions shared with JSRF (names, overrides, tables);
  also Orta vs pd1 (`docs/two-programs.md`)
- `tools/recomp.py`: XBE/ISO -> `gen/` (xboxrecomp pipeline); `--program pd1`
  -> `gen-pd1/`
- `tools/program_map.py`: Orta's `src/hle/` hooks onto pd1, checked
- `src/`: launcher, game-file picker, XISO reader, XBE identification
- `src/hle/`: Direct3D, DirectSound, CRI and input in place of the Xbox's,
  shared by both programs
- `src/guest/`: bring-up of the recompiled title and Orta's own game
  hooks; `program.c` picks the program that runs
- `src/pd1/`: pd1's overrides (pd1 addresses)
- `src/gfx/`, `src/audio/`: renderer, launcher/menu overlay, mixer
- `tools/package.py`: release zip; `tools/make_icon.py`: the app icon
- `tools/check_menu_text.py`: run after changing menu labels, values or help
- `external/xboxrecomp`, `external/SDL`: pinned submodules
