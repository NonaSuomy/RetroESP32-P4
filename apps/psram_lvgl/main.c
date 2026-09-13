/*
 * ============================================================================
 *  psram_lvgl — LVGL v9 running as a PSRAM app (.papp), with touch input.
 * ============================================================================
 *
 *  This is a TRAINING REFERENCE. It shows the three things you must wire up to
 *  get LVGL running inside a PAPP:
 *
 *    1. A DISPLAY   — LVGL renders into our own RGB565 canvas, which we hand to
 *                     the launcher via svc->display_write_frame_custom().
 *    2. AN INPUT    — an LVGL "pointer" indev backed by svc->touch_read().
 *    3. A TICK      — LVGL needs a millisecond clock; we use svc->get_time_us().
 *
 *  ── Why a 400x240 canvas (and not the native framebuffer)? ─────────────────
 *  The native framebuffer is 800x480 on the LCD build but 640x480 on the HDMI
 *  build, and the service table has no "get framebuffer size" call. So a PAPP
 *  that wants to run unmodified on BOTH targets should render a fixed-size
 *  canvas and let display_write_frame_custom() scale it. 400x240 at scale 2.0
 *  maps exactly onto 800x480 and is letterboxed sensibly on HDMI. It is also
 *  4x less pixel work than native, which keeps LVGL's software renderer smooth.
 *
 *  ── Touch coordinate mapping ───────────────────────────────────────────────
 *  svc->touch_read() reports 180-degree-corrected LANDSCAPE NATIVE coordinates
 *  (x:0..799, y:0..479).
 *  Our canvas is half that in each axis, so canvas = native / 2.
 *
 *  ── Exiting ────────────────────────────────────────────────────────────────
 *  We exit on the PHYSICAL L3 button through the dedicated append-only
 *  input_l3_read service. X remains available as a normal gamepad input.
 *
 *  Build:  .\tools\build_lvgl_papp.ps1
 *  Deploy: firmware\psram_lvgl.papp  ->  /sd/roms/papp/psram_lvgl.papp
 * ============================================================================
 */

#define PAPP_APP_SIDE 1
#include "psram_app.h"

#include "lvgl.h"

/* ── Canvas geometry ─────────────────────────────────────────────────────── */
#define CANVAS_W      400
#define CANVAS_H      240
#define CANVAS_SCALE  2.0f                 /* 400x240 * 2 -> 800x480          */
#define TOUCH_DIV     2                    /* native -> canvas divisor        */

/* Analog-wheel detection, matching the Atari cores' constants */
#define PADDLE_SAMPLES        4
#define PADDLE_DETECT_SPREAD  300          /* max ADC spread for a real pot   */
#define CANVAS_PIXELS (CANVAS_W * CANVAS_H)
#define CANVAS_BYTES  (CANVAS_PIXELS * (int)sizeof(uint16_t))

/* ── Palette (kept close to the launcher's own dark look) ────────────────── */
#define COL_BG      0x101820
#define COL_PANEL   0x1b2836
#define COL_ACCENT  0x38bdf8   /* cyan   */
#define COL_ACCENT2 0xf5c518   /* amber  */
#define COL_OK      0x35c46a   /* green  */
#define COL_TEXT    0xe6edf3
#define COL_MUTED   0x7d8fa1

/* ── Globals (a PAPP is single-instance, so file-scope state is fine) ─────── */
static const app_services_t *g_svc;
static uint16_t *g_canvas;                 /* LVGL renders here               */

static lv_obj_t *g_arc, *g_arc_val, *g_stats, *g_chart, *g_bar, *g_touchdot;
static lv_obj_t *g_arc_cap, *g_raw;
static lv_chart_series_t *g_ser;
static bool g_feed_chart = true;
static bool g_have_wheel = false;   /* physical analog wheel present?         */
static uint32_t g_frames, g_fps;

/* ========================================================================== */
/*  1. DISPLAY  — LVGL renders into g_canvas, we push it to the launcher      */
/* ========================================================================== */
/*
 * With LV_DISPLAY_RENDER_MODE_FULL the draw buffer is screen-sized and LVGL
 * hands us the whole canvas once all dirty areas have been redrawn — so we can
 * simply push the entire buffer and ignore `area`.
 *
 * NOTE ON COLOUR ORDER: LVGL writes native RGB565 and the launcher's
 * display_write_frame_custom() takes a `byte_swap` flag. We pass false, which
 * matches the convention used by the other PAPPs (0xF800 == red). If colours
 * ever come out inverted-looking (red<->blue), flip that flag.
 */
