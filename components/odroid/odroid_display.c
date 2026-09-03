/*
 * Odroid Display Compatibility Layer — ESP32-P4 Implementation
 *
 * Manages an 800×480 RGB565 framebuffer in PSRAM (native landscape
 * resolution). On display_flush(), PPA rotates it 180° and centers it on
 * the 1024×600 MIPI DSI LCD.
 */

#include "odroid_display.h"
#include "ppa_engine.h"
#include "pins_config.h"

#ifdef CONFIG_HDMI_OUTPUT
#include "hdmi_display.h"
#include "odroid_system.h"
#include "esp_cache.h"
#else
#include "st7701_lcd.h"
#endif

#include <string.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "driver/ledc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "odroid_display";

#ifdef CONFIG_HDMI_OUTPUT
/* HDMI: 640×480 landscape, RGB888 (3 bytes/pixel) via LT8912 */
#define FB_W  640
#define FB_H  480
static hdmi_display_t s_hdmi_disp;
static bool s_hdmi_initialized = false;
#else
/* LCD: 800×480 landscape, centered on the 1024×600 panel */
#define FB_W  800
#define FB_H  480
#endif

#define FB_PIXELS (FB_W * FB_H)
#define FB_SIZE   (FB_PIXELS * sizeof(uint16_t))  /* internal drawing is always RGB565 */

static uint16_t *s_framebuffer = NULL;
static bool s_fb_dirty = false;
static bool s_backlight_init = false;

/* ─── Backlight (LEDC on GPIO23) — LCD only ───────────────────── */
#ifndef CONFIG_HDMI_OUTPUT
#define BL_GPIO       LCD_BK_LIGHT_GPIO
#define BL_LEDC_CH    LEDC_CHANNEL_0
#define BL_LEDC_TIMER LEDC_TIMER_0
#define BL_DUTY_RES   LEDC_TIMER_13_BIT
#define BL_DUTY_MAX   ((1 << 13) - 1)  /* 8191 */
#define BL_FREQ_HZ    5000

static void backlight_init(void)
{
    ledc_timer_config_t timer_cfg = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .duty_resolution = BL_DUTY_RES,
        .timer_num       = BL_LEDC_TIMER,
        .freq_hz         = BL_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&timer_cfg);

    ledc_channel_config_t ch_cfg = {
        .gpio_num   = BL_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = BL_LEDC_CH,
        .timer_sel  = BL_LEDC_TIMER,
        .duty       = BL_DUTY_MAX,  /* start at full brightness */
        .hpoint     = 0,
    };
    ledc_channel_config(&ch_cfg);
    ledc_fade_func_install(0);
    s_backlight_init = true;
    ESP_LOGI(TAG, "Backlight LEDC initialized on GPIO %d", BL_GPIO);
}
#endif /* !CONFIG_HDMI_OUTPUT */

/* ─── Pre-allocated PPA output buffer ──────────────────────────── */
#ifdef CONFIG_HDMI_OUTPUT
/* HDMI: PPA output is 640×480 RGB888 = 921,600 bytes.
 * But we write directly to the HDMI DPI framebuffer (s_hdmi_disp.fb),
 * so we only need a temporary PPA buffer for the launcher flush
 * (RGB565→RGB888 conversion). Emulators also use this. */
#define HDMI_OUT_W     640
#define HDMI_OUT_H     480
#define HDMI_OUT_BPP   3
#define HDMI_OUT_SIZE  (HDMI_OUT_W * HDMI_OUT_H * HDMI_OUT_BPP)  /* 921600 */
#define PPA_BUF_ALIGN  64
/* s_ppa_out_buf not needed for HDMI — we write directly to s_hdmi_disp.fb */
#else
/* LCD: Max PPA output: full 1024×600 panel. */
#define PPA_OUT_MAX_W  1024
#define PPA_OUT_MAX_H  600
#define PPA_OUT_MAX_SIZE (PPA_OUT_MAX_W * PPA_OUT_MAX_H * sizeof(uint16_t))  /* 768000 */
#define PPA_BUF_ALIGN 64
#define PPA_OUT_ALIGNED ((PPA_OUT_MAX_SIZE + PPA_BUF_ALIGN - 1) & ~(PPA_BUF_ALIGN - 1))

static void *s_ppa_out_buf = NULL;
static size_t s_ppa_out_size = 0;
#endif

/* Emulator standard resolution (all Pipeline A emulators scale to this) */
#define EMU_W 320
#define EMU_H 240
#define EMU_PIXELS (EMU_W * EMU_H)
#define EMU_SIZE   (EMU_PIXELS * sizeof(uint16_t))

/* Shared 320×240 intermediate buffer for Pipeline A emulators */
static uint16_t *s_emu_scaled = NULL;
#ifndef CONFIG_HDMI_OUTPUT
static bool s_emu_borders_cleared_a = false;
#endif

/* Configurable scale factors (1×1 = native resolution → landscape LCD) */
static float s_scale_x = 1.0f;
static float s_scale_y = 1.0f;

/* Physical touch/display geometry. The launcher is rendered into an
 * 800×480 landscape viewport centered in the 1024×600 panel, then rotated
 * 180° by PPA. Emulator output has its own layout, updated before each flush. */
#ifndef CONFIG_HDMI_OUTPUT
static volatile uint16_t s_touch_game_x = 192;
static volatile uint16_t s_touch_game_y = 60;
static volatile uint16_t s_touch_game_w = 640;
static volatile uint16_t s_touch_game_h = 480;
#endif
static volatile bool s_touch_controls_enabled = false;
#ifndef CONFIG_HDMI_OUTPUT
static bool s_touch_side_controls_drawn = false;
static bool s_touch_side_controls_visible = false;
#endif

#define TOUCH_UI_W 800
#define TOUCH_UI_H 480
#define TOUCH_LCD_W 1024
#define TOUCH_LCD_H 600
#define TOUCH_UI_X0 ((TOUCH_LCD_W - TOUCH_UI_W) / 2)
#define TOUCH_UI_Y0 ((TOUCH_LCD_H - TOUCH_UI_H) / 2)

bool odroid_display_touch_to_ui(uint16_t touch_x, uint16_t touch_y,
                                int *ui_x, int *ui_y)
{
#ifdef CONFIG_HDMI_OUTPUT
    (void)touch_x; (void)touch_y; (void)ui_x; (void)ui_y;
    return false;
#else
    if (!ui_x || !ui_y || touch_x < TOUCH_UI_X0 || touch_y < TOUCH_UI_Y0 ||
        touch_x >= TOUCH_UI_X0 + TOUCH_UI_W ||
        touch_y >= TOUCH_UI_Y0 + TOUCH_UI_H) {
        return false;
    }

    /* The framebuffer is sent through a 180° PPA rotation. */
    *ui_x = TOUCH_UI_W - 1 - (int)(touch_x - TOUCH_UI_X0);
    *ui_y = TOUCH_UI_H - 1 - (int)(touch_y - TOUCH_UI_Y0);
    return true;
#endif
}

void odroid_display_set_touch_game_layout(uint16_t x, uint16_t y,
                                          uint16_t w, uint16_t h)
{
#ifndef CONFIG_HDMI_OUTPUT
    if (w == 0 || h == 0) return;
    if (x != s_touch_game_x || y != s_touch_game_y ||
        w != s_touch_game_w || h != s_touch_game_h) {
        s_touch_side_controls_drawn = false;
    }
    s_touch_game_x = x;
    s_touch_game_y = y;
    s_touch_game_w = w;
    s_touch_game_h = h;
#else
    (void)x; (void)y; (void)w; (void)h;
#endif
}

