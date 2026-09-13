#pragma once
#include <stdint.h>
#include "psram_app.h"
extern const app_services_t *_papp_svc;
static inline int64_t esp_timer_get_time(void) { return _papp_svc->get_time_us(); }
