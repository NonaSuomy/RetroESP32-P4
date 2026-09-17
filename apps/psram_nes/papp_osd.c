/* PAPP OSD bridge for nofrendo. */
#define PAPP_APP_SIDE 1
#include "psram_app.h"
#include "nofrendo.h"
#include "osd.h"
#include "bitmap.h"
#include "vid_drv.h"
#include "event.h"
#include "nesinput.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const app_services_t *_papp_svc;
extern volatile int papp_nes_exit;
extern volatile int papp_exit_requested;
extern void papp_nes_request_exit(void);
extern void main_quit(void);

#define W 256
#define H 224
static bitmap_t *screen;
static uint8_t *indexed;
static uint16_t *rgb565;
static uint16_t palette[256];
static void (*sound_cb)(void *, int);
static unsigned papp_video_frames;
static unsigned papp_audio_frames;
static unsigned papp_audio_submit_calls;

void papp_nes_frame_pace(int64_t frame_start)
{
    const int64_t frame_us = 16667; /* 60.00 Hz */
    const int64_t elapsed = _papp_svc->get_time_us() - frame_start;

    int64_t remaining = frame_us - elapsed;
    if (remaining >= 1000)
        /* Floor the millisecond sleep. Rounding up every frame makes the
         * emulator run at roughly 58.8 Hz instead of the NES refresh rate. */
        _papp_svc->delay_ms((int)(remaining / 1000));

    /* Do not log from the frame pacer: the standalone launcher's log service
     * can block for 100 ms when USB JTAG has no reader (UART is separate). */
}

static int vinit(int w, int h) { (void)w; (void)h; return 0; }
static void vshutdown(void) {}
static int vmode(int w, int h) { (void)w; (void)h; return 0; }
static void vpalette(rgb_t *p) {
    for (int i = 0; i < 256; ++i)
        palette[i] = (uint16_t)(((p[i].r >> 3) << 11) | ((p[i].g >> 2) << 5) | (p[i].b >> 3));
}
static void vclear(uint8_t c) { if (indexed) memset(indexed, c, W * H); }
static bitmap_t *vlock(void) { if (!screen) screen = bmp_createhw(indexed, W, H, W); return screen; }
static void vfree(int n, rect_t *r) { (void)n; (void)r; }
static void vblit(bitmap_t *b, int n, rect_t *r) {
    (void)n; (void)r;
    if (!b || !b->line[0] || !_papp_svc) return;
    unsigned nonzero_pixels = 0;
    for (int i = 0; i < W * H; ++i) {
        rgb565[i] = palette[b->line[0][i]];
        nonzero_pixels += rgb565[i] != 0;
    }
    if (papp_video_frames < 4)
        _papp_svc->log_printf("NES PAPP: submitting video frame=%u\n",
                              papp_video_frames + 1u);
    _papp_svc->display_write_frame_custom(rgb565, W, H, 2.0f, false);
    ++papp_video_frames;
    if ((papp_video_frames % 60u) == 0u)
        _papp_svc->log_printf("NES PAPP: video frames=%u nonzero_pixels=%u\n",
                              papp_video_frames, nonzero_pixels);
}
static viddriver_t driver = { "PAPP", vinit, vshutdown, vmode, vpalette, vclear, vlock, vfree, vblit, false };

int osd_main(int argc, char **argv)
{
    (void)argc;
    return main_loop(argv[0], system_nes);
}

void osd_getvideoinfo(vidinfo_t *i) { i->default_width = W; i->default_height = H; i->driver = &driver; }
void osd_getsoundinfo(sndinfo_t *i) { i->sample_rate = 32000; i->bps = 16; }
int osd_installtimer(int f, void *fn, int fs, void *c, int cs) { (void)f;(void)fn;(void)fs;(void)c;(void)cs; return 0; }
void osd_setsound(void (*fn)(void *, int)) { sound_cb = fn; }
void osd_getmouse(int *x, int *y, int *b) { if(x)*x=0;if(y)*y=0;if(b)*b=0; }
void osd_togglefullscreen(int c) { (void)c; }
void osd_fullname(char *d, const char *s) { strcpy(d, s); }
char *osd_newextension(char *s, char *e) { (void)e; return s; }
int osd_makesnapname(char *s, int n) { (void)s;(void)n; return -1; }
void osd_freeinput(void) {}