void odroid_display_get_touch_game_layout(uint16_t *x, uint16_t *y,
                                          uint16_t *w, uint16_t *h)
{
#ifdef CONFIG_HDMI_OUTPUT
    if (x) *x = 0;
    if (y) *y = 0;
    if (w) *w = 0;
    if (h) *h = 0;
#else
    if (x) *x = s_touch_game_x;
    if (y) *y = s_touch_game_y;
    if (w) *w = s_touch_game_w;
    if (h) *h = s_touch_game_h;
#endif
}

bool odroid_display_touch_to_game(uint16_t touch_x, uint16_t touch_y,
                                  int *game_x, int *game_y)
{
#ifdef CONFIG_HDMI_OUTPUT
    (void)touch_x; (void)touch_y; (void)game_x; (void)game_y;
    return false;
#else
    if (!game_x || !game_y) return false;
    uint16_t x0 = s_touch_game_x, y0 = s_touch_game_y;
    uint16_t w = s_touch_game_w, h = s_touch_game_h;
    if (w == 0 || h == 0 || touch_x < x0 || touch_y < y0 ||
        touch_x >= x0 + w || touch_y >= y0 + h) return false;

    /* The LCD receives the emulator framebuffer after a 180° PPA rotation.
     * Convert the physical point back into the source framebuffer space so
     * an app's touch UI follows the pixels the user is actually touching. */
    int physical_x = ((int)(touch_x - x0) * TOUCH_UI_W) / w;
    int physical_y = ((int)(touch_y - y0) * TOUCH_UI_H) / h;
    if (physical_x >= TOUCH_UI_W) physical_x = TOUCH_UI_W - 1;
    if (physical_y >= TOUCH_UI_H) physical_y = TOUCH_UI_H - 1;
    *game_x = TOUCH_UI_W - 1 - physical_x;
    *game_y = TOUCH_UI_H - 1 - physical_y;
    return true;
#endif
}

void odroid_display_touch_controls_set_enabled(bool enabled)
{
    s_touch_controls_enabled = enabled;
#ifndef CONFIG_HDMI_OUTPUT
    s_touch_side_controls_drawn = false;
#endif
    ESP_LOGI(TAG, "Landscape touch controls %s", enabled ? "enabled" : "disabled");
}