static void disp_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    (void)area;
    g_svc->display_write_frame_custom((const uint16_t *)px_map,
                                      CANVAS_W, CANVAS_H, CANVAS_SCALE, false);
    g_frames++;
    lv_display_flush_ready(disp);          /* MUST be called, or LVGL stalls  */
}

/* ========================================================================== */
/*  2. INPUT  — an LVGL pointer device backed by the GT911 touch panel        */
/* ========================================================================== */
static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    int tx, ty;

    /* touch_read lives in the append-only zone of the ABI, so an older
     * launcher may not provide it — app_entry() already null-checked it. */
    if (g_svc->touch_read(&tx, &ty)) {
        data->point.x = (lv_coord_t)(tx / TOUCH_DIV);
        data->point.y = (lv_coord_t)(ty / TOUCH_DIV);
        data->state   = LV_INDEV_STATE_PRESSED;
    } else {
        data->state   = LV_INDEV_STATE_RELEASED;   /* keep last point */
    }
}

/* ========================================================================== */
/*  3. TICK  — LVGL's millisecond time base                                   */
/* ========================================================================== */
static uint32_t tick_cb(void)
{
    return (uint32_t)(g_svc->get_time_us() / 1000);
}

/* ── Event handlers ──────────────────────────────────────────────────────── */

/* Slider always drives the bar. It only drives the arc when there is no
 * physical wheel — otherwise the two would fight over the same gauge. */
static void slider_cb(lv_event_t *e)
{
    lv_obj_t *sl = lv_event_get_target_obj(e);
    int32_t v = lv_slider_get_value(sl);
    lv_bar_set_value(g_bar, v, LV_ANIM_ON);
    if (!g_have_wheel) {
        lv_arc_set_value(g_arc, v);
        lv_label_set_text_fmt(g_arc_val, "%d", (int)v);
    }
}

/* Dragging the arc directly updates its own label (fallback mode only). */
static void arc_cb(lv_event_t *e)
{
    lv_obj_t *a = lv_event_get_target_obj(e);
    lv_label_set_text_fmt(g_arc_val, "%d", (int)lv_arc_get_value(a));
}

/* Switch pauses/resumes the live chart. */
static void sw_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target_obj(e);
    g_feed_chart = lv_obj_has_state(sw, LV_STATE_CHECKED);
}

