/*
 * USB HID Gamepad Host — ESP32-P4
 *
 * Reads wired USB gamepads via the board's USB 2.0 Type-A host port.
 * Auto-detects PS4 DualShock 4, PS5 DualSense, and generic HID gamepads.
 *
 * Three background tasks:
 *   1. usb_lib_task   — USB host library event loop (core 0)
 *   2. HID host task  — created by hid_host_install() (core 0)
 *   3. gamepad_task   — processes queued device events (core 1)
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "usb/usb_host.h"
#include "usb/hid_host.h"
#include "usb/hid_usage_keyboard.h"
#include "usb/hid_usage_mouse.h"
#include "gamepad.h"

static const char *TAG = "gamepad";

/* ========================= Report Format Detection ========================= */

typedef enum {
    GP_FORMAT_UNKNOWN = 0,
    GP_FORMAT_PS3,      /* [id=0x01][00][btn1][btn2][ps][00][LX][LY][RX][RY][pressure...]  49 bytes */
    GP_FORMAT_PS4,      /* [id=0x01][LX][LY][RX][RY][hat+btn1][btn2][btn3][L2][R2]... */
    GP_FORMAT_PS5,      /* [id=0x01][LX][LY][RX][RY][L2][R2][cnt][hat+btn1][btn2][btn3]... */
    GP_FORMAT_GENERIC,  /* [LX][LY][RX][RY][hat+btn][btn]... or [id][LX][LY]... */
	GP_FORMAT_SWITCH_PRO, /* Nintendo Switch Pro USB input report 0x30 */
	GP_FORMAT_DUAL_PSX_ADAPTOR /* [id=0x01][80][80][HatX][HatY][XABY|0][Sel,Start|LX,RX][00] 8 bytes */
} gp_report_format_t;

/* ========================= Internal State ========================= */

static gamepad_state_t s_state;
/* Kept separately because gamepad reports can arrive continuously and replace
 * the shared gamepad snapshot while a keyboard is also connected. */
static volatile uint32_t s_keyboard_keys;
static volatile bool s_keyboard_connected;
static SemaphoreHandle_t s_mutex = NULL;
static QueueHandle_t s_event_queue = NULL;
static TaskHandle_t s_usb_task = NULL;
static TaskHandle_t s_gp_task = NULL;
static volatile bool s_running = false;
static hid_host_device_handle_t s_keyboard_handle = NULL;
static hid_host_device_handle_t s_gamepad_handle = NULL;
static gp_report_format_t s_format = GP_FORMAT_UNKNOWN;
static int s_detect_count = 0;  /* number of reports used for format detection */
static volatile int s_format_log_pending = 0;  /* deferred format detection log */
static volatile int s_format_log_len = 0;      /* report len for deferred log */
static volatile int s_raw_dump_count = 0;      /* how many raw dumps have been emitted */
static uint8_t s_raw_dump_buf[64];             /* buffer for deferred raw hex dump */
static volatile int s_raw_dump_len = 0;
static volatile uint32_t s_keyboard_log_pending = 0;
static volatile uint32_t s_keyboard_log_mask = 0;
/* Official wired Switch Pro pads can enumerate successfully while ignoring
 * the first activation sequence when they were already connected at boot. */
static volatile bool s_switch_report_seen = false;

/* ========================= Device Identity ========================= */
static uint16_t s_vid = 0;
static uint16_t s_pid = 0;


/* ========================= Event Queue ========================= */

typedef enum {
    GP_EVT_DEVICE_CONNECTED,
} gp_evt_type_t;

typedef struct {
    gp_evt_type_t type;
    hid_host_device_handle_t hid_handle;
} gp_evt_t;

/* ========================= Hat Switch → D-pad ========================= */

static uint8_t hat_to_dpad(uint8_t hat)
{
    static const uint8_t map[] = {
        GAMEPAD_DPAD_UP,                                /* 0 = N  */
        GAMEPAD_DPAD_UP   | GAMEPAD_DPAD_RIGHT,         /* 1 = NE */
        GAMEPAD_DPAD_RIGHT,                             /* 2 = E  */
        GAMEPAD_DPAD_DOWN | GAMEPAD_DPAD_RIGHT,         /* 3 = SE */
        GAMEPAD_DPAD_DOWN,                              /* 4 = S  */
        GAMEPAD_DPAD_DOWN | GAMEPAD_DPAD_LEFT,          /* 5 = SW */
        GAMEPAD_DPAD_LEFT,                              /* 6 = W  */
        GAMEPAD_DPAD_UP   | GAMEPAD_DPAD_LEFT,          /* 7 = NW */
    };
    return (hat <= 7) ? map[hat] : 0;
}

static uint8_t data_to_dpad(uint8_t xaxis, uint8_t yaxis)
{
	uint8_t hat = 0;
	if (yaxis < 64) hat |= GAMEPAD_DPAD_UP;
	else if (yaxis > 192) hat |= GAMEPAD_DPAD_DOWN;
	if (xaxis < 64) hat |= GAMEPAD_DPAD_LEFT;
	else if (xaxis > 192) hat |= GAMEPAD_DPAD_RIGHT;
	return hat;
}

/* ========================= Format Detection ========================= */

/**
 * Detect report format from the first few idle reports.
 * PS4: byte[5] is hat+buttons byte (hat in lower nibble, 0-8 when idle = 0x08 centered)
 * PS5: byte[5] is L2 trigger (0 when idle), byte[8] is hat+buttons (0x08 when idle)
 */
