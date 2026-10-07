/*
 * Real graphics backend: SDL2 (loaded at run time with dlopen, so no SDL headers/libs are needed to build).
 * Used with UNITY_GL=sdl on the handheld (KMSDRM + libmali) or on a desktop for testing.
 */
#define _GNU_SOURCE
#include "android_native.h"
#include "loader.h"

#include <dlfcn.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define SDL_INIT_AUDIO 0x00000010u
#define SDL_INIT_VIDEO 0x00000020u
#define SDL_INIT_JOYSTICK 0x00000200u
#define SDL_INIT_GAMECONTROLLER 0x00002000u
#define SDL_WINDOW_OPENGL 0x00000002u
#define SDL_WINDOW_SHOWN 0x00000004u
#define SDL_WINDOW_FULLSCREEN_DESKTOP 0x00001001u
#define SDL_WINDOWPOS_UNDEFINED 0x1FFF0000
enum { GL_RED = 0, GL_GREEN, GL_BLUE, GL_ALPHA, GL_BUFFER, GL_DOUBLEBUF, GL_DEPTH, GL_STENCIL,
       GL_CTX_MAJOR = 17, GL_CTX_MINOR = 18, GL_CTX_PROFILE = 21, GL_SHARE = 22 };
#define SDL_GL_CONTEXT_PROFILE_ES 4

#define SDL_QUIT_EVENT 0x100
#define SDL_KEYDOWN_EVENT 0x300
#define SDL_KEYUP_EVENT 0x301
#define SDL_MOUSEBUTTONDOWN_EVENT 0x401
#define SDL_MOUSEBUTTONUP_EVENT 0x402
#define SDL_JOYAXISMOTION_EVENT 0x600
#define SDL_JOYHATMOTION_EVENT 0x602
#define SDL_JOYBUTTONDOWN_EVENT 0x603
#define SDL_JOYBUTTONUP_EVENT 0x604
#define SDL_CONTROLLERAXISMOTION_EVENT 0x650
#define SDL_CONTROLLERBUTTONDOWN_EVENT 0x651
#define SDL_CONTROLLERBUTTONUP_EVENT 0x652

#define MAX_CTX 4

static struct {
    int (*Init)(uint32_t);
    const char *(*GetError)(void);
    void *(*CreateWindow)(const char *, int, int, int, int, uint32_t);
    int (*GL_SetAttribute)(int, int);
    void *(*GL_CreateContext)(void *);
    int (*GL_MakeCurrent)(void *, void *);
    void (*GL_SwapWindow)(void *);
    int (*GL_SetSwapInterval)(int);
    void *(*GL_GetProcAddress)(const char *);
    void (*GL_GetDrawableSize)(void *, int *, int *);
    int (*PollEvent)(void *);
    int (*ShowCursor)(int);

    /* Gamepad / Joystick */
    int (*NumJoysticks)(void);
    void *(*JoystickOpen)(int);
    const char *(*JoystickName)(void *);
    int (*IsGameController)(int);
    void *(*GameControllerOpen)(int);
    const char *(*GameControllerName)(void *);
    char *(*GameControllerMapping)(void *);
    int (*GameControllerAddMapping)(const char *);
    int (*GameControllerAddMappingsFromFile)(const char *);

    /* Audio */
    int (*OpenAudioDevice)(const char *, int, const void *, void *, int);
    void (*CloseAudioDevice)(uint32_t);
    void (*PauseAudioDevice)(uint32_t, int);
    int (*QueueAudio)(uint32_t, const void *, uint32_t);
    uint32_t (*GetQueuedAudioSize)(uint32_t);
    void (*ClearQueuedAudio)(uint32_t);
    const char *(*GetCurrentAudioDriver)(void);
} S;

static int g_active;
static void *g_win;
static void *g_ctx[MAX_CTX];
static int g_nctx, g_ctx_used;
static uint32_t g_audio_dev = 0;
static int g_has_gamecontroller = 0;
static int s_swap_ab = 1;

int sdl_gl_active(void) { return g_active; }

#define LOAD(name) do { *(void **)&S.name = dlsym(lib, "SDL_" #name); if (!S.name) { so_log("[sdl] missing SDL_" #name); return 0; } } while (0)
#define LOAD_OPT(name) do { *(void **)&S.name = dlsym(lib, "SDL_" #name); } while (0)