/* ── Periodic timer: feeds the chart and refreshes the stats readout ─────── */
static void tick_timer_cb(lv_timer_t *t)
{
    (void)t;
    static uint32_t last_ms;

    /* ── Sample the PHYSICAL analog wheel ────────────────────────────────
     * The paddle potentiometer reports raw 12-bit (0..4095); we scale it to
     * the arc's 0..100 range, drive the gauge with it, and trace it on the
     * chart. If the wheel is unavailable (older launcher, or HDMI build) we
     * fall back to whatever the on-screen arc is set to, so the demo still
     * works. The switch pauses charting. */
    int32_t plot;
    if (g_have_wheel) {
        int raw = g_svc->paddle_read();
        if (raw >= 0) {
            int32_t pct = (int32_t)((raw * 100) / 4095);
            if (pct < 0)   pct = 0;
            if (pct > 100) pct = 100;
            lv_arc_set_value(g_arc, pct);
            lv_label_set_text_fmt(g_arc_val, "%d", (int)pct);
            lv_label_set_text_fmt(g_raw, "raw %d", raw);
        }
    }
    plot = lv_arc_get_value(g_arc);

    if (g_feed_chart) {
        lv_chart_set_next_value(g_chart, g_ser, plot);
    }

    /* FPS over the last second */
    uint32_t now = tick_cb();
    if (now - last_ms >= 1000) {
        g_fps = g_frames;
        g_frames = 0;
        last_ms = now;
    }

    int tx = -1, ty = -1;
    bool touched = g_svc->touch_read(&tx, &ty) != 0;

    lv_mem_monitor_t mon;
    lv_mem_monitor(&mon);
    lv_label_set_text_fmt(g_stats, "%lu FPS   %luKB free",
                          (unsigned long)g_fps,
                          (unsigned long)(mon.free_size / 1024));

    /* A dot that follows your finger — the most direct proof touch works. */
    if (touched) {
        lv_obj_clear_flag(g_touchdot, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(g_touchdot, tx / TOUCH_DIV - 7, ty / TOUCH_DIV - 7);
    } else {
        lv_obj_add_flag(g_touchdot, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ── Small helper: a rounded "card" panel ────────────────────────────────── */
static lv_obj_t *make_card(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_set_pos(c, x, y);
    lv_obj_set_size(c, w, h);
    lv_obj_set_style_bg_color(c, lv_color_hex(COL_PANEL), LV_PART_MAIN);
    lv_obj_set_style_border_width(c, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(c, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_all(c, 6, LV_PART_MAIN);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    return c;
}

/* ========================================================================== */
/*  UI                                                                        */
/* ========================================================================== */
static void build_ui(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(COL_BG), LV_PART_MAIN);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* ── Header ──────────────────────────────────────────────────────────── */
    /* NOTE: keep ALL label text pure ASCII. LVGL's built-in Montserrat fonts
     * only carry the ASCII range, so anything above 0x7F (a middle dot, an
     * en-dash, an accented letter) renders as an empty "tofu" box. */
    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "LVGL 9.5");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_hex(COL_TEXT), LV_PART_MAIN);
    lv_obj_set_pos(title, 10, 5);

    /* Right-aligned readout. The title above is deliberately short so these
     * two never collide on a 400 px-wide canvas. */
    g_stats = lv_label_create(scr);
    lv_label_set_text(g_stats, "-- FPS");
    lv_obj_set_style_text_color(g_stats, lv_color_hex(COL_ACCENT), LV_PART_MAIN);
    lv_obj_align(g_stats, LV_ALIGN_TOP_RIGHT, -10, 10);

    /* ── Left card: interactive arc gauge + slider ───────────────────────── */
    lv_obj_t *left = make_card(scr, 8, 36, 182, 168);

    g_arc = lv_arc_create(left);
    lv_obj_set_size(g_arc, 108, 108);
    lv_obj_align(g_arc, LV_ALIGN_TOP_MID, 0, 0);
    lv_arc_set_rotation(g_arc, 135);
    lv_arc_set_bg_angles(g_arc, 0, 270);
    lv_arc_set_range(g_arc, 0, 100);
    lv_arc_set_value(g_arc, 50);
    lv_obj_set_style_arc_color(g_arc, lv_color_hex(COL_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(g_arc, 9, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(g_arc, 9, LV_PART_MAIN);
    lv_obj_set_style_bg_color(g_arc, lv_color_hex(COL_ACCENT), LV_PART_KNOB);
    lv_obj_add_event_cb(g_arc, arc_cb, LV_EVENT_VALUE_CHANGED, NULL);

    g_arc_val = lv_label_create(g_arc);
    lv_label_set_text(g_arc_val, "50");
    lv_obj_set_style_text_font(g_arc_val, &lv_font_montserrat_28, LV_PART_MAIN);
    lv_obj_set_style_text_color(g_arc_val, lv_color_hex(COL_TEXT), LV_PART_MAIN);
    lv_obj_center(g_arc_val);

    /* When the physical wheel drives the gauge, take it out of the touch
     * path entirely so a stray finger cannot fight the potentiometer. */
    if (g_have_wheel) {
        lv_obj_remove_flag(g_arc, LV_OBJ_FLAG_CLICKABLE);
    }

    g_arc_cap = lv_label_create(left);
    lv_label_set_text(g_arc_cap, g_have_wheel ? "PHYSICAL WHEEL" : "DIAL (touch)");
    lv_obj_set_style_text_color(g_arc_cap, lv_color_hex(COL_MUTED), LV_PART_MAIN);
    lv_obj_align(g_arc_cap, LV_ALIGN_TOP_MID, 0, 110);

    g_raw = lv_label_create(left);
    lv_label_set_text(g_raw, g_have_wheel ? "raw --" : "no wheel");
    lv_obj_set_style_text_color(g_raw, lv_color_hex(COL_ACCENT2), LV_PART_MAIN);
    lv_obj_align(g_raw, LV_ALIGN_TOP_MID, 0, 126);

    lv_obj_t *sl = lv_slider_create(left);
    lv_obj_set_size(sl, 150, 10);
    lv_obj_align(sl, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_slider_set_range(sl, 0, 100);
    lv_slider_set_value(sl, 50, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(sl, lv_color_hex(COL_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sl, lv_color_hex(COL_ACCENT), LV_PART_KNOB);
    lv_obj_add_event_cb(sl, slider_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* ── Right card: live chart + a bar + a switch ───────────────────────── */
    lv_obj_t *right = make_card(scr, 198, 36, 194, 168);

    g_chart = lv_chart_create(right);
    lv_obj_set_size(g_chart, 178, 92);
    lv_obj_align(g_chart, LV_ALIGN_TOP_MID, 0, 0);
    lv_chart_set_type(g_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(g_chart, 32);
    lv_chart_set_range(g_chart, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
    lv_chart_set_div_line_count(g_chart, 3, 5);
    lv_obj_set_style_bg_color(g_chart, lv_color_hex(COL_BG), LV_PART_MAIN);
    lv_obj_set_style_border_width(g_chart, 0, LV_PART_MAIN);
    lv_obj_set_style_size(g_chart, 0, 0, LV_PART_INDICATOR);   /* hide points */
    g_ser = lv_chart_add_series(g_chart, lv_color_hex(COL_ACCENT2),
                                LV_CHART_AXIS_PRIMARY_Y);

    g_bar = lv_bar_create(right);
    lv_obj_set_size(g_bar, 120, 8);
    lv_obj_align(g_bar, LV_ALIGN_BOTTOM_LEFT, 2, -6);
    lv_bar_set_range(g_bar, 0, 100);
    lv_bar_set_value(g_bar, 50, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(g_bar, lv_color_hex(COL_OK), LV_PART_INDICATOR);

    lv_obj_t *sw = lv_switch_create(right);
    lv_obj_set_size(sw, 40, 21);
    lv_obj_align(sw, LV_ALIGN_BOTTOM_RIGHT, -2, -1);
    lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(sw, lv_color_hex(COL_OK), LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw, sw_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* ── Footer hint ─────────────────────────────────────────────────────── */
    lv_obj_t *hint = lv_label_create(scr);
    lv_label_set_text(hint, g_have_wheel
                      ? "Turn the wheel - the graph follows it    L3 = close"
                      : "Drag the dial - the graph follows it    L3 = close");
    lv_obj_set_style_text_color(hint, lv_color_hex(COL_MUTED), LV_PART_MAIN);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -6);

    /* ── Finger-follow dot (drawn on top, hidden until touched) ──────────── */
    g_touchdot = lv_obj_create(scr);
    lv_obj_set_size(g_touchdot, 14, 14);
    lv_obj_set_style_radius(g_touchdot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(g_touchdot, lv_color_hex(COL_ACCENT2), LV_PART_MAIN);
    lv_obj_set_style_border_width(g_touchdot, 0, LV_PART_MAIN);
    lv_obj_set_style_opa(g_touchdot, LV_OPA_70, LV_PART_MAIN);
    lv_obj_add_flag(g_touchdot, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(g_touchdot, LV_OBJ_FLAG_CLICKABLE);   /* never steal input */
}

/* ========================================================================== */
/*  Entry point                                                               */
/* ========================================================================== */
__attribute__((section(".text.entry")))
int app_entry(const app_services_t *svc)
{
    g_svc = svc;

    svc->log_printf("=== LVGL PAPP starting ===\n");

    if (svc->abi_version != PAPP_ABI_VERSION) {
        svc->log_printf("ABI mismatch: got %lu expected %d\n",
                        (unsigned long)svc->abi_version, PAPP_ABI_VERSION);
        return -1;
    }
    if (!svc->touch_read) {
        svc->log_printf("ERROR: launcher has no touch_read service (update it).\n");
        return -1;
    }

    /* LVGL renders into this canvas; DMA-capable so the scaler can read it. */
    g_canvas = (uint16_t *)svc->mem_caps_alloc(CANVAS_BYTES,
                                               PAPP_MEM_CAP_SPIRAM | PAPP_MEM_CAP_DMA);
    if (!g_canvas) {
        svc->log_printf("ERROR: canvas alloc failed (%d bytes)\n", CANVAS_BYTES);
        return -1;
    }

    /* ── Bring LVGL up ───────────────────────────────────────────────────── */
    /*
     * ESPHome already owns a live LVGL instance. PAPPs share that library,
     * so do not replace its tick callback: the callback would point into this
     * app after the PAPP is unmapped and crash the host on its next timer
     * pass. Standalone launchers may still enter with LVGL uninitialized.
     */
    const bool owns_lvgl = !lv_is_initialized();
    if (owns_lvgl) {
        lv_init();
        lv_tick_set_cb(tick_cb);
    }

    lv_display_t *disp = lv_display_create(CANVAS_W, CANVAS_H);
    lv_display_set_flush_cb(disp, disp_flush_cb);
    lv_display_set_buffers(disp, g_canvas, NULL, CANVAS_BYTES,
                           LV_DISPLAY_RENDER_MODE_FULL);

    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, touch_read_cb);
    /* LVGL 9.5 supports multiple displays and no longer reliably infers the
     * target display when the host already owns one. Bind this PAPP's input
     * device explicitly so touch events reach these controls. */
    lv_indev_set_display(indev, disp);

    /* Probe the physical analog wheel. paddle_read sits in the ABI's
     * append-only zone, so an older launcher will not have it at all; and
     * even a current launcher returns -1 on the HDMI build, where no paddle
     * is wired. Either way we fall back to the touch-driven dial. */
    if (svc->paddle_read) {
        /* The first call initialises the ADC. The reading itself is only
         * refreshed inside input_gamepad_read(), so each sample must be
         * preceded by an input poll. */
        (void)svc->paddle_read();

        /* Floating-pin detection, mirroring the Atari cores
         * (components/stella/stella_run.cpp, atari800_run.cpp):
         * a REAL potentiometer sits at a steady voltage, so its samples
         * barely move. A disconnected pin floats and picks up noise, so its
         * samples scatter. Hence a SMALL spread means the pot is present --
         * the opposite of the intuition that movement implies a real device. */
        int lo = 4095, hi = 0, raw = -1;
        for (int i = 0; i < PADDLE_SAMPLES; i++) {
            papp_gamepad_state_t dummy;
            svc->input_gamepad_read(&dummy);       /* triggers the ADC read */
            raw = svc->paddle_read();
            if (raw < 0) break;                    /* unavailable entirely   */
            if (raw < lo) lo = raw;
            if (raw > hi) hi = raw;
            svc->delay_ms(5);
        }

        if (raw < 0) {
            g_have_wheel = false;
            svc->log_printf("paddle probe: unavailable (raw=-1)\n");
        } else {
            int spread = hi - lo;
            g_have_wheel = (spread < PADDLE_DETECT_SPREAD);
            svc->log_printf("paddle probe: lo=%d hi=%d spread=%d -> wheel %s\n",
                            lo, hi, spread,
                            g_have_wheel ? "PRESENT" : "absent (floating?)");
        }
    } else {
        svc->log_printf("paddle probe: launcher has no paddle_read service\n");
    }

    build_ui();
    lv_timer_t *app_timer = lv_timer_create(tick_timer_cb, 100, NULL);

    svc->log_printf("LVGL up: %dx%d canvas, %d KB heap\n",
                    CANVAS_W, CANVAS_H, (int)(LV_MEM_SIZE / 1024));
    svc->log_printf("BUILD MARKER: v5 host-safe-lvgl-teardown\n");

    /* ── Main loop ───────────────────────────────────────────────────────── */
    papp_gamepad_state_t pad;
    for (;;) {
        svc->input_gamepad_read(&pad);
        if (g_svc->input_l3_read && g_svc->input_l3_read()) break;

        uint32_t next = lv_timer_handler();       /* renders + handles input */
        if (next > 20) next = 20;                 /* stay responsive to X    */
        svc->delay_ms((int)next ? (int)next : 1);
    }

    /* ── Tear down ───────────────────────────────────────────────────────── */
    /*
     * Delete only objects created by this PAPP. lv_deinit() destroys every
     * display, input device, timer, and allocator owned by the host LVGL
     * instance too; that is unsafe when this app is embedded in ESPHome.
     */
    if (app_timer != NULL)
        lv_timer_delete(app_timer);
    lv_indev_delete(indev);
    lv_display_delete(disp);
    if (owns_lvgl)
        lv_deinit();
    svc->mem_free(g_canvas);
    g_canvas = NULL;

    svc->display_clear(0x0000);
    svc->display_flush();
    svc->log_printf("=== LVGL PAPP exit ===\n");
    return 0;
}
