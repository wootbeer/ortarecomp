# Orta Recomp: Panzer Dragoon Orta (2002 Xbox) Recompiled and Port

You will need the game files from a licensed copy in order to play it.

A native PC port of Panzer Dragoon Orta, made by statically recompiling the
Xbox game with [xboxrecomp](https://github.com/sp00nznet/xboxrecomp), with
graphics, audio and input of its own on [SDL 3](https://libsdl.org).

Unofficial, and not affiliated with the rights holders. **No game data is
included.** You need your own copy of the game to play this.

**Beta.** The whole game plays through, but expect issues, mostly with frame
interpolation (smooth motion above 60 fps). If something looks wrong, turn
Frame interp. off in F1 > Advanced and see if it goes away, then report it.

Android build is planned.

## Features

- **Higher resolutions:** render at 1x to 8x the original resolution (8x is
  5120x3840).
- **Widescreen:** the game's own 16:9 mode, or the original 4:3; bars,
  stretch or zoom to fit any window or screen.
- **Anti-aliasing:** MSAA up to 8x, or FXAA; anisotropic texture filtering
  up to 16x.
- **Smooth motion:** frame interpolation above 60 fps for high refresh rate
  displays (beta), or Lossless Scaling's frame generation with your own copy
  of Lossless Scaling (experimental).
- **Color correction:** brightness, contrast, gamma and saturation, a CRT
  look, optional TV softening, and bloom (Low or High) that leaves the HUD
  alone.
- **Sound:** stereo 3D positional audio, a headphones mode (a modelled
  head: each ear hears a sound at its own time and loudness), or 5.1
  surround (3D sounds placed around four speakers, and the movies' own 5.1
  soundtracks), with the game's room
  reverb and a master volume.
- **Controls:** controllers, keyboard and mouse (with mouse aim), every
  button rebindable.
- **Three save files** (the original keeps one), picked in the launcher.
- **Extras:** open every Pandora's Box item without touching your save,
  skip the boot logos or the launcher, an FPS counter, fullscreen (Alt+Enter), portable
  mode.
- **Easy setup:** point the launcher at your disc image or game folder.
- **Crash and freeze reports** written to a log, to attach to bug reports.

## Game Files

You need Panzer Dragoon Orta (USA), either:

- a disc image (`.iso` / `.xiso`), or
- a folder with the extracted game (`default.xbe` and the rest of the disc).

default.xbe SHA-256:
c1af4eafccc02e11105d606b177263216cb7aa71bf1fbe8486ddfc3b1b848d2c

On first launch, choose Game files in the launcher and pick your disc image
or folder. A disc image is copied into the app's data folder.

Add a blank portable.txt in the application's folder to make portable if you
want to save settings, saves and game files locally.

## Playing

- F1, or Back + Start on a controller: the options menu (resolution,
  anti-aliasing, aspect ratio, picture, sound, controls and more).
- Alt+Enter: fullscreen.
- Save file 1, 2 or 3 is picked in the launcher before starting.
- Needs a 64-bit PC with a Vulkan graphics driver: Windows 10/11, or
  Linux / Steam Deck.

## Frame Generation

Options (F1) > Advanced > Frame gen.: **Interp.** blends the game's own
frames (ours); **LS 2x/3x/4x** uses Lossless Scaling's frame generation
(LSFG 3.1) to show two to four pictures per game frame, 120 to 240 fps from
the game's 60, so it wants a screen that fast. It needs your own copy of
[Lossless Scaling](https://store.steampowered.com/app/993090/Lossless_Scaling/)'s
`Lossless.dll`: found in Steam's default library folder, in
`lossless_scaling` next to the settings, or chosen with the Lossless.dll
line. **LS mode** Performance is lighter on the GPU and rougher in fast
motion. Turning Lossless Scaling on or off takes effect at the next start;
it adds about one game frame of delay. Lossless Scaling guesses the frames
between from the finished pictures, so fast motion can smear, as in the
Lossless Scaling app; Interp. redraws the scene itself. Experimental.
Nothing from Lossless Scaling is in this repository or a release.

## Steam Deck / Linux

- Extract the `linux-x86_64` download and run `OrtaRecomp`.
- Set up the game files once in Desktop Mode (the file picker needs it), or
  put your disc image next to the executable before the first launch and it
  is imported automatically.
- Then add `OrtaRecomp` to Steam as a non-Steam game to play in Gaming Mode;
  the launcher and the F1 menu work with the Deck's controls (View + Menu
  opens the menu in game).

## Reporting Issues

Every run writes `ortarecomp.log` to the data folder
(`%APPDATA%\OrtaRecomp\OrtaRecomp` on Windows,
`~/.local/share/OrtaRecomp/OrtaRecomp` on Linux and Steam Deck, or the
app's folder when portable); the
run before is kept as `ortarecomp-previous.log`. After a crash or a freeze,
attach that log (the previous one if you have started the game again) to
your report in the GitHub Issues section, with what you were doing.

## Building

See [BUILDING.md](BUILDING.md).

## Issues and Limitations

Known:
- Beta status. Played through on Windows; Linux and Steam Deck are less
  tested so far.
- Frame interpolation can show odd frames in some scenes.
- The first time an effect appears it can stutter once while its shaders are
  built; they are kept for next time.
- The original Panzer Dragoon (the Saturn game unlocked in Pandora's Box)
  is not available yet.
- USA version only.

Else:
- Please see the "Issues" section in GitHub.

## Credits

- [xboxrecomp](https://github.com/sp00nznet/xboxrecomp) by sp00nz and
  contributors: the recompiler and the Xbox kernel layer this port runs on
- [SDL](https://libsdl.org): windowing, graphics, audio and input
- [lsfg-vk](https://github.com/PancakeTAS/lsfg-vk) (MIT) and
  [DXVK](https://github.com/doitsujin/dxvk)'s shader compiler (zlib), through
  `lib/lsfg` from BattleTanx: Global Assault Recompiled: Lossless Scaling
  frame generation
- [SPIRV-Headers](https://github.com/KhronosGroup/SPIRV-Headers) (MIT), for
  building that

## License

The project's own code is GPL-3.0; see [COPYING](COPYING). Third-party
licenses are in [licenses/](licenses). The game itself remains the property
of its rights holders.
