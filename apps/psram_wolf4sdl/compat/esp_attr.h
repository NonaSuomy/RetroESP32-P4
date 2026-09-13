#ifndef ESP_ATTR_H
#define ESP_ATTR_H

/* The launcher already places the PAPP image in executable PSRAM. The IDF
 * section attributes used by the original ESP32 port are unnecessary here. */
#define IRAM_ATTR
#define DRAM_ATTR
#define EXT_RAM_ATTR

#endif
