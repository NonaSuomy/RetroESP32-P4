/* AZIP Z-machine interpreter adapted to the ESP32-P4 PAPP ABI.
 *
 * The upstream firmware uses a FabGL terminal and keeps its story file in
 * MEMORY.DAT.  A PAPP instead uses the launcher's LCD, keyboard/gamepad and
 * SD services.  The interpreter itself remains unchanged, including the
 * original Infocom/Inform story format support.
 */
#define PAPP_APP_SIDE 1

#include "runtime.h"
#include "ztypes.h"
#include "font8x16.h"

#include <setjmp.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

const app_services_t *_papp_svc;
static jmp_buf s_exit_env;
static uint16_t *s_frame;
static uint8_t *s_story;
static size_t s_story_size;
static char s_story_path[512];
static char s_save_path[512];
static int s_cursor_x;
static int s_cursor_y;
static int s_last_a;
static int s_last_b;

extern "C" void app_return_to_launcher(void);

static const int SCREEN_W = 400;
static const int SCREEN_H = 240;
static const int CHAR_W = 8;
static const int CHAR_H = 16;
static const int SCREEN_COLS = SCREEN_W / CHAR_W;
static const int SCREEN_ROWS = SCREEN_H / CHAR_H;

static uint16_t rgb565(unsigned r, unsigned g, unsigned b)
{
    return (uint16_t)(((r & 0xf8u) << 8) | ((g & 0xfcu) << 3) | (b >> 3));
}

static void clear_frame(void)
{
    if (!s_frame) return;
    const uint16_t black = rgb565(0, 0, 0);
    for (unsigned i = 0; i < (unsigned)(SCREEN_W * SCREEN_H); ++i)
        s_frame[i] = black;
}

static void draw_char(int x, int y, unsigned char c)
{
    if (!s_frame || c < 32 || c > 126) return;
    const uint16_t ink = rgb565(224, 238, 224);
    const uint8_t *glyph = FONT_8x16[c - 32];
    for (int row = 0; row < CHAR_H; ++row) {
        uint16_t *dst = s_frame + (y + row) * SCREEN_W + x;
        uint8_t bits = glyph[row];
        for (int col = 0; col < CHAR_W; ++col)
            dst[col] = (bits & (uint8_t)(0x80u >> col)) ? ink : 0;
    }
}

static void flush_frame(void)
{
    if (_papp_svc && _papp_svc->display_write_frame_custom && s_frame)
        _papp_svc->display_write_frame_custom(s_frame, SCREEN_W, SCREEN_H,
                                              2.0f, false);
}

static void scroll_text(void)
{
    if (!s_frame) return;
    for (int y = CHAR_H; y < SCREEN_H; ++y) {
        uint16_t *dst = s_frame + (y - CHAR_H) * SCREEN_W;
        const uint16_t *src = s_frame + y * SCREEN_W;
        for (int x = 0; x < SCREEN_W; ++x) dst[x] = src[x];
    }
    for (int y = SCREEN_H - CHAR_H; y < SCREEN_H; ++y)
        for (int x = 0; x < SCREEN_W; ++x) s_frame[y * SCREEN_W + x] = 0;
    s_cursor_y = SCREEN_ROWS - 1;
}

static void next_line(void)
{
    s_cursor_x = 0;
    ++s_cursor_y;
    if (s_cursor_y >= SCREEN_ROWS) scroll_text();
}

void azip_process_output(int c)
{
    if (c == '\r') return;
    if (c == '\n') {
        next_line();
        flush_frame();
        return;
    }
    if (c == '\b') {
        if (s_cursor_x > 0) {
            --s_cursor_x;
            draw_char(s_cursor_x * CHAR_W, s_cursor_y * CHAR_H, ' ');
        }
        return;
    }
    if (c < 32 || c > 126) return;
    if (s_cursor_x >= SCREEN_COLS) next_line();
    draw_char(s_cursor_x * CHAR_W, s_cursor_y * CHAR_H, (unsigned char)c);
    ++s_cursor_x;
    /* Avoid pushing a full LCD frame for every character while still making
     * prompts visible before the interpreter starts waiting for input. */
    if ((s_cursor_x & 15) == 0) flush_frame();
}

static int poll_keyboard(int *value)
{
    if (!_papp_svc || !_papp_svc->input_keyboard_read) return 0;
    papp_keyboard_event_t event;
    while (_papp_svc->input_keyboard_read(&event)) {
        if (!event.down) continue;
        *value = event.key;
        return 1;
    }
    return 0;
}

