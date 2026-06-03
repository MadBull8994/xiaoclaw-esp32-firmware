#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

#include <driver/gpio.h>

// ESP32-S3 OpenClaw V2.7, N16R8
#define AUDIO_INPUT_SAMPLE_RATE  16000
#define AUDIO_OUTPUT_SAMPLE_RATE 16000

#define AUDIO_I2S_METHOD_SIMPLEX
#define AUDIO_I2S_MIC_GPIO_WS    GPIO_NUM_4
#define AUDIO_I2S_MIC_GPIO_SCK   GPIO_NUM_5
#define AUDIO_I2S_MIC_GPIO_DIN   GPIO_NUM_6
#define AUDIO_I2S_SPK_GPIO_DOUT  GPIO_NUM_7
#define AUDIO_I2S_SPK_GPIO_BCLK  GPIO_NUM_15
#define AUDIO_I2S_SPK_GPIO_LRCK  GPIO_NUM_16

#define BUILTIN_LED_GPIO         GPIO_NUM_48
#define BOOT_BUTTON_GPIO         GPIO_NUM_0
#define VOLUME_UP_BUTTON_GPIO    GPIO_NUM_38
#define VOLUME_DOWN_BUTTON_GPIO  GPIO_NUM_39

#define DISPLAY_BACKLIGHT_PIN    GPIO_NUM_42
#define DISPLAY_MOSI_PIN         GPIO_NUM_47
#define DISPLAY_CLK_PIN          GPIO_NUM_21
#define DISPLAY_DC_PIN           GPIO_NUM_40
#define DISPLAY_RST_PIN          GPIO_NUM_45
#define DISPLAY_CS_PIN           GPIO_NUM_41

// 1.8 inch 8-pin ST7735. ESP-IDF's ST7789 serial panel driver also covers
// the ST7735 command subset used by this module.
#define LCD_TYPE_ST7735_SERIAL
#define DISPLAY_WIDTH            128
#define DISPLAY_HEIGHT           160
#define DISPLAY_MIRROR_X         false
#define DISPLAY_MIRROR_Y         false
#define DISPLAY_SWAP_XY          false
#define DISPLAY_INVERT_COLOR     false
#define DISPLAY_RGB_ORDER        LCD_RGB_ELEMENT_ORDER_RGB
#define DISPLAY_OFFSET_X         0
#define DISPLAY_OFFSET_Y         0
#define DISPLAY_BACKLIGHT_OUTPUT_INVERT false
#define DISPLAY_SPI_MODE         0

#endif // _BOARD_CONFIG_H_
