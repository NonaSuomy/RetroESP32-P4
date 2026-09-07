/*
 * Serial Upload — receive files over USB Serial JTAG and write to SD card.
 *
 * Protocol (all text-based header, then raw binary data):
 *   PC → ESP:  "PAPU"               (4-byte magic)
 *   PC → ESP:  "<path>\n"           (destination path, e.g. /sd/roms/papp/foo.papp)
 *   PC → ESP:  "<size>\n"           (decimal file size in bytes)
 *   ESP → PC:  "\x06READY\n"       (ready to receive, \x06 = ACK prefix)
 *   PC → ESP:  <raw binary data>   (exactly <size> bytes)
 *   ESP → PC:  "\x06OK <size>\n"   (success) or "\x06ERR:<msg>\n" (failure)
 *
 * The \x06 prefix lets the PC-side script distinguish protocol responses
 * from interleaved ESP_LOG output.
 */

#include "serial_upload.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "driver/usb_serial_jtag.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "odroid_settings.h"

static const char *TAG = "serial_upload";
static TaskHandle_t s_upload_task = NULL;
/* The Elecrow board exposes UART0 through the connected CH340 bridge as
 * /dev/ttyUSB0.  Keep the USB Serial JTAG backend available, but prefer
 * UART0 so the PAPU protocol works on the port physically connected here. */
static bool s_uart_ready = false;

#define MAGIC       "PAPU"
#define MAGIC_LEN   4
#define LAUNCH_MAGIC "RUNR"
#define CHUNK_SIZE  4096
#define MAX_PATH    256
#define MAX_FILE_SIZE (68 * 1024 * 1024)  /* 68 MB — large enough for Neo Geo sprite caches (up to 64 MB) */

/* ── Helpers ─────────────────────────────────────────────────────────── */

static int transport_read(uint8_t *buf, size_t len, TickType_t timeout)
{
    if (s_uart_ready) {
        return uart_read_bytes(UART_NUM_0, buf, len, timeout);
    }
    return usb_serial_jtag_read_bytes(buf, len, timeout);
}

static int transport_write(const uint8_t *buf, size_t len, TickType_t timeout)
{
    if (s_uart_ready) {
        int n = uart_write_bytes(UART_NUM_0, (const char *)buf, len);
        uart_wait_tx_done(UART_NUM_0, timeout);
        return n;
    }
    return usb_serial_jtag_write_bytes(buf, len, timeout);
}

/* Read exactly `len` bytes, return 0 on success, -1 on timeout. */
static int read_exact(uint8_t *buf, size_t len, int timeout_ms)
{
    size_t got = 0;
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (got < len) {
        TickType_t now = xTaskGetTickCount();
        if ((int32_t)(now - deadline) >= 0) return -1;
        TickType_t remaining = deadline - now;
        int n = transport_read(buf + got, len - got, remaining);
        if (n > 0) got += n;
    }
    return 0;
}

/* Read until '\n', null-terminate, return length (excl. NUL), -1 on timeout. */
static int read_line(char *buf, size_t max, int timeout_ms)
{
    size_t pos = 0;
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (pos < max - 1) {
        TickType_t now = xTaskGetTickCount();
        if ((int32_t)(now - deadline) >= 0) return -1;
        TickType_t remaining = deadline - now;
        uint8_t c;
        int n = transport_read(&c, 1, remaining);
        if (n <= 0) continue;
        if (c == '\n') { buf[pos] = '\0'; return (int)pos; }
        if (c != '\r') buf[pos++] = (char)c;
    }
    buf[pos] = '\0';
    return (int)pos;
}

/* Send a protocol response (prefixed with \x06 so Python can find it). */
static void send_response(const char *msg)
{
    const char prefix = '\x06';
    transport_write((const uint8_t *)&prefix, 1, pdMS_TO_TICKS(100));
    transport_write((const uint8_t *)msg, strlen(msg), pdMS_TO_TICKS(1000));
}