int sdl_gl_init(void) {
    void *lib = dlopen("libSDL2-2.0.so.0", RTLD_NOW | RTLD_GLOBAL);
    if (!lib) lib = dlopen("libSDL2.so", RTLD_NOW | RTLD_GLOBAL);
    if (!lib) { so_log("[sdl] cannot load libSDL2: %s", dlerror()); return 0; }
    LOAD(Init); LOAD(GetError); LOAD(CreateWindow); LOAD(GL_SetAttribute); LOAD(GL_CreateContext);
    LOAD(GL_MakeCurrent); LOAD(GL_SwapWindow); LOAD(GL_SetSwapInterval); LOAD(GL_GetProcAddress);
    LOAD(GL_GetDrawableSize); LOAD(PollEvent); LOAD(ShowCursor);

    LOAD_OPT(NumJoysticks); LOAD_OPT(JoystickOpen); LOAD_OPT(JoystickName);
    LOAD_OPT(IsGameController); LOAD_OPT(GameControllerOpen);
    LOAD_OPT(GameControllerName); LOAD_OPT(GameControllerMapping);
    LOAD_OPT(GameControllerAddMapping); LOAD_OPT(GameControllerAddMappingsFromFile);
    LOAD_OPT(OpenAudioDevice); LOAD_OPT(CloseAudioDevice); LOAD_OPT(PauseAudioDevice);
    LOAD_OPT(QueueAudio); LOAD_OPT(GetQueuedAudioSize); LOAD_OPT(ClearQueuedAudio);
    LOAD_OPT(GetCurrentAudioDriver);

    if (S.Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER) != 0) {
        so_log("[sdl] SDL_Init failed: %s", S.GetError());
        return 0;
    }
    S.GL_SetAttribute(GL_CTX_PROFILE, SDL_GL_CONTEXT_PROFILE_ES);
    S.GL_SetAttribute(GL_CTX_MAJOR, 3);
    S.GL_SetAttribute(GL_CTX_MINOR, 0);
    S.GL_SetAttribute(GL_RED, 8); S.GL_SetAttribute(GL_GREEN, 8); S.GL_SetAttribute(GL_BLUE, 8);
    S.GL_SetAttribute(GL_ALPHA, 8);
    S.GL_SetAttribute(GL_DEPTH, 24); S.GL_SetAttribute(GL_STENCIL, 8);
    S.GL_SetAttribute(GL_DOUBLEBUF, 1);

    g_win = S.CreateWindow("Unreal Life", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 0, 0,
                           SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN | SDL_WINDOW_FULLSCREEN_DESKTOP);
    if (!g_win) { so_log("[sdl] SDL_CreateWindow failed: %s", S.GetError()); return 0; }
    S.ShowCursor(0);

    /* all contexts are created now, in one share group */
    for (int i = 0; i < MAX_CTX; i++) {
        if (i == 1) S.GL_SetAttribute(GL_SHARE, 1);
        void *c = S.GL_CreateContext(g_win);
        if (!c) {
            so_log("[sdl] context #%d failed: %s", i, S.GetError());
            if (i == 0) return 0;
            break;
        }
        g_ctx[g_nctx++] = c;
    }
    S.GL_MakeCurrent(g_win, NULL);
    const char *fps_env = getenv("UNITY_FPS");
    int fps = fps_env ? atoi(fps_env) : 0;
    int interval = (fps == 30) ? 2 : 1;
    const char *swap_env = getenv("UNITY_SWAP_INTERVAL");
    if (swap_env) interval = atoi(swap_env);
    if (interval < 0) interval = 1;
    S.GL_SetSwapInterval(interval);
    so_log("[sdl] GL SwapInterval set to %d (UNITY_FPS=%d)", interval, fps);

    int w = 0, h = 0;
    S.GL_GetDrawableSize(g_win, &w, &h);
    if (w > 0 && h > 0) { g_screen_w = w; g_screen_h = h; }
    so_log("[sdl] window %dx%d, %d GL contexts", g_screen_w, g_screen_h, g_nctx);

    /* Register known RK3326 mappings and external gamecontrollerdb.txt */
    const char *sw = getenv("UNITY_SWAP_AB");
    int swap_ab = (sw && (!strcmp(sw, "0") || !strcmp(sw, "false") || !strcmp(sw, "xbox"))) ? 0 : 1;
    s_swap_ab = swap_ab;
    so_log("[sdl] AB button layout: %s", swap_ab ? "Nintendo (physical A=A, B=B)" : "Xbox (physical B=A, A=B)");

    if (S.GameControllerAddMapping) {
        if (swap_ab) {
            /* Nintendo layout: physical A (b1) -> a, physical B (b0) -> b, physical X (b3) -> x, physical Y (b2) -> y */
            S.GameControllerAddMapping("1900bb3e4b4800000011000000010000,GO-Super Gamepad,a:b1,b:b0,x:b3,y:b2,back:b12,start:b13,leftshoulder:b4,rightshoulder:b5,lefttrigger:b6,righttrigger:b7,leftstick:b14,rightstick:b15,dpup:b8,dpdown:b9,dpleft:b10,dpright:b11,leftx:a0,lefty:a1,rightx:a2,righty:a3,platform:Linux,");
            S.GameControllerAddMapping("1900bb3e4b4800000011000000010000,GO-Super Gamepad,a:b1,b:b0,x:b3,y:b2,back:b12,start:b13,leftshoulder:b4,rightshoulder:b5,lefttrigger:b6,righttrigger:b7,leftstick:b14,rightstick:b15,dpup:b8,dpdown:b9,dpleft:b10,dpright:b11,leftx:a0,lefty:a1,rightx:a2,righty:a3,crc:3ebb,platform:Linux,");
            S.GameControllerAddMapping("19000000010000000100000001010000,OpenSimHardware OSH PB Controller,a:b1,b:b0,x:b3,y:b2,back:b8,start:b9,leftshoulder:b4,rightshoulder:b5,lefttrigger:b6,righttrigger:b7,leftstick:b10,rightstick:b11,dpup:b12,dpdown:b13,dpleft:b14,dpright:b15,leftx:a0,lefty:a1,platform:Linux,");
            S.GameControllerAddMapping("03000000010000000100000001010000,OpenSimHardware OSH PB Controller,a:b1,b:b0,x:b3,y:b2,back:b8,start:b9,leftshoulder:b4,rightshoulder:b5,lefttrigger:b6,righttrigger:b7,leftstick:b10,rightstick:b11,dpup:b12,dpdown:b13,dpleft:b14,dpright:b15,leftx:a0,lefty:a1,platform:Linux,");
            S.GameControllerAddMapping("190000004b4800000010000001010000,GO-Advance Gamepad,a:b1,b:b0,x:b3,y:b2,back:b8,start:b9,leftshoulder:b4,rightshoulder:b5,lefttrigger:b6,righttrigger:b7,leftstick:b10,rightstick:b11,dpup:b12,dpdown:b13,dpleft:b14,dpright:b15,leftx:a0,lefty:a1,platform:Linux,");
            S.GameControllerAddMapping("030000004b4800000010000001010000,GO-Advance Gamepad,a:b1,b:b0,x:b3,y:b2,back:b8,start:b9,leftshoulder:b4,rightshoulder:b5,lefttrigger:b6,righttrigger:b7,leftstick:b10,rightstick:b11,dpup:b12,dpdown:b13,dpleft:b14,dpright:b15,leftx:a0,lefty:a1,platform:Linux,");
        } else {
            /* Xbox layout */
            S.GameControllerAddMapping("1900bb3e4b4800000011000000010000,GO-Super Gamepad,a:b0,b:b1,x:b2,y:b3,back:b12,start:b13,leftshoulder:b4,rightshoulder:b5,lefttrigger:b6,righttrigger:b7,leftstick:b14,rightstick:b15,dpup:b8,dpdown:b9,dpleft:b10,dpright:b11,leftx:a0,lefty:a1,rightx:a2,righty:a3,platform:Linux,");
            S.GameControllerAddMapping("1900bb3e4b4800000011000000010000,GO-Super Gamepad,a:b0,b:b1,x:b2,y:b3,back:b12,start:b13,leftshoulder:b4,rightshoulder:b5,lefttrigger:b6,righttrigger:b7,leftstick:b14,rightstick:b15,dpup:b8,dpdown:b9,dpleft:b10,dpright:b11,leftx:a0,lefty:a1,rightx:a2,righty:a3,crc:3ebb,platform:Linux,");
            S.GameControllerAddMapping("19000000010000000100000001010000,OpenSimHardware OSH PB Controller,a:b0,b:b1,x:b2,y:b3,back:b8,start:b9,leftshoulder:b4,rightshoulder:b5,lefttrigger:b6,righttrigger:b7,leftstick:b10,rightstick:b11,dpup:b12,dpdown:b13,dpleft:b14,dpright:b15,leftx:a0,lefty:a1,platform:Linux,");
            S.GameControllerAddMapping("03000000010000000100000001010000,OpenSimHardware OSH PB Controller,a:b0,b:b1,x:b2,y:b3,back:b8,start:b9,leftshoulder:b4,rightshoulder:b5,lefttrigger:b6,righttrigger:b7,leftstick:b10,rightstick:b11,dpup:b12,dpdown:b13,dpleft:b14,dpright:b15,leftx:a0,lefty:a1,platform:Linux,");
            S.GameControllerAddMapping("190000004b4800000010000001010000,GO-Advance Gamepad,a:b0,b:b1,x:b2,y:b3,back:b8,start:b9,leftshoulder:b4,rightshoulder:b5,lefttrigger:b6,righttrigger:b7,leftstick:b10,rightstick:b11,dpup:b12,dpdown:b13,dpleft:b14,dpright:b15,leftx:a0,lefty:a1,platform:Linux,");
            S.GameControllerAddMapping("030000004b4800000010000001010000,GO-Advance Gamepad,a:b0,b:b1,x:b2,y:b3,back:b8,start:b9,leftshoulder:b4,rightshoulder:b5,lefttrigger:b6,righttrigger:b7,leftstick:b10,rightstick:b11,dpup:b12,dpdown:b13,dpleft:b14,dpright:b15,leftx:a0,lefty:a1,platform:Linux,");
        }
    }
    if (S.GameControllerAddMappingsFromFile) {
        S.GameControllerAddMappingsFromFile("gamecontrollerdb.txt");
        S.GameControllerAddMappingsFromFile("/roms/ports/unreallife/gamecontrollerdb.txt");
    }

    /* Open controllers */
    int nj = S.NumJoysticks ? S.NumJoysticks() : 0;
    so_log("[sdl] detected %d joysticks/controllers", nj);
    for (int i = 0; i < nj; i++) {
        if (S.IsGameController && S.IsGameController(i) && S.GameControllerOpen) {
            void *c = S.GameControllerOpen(i);
            if (c) {
                g_has_gamecontroller = 1;
                const char *cname = S.GameControllerName ? S.GameControllerName(c) : "Unknown";
                char *cmap = S.GameControllerMapping ? S.GameControllerMapping(c) : NULL;
                so_log("[sdl] opened GameController #%d: '%s' mapping: %s", i, cname, cmap ? cmap : "(none)");
            }
        } else if (S.JoystickOpen) {
            void *j = S.JoystickOpen(i);
            const char *jname = S.JoystickName ? S.JoystickName(j) : "Unknown";
            so_log("[sdl] opened Joystick #%d: '%s'", i, jname);
        }
    }

    g_active = 1;
    return 1;
}

