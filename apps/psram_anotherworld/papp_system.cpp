#include "papp_system.h"
#include "Arduino.h"

#include <string.h>

PappSerial Serial;
static PappAnotherWorldSystem *s_system;

unsigned long millis(void)
{
    return s_system ? s_system->getTimeStamp() : 0;
}

void delay(unsigned long milliseconds)
{
    if (s_system) s_system->sleep((unsigned int)milliseconds);
}

PappAnotherWorldSystem::PappAnotherWorldSystem(const app_services_t *services)
    : _svc(services), _initialized(false), _frame(NULL), _audioCallback(NULL), _audioParam(NULL),
      _nextTimerId(1), _audioRemainder(0)
{
    memset(_palette, 0, sizeof(_palette));
    memset(_timers, 0, sizeof(_timers));
    s_system = this;
}

PappAnotherWorldSystem::~PappAnotherWorldSystem()
{
    destroy();
    if (s_system == this) s_system = NULL;
}

void PappAnotherWorldSystem::init(const char *title)
{
    (void)title;
    input = PlayerInput();
    _frame = (uint16_t *)_svc->mem_caps_alloc(
        320u * 200u * sizeof(uint16_t), PAPP_MEM_CAP_SPIRAM);
    if (_frame) memset(_frame, 0, 320u * 200u * sizeof(uint16_t));
    _svc->audio_init((int)getOutputSampleRate());
    _initialized = true;
    if (_svc->display_write_frame_custom && _frame)
        _svc->display_write_frame_custom(_frame, 320, 200, 2.4f, false);
}

void PappAnotherWorldSystem::destroy()
{
    if (!_initialized && !_frame && !_audioCallback) return;
    stopAudio();
    if (_svc && _svc->audio_init) _svc->audio_init(0);
    if (_frame && _svc && _svc->mem_free) _svc->mem_free(_frame);
    _frame = NULL;
    _initialized = false;
}

void PappAnotherWorldSystem::setPalette(const unsigned char *buf)
{
    if (!buf) return;
    for (int i = 0; i < NUM_COLORS; ++i)
        _palette[i] = (uint16_t)(((uint16_t)buf[i * 2] << 8) | buf[i * 2 + 1]);
}

void PappAnotherWorldSystem::updateDisplay(const unsigned char *buf)
{
    if (!buf || !_frame || !_svc || !_svc->display_write_frame_custom) return;
    for (int y = 0; y < 200; ++y) {
        const unsigned char *src = buf + y * 160;
        uint16_t *dst = _frame + y * 320;
        for (int x = 0; x < 160; ++x) {
            unsigned char packed = src[x];
            dst[x * 2] = _palette[(packed >> 4) & 0x0f];
            dst[x * 2 + 1] = _palette[packed & 0x0f];
        }
    }
    _svc->display_write_frame_custom(_frame, 320, 200, 2.4f, false);
}

static bool key_is(int key, int a, int b)
{
    return key == a || key == b;
}

void PappAnotherWorldSystem::processEvents()
{
    input.dirMask = 0;
    input.button = false;
    input.lastChar = 0;

    papp_gamepad_state_t pad;
    memset(&pad, 0, sizeof(pad));
    if (_svc && _svc->input_gamepad_read) _svc->input_gamepad_read(&pad);
    input.dirMask |= pad.values[PAPP_INPUT_LEFT] ? PlayerInput::DIR_LEFT : 0;
    input.dirMask |= pad.values[PAPP_INPUT_RIGHT] ? PlayerInput::DIR_RIGHT : 0;
    input.dirMask |= pad.values[PAPP_INPUT_UP] ? PlayerInput::DIR_UP : 0;
    input.dirMask |= pad.values[PAPP_INPUT_DOWN] ? PlayerInput::DIR_DOWN : 0;
    input.button = pad.values[PAPP_INPUT_A] || pad.values[PAPP_INPUT_B];
    input.quit = pad.values[PAPP_INPUT_MENU] ||
                 (_svc->input_l3_read && _svc->input_l3_read());

    /* USB keyboard events supplement the controller mapping. The launcher
     * normally exposes keyboard keys through the gamepad state, but consume
     * the event queue too when a newer launcher provides it. */
    if (_svc && _svc->input_keyboard_read) {
        papp_keyboard_event_t event;
        while (_svc->input_keyboard_read(&event)) {
            if (!event.down) continue;
            const int k = event.key;
            if (k == 27) { input.quit = true; continue; }
            if (key_is(k, 273, 128)) input.dirMask |= PlayerInput::DIR_UP;
            else if (key_is(k, 274, 129)) input.dirMask |= PlayerInput::DIR_DOWN;
            else if (key_is(k, 276, 130)) input.dirMask |= PlayerInput::DIR_LEFT;
            else if (key_is(k, 275, 131)) input.dirMask |= PlayerInput::DIR_RIGHT;
            else if (k == 'w' || k == 'W') input.dirMask |= PlayerInput::DIR_UP;
            else if (k == 's' || k == 'S') input.dirMask |= PlayerInput::DIR_DOWN;
            else if (k == 'a' || k == 'A') input.dirMask |= PlayerInput::DIR_LEFT;
            else if (k == 'd' || k == 'D') input.dirMask |= PlayerInput::DIR_RIGHT;
            else if (k == ' ' || k == 13) input.button = true;
            if (k >= 0 && k < 256) input.lastChar = (char)k;
        }
    }
    serviceTimers();
}