/* Recursively create directories for a given file path (like mkdir -p). */
static void ensure_parent_dirs(const char *filepath)
{
    char tmp[MAX_PATH];
    strncpy(tmp, filepath, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
}

/* ── Upload handler ──────────────────────────────────────────────────── */

static void handle_upload(void)
{
    char path[MAX_PATH];
    char size_str[32];

    ESP_LOGI(TAG, "Upload magic received");

    /* Read destination path */
    if (read_line(path, sizeof(path), 5000) <= 0) {
        send_response("ERR:timeout reading path\n");
        return;
    }

    /* Read file size */
    if (read_line(size_str, sizeof(size_str), 5000) <= 0) {
        send_response("ERR:timeout reading size\n");
        return;
    }

    uint32_t file_size = (uint32_t)strtoul(size_str, NULL, 10);
    if (file_size == 0 || file_size > MAX_FILE_SIZE) {
        send_response("ERR:invalid size\n");
        return;
    }

    ESP_LOGI(TAG, "Upload: '%s' (%lu bytes)", path, (unsigned long)file_size);

    /* Ensure parent directories exist */
    ensure_parent_dirs(path);

    /* Open file for writing */
    FILE *f = fopen(path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "Cannot open '%s' for writing", path);
        send_response("ERR:cannot open file\n");
        return;
    }

    send_response("READY\n");

    /* Receive file data in chunks with flow control.
       After writing each chunk to SD, we send back '\x06' so the PC
       knows it's safe to send the next chunk.  Without this, USB
       Serial JTAG's 16 KB ring buffer overflows at high speed. */
    uint8_t *chunk = malloc(CHUNK_SIZE);
    if (!chunk) {
        fclose(f);
        remove(path);
        send_response("ERR:no memory\n");
        return;
    }

    uint32_t remaining = file_size;
    uint32_t written = 0;
    const char ack_byte = '\x06';

    while (remaining > 0) {
        size_t to_read = (remaining > CHUNK_SIZE) ? CHUNK_SIZE : remaining;
        if (read_exact(chunk, to_read, 30000) < 0) {
            ESP_LOGE(TAG, "Timeout at byte %lu/%lu",
                     (unsigned long)written, (unsigned long)file_size);
            free(chunk);
            fclose(f);
            remove(path);
            send_response("ERR:timeout during transfer\n");
            return;
        }
        size_t w = fwrite(chunk, 1, to_read, f);
        if (w != to_read) {
            ESP_LOGE(TAG, "Write error at byte %lu", (unsigned long)written);
            free(chunk);
            fclose(f);
            remove(path);
            send_response("ERR:write failed\n");
            return;
        }
        remaining -= to_read;
        written += to_read;

        /* Chunk ACK — tell PC to send next chunk */
        transport_write((const uint8_t *)&ack_byte, 1, pdMS_TO_TICKS(100));
    }

    free(chunk);
    fclose(f);

    char resp[64];
    snprintf(resp, sizeof(resp), "OK %lu\n", (unsigned long)written);
    send_response(resp);

    ESP_LOGI(TAG, "Upload complete: %lu bytes → %s",
             (unsigned long)written, path);
}

/* ── Launch handler ───────────────────────────────────────────────────
 *
 * Protocol:
 *   PC → ESP:  "RUNR"               (4-byte magic)
 *   PC → ESP:  "<rom_path>\n"       (full SD path, e.g. /sd/roms/neogeo/mslug3.zip)
 *   ESP → PC:  "\x06LAUNCH:<path>\n" (confirm) then reboots into emulator
 *   ESP → PC:  "\x06ERR:<msg>\n"     (on failure)
 */

/* Map file extension to OTA slot — mirrors get_ota_slot() in launcher. */
static int ext_to_ota_slot(const char *ext)
{
    struct { const char *e; int slot; } map[] = {
        {"nes",0}, {"gb",1}, {"gbc",1}, {"sms",2}, {"gg",2}, {"col",2},
        {"z80",3}, {"sna",3}, {"a26",4}, {"bin",4}, {"a78",5}, {"lnx",6},
        {"pce",7}, {"xex",8}, {"atr",8}, {"a52",8}, {"smc",10}, {"sfc",10},
        {"md",11}, {"gen",11}, {"zip",12}, {NULL,0}
    };
    for (int i = 0; map[i].e; i++) {
        if (strcasecmp(ext, map[i].e) == 0) return map[i].slot;
    }
    return -1;
}

static void handle_launch(void)
{
    char rom_path[MAX_PATH];

    ESP_LOGI(TAG, "Launch magic received");

    if (read_line(rom_path, sizeof(rom_path), 5000) <= 0) {
        send_response("ERR:timeout reading rom path\n");
        return;
    }

    /* Extract extension */
    const char *dot = strrchr(rom_path, '.');
    if (!dot || dot == rom_path) {
        send_response("ERR:no file extension\n");
        return;
    }
    const char *ext = dot + 1;

    /* PAPPs run from the factory launcher rather than an OTA emulator slot.
       Queue the path in NVS and let the launcher execute it after reboot. */
    if (strcasecmp(ext, "papp") == 0) {
        odroid_settings_RomFilePath_set(rom_path);
        odroid_settings_StartAction_set(ODROID_START_ACTION_PAPP);
        char resp[MAX_PATH + 32];
        snprintf(resp, sizeof(resp), "LAUNCH:%s\n", rom_path);
        send_response(resp);
        ESP_LOGI(TAG, "Launching PAPP: '%s'", rom_path);
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_restart();
        return;
    }

    int ota_slot = ext_to_ota_slot(ext);
    if (ota_slot < 0) {
        char resp[64];
        snprintf(resp, sizeof(resp), "ERR:unknown ext '%s'\n", ext);
        send_response(resp);
        return;
    }

    const esp_partition_t *emu_part = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP,
        ESP_PARTITION_SUBTYPE_APP_OTA_MIN + ota_slot,
        NULL);
    if (!emu_part) {
        char resp[64];
        snprintf(resp, sizeof(resp), "ERR:no partition ota_%d\n", ota_slot);
        send_response(resp);
        return;
    }

    /* Write ROM path to NVS (same keys the launcher uses) */
    odroid_settings_RomFilePath_set(rom_path);
    odroid_settings_DataSlot_set(0);

    char resp[MAX_PATH + 32];
    snprintf(resp, sizeof(resp), "LAUNCH:%s\n", rom_path);
    send_response(resp);

    ESP_LOGI(TAG, "Launching: '%s' → %s (ota_%d)",
             rom_path, emu_part->label, ota_slot);

    /* Small delay to let the response flush over USB */
    vTaskDelay(pdMS_TO_TICKS(100));

    esp_ota_set_boot_partition(emu_part);
    esp_restart();
    /* Does not return */
}