void *sdl_gl_create_context(void) {
    if (g_ctx_used >= g_nctx) { so_log("[sdl] out of pre-created GL contexts"); return NULL; }
    return g_ctx[g_ctx_used++];
}

int sdl_gl_make_current(void *ctx) {
    int r = S.GL_MakeCurrent(g_win, ctx);
    if (r != 0) so_log("[sdl] SDL_GL_MakeCurrent failed: %s", S.GetError());
    return r == 0;
}

void sdl_gl_swap(void) { S.GL_SwapWindow(g_win); }
void sdl_gl_swap_interval(int n) { S.GL_SetSwapInterval(n); }
void *sdl_gl_get_proc(const char *name) { return S.GL_GetProcAddress(name); }

/* --- SDL Audio --- */
typedef struct {
    int freq;
    uint16_t format;
    uint8_t channels;
    uint8_t silence;
    uint16_t samples;
    uint16_t padding;
    uint32_t size;
    void (*callback)(void *, uint8_t *, int);
    void *userdata;
} AudioSpec;

int sdl_audio_init(int freq, int channels, void (*cb)(void *, uint8_t *, int), void *userdata) {
    if (!S.OpenAudioDevice) { so_log("[sdl] OpenAudioDevice not available"); return 0; }
    if (g_audio_dev) return 1;
    /* A/B diagnostic: UNITY_NO_AUDIO=1 leaves FMOD without a device, so we can measure how
     * much of the frame budget the 176 MB FSB5 music bundle + FMOD decode actually costs. */
    const char *noaudio = getenv("UNITY_NO_AUDIO");
    if (noaudio && noaudio[0] == '1') {
        so_log("[sdl] UNITY_NO_AUDIO=1 -> audio disabled (diagnostic)");
        return 0;
    }

    /* Silence ALSA errors so underruns/stalls don't spam stderr and disk.
     * UNITY_ALSA_LOG=1 keeps them ON: the previous session's log showed "22 ALSA underruns" and
     * underruns are the classic cause of crackle, so we need to be able to see them. */
    const char *alsalog = getenv("UNITY_ALSA_LOG");
    if (!(alsalog && alsalog[0] == '1')) {
        typedef void (*snd_err_fn)(const char *, int, const char *, int, const char *, ...);
        typedef int (*snd_set_err_fn)(snd_err_fn);
        void *h_alsa = dlopen("libasound.so.2", RTLD_LAZY | RTLD_NOLOAD);
        if (!h_alsa) h_alsa = dlopen("libasound.so.2", RTLD_LAZY);
        if (h_alsa) {
            snd_set_err_fn set_err = (snd_set_err_fn)dlsym(h_alsa, "snd_lib_error_set_handler");
            if (set_err) set_err(NULL);
        }
    } else {
        so_log("[sdl] UNITY_ALSA_LOG=1 -> ALSA errors left ON (underruns will show in log)");
    }

    AudioSpec desired, obtained;
    memset(&desired, 0, sizeof(desired));
    desired.freq = freq ? freq : 48000;
    desired.format = 0x8010; /* AUDIO_S16LSB */
    desired.channels = channels ? channels : 2;
    /* Hardware buffer. 1024 is the known-good value from the build that played music correctly;
     * it was regressed to 2048 alongside the pump change. Override with UNITY_ALSA_BUFFER=<samples>
     * to tune on-device (bigger = fewer underruns at the cost of latency). */
    int samples = 1024;
    const char *bs = getenv("UNITY_ALSA_BUFFER");
    if (bs) { int v = atoi(bs); if (v >= 256 && v <= 65536) samples = v; }
    desired.samples = (uint16_t)samples;
    desired.callback = cb;
    desired.userdata = userdata;
    g_audio_dev = S.OpenAudioDevice(NULL, 0, &desired, &obtained, 0);
    if (!g_audio_dev) {
        so_log("[sdl] OpenAudioDevice failed: %s", S.GetError ? S.GetError() : "unknown");
        return 0;
    }
    /* Start paused; unpaused once initial buffer is primed */
    if (S.PauseAudioDevice) S.PauseAudioDevice(g_audio_dev, 1);
    so_log("[sdl] Audio opened dev=%u (freq=%d channels=%d samples=%d, %s mode) driver=%s",
           g_audio_dev, obtained.freq, obtained.channels, obtained.samples, cb ? "Callback" : "QueueAudio",
           S.GetCurrentAudioDriver ? S.GetCurrentAudioDriver() : "?");
    return 1;
}

