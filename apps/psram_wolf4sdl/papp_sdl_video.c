/* Wolf4SDL video backend for the PSRAM app ABI.
 *
 * Wolf4SDL renders an 8-bit 320x200 indexed framebuffer. The launcher owns
 * the real LCD, so this shim keeps the indexed surfaces in PSRAM, maintains
 * the game's palette, converts each completed frame to RGB565, and submits
 * it through display_write_frame_custom().
 */
#include "psram_app.h"
#include "SDL.h"
#include <string.h>

extern const app_services_t *_papp_svc;
extern void papp_sdl_pump_audio(void);

static SDL_Surface *s_primary;
static SDL_PixelFormat s_primary_format;
static SDL_Palette s_palette;
static SDL_Color s_palette_colors[256];
/* The palette changes rarely, while SDL_Flip converts every pixel. Cache the
 * exact RGB565 value once per palette update so the hot path is one indexed
 * load instead of three shifts/masks and three palette-field loads. */
static uint16_t s_palette_rgb565[256];
static uint16_t *s_rgb565;
static unsigned s_flip_count;

SDL_Surface *primary_surface;

static void *papp_alloc(size_t size)
{
    void *p = _papp_svc->mem_caps_alloc(size, PAPP_MEM_CAP_SPIRAM);
    return p ? p : _papp_svc->mem_alloc(size);
}

static void init_palette(void)
{
    memset(&s_palette, 0, sizeof(s_palette));
    memset(s_palette_colors, 0, sizeof(s_palette_colors));
    memset(s_palette_rgb565, 0, sizeof(s_palette_rgb565));
    s_palette.ncolors = 256;
    s_palette.colors = s_palette_colors;
}

static void init_format(SDL_PixelFormat *format, int depth)
{
    memset(format, 0, sizeof(*format));
    format->palette = &s_palette;
    format->BitsPerPixel = (Uint8)depth;
    format->BytesPerPixel = (Uint8)(depth / 8);
}

SDL_Surface *SDL_CreateRGBSurface(Uint32 flags, int width, int height,
                                  int depth, Uint32 Rmask, Uint32 Gmask,
                                  Uint32 Bmask, Uint32 Amask)
{
    (void)Rmask; (void)Gmask; (void)Bmask; (void)Amask;
    if (width <= 0 || height <= 0 || depth != 8)
        return NULL;

    SDL_Surface *surface = (SDL_Surface *)_papp_svc->mem_calloc(1, sizeof(*surface));
    if (!surface) return NULL;
    surface->pixels = papp_alloc((size_t)width * (size_t)height);
    if (!surface->pixels) {
        _papp_svc->mem_free(surface);
        return NULL;
    }

    SDL_PixelFormat *format = (SDL_PixelFormat *)_papp_svc->mem_calloc(1, sizeof(*format));
    if (!format) {
        _papp_svc->mem_free(surface->pixels);
        _papp_svc->mem_free(surface);
        return NULL;
    }
    init_format(format, depth);
    surface->flags = flags;
    surface->format = format;
    surface->w = width;
    surface->h = height;
    surface->pitch = (Uint16)width;
    surface->clip_rect.x = 0;
    surface->clip_rect.y = 0;
    surface->clip_rect.w = (Uint16)width;
    surface->clip_rect.h = (Uint16)height;
    surface->refcount = 1;
    memset(surface->pixels, 0, (size_t)width * (size_t)height);
    return surface;
}

SDL_Surface *SDL_SetVideoMode(int width, int height, int bpp, Uint32 flags)
{
    if (bpp != 8 || width != 320 || height != 200)
        return NULL;
    if (s_primary) return s_primary;

    init_palette();
    init_format(&s_primary_format, 8);
    s_primary = (SDL_Surface *)_papp_svc->mem_calloc(1, sizeof(*s_primary));
    if (!s_primary) return NULL;
    s_primary->pixels = papp_alloc(320u * 200u);
    if (!s_primary->pixels) {
        _papp_svc->mem_free(s_primary);
        s_primary = NULL;
        return NULL;
    }
    s_primary->flags = flags;
    s_primary->format = &s_primary_format;
    s_primary->w = width;
    s_primary->h = height;
    s_primary->pitch = width;
    s_primary->clip_rect.x = 0;
    s_primary->clip_rect.y = 0;
    s_primary->clip_rect.w = width;
    s_primary->clip_rect.h = height;
    s_primary->refcount = 1;
    memset(s_primary->pixels, 0, 320u * 200u);

    s_rgb565 = (uint16_t *)papp_alloc(320u * 200u * sizeof(uint16_t));
    if (!s_rgb565) {
        _papp_svc->mem_free(s_primary->pixels);
        _papp_svc->mem_free(s_primary);
        s_primary = NULL;
        return NULL;
    }
    primary_surface = s_primary;
    _papp_svc->log_printf("Wolf4SDL PAPP: video %dx%d indexed\n", width, height);
    return s_primary;
}