static int poll_gamepad(int *value)
{
    if (!_papp_svc || !_papp_svc->input_gamepad_read) return 0;
    papp_gamepad_state_t pad;
    memset(&pad, 0, sizeof(pad));
    _papp_svc->input_gamepad_read(&pad);
    if (emu_quit(&pad)) app_return_to_launcher();

    int a = pad.values[PAPP_INPUT_A] != 0;
    int b = pad.values[PAPP_INPUT_B] != 0;
    int result = 0;
    if (a && !s_last_a) { *value = ' '; result = 1; }
    else if (b && !s_last_b) { *value = '\r'; result = 1; }
    else if (pad.values[PAPP_INPUT_UP]) { *value = 0x80; result = 1; }
    else if (pad.values[PAPP_INPUT_DOWN]) { *value = 0x81; result = 1; }
    s_last_a = a;
    s_last_b = b;
    return result;
}

int azip_read_character(void)
{
    for (;;) {
        int c = 0;
        if (poll_keyboard(&c)) return c;
        if (poll_gamepad(&c)) return c;
        if (_papp_svc && _papp_svc->delay_ms) _papp_svc->delay_ms(8);
    }
}

int azip_read_line(int buflen, unsigned long addr, int *read_size)
{
    if (buflen < 1) return '\r';
    int count = read_size ? *read_size : 0;
    if (count < 0) count = 0;
    if (count >= buflen) count = buflen - 1;
    flush_frame();
    for (;;) {
        int c = 0;
        if (!poll_keyboard(&c) && !poll_gamepad(&c)) {
            if (_papp_svc && _papp_svc->delay_ms) _papp_svc->delay_ms(8);
            continue;
        }
        if (c == 0x80 || c == 0x81) continue;
        if (c == 8 || c == 127) {
            if (count > 0) {
                --count;
                set_byte(addr + (unsigned long)count, 0);
                azip_process_output('\b');
                flush_frame();
            }
            continue;
        }
        if (c == '\r' || c == '\n') {
            if (read_size) *read_size = count;
            azip_process_output('\r');
            azip_process_output('\n');
            return '\r';
        }
        if (c < 32 || c > 126 || count >= buflen - 1) continue;
        set_byte(addr + (unsigned long)count, (zbyte_t)c);
        ++count;
        azip_process_output(c);
    }
}

static uint32_t s_random_state = 0x6d2b79f5u;

unsigned long azip_random(void)
{
    s_random_state = s_random_state * 1664525u + 1013904223u;
    return s_random_state;
}

void azip_seed(unsigned int seed)
{
    s_random_state = seed ? seed : 1u;
}

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t version;
    uint32_t story_size;
    uint32_t dynamic_size;
    uint32_t pc;
    uint16_t sp;
    uint16_t fp;
    uint16_t frame_count;
    uint16_t interpreter_state;
    uint16_t interpreter_status;
    uint16_t reserved;
} azip_save_header_t;

static const uint32_t AZIP_SAVE_MAGIC = 0x415A5356u; /* AZSV */

int azip_save_state(void)
{
    if (!_papp_svc || !_papp_svc->file_open || !_papp_svc->file_write ||
        !_papp_svc->file_close || !s_story || s_save_path[0] == '\0') return 0;
    unsigned long dynamic_size = (unsigned long)h_data_size * 2ul;
    if (dynamic_size > azip_story_size()) dynamic_size = azip_story_size();
    azip_save_header_t header = { AZIP_SAVE_MAGIC, 1u,
        (uint32_t)azip_story_size(), (uint32_t)dynamic_size, (uint32_t)pc,
        sp, fp, frame_count, (uint16_t)interpreter_state,
        (uint16_t)interpreter_status, 0 };
    void *file = _papp_svc->file_open(s_save_path, "wb");
    if (!file) return 0;
    int ok = _papp_svc->file_write(&header, 1, sizeof(header), file) == sizeof(header);
    if (ok) ok = _papp_svc->file_write(s_story, 1, dynamic_size, file) == dynamic_size;
    if (ok) ok = _papp_svc->file_write(stack, 1, sizeof(stack), file) == sizeof(stack);
    _papp_svc->file_close(file);
    if (_papp_svc->log_printf)
        _papp_svc->log_printf("AZIP: saved game state to %s\n", s_save_path);
    return ok;
}