void sdl_audio_pause(int pause_on) {
    if (g_audio_dev && S.PauseAudioDevice) S.PauseAudioDevice(g_audio_dev, pause_on);
}

void sdl_audio_queue(const void *data, uint32_t len) {
    if (!g_audio_dev || !S.QueueAudio) return;
    /* UNITY_SWAP_PCM=1 byte-swaps every 16-bit sample before queueing. FMOD reports the OpenSL
     * format's endianness field as 2, and if that means big-endian while this ARM/ALSA path is
     * little-endian, the result is exactly the "plays but is static" symptom. This is a switch so
     * it can be tested on-device without a rebuild; if the rolling PCM dump shows byte-swapped
     * data, set it to 1 in Unreal Life.sh. */
    static int swap = -1;
    if (swap < 0) { const char *e = getenv("UNITY_SWAP_PCM"); swap = (e && e[0] == '1') ? 1 : 0; }
    if (swap && (len & 1u) == 0) {
        static uint8_t buf[4096];
        const uint8_t *src = (const uint8_t *)data;
        uint32_t off = 0;
        while (off < len) {
            uint32_t n = len - off;
            if (n > sizeof buf) n = sizeof buf;
            n &= ~1u;
            if (!n) break;
            for (uint32_t i = 0; i < n; i += 2) { buf[i] = src[off + i + 1]; buf[i + 1] = src[off + i]; }
            S.QueueAudio(g_audio_dev, buf, n);
            off += n;
        }
        return;
    }
    S.QueueAudio(g_audio_dev, data, len);
}