/* ── Background task ─────────────────────────────────────────────────── */

static void serial_upload_task(void *arg)
{
    ESP_LOGI(TAG, "Listening on UART0 + USB Serial JTAG (upload=PAPU, launch=RUNR)...");

    const uint8_t upload_magic[] = MAGIC;
    const uint8_t launch_magic[] = LAUNCH_MAGIC;
    int u_idx = 0, l_idx = 0;

    for (;;) {
        uint8_t byte;
        int n = transport_read(&byte, 1, pdMS_TO_TICKS(500));
        if (n <= 0) continue;

        /* Match upload magic "PAPU" */
        if (byte == upload_magic[u_idx]) {
            u_idx++;
            if (u_idx == MAGIC_LEN) {
                handle_upload();
                u_idx = 0; l_idx = 0;
                continue;
            }
        } else {
            u_idx = (byte == upload_magic[0]) ? 1 : 0;
        }

        /* Match launch magic "RUNR" */
        if (byte == launch_magic[l_idx]) {
            l_idx++;
            if (l_idx == MAGIC_LEN) {
                handle_launch();
                u_idx = 0; l_idx = 0;
                continue;
            }
        } else {
            l_idx = (byte == launch_magic[0]) ? 1 : 0;
        }
    }
}

/* ── Public API ──────────────────────────────────────────────────────── */

void serial_upload_init(void)
{
    /* Install a buffered UART0 driver for the CH340 console bridge.  The
       ESP-IDF console uses ROM UART output, so this does not remove boot/app
       logging; it adds buffered RX/TX for the PAPU file-transfer protocol. */
    uart_config_t uart_cfg = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t uart_err = uart_param_config(UART_NUM_0, &uart_cfg);
    if (uart_err == ESP_OK) {
        uart_err = uart_driver_install(UART_NUM_0, 16384, 4096, 0, NULL, 0);
    }
    if (uart_err == ESP_OK || uart_err == ESP_ERR_INVALID_STATE) {
        s_uart_ready = true;
        ESP_LOGI(TAG, "UART0 upload transport enabled (115200 8N1)");
    } else {
        s_uart_ready = false;
        ESP_LOGW(TAG, "UART0 upload transport unavailable: %s; using USB Serial JTAG",
                 esp_err_to_name(uart_err));
    }

    /* Install the USB Serial JTAG interrupt-driven driver with generous
       RX/TX buffers so file data doesn't get dropped. */
    usb_serial_jtag_driver_config_t cfg = {
        .rx_buffer_size = 16384,
        .tx_buffer_size = 4096,
    };
    esp_err_t err = usb_serial_jtag_driver_install(&cfg);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "USB Serial JTAG driver installed (rx=%d tx=%d)",
                 (int)cfg.rx_buffer_size, (int)cfg.tx_buffer_size);
    } else {
        ESP_LOGW(TAG, "USB Serial JTAG driver install: %s (may already be active)",
                 esp_err_to_name(err));
    }

    /* Note: USB Serial JTAG secondary console is disabled in sdkconfig
     * (CONFIG_ESP_CONSOLE_SECONDARY_NONE=y) to prevent 50ms-per-char
     * stalls from ESP_LOG when the USB host isn't reading.
     * Serial upload still works via usb_serial_jtag_read/write_bytes(). */

    xTaskCreatePinnedToCore(serial_upload_task, "serial_upload",
                            8192, NULL, 3, &s_upload_task, 1);
}

void serial_upload_deinit(void)
{
    if (s_upload_task) {
        vTaskDelete(s_upload_task);
        s_upload_task = NULL;
        /* Small delay so the deleted task's stack/TCB are freed */
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    usb_serial_jtag_driver_uninstall();
}