static gp_report_format_t detect_format(const uint8_t *data, int len)
{
	// Check for specific device signatures:
	// VID   PID	 Device
	// 0x054C 0x05C4 Sony DualShock 4 (PS4)
	// 0x054C 0x0CE6 Sony DualSense (PS5)
	// 0x0079 0x0006 PC TWIN SHOCK Gamepad/Generic USB Gamepad
	// 0x0810 0x0001 Dual PSX Adaptor

	// Step 1: Use the VID/PID combo to identify known controllers.
	/* The Pro controller emits zero-filled startup packets before its first
	 * real 0x30 report. Identify it by VID/PID so those packets can never be
	 * mistaken for a generic joystick at (-128,-128). */
	if (s_vid == 0x057E && s_pid == 0x2009) {
		return GP_FORMAT_SWITCH_PRO;
	}
	if (len == 8 && s_vid == 0x0810 && s_pid == 0x0001) {
		return GP_FORMAT_DUAL_PSX_ADAPTOR;
	}

	/* Short report with ID 0x01: likely PS4/PS5 or clone (some clones use same format but generic VID/PID) */
	/* Longer report with ID 0x01 and PS4-like idle axes: likely PS4 or PS5 (some clones use same format but generic VID/PID) */


    /* Short report or no report ID → generic */
    if (len < 10 || data[0] != 0x01) {
        return GP_FORMAT_GENERIC;
    }

    /* PS3 DualShock 3: 49 bytes, id=0x01, byte[1]=0x00 (reserved),
     * axes at bytes [6..9] near 0x80, bytes [1..4] are NOT axes (buttons, usually low). */
    if (len >= 49) {
        bool ps3_axes_center = (data[6] >= 0x60 && data[6] <= 0xA0) &&
                               (data[7] >= 0x60 && data[7] <= 0xA0) &&
                               (data[8] >= 0x60 && data[8] <= 0xA0) &&
                               (data[9] >= 0x60 && data[9] <= 0xA0);
        bool ps4_axes_center = (data[1] >= 0x70 && data[1] <= 0x90) &&
                               (data[2] >= 0x70 && data[2] <= 0x90);
        if (ps3_axes_center && !ps4_axes_center && data[1] == 0x00) {
            return GP_FORMAT_PS3;
        }
    }

    /* Long report with ID 0x01: PS4 or PS5 */
    /* Check idle axes (bytes 1-4 should be near 0x80 = center) */
    bool axes_near_center = (data[1] >= 0x70 && data[1] <= 0x90) &&
                            (data[2] >= 0x70 && data[2] <= 0x90) &&
                            (data[3] >= 0x70 && data[3] <= 0x90) &&
                            (data[4] >= 0x70 && data[4] <= 0x90);

    if (axes_near_center) {
        /* Idle state pattern:
         * PS4: byte[5]=0x08 (hat centered), byte[8]=0x00 (L2 trigger)
         * PS5: byte[5]=0x00 (L2 trigger),   byte[8]=0x08 (hat centered)
         */
        if (data[5] == 0x08 && data[8] != 0x08) return GP_FORMAT_PS4;
        if (data[5] == 0x00 && data[8] == 0x08) return GP_FORMAT_PS5;
    }

    /* Non-idle heuristic: check which byte position has a valid hat (0-8) */
    uint8_t b5_hat = data[5] & 0x0F;
    uint8_t b8_hat = data[8] & 0x0F;

    if (b5_hat <= 8 && b8_hat > 8) return GP_FORMAT_PS4;
    if (b8_hat <= 8 && b5_hat > 8) return GP_FORMAT_PS5;

    /* Default: PS4 format (most common for wired USB gamepads) */
    return GP_FORMAT_PS4;
}

/* ========================= Button Parsing Helpers ========================= */

static uint32_t parse_ps_buttons(uint8_t hat_btn_byte, uint8_t btn2_byte, uint8_t btn3_byte)
{
    uint32_t btns = 0;

    /* hat_btn_byte upper nibble: face buttons */
    if (hat_btn_byte & 0x10) btns |= GAMEPAD_BTN_X;       /* Square */
    if (hat_btn_byte & 0x20) btns |= GAMEPAD_BTN_A;       /* Cross  */
    if (hat_btn_byte & 0x40) btns |= GAMEPAD_BTN_B;       /* Circle */
    if (hat_btn_byte & 0x80) btns |= GAMEPAD_BTN_Y;       /* Triangle */

    /* btn2_byte: shoulder + system */
    if (btn2_byte & 0x01) btns |= GAMEPAD_BTN_L1;
    if (btn2_byte & 0x02) btns |= GAMEPAD_BTN_R1;
    if (btn2_byte & 0x04) btns |= GAMEPAD_BTN_L2;
    if (btn2_byte & 0x08) btns |= GAMEPAD_BTN_R2;
    if (btn2_byte & 0x10) btns |= GAMEPAD_BTN_SELECT;     /* Share/Create */
    if (btn2_byte & 0x20) btns |= GAMEPAD_BTN_START;      /* Options */
    if (btn2_byte & 0x40) btns |= GAMEPAD_BTN_L3;
    if (btn2_byte & 0x80) btns |= GAMEPAD_BTN_R3;

    /* btn3_byte: PS + touchpad */
    if (btn3_byte & 0x01) btns |= GAMEPAD_BTN_HOME;
    if (btn3_byte & 0x02) btns |= GAMEPAD_BTN_MISC;       /* Touchpad click */

    return btns;
}

/* ========================= Report Parsing ========================= */