#ifndef CONFIG_HDMI_OUTPUT
static uint16_t touch_blend(uint16_t dst, uint16_t src)
{
    uint16_t r = (((dst >> 11) & 0x1F) + ((src >> 11) & 0x1F)) >> 1;
    uint16_t g = (((dst >> 5)  & 0x3F) + ((src >> 5)  & 0x3F)) >> 1;
    uint16_t b = ((dst & 0x1F) + (src & 0x1F)) >> 1;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

static void touch_fill_rect(uint16_t *buf, int stride, int width, int height,
                            int x, int y, int w, int h, uint16_t color)
{
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + w > width ? width : x + w;
    int y1 = y + h > height ? height : y + h;
    for (int yy = y0; yy < y1; yy++) {
        for (int xx = x0; xx < x1; xx++) {
            buf[yy * stride + xx] = touch_blend(buf[yy * stride + xx], color);
        }
    }
}

static void touch_rect_border(uint16_t *buf, int stride, int width, int height,
                              int x, int y, int w, int h, uint16_t color)
{
    for (int xx = x; xx < x + w; xx++) {
        if (xx >= 0 && xx < width) {
            if (y >= 0 && y < height) buf[y * stride + xx] = color;
            if (y + h - 1 >= 0 && y + h - 1 < height)
                buf[(y + h - 1) * stride + xx] = color;
        }
    }
    for (int yy = y; yy < y + h; yy++) {
        if (yy >= 0 && yy < height) {
            if (x >= 0 && x < width) buf[yy * stride + x] = color;
            if (x + w - 1 >= 0 && x + w - 1 < width)
                buf[yy * stride + x + w - 1] = color;
        }
    }
}

static void touch_circle(uint16_t *buf, int stride, int width, int height,
                         int cx, int cy, int radius, uint16_t color)
{
    int r2 = radius * radius;
    int inner = radius > 3 ? (radius - 3) * (radius - 3) : 0;
    for (int y = cy - radius; y <= cy + radius; y++) {
        if (y < 0 || y >= height) continue;
        for (int x = cx - radius; x <= cx + radius; x++) {
            if (x < 0 || x >= width) continue;
            int dx = x - cx, dy = y - cy;
            int d2 = dx * dx + dy * dy;
            if (d2 <= r2) {
                if (d2 >= inner) buf[y * stride + x] = color;
                else buf[y * stride + x] = touch_blend(buf[y * stride + x], color);
            }
        }
    }
}

/* Small 3×5 glyphs are enough to label the virtual controls without pulling
 * a full font into every emulator image. */
static void touch_glyph(uint16_t *buf, int stride, int width, int height,
                        int x, int y, char c, uint16_t color)
{
    static const uint8_t glyph_a[5] = {0x2, 0x5, 0x7, 0x5, 0x5};
    static const uint8_t glyph_b[5] = {0x6, 0x5, 0x6, 0x5, 0x6};
    static const uint8_t glyph_s[5] = {0x7, 0x4, 0x7, 0x1, 0x7};
    static const uint8_t glyph_e[5] = {0x7, 0x4, 0x6, 0x4, 0x7};
    static const uint8_t glyph_l[5] = {0x4, 0x4, 0x4, 0x4, 0x7};
    static const uint8_t glyph_t[5] = {0x7, 0x2, 0x2, 0x2, 0x2};
    static const uint8_t glyph_r[5] = {0x6, 0x5, 0x6, 0x5, 0x5};
    static const uint8_t glyph_m[5] = {0x5, 0x7, 0x7, 0x5, 0x5};
    static const uint8_t glyph_n[5] = {0x5, 0x7, 0x7, 0x7, 0x5};
    static const uint8_t glyph_o[5] = {0x7, 0x5, 0x5, 0x5, 0x7};
    static const uint8_t glyph_u[5] = {0x5, 0x5, 0x5, 0x5, 0x7};
    static const uint8_t glyph_v[5] = {0x5, 0x5, 0x5, 0x5, 0x2};
    const uint8_t *g = NULL;
    switch (c) {
    case 'A': g = glyph_a; break; case 'B': g = glyph_b; break;
    case 'S': g = glyph_s; break; case 'E': g = glyph_e; break;
    case 'L': g = glyph_l; break; case 'T': g = glyph_t; break;
    case 'R': g = glyph_r; break; case 'M': g = glyph_m; break;
    case 'N': g = glyph_n; break; case 'O': g = glyph_o; break;
    case 'U': g = glyph_u; break; case 'V': g = glyph_v; break;
    default: return;
    }
    for (int row = 0; row < 5; row++) {
        for (int col = 0; col < 3; col++) {
            if ((g[row] & (1 << (2 - col))) && x + col >= 0 && x + col < width &&
                y + row >= 0 && y + row < height)
                buf[(y + row) * stride + x + col] = color;
        }
    }
}

static void touch_label(uint16_t *buf, int stride, int width, int height,
                        int cx, int cy, const char *text, uint16_t color)
{
    int n = 0;
    while (text[n]) n++;
    int x = cx - (n * 4 - 1) / 2;
    for (int i = 0; i < n; i++) touch_glyph(buf, stride, width, height,
                                               x + i * 4, cy - 2, text[i], color);
}

/* Draw controls into the physical side margins, not into the game image.
 * The game image is rotated by PPA. These panels are copied directly into the
 * LCD framebuffer, so their coordinates and glyphs are already final physical
 * landscape coordinates: apply no second rotation to the overlay. */
#define TOUCH_PANEL_MAX_W 192
#define TOUCH_PANEL_MAX_H 600
#define TOUCH_PANEL_BYTES (TOUCH_PANEL_MAX_W * TOUCH_PANEL_MAX_H * sizeof(uint16_t))
static uint16_t *s_touch_panel_buf = NULL;

static void draw_touch_side_panel(uint16_t *buf, int width, int height, bool right)
{
    memset(buf, 0, (size_t)width * height * sizeof(uint16_t));

    int box_w = width > 112 ? 100 : width - 12;
    if (box_w < 48) box_w = width;
    int box_x = (width - box_w) / 2;
    int box_h = 36;

    if (right) {
        /* The game image is rotated 180 degrees for the panel. Keep the
         * virtual controller in that same physical orientation: START is
         * at the top, VOLUME at the bottom, and A/B follow the rotation. */
        int top_y = 10;
        int bottom_y = height - box_h - 10;
        touch_fill_rect(buf, width, width, height, box_x, top_y, box_w, box_h, 0x39E7);
        touch_rect_border(buf, width, width, height, box_x, top_y, box_w, box_h, 0xBDF7);
        touch_label(buf, width, width, height, width / 2, top_y + box_h / 2, "START", 0xFFFF);

        int radius = width > 160 ? 34 : 24;
        int ax = width - width * 34 / 100, ay = height - 270;
        int bx = width - width * 68 / 100, by = height - 380;
        touch_circle(buf, width, width, height, ax, ay, radius, 0xF800);
        touch_circle(buf, width, width, height, bx, by, radius, 0x001F);
        touch_label(buf, width, width, height, ax, ay, "A", 0xFFFF);
        touch_label(buf, width, width, height, bx, by, "B", 0xFFFF);

        touch_fill_rect(buf, width, width, height, box_x, bottom_y, box_w, box_h, 0x39E7);
        touch_rect_border(buf, width, width, height, box_x, bottom_y, box_w, box_h, 0xBDF7);
        touch_label(buf, width, width, height, width / 2, bottom_y + box_h / 2, "VOL", 0xFFFF);
    } else {
        int top_y = 10;
        int bottom_y = height - box_h - 10;
        touch_fill_rect(buf, width, width, height, box_x, top_y, box_w, box_h, 0x39E7);
        touch_rect_border(buf, width, width, height, box_x, top_y, box_w, box_h, 0xBDF7);
        touch_label(buf, width, width, height, width / 2, top_y + box_h / 2, "SEL", 0xFFFF);

        int dpad_cx = width / 2, dpad_cy = height - 315;
        int arm = width > 160 ? 54 : 38;
        int arm_w = width > 160 ? 36 : 28;
        touch_fill_rect(buf, width, width, height, dpad_cx - arm_w / 2, dpad_cy - arm,
                        arm_w, arm * 2, 0x39E7);
        touch_fill_rect(buf, width, width, height, dpad_cx - arm, dpad_cy - arm_w / 2,
                        arm * 2, arm_w, 0x39E7);
        touch_rect_border(buf, width, width, height, dpad_cx - arm, dpad_cy - arm,
                          arm * 2, arm * 2, 0xBDF7);

        touch_fill_rect(buf, width, width, height, box_x, bottom_y, box_w, box_h, 0x39E7);
        touch_rect_border(buf, width, width, height, box_x, bottom_y, box_w, box_h, 0xBDF7);
        touch_label(buf, width, width, height, width / 2, bottom_y + box_h / 2, "MENU", 0xFFFF);
    }
}

static void draw_touch_controls_physical(uint16_t game_x, uint16_t game_y,
                                         uint16_t game_w, uint16_t game_h)
{
    uint16_t lcd_w = st7701_lcd_width();
    uint16_t lcd_h = st7701_lcd_height();
    int left_w = game_x;
    int right_x = (int)game_x + game_w;
    int right_w = (int)lcd_w - right_x;
    if (left_w <= 0 && right_w <= 0) return;

    /* Do not leave an old overlay behind when an emulator returns to the
     * launcher or changes output size. The game bitmap does not cover these
     * margins, so they must be explicitly cleared. */
    if (!s_touch_controls_enabled) {
        if (s_touch_side_controls_visible && s_touch_panel_buf) {
            memset(s_touch_panel_buf, 0,
                   (size_t)TOUCH_PANEL_MAX_W * lcd_h * sizeof(uint16_t));
            if (left_w > 0) {
                int clear_w = left_w > TOUCH_PANEL_MAX_W ? TOUCH_PANEL_MAX_W : left_w;
                st7701_lcd_draw_to_fb(0, 0, clear_w, lcd_h, s_touch_panel_buf);
            }
            if (right_w > 0) {
                int clear_w = right_w > TOUCH_PANEL_MAX_W ? TOUCH_PANEL_MAX_W : right_w;
                st7701_lcd_draw_to_fb(right_x, 0, clear_w, lcd_h, s_touch_panel_buf);
            }
            s_touch_side_controls_visible = false;
        }
        return;
    }

    if (s_touch_side_controls_drawn) return;

    if (!s_touch_panel_buf) {
        s_touch_panel_buf = heap_caps_aligned_calloc(
            64, 1, TOUCH_PANEL_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
        if (!s_touch_panel_buf) {
            ESP_LOGE(TAG, "Touch side-panel buffer allocation failed");
            return;
        }
    }

    if (left_w > 0) {
        if (left_w > TOUCH_PANEL_MAX_W) left_w = TOUCH_PANEL_MAX_W;
        draw_touch_side_panel(s_touch_panel_buf, left_w, lcd_h, false);
        /* draw_rgb_bitmap may queue an async DMA2D transfer. The buffer is
         * reused for the right panel immediately, so copy through the DPI
         * framebuffer to make both panels deterministic. */
        st7701_lcd_draw_to_fb(0, 0, left_w, lcd_h, s_touch_panel_buf);
    }
    if (right_w > 0) {
        if (right_w > TOUCH_PANEL_MAX_W) right_w = TOUCH_PANEL_MAX_W;
        draw_touch_side_panel(s_touch_panel_buf, right_w, lcd_h, true);
        st7701_lcd_draw_to_fb(right_x, 0, right_w, lcd_h, s_touch_panel_buf);
    }

    s_touch_side_controls_drawn = true;
    s_touch_side_controls_visible = true;
    ESP_LOGI(TAG, "Touch controls drawn in LCD margins: game=%ux%u+%u,%u left=%d right=%d",
             game_w, game_h, game_x, game_y, left_w, right_w);
}
#else
static void draw_touch_controls_physical(uint16_t game_x, uint16_t game_y,
                                         uint16_t game_w, uint16_t game_h)
{
    (void)game_x; (void)game_y; (void)game_w; (void)game_h;
}
#endif

/* ─── Timing instrumentation ──────────────────────────────────── */
static int64_t s_timing_ppa_acc = 0;
static int64_t s_timing_lcd_acc = 0;
static int64_t s_timing_pal_acc = 0;
static int     s_timing_count   = 0;
#define TIMING_INTERVAL 60

/* ─── Display flush ───────────────────────────────────────────── */
void display_flush(void)
{
    if (!s_fb_dirty || !s_framebuffer) return;
    s_fb_dirty = false;

#ifdef CONFIG_HDMI_OUTPUT
    /* HDMI: PPA convert 640×480 RGB565 → RGB888 directly into HDMI DPI FB */
    if (!s_hdmi_initialized) return;
    int64_t t0 = esp_timer_get_time();
    esp_err_t ret = ppa_scale_rgb565_to_rgb888(
        s_framebuffer, FB_W, FB_H,
        1.0f, 1.0f,
        s_hdmi_disp.fb, s_hdmi_disp.fb_size,
        NULL, NULL, false);  /* DSI outputs RGB byte order */
    int64_t t1 = esp_timer_get_time();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "PPA RGB565→RGB888 flush failed (0x%x)", ret);
        return;
    }
    esp_cache_msync(s_hdmi_disp.fb, s_hdmi_disp.fb_size, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    int64_t t2 = esp_timer_get_time();
#else
    /* LCD: landscape scale → push to ST7701 */
    /* Lazy-allocate the persistent PPA output buffer */
    if (!s_ppa_out_buf) {
        s_ppa_out_buf = heap_caps_aligned_calloc(
            PPA_BUF_ALIGN, 1, PPA_OUT_ALIGNED,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
        if (!s_ppa_out_buf) {
            ESP_LOGE(TAG, "Failed to allocate PPA output buffer (%d bytes)", PPA_OUT_ALIGNED);
            return;
        }
        s_ppa_out_size = PPA_OUT_ALIGNED;
        ESP_LOGI(TAG, "PPA output buffer allocated: %d bytes", PPA_OUT_ALIGNED);
    }

    /* Single PPA SRM: landscape scale in one operation */
    uint32_t out_w = 0, out_h = 0;
    int64_t t0 = esp_timer_get_time();
    esp_err_t ret = ppa_rotate_scale_rgb565_to(
        s_framebuffer, FB_W, FB_H,
        180, s_scale_x, s_scale_y,
        s_ppa_out_buf, s_ppa_out_size,
        &out_w, &out_h, false);
    int64_t t1 = esp_timer_get_time();

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "PPA rotate+scale failed (0x%x)", ret);
        return;
    }

    /* Center the 800×480 virtual framebuffer on the 1024×600 panel. */
    uint16_t lcd_w = st7701_lcd_width();
    uint16_t lcd_h = st7701_lcd_height();
    uint16_t x_off = (lcd_w > out_w) ? (lcd_w - out_w) / 2 : 0;
    uint16_t y_off = (lcd_h > out_h) ? (lcd_h - out_h) / 2 : 0;

    odroid_display_set_touch_game_layout(x_off, y_off, out_w, out_h);
    st7701_lcd_draw_rgb_bitmap(x_off, y_off, out_w, out_h, (const uint16_t *)s_ppa_out_buf);
    draw_touch_controls_physical(x_off, y_off, out_w, out_h);
    int64_t t2 = esp_timer_get_time();
#endif /* CONFIG_HDMI_OUTPUT */

    s_timing_ppa_acc += (t1 - t0);
    s_timing_lcd_acc += (t2 - t1);
    s_timing_count++;
    if (s_timing_count >= TIMING_INTERVAL) {
        printf("DISP TIMING (%d frames): PPA=%.1fms  LCD=%.1fms  PAL=%.1fms\n",
               s_timing_count,
               s_timing_ppa_acc / (s_timing_count * 1000.0f),
               s_timing_lcd_acc / (s_timing_count * 1000.0f),
               s_timing_pal_acc / (s_timing_count * 1000.0f));
        s_timing_ppa_acc = 0;
        s_timing_lcd_acc = 0;
        s_timing_pal_acc = 0;
        s_timing_count = 0;
    }
}

