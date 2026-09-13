#include "psram_app.h"
#include "SDL.h"
#include <string.h>

extern const app_services_t *_papp_svc;

/* Wolf4SDL expects these legacy input-mode globals from SDL_event.c. */
int keyMode = 1;
int weaponToggle = 1;

typedef struct {
    int papp_button;
    SDL_Keycode key;
    SDL_Scancode scancode;
} key_binding_t;

static const key_binding_t s_bindings[] = {
    {PAPP_INPUT_UP,     SDLK_UP,    SDL_SCANCODE_UP},
    {PAPP_INPUT_RIGHT,  SDLK_RIGHT, SDL_SCANCODE_RIGHT},
    {PAPP_INPUT_DOWN,   SDLK_DOWN,  SDL_SCANCODE_DOWN},
    {PAPP_INPUT_LEFT,   SDLK_LEFT,  SDL_SCANCODE_LEFT},
    {PAPP_INPUT_SELECT, SDLK_SPACE, SDL_SCANCODE_SPACE},
    {PAPP_INPUT_START,  SDLK_ESCAPE, SDL_SCANCODE_ESCAPE}, /* in-game menu */
    {PAPP_INPUT_A,      SDLK_LCTRL, SDL_SCANCODE_LCTRL},
    {PAPP_INPUT_B,      SDLK_LSHIFT, SDL_SCANCODE_LSHIFT},
    {PAPP_INPUT_X,      SDLK_SPACE,  SDL_SCANCODE_SPACE},
    {PAPP_INPUT_Y,      SDLK_RETURN, SDL_SCANCODE_RETURN},
    {PAPP_INPUT_L,      SDLK_1,     SDL_SCANCODE_1},
    {PAPP_INPUT_R,      SDLK_2,     SDL_SCANCODE_2},
    {PAPP_INPUT_MENU,   SDLK_ESCAPE, SDL_SCANCODE_ESCAPE}
};

static SDL_Event s_queue[16];
static unsigned s_head, s_tail;
static int s_last[PAPP_INPUT_MAX];
static int s_have_last;
static Uint32 s_hold_start;
static int s_hold_active;
static int s_close_sent;
static unsigned s_input_log_mask;

static void queue_key(SDL_Keycode key, SDL_Scancode scancode, int down)
{
    unsigned next = (s_tail + 1u) % (unsigned)(sizeof(s_queue) / sizeof(s_queue[0]));
    if (next == s_head) return;
    SDL_Event *event = &s_queue[s_tail];
    memset(event, 0, sizeof(*event));
    event->key.type = down ? SDL_KEYDOWN : SDL_KEYUP;
    event->key.timestamp = SDL_GetTicks();
    event->key.state = down ? SDL_PRESSED : SDL_RELEASED;
    event->key.keysym.sym = key;
    event->key.keysym.scancode = scancode;
    s_tail = next;
}

static void queue_quit(void)
{
    unsigned next = (s_tail + 1u) % (unsigned)(sizeof(s_queue) / sizeof(s_queue[0]));
    if (next == s_head) return;
    memset(&s_queue[s_tail], 0, sizeof(s_queue[s_tail]));
    s_queue[s_tail].type = SDL_QUIT;
    s_tail = next;
}