static void parse_gamepad_report(const uint8_t *data, int len)
{
    if (len < 4) return;

    /* Log first 16 bytes of raw report at DEBUG level */
    ESP_LOG_BUFFER_HEX_LEVEL(TAG, data, (len > 16) ? 16 : len, ESP_LOG_DEBUG);

    /* Always capture latest raw report for gamepad_get_raw_report() API */
    {
        int copy_len = (len > 64) ? 64 : len;
        memcpy(s_raw_dump_buf, data, copy_len);
        s_raw_dump_len = copy_len;
    }

    /* Auto-detect format from first few reports — NO LOGGING here (USB callback context) */
    if (s_format == GP_FORMAT_UNKNOWN && s_detect_count < 5) {
        gp_report_format_t detected = detect_format(data, len);
        if (detected != GP_FORMAT_UNKNOWN) {
            s_format = detected;
            s_format_log_len = len;
            s_format_log_pending = 1;  /* defer logging to gamepad_task */
        }
        s_detect_count++;
        if (s_detect_count >= 5 && s_format == GP_FORMAT_UNKNOWN) {
            s_format = (len >= 10 && data[0] == 0x01) ? GP_FORMAT_PS4 : GP_FORMAT_GENERIC;
            s_format_log_len = len;
            s_format_log_pending = 1;
        }
        if (s_format == GP_FORMAT_UNKNOWN) return;
    }

    gamepad_state_t gs;
    memset(&gs, 0, sizeof(gs));
    gs.connected = 1;
    /* Keyboard and gamepad can be connected at the same time. Preserve the
     * keyboard snapshot when a gamepad report updates the shared state. */
    gs.keyboard_keys = s_keyboard_keys;

    switch (s_format) {
	case GP_FORMAT_SWITCH_PRO:
		/* Standard wired Pro Controller report 0x30:
		 * [id][timer][battery][buttons0..2][LX0..2][RX0..2][rumble]
		 * Buttons are a 24-bit little-endian mask; sticks are packed 12-bit.
		 */
		if (len < 13 || data[0] != 0x30) return;
		s_switch_report_seen = true;
		{
			uint32_t b = (uint32_t)data[3] | ((uint32_t)data[4] << 8) |
			             ((uint32_t)data[5] << 16);
			if (b & (1u << 3))  gs.buttons |= GAMEPAD_BTN_A;
			if (b & (1u << 2))  gs.buttons |= GAMEPAD_BTN_B;
			if (b & (1u << 1))  gs.buttons |= GAMEPAD_BTN_X;
			if (b & (1u << 0))  gs.buttons |= GAMEPAD_BTN_Y;
			if (b & (1u << 22)) gs.buttons |= GAMEPAD_BTN_L1;
			if (b & (1u << 6))  gs.buttons |= GAMEPAD_BTN_R1;
			if (b & (1u << 23)) gs.buttons |= GAMEPAD_BTN_L2;
			if (b & (1u << 7))  gs.buttons |= GAMEPAD_BTN_R2;
			if (b & (1u << 8))  gs.buttons |= GAMEPAD_BTN_SELECT;
			if (b & (1u << 9))  gs.buttons |= GAMEPAD_BTN_START;
			if (b & (1u << 11)) gs.buttons |= GAMEPAD_BTN_L3;
			if (b & (1u << 10)) gs.buttons |= GAMEPAD_BTN_R3;
			if (b & (1u << 12)) gs.buttons |= GAMEPAD_BTN_HOME;
			if (b & (1u << 16)) gs.dpad |= GAMEPAD_DPAD_DOWN;
			if (b & (1u << 17)) gs.dpad |= GAMEPAD_DPAD_UP;
			if (b & (1u << 18)) gs.dpad |= GAMEPAD_DPAD_RIGHT;
			if (b & (1u << 19)) gs.dpad |= GAMEPAD_DPAD_LEFT;
			uint16_t lx = data[6] | ((uint16_t)(data[7] & 0x0F) << 8);
			uint16_t ly = (data[7] >> 4) | ((uint16_t)data[8] << 4);
			uint16_t rx = data[9] | ((uint16_t)(data[10] & 0x0F) << 8);
			uint16_t ry = (data[10] >> 4) | ((uint16_t)data[11] << 4);
			gs.axis_lx = (int16_t)(lx >> 4) - 128;
			/* This controller reports stick Y in the opposite direction from
			 * the launcher/emulator convention: physical up must be negative. */
			gs.axis_ly = 128 - (int16_t)(ly >> 4);
			gs.axis_rx = (int16_t)(rx >> 4) - 128;
			gs.axis_ry = (int16_t)(ry >> 4) - 128;
		}
		break;
    case GP_FORMAT_PS3:
        /* PS3 DualShock 3: [0x01][00][btn1][btn2][ps_btn][00][LX][LY][RX][RY]...
         * btn1 byte[2]: bit0=Sel bit1=L3 bit2=R3 bit3=Start bit4=Up bit5=Right bit6=Down bit7=Left
         * btn2 byte[3]: bit0=L2 bit1=R2 bit2=L1 bit3=R1 bit4=Tri bit5=Cir bit6=Cross bit7=Sq
         */
        if (len < 10) break;
        gs.axis_lx  = (int16_t)data[6] - 128;
        gs.axis_ly  = (int16_t)data[7] - 128;
        gs.axis_rx  = (int16_t)data[8] - 128;
        gs.axis_ry  = (int16_t)data[9] - 128;
        /* D-pad from byte 2 digital bits */
        if (data[2] & 0x10) gs.dpad |= GAMEPAD_DPAD_UP;
        if (data[2] & 0x20) gs.dpad |= GAMEPAD_DPAD_RIGHT;
        if (data[2] & 0x40) gs.dpad |= GAMEPAD_DPAD_DOWN;
        if (data[2] & 0x80) gs.dpad |= GAMEPAD_DPAD_LEFT;
        /* Face buttons from byte 3 */
        if (data[3] & 0x40) gs.buttons |= GAMEPAD_BTN_A;       /* Cross  */
        if (data[3] & 0x20) gs.buttons |= GAMEPAD_BTN_B;       /* Circle */
        if (data[3] & 0x80) gs.buttons |= GAMEPAD_BTN_X;       /* Square */
        if (data[3] & 0x10) gs.buttons |= GAMEPAD_BTN_Y;       /* Triangle */
        if (data[3] & 0x04) gs.buttons |= GAMEPAD_BTN_L1;
        if (data[3] & 0x08) gs.buttons |= GAMEPAD_BTN_R1;
        if (data[3] & 0x01) gs.buttons |= GAMEPAD_BTN_L2;
        if (data[3] & 0x02) gs.buttons |= GAMEPAD_BTN_R2;
        /* System buttons from byte 2 */
        if (data[2] & 0x01) gs.buttons |= GAMEPAD_BTN_SELECT;
        if (data[2] & 0x08) gs.buttons |= GAMEPAD_BTN_START;
        if (data[2] & 0x02) gs.buttons |= GAMEPAD_BTN_L3;
        if (data[2] & 0x04) gs.buttons |= GAMEPAD_BTN_R3;
        /* PS button from byte 4 */
        if (data[4] & 0x01) gs.buttons |= GAMEPAD_BTN_HOME;
        /* Analog triggers (if report long enough) */
        if (len >= 20) {
            gs.brake    = ((uint16_t)data[18]) << 2;   /* L2 pressure */
            gs.throttle = ((uint16_t)data[19]) << 2;   /* R2 pressure */
        }
        break;

    case GP_FORMAT_PS4:
        /* PS4: [0x01][LX][LY][RX][RY][hat+btn1][btn2][btn3][L2][R2]... */
        if (len < 10) break;
        gs.axis_lx  = (int16_t)data[1] - 128;
        gs.axis_ly  = (int16_t)data[2] - 128;
        gs.axis_rx  = (int16_t)data[3] - 128;
        gs.axis_ry  = (int16_t)data[4] - 128;
        gs.dpad     = hat_to_dpad(data[5] & 0x0F);
        gs.buttons  = parse_ps_buttons(data[5], data[6], data[7]);
        gs.brake    = ((uint16_t)data[8]) << 2;   /* L2: 0-255 → 0-1020 */
        gs.throttle = ((uint16_t)data[9]) << 2;   /* R2: 0-255 → 0-1020 */
        break;

    case GP_FORMAT_PS5:
        /* PS5: [0x01][LX][LY][RX][RY][L2][R2][cnt][hat+btn1][btn2][btn3]... */
        if (len < 11) break;
        gs.axis_lx  = (int16_t)data[1] - 128;
        gs.axis_ly  = (int16_t)data[2] - 128;
        gs.axis_rx  = (int16_t)data[3] - 128;
        gs.axis_ry  = (int16_t)data[4] - 128;
        gs.brake    = ((uint16_t)data[5]) << 2;
        gs.throttle = ((uint16_t)data[6]) << 2;
        gs.dpad     = hat_to_dpad(data[8] & 0x0F);
        gs.buttons  = parse_ps_buttons(data[8], data[9], data[10]);
        break;

	case GP_FORMAT_DUAL_PSX_ADAPTOR:
		/* [0x01][80][80][HatX][HatY][XABY|0][Sel,Start|LX,RX][00] 8 bytes */
		if (len < 8) break;
		gs.axis_lx  = (int16_t)data[1] - 128;
        gs.axis_ly  = (int16_t)data[2] - 128;
        gs.axis_rx  = (int16_t)data[3] - 128;
        gs.axis_ry  = (int16_t)data[4] - 128;
        gs.brake    = 0; 
        gs.throttle = 0;
        gs.dpad     = data_to_dpad(data[3], data[4]);
        gs.buttons  = parse_ps_buttons(data[5], data[6], data[7]);
		break;

    case GP_FORMAT_GENERIC: {
        /*
         * Generic USB gamepad layout (8-byte, NO report ID):
         *
         *   [0]=LX  [1]=LY  [2]=RX  [3]=RY  [4]=Z-axis(0x80)  [5]=hat_lo|btns_hi  [6]=btns_lo  [7]=unused
         *
         *   Byte 5 low nibble:  hat switch (0x0F = centered, 0-7 = directions)
         *   Byte 5 high nibble: bit4=X, bit5=A, bit6=B, bit7=Y
         *   Byte 6:             bit0=L1, bit1=R1, bit4=Select, bit5=Start
         *   Byte 7:             unused on this pad
         *
         *   D-pad:  mapped to Left-stick axes (byte0=X, byte1=Y):
         *           Up=LY(0x00), Down=LY(0xFF), Left=LX(0x00), Right=LX(0xFF)
         *           Center = 0x7F
         */
        int off = 0;
        if (len > 8 && data[0] >= 0x01 && data[0] <= 0x0F) {
            off = 1;  /* Skip report ID */
        }
        if (len - off < 4) break;

        gs.axis_lx = (int16_t)data[off + 0] - 128;
        gs.axis_ly = (int16_t)data[off + 1] - 128;
        if (len - off >= 4) {
            gs.axis_rx = (int16_t)data[off + 2] - 128;
            gs.axis_ry = (int16_t)data[off + 3] - 128;
        }

        /* D-pad from LX/LY axes (this gamepad uses axes, not hat, for d-pad) */
        {
            uint8_t lx = data[off + 0];
            uint8_t ly = data[off + 1];
            /* Threshold: <0x20 = min, >0xE0 = max, else center */
            if (ly < 0x20) gs.dpad |= GAMEPAD_DPAD_UP;
            if (ly > 0xE0) gs.dpad |= GAMEPAD_DPAD_DOWN;
            if (lx < 0x20) gs.dpad |= GAMEPAD_DPAD_LEFT;
            if (lx > 0xE0) gs.dpad |= GAMEPAD_DPAD_RIGHT;
        }

        /* Also check hat in case another gamepad uses it: byte 5 low nibble */
        if (len - off >= 6) {
            uint8_t hat_val = data[off + 5] & 0x0F;
            if (hat_val <= 7) {
                gs.dpad |= hat_to_dpad(hat_val);
            }
        }

        /* Face buttons: byte 5 high nibble */
        if (len - off >= 6) {
            uint8_t bh5 = data[off + 5];
            if (bh5 & 0x10) gs.buttons |= GAMEPAD_BTN_X;
            if (bh5 & 0x20) gs.buttons |= GAMEPAD_BTN_A;
            if (bh5 & 0x40) gs.buttons |= GAMEPAD_BTN_B;
            if (bh5 & 0x80) gs.buttons |= GAMEPAD_BTN_Y;
        }

        /* Shoulder + meta buttons: byte 6 */
        if (len - off >= 7) {
            uint8_t bl = data[off + 6];
            if (bl & 0x01) gs.buttons |= GAMEPAD_BTN_L1;
            if (bl & 0x02) gs.buttons |= GAMEPAD_BTN_R1;
            if (bl & 0x10) gs.buttons |= GAMEPAD_BTN_SELECT;
            if (bl & 0x20) gs.buttons |= GAMEPAD_BTN_START;
        }
        break;
    }
    default:
        return;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    memcpy(&s_state, &gs, sizeof(gs));
    xSemaphoreGive(s_mutex);
}

/* Boot-protocol keyboard report: modifiers, reserved, six key usages. */
static void parse_keyboard_report(const uint8_t *data, int len)
{
    if (len < 8) return;
    uint32_t keys = 0;
    int start = (len >= 9 && data[0] == 0x01) ? 1 : 0;
    for (int i = start + 2; i < start + 8 && i < len; ++i) {
        switch (data[i]) {
        case 0x52: keys |= GAMEPAD_KEY_UP; break;
        case 0x51: keys |= GAMEPAD_KEY_DOWN; break;
        case 0x50: keys |= GAMEPAD_KEY_LEFT; break;
        case 0x4F: keys |= GAMEPAD_KEY_RIGHT; break;
        case 0x1D: keys |= GAMEPAD_KEY_A; break; /* Z */
        case 0x1B: keys |= GAMEPAD_KEY_B; break; /* X */
        case 0x04: keys |= GAMEPAD_KEY_X; break; /* A */
        case 0x16: keys |= GAMEPAD_KEY_Y; break; /* S */
        case 0x14: keys |= GAMEPAD_KEY_L; break; /* Q */
        case 0x1A: keys |= GAMEPAD_KEY_R; break; /* W */
        case 0x28: keys |= GAMEPAD_KEY_START; break; /* Enter */
        case 0x29: keys |= GAMEPAD_KEY_MENU; break; /* Escape */
        case 0x2D: keys |= GAMEPAD_KEY_VOLUME_DOWN; break; /* - */
        case 0x2E: keys |= GAMEPAD_KEY_VOLUME_UP; break; /* = */
        default: break;
        }
    }
    /* Either Shift key acts as Select. */
    if (data[start] & 0x22) keys |= GAMEPAD_KEY_SELECT;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_keyboard_keys = keys;
    s_state.keyboard_keys = keys;
    xSemaphoreGive(s_mutex);
    /* Defer logging out of the USB callback, but retain every state change. */
    if (keys != s_keyboard_log_mask) {
        s_keyboard_log_mask = keys;
        s_keyboard_log_pending = 1;
    }
}

/* ========================= HID Host Callbacks ========================= */

/**
 * Interface-level callback: receives input reports and disconnect events
 */
static void hid_interface_cb(hid_host_device_handle_t hid_dev,
                              const hid_host_interface_event_t event,
                              void *arg)
{
    switch (event) {
    case HID_HOST_INTERFACE_EVENT_INPUT_REPORT: {
        uint8_t data[64];
        size_t data_len = 0;
        esp_err_t err = hid_host_device_get_raw_input_report_data(hid_dev, data, sizeof(data), &data_len);
        if (err == ESP_OK && data_len > 0) {
            hid_host_dev_params_t params;
            hid_host_device_get_params(hid_dev, &params);
            if (params.proto == HID_PROTOCOL_KEYBOARD) {
                parse_keyboard_report(data, (int)data_len);
            } else if (params.proto != HID_PROTOCOL_MOUSE) {
                parse_gamepad_report(data, (int)data_len);
            }
        }
        break;
    }
    case HID_HOST_INTERFACE_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "USB HID device disconnected");
        hid_host_dev_params_t params;
        bool is_keyboard = false;
        bool is_gamepad = false;
        if (hid_host_device_get_params(hid_dev, &params) == ESP_OK) {
            is_keyboard = (params.proto == HID_PROTOCOL_KEYBOARD);
            is_gamepad = (params.proto != HID_PROTOCOL_KEYBOARD &&
                          params.proto != HID_PROTOCOL_MOUSE);
        }
        /* The device descriptor can already be partially torn down by the
         * time DISCONNECTED is delivered, so retain the handle identity as
         * a fallback for composite USB devices. */
        if (hid_dev == s_keyboard_handle) is_keyboard = true;
        if (hid_dev == s_gamepad_handle) is_gamepad = true;
        hid_host_device_close(hid_dev);
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        if (is_keyboard) {
            /* A keyboard may be one interface of a composite gamepad. Do not
             * erase the gamepad snapshot when only its keyboard interface
             * disconnects. */
            s_keyboard_connected = false;
            s_keyboard_keys = 0;
            s_state.keyboard_keys = 0;
            s_keyboard_handle = NULL;
        } else if (is_gamepad) {
            /* Preserve a still-connected keyboard while clearing gamepad
             * input. The old code memset the whole shared state here, which
             * made reconnecting a keyboard disturb the controller. */
            uint32_t keyboard_keys = s_keyboard_keys;
            memset(&s_state, 0, sizeof(s_state));
            s_state.keyboard_keys = keyboard_keys;
            s_vid = 0;
            s_pid = 0;
            /* Reset format detection for next device */
            s_format = GP_FORMAT_UNKNOWN;
            s_detect_count = 0;
            s_gamepad_handle = NULL;
        }
        xSemaphoreGive(s_mutex);
        break;
    case HID_HOST_INTERFACE_EVENT_TRANSFER_ERROR:
        /* Transfer error — most likely device disconnecting.
         * Do NOT close device here! The USB Host Library will fire DEV_GONE → DISCONNECTED
         * through the normal disconnect flow once it finishes cleaning up pending URBs.
         * Closing here would race the USB core and leave interfaces unreleased. */
        ESP_LOGW(TAG, "HID transfer error — waiting for USB core disconnect flow");
        break;
    default:
        break;
    }
}

