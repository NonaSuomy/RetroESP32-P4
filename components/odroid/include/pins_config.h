#pragma once

/* LCD Resolution */
#define LCD_H_RES 1024
#define LCD_V_RES 600

/* LCD Reset Pin */
#define LCD_RST   -1

/* LCD Backlight Pin */
#define LCD_BK_LIGHT_GPIO  20

/* Touch I2C Pins */
#define TP_I2C_SDA  18
#define TP_I2C_SCL  19
#define TP_RST      40
#define TP_INT      41

/* I2S / ES8311 Audio Codec Pins */
#define I2S_MCLK_IO   -1
#define I2S_BCLK_IO   22
#define I2S_WS_IO     21
#define I2S_DOUT_IO   23   /* ESP32-P4 -> NS4168 DSDIN */
#define I2S_DIN_IO    -1
#define AUDIO_PA_IO    6   /* NS4168 power gate via P-MOS (active low) */

/* SD MMC Pins */
#define SD_MMC_CLK  43
#define SD_MMC_CMD  44
#define SD_MMC_D0   39
#define SD_MMC_D1   40
#define SD_MMC_D2   41
/* GPIO42 is the Elecrow relay control.  The TF socket is wired for the
 * one-bit bus only; deliberately do not expose/configure SD D3 here. */