void display_flush_force(void)
{
    s_fb_dirty = true;
    display_flush();
}

void display_set_scale(float sx, float sy)
{
    s_scale_x = sx;
    s_scale_y = sy;
    ESP_LOGI(TAG, "PPA scale set to %.2fx%.2f", sx, sy);
}

/* ─── Pipeline A helper: 320×240 → PPA 2× + 180° → landscape LCD ─
 *
 * Called from Pipeline A functions that already hold the display lock.
 * Does the same thing as ili9341_write_frame_rgb565_ex() but without
 * lock/unlock, since the caller already owns the mutex.
 */
/* ─── Emulator flush helper ────────────────────────────────────── */
static void display_emu_flush_320x240(const uint16_t *buf, bool byte_swap)
{
#ifdef CONFIG_HDMI_OUTPUT
    /* HDMI: PPA scale 320×240 RGB565 → 640×480 RGB888 into HDMI FB */
    if (!s_hdmi_initialized) return;
    int64_t t0 = esp_timer_get_time();
    esp_err_t ret = ppa_scale_rgb565_to_rgb888(
        buf, EMU_W, EMU_H,
        (float)HDMI_OUT_W / EMU_W, (float)HDMI_OUT_H / EMU_H,
        s_hdmi_disp.fb, s_hdmi_disp.fb_size,
        NULL, NULL, false);  /* DSI outputs RGB byte order */
    int64_t t1 = esp_timer_get_time();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "PPA emu HDMI flush failed (0x%x)", ret);
        return;
    }
    esp_cache_msync(s_hdmi_disp.fb, s_hdmi_disp.fb_size, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    int64_t t2 = esp_timer_get_time();
    (void)byte_swap;
#else
    /* LCD: PPA 2× scale → 640×480 landscape */
    /* Lazy-allocate the persistent PPA output buffer */
    if (!s_ppa_out_buf) {
        s_ppa_out_buf = heap_caps_aligned_calloc(
            PPA_BUF_ALIGN, 1, PPA_OUT_ALIGNED,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
        if (!s_ppa_out_buf) {
            ESP_LOGE(TAG, "Failed to allocate PPA output buffer (%d bytes)", PPA_OUT_ALIGNED);
            return;
        }
        s_ppa_out_size = PPA_OUT_ALIGNED;
    }

    /* Clear the unused panel area once before drawing the centered frame. */
    if (!s_emu_borders_cleared_a) {
        st7701_lcd_fill_screen(0x0000);
        s_emu_borders_cleared_a = true;
    }

    /* PPA: 320×240 → scale 2× → 640×480 */
    uint32_t out_w = 0, out_h = 0;
    int64_t t0 = esp_timer_get_time();
    esp_err_t ret = ppa_rotate_scale_rgb565_to(
        buf, EMU_W, EMU_H,
        180, 2.0f, 2.0f,
        s_ppa_out_buf, s_ppa_out_size,
        &out_w, &out_h, byte_swap);
    int64_t t1 = esp_timer_get_time();

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "PPA emu 2x+rot failed (0x%x)", ret);
        return;
    }

    /* Center the 640×480 frame on the 1024×600 LCD. */
    uint16_t lcd_w = st7701_lcd_width();
    uint16_t lcd_h = st7701_lcd_height();
    uint16_t x_off = (lcd_w > out_w) ? (lcd_w - out_w) / 2 : 0;
    uint16_t y_off = (lcd_h > out_h) ? (lcd_h - out_h) / 2 : 0;
    odroid_display_set_touch_game_layout(x_off, y_off, out_w, out_h);
    st7701_lcd_draw_rgb_bitmap(x_off, y_off, out_w, out_h, (const uint16_t *)s_ppa_out_buf);
    draw_touch_controls_physical(x_off, y_off, out_w, out_h);
    int64_t t2 = esp_timer_get_time();
#endif /* CONFIG_HDMI_OUTPUT */

    s_timing_ppa_acc += (t1 - t0);
    s_timing_lcd_acc += (t2 - t1);
    s_timing_count++;
    if (s_timing_count >= TIMING_INTERVAL) {
        printf("DISP TIMING (%d frames): PPA=%.1fms  LCD=%.1fms\n",
               s_timing_count,
               s_timing_ppa_acc / (s_timing_count * 1000.0f),
               s_timing_lcd_acc / (s_timing_count * 1000.0f));
        s_timing_ppa_acc = 0;
        s_timing_lcd_acc = 0;
        s_timing_pal_acc = 0;
        s_timing_count = 0;
    }
}