uint32_t sdl_audio_queued_size(void) {
    if (g_audio_dev && S.GetQueuedAudioSize) return S.GetQueuedAudioSize(g_audio_dev);
    return 0;
}

void sdl_audio_close(void) {
    if (g_audio_dev && S.CloseAudioDevice) {
        S.CloseAudioDevice(g_audio_dev);
        g_audio_dev = 0;
    }
}

/* --- Input event translation --- */
volatile int g_user_interacted = 0;
static int s_btn_select = 0;
static int s_btn_start = 0;
static int s_raw_joy_btn[32] = {0};
static int s_ctrl_btn[32] = {0};
static int s_prev_hat = 0;
static float s_stick_x = 0.0f;
static float s_stick_y = 0.0f;

static uint64_t get_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (ts.tv_nsec / 1000000);
}

static void check_rapid_tap_exit(int is_down) {
    if (!is_down) return;
    static uint64_t last_time = 0;
    static int tap_count = 0;
    uint64_t now = get_time_ms();
    if (now - last_time < 900) {
        tap_count++;
        so_log("[sdl] Rapid tap count: %d/4", tap_count);
        if (tap_count >= 4) {
            so_log("[sdl] Rapid tap exit (4x tap on Select/Start) -> exiting cleanly!");
            fflush(NULL);
            _exit(0);
        }
    } else {
        tap_count = 1;
    }
    last_time = now;
}

static float s_dpad_x = 0.0f;
static float s_dpad_y = 0.0f;
static int s_dpad_down[4] = {0}; /* 0: UP, 1: DOWN, 2: LEFT, 3: RIGHT */

