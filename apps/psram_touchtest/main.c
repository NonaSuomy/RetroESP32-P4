/*
 * PAPP Touch Test — verify the touch_read service and its coordinate mapping.
 *
 * Draws four colored corner markers (to confirm canvas orientation) and a
 * white crosshair wherever you touch. touch_read reports 180-degree-corrected LANDSCAPE
 * native-framebuffer space (x:0..799, y:0..479); this app draws into a
 * 400x240 canvas scaled x2.0 -> 480x800, so canvas = touch / 2.
 *
 * Controls:  touch = move crosshair   |   X button = exit
 *
 * NOTE: exit is the physical X button, NOT MENU. On the LCD board
 * input_gamepad_read() synthesizes MENU from the top touch strip
 * (odroid_input.c), so a MENU-to-exit would quit as soon as you touch
 * the top of the screen. Touch apps should exit on a physical button.
 */

#define PAPP_APP_SIDE 1
#include "psram_app.h"

/* Minimal C runtime for the -nostdlib build */
void *memset(void *s, int c, unsigned int n)
{
    unsigned char *p = (unsigned char *)s;
    while (n--) *p++ = (unsigned char)c;
    return s;
}

#define FB_W  400
#define FB_H  240
#define FB_BYTES (FB_W * FB_H * sizeof(uint16_t))

#define C_BG      0x18E3   /* dark grey  */
#define C_WHITE   0xFFFF
#define C_RED     0xF800
#define C_GREEN   0x07E0
#define C_BLUE    0x001F
#define C_YELLOW  0xFFE0

#define MARK 28   /* corner marker size (px) */
#define CROSS 12  /* crosshair half-length    */

/* Filled rectangle with clipping to the canvas bounds. */
static void fill_rect(uint16_t *fb, int x, int y, int w, int h, uint16_t color)
{
    int x0 = x, y0 = y, x1 = x + w, y1 = y + h;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > FB_W) x1 = FB_W;
    if (y1 > FB_H) y1 = FB_H;
    for (int yy = y0; yy < y1; yy++) {
        uint16_t *row = &fb[yy * FB_W];
        for (int xx = x0; xx < x1; xx++) row[xx] = color;
    }
}

__attribute__((section(".text.entry")))
int app_entry(const app_services_t *svc)
{
    svc->log_printf("=== PAPP Touch Test ===\n");

    if (svc->abi_version != PAPP_ABI_VERSION) {
        svc->log_printf("ABI mismatch: got %lu expected %d\n",
                        (unsigned long)svc->abi_version, PAPP_ABI_VERSION);
        return -1;
    }

    /* touch_read is appended after ABI v1 — guard against an older launcher */
    if (!svc->touch_read) {
        svc->log_printf("ERROR: this launcher has no touch_read service.\n");
        svc->log_printf("Flash the updated launcher, then retry.\n");
        return -1;
    }

    uint16_t *fb = (uint16_t *)svc->mem_caps_alloc(
        FB_BYTES, PAPP_MEM_CAP_SPIRAM | PAPP_MEM_CAP_DMA);
    if (!fb) {
        svc->log_printf("ERROR: framebuffer alloc failed\n");
        return -1;
    }

    int cx = FB_W / 2, cy = FB_H / 2;   /* current crosshair (canvas coords) */
    int have_touch = 0;
    int last_tx = -1, last_ty = -1;     /* for throttled logging */

    papp_gamepad_state_t pad;

    for (;;) {
        svc->input_gamepad_read(&pad);
        if (pad.values[PAPP_INPUT_X]) break;   /* physical X = exit (not MENU — see header) */

        int tx, ty;
        if (svc->touch_read(&tx, &ty)) {
            have_touch = 1;
            cx = tx / 2;                 /* 800x480 native -> 400x240 canvas */
            cy = ty / 2;
            if (tx != last_tx || ty != last_ty) {
                svc->log_printf("touch  native=(%d,%d)  canvas=(%d,%d)\n",
                                tx, ty, cx, cy);
                last_tx = tx; last_ty = ty;
            }
        }

        /* ── Draw ─────────────────────────────────────────────────────── */
        fill_rect(fb, 0, 0, FB_W, FB_H, C_BG);

        /* Corner markers: TL red, TR green, BL blue, BR yellow */
        fill_rect(fb, 0,          0,          MARK, MARK, C_RED);
        fill_rect(fb, FB_W - MARK, 0,         MARK, MARK, C_GREEN);
        fill_rect(fb, 0,          FB_H - MARK, MARK, MARK, C_BLUE);
        fill_rect(fb, FB_W - MARK, FB_H - MARK, MARK, MARK, C_YELLOW);

        /* Crosshair at the last touch point */
        if (have_touch) {
            fill_rect(fb, cx - CROSS, cy - 1, CROSS * 2, 3, C_WHITE);  /* horizontal */
            fill_rect(fb, cx - 1, cy - CROSS, 3, CROSS * 2, C_WHITE);  /* vertical   */
            fill_rect(fb, cx - 3, cy - 3, 6, 6, C_RED);                /* center dot */
        }

        svc->display_write_frame_custom(fb, FB_W, FB_H, 2.0f, false);
        svc->delay_ms(10);
    }

    svc->mem_free(fb);
    svc->display_clear(C_BG);
    svc->display_flush();
    svc->log_printf("=== Touch Test — exit ===\n");
    return 0;
}
