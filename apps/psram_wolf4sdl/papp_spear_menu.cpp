/* Small pre-menu for the three Spear of Destiny mission packs.
 *
 * This runs before Wolf4SDL opens any data files. It draws directly through
 * the PAPP display service so the campaign can be chosen before
 * CheckForEpisodes() selects the resource-file extension.
 */
#include "psram_app.h"
#include <stdint.h>
#include <string.h>

extern int param_mission;

static const uint8_t s_font[][5] = {
    /* A-Z */
    {0x0e,0x11,0x1f,0x11,0x11}, {0x1e,0x11,0x1e,0x11,0x1e},
    {0x0f,0x10,0x10,0x10,0x0f}, {0x1e,0x11,0x11,0x11,0x1e},
    {0x1f,0x10,0x1e,0x10,0x1f}, {0x1f,0x10,0x1e,0x10,0x10},
    {0x0f,0x10,0x17,0x11,0x0f}, {0x11,0x11,0x1f,0x11,0x11},
    {0x1f,0x04,0x04,0x04,0x1f}, {0x01,0x01,0x01,0x11,0x0e},
    {0x15,0x16,0x1c,0x16,0x15}, {0x10,0x10,0x10,0x10,0x1f},
    {0x11,0x1b,0x15,0x11,0x11}, {0x11,0x19,0x15,0x13,0x11},
    {0x0e,0x11,0x11,0x11,0x0e}, {0x1e,0x11,0x1e,0x10,0x10},
    {0x0e,0x11,0x11,0x15,0x0f}, {0x1e,0x11,0x1e,0x14,0x12},
    {0x0f,0x10,0x0e,0x01,0x1e}, {0x1f,0x04,0x04,0x04,0x04},
    {0x11,0x11,0x11,0x11,0x0e}, {0x11,0x11,0x11,0x0a,0x04},
    {0x11,0x11,0x15,0x1b,0x11}, {0x11,0x0a,0x04,0x0a,0x11},
    {0x11,0x0a,0x04,0x04,0x04}, {0x1f,0x02,0x04,0x08,0x1f},
    /* 0-9 */
    {0x0e,0x13,0x15,0x19,0x0e}, {0x04,0x0c,0x04,0x04,0x0e},
    {0x0e,0x11,0x02,0x04,0x1f}, {0x1e,0x01,0x06,0x01,0x1e},
    {0x02,0x06,0x0a,0x1f,0x02}, {0x1f,0x10,0x1e,0x01,0x1e},
    {0x06,0x08,0x1e,0x11,0x0e}, {0x1f,0x01,0x02,0x04,0x04},
    {0x0e,0x11,0x0e,0x11,0x0e}, {0x0e,0x11,0x0f,0x01,0x06},
};

static const uint8_t s_arrow[5] = {0x04,0x02,0x01,0x02,0x04};

static uint16_t rgb565(unsigned r, unsigned g, unsigned b)
{
    return (uint16_t)(((r & 0xf8u) << 8) | ((g & 0xfcu) << 3) | (b >> 3));
}

static const uint8_t *glyph(char c)
{
    if (c == '>') return s_arrow;
    if (c >= 'A' && c <= 'Z') return s_font[(unsigned)(c - 'A')];
    if (c >= '0' && c <= '9') return s_font[26u + (unsigned)(c - '0')];
    return NULL;
}

static int text_width(const char *value, int scale)
{
    return (int) strlen(value) * 6 * scale;
}

static void fill(uint16_t *fb, uint16_t color)
{
    for (unsigned i = 0; i < 320u * 200u; ++i) fb[i] = color;
}

static void rect(uint16_t *fb, int x, int y, int w, int h, uint16_t color)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > 320) w = 320 - x;
    if (y + h > 200) h = 200 - y;
    if (w <= 0 || h <= 0) return;
    for (int row = 0; row < h; ++row)
        for (int col = 0; col < w; ++col)
            fb[(y + row) * 320 + x + col] = color;
}

static void text(uint16_t *fb, int x, int y, const char *value,
                 uint16_t color, int scale)
{
    for (const char *p = value; *p; ++p) {
        const uint8_t *g = glyph(*p);
        if (g) {
            for (int row = 0; row < 5; ++row) {
                for (int col = 0; col < 5; ++col) {
                    /* Each byte is one horizontal scanline.  The previous
                     * lookup used g[col], transposing every character. */
                    if (!(g[row] & (1u << (4 - col)))) continue;
                    rect(fb, x + col * scale, y + row * scale,
                         scale, scale, color);
                }
            }
        }
        x += 6 * scale;
    }
}

static void text_centered(uint16_t *fb, int y, const char *value,
                          uint16_t color, int scale)
{
    text(fb, (320 - text_width(value, scale)) / 2, y, value, color, scale);
}