static void handle_dpad(int btn_code, int is_down) {
    int idx;
    switch (btn_code) {
        case AKEYCODE_DPAD_UP:    idx = 0; break;
        case AKEYCODE_DPAD_DOWN:  idx = 1; break;
        case AKEYCODE_DPAD_LEFT:  idx = 2; break;
        case AKEYCODE_DPAD_RIGHT: idx = 3; break;
        default: return;
    }
    int now = is_down ? 1 : 0;
    if (s_dpad_down[idx] == now) return;   /* debounce duplicate state (button + hat paths) */
    s_dpad_down[idx] = now;

    /* (1) A real Android gamepad delivers the D-pad as KEYCODE_DPAD_* KeyEvents with
     *     source SOURCE_DPAD. Unity's Android input turns those keycodes into joystick
     *     *buttons*, which is exactly what Rewired reads - and the A/B buttons prove this
     *     KeyEvent path works (they go through the very same android_inject_key()). The old
     *     code never sent these, it only sent AXIS_HAT_X/Y motion events, so the D-pad did
     *     nothing. This is the primary fix. */
    android_inject_key(btn_code, is_down ? 0 : 1);

    /* (2) Optional fallback for a Rewired gamepad definition that binds the D-pad to
     *     AXIS_HAT_X/Y instead of the DPAD keycodes. Off by default: sending both a key and
     *     a hat axis for one press could double-trigger a menu. Enable with UNITY_DPAD_HAT=1
     *     if the keycode path alone still leaves the D-pad dead on device. */
    static int hat_mode = -1;
    if (hat_mode < 0) {
        const char *h = getenv("UNITY_DPAD_HAT");
        hat_mode = (h && h[0] == '1') ? 1 : 0;
    }
    if (hat_mode) {
        float new_x = (s_dpad_down[3] ? 1.0f : 0.0f) - (s_dpad_down[2] ? 1.0f : 0.0f);
        /* In Android MotionEvent AXIS_HAT_Y: -1.0f is UP, +1.0f is DOWN */
        float new_y = (s_dpad_down[1] ? 1.0f : 0.0f) - (s_dpad_down[0] ? 1.0f : 0.0f);
        if (new_x != s_dpad_x || new_y != s_dpad_y) {
            s_dpad_x = new_x;
            s_dpad_y = new_y;
            int axes[2] = {AXIS_HAT_X, AXIS_HAT_Y};
            float vals[2] = {s_dpad_x, s_dpad_y};
            android_inject_axes(2, axes, vals);
        }
    }
}

static void handle_button(int btn_code, int is_down) {
    if (is_down) g_user_interacted = 1;

    if (btn_code == AKEYCODE_BUTTON_SELECT) {
        s_btn_select = is_down;
        if (is_down) check_rapid_tap_exit(1);
    }
    if (btn_code == AKEYCODE_BUTTON_START) {
        s_btn_start = is_down;
        if (is_down) check_rapid_tap_exit(1);
    }

    if (s_btn_select && s_btn_start) {
        so_log("[sdl] Exit combo pressed (Select + Start) -> exiting cleanly!");
        fflush(NULL);
        _exit(0);
    }

    if (btn_code >= AKEYCODE_DPAD_UP && btn_code <= AKEYCODE_DPAD_RIGHT) {
        handle_dpad(btn_code, is_down);
        return;
    }

    /* Inject pure Gamepad KeyEvent (source=GAMEPAD, deviceId=1) */
    android_inject_key(btn_code, is_down ? 0 : 1);
}

static int map_controller_btn(int b) {
    switch (b) {
        case 0: return AKEYCODE_BUTTON_A;
        case 1: return AKEYCODE_BUTTON_B;
        case 2: return AKEYCODE_BUTTON_X;
        case 3: return AKEYCODE_BUTTON_Y;
        case 4: /* BACK */
        case 5: /* GUIDE */
            return AKEYCODE_BUTTON_SELECT;
        case 6: return AKEYCODE_BUTTON_START;
        case 7: return AKEYCODE_BUTTON_THUMBL;
        case 8: return AKEYCODE_BUTTON_THUMBR;
        case 9: return AKEYCODE_BUTTON_L1;
        case 10: return AKEYCODE_BUTTON_R1;
        case 11: return AKEYCODE_DPAD_UP;
        case 12: return AKEYCODE_DPAD_DOWN;
        case 13: return AKEYCODE_DPAD_LEFT;
        case 14: return AKEYCODE_DPAD_RIGHT;
        default: return 0;
    }
}