/**
 * Driver-level callback: new HID device detected → queue for gamepad_task
 */
static void hid_device_cb(hid_host_device_handle_t hid_dev,
                           const hid_host_driver_event_t event,
                           void *arg)
{
    if (event == HID_HOST_DRIVER_EVENT_CONNECTED) {
        gp_evt_t evt = {
            .type = GP_EVT_DEVICE_CONNECTED,
            .hid_handle = hid_dev,
        };
        if (s_event_queue) {
            xQueueSend(s_event_queue, &evt, 0);
        }
    }
}

/* ========================= USB Host Library Task ========================= */

static void usb_lib_task(void *arg)
{
    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL3,  /* Higher priority for USB interrupt */
    };

    ESP_ERROR_CHECK(usb_host_install(&host_config));
    ESP_LOGI(TAG, "USB host library installed");
    xTaskNotifyGive((TaskHandle_t)arg);

    while (s_running) {
        uint32_t event_flags;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            usb_host_device_free_all();
            if (!s_running) break;
        }
    }

    ESP_LOGI(TAG, "USB host library shutting down");
    vTaskDelay(pdMS_TO_TICKS(10));
    usb_host_uninstall();
    vTaskDelete(NULL);
}

/* ========================= Sony DualShock 3 Operational-Mode Enable ========================= */

