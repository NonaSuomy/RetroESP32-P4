/* NES/nofrendo PSRAM application entry point. */
#define PAPP_APP_SIDE 1
#include "runtime.h"
#include "psram_app.h"
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const app_services_t *_papp_svc;
static jmp_buf s_exit;
volatile int papp_nes_exit;

extern int nofrendo_main(int argc, char **argv);
extern volatile int nofrendo_quit_flag;
extern char *papp_nes_rom_data;
extern int papp_nes_load_rom(const char *path);
extern void papp_nes_cleanup(void);
extern volatile int papp_exit_requested;

void papp_nes_request_exit(void)
{
    papp_nes_exit = 1;
    papp_exit_requested = 1;
    nofrendo_quit_flag = 1;
    longjmp(s_exit, 1);
}

static void nes_watchdog(void *arg)
{
    (void)arg;
    papp_gamepad_state_t pad;
    int held = 0;
    for (;;) {
        _papp_svc->input_gamepad_read(&pad);
        if (pad.values[PAPP_INPUT_MENU] ||
            (_papp_svc->input_l3_read && _papp_svc->input_l3_read())) {
            if (++held >= 3) {
                papp_nes_exit = 1;
                papp_exit_requested = 1;
                /* The launcher deletes this task after nofrendo_main()
                   unwinds.  Do not return from a PAPP-created task: the
                   shared PAPP ABI keeps task ownership with the app until
                   task_delete() is called explicitly. */
                for (;;) {
                    _papp_svc->delay_ms(100);
                }
            }
        } else {
            held = 0;
        }
        _papp_svc->delay_ms(100);
    }
}

__attribute__((section(".text.entry")))
int app_entry(const app_services_t *svc)
{
    _papp_svc = svc;
    papp_nes_exit = 0;
    if (!svc || svc->abi_version != PAPP_ABI_VERSION)
        return -1;

    char selected_path_buf[512];
    const char *selected_path = NULL;
    /* The launcher stores the selected PAPP path in NVS.  Read the matching
       /sd/roms/papp/nes.rom sidecar first so the PAPP launch menu can choose
       any NES image without confusing the .papp path for a ROM. */
    if (emu_rom_path("nes", ".nes|", selected_path_buf,
                     sizeof(selected_path_buf)) == 0) {
        selected_path = selected_path_buf;
    } else {
        selected_path = "/sd/roms/nes/River City Ransom (USA).nes";
        svc->log_printf("NES PAPP: no ROM selected; using default %s\n", selected_path);
    }
    /* The loader owns this static settings buffer. Copy it before using it
       and only free the app-owned copy on exit. */
    size_t path_len = strlen(selected_path) + 1;
    char *rom_path = svc->mem_alloc(path_len);
    if (!rom_path) return -1;
    memcpy(rom_path, selected_path, path_len);
    svc->log_printf("=== NES PAPP: %s ===\n", rom_path);
    svc->audio_init(32000);

    void *watchdog = NULL;
    svc->task_create(nes_watchdog, "nes_wd", 4096, NULL, 2, &watchdog, 1);

    int result = -1;
    if (papp_nes_load_rom(rom_path) == 0 && setjmp(s_exit) == 0) {
        char *argv[] = { rom_path, NULL };
        result = nofrendo_main(1, argv);
    }

    if (watchdog) svc->task_delete(watchdog);
    papp_nes_cleanup();
    svc->mem_free(rom_path);
    svc->display_clear(0);
    svc->display_flush();
    svc->log_printf("=== NES PAPP exited (%d) ===\n", result);
    return result;
}
