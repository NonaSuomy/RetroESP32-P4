/*
 * PAPP Touch Test — verify the touch_read service and its coordinate mapping.
 *
 * Draws four colored touch boxes (to confirm canvas orientation) and a
 * white crosshair wherever you touch. touch_read reports 180-degree-corrected
 * LANDSCAPE native-framebuffer space (x:0..799, y:0..479); this app draws into a
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

void *memcpy(void *dst, const void *src, unsigned int n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    while (n--) *d++ = *s++;
    return dst;
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
#define C_CLOSE   0x7800

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
    int touch_down = 0;
    int selected_box = -1;
    unsigned int touch_sequence = 0;
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
            /* The launcher rotates the logical PAPP canvas 180 degrees.  A
             * logical bottom-left hit therefore appears at the physical
             * top-right of the landscape LCD. */
            const int close_x = 16, close_y = FB_H - 12 - 32;
            if (cx >= close_x && cx < close_x + 54 &&
                cy >= close_y && cy < close_y + 32) {
                svc->log_printf("touch close button\n");
                break;
            }
            if (!touch_down) {
                /* A fresh press selects one of four large test boxes.  The
                 * selected box changes on every new press, which makes it
                 * obvious that release/re-press is still being delivered. */
                const int right = cx >= FB_W / 2;
                const int bottom = cy >= FB_H / 2;
                selected_box = (bottom ? 2 : 0) + (right ? 1 : 0);
                touch_sequence++;
                svc->log_printf("touch press #%u box=%d\n", touch_sequence, selected_box);
                touch_down = 1;
            }
            if (tx != last_tx || ty != last_ty) {
                svc->log_printf("touch  native=(%d,%d)  canvas=(%d,%d)\n",
                                tx, ty, cx, cy);
                last_tx = tx; last_ty = ty;
            }
        } else {
            touch_down = 0;
        }

        /* ── Draw ─────────────────────────────────────────────────────── */
        fill_rect(fb, 0, 0, FB_W, FB_H, C_BG);

        /* Four large boxes: TL red, TR green, BL blue, BR yellow. */
        const int box_w = 100, box_h = 52;
        const int box_x[4] = { 78, FB_W - 78 - box_w, 78, FB_W - 78 - box_w };
        const int box_y[4] = { 28, 28, FB_H - 28 - box_h, FB_H - 28 - box_h };
        const uint16_t box_color[4] = { C_RED, C_GREEN, C_BLUE, C_YELLOW };
        for (int i = 0; i < 4; i++) {
            fill_rect(fb, box_x[i], box_y[i], box_w, box_h, box_color[i]);
            if (selected_box == i) {
                /* White inset marks the box selected by the latest press. */
                fill_rect(fb, box_x[i] + 5, box_y[i] + 5, box_w - 10, 4, C_WHITE);
                fill_rect(fb, box_x[i] + 5, box_y[i] + box_h - 9, box_w - 10, 4, C_WHITE);
                fill_rect(fb, box_x[i] + 5, box_y[i] + 5, 4, box_h - 10, C_WHITE);
                fill_rect(fb, box_x[i] + box_w - 9, box_y[i] + 5, 4, box_h - 10, C_WHITE);
            }
        }

        /* This is logical bottom-left so the launcher's 180-degree direct
         * PAPP flush places it at the physical top-right of the LCD. */
        const int close_x = 16, close_y = FB_H - 12 - 32;
        fill_rect(fb, close_x, close_y, 54, 32, C_CLOSE);
        for (int n = 0; n < 4; n++) {
            fill_rect(fb, close_x + 11 + n, close_y + 8 + n, 4, 4, C_WHITE);
            fill_rect(fb, close_x + 39 - n, close_y + 8 + n, 4, 4, C_WHITE);
            fill_rect(fb, close_x + 11 + n, close_y + 20 - n, 4, 4, C_WHITE);
            fill_rect(fb, close_x + 39 - n, close_y + 20 - n, 4, 4, C_WHITE);
        }

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