/*
 * Sony DualShock 3 / Sixaxis pads (VID 0x054C, PID 0x0268) do NOT stream input reports
 * over USB after enumeration — they sit silent with all four LEDs blinking until the
 * host puts them into "operational mode" via a HID Feature request. Clone pads auto-
 * stream (and use a different VID/PID), so this is gated to the genuine Sony VID/PID and
 * every step is best-effort (a STALL is non-fatal — input arrives on the interrupt IN
 * endpoint, not EP0).
 *
 * NOTE: on the units seen so far the operational-mode enable + LED report are necessary
 * but NOT sufficient — the pad lights player LED 1 but its input subsystem stays asleep
 * until a physical button is pressed once. Nothing host-side (periodic re-send of the
 * enable/LED report, SET_IDLE) was found to wake it; those were tried and removed. So a
 * single button press after connect is expected before the pad responds.
 */

/* DS3 LED/rumble OUTPUT report (report id 0x01). byte[9] is the LED bitmask
 * (0x02 = player LED 1); the four 5-byte blocks that follow are the standard
 * per-LED on/off/duty config. Module-scope (non-const: the HID API takes a
 * non-const pointer) and only touched from gamepad_task, so no locking needed. */
static uint8_t s_ds3_led_report[] = {
    0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x02,   /* byte[9] = LED1 */
    0xff, 0x27, 0x10, 0x00, 0x32,   /* LED4 config */
    0xff, 0x27, 0x10, 0x00, 0x32,   /* LED3 config */
    0xff, 0x27, 0x10, 0x00, 0x32,   /* LED2 config */
    0xff, 0x27, 0x10, 0x00, 0x32,   /* LED1 config */
    0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00
};