static int map_joy_btn(int b) {
    switch (b) {
        case 0: return s_swap_ab ? AKEYCODE_BUTTON_B : AKEYCODE_BUTTON_A;
        case 1: return s_swap_ab ? AKEYCODE_BUTTON_A : AKEYCODE_BUTTON_B;
        case 2: return s_swap_ab ? AKEYCODE_BUTTON_Y : AKEYCODE_BUTTON_X;
        case 3: return s_swap_ab ? AKEYCODE_BUTTON_X : AKEYCODE_BUTTON_Y;
        case 4: return AKEYCODE_BUTTON_L1;
        case 5: return AKEYCODE_BUTTON_R1;
        case 6: return AKEYCODE_BUTTON_L2;
        case 7: return AKEYCODE_BUTTON_R2;
        case 8: return AKEYCODE_DPAD_UP;
        case 9: return AKEYCODE_DPAD_DOWN;
        case 10: return AKEYCODE_DPAD_LEFT;
        case 11: return AKEYCODE_DPAD_RIGHT;
        case 12: return AKEYCODE_BUTTON_SELECT;
        case 13: return AKEYCODE_BUTTON_START;
        case 14: return AKEYCODE_BUTTON_THUMBL;
        case 15: return AKEYCODE_BUTTON_THUMBR;
        default: return 0;
    }
}

static int map_key(int sym) {
    switch (sym) {
        case 27: return -1; /* ESC -> exit */
        case 13: /* ENTER */
        case 32: /* SPACE */
        case 122: /* Z */ return AKEYCODE_BUTTON_A;
        case 120: /* X */
        case 8: /* BACKSPACE */ return AKEYCODE_BUTTON_B;
        case 99: /* C */ return AKEYCODE_BUTTON_X;
        case 118: /* V */ return AKEYCODE_BUTTON_Y;
        case 1073741906: /* UP */ return AKEYCODE_DPAD_UP;
        case 1073741905: /* DOWN */ return AKEYCODE_DPAD_DOWN;
        case 1073741904: /* LEFT */ return AKEYCODE_DPAD_LEFT;
        case 1073741903: /* RIGHT */ return AKEYCODE_DPAD_RIGHT;
        default: return 0;
    }
}

