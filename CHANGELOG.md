# Changelog

## v0.1.5 (beta)

### Added

- Vibration strength: Options > Gameplay > Vibration, Off or 10 % to
  100 % (the default) in steps of 10.
- Resolution scale 1.5x (Options > Display), between 1x and 2x: about
  half of 2x's work, for laptops and integrated graphics.
- VSync: Options > Display, On (the default), Fast or Off. Fast and Off
  show more frames when the PC cannot keep up with the display instead of
  dropping to half; Off may tear.
- Shadows: Options > Display, High (scaled with the resolution, the
  default) or Low (the game's own size, lighter on the GPU).
- 5.1 movie soundtracks: with Sound > Output on 5.1, the opening and the
  two story movies in Pandora's Box play their own 5.1 soundtracks, as on
  a console with Dolby Digital. Other movies are stereo on the disc.

### Fixed

- Controls: rebound buttons went back to the defaults when the launcher or
  the menu closed, or the window moved (issue 22).

## v0.1.4 (beta)

### Added

- Skip launcher: Options > Gameplay. Start the game at once with the save
  file and settings from last time. Hold Shift while starting to see the
  launcher once; the option is also in the in-game menu (F1).

### Fixed

- Windows: the log (ortarecomp.log) stayed empty when the game was started
  normally, so crash reports and logs could not be attached.
- Sound: the mix is 6 dB lower, as the Xbox's sound library sets it up; the
  busiest fights no longer distort. Distant sounds feed less reverb, as on
  the Xbox, in the stages that use it.

## v0.1.3 (beta)

### Fixed

- Linux and Steam Deck: "There's a problem with the disc you're using" could
  still appear on v0.1.2, going into a tutorial (or into one again), loading
  a save, or starting the next episode. The game keeps stage files in a
  hard-disk cache, and three things went wrong with it on Linux: a file
  opened through its folder was not found; looking a file up in a folder
  could carry on an earlier, finished search; and the cache was never
  emptied when the game formats it at every start, so new copies collided
  with old ones. The cache is now emptied at each start, as on the Xbox;
  the game fills it again as it goes. Thanks to nehobi for the second and
  third.
- Windows: opening a file through its folder no longer depends on Windows
  reporting the folder's path, which fails on some drives.

## v0.1.2 (beta)

### Fixed

- Linux and Steam Deck: "There's a problem with the disc you're using" at
  the end of an episode. The game clears old stages from its hard-disk
  cache to make room for the next; reopening a cleared stage folder got
  the wrong error back on Linux ("path not found" instead of "not found"),
  which the game takes for a bad disc.

### Added

- Bloom: Options > Picture > Bloom, Off (the default), Low or High. A glow
  on the bright parts of the scene, applied before the HUD is drawn, so the
  HUD, menus and text are untouched. Off costs nothing.
- Anisotropic texture filtering: Options > Display > Texture filter, Off
  (the original look, the default), 2x, 4x, 8x, 16x. Sharper textures at an
  angle and in the distance; point-filtered ones (menus, text) unchanged.
- 5.1 surround output: Options > Sound > Output now steps Stereo,
  Headphones, 5.1. In 5.1 the game's 3D sounds are placed around the four
  corner speakers by where they are, as the Xbox did on a surround setup;
  music, voices and menus stay on the front speakers; the room reverb comes
  from all around. Movies stay stereo for now.

## v0.1.1 (beta)

### Added

- Resolution scale up to 8x (was 4x), for high-end GPUs.
- Quit game in the in-game options menu (press it twice), for fullscreen
  play.
- Lossless Scaling frame generation (experimental), with your own copy of
  Lossless Scaling: Options > Advanced > Frame gen. now steps Off, Interp.
  (our frame interpolation, as before), LS 2x, LS 3x, LS 4x, with an LS mode
  (Quality or Performance) and a Lossless.dll line to choose the DLL.

### Fixed

- Linux and Steam Deck: the game could crash when starting (at the launcher's
  Start game, or at once), depending on where the system placed memory that
  run. The game's whole memory range is now reserved up front.

## v0.1.0 (beta)

The first release. Changes are listed against the original Xbox game.

### Added

- Native PC port: the Xbox game statically recompiled, running without an
  emulator.
- Windows and Linux builds; the Linux one runs on the Steam Deck.
- Launcher: pick your game files (disc image or extracted folder), save
  file and settings before starting.
- Three save files (the original keeps one).
- Resolution scale, 1x to 4x.
- Anti-aliasing: MSAA up to 8x, or FXAA.
- Frame interpolation above 60 fps (beta).
- Aspect ratio 4:3 or 16:9 (the game's own widescreen mode), with bars,
  stretch or zoom to fit the window.
- Fullscreen (Alt+Enter) and a resizable window.
- Picture settings: CRT look, brightness, contrast, gamma, saturation, and
  optional TV softening.
- 3D audio for headphones, and a master volume.
- Keyboard and mouse controls, with mouse aim; controller support; every
  button can be rebound.
- Options menu in game (F1, or Back + Start on a controller).
- Pandora's Box option to open every item, without touching the save.
- Skip the boot logos with a press.
- FPS counter.
- Portable mode (a blank portable.txt next to the executable).
- A log of every run, and a crash report in it, to attach to bug reports.

### Changed

- Graphics, sound and input are new PC code in place of the Xbox's.
- Settings are kept in ortarecomp.ini, saves in the data folder.