void osd_getinput(void)
{
    static int old;
    papp_gamepad_state_t p;
    int now = 0;
    _papp_svc->input_gamepad_read(&p);
    if (p.values[PAPP_INPUT_SELECT]) now |= 1 << 0;
    if (p.values[PAPP_INPUT_START])  now |= 1 << 3;
    if (p.values[PAPP_INPUT_UP])     now |= 1 << 4;
    if (p.values[PAPP_INPUT_RIGHT])  now |= 1 << 5;
    if (p.values[PAPP_INPUT_DOWN])   now |= 1 << 6;
    if (p.values[PAPP_INPUT_LEFT])   now |= 1 << 7;
    if (p.values[PAPP_INPUT_A])      now |= 1 << 13;
    if (p.values[PAPP_INPUT_B])      now |= 1 << 14;
    if (p.values[PAPP_INPUT_MENU] || (p.values[PAPP_INPUT_L] && p.values[PAPP_INPUT_R]) || papp_exit_requested)
        main_quit();
    int changed = now ^ old;
    const int events[16] = { event_joypad1_select,0,0,event_joypad1_start,event_joypad1_up,event_joypad1_right,event_joypad1_down,event_joypad1_left,0,0,0,0,0,event_joypad1_a,event_joypad1_b,0 };
    for (int i = 0; i < 16; ++i) if ((changed & (1 << i)) && events[i]) {
        event_t fn = event_get(events[i]);
        if (fn) fn((now & (1 << i)) ? INP_STATE_MAKE : INP_STATE_BREAK);
    }
    old = now;
}

int osd_init(void) { indexed = _papp_svc->mem_alloc(W * H); rgb565 = _papp_svc->mem_caps_alloc(W * H * 2, PAPP_MEM_CAP_SPIRAM | PAPP_MEM_CAP_DMA); return indexed && rgb565 ? 0 : -1; }
void osd_shutdown(void) { bmp_destroy(&screen); if (indexed) _papp_svc->mem_free(indexed); if (rgb565) _papp_svc->mem_free(rgb565); indexed = NULL; rgb565 = NULL; sound_cb = NULL; }
void osd_main_dummy(void) {}

/* Called by the nofrendo APU bridge in papp_audio.c. */
void do_audio_frame(void) {
    static unsigned remainder;
    if (!sound_cb) return;
    /* 32000 / 60 = 533, 533, 534 samples; 512 underfeeds the speaker. */
    remainder += 32000;
    int samples = remainder / 60;
    remainder %= 60;
    short stereo[534 * 2];
    sound_cb(stereo, samples);
    int peak = 0;
    for (int i = 0; i < samples; ++i) {
        int value = stereo[i];
        if (value < 0) value = -value;
        if (value > peak) peak = value;
    }
    for (int i = samples - 1; i >= 0; --i) {
        stereo[i * 2] = stereo[i];
        stereo[i * 2 + 1] = stereo[i];
    }
    if (papp_audio_submit_calls < 8u)
        _papp_svc->log_printf("NES PAPP: audio submit begin=%u peak=%d\n",
                              papp_audio_submit_calls + 1u, peak);
    _papp_svc->audio_submit(stereo, samples);
    if (papp_audio_submit_calls < 8u)
        _papp_svc->log_printf("NES PAPP: audio submit end=%u\n",
                              papp_audio_submit_calls + 1u);
    ++papp_audio_submit_calls;
    ++papp_audio_frames;
    if ((papp_audio_frames % 60u) == 0u)
        _papp_svc->log_printf("NES PAPP: audio frames=%u peak=%d\n",
                              papp_audio_frames, peak);
}