static void fill_queue(void)
{
    papp_gamepad_state_t pad;
    memset(&pad, 0, sizeof(pad));
    if (_papp_svc && _papp_svc->input_gamepad_read)
        _papp_svc->input_gamepad_read(&pad);

    /* The shared launcher layer mirrors X/Y into its virtual MENU/VOLUME
     * fields for emulators that do not consume those face buttons. Wolf uses
     * both buttons natively, so discard only those derived aliases here; an
     * X hold must remain the run key and must not become a delayed close. */
    if (pad.values[PAPP_INPUT_X]) pad.values[PAPP_INPUT_MENU] = 0;
    if (pad.values[PAPP_INPUT_Y]) pad.values[PAPP_INPUT_VOLUME] = 0;

    if (!s_have_last) {
        memcpy(s_last, pad.values, sizeof(s_last));
        s_have_last = 1;
    }
    for (unsigned i = 0; i < sizeof(s_bindings) / sizeof(s_bindings[0]); ++i) {
        int b = s_bindings[i].papp_button;
        int now = pad.values[b] ? 1 : 0;
        if (now != s_last[b])
            queue_key(s_bindings[i].key, s_bindings[i].scancode, now);
    }
    memcpy(s_last, pad.values, sizeof(s_last));

    unsigned input_mask = 0;
    for (int i = 0; i < PAPP_INPUT_MAX; ++i)
        if (pad.values[i]) input_mask |= 1u << i;
    if (input_mask != s_input_log_mask) {
        s_input_log_mask = input_mask;
        if (_papp_svc && _papp_svc->log_printf)
            _papp_svc->log_printf("Wolf4SDL input mask=0x%04x (A=fire B=use X=run)\n",
                                  input_mask);
    }

    /* START is the dedicated Escape key for Wolf's in-game menu. MENU remains
     * an Escape key for a short press as well,
     * while a held MENU or L3 follows the launcher convention and closes the
     * PAPP. Generate SDL_QUIT here so Quit() longjmps on the game task. */
    int close_button = pad.values[PAPP_INPUT_MENU] ? 1 : 0;
    if (_papp_svc && _papp_svc->input_l3_read && _papp_svc->input_l3_read())
        close_button = 1;
    Uint32 now = SDL_GetTicks();
    if (!close_button) {
        s_hold_active = 0;
        s_close_sent = 0;
    } else if (!s_hold_active) {
        s_hold_active = 1;
        s_hold_start = now;
    } else if (!s_close_sent && (Uint32)(now - s_hold_start) >= 750u) {
        queue_quit();
        s_close_sent = 1;
    }

    if (_papp_svc && _papp_svc->input_keyboard_read) {
        papp_keyboard_event_t event;
        while (s_head != (s_tail + 1u) % (unsigned)(sizeof(s_queue) / sizeof(s_queue[0])) &&
               _papp_svc->input_keyboard_read(&event))
            queue_key((SDL_Keycode)event.key, (SDL_Scancode)0, event.down != 0);
    }
}

int SDL_PollEvent(SDL_Event *event)
{
    if (s_head == s_tail) fill_queue();
    if (s_head == s_tail) return 0;
    if (event) *event = s_queue[s_head];
    s_head = (s_head + 1u) % (unsigned)(sizeof(s_queue) / sizeof(s_queue[0]));
    return 1;
}

int SDL_WaitEvent(SDL_Event *event)
{
    while (!SDL_PollEvent(event)) SDL_Delay(5);
    return 1;
}

int SDL_WaitEventTimeout(SDL_Event *event, int timeout)
{
    Uint32 start = SDL_GetTicks();
    do {
        if (SDL_PollEvent(event)) return 1;
        SDL_Delay(5);
    } while ((int)(SDL_GetTicks() - start) < timeout);
    return 0;
}

Uint8 SDL_EventState(Uint32 type, int state)
{
    (void)type;
    return (state == SDL_QUERY) ? SDL_ENABLE : (Uint8)state;
}

void SDL_SetModState(SDL_Keymod modstate) { (void)modstate; }
SDL_Keymod SDL_GetModState(void) { return KMOD_NONE; }
SDL_GrabMode SDL_WM_GrabInput(SDL_GrabMode mode) { return mode; }
int SDL_ShowCursor(int toggle) { return toggle; }
Uint32 SDL_GetMouseState(int *x, int *y) { if (x) *x = 0; if (y) *y = 0; return 0; }
void SDL_WarpMouse(Uint16 x, Uint16 y) { (void)x; (void)y; }

int SDL_NumJoysticks(void) { return 0; }
SDL_Joystick *SDL_JoystickOpen(int device_index) { (void)device_index; return NULL; }
void SDL_JoystickClose(SDL_Joystick *joystick) { (void)joystick; }
void SDL_JoystickUpdate(void) {}
int SDL_JoystickNumAxes(SDL_Joystick *joystick) { (void)joystick; return 0; }
int SDL_JoystickNumHats(SDL_Joystick *joystick) { (void)joystick; return 0; }
int SDL_JoystickNumButtons(SDL_Joystick *joystick) { (void)joystick; return 0; }
Sint16 SDL_JoystickGetAxis(SDL_Joystick *joystick, int axis)
{ (void)joystick; (void)axis; return 0; }
Uint8 SDL_JoystickGetHat(SDL_Joystick *joystick, int hat)
{ (void)joystick; (void)hat; return 0; }
Uint8 SDL_JoystickGetButton(SDL_Joystick *joystick, int button)
{ (void)joystick; (void)button; return 0; }