static void ds3_enable_operational(hid_host_device_handle_t hid_dev)
{
    /* Step 1: GET_REPORT (Feature) 0xF2 — reads the controller's Bluetooth MAC and
     * primes some genuine units to begin reporting. */
    uint8_t f2[17] = {0};
    size_t  f2_len = sizeof(f2);
    esp_err_t err = hid_class_request_get_report(hid_dev, HID_REPORT_TYPE_FEATURE,
                                                 0xF2, f2, &f2_len);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "DS3: GET_REPORT 0xF2 failed: %s (continuing)", esp_err_to_name(err));
    }

    /* Step 2: SET_REPORT (Feature) 0xF4 = 42 0C 00 00 — enable operational mode. */
    uint8_t enable[4] = {0x42, 0x0C, 0x00, 0x00};
    err = hid_class_request_set_report(hid_dev, HID_REPORT_TYPE_FEATURE,
                                       0xF4, enable, sizeof(enable));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "DS3: SET_REPORT 0xF4 (enable) failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "DS3: operational-mode enable sent (0xF4 42 0C 00 00)");
    }

    /* Step 3: SET_REPORT (Output) report 0x01 — LED/rumble control report.
     * Assigns player LED 1 (cosmetic recognition indicator). Best-effort. */
    err = hid_class_request_set_report(hid_dev, HID_REPORT_TYPE_OUTPUT,
                                       0x01, s_ds3_led_report, sizeof(s_ds3_led_report));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "DS3: SET_REPORT 0x01 (LED/activate) failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "DS3: LED/activate output report sent (%d bytes)",
                 (int)sizeof(s_ds3_led_report));
    }
}

/* The genuine Switch Pro controller enumerates, but remains silent until the
 * USB transport handshake is sent. These reports must go through the
 * controller's interrupt OUT endpoint. The class-control SET_REPORT path
 * returns STALL on the official 057E:2009 device, which was why a controller
 * plugged in at boot stayed at all-zero reports until it was re-enumerated. */
static const char *switch_pro_err_name(esp_err_t err)
{
	return (err == ESP_OK) ? "OK" : esp_err_to_name(err);
}

static void switch_pro_enable(hid_host_device_handle_t hid_dev)
{
	const struct {
		uint8_t command;
		const char *name;
	} usb_commands[] = {
		{ 0x02, "handshake 1" },
		{ 0x03, "baudrate" },
		{ 0x02, "handshake 2" },
		{ 0x04, "no-timeout" },
	};

	for (size_t i = 0; i < sizeof(usb_commands) / sizeof(usb_commands[0]); ++i) {
		/* The report ID is part of the raw interrupt-OUT payload. */
		uint8_t report[] = { 0x80, usb_commands[i].command };
		esp_err_t err = hid_host_device_send_report(hid_dev, report, sizeof(report), 1000);
		ESP_LOGI(TAG, "Switch Pro USB %s: %s", usb_commands[i].name,
		         switch_pro_err_name(err));
		vTaskDelay(pdMS_TO_TICKS(25));
	}

	/* Output report 0x01: packet number, eight rumble bytes, subcommand 0x03,
	 * and report mode 0x30. This is 12 bytes including the report ID. */
	uint8_t mode[] = {
		0x01, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x03, 0x30,
	};
	esp_err_t err = hid_host_device_send_report(hid_dev, mode, sizeof(mode), 1000);
	ESP_LOGI(TAG, "Switch Pro report mode 0x30: %s", switch_pro_err_name(err));
	vTaskDelay(pdMS_TO_TICKS(50));
}

/* ========================= Gamepad Processing Task ========================= */

static const char *proto_names[] = { "NONE", "KEYBOARD", "MOUSE" };

