#ifndef _SDL_events_h
#define _SDL_events_h

#include "SDL_stdinc.h"
#include "SDL_input.h"

#define SDL_RELEASED 0
#define SDL_PRESSED  1
#define SDL_QUERY    -1
#define SDL_IGNORE   0
#define SDL_DISABLE  0
#define SDL_ENABLE   1

enum {
    SDL_FIRSTEVENT = 0,
    SDL_QUIT = 0x100,
    SDL_KEYDOWN = 0x300,
    SDL_KEYUP,
    SDL_MOUSEMOTION = 0x400,
    SDL_MOUSEBUTTONDOWN,
    SDL_MOUSEBUTTONUP,
    SDL_MOUSEWHEEL,
    SDL_JOYAXISMOTION = 0x600,
    SDL_JOYBUTTONDOWN,
    SDL_JOYBUTTONUP,
    SDL_LASTEVENT = 0xFFFF
};

typedef struct {
    Uint32 type;
    Uint32 timestamp;
    Uint32 windowID;
    Uint8 state;
    Uint8 repeat;
    Uint8 padding2;
    Uint8 padding3;
    SDL_Keysym keysym;
} SDL_KeyboardEvent;

typedef union SDL_Event {
    Uint32 type;
    SDL_KeyboardEvent key;
} SDL_Event;

int SDL_PollEvent(SDL_Event *event);
int SDL_WaitEvent(SDL_Event *event);
int SDL_WaitEventTimeout(SDL_Event *event, int timeout);
Uint8 SDL_EventState(Uint32 type, int state);

#endif
