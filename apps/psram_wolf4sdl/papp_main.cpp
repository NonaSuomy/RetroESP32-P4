/* Wolf4SDL Wolfenstein 3D PAPP entry point. */
#define PAPP_APP_SIDE 1

#include "psram_app.h"
#include <setjmp.h>
#include <stddef.h>

extern "C" {
const app_services_t *_papp_svc = NULL;
}
extern int main(int argc, char **argv);
#ifdef SPEAR
extern "C" int papp_spear_select_mission(const app_services_t *svc);
#endif
extern "C" void papp_sdl_shutdown(void);
extern "C" void papp_sdl_audio_shutdown(void);
extern "C" void papp_cleanup_fds(void);
extern "C" void papp_cleanup_heap(void);
extern "C" void papp_close_streams(void);

static jmp_buf s_exit_env;
static volatile int s_exit_requested;
static volatile int s_reselect_requested;
static volatile int s_game_active;

extern "C" void app_return_to_launcher(void)
{
#ifdef SPEAR
    /* A normal Wolf exit (including SDL_QUIT from the remote close control)
     * should return to the mission picker while the PAPP is active. The
     * picker itself calls this again with s_game_active cleared to leave the
     * PAPP and return to the launcher. */
    if (s_game_active) {
        if (_papp_svc && _papp_svc->log_printf)
            _papp_svc->log_printf("Wolf4SDL exit while mission active: returning to selector\n");
        s_game_active = 0;
        s_reselect_requested = 1;
        longjmp(s_exit_env, 1);
    }
#endif
    s_exit_requested = 1;
    longjmp(s_exit_env, 1);
}

extern "C" void app_reselect_mission(void)
{
    if (_papp_svc && _papp_svc->log_printf)
        _papp_svc->log_printf("Wolf4SDL mission selector requested by game menu\n");
    s_game_active = 0;
    s_reselect_requested = 1;
    longjmp(s_exit_env, 1);
}

static void *operator_new(size_t size)
{
    return _papp_svc ? _papp_svc->mem_alloc(size) : NULL;
}

/* Wolf4SDL is compiled as C++ but does not use exceptions or RTTI. These
 * operators keep the freestanding link independent of libstdc++ startup. */
void *operator new(size_t size) noexcept { return operator_new(size); }
void *operator new[](size_t size) noexcept { return operator_new(size); }
void operator delete(void *p) noexcept { if (p) _papp_svc->mem_free(p); }
void operator delete[](void *p) noexcept { if (p) _papp_svc->mem_free(p); }
void operator delete(void *p, size_t) noexcept { if (p) _papp_svc->mem_free(p); }
void operator delete[](void *p, size_t) noexcept { if (p) _papp_svc->mem_free(p); }

extern "C" __attribute__((section(".text.entry"), used))
int app_entry(const app_services_t *svc)
{
    if (!svc || svc->abi_version != PAPP_ABI_VERSION) return -1;
    _papp_svc = svc;
    svc->log_printf("=== Wolf4SDL PAPP starting ===\n");

    for (;;) {
        s_exit_requested = 0;
        s_reselect_requested = 0;
        s_game_active = 0;

        if (setjmp(s_exit_env) == 0) {
#ifdef SPEAR
            if (papp_spear_select_mission(svc) < 0)
                app_return_to_launcher();
            s_game_active = 1;
#endif
            char *argv[] = {(char *)"wolf3d", NULL};
            main(1, argv);
        }

        papp_sdl_audio_shutdown();
        papp_sdl_shutdown();
        papp_close_streams();
        papp_cleanup_fds();
        papp_cleanup_heap();
        svc->display_clear(0);
        svc->display_flush();

        if (s_reselect_requested) {
            svc->log_printf("=== Returning to Spear mission selector ===\n");
            if (svc->delay_ms) svc->delay_ms(50);
            continue;
        }
        break;
    }

    svc->log_printf("=== Wolf4SDL PAPP exited (%d) ===\n", s_exit_requested);
    return 0;
}