static void gamepad_task(void *arg)
{
    gp_evt_t evt;

    ESP_LOGI(TAG, "Waiting for USB HID gamepad...");

    while (s_running) {
        if (s_keyboard_log_pending) {
            s_keyboard_log_pending = 0;
            ESP_LOGI(TAG, "Keyboard state: 0x%08lx (Z=A, X=B, arrows=D-pad)",
                     (unsigned long)s_keyboard_log_mask);
        }
        /* Deferred format detection log (avoid logging in USB callback context) */
        if (s_format_log_pending) {
            s_format_log_pending = 0;
			const char *names[] = {"unknown", "PS3", "PS4", "PS5", "Generic", "Switch Pro", "Dual PSX Adaptor"};
            ESP_LOGI(TAG, "Detected report format: %s (len=%d)", names[s_format], s_format_log_len);
        }

        /* Deferred raw hex dump (first 3 reports only, for initial format debugging) */
        if (s_raw_dump_count < 3 && s_raw_dump_len > 0) {
            s_raw_dump_count++;
            ESP_LOGI(TAG, "Raw report (%d bytes):", s_raw_dump_len);
            ESP_LOG_BUFFER_HEX_LEVEL(TAG, s_raw_dump_buf, s_raw_dump_len, ESP_LOG_INFO);
        }

        if (xQueueReceive(s_event_queue, &evt, pdMS_TO_TICKS(50))) {
            if (evt.type == GP_EVT_DEVICE_CONNECTED) {
                hid_host_dev_params_t params;
                ESP_ERROR_CHECK(hid_host_device_get_params(evt.hid_handle, &params));

                int proto_idx = (params.proto <= HID_PROTOCOL_MOUSE) ? params.proto : 0;
                ESP_LOGI(TAG, "HID device connected: sub_class=%d, proto=%s",
                         params.sub_class, proto_names[proto_idx]);

                if (params.proto != HID_PROTOCOL_KEYBOARD && params.proto != HID_PROTOCOL_MOUSE) {
                    ESP_LOGI(TAG, "==> Gamepad detected!");

                    /* Retrieve VID/PID for controller identification */
                    hid_host_dev_info_t dev_info;
                    if (hid_host_get_device_info(evt.hid_handle, &dev_info) == ESP_OK) {
                        s_vid = dev_info.VID;
                        s_pid = dev_info.PID;
                        ESP_LOGI(TAG, "Gamepad VID=0x%04X PID=0x%04X", s_vid, s_pid);
                    }
                }

                /* Open device and configure interface callback */
                const hid_host_device_config_t dev_config = {
                    .callback = hid_interface_cb,
                    .callback_arg = NULL,
                };

                esp_err_t err = hid_host_device_open(evt.hid_handle, &dev_config);
                if (err != ESP_OK) {
                    ESP_LOGW(TAG, "hid_host_device_open failed: %s", esp_err_to_name(err));
                    continue;
                }

                /* Publish the handle before the one-second stabilization
                 * delay. Some controllers begin reporting immediately after
                 * the interface is opened; keeping the handle here also lets
                 * the disconnect callback classify an early teardown safely. */
                bool is_gamepad = (params.proto != HID_PROTOCOL_KEYBOARD &&
                                   params.proto != HID_PROTOCOL_MOUSE);
				if (is_gamepad) {
					s_gamepad_handle = evt.hid_handle;
					s_switch_report_seen = false;
                    xSemaphoreTake(s_mutex, portMAX_DELAY);
                    s_state.connected = 1;
                    xSemaphoreGive(s_mutex);
                }

                /* Boot-interface devices: set protocol */
                if (HID_SUBCLASS_BOOT_INTERFACE == params.sub_class) {
                    hid_class_request_set_protocol(evt.hid_handle, HID_REPORT_PROTOCOL_BOOT);
                }

                /* SET_IDLE is useful for boot keyboards; gamepads may STALL it. */
                if (params.proto == HID_PROTOCOL_KEYBOARD) {
                    s_keyboard_connected = true;
                    s_keyboard_handle = evt.hid_handle;
                    hid_class_request_set_idle(evt.hid_handle, 0, 0);
                    ESP_LOGI(TAG, "USB keyboard enabled (boot protocol)");
                }

                /* Let device settle after SET_CONFIGURATION. PS4 controllers may
                 * draw high current that causes VBUS droop → brief disconnect.
                 * Long delay gives power supply time to stabilize. */
                ESP_LOGI(TAG, "Waiting 1000ms for device to stabilize...");
                vTaskDelay(pdMS_TO_TICKS(1000));

                /* Try to start with retry logic */
                err = ESP_FAIL;
                for (int attempt = 0; attempt < 3; attempt++) {
                    err = hid_host_device_start(evt.hid_handle);
                    if (err == ESP_OK) {
                        ESP_LOGI(TAG, "hid_host_device_start OK (attempt %d)", attempt + 1);
                        break;
                    }
                    ESP_LOGW(TAG, "hid_host_device_start failed (attempt %d): %s",
                             attempt + 1, esp_err_to_name(err));
                    vTaskDelay(pdMS_TO_TICKS(100));
                }
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "hid_host_device_start giving up after 3 attempts");
                    hid_host_device_close(evt.hid_handle);
                    continue;
                }

                /* Genuine Sony DualShock 3 (Sixaxis) needs the operational-mode enable
                 * before it can stream input over USB. Sent AFTER device_start so the
                 * host is already polling the interrupt-IN endpoint. (Even so, this pad
                 * still needs one physical button press to actually begin streaming —
                 * see ds3_enable_operational.) */
                if (s_vid == 0x054C && s_pid == 0x0268) {
                    ESP_LOGI(TAG, "DS3 detected (054C:0268) — sending operational-mode enable");
                    ds3_enable_operational(evt.hid_handle);
                }
				if (s_vid == 0x057E && s_pid == 0x2009) {
					ESP_LOGI(TAG, "Switch Pro detected (057E:2009) — initializing USB HID transport");
					/* A cold-boot-connected Pro controller may still be waking its
					 * USB MCU when the first sequence is sent. Repeat the complete
					 * activation sequence until a real 0x30 input report arrives. */
					for (int attempt = 1; attempt <= 3; ++attempt) {
						switch_pro_enable(evt.hid_handle);
						vTaskDelay(pdMS_TO_TICKS(150));
						if (s_switch_report_seen) {
							ESP_LOGI(TAG, "Switch Pro input report active after handshake attempt %d", attempt);
							break;
						}
						ESP_LOGW(TAG, "Switch Pro sent no 0x30 input report after handshake attempt %d", attempt);
					}
					if (!s_switch_report_seen) {
						ESP_LOGW(TAG, "Switch Pro transport initialized but input is still silent; retry by reconnecting controller");
					}
				}

                /* Mark connected for gamepad devices */
                /* The state was published before stabilization above so
                 * early input and disconnects are associated with this HID
                 * interface. */
            }
        }
    }

    vTaskDelete(NULL);
}

