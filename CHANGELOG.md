# Changelog

## v0.1.13 (beta)

### Added

- **Android (preview):** an APK for Android 11+ with Vulkan. Touch
  controls, controller support, the original Panzer Dragoon, and custom GPU
  drivers like Turnip (Options > Advanced > GPU driver). Tested on a
  Retroid Pocket 6.
- Mouse and touch work in the menus and launcher.
- Options > Advanced > About.
- A small "Building shaders" note while new shaders are being built.

### Changed

- **Smoother first runs:** shaders build in the background, and the
  graphics driver's compiled shaders are saved too, so scenes you've seen
  start smooth. Shaders rebuild once with this version.
- **AMD on Windows:** the dragon's wings (and other models) no longer
  disappear for a moment the first time a stage plays.
- **Frame interpolation is much lighter:** 144 fps at 4x on a mid-range
  GPU.
- Less CPU and video memory used overall. Better for handhelds and battery.
- GPUs missing some features (some older Android ones) now run.
- Menu: Picture and Bloom moved to Display, Show FPS to Advanced. Smaller
  FPS counter.

### Fixed

- A `Lossless.dll` picked in the launcher wasn't used.

## v0.1.12 (beta)

### Fixed

- Difficulty: Normal played like Easy, and every difficulty was a step
  easier than on a USA console (issue 8). The game sets its difficulty
  from the console's region, and the port answered with no region at all;
  it now answers North America, as a USA console does. In episode 2 the
  wall plants (Golegs) take two Base Wing volleys on Normal again, not one.

### Changed

- The `Partition*.img` files in the save folder now hold only what the
  game writes to them, about 120 KB in all, instead of showing as several
  GB (issue 3). They were placeholders for the Xbox hard disk's
  partitions, stretched to the partitions' full sizes: almost empty on most
  drives, but shown at full size by file managers, and really taking that
  space on a drive without sparse files (a USB stick formatted FAT32 or
  exFAT). The game sees the same disk as before. Existing files are
  trimmed at the next start, keeping everything written to them.

## v0.1.10 (beta)

### Fixed

- "The disc may be dirty or damaged" at the episode 7 boss, after playing
  from episode 1 in one sitting (issue 10). It was not the disc: by then
  the game's memory was free in pieces too small for what the boss loads,
  which the console (whose memory need not be in one piece) never runs
  into. A request that finds no piece large enough now gets memory of its
  own.

### Changed

- Vertex programs (the game's 3D models) are each made into a GPU shader
  of their own, instead of all going through one that interprets them:
  much lighter on handhelds, laptops and integrated graphics. The first
  time a scene appears its shaders and pipelines are made in the
  background while it plays (the old way draws meanwhile), then kept in
  `shaders/` in the data folder and by the graphics driver, so it is
  smooth from then on. The saved pipeline list (`pipelines.bin`) starts
  again once.

## v0.1.9 (beta)

### Added

- Options > Display > Frame rate: 60 (the default) or 30. At 30 every
  other frame is shown, a steady 30 fps: smoother on a weak graphics chip
  than a rate that keeps dropping from 60, and half its work. The game
  still runs at full speed. Orta only; the original Panzer Dragoon keeps
  its own 20 fps (issue 28).

### Fixed

- The original Panzer Dragoon: the far sea was a flat pale band, distant
  objects did not fade in and the radar was not see-through (issue 7). Its
  renderer draws those through a dither pattern it rotates a bit per pixel,
  and the recompiler lost the bit each rotation reports; it keeps it now.
- Windows: switching between Orta and the original Panzer Dragoon, the new
  window could open behind another one (a browser, say) without the focus
  or the gamepad. The old window now hands the focus over before closing.

## v0.1.8 (beta)

### Added

- Pandora's Box: APPENDIX > PANZER DRAGOON plays the original Panzer
  Dragoon (pd1.xbe on the Orta disc), as on the console, and its title's
  EXIT goes back to Orta. The window closes and opens again at each switch;
  settings, the window's place and the log carry on. Its Direct3D, sound,
  movies and controls are Orta's own; it draws its frames itself, so the
  Options that change how the 3D world is drawn are greyed while it runs
  (Display > Fit Zoom keeps it whole, with bars). Building it needs
  `tools/recomp.py --program pd1` on your disc image (BUILDING.md).

### Fixed

- Two recompiler fixes for instructions Orta's code uses rarely: rounding
  float-to-integer conversions now round instead of truncating, and agree
  between compilers for values out of range.

## v0.1.7 (beta)

### Added

- Ultrawide: Options > Display > Aspect ratio 21:9. The game shows a wider
  view, as its own 16:9 mode does, and the HUD keeps its shape: Display >
  HUD Centered keeps its 16:9 layout in the middle of the screen, Edges
  moves the radar and gauges out to the screen's sides (menus and
  subtitles stay in the middle). Real-time cutscenes fill the screen,
  without the game's black bars above and below, the subtitles over the
  picture; movies follow Display > Movies (Zoom fills a 21:9 screen).
  Menus are shown at 16:9 in the middle (issue 13).
- Switching between 16:9 and 21:9 takes effect at once, mid-game too;
  between 4:3 and widescreen the next time the game starts (the menu says
  so), the picture keeping its shape until then.

### Changed

- 5.1: 3D sound now works on 5.1 too, as on the console: each sound goes
  through the Xbox's HRTF filters for left and right and is shared between
  the front and back speakers by the game's own rule (all front within 45
  degrees of ahead, all back within 45 degrees of behind, even overhead).
  Options > Sound > 3D sound Off keeps the earlier panning around the four
  speakers.

## v0.1.6 (beta)

### Added

- Movies: Options > Display > Movies. Original (the default) shows them
  as the game frames them, with its black bars, and in the game's 16:9
  mode widened with the rest of the picture, as on a widescreen TV. Bars
  shows the picture alone at its own shape (filling a 16:9 window edge to
  edge), Stretch and Zoom fill the window (issue 18). Subtitles stay
  whole, laid over the bottom of the picture.

### Changed

- Sound: 3D sounds (enemies, gunfire, explosions) are placed by the Xbox's
  own HRTF filters, read from the game: each ear hears its own filtered
  sound and delay, as on the console, on speakers or headphones (Stereo and
  Headphones outputs; 5.1 is unchanged). Gunfire is softer and less harsh.
  Options > Sound > 3D sound: On (the default) or Off (the earlier
  left/right pan).

### Fixed

- Windows: movies played at about 20 frames a second and Pandora's Box's
  opening could stop partway back to the box: the movie decoder was built
  without optimisation (the compiler skips very large functions unless
  told otherwise). They play at full speed now.
- Menu: long pages (Display in the launcher) ran past the bottom of the
  screen, hiding the help line; their rows now sit closer.

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