/* Pump events on the main thread; returns 1 when the window was closed. */
int sdl_poll(void) {
    uint8_t ev[128];
    int quit = 0;
    while (S.PollEvent(ev)) {
        uint32_t type = *(uint32_t *)ev;
        if (type == SDL_QUIT_EVENT) {
            so_log("[sdl] SDL_QUIT_EVENT received");
            quit = 1;
        }
        else if (type == SDL_CONTROLLERBUTTONDOWN_EVENT || type == SDL_CONTROLLERBUTTONUP_EVENT) {
            uint8_t btn = ev[12];
            uint8_t state = ev[13];
            if (btn < 32) s_ctrl_btn[btn] = (state == 1);
            if (so_verbose) {
                so_log("[sdl] controller btn=%u state=%u (c4=%d c5=%d c6=%d c7=%d c8=%d)",
                       btn, state, s_ctrl_btn[4], s_ctrl_btn[5], s_ctrl_btn[6], s_ctrl_btn[7], s_ctrl_btn[8]);
            }
            if (state == 1 && (btn == 4 || btn == 6)) {
                check_rapid_tap_exit(1);
            }
            /* Controller exit combo: BACK(4) or GUIDE(5) + START(6) */
            if ((s_ctrl_btn[4] && s_ctrl_btn[6]) || (s_ctrl_btn[5] && s_ctrl_btn[6])) {
                so_log("[sdl] Controller exit combo triggered (BACK + START) -> exiting cleanly!");
                fflush(NULL);
                _exit(0);
            }
            /* Controller exit combo: L3(7) + R3(8) */
            if (s_ctrl_btn[7] && s_ctrl_btn[8]) {
                so_log("[sdl] Controller exit combo triggered (L3 + R3) -> exiting cleanly!");
                fflush(NULL);
                _exit(0);
            }
            int code = map_controller_btn(btn);
            if (code) handle_button(code, state == 1);
        }
        else if (type == SDL_JOYBUTTONDOWN_EVENT || type == SDL_JOYBUTTONUP_EVENT) {
            uint8_t btn = ev[12];
            uint8_t state = ev[13];
            if (btn < 32) s_raw_joy_btn[btn] = (state == 1);
            if (so_verbose) {
                so_log("[sdl] joy btn=%u state=%u (b12=%d b13=%d)",
                       btn, state, s_raw_joy_btn[12], s_raw_joy_btn[13]);
            }
            /* Rapid tap exit only on Select(12) or Start(13) */
            if (state == 1 && (btn == 12 || btn == 13)) {
                check_rapid_tap_exit(1);
            }
            /* Raw exit combo 1: SELECT(12) + START(13) on RG351MP joydev */
            if (s_raw_joy_btn[12] && s_raw_joy_btn[13]) {
                so_log("[sdl] Raw joy exit combo triggered (Select[12] + Start[13]) -> exiting cleanly!");
                fflush(NULL);
                _exit(0);
            }
            /* Raw exit combo 2: SELECT(12) + L1(4) + R1(5) */
            if (s_raw_joy_btn[12] && s_raw_joy_btn[4] && s_raw_joy_btn[5]) {
                so_log("[sdl] Raw joy exit combo triggered (Select + L1 + R1) -> exiting cleanly!");
                fflush(NULL);
                _exit(0);
            }
            /* Raw exit combo 3: L3(14) + R3(15) */
            if (s_raw_joy_btn[14] && s_raw_joy_btn[15]) {
                so_log("[sdl] Raw joy exit combo triggered (L3[14] + R3[15]) -> exiting cleanly!");
                fflush(NULL);
                _exit(0);
            }

            /* Route joystick buttons to the game. Usually only when no GameController was
             * opened (the controller path handles buttons 0-15 itself). The exception is the
             * D-pad: on some RK3326 units the D-pad is exposed as raw buttons 8-11, which the
             * controller mapping above does not cover, so always route those four. handle_dpad
             * is idempotent, so a duplicate press cannot double-fire. */
            int code = map_joy_btn(btn);
            if (code && (!g_has_gamecontroller || (code >= AKEYCODE_DPAD_UP && code <= AKEYCODE_DPAD_RIGHT)))
                handle_button(code, state == 1);
        }
        else if (type == SDL_JOYHATMOTION_EVENT) {
            /* On RK3326 (GO-Super / GO-Advance) the physical D-pad is ABS_HAT0X/Y, not buttons.
             * SDL's game-controller layer only maps the hat to dpad buttons when the mapping uses
             * `dpup:h0.1` bindings; ours (and ArkOS's db) bind dp*:b8..b11, so the controller layer
             * emits NOTHING for the D-pad. The raw SDL_JOYHATMOTION is therefore the only source of
             * D-pad input, and it must be honoured even while a GameController is open.
             * handle_dpad() is idempotent per direction, so this cannot double-fire if a future
             * mapping also produces D-pad button events. */
            uint8_t val = ev[13];
            int up = (val & 1) != 0, prev_up = (s_prev_hat & 1) != 0;
            int right = (val & 2) != 0, prev_right = (s_prev_hat & 2) != 0;
            int down = (val & 4) != 0, prev_down = (s_prev_hat & 4) != 0;
            int left = (val & 8) != 0, prev_left = (s_prev_hat & 8) != 0;
            if (up != prev_up) handle_button(AKEYCODE_DPAD_UP, up);
            if (down != prev_down) handle_button(AKEYCODE_DPAD_DOWN, down);
            if (left != prev_left) handle_button(AKEYCODE_DPAD_LEFT, left);
            if (right != prev_right) handle_button(AKEYCODE_DPAD_RIGHT, right);
            s_prev_hat = val;
        }
        else if (type == SDL_CONTROLLERAXISMOTION_EVENT || (!g_has_gamecontroller && type == SDL_JOYAXISMOTION_EVENT)) {
            uint8_t axis = ev[12];
            int16_t val = *(int16_t *)&ev[16];
            const int deadzone = 8000;
            float norm = 0.0f;
            if (val < -deadzone) norm = (float)(val + deadzone) / (32768.0f - deadzone);
            else if (val > deadzone) norm = (float)(val - deadzone) / (32767.0f - deadzone);

            if (axis == 0) {
                if (fabsf(norm - s_stick_x) > 0.01f) {
                    s_stick_x = norm;
                    android_inject_axis(AXIS_X, norm);
                }
            } else if (axis == 1) {
                if (fabsf(norm - s_stick_y) > 0.01f) {
                    s_stick_y = norm;
                    android_inject_axis(AXIS_Y, norm);
                }
            }
        }
        else if (type == SDL_KEYDOWN_EVENT || type == SDL_KEYUP_EVENT) {
            int32_t sym = *(int32_t *)&ev[20];
            so_log("[sdl] key sym=%d state=%d", sym, type == SDL_KEYDOWN_EVENT);
            int code = map_key(sym);
            if (code == -1) {
                so_log("[sdl] ESC pressed -> exiting cleanly!");
                fflush(NULL);
                _exit(0);
            }
            if (code) handle_button(code, type == SDL_KEYDOWN_EVENT);
        }
        else if (type == SDL_MOUSEBUTTONDOWN_EVENT || type == SDL_MOUSEBUTTONUP_EVENT) {
            const char *allow_mouse = getenv("UNITY_ALLOW_MOUSE");
            if (allow_mouse && !strcmp(allow_mouse, "1")) {
                int32_t mx = *(int32_t *)&ev[16];
                int32_t my = *(int32_t *)&ev[20];
                android_inject_touch((float)mx, (float)my, (type == SDL_MOUSEBUTTONDOWN_EVENT) ? 0 : 1);
            }
        }
    }
    return quit;
}
