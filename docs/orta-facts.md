# Panzer Dragoon Orta: facts

Facts about the game's executable that the port depends on. Addresses are
virtual addresses in the USA `default.xbe`.

## The executable

- Panzer Dragoon ORTA (USA), title ID 53450007 (SE-007), North America,
  retail build, version 1. One dump known (hash in `src/xbe.c`).
- Entry 0x00231E6C, kernel thunk table 0x0030C8A0 (start of `.rdata`).
- **XDK 4928** (QFE 1) for every library: XAPILIB, D3D8, D3DX8, DSOUND,
  XGRAPHC, LIBCMT, LIBCPMT, XBOXKRNL. JSRF is 4134, so D3D render-state and
  texture-stage-state numbering must be re-derived from Orta's setters.

| Section | VA | Size | Notes |
|---|---|---|---|
| .text | 0x00011000 | 2,732,144 | game code, CRT, likely CRI |
| MMATRIX | 0x002AC080 | 6,432 | XDK matrix library |
| D3D | 0x002AD9A0 | 78,224 | Direct3D 8 |
| D3DX | 0x002C0B40 | 1,520 | |
| DSOUND | 0x002C1140 | 126,684 | DirectSound |
| WMADEC | 0x002E0020 | 102,624 | WMA decoder (not in JSRF) |
| PSGSFD00, _I, _B, _P | 0x002F9100.. | ~17,900 | probably CRI Sofdec (SFD movies): to confirm |
| XGRPH | 0x002FD740 | 36,960 | XGraphics (swizzling) |
| XPP | 0x003067A0 | 24,821 | XAPI input (XInput) |
| .rdata | 0x0030C8A0 | 213,060 | |
| APPDATA | 0x00340900 | 1,895,388 | writable data (no `.data` section); flagged executable, excluded from disassembly |
| DOLBY | 0x0050F4E0 | 29,056 | Dolby Digital encoder microcode |
| $$XTIMAGE | 0x00516660 | 10,240 | title image |

## Recompilation (xboxrecomp v0.13.1 + patches 0001-0012)

- The CRT saves MXCSR with `stmxcsr` at boot (0x00269711): patch 0012.
- 16,647 functions, all translated; ~1.78M lines of C in 67 files; 382
  call targets not detected as functions (stubbed).
- APPDATA is data but flagged executable: swept as code it gave 129 junk
  "functions" (no prologues, calling only each other), one of which did not
  compile (`lahf` after `add`, patch 0011). `recomp.py` excludes it (patch
  0010, `--exclude-sections`); its vtables then add ~660 real functions.
- `mov eax, dr0` twice in .text near 0x00211199 (data decoded as code) broke
  the build: patch 0009. Untranslated-instruction sites (`out`, `in`,
  `lcall`, `aas`...) are linear-sweep reads over data, as in JSRF.

## Bring-up findings

- Missed functions are dangerous, not just missing: the recompiler stubs a
  direct-call target it did not detect with an empty function that pops no
  arguments. XInitDevices (0x003068D4, at the start of XPP, reached through
  a jmp thunk at 0x003075AF) was one; its caller's stack shifted 8 bytes
  and a COM call five frames later read a wrong slot as its object.
  `recomp.py` now seeds every direct call target in code sections and
  disassembles again (154 more functions). The 245 stubs left are calls
  decoded from data.
