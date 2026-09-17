#define PAPP_APP_SIDE 1
#include "psram_app.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "odroid_input.h"

extern const app_services_t *_papp_svc;
extern void papp_nes_request_exit(void);
char *papp_nes_rom_data;
volatile int nofrendo_quit_flag;
int forceConsoleReset = 1;
volatile int papp_exit_requested;
odroid_battery_state battery;
void *osd_getromdata(void) { return papp_nes_rom_data; }

int papp_nes_load_rom(const char *path)
{
    void *f = _papp_svc->file_open(path, "rb");
    if (!f) return -1;
    _papp_svc->file_seek(f, 0, 2);
    long n = _papp_svc->file_tell(f);
    _papp_svc->file_seek(f, 0, 0);
    if (n <= 0 || n > 8 * 1024 * 1024) { _papp_svc->file_close(f); return -1; }
    papp_nes_rom_data = _papp_svc->mem_caps_alloc((size_t)n, PAPP_MEM_CAP_SPIRAM);
    if (!papp_nes_rom_data || _papp_svc->file_read(papp_nes_rom_data, 1, (size_t)n, f) != (size_t)n) {
        if (papp_nes_rom_data) _papp_svc->mem_free(papp_nes_rom_data);
        papp_nes_rom_data = NULL; _papp_svc->file_close(f); return -1;
    }
    _papp_svc->file_close(f);
    return 0;
}
void papp_nes_cleanup(void) { if (papp_nes_rom_data) _papp_svc->mem_free(papp_nes_rom_data); papp_nes_rom_data = NULL; }
void app_return_to_launcher(void) { papp_exit_requested = 1; papp_nes_request_exit(); }

/* Save states are intentionally disabled in the first PAPP port. */
void save_sram(void) {}
void load_sram(void) {}
void state_setslot(int slot) { (void)slot; }