void PappAnotherWorldSystem::sleep(unsigned int duration)
{
    unsigned int remaining = duration;
    while (remaining) {
        unsigned int step = remaining > 5 ? 5 : remaining;
        if (_svc && _svc->delay_ms) _svc->delay_ms((int)step);
        remaining -= step;
        _audioRemainder += step * getOutputSampleRate();
        unsigned int frames = _audioRemainder / 1000u;
        _audioRemainder %= 1000u;
        if (frames) pumpAudio(frames);
        serviceTimers();
        if (input.quit) return;
    }
}

unsigned int PappAnotherWorldSystem::getTimeStamp()
{
    return (_svc && _svc->get_time_us) ?
        (unsigned int)(_svc->get_time_us() / 1000) : 0;
}

void PappAnotherWorldSystem::startAudio(AudioCallback callback, void *param)
{
    _audioCallback = callback;
    _audioParam = param;
}

void PappAnotherWorldSystem::stopAudio()
{
    _audioCallback = NULL;
    _audioParam = NULL;
}

unsigned int PappAnotherWorldSystem::getOutputSampleRate()
{
    return 22050;
}

int PappAnotherWorldSystem::addTimer(unsigned int delayMs, TimerCallback callback,
                                     void *param)
{
    for (unsigned i = 0; i < sizeof(_timers) / sizeof(_timers[0]); ++i) {
        if (_timers[i].active) continue;
        _timers[i].active = true;
        _timers[i].due = getTimeStamp() + (delayMs ? delayMs : 1);
        _timers[i].interval = delayMs ? delayMs : 1;
        _timers[i].callback = callback;
        _timers[i].param = param;
        return (int)_nextTimerId++;
    }
    return 0;
}

void PappAnotherWorldSystem::removeTimer(int timerId)
{
    if (!timerId) return;
    /* The upstream only needs one music timer at a time. Clearing all active
     * timers keeps the implementation deterministic without a handle table. */
    for (unsigned i = 0; i < sizeof(_timers) / sizeof(_timers[0]); ++i)
        _timers[i].active = false;
}

void PappAnotherWorldSystem::serviceTimers()
{
    const unsigned now = getTimeStamp();
    for (unsigned i = 0; i < sizeof(_timers) / sizeof(_timers[0]); ++i) {
        Timer &timer = _timers[i];
        if (!timer.active || (int)(now - timer.due) < 0) continue;
        unsigned next = timer.callback ? timer.callback(timer.interval, timer.param) : 0;
        if (!next) next = timer.interval ? timer.interval : 1;
        timer.due = now + next;
    }
}

void PappAnotherWorldSystem::pumpAudio(unsigned int frames)
{
    if (!_audioCallback || !_svc || !_svc->audio_submit) return;
    static unsigned char mono[512];
    static short stereo[1024];
    while (frames) {
        unsigned chunk = frames > 512 ? 512 : frames;
        _audioCallback(_audioParam, mono, (int)chunk);
        for (unsigned i = 0; i < chunk; ++i) {
            short sample = (short)(((signed char)mono[i]) << 8);
            stereo[i * 2] = sample;
            stereo[i * 2 + 1] = sample;
        }
        _svc->audio_submit(stereo, (int)chunk);
        frames -= chunk;
    }
}

void *PappAnotherWorldSystem::createMutex() { return this; }
void PappAnotherWorldSystem::destroyMutex(void *mutex) { (void)mutex; }
void PappAnotherWorldSystem::lockMutex(void *mutex) { (void)mutex; }
void PappAnotherWorldSystem::unlockMutex(void *mutex) { (void)mutex; }
