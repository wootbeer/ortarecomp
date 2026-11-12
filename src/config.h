#ifndef PDO_CONFIG_H
#define PDO_CONFIG_H

#include <stdbool.h>
#include <stddef.h>

#define PDO_PATH_MAX 1024

/* VSync: On waits for the display (FIFO); Fast shows the newest frame at
 * each refresh without tearing (mailbox); Off shows frames at once (may
 * tear). */
enum { PDO_VSYNC_ON, PDO_VSYNC_FAST, PDO_VSYNC_OFF };

typedef struct pdo_config {
    char game_dir[PDO_PATH_MAX];    /* folder holding default.xbe and media/ */
} pdo_config;

/* Options the overlay menu (F1) changes; saved with the rest. */
typedef struct pdo_settings {
    int  render_scale2;     /* 2-16: render targets at half this multiple of the game's size (3: 1.5x) */
    int  msaa;              /* 1, 2, 4 or 8 samples */
    int  aspect;            /* PDO_ASPECT_*; 16:9 is the game's own widescreen mode */
    int  fit;               /* PDO_FIT_*: how the picture fills the window */
    bool soft_filter;       /* TV-like vertical softening at presentation */
    int  master_volume;     /* 0-100 % */
    bool show_fps;
    bool gpu_vertex;        /* vertex programs on the GPU (shaders/vsh.vert), not the CPU */
    bool gpu_fixed;         /* fixed-function vertices on the GPU (shaders/fvf.vert) */
    bool frame_interp;      /* frames between the game's 60, blended (renderer.c) */
    int  lsfg_multiplier;   /* Lossless Scaling frame generation: pictures per game
                               frame, 2-4; 1 off (lsfg_present.cpp, from the next start) */
    bool lsfg_performance;  /* its performance mode, lighter and less exact */
    char lsfg_dll[PDO_PATH_MAX];    /* the player's Lossless.dll; "" looks in the usual places */
    bool fullscreen;        /* borderless, the whole monitor (Alt+Enter) */
    bool fxaa;              /* edge smoothing on the finished frame (shaders/screen.frag) */
    int  vsync;             /* PDO_VSYNC_*: how frames reach the screen (renderer.c) */
    bool shadow_original;   /* Shadows: Low, shadow maps at the game's size, not scaled (renderer.c) */
    int  anisotropy;        /* 1 off, 2, 4, 8, 16: anisotropic filtering of smooth-filtered textures */
    int  bloom;             /* 0 off, 1 low, 2 high: on the scene, before the HUD (renderer.c) */
    int  win_x, win_y, win_w, win_h;    /* the window when not fullscreen (w 0: not set yet) */
    bool win_maximized;
    bool mouse_aim;         /* the mouse steers the left stick (input_hle.c) */
    int  mouse_speed;       /* 10-400 % */
    bool unlock_box;        /* every Pandora's Box item open, the save untouched (src/guest/unlocks.c) */
    bool skip_logos;        /* a press during the boot logos goes to the title (src/guest/intro_skip.c) */
    bool skip_launcher;     /* start the game without the launcher (src/main.c; Shift held shows it) */
    int  rumble;            /* controller vibration strength, 0-100 % (src/guest/input_hle.c) */
    bool depth_reversed;    /* depth stored as 1 - z (renderer.c, read at start) */
    /* Picture (present.frag), on the finished frame: */
    bool picture_crt;       /* a CRT's darker midtones: x^(2.5/2.2) */
    int  pic_brightness;    /* -50..50, % added */
    int  pic_contrast;      /* 50..150 % */
    int  pic_gamma;         /* 50..200: 100 as is, more brightens the midtones */
    int  pic_saturation;    /* 0..200 % */
    int  audio_out;         /* PDO_AUDIO_*: stereo; 3D sounds through a model head for
                               headphones; or 5.1, 3D sounds around four speakers (mixer.c) */
    int  save_slot;         /* 1-3: which save file the game sees (pdo_save_dir), chosen before it starts */
} pdo_settings;

enum { PDO_ASPECT_4_3, PDO_ASPECT_16_9, PDO_ASPECT_COUNT };
enum { PDO_AUDIO_STEREO, PDO_AUDIO_HEADPHONES, PDO_AUDIO_SURROUND, PDO_AUDIO_COUNT };
/* The whole picture with bars; stretched to the window; zoomed to fill it,
 * the overflow cut off. */
enum { PDO_FIT_BARS, PDO_FIT_STRETCH, PDO_FIT_ZOOM, PDO_FIT_COUNT };

/* Whether the game set itself up in its widescreen mode (anamorphic 16:9:
 * a wider view squeezed into 640x480), which it reads once at start:
 * src/guest/widescreen.c. */
bool pdo_widescreen_active(void);

/* The current settings: defaults until pdo_config_load reads the file. */
pdo_settings *pdo_settings_get(void);
/* Write the settings, keeping the game folder the file already holds. */
bool pdo_settings_save(void);

/* Where settings, saves and imported game files live. With a portable.txt
 * next to the executable this is the executable's folder; otherwise the
 * per-user data folder. Always ends in a path separator. */
const char *pdo_data_dir(void);

/* The log file this run writes, empty without one (main.c); a crash report
 * names it. */
extern char pdo_log_file[PDO_PATH_MAX];

/* The game keeps one save. The console's E: (TDATA, UDATA), where it
 * lives, is a folder under saves/ per save file: E for file 1 (where saves
 * always were), E2 and E3. Written without a trailing separator. */
void pdo_save_dir(int slot, char *out, size_t len);
/* Whether that save file holds a save. */
bool pdo_save_slot_used(int slot);

bool pdo_config_load(pdo_config *cfg);
bool pdo_config_save(const pdo_config *cfg);

#endif