- Game init (0x00011820) creates DirectSound through 0x0021E1E0 (same code
  as JSRF's 0x001680D0) and downloads `dsstdfx.bin` through 0x0021DE70
  (JSRF's 0x00168130); a failure sends it back to the dashboard.
- D3D device fields moved by 0x124 from 4134 (vblank event +0x2554, vblank
  callback +0x2550).

- A function can also start right after the switch jump tables of the one
  before it: 0x00111BC0, reached only through a pointer, follows
  0x00111990's tables. Undetected, every call to it failed from the start
  of episode 2's second boss fight, and the boss was not drawn. `recomp.py`
  seeds such addresses too (6 functions).
- Alias entries (a tail jump into the middle of a function) are lifted as
  their own body from the entry to the enclosing function's end. 224 of
  Orta's had a loop back-edge to before the entry (0x000B7082 loops to
  0x000B7050 in sub_000B6EA0), which became a call to a stub: the loop ran
  once and esp came back 4 bytes off. Patch 0024 makes such targets
  aliases too, until no new one turns up, including from aliases in a gap
  once their end is measured: 0x0015EB10 (vtable 0x003207F8 +0x38, called
  through the thunk 0x0017BA10 as a render callback) was cut at a false
  start 0x0015EBAC after its first ret, and its `jle` to its own
  epilogue 0x0015EBBE skipped `pop esi; pop ecx`. That was the episode 4
  crash.

- Render callbacks: 0x00044830(list, func, arg, key) takes a 0x50-byte
  node (func +0, arg +4, next +8, key +0xC, matrix +0x10) from a pool of
  0x2000 nodes (base 0x419FBC, next free 0x419FB8, used 0x419FB4,
  capacity 0x419FB0; made by 0x00044790, reset every frame by 0x00044800,
  freed by 0x000447D0) without checking the capacity. Nine lists
  (head/tail pairs from 0x419F68) are sorted by key by 0x00044D50 (a
  merge sort, 0x00044970, through a static array at 0x3F8580) and walked
  by 0x00044A20, which keeps the node in esi across each callback. Episode
  1 uses at most about 340 nodes a frame; the pool is allocated once at
  boot (0x000118FD) and freed only at shutdown. The XAPI heap under the
  game's allocator: RtlAllocateHeap 0x00230435, RtlFreeHeap 0x00230BEC
  (both stdcall, 3 arguments), HeapFree wrapper 0x00231597.

- Orta's threads assume one CPU. Run truly in parallel, the sound request
  processor (0x000692C0) walked a list (0x0021E570, 0x00220520) another
  thread was reallocating and called through a stale entry (a crash after
  episode 4). guest.c turns xboxrecomp's guest CPU lock on: one guest
  thread at a time, switching at kernel calls. CRI's spin thread
  (0x00233CB0) loops without a kernel call and is overridden to give the
  CPU up; CRI's region mutex (cri_hle.c) is waited on without it. CRI
  also suspends and resumes its threads around every locked region; under
  the lock that suspension is cooperative (patch 0023), since a host
  SuspendThread froze threads holding the guest CPU or that mutex. Guest
  code the kernel runs itself (file I/O completion APCs, exception
  handlers, DPCs) takes the guest CPU too, and our vblank wait gives it
  up while it sleeps.

## Episode 1 music (checked against a longplay)

- Music keys map to sound numbers through the table at 0x00340F70 (key,
  sound). Episode 1: 0x2777 = stage music (`s01_bgm1`), 0x2774 = boss
  music (`s01_bgm2`), 0x2775 = a short cue started and faded by zone
  triggers 303/304.
- The canyon is silent by design: the ramp event (0xF6AEA, started by
  trigger 204) fades 0x2777 over 600 frames and its completion callback
  0x000D66F0 jumps to segment 6. Trigger 406 at 3500 in that segment's list
  (0x00373B20) creates a fader whose callback 0x000D68A0 requests the boss
  scene 0xF6B58. The scene table at 0x00340DD8 (scene, keys, count) starts
  0x2774 for that scene through 0x00013CC0.
- Requests go through the queue at 0x0046D480 (enqueue 0x00068F60 and
  0x00068FB0, processed by 0x000692C0 into sound manager vtable 0x0032D4A0).
- Orta passes IDs that look like code addresses (scene 0x000F6B58, the
  argument 0x00225574 to 0x001B25B0). The disassembler took 0x00225574 for a
  function and cut the play method 0x00225270 there; every sound played
  through that path left the stack 44 bytes low, and the queue processor then
  dropped the requests after it in the same frame, the boss music among them
  (patch 0022).

## Rendering

- No multisampling (MultiSampleType 0x11, D3DMULTISAMPLE_NONE), a 16-bit
  640x480 back buffer, presentation flags 0x2E. The 3D scene is drawn into
  a 512x384 R5G6B5 target and stretched over the screen by the game;
  on hardware the TV encoder's flicker filter then softens it.

- Episode 2's camouflaged creatures mix 64% of a 128x128 copy of the frame
  (copied from the back buffer each frame) into their texture, through a
  pixel shader with texture mode PROJECT2D; their vertex program writes
  screen coordinates as (x, y, 0, w), so the lookup needs the divide by q.
- Particles are point sprites (states 116-123, set by the reset code at
  0x0027D539): sparks in episode 1 come from a vertex program that writes
  oPts; runs of 15 and 30 points use a 32x32 sprite texture.
- Lights: the sun is light 0; per-object point lights 6-9 (picked by range
  against the object's bounding sphere, 0x0027F450); presets switch
  between the sun and lights 33-35 (episode 2's creatures: sun off, 33 and
  34 on). The device keeps the enabled ones in a list at +0x398.
- Episode 2 water uses bump environment mapping (V8U8 textures, texture
  modes BUMPENVMAP and BUMPENVMAP_LUM).

- Shadow map: each frame starts with a depth-only pass (a color surface
  without memory, 512x512, depth surface LIN_D16 at 0x00718000 in episode
  1, polygon offset 256 16-bit units) and then samples that depth surface
  as a texture on stage 1 of the scene's pixel shaders. The coordinates
  come from the fixed-function texture transform: s, t in texels, r in
  16-bit depth units, q 0. SHADOWFUNC (render state 156, set by
  SetRenderState_ShadowFunc 0x002AFA50, which also sends NV2A method
  0x1E6C the D3DCMP minus 0x200) is GREATER, and the texel is the stored
  depth compared with r/q in that order: lit reads 1. The pixel shader
  takes T1 + 0.5 (1 - T1) and multiplies diffuse and specular by it, so a
  shadow halves the light.

## Compared with JSRF (player notes)

- Not cel-shaded: no inverted-hull outlines to get right.
- Runs well on xemu: use it for reference screenshots and audio.

## Sound

- Every DirectSound buffer is created with no mixbins (DirectSound's
  default routing). 3D buffers (flags 0x40010) are most of the effects;
  the game calls SetI3DL2Source on them with direct 0, room 0 (full reverb
  send), occlusion LF ratio 0.25.
- SetI3DL2Listener: room -10000 (reverb off) through episode 1's stage;
  the I3DL2 Bathroom preset (-1000, -1200, 0, 1.49 s, 0.54, -370, 0.007,
  1030, 0.011, 100, 60, 5000 Hz) from the boss cutscene (event
  st01_ev5b) through the fight. DownloadEffectsImage gets a 30,720-byte
  image each stage; SetEffectData is not called.
- Headroom. A voice's default headroom is 600 mB unless it is 3D (flag
  0x10) or has flags 0x182000 (0x002C40C0), but the game sets 0 on every
  buffer itself. DirectSound's start-up gives mixbins 0-30 a headroom of 1
  (a shift right, -6.02 dB; table filled at 0x002C3B2A, written at
  0x002C67C6 through 0x002C6304 to APU method 0x200 + 4 * bin) and the HRTF
  headroom 0 (0x002C62CD, method 0x280); the game never changes either. Our
  DirectSoundCreate skips that start-up; src/audio/mixer.c halves the
  bins instead. 3D buffers with no mixbins (all of Orta's) go to bins 6, 8,
  7, 9 and 10 (default table at 0x002DF55C; 2D mono/stereo: 0, 1), so the
  headroom covers 3D sound too. xemu applies only the HRTF headroom (0) to
  a 3D voice's four bins ("not sure" in its source), and lists Orta's
  gunfire as too loud in its audio issue (xemu-project/xemu#904): its
  levels, which matched ours without the headroom, are not a reference.
  Without the headroom our dense episode 1 moments reach +10 dBFS before
  the final clamp (92 of 235 s clipping); with it +4.9 dBFS (58 of 234 s).
- DirectSound's 3D voice volumes (0x002C6E40): the voice's mixbin volume
  plus, for a 3D voice, the distance attenuation (0x002C4FA9: 0 inside the
  minimum distance, else -2000 log10(1 + rolloff (d / min - 1)) mB, as
  ours) with, per bin: 6/7 the front share, 8/9 the back share (0x002C520B
  splits a 3D voice between front and back by where it is), 10 the I3DL2
  room level, 2 a centre term when enabled. Left and right get the same
  volume: the APU's per-voice HRTF filter places a 3D voice left or right
  (0x002C7551 uploads 31 taps per ear and an ITD). Tables: 1,111 left/right
  pairs of 32 bytes at 0x002CCFD0, picked through a 61 x 31 index of
  uint16 at 0x002DE590 (rows: azimuth 0-180 in 3 degree steps, mirrored by
  swapping ears; columns: elevation in 6 degree steps; 0x002C5497). The tap
  format and the APU's HRTF scaling are not known: read as two's
  complement / 128 (as xemu does) a frontal filter has about +9 dB of gain,
  as sign-magnitude about +3 dB. Our 3D path pans instead (equal power, -3
  dB each ear straight ahead).
- Enemy machine guns (24,012 and 13,212-byte ADPCM samples, mono 44.1 kHz,
  0.97 s and 0.53 s): each sound has a template buffer; every burst moves
  the template and every copy still playing to the shooter, then starts a
  new copy (Play, looping) there, stopped after about one pass (55 and 30
  frames). Up to 12 copies of one sample play at once from one spot; cones
  are 360/360 and velocities 0, so neither cones nor Doppler separate them.
- Voices: DSOUND allocates hardware voices at Play for DSBCAPS_LOCDEFER
  (0x40000) buffers (0x002C8CBB, allocator 0x002C5DBB) from two pools, 64
  3D voices (slots 0-63) and 192 2D voices (slots 64-255), set at
  0x002C5D3E; when a pool is empty Play fails with 0x88780032. A deferred
  buffer gives its voice back on Stop. We do not cap voices; episode 1's
  busiest second has 24 playing.

## Widescreen

Orta has an anamorphic 16:9 mode. The display setup (0x0003D020) asks its
display object (pointer at 0x0041A06C, query method 0x00281330, id 15) for
the video mode: the low byte (1 or 2) picks the pixel aspect stored at
0x00342724 (1.0 or 0.909), and with bit 8 set it multiplies that by 4/3 and
sets the widescreen flag at 0x00342754, which the camera and the screen
layout read (the HUD's radar is drawn narrow, to be stretched to 16:9).
Our kernel's video flags never set bit 8; src/guest/widescreen.c does when
the aspect setting is 16:9. The game reads it once, at start.

## Input and the reticle

The pad state is copied per player each frame (0x0004F1D0) into blocks of
0x258 bytes at 0x004185A8: +0x00 buttons, +0x20/+0x24 left stick X/Y and
+0x28/+0x2C right stick, as floats -1..1. 0x00077CD0 scales the sticks into
an aiming object (+0x404/+0x408 left, +0x434/+0x438 right; the D-pad nudges
the same values). 0x00033960 records the sticks for demos (4096 entries at
0x0041CE28). The reticle object (pointer at 0x00472030) holds the reticle's
screen position in pixels at +0xB8/+0xBC, recomputed every frame (writing it
does not move the reticle); the state it comes from is not found yet.

## Saves and Pandora's Box

PDOSYSTEM.DAT (16 KB) is a 20-byte signature, then the block the game keeps
at 0x00424E48: "PDOD", size 0x3B8C, version 0x100 (defaults: 0x00033F00).
File offset X is address 0x00424E34 + X. 0x00034120 copies it into the
game's working state and 0x00034340 back: options from 0x00424E78, play
statistics from 0x004255C0 to 0x003427F4 (total play time in frames at
0x003427F8, main game at 0x003427FC; 216000 frames an hour), the stage
records (0xB0 bytes each) from 0x00425604 to 0x00342838, Box unlock bits
from 0x00428994 to 0x00345D90 and Box "seen" bits from 0x004289A0 to
0x00345D9C. The story save (episode at 0x00424EA0, difficulty 0x00424E70,
shown as "EPISODE n" in the save's description) loads only with bit 1 of
0x00034120's argument.

Box item n (347, table of {bit, hours index} at 0x003CE0A8) is open when its
bit is negative, set in 0x00345D90, or when the total play time reaches its
hours (byte table at 0x00346418): 0x000309B0. Every Box check goes through
it, the Box menu's too (entries of 0x40 bytes at 0x0036D0B0, item number at
+0x38, set open at +0x3C by 0x000B6E60 through 0x000B5A70). 0x00030B60 is
"new" (open, not seen); 0x00030A10 counts items still waiting on play time
(for "new pandra's box added!"). src/guest/unlocks.c opens them all when
the launcher's Pandora's Box setting is on.

0x0034277C, set from launch data tagged "PDLD" (0x00011620), is a kiosk
mode: everything open, but saving is skipped (0x00034050, 0x000340A0) and
menus change; not used.

## Boot logos

Scene 10 (current scene at 0x0034271C, next at 0x00342718, a table of
16-byte entries at 0x00341E30) is the title, logos included. Its object
(pointer at 0x004741B0) keeps a state at +0x1A0: 0 Sega, 1 Smilebit, 2
Sofdec, 3 the title; 0x000C0940 runs the state's handler (table at
0x0036E56C) and moves to the state it returns (old state to +0x1A4, +0x10
to 0, +4 counted). A card (0x000BF690, 0x000BF740, 0x000BF7F0) fades in
and starts its sprite animation (step 0), shows until the animation's time
(sprite +0x24, +0x50, +8) reaches 300.0 (0x00314D88; Sofdec 0x00315150),
fades out (step 2) and frees it (step 3). A press does nothing on the
console. Going straight to state 3 leaves the title without its logo, so
src/guest/intro_skip.c ends each card's animation instead (skip_logos).

## The original Panzer Dragoon (pd1.xbe)

Pandora's Box's APPENDIX > PANZER DRAGOON is another program on the disc,
d:\pd1.xbe. Confirming it ("Begin playing PANZER DRAGOON?", No by default)
leaves the Box (0x000B8080) with next scene -2 (Box object at 0x00474058,
+0x7D4): it takes everything down, saves and tail-jumps to 0x000120D0, which
shuts down sound and graphics and calls XLaunchNewImageA (0x0022CE3C) with
"d:\pd1.xbe". EXIT leaves for scene 9 (the main menu, started in mode
0x20003); scene 10 is the title as at boot. src/guest/launch_guard.c sends
-2 to scene 9 until pd1.xbe is recompiled too. Box navigation from FLIGHT
RECORDS: right goes EXIT, APPENDIX, ENCYCLOPEDIA, BOX GAME; left SUB
SCENARIOS.

## Boost and brake ends

At the end of a boost or a brake the game renders the scene at another
size: the draws into the scene target (0x00788000, 512x384) go from a
512x360 viewport at 0,12 to 426x320 at 0,0, and the full-screen paste
scales it up. On the same frame the models on screen go to other vertex
programs (ids keep growing: new programs each time). A dragon form change
also switches programs, but not the size. The dragon is skinned through
its program's constants, read through a0 (all 192 registers). The seven
3D draws that read a render target in play are the dragon's parts reading
the shadow map (0x00708000).
