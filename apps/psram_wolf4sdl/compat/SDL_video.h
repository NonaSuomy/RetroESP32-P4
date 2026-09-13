#ifndef SDL_TFT_H
#define SDL_TFT_H

#include "SDL_stdinc.h"
#include "SDL_error.h"
#include "SDL_rwops.h"

typedef struct {
    Uint8 r;
    Uint8 g;
    Uint8 b;
    Uint8 unused;
} SDL_Color;

typedef struct {
    int ncolors;
    SDL_Color *colors;
} SDL_Palette;

typedef struct SDL_PixelFormat {
    SDL_Palette *palette;
    Uint8 BitsPerPixel;
    Uint8 BytesPerPixel;
    Uint8 Rloss, Gloss, Bloss, Aloss;
    Uint8 Rshift, Gshift, Bshift, Ashift;
    Uint32 Rmask, Gmask, Bmask, Amask;
    Uint32 colorkey;
    Uint8 alpha;
} SDL_PixelFormat;

typedef struct SDL_Surface {
    Uint32 flags;
    SDL_PixelFormat *format;
    int w, h;
    Uint16 pitch;
    void *pixels;
    int offset;
    void *hwdata;
    SDL_Rect clip_rect;
    Uint32 unused1;
    Uint32 locked;
    void *map;
    unsigned int format_version;
    int refcount;
} SDL_Surface;

#define SDL_SWSURFACE   0x00000000
#define SDL_HWSURFACE   0x00000001
#define SDL_DOUBLEBUF   0x40000000
#define SDL_FULLSCREEN  0x80000000
#define SDL_HWPALETTE   0x20000000
#define SDL_RLEACCEL    0x00004000
#define SDL_MUSTLOCK(surface) (((surface)->flags & SDL_RLEACCEL) != 0)
#define SDL_LOGPAL      0x01
#define SDL_PHYSPAL     0x02
#define SDL_DISABLE     0
#define SDL_ENABLE      1

typedef struct {
    Uint32 hw_available:1;
    Uint32 wm_available:1;
    Uint32 blit_hw:1;
    Uint32 blit_hw_CC:1;
    Uint32 blit_hw_A:1;
    Uint32 blit_sw:1;
    Uint32 blit_sw_CC:1;
    Uint32 blit_sw_A:1;
    Uint32 blit_fill:1;
    Uint32 video_mem;
    SDL_PixelFormat *vfmt;
} SDL_VideoInfo;

typedef int SDLKey;

void SDL_WM_SetCaption(const char *title, const char *icon);
Uint32 SDL_WasInit(Uint32 flags);
int SDL_InitSubSystem(Uint32 flags);
int SDL_FillRect(SDL_Surface *dst, SDL_Rect *dstrect, Uint32 color);
SDL_Surface *SDL_CreateRGBSurface(Uint32 flags, int width, int height,
                                  int depth, Uint32 Rmask, Uint32 Gmask,
                                  Uint32 Bmask, Uint32 Amask);
int SDL_SaveBMP(SDL_Surface *surface, const char *file);
SDL_Surface *SDL_LoadBMP_RW(SDL_RWops *src, int freesrc);
char *SDL_GetKeyName(SDLKey key);
Uint32 SDL_GetTicks(void);
SDL_Surface *SDL_GetVideoSurface(void);
Uint32 SDL_MapRGB(SDL_PixelFormat *fmt, Uint8 r, Uint8 g, Uint8 b);
int SDL_SetColors(SDL_Surface *surface, SDL_Color *colors,
                  int firstcolor, int ncolors);
int SDL_SetPalette(SDL_Surface *surface, int flags, SDL_Color *colors,
                   int firstcolor, int ncolors);
SDL_Surface *SDL_SetVideoMode(int width, int height, int bpp, Uint32 flags);
void SDL_FreeSurface(SDL_Surface *surface);
void SDL_QuitSubSystem(Uint32 flags);
int SDL_Flip(SDL_Surface *screen);
int SDL_VideoModeOK(int width, int height, int bpp, Uint32 flags);
int SDL_LockSurface(SDL_Surface *surface);
void SDL_UnlockSurface(SDL_Surface *surface);
void SDL_UpdateRect(SDL_Surface *screen, Sint32 x, Sint32 y,
                    Sint32 w, Sint32 h);
SDL_Rect **SDL_ListModes(SDL_PixelFormat *format, Uint32 flags);
SDL_VideoInfo *SDL_GetVideoInfo(void);
char *SDL_VideoDriverName(char *namebuf, int maxlen);

#define SDL_BlitSurface SDL_UpperBlit
int SDL_UpperBlit(SDL_Surface *src, SDL_Rect *srcrect,
                  SDL_Surface *dst, SDL_Rect *dstrect);

void SDL_LockDisplay(void);
void SDL_UnlockDisplay(void);
void spi_lcd_init(void);
void spi_lcd_clear(void);

#endif