int azip_restore_state(void)
{
    if (!_papp_svc || !_papp_svc->file_open || !_papp_svc->file_read ||
        !_papp_svc->file_close || !s_story || s_save_path[0] == '\0') return 0;
    void *file = _papp_svc->file_open(s_save_path, "rb");
    if (!file) return 0;
    azip_save_header_t header;
    int ok = _papp_svc->file_read(&header, 1, sizeof(header), file) == sizeof(header);
    unsigned long dynamic_size = (unsigned long)h_data_size * 2ul;
    if (dynamic_size > azip_story_size()) dynamic_size = azip_story_size();
    if (!ok || header.magic != AZIP_SAVE_MAGIC || header.version != 1u ||
        header.story_size != azip_story_size() || header.dynamic_size != dynamic_size) ok = 0;
    if (ok) ok = _papp_svc->file_read(s_story, 1, dynamic_size, file) == dynamic_size;
    if (ok) ok = _papp_svc->file_read(stack, 1, sizeof(stack), file) == sizeof(stack);
    _papp_svc->file_close(file);
    if (ok) {
        pc = header.pc;
        sp = header.sp;
        fp = header.fp;
        frame_count = header.frame_count;
        interpreter_state = header.interpreter_state;
        interpreter_status = header.interpreter_status;
        if (_papp_svc->log_printf)
            _papp_svc->log_printf("AZIP: restored game state from %s\n", s_save_path);
    }
    return ok;
}

extern "C" void app_return_to_launcher(void)
{
    longjmp(s_exit_env, 1);
}

void *operator new(size_t size) noexcept
{
    return _papp_svc ? _papp_svc->mem_alloc(size) : NULL;
}
void *operator new[](size_t size) noexcept
{
    return _papp_svc ? _papp_svc->mem_alloc(size) : NULL;
}
void operator delete(void *p) noexcept { if (p && _papp_svc) _papp_svc->mem_free(p); }
void operator delete[](void *p) noexcept { if (p && _papp_svc) _papp_svc->mem_free(p); }
void operator delete(void *p, size_t) noexcept { if (p && _papp_svc) _papp_svc->mem_free(p); }
void operator delete[](void *p, size_t) noexcept { if (p && _papp_svc) _papp_svc->mem_free(p); }

extern "C" __attribute__((section(".text.entry"), used))
int app_entry(const app_services_t *svc)
{
    if (!svc || svc->abi_version != PAPP_ABI_VERSION) return -1;
    _papp_svc = svc;
    s_frame = NULL;
    s_story = NULL;
    s_story_size = 0;
    s_story_path[0] = '\0';
    s_save_path[0] = '\0';
    s_cursor_x = 0;
    s_cursor_y = 0;
    s_last_a = 0;
    s_last_b = 0;

    uint8_t *story = NULL;
    size_t story_size = 0;
    if (emu_load_rom("azip", ".dat|.z1|.z2|.z3|.z4|.z5|.z6|.z7|.z8|",
                     &story, &story_size, s_story_path,
                     sizeof(s_story_path))) {
        svc->log_printf("AZIP: set /sd/roms/papp/azip.rom to a .DAT or Z-code story\n");
        return -1;
    }
    if (story_size < 64 || story_size > 16u * 1024u * 1024u) {
        svc->log_printf("AZIP: story size is invalid (%u bytes)\n", (unsigned)story_size);
        svc->mem_free(story);
        return -1;
    }

    s_story = story;
    s_story_size = story_size;
    azip_set_story(s_story, (unsigned long)s_story_size);
    strncpy(s_save_path, s_story_path, sizeof(s_save_path) - 5);
    s_save_path[sizeof(s_save_path) - 5] = '\0';
    strncat(s_save_path, ".sav", sizeof(s_save_path) - strlen(s_save_path) - 1);

    s_frame = (uint16_t *)svc->mem_caps_alloc(
        (size_t)SCREEN_W * SCREEN_H * sizeof(uint16_t), PAPP_MEM_CAP_SPIRAM);
    if (!s_frame) {
        svc->mem_free(story);
        return -1;
    }
    clear_frame();
    flush_frame();
    svc->log_printf("AZIP PAPP: running %s (%u bytes)\n",
                    s_story_path, (unsigned)s_story_size);

    int result = 0;
    if (setjmp(s_exit_env) == 0) {
        initialize_screen();
        configure(V1, V8);
        z_restart();
        interpret();
    }

    if (svc->display_clear) svc->display_clear(0);
    if (svc->display_flush) svc->display_flush();
    svc->mem_free(s_frame);
    svc->mem_free(story);
    s_frame = NULL;
    s_story = NULL;
    s_story_size = 0;
    return result;
}
