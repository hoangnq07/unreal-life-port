#ifndef ANDROID_NATIVE_H
#define ANDROID_NATIVE_H

#include <stdint.h>

void android_native_init(void);
void *android_gl_proc(const char *name);    /* GLES entry point lookup (null GL or SDL) */
extern int g_screen_w, g_screen_h;

/* Android keycodes */
#define AKEYCODE_DPAD_UP 19
#define AKEYCODE_DPAD_DOWN 20
#define AKEYCODE_DPAD_LEFT 21
#define AKEYCODE_DPAD_RIGHT 22
#define AKEYCODE_BUTTON_A 96
#define AKEYCODE_BUTTON_B 97
#define AKEYCODE_BUTTON_X 99
#define AKEYCODE_BUTTON_Y 100
#define AKEYCODE_BUTTON_L1 102
#define AKEYCODE_BUTTON_R1 103
#define AKEYCODE_BUTTON_L2 104
#define AKEYCODE_BUTTON_R2 105
#define AKEYCODE_BUTTON_THUMBL 106
#define AKEYCODE_BUTTON_THUMBR 107
#define AKEYCODE_BUTTON_START 108
#define AKEYCODE_BUTTON_SELECT 109

/* Android motion axes */
#define AXIS_X 0
#define AXIS_Y 1
#define AXIS_Z 11
#define AXIS_RZ 14
#define AXIS_HAT_X 15
#define AXIS_HAT_Y 16
#define AXIS_LTRIGGER 17
#define AXIS_RTRIGGER 18

/* Event injection for Unity */
void android_inject_key(int keycode, int action);
void android_inject_touch(float x, float y, int action);
void android_inject_axis(int axis, float val);
void android_inject_axes(int count, const int *axes, const float *vals);

/* SDL2 backend (sdl_gl.c), enabled with UNITY_GL=sdl */
int sdl_gl_init(void);                      /* main thread only; returns 0 on failure */
int sdl_gl_active(void);
void *sdl_gl_create_context(void);
int sdl_gl_make_current(void *ctx);         /* ctx == NULL releases the current context */
void sdl_gl_swap(void);
void sdl_gl_swap_interval(int n);
void *sdl_gl_get_proc(const char *name);
int sdl_poll(void);                         /* main thread; returns 1 when the user asked to quit */

/* SDL2 audio backend for OpenSL ES */
int sdl_audio_init(int freq, int channels, void (*cb)(void *, uint8_t *, int), void *userdata);
void sdl_audio_pause(int pause_on);
void sdl_audio_queue(const void *data, uint32_t len);
uint32_t sdl_audio_queued_size(void);
void sdl_audio_close(void);

#endif