/* ─── ILI9341-compatible API ──────────────────────────────────── */

void ili9341_init(void)
{
    if (s_framebuffer) return;  /* already initialized */

    /* 768KB framebuffer requires PSRAM (won't fit internal SRAM) */
    s_framebuffer = (uint16_t *)heap_caps_aligned_calloc(
        64, 1, FB_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    if (!s_framebuffer) {
        ESP_LOGE(TAG, "Failed to allocate %d byte framebuffer!", FB_SIZE);
        return;
    }
    ESP_LOGI(TAG, "Virtual framebuffer allocated in PSRAM: %dx%d (%d bytes)",
             FB_W, FB_H, FB_SIZE);

#ifdef CONFIG_HDMI_OUTPUT
    /* HDMI: Initialize the HDMI display (LT8912 via DSI) */
    if (!s_hdmi_initialized) {
        esp_err_t ret = hdmi_display_init(HDMI_MODE_640x480, &s_hdmi_disp,
                                          odroid_system_get_i2c_bus());
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "HDMI display init failed (0x%x)", ret);
            return;
        }
        s_hdmi_initialized = true;
        ESP_LOGI(TAG, "HDMI display initialized: %dx%d, fb=%p (%lu bytes)",
                 s_hdmi_disp.h_res, s_hdmi_disp.v_res,
                 s_hdmi_disp.fb, (unsigned long)s_hdmi_disp.fb_size);
    }
#else
    /* LCD: Initialize backlight */
    if (!s_backlight_init) {
        backlight_init();
    }
#endif
}

void ili9341_write_frame_rectangleLE(int x, int y, int w, int h, const uint16_t *data)
{
    if (!s_framebuffer || !data) return;

    /* Clip to framebuffer bounds and copy */
    for (int row = 0; row < h; row++) {
        int fb_y = y + row;
        if (fb_y < 0 || fb_y >= FB_H) continue;
        for (int col = 0; col < w; col++) {
            int fb_x = x + col;
            if (fb_x < 0 || fb_x >= FB_W) continue;
            s_framebuffer[fb_y * FB_W + fb_x] = data[row * w + col];
        }
    }
    s_fb_dirty = true;
}

void ili9341_clear(uint16_t color)
{
    if (!s_framebuffer) return;
    for (int i = 0; i < FB_PIXELS; i++) {
        s_framebuffer[i] = color;
    }
    s_fb_dirty = true;
}

bool is_backlight_initialized(void)
{
#ifdef CONFIG_HDMI_OUTPUT
    return s_hdmi_initialized;
#else
    return s_backlight_init;
#endif
}

uint16_t *display_get_framebuffer(void)
{
    return s_framebuffer;
}