/* ========================= Public API ========================= */

esp_err_t gamepad_init(const gamepad_config_t *config)
{
    if (s_running) {
        return ESP_ERR_INVALID_STATE;
    }

    memset(&s_state, 0, sizeof(s_state));
    s_keyboard_keys = 0;
    s_keyboard_connected = false;
    s_keyboard_handle = NULL;
    s_gamepad_handle = NULL;
    s_format = GP_FORMAT_UNKNOWN;
    s_detect_count = 0;
    s_format_log_pending = 0;
    s_raw_dump_count = 0;
    s_raw_dump_len = 0;
    s_switch_report_seen = false;

    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) return ESP_ERR_NO_MEM;

    s_event_queue = xQueueCreate(10, sizeof(gp_evt_t));
    if (!s_event_queue) {
        vSemaphoreDelete(s_mutex);
        return ESP_ERR_NO_MEM;
    }

    s_running = true;

    /* Start USB host library task */
    BaseType_t ret = xTaskCreatePinnedToCore(usb_lib_task, "usb_host",
                                              config->usb_task_stack,
                                              xTaskGetCurrentTaskHandle(),
                                              config->usb_task_priority,
                                              &s_usb_task, 0);
    if (ret != pdTRUE) {
        ESP_LOGE(TAG, "Failed to create USB host task");
        s_running = false;
        vQueueDelete(s_event_queue);
        vSemaphoreDelete(s_mutex);
        return ESP_ERR_NO_MEM;
    }

    /* Wait for USB host to be ready */
    ulTaskNotifyTake(false, pdMS_TO_TICKS(3000));

    /* Install HID host driver (creates its own background task) */
    const hid_host_driver_config_t hid_config = {
        .create_background_task = true,
        .task_priority = config->hid_task_priority,
        .stack_size = config->hid_task_stack,
        .core_id = 0,
        .callback = hid_device_cb,
        .callback_arg = NULL,
    };

    esp_err_t err = hid_host_install(&hid_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to install HID host driver: %s", esp_err_to_name(err));
        s_running = false;
        /* USB host will shut down in its task when s_running becomes false */
        vQueueDelete(s_event_queue);
        vSemaphoreDelete(s_mutex);
        return err;
    }

    /* Start gamepad event processing task */
    ret = xTaskCreatePinnedToCore(gamepad_task, "gamepad", 4096, NULL,
                                   config->hid_task_priority, &s_gp_task, 1);
    if (ret != pdTRUE) {
        ESP_LOGE(TAG, "Failed to create gamepad task");
        hid_host_uninstall();
        s_running = false;
        vQueueDelete(s_event_queue);
        vSemaphoreDelete(s_mutex);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "USB HID gamepad host initialized — plug in a USB gamepad!");
    return ESP_OK;
}

void gamepad_deinit(void)
{
    if (!s_running) return;

    s_running = false;

    /* Wait for tasks to finish */
    vTaskDelay(pdMS_TO_TICKS(500));

    hid_host_uninstall();

    if (s_event_queue) {
        vQueueDelete(s_event_queue);
        s_event_queue = NULL;
    }
    if (s_mutex) {
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
    }

    s_usb_task = NULL;
    s_gp_task = NULL;
    memset(&s_state, 0, sizeof(s_state));
}

void gamepad_get_state(gamepad_state_t *state)
{
    if (!state || !s_mutex) {
        if (state) memset(state, 0, sizeof(*state));
        return;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    memcpy(state, &s_state, sizeof(gamepad_state_t));
    state->keyboard_keys = s_keyboard_keys;
    xSemaphoreGive(s_mutex);
}

bool gamepad_is_connected(void)
{
    if (!s_mutex) return false;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool conn = (s_state.connected != 0);
    xSemaphoreGive(s_mutex);
    return conn;
}

bool gamepad_is_keyboard_connected(void)
{
    return s_keyboard_connected;
}

void gamepad_buttons_to_str(uint32_t buttons, char *buf, size_t buf_size)
{
    if (!buf || buf_size == 0) return;
    buf[0] = '\0';

    static const struct { uint32_t mask; const char *name; } btn_names[] = {
        { GAMEPAD_BTN_A,      "A"      },
        { GAMEPAD_BTN_B,      "B"      },
        { GAMEPAD_BTN_X,      "X"      },
        { GAMEPAD_BTN_Y,      "Y"      },
        { GAMEPAD_BTN_L1,     "L1"     },
        { GAMEPAD_BTN_R1,     "R1"     },
        { GAMEPAD_BTN_L2,     "L2"     },
        { GAMEPAD_BTN_R2,     "R2"     },
        { GAMEPAD_BTN_SELECT, "SEL"    },
        { GAMEPAD_BTN_START,  "START"  },
        { GAMEPAD_BTN_L3,     "L3"     },
        { GAMEPAD_BTN_R3,     "R3"     },
        { GAMEPAD_BTN_HOME,   "HOME"   },
        { GAMEPAD_BTN_MISC,   "MISC"   },
    };

    size_t pos = 0;
    for (int i = 0; i < sizeof(btn_names) / sizeof(btn_names[0]); i++) {
        if (buttons & btn_names[i].mask) {
            int written = snprintf(buf + pos, buf_size - pos,
                                   "%s%s", (pos > 0) ? " " : "", btn_names[i].name);
            if (written > 0) pos += written;
            if (pos >= buf_size - 1) break;
        }
    }

    if (pos == 0) {
        snprintf(buf, buf_size, "(none)");
    }
}

int gamepad_get_raw_report(uint8_t *buf, size_t buf_size)
{
    if (!buf || buf_size == 0 || s_raw_dump_len == 0) return 0;
    int copy = (s_raw_dump_len < (int)buf_size) ? s_raw_dump_len : (int)buf_size;
    memcpy(buf, s_raw_dump_buf, copy);
    return copy;
}

void gamepad_get_vid_pid(uint16_t *vid, uint16_t *pid)
{
    if (vid) *vid = s_vid;
    if (pid) *pid = s_pid;
}