static int any_button(const papp_gamepad_state_t *pad)
{
    return pad->values[PAPP_INPUT_UP] || pad->values[PAPP_INPUT_DOWN] ||
           pad->values[PAPP_INPUT_A] || pad->values[PAPP_INPUT_B] ||
           pad->values[PAPP_INPUT_SELECT] || pad->values[PAPP_INPUT_START] ||
           pad->values[PAPP_INPUT_MENU];
}

static void wait_for_release(const app_services_t *svc)
{
    papp_gamepad_state_t pad;
    memset(&pad, 0, sizeof(pad));
    for (;;) {
        memset(&pad, 0, sizeof(pad));
        if (svc->input_gamepad_read) svc->input_gamepad_read(&pad);
        if (!any_button(&pad)) return;
        if (svc->delay_ms) svc->delay_ms(20);
    }
}

static void draw_menu(const app_services_t *svc, uint16_t *fb, int selected)
{
    const uint16_t bg = rgb565(5, 8, 18);
    const uint16_t panel = rgb565(11, 24, 46);
    const uint16_t white = rgb565(235, 245, 255);
    const uint16_t cyan = rgb565(52, 203, 245);
    const uint16_t yellow = rgb565(255, 216, 70);
    fill(fb, bg);
    rect(fb, 10, 10, 300, 180, panel);
    rect(fb, 12, 12, 296, 2, cyan);
    rect(fb, 12, 186, 296, 2, cyan);
    text_centered(fb, 24, "SPEAR OF DESTINY", cyan, 2);
    text_centered(fb, 43, "SELECT MISSION", white, 2);

    static const char *const labels[] = {
        "ORIGINAL CAMPAIGN",
        "RETURN TO DANGER",
        "ULTIMATE CHALLENGE",
        "QUIT TO LAUNCHER"
    };
    for (int i = 0; i < 4; ++i) {
        int y = 70 + i * 27;
        int arrow_width = text_width(">", 2);
        int label_width = text_width(labels[i], 2);
        int gap = 4;
        int group_x = (320 - arrow_width - gap - label_width) / 2;
        if (i == selected) {
            rect(fb, 20, y - 4, 280, 21, rgb565(33, 53, 72));
            text(fb, group_x, y, ">", yellow, 2);
            text(fb, group_x + arrow_width + gap, y, labels[i], yellow, 2);
        } else {
            text(fb, group_x + arrow_width + gap, y, labels[i], white, 2);
        }
    }
    text_centered(fb, 168, "A SELECTS   B QUIT   UP DOWN CHOOSE", cyan, 1);
    svc->display_write_frame_custom(fb, 320, 200, 2.4f, false);
}

extern "C" int papp_spear_select_mission(const app_services_t *svc)
{
    if (!svc || !svc->input_gamepad_read ||
        !svc->display_write_frame_custom || !svc->mem_caps_alloc)
        return 0;

    uint16_t *fb = (uint16_t *)svc->mem_caps_alloc(
        320u * 200u * sizeof(uint16_t), PAPP_MEM_CAP_SPIRAM);
    if (!fb) return 0;

    wait_for_release(svc);
    int selected = 0;
    int last_up = 0, last_down = 0, last_a = 0, last_b = 0;
    for (;;) {
        draw_menu(svc, fb, selected);
        papp_gamepad_state_t pad;
        memset(&pad, 0, sizeof(pad));
        svc->input_gamepad_read(&pad);
        int up = pad.values[PAPP_INPUT_UP] != 0;
        int down = pad.values[PAPP_INPUT_DOWN] != 0;
        int a = pad.values[PAPP_INPUT_A] != 0;
        int b = pad.values[PAPP_INPUT_B] != 0;
        if (up && !last_up) selected = (selected + 3) % 4;
        if (down && !last_down) selected = (selected + 1) % 4;
        if (a && !last_a) {
            if (selected == 3) {
                wait_for_release(svc);
                svc->mem_free(fb);
                return -1;
            }
            break;
        }
        if (b && !last_b) {
            wait_for_release(svc);
            svc->mem_free(fb);
            return -1;
        }
        last_up = up;
        last_down = down;
        last_a = a;
        last_b = b;
        if (svc->delay_ms) svc->delay_ms(25);
    }
    wait_for_release(svc);
    svc->mem_free(fb);
    param_mission = selected;
    if (svc->log_printf)
        svc->log_printf("Spear mission menu: selected %d (%s)\n",
                        selected + 1,
                        selected == 0 ? "Original Campaign" :
                        selected == 1 ? "Return to Danger" :
                                         "Ultimate Challenge");
    return selected;
}