SDL_Surface *SDL_GetVideoSurface(void) { return s_primary; }

int SDL_SaveBMP(SDL_Surface *surface, const char *file)
{
    (void)surface; (void)file;
    return 0;
}

SDL_Surface *SDL_LoadBMP_RW(SDL_RWops *src, int freesrc)
{
    (void)src; (void)freesrc;
    return NULL;
}

void SDL_FreeSurface(SDL_Surface *surface)
{
    if (!surface) return;
    if (surface == s_primary) {
        s_primary = NULL;
        primary_surface = NULL;
    }
    if (surface->pixels) _papp_svc->mem_free(surface->pixels);
    if (surface->format && surface->format != &s_primary_format)
        _papp_svc->mem_free(surface->format);
    _papp_svc->mem_free(surface);
}

int SDL_LockSurface(SDL_Surface *surface) { (void)surface; return 0; }
void SDL_UnlockSurface(SDL_Surface *surface) { (void)surface; }

int SDL_SetColors(SDL_Surface *surface, SDL_Color *colors,
                  int firstcolor, int ncolors)
{
    (void)surface;
    if (!colors || firstcolor < 0 || ncolors < 0) return 0;
    if (firstcolor >= 256) return 0;
    if (firstcolor + ncolors > 256) ncolors = 256 - firstcolor;
    for (int i = 0; i < ncolors; ++i) {
        int index = firstcolor + i;
        s_palette_colors[index] = colors[i];
        s_palette_rgb565[index] =
            (uint16_t)(((uint16_t)(colors[i].r & 0xf8) << 8) |
                       ((uint16_t)(colors[i].g & 0xfc) << 3) |
                       (colors[i].b >> 3));
    }
    return 1;
}

int SDL_SetPalette(SDL_Surface *surface, int flags, SDL_Color *colors,
                   int firstcolor, int ncolors)
{
    (void)flags;
    return SDL_SetColors(surface, colors, firstcolor, ncolors);
}

Uint32 SDL_MapRGB(SDL_PixelFormat *format, Uint8 r, Uint8 g, Uint8 b)
{
    if (format && format->BitsPerPixel == 16)
        return ((Uint32)(r >> 3) << 11) | ((Uint32)(g >> 2) << 5) | (b >> 3);
    for (int i = 0; i < 256; ++i) {
        if (s_palette_colors[i].r == r && s_palette_colors[i].g == g &&
            s_palette_colors[i].b == b) return (Uint32)i;
    }
    return 0;
}

/* The original SDL blitter is larger than the engine needs. All Wolf4SDL
 * surfaces are indexed 8-bit, so this clipped row copy is both sufficient
 * and faster than rebuilding SDL's blit map for every temporary surface. */
int SDL_UpperBlit(SDL_Surface *src, SDL_Rect *srcrect,
                  SDL_Surface *dst, SDL_Rect *dstrect)
{
    if (!src || !dst || !src->pixels || !dst->pixels ||
        src->format->BytesPerPixel != 1 || dst->format->BytesPerPixel != 1)
        return -1;

    SDL_Rect sr = srcrect ? *srcrect : src->clip_rect;
    SDL_Rect dr = dstrect ? *dstrect : dst->clip_rect;
    if (sr.w == 0 || sr.h == 0) return 0;
    if (sr.x < 0) { dr.x -= sr.x; sr.w += sr.x; sr.x = 0; }
    if (sr.y < 0) { dr.y -= sr.y; sr.h += sr.y; sr.y = 0; }
    if (dr.x < 0) { sr.x -= dr.x; sr.w += dr.x; dr.x = 0; }
    if (dr.y < 0) { sr.y -= dr.y; sr.h += dr.y; dr.y = 0; }
    if (sr.x + sr.w > src->w) sr.w = (Sint16)(src->w - sr.x);
    if (sr.y + sr.h > src->h) sr.h = (Uint16)(src->h - sr.y);
    if (dr.x + sr.w > dst->w) sr.w = (Sint16)(dst->w - dr.x);
    if (dr.y + sr.h > dst->h) sr.h = (Uint16)(dst->h - dr.y);
    if (sr.w <= 0 || sr.h <= 0) return 0;

    for (int y = 0; y < sr.h; ++y) {
        const uint8_t *s = (const uint8_t *)src->pixels +
                           (sr.y + y) * src->pitch + sr.x;
        uint8_t *d = (uint8_t *)dst->pixels +
                     (dr.y + y) * dst->pitch + dr.x;
        memmove(d, s, (size_t)sr.w);
    }
    if (dstrect) {
        dstrect->w = sr.w;
        dstrect->h = sr.h;
    }
    return 0;
}