uint16_t *display_get_emu_buffer(void)
{
    if (!s_emu_scaled) {
        s_emu_scaled = heap_caps_aligned_calloc(
            64, 1, EMU_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
        if (!s_emu_scaled) {
            s_emu_scaled = heap_caps_aligned_calloc(
                64, 1, EMU_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
        }
    }
    return s_emu_scaled;
}

void display_emu_flush(void)
{
    if (s_emu_scaled) {
        display_emu_flush_320x240(s_emu_scaled, false);
    }
}

void display_lcd_draw_raw(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                          const uint16_t *data)
{
#ifdef CONFIG_HDMI_OUTPUT
    /* HDMI: no direct portrait draw — no-op (HDMI has no portrait mode) */
    (void)x; (void)y; (void)w; (void)h; (void)data;
#else
    odroid_display_lock();
    st7701_lcd_draw_rgb_bitmap(x, y, w, h, data);
    odroid_display_unlock();
#endif
}

/* ─── Display mutex for exclusive access ──────────────────────── */
static SemaphoreHandle_t s_display_mutex = NULL;

static void ensure_mutex(void)
{
    if (!s_display_mutex) {
        s_display_mutex = xSemaphoreCreateMutex();
        if (!s_display_mutex) abort();
    }
}

void odroid_display_lock(void)           { ensure_mutex(); xSemaphoreTake(s_display_mutex, portMAX_DELAY); }
void odroid_display_unlock(void)         { if (s_display_mutex) xSemaphoreGive(s_display_mutex); }
void odroid_display_lock_gb_display(void)   { odroid_display_lock(); }
void odroid_display_unlock_gb_display(void) { odroid_display_unlock(); }
void odroid_display_lock_nes_display(void)  { odroid_display_lock(); }
void odroid_display_unlock_nes_display(void){ odroid_display_unlock(); }
void odroid_display_lock_sms_display(void)  { odroid_display_lock(); }
void odroid_display_unlock_sms_display(void){ odroid_display_unlock(); }

/* ─── Game Boy display: 160×144 direct RGB565 ─────────────────── */
#define GAMEBOY_WIDTH  160
#define GAMEBOY_HEIGHT 144
#define GB_PIXELS      (GAMEBOY_WIDTH * GAMEBOY_HEIGHT)

/* Static DMA-capable temp buffer for GB input (allocated on first use) */
static uint16_t *s_gb_temp = NULL;

void ili9341_write_frame_gb(uint16_t *buffer, int scale)
{
    (void)scale;
    odroid_display_lock_gb_display();

    if (buffer == NULL) {
        ili9341_clear(0x0000);
    } else {
        /* Lazy-allocate DMA-capable temp buffer for PPA input */
        if (!s_gb_temp) {
            s_gb_temp = heap_caps_aligned_calloc(
                64, 1, GB_PIXELS * sizeof(uint16_t),
                MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
            if (!s_gb_temp) {
                ESP_LOGE(TAG, "GB temp buffer alloc failed");
                odroid_display_unlock_gb_display();
                return;
            }
        }

        /* Copy input into DMA-aligned temp buffer */
        memcpy(s_gb_temp, buffer, GB_PIXELS * sizeof(uint16_t));

        /* Lazy-allocate shared 320×240 intermediate buffer (prefer internal SRAM) */
        if (!s_emu_scaled) {
            s_emu_scaled = heap_caps_aligned_calloc(
                64, 1, EMU_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
            if (!s_emu_scaled) {
                s_emu_scaled = heap_caps_aligned_calloc(
                    64, 1, EMU_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
            }
            if (!s_emu_scaled) { ESP_LOGE(TAG, "emu_scaled alloc failed"); odroid_display_unlock_gb_display(); return; }
        }

#ifdef CONFIG_HDMI_OUTPUT
        /* Software NN upscale 160×144 → 320×240 (PPA fractional scale leaves
         * bottom rows unfilled; software fill is exact and avoids artifacts) */
        for (int y = 0; y < EMU_H; y++) {
            int src_y = y * GAMEBOY_HEIGHT / EMU_H;
            const uint16_t *src_row = &s_gb_temp[src_y * GAMEBOY_WIDTH];
            uint16_t *dst_row = &s_emu_scaled[y * EMU_W];
            for (int x = 0; x < EMU_W; x++) {
                dst_row[x] = src_row[x * GAMEBOY_WIDTH / EMU_W];
            }
        }
        esp_cache_msync(s_emu_scaled, EMU_SIZE, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
#else
        /* PPA scale 160×144 → 320×240 */
        float sx = (float)EMU_W / GAMEBOY_WIDTH;   /* 2.0 */
        float sy = (float)EMU_H / GAMEBOY_HEIGHT;  /* 1.6667 */
        uint32_t out_w = 0, out_h = 0;
        esp_err_t ret = ppa_rotate_scale_rgb565_to(
            s_gb_temp, GAMEBOY_WIDTH, GAMEBOY_HEIGHT,
            0, sx, sy,
            s_emu_scaled, EMU_SIZE,
            &out_w, &out_h, false);

        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "PPA GB scale failed: %s", esp_err_to_name(ret));
            odroid_display_unlock_gb_display();
            return;
        }
#endif

        display_emu_flush_320x240(s_emu_scaled, false);
    }

    odroid_display_unlock_gb_display();
}

/* ─── NES display: 256×224, 8-bit indexed, 256-entry palette ──── */
#define NES_GAME_WIDTH  256
#define NES_GAME_HEIGHT 224
#define NES_PIXELS      (NES_GAME_WIDTH * NES_GAME_HEIGHT)

/* Static DMA-capable temp buffer for NES 256×224 RGB565 */
static uint16_t *s_nes_temp = NULL;

void ili9341_write_frame_nes(uint8_t *buffer, uint16_t *myPalette, uint8_t scale)
{
    (void)scale;
    odroid_display_lock_nes_display();

    if (buffer == NULL) {
        ili9341_clear(0x0000);
    } else {
        /* Lazy-allocate DMA-capable temp buffer */
        if (!s_nes_temp) {
            s_nes_temp = heap_caps_aligned_calloc(
                64, 1, NES_PIXELS * sizeof(uint16_t),
                MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
            if (!s_nes_temp) {
                ESP_LOGE(TAG, "NES temp buffer alloc failed");
                odroid_display_unlock_nes_display();
                return;
            }
        }

        /* Palette conversion: 8-bit indexed → RGB565 (byte-swapped to LE) */
        for (int i = 0; i < NES_PIXELS; i++) {
            uint16_t pixel = myPalette[buffer[i]];
            s_nes_temp[i] = (pixel >> 8) | (pixel << 8);
        }

        /* Lazy-allocate shared 320×240 intermediate buffer (prefer internal SRAM) */
        if (!s_emu_scaled) {
            s_emu_scaled = heap_caps_aligned_calloc(
                64, 1, EMU_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
            if (!s_emu_scaled) {
                s_emu_scaled = heap_caps_aligned_calloc(
                    64, 1, EMU_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
            }
            if (!s_emu_scaled) { ESP_LOGE(TAG, "emu_scaled alloc failed"); odroid_display_unlock_nes_display(); return; }
        }

        /* PPA scale 256×224 → 320×240 */
        float sx = (float)EMU_W / NES_GAME_WIDTH;   /* 1.25 */
        float sy = (float)EMU_H / NES_GAME_HEIGHT;  /* 1.0714 */
        uint32_t out_w = 0, out_h = 0;
        esp_err_t ret = ppa_rotate_scale_rgb565_to(
            s_nes_temp, NES_GAME_WIDTH, NES_GAME_HEIGHT,
            0, sx, sy,
            s_emu_scaled, EMU_SIZE,
            &out_w, &out_h, false);

        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "PPA NES scale failed: %s", esp_err_to_name(ret));
            odroid_display_unlock_nes_display();
            return;
        }

        display_emu_flush_320x240(s_emu_scaled, false);
    }

    odroid_display_unlock_nes_display();
}

/* ─── SMS/Game Gear display: 8-bit indexed → PPA-scaled ───────── */
#define SMS_WIDTH       256
#define SMS_HEIGHT      192
#define GAMEGEAR_WIDTH  160
#define GAMEGEAR_HEIGHT 144
#define PIXEL_MASK      0x1F
#define SMS_MAX_PIXELS  (SMS_WIDTH * SMS_HEIGHT)  /* larger of SMS / GG */

/* Static DMA-capable temp buffer (sized for the larger SMS resolution) */
static uint16_t *s_sms_temp = NULL;

void ili9341_write_frame_sms(uint8_t *buffer, uint16_t color[], uint8_t isGameGear, uint8_t scale)
{
    (void)scale;
    odroid_display_lock_sms_display();

    if (buffer == NULL) {
        ili9341_clear(0x0000);
    } else {
        /* Lazy-allocate DMA-capable temp buffer */
        if (!s_sms_temp) {
            s_sms_temp = heap_caps_aligned_calloc(
                64, 1, SMS_MAX_PIXELS * sizeof(uint16_t),
                MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
            if (!s_sms_temp) {
                ESP_LOGE(TAG, "SMS temp buffer alloc failed");
                odroid_display_unlock_sms_display();
                return;
            }
        }

        const int src_w      = isGameGear ? GAMEGEAR_WIDTH  : SMS_WIDTH;
        const int src_h      = isGameGear ? GAMEGEAR_HEIGHT : SMS_HEIGHT;
        const int src_stride = isGameGear ? 256 : SMS_WIDTH;
        const int src_x_off  = isGameGear ? 48  : 0;

        /* Palette conversion: 8-bit indexed → RGB565 into temp buffer */
        for (int y = 0; y < src_h; y++) {
            const uint8_t *src_row = &buffer[y * src_stride + src_x_off];
            uint16_t *dst_row = &s_sms_temp[y * src_w];
            for (int x = 0; x < src_w; x++) {
                dst_row[x] = color[src_row[x] & PIXEL_MASK];
            }
        }

        /* Lazy-allocate shared 320×240 intermediate buffer (prefer internal SRAM) */
        if (!s_emu_scaled) {
            s_emu_scaled = heap_caps_aligned_calloc(
                64, 1, EMU_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
            if (!s_emu_scaled) {
                s_emu_scaled = heap_caps_aligned_calloc(
                    64, 1, EMU_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
            }
            if (!s_emu_scaled) { ESP_LOGE(TAG, "emu_scaled alloc failed"); odroid_display_unlock_sms_display(); return; }
        }

#ifdef CONFIG_HDMI_OUTPUT
        if (isGameGear) {
            /* Software NN upscale 160×144 → 320×240 (PPA fractional scale
             * leaves bottom rows unfilled; software fill is exact) */
            for (int y = 0; y < EMU_H; y++) {
                int src_y = y * GAMEGEAR_HEIGHT / EMU_H;
                const uint16_t *sr = &s_sms_temp[src_y * GAMEGEAR_WIDTH];
                uint16_t *dr = &s_emu_scaled[y * EMU_W];
                for (int x = 0; x < EMU_W; x++) {
                    dr[x] = sr[x * GAMEGEAR_WIDTH / EMU_W];
                }
            }
            esp_cache_msync(s_emu_scaled, EMU_SIZE, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
        } else {
            /* SMS: 256×192 → 320×240 (exact 1.25× both axes, PPA handles it) */
            float sx = (float)EMU_W / src_w;
            float sy = (float)EMU_H / src_h;
            uint32_t out_w = 0, out_h = 0;
            esp_err_t ret = ppa_rotate_scale_rgb565_to(
                s_sms_temp, src_w, src_h,
                0, sx, sy,
                s_emu_scaled, EMU_SIZE,
                &out_w, &out_h, false);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "PPA SMS scale failed: %s", esp_err_to_name(ret));
                odroid_display_unlock_sms_display();
                return;
            }
        }
#else
        /* PPA scale src_w×src_h → 320×240 */
        float sx = (float)EMU_W / src_w;
        float sy = (float)EMU_H / src_h;
        uint32_t out_w = 0, out_h = 0;
        esp_err_t ret = ppa_rotate_scale_rgb565_to(
            s_sms_temp, src_w, src_h,
            0, sx, sy,
            s_emu_scaled, EMU_SIZE,
            &out_w, &out_h, false);

        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "PPA SMS scale failed: %s", esp_err_to_name(ret));
            odroid_display_unlock_sms_display();
            return;
        }
#endif

        display_emu_flush_320x240(s_emu_scaled, false);
    }

    odroid_display_unlock_sms_display();
}

/* ─── C64-specific display function ───────────────────────────── */
void ili9341_write_frame_c64(uint8_t *buffer, uint16_t *palette)
{
    const int C64_DISPLAY_X = 0x180; /* 384 */
    const int C64_DISPLAY_Y = 0x110; /* 272 */

    odroid_display_lock();

    if (buffer == NULL) {
        ili9341_clear(0x0000);
    } else {
        /* Lazy-allocate shared 320×240 intermediate buffer (prefer internal SRAM) */
        if (!s_emu_scaled) {
            s_emu_scaled = heap_caps_aligned_calloc(
                64, 1, EMU_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
            if (!s_emu_scaled) {
                s_emu_scaled = heap_caps_aligned_calloc(
                    64, 1, EMU_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
            }
            if (!s_emu_scaled) { ESP_LOGE(TAG, "emu_scaled alloc failed"); odroid_display_unlock(); return; }
        }

        /* Crop 384×272 → center 320×240 with palette conversion */
        const int offX = (C64_DISPLAY_X - EMU_W) / 2; /* (384-320)/2 = 32 */
        const int offY = (C64_DISPLAY_Y - EMU_H) / 2; /* (272-240)/2 = 16 */

        for (int y = 0; y < EMU_H; ++y) {
            int src_base = (y + offY) * C64_DISPLAY_X + offX;
            int dst_base = y * EMU_W;
            for (int x = 0; x < EMU_W; ++x) {
                s_emu_scaled[dst_base + x] = palette[buffer[src_base + x]];
            }
        }

        display_emu_flush_320x240(s_emu_scaled, false);
    }

    odroid_display_unlock();
}

/* ─── Atari 7800 / PCE display: 320×240 8-bit indexed → Pipeline B ── */
void ili9341_write_frame_prosystem(uint8_t *buffer, uint16_t *palette)
{
    odroid_display_lock();

    if (buffer == NULL) {
        ili9341_clear(0x0000);
    } else {
        /* Lazy-allocate shared 320×240 intermediate buffer (prefer internal SRAM to reduce PSRAM contention) */
        if (!s_emu_scaled) {
            s_emu_scaled = heap_caps_aligned_calloc(
                64, 1, EMU_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
            if (!s_emu_scaled) {
                s_emu_scaled = heap_caps_aligned_calloc(
                    64, 1, EMU_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
            }
            if (!s_emu_scaled) { ESP_LOGE(TAG, "emu_scaled alloc failed"); odroid_display_unlock(); return; }
        }

        int64_t tp0 = esp_timer_get_time();
        /* Palette lookup: 320×240 8-bit indexed → RGB565 into s_emu_scaled */
        const uint32_t *in32  = (const uint32_t *)buffer;
        uint32_t       *out32 = (uint32_t *)s_emu_scaled;
        for (int i = 0; i < EMU_PIXELS / 4; i++) {
            uint32_t pix4 = in32[i];
            uint16_t p0 = palette[(pix4 >>  0) & 0xFF];
            uint16_t p1 = palette[(pix4 >>  8) & 0xFF];
            uint16_t p2 = palette[(pix4 >> 16) & 0xFF];
            uint16_t p3 = palette[(pix4 >> 24) & 0xFF];
            out32[i * 2]     = p0 | ((uint32_t)p1 << 16);
            out32[i * 2 + 1] = p2 | ((uint32_t)p3 << 16);
        }
        s_timing_pal_acc += (esp_timer_get_time() - tp0);

        display_emu_flush_320x240(s_emu_scaled, false);
    }

    odroid_display_unlock();
}

/* ─── Atari Lynx display: 160×102 RGB565 → single PPA to display ──── */
#define LYNX_GAME_WIDTH  160
#define LYNX_GAME_HEIGHT 102
#define LYNX_PIXELS      (LYNX_GAME_WIDTH * LYNX_GAME_HEIGHT)

static uint16_t *s_lynx_temp = NULL;

void ili9341_write_frame_lynx(const uint16_t *buffer)
{
    odroid_display_lock();

    if (buffer == NULL) {
        ili9341_clear(0x0000);
    } else {
        if (!s_lynx_temp) {
            s_lynx_temp = heap_caps_aligned_calloc(
                64, 1, LYNX_PIXELS * sizeof(uint16_t),
                MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
            if (!s_lynx_temp) {
                ESP_LOGE(TAG, "Lynx temp buffer alloc failed");
                odroid_display_unlock();
                return;
            }
        }

        memcpy(s_lynx_temp, buffer, LYNX_PIXELS * sizeof(uint16_t));

#ifdef CONFIG_HDMI_OUTPUT
        /* HDMI: single PPA scale 160×102 → 640×480 RGB888 */
        if (!s_hdmi_initialized) { odroid_display_unlock(); return; }
        float sx = (float)HDMI_OUT_W / LYNX_GAME_WIDTH;
        float sy = (float)HDMI_OUT_H / LYNX_GAME_HEIGHT;
        esp_err_t ret = ppa_scale_rgb565_to_rgb888(
            s_lynx_temp, LYNX_GAME_WIDTH, LYNX_GAME_HEIGHT,
            sx, sy,
            s_hdmi_disp.fb, s_hdmi_disp.fb_size,
            NULL, NULL, false);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "PPA Lynx HDMI scale failed (0x%x)", ret);
            odroid_display_unlock();
            return;
        }
        esp_cache_msync(s_hdmi_disp.fb, s_hdmi_disp.fb_size, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
#else
        /* LCD: single PPA scale 160×102 → 800×480 into framebuffer, then flush */
        float sx = (float)FB_W / LYNX_GAME_WIDTH;
        float sy = (float)FB_H / LYNX_GAME_HEIGHT;
        uint32_t out_w = 0, out_h = 0;
        esp_err_t ret = ppa_rotate_scale_rgb565_to(
            s_lynx_temp, LYNX_GAME_WIDTH, LYNX_GAME_HEIGHT,
            0, sx, sy,
            s_framebuffer, FB_SIZE,
            &out_w, &out_h, false);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "PPA Lynx scale failed: %s", esp_err_to_name(ret));
            odroid_display_unlock();
            return;
        }
        s_fb_dirty = true;
        display_flush();
#endif
    }

    odroid_display_unlock();
}

/* ─── Generic RGB565 display: 320×240 emulator output ────────── */
/*
 * Optimized path: PPA hardware does 2× scale + 180° rotation in one
 * operation directly from the 320×240 input → 640×480 output.
 * This avoids the intermediate 800×480 framebuffer, CPU 2× scaling,
 * border clearing, and the separate PPA rotation of the full 800×480.
 *
 * Input can be LE (pre-swapped by caller) or BE (native emulator output).
 * When byte_swap_input is set, PPA hardware swaps bytes during processing.
 */

#ifndef CONFIG_HDMI_OUTPUT
static bool s_emu_borders_cleared = false;
#endif

void ili9341_write_frame_rgb565_ex(const uint16_t *buffer, bool byte_swap_input)
{
    odroid_display_lock();

    if (buffer == NULL) {
        ili9341_clear(0x0000);
        display_flush();
#ifndef CONFIG_HDMI_OUTPUT
        s_emu_borders_cleared = false;
#endif
        odroid_display_unlock();
        return;
    }

#ifdef CONFIG_HDMI_OUTPUT
    /* HDMI: PPA scale 320×240 RGB565 → 640×480 RGB888 directly into HDMI FB */
    display_emu_flush_320x240(buffer, byte_swap_input);
#else
    /* LCD: PPA 2× scale → push to ST7701 */
    if (!s_ppa_out_buf) {
        s_ppa_out_buf = heap_caps_aligned_calloc(
            PPA_BUF_ALIGN, 1, PPA_OUT_ALIGNED,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
        if (!s_ppa_out_buf) {
            ESP_LOGE(TAG, "Failed to allocate PPA output buffer (%d bytes)", PPA_OUT_ALIGNED);
            odroid_display_unlock();
            return;
        }
        s_ppa_out_size = PPA_OUT_ALIGNED;
    }

    if (!s_emu_borders_cleared) {
        st7701_lcd_fill_screen(0x0000);
        s_emu_borders_cleared = true;
    }

    uint32_t out_w = 0, out_h = 0;
    int64_t t0 = esp_timer_get_time();
    esp_err_t ret = ppa_rotate_scale_rgb565_to(
        buffer, EMU_W, EMU_H,
        180, 2.0f, 2.0f,
        s_ppa_out_buf, s_ppa_out_size,
        &out_w, &out_h, byte_swap_input);
    int64_t t1 = esp_timer_get_time();

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "PPA emu rotate+scale failed (0x%x)", ret);
        odroid_display_unlock();
        return;
    }

    uint16_t lcd_w = st7701_lcd_width();
    uint16_t lcd_h = st7701_lcd_height();
    uint16_t x_off = (lcd_w > out_w) ? (lcd_w - out_w) / 2 : 0;
    uint16_t y_off = (lcd_h > out_h) ? (lcd_h - out_h) / 2 : 0;
    odroid_display_set_touch_game_layout(x_off, y_off, out_w, out_h);
    st7701_lcd_draw_rgb_bitmap(x_off, y_off, out_w, out_h, (const uint16_t *)s_ppa_out_buf);
    draw_touch_controls_physical(x_off, y_off, out_w, out_h);
    int64_t t2 = esp_timer_get_time();

    s_timing_ppa_acc += (t1 - t0);
    s_timing_lcd_acc += (t2 - t1);
    s_timing_count++;
    if (s_timing_count >= TIMING_INTERVAL) {
        printf("DISP TIMING (%d frames): PPA=%.1fms  LCD=%.1fms\n",
               s_timing_count,
               s_timing_ppa_acc / (s_timing_count * 1000.0f),
               s_timing_lcd_acc / (s_timing_count * 1000.0f));
        s_timing_ppa_acc = 0;
        s_timing_lcd_acc = 0;
        s_timing_pal_acc = 0;
        s_timing_count = 0;
    }
#endif /* CONFIG_HDMI_OUTPUT */

    odroid_display_unlock();
}

/* Backward-compatible wrapper: caller has already byte-swapped to LE */
void ili9341_write_frame_rgb565(const uint16_t *buffer)
{
    ili9341_write_frame_rgb565_ex(buffer, false);
}

/* ─── Custom-size RGB565 frame writer (PPA scale + rotate) ───── */
#ifndef CONFIG_HDMI_OUTPUT
static bool s_custom_borders_cleared = false;
#endif

void ili9341_write_frame_rgb565_custom(const uint16_t *buffer, uint16_t in_w,
                                        uint16_t in_h, float scale,
                                        bool byte_swap_input)
{
    odroid_display_lock();

    if (buffer == NULL) {
        ili9341_clear(0x0000);
        display_flush();
#ifndef CONFIG_HDMI_OUTPUT
        s_custom_borders_cleared = false;
#endif
        odroid_display_unlock();
        return;
    }

#ifdef CONFIG_HDMI_OUTPUT
    /* HDMI: PPA scale in_w×in_h RGB565 → 640×480 RGB888 directly into HDMI FB */
    if (!s_hdmi_initialized) { odroid_display_unlock(); return; }
    float sx = (float)HDMI_OUT_W / in_w;
    float sy = (float)HDMI_OUT_H / in_h;
    esp_err_t ret = ppa_scale_rgb565_to_rgb888(
        buffer, in_w, in_h,
        sx, sy,
        s_hdmi_disp.fb, s_hdmi_disp.fb_size,
        NULL, NULL, false);  /* DSI outputs RGB byte order */
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "PPA custom HDMI scale failed (0x%x)", ret);
        odroid_display_unlock();
        return;
    }
    esp_cache_msync(s_hdmi_disp.fb, s_hdmi_disp.fb_size, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    (void)scale; (void)byte_swap_input;
#else
    /* LCD: PPA scale in landscape → push to ST7701 */
    if (!s_ppa_out_buf) {
        s_ppa_out_buf = heap_caps_aligned_calloc(
            PPA_BUF_ALIGN, 1, PPA_OUT_ALIGNED,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
        if (!s_ppa_out_buf) {
            ESP_LOGE(TAG, "Failed to allocate PPA output buffer (%d bytes)", PPA_OUT_ALIGNED);
            odroid_display_unlock();
            return;
        }
        s_ppa_out_size = PPA_OUT_ALIGNED;
    }

    /* Compute output dimensions after landscape scaling. */
    uint16_t out_w_exp = (uint16_t)(in_w * scale);
    uint16_t out_h_exp = (uint16_t)(in_h * scale);
    uint16_t lcd_w = st7701_lcd_width();
    uint16_t lcd_h = st7701_lcd_height();
    uint16_t x_off = (lcd_w > out_w_exp) ? (lcd_w - out_w_exp) / 2 : 0;
    uint16_t y_off = (lcd_h > out_h_exp) ? (lcd_h - out_h_exp) / 2 : 0;

    if (!s_custom_borders_cleared) {
        ili9341_clear(0x0000);
        display_flush_force();
        s_custom_borders_cleared = true;
    }

    uint32_t out_w = 0, out_h = 0;
    esp_err_t ret = ppa_rotate_scale_rgb565_to(
        buffer, in_w, in_h,
        180, scale, scale,
        s_ppa_out_buf, s_ppa_out_size,
        &out_w, &out_h, byte_swap_input);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "PPA custom rotate+scale failed (0x%x)", ret);
        odroid_display_unlock();
        return;
    }

    odroid_display_set_touch_game_layout(x_off, y_off, out_w, out_h);
    st7701_lcd_draw_rgb_bitmap(x_off, y_off, out_w, out_h,
                               (const uint16_t *)s_ppa_out_buf);
    draw_touch_controls_physical(x_off, y_off, out_w, out_h);
#endif /* CONFIG_HDMI_OUTPUT */
    odroid_display_unlock();
}

/* ─── Misc display functions ──────────────────────────────────── */
void ili9341_poweroff(void)
{
#ifndef CONFIG_HDMI_OUTPUT
    /* Turn off backlight to avoid white flash during OTA reboot */
    if (s_backlight_init) {
        ledc_set_duty(LEDC_LOW_SPEED_MODE, BL_LEDC_CH, 0);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, BL_LEDC_CH);
    }
#endif
}

void ili9341_prepare(void)
{
    /* No-op on ESP32-P4 — LCD is already initialized */
}

void odroid_display_show_sderr(int errNum)
{
    (void)errNum;
    ESP_LOGE(TAG, "SD card error: %d", errNum);
    ili9341_clear(0xF800); /* Red screen */
    display_flush();
}

void odroid_display_show_hourglass(void)
{
    ESP_LOGI(TAG, "Hourglass (loading) indicator shown");
}

void odroid_display_show_splash(void)
{
    ESP_LOGI(TAG, "Splash screen (no-op on P4)");
}

void odroid_display_drain_spi(void)
{
    /* No-op — no SPI on P4 */
}
