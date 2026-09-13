#include "psram_app.h"
#include "SDL.h"

extern const app_services_t *_papp_svc;
extern void papp_sdl_video_shutdown(void);

int SDL_Init(Uint32 flags)
{
    if (flags & SDL_INIT_VIDEO) {
        /* Video is actually allocated by VL_SetVGAPlaneMode later. */
        SDL_InitSubSystem(SDL_INIT_VIDEO);
    }
    return 0;
}

void SDL_Quit(void) {}

void SDL_Delay(Uint32 ms)
{
    if (_papp_svc && _papp_svc->delay_ms) _papp_svc->delay_ms((int)ms);
}

Uint32 SDL_GetTicks(void)
{
    return _papp_svc ? (Uint32)(_papp_svc->get_time_us() / 1000) : 0;
}

const SDL_version *SDL_Linked_Version(void)
{
    static SDL_version version = {SDL_MAJOR_VERSION, SDL_MINOR_VERSION,
                                  SDL_PATCHLEVEL};
    return &version;
}

void SDL_InitSD(void)
{
    if (_papp_svc) _papp_svc->log_printf("Wolf4SDL PAPP: SD already mounted\n");
}

void Check(const char *str) { (void)str; }

SDL_mutex *SDL_CreateMutex(void) { return NULL; }
void SDL_DestroyMutex(SDL_mutex *mutex) { (void)mutex; }
int SDL_LockMutex(SDL_mutex *mutex) { (void)mutex; return 0; }
int SDL_UnlockMutex(SDL_mutex *mutex) { (void)mutex; return 0; }

char ***allocateTwoDimenArrayOnHeapUsingMalloc(int row, int col)
{
    if (row <= 0 || col <= 0) return NULL;
    size_t pointer_bytes = (size_t)row * sizeof(char **);
    size_t data_bytes = (size_t)row * (size_t)col * sizeof(char *);
    char ***out = (char ***)malloc(pointer_bytes + data_bytes);
    if (!out) return NULL;
    char **data = (char **)((uint8_t *)out + pointer_bytes);
    for (int i = 0; i < row; ++i) out[i] = data + (size_t)i * col;
    return out;
}

void papp_sdl_shutdown(void)
{
    papp_sdl_video_shutdown();
}