int SDL_FillRect(SDL_Surface *dst, SDL_Rect *dstrect, Uint32 color)
{
    if (!dst || !dst->pixels) return -1;
    SDL_Rect r = dstrect ? *dstrect : dst->clip_rect;
    if (r.x < 0) { r.w += r.x; r.x = 0; }
    if (r.y < 0) { r.h += r.y; r.y = 0; }
    if (r.x + r.w > dst->w) r.w = (Uint16)(dst->w - r.x);
    if (r.y + r.h > dst->h) r.h = (Uint16)(dst->h - r.y);
    for (int y = 0; y < r.h; ++y)
        memset((uint8_t *)dst->pixels + (r.y + y) * dst->pitch + r.x,
               (uint8_t)color, r.w);
    return 0;
}

int SDL_Flip(SDL_Surface *screen)
{
    if (!screen || !screen->pixels || !s_rgb565) return -1;
    const uint8_t *src = (const uint8_t *)screen->pixels;
    for (int y = 0; y < 200; ++y) {
        uint16_t *out = s_rgb565 + y * 320;
        const uint8_t *in = src + y * screen->pitch;
        for (int x = 0; x < 320; ++x) {
            out[x] = s_palette_rgb565[in[x]];
        }
    }
    _papp_svc->display_write_frame_custom(s_rgb565, 320, 200, 2.4f, false);
    papp_sdl_pump_audio();
    ++s_flip_count;
    if ((s_flip_count % 300u) == 0u)
        _papp_svc->log_printf("Wolf4SDL PAPP: frames=%u\n", s_flip_count);
    return 0;
}

void SDL_UpdateRect(SDL_Surface *screen, Sint32 x, Sint32 y, Sint32 w, Sint32 h)
{
    (void)x; (void)y; (void)w; (void)h;
    SDL_Flip(screen);
}

void SDL_WM_SetCaption(const char *title, const char *icon)
{
    (void)icon;
    if (_papp_svc && title) _papp_svc->log_printf("Wolf4SDL title: %s\n", title);
}

char *SDL_GetKeyName(SDLKey key) { (void)key; return (char *)"PAPP"; }
int SDL_VideoModeOK(int width, int height, int bpp, Uint32 flags)
{ (void)flags; return (width == 320 && height == 200 && bpp == 8) ? 1 : 0; }

SDL_VideoInfo *SDL_GetVideoInfo(void)
{
    static SDL_VideoInfo info;
    static SDL_PixelFormat format;
    memset(&info, 0, sizeof(info));
    init_format(&format, 8);
    info.vfmt = &format;
    info.blit_sw = 1;
    return &info;
}

char *SDL_VideoDriverName(char *namebuf, int maxlen)
{
    static const char name[] = "PAPP-P4";
    if (namebuf && maxlen > 0) {
        strncpy(namebuf, name, (size_t)maxlen - 1u);
        namebuf[maxlen - 1] = '\0';
        return namebuf;
    }
    return (char *)name;
}

SDL_Rect **SDL_ListModes(SDL_PixelFormat *format, Uint32 flags)
{
    (void)format; (void)flags;
    static SDL_Rect mode = {0, 0, 320, 200};
    static SDL_Rect *modes[] = {&mode, NULL};
    return modes;
}

Uint32 SDL_WasInit(Uint32 flags) { return flags; }
int SDL_InitSubSystem(Uint32 flags) { (void)flags; return 0; }
void SDL_QuitSubSystem(Uint32 flags) { (void)flags; }

void SDL_LockDisplay(void) {}
void SDL_UnlockDisplay(void) {}
void spi_lcd_init(void) {}
void spi_lcd_clear(void) { _papp_svc->display_clear(0); }

void papp_sdl_video_shutdown(void)
{
    if (s_primary) SDL_FreeSurface(s_primary);
    if (s_rgb565) {
        _papp_svc->mem_free(s_rgb565);
        s_rgb565 = NULL;
    }
    s_flip_count = 0;
}
