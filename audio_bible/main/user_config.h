#ifndef USER_CONFIG_H
#define USER_CONFIG_H

#include "driver/gpio.h"
#include "esp_io_expander.h"

// =====================================================================
//  Board: Waveshare ESP32-S3-Touch-LCD-1.85C  V2  (round 360x360)
//  Display: ST77916 (QSPI)   Touch: CST816S (I2C @0x15)
//  Ported from the 3.49" bar-LCD build (AXS15231B, 172x640).
// =====================================================================

// ---- SPI / I2C ports ----
#define LCD_HOST SPI2_HOST

// Single shared I2C bus (port 0): touch + RTC + IMU + ES8311/ES7210 + TCA9554.
// On this board SCL/SDA are fixed to GPIO10/GPIO11 and cannot be remapped.
#define ESP_SCL_NUM (GPIO_NUM_10)
#define ESP_SDA_NUM (GPIO_NUM_11)

// ---- Display (ST77916, QSPI) ----
#define EXAMPLE_PIN_NUM_LCD_CS     (GPIO_NUM_21)
#define EXAMPLE_PIN_NUM_LCD_PCLK   (GPIO_NUM_40)
#define EXAMPLE_PIN_NUM_LCD_DATA0  (GPIO_NUM_46)
#define EXAMPLE_PIN_NUM_LCD_DATA1  (GPIO_NUM_45)
#define EXAMPLE_PIN_NUM_LCD_DATA2  (GPIO_NUM_42)
#define EXAMPLE_PIN_NUM_LCD_DATA3  (GPIO_NUM_41)
#define EXAMPLE_PIN_NUM_LCD_RST    (-1)            // hardware reset via TCA9554 EXIO2
#define EXAMPLE_PIN_NUM_BK_LIGHT   (GPIO_NUM_5)    // backlight, active-high PWM

// Panel is a 360 x 360 round LCD.
#define EXAMPLE_LCD_H_RES 360
#define EXAMPLE_LCD_V_RES 360
// Chunked full-refresh flush: V_RES must divide evenly by the chunk height.
// 360 / 40 = 9 chunks.  DMA chunk = one strip of 40 rows.
#define LVGL_DMA_BUFF_LEN     (EXAMPLE_LCD_H_RES * 40 * 2)
#define LVGL_SPIRAM_BUFF_LEN  (EXAMPLE_LCD_H_RES * EXAMPLE_LCD_V_RES * 2)

// ---- Touch (CST816S, I2C) ----
#define EXAMPLE_PIN_NUM_TOUCH_ADDR  0x15
#define EXAMPLE_PIN_NUM_TOUCH_RST   (-1)           // hardware reset via TCA9554 EXIO1
#define EXAMPLE_PIN_NUM_TOUCH_INT   (GPIO_NUM_4)
#define CST816S_REG_TOUCH_DATA      0x02           // num,xH,xL,yH,yL

// ---- TCA9554 IO-expander (I2C @0x20) pin indices (0-based) ----
// EXIO1 -> pin 0 = touch reset ; EXIO2 -> pin 1 = LCD reset.
#define EXIO_TOUCH_RST  IO_EXPANDER_PIN_NUM_0
#define EXIO_LCD_RST    IO_EXPANDER_PIN_NUM_1

#define EXAMPLE_LVGL_TICK_PERIOD_MS    5
#define EXAMPLE_LVGL_TASK_MAX_DELAY_MS 500
#define EXAMPLE_LVGL_TASK_MIN_DELAY_MS 5

// ---- I2C device addresses ----
#define EXAMPLE_RTC_ADDR 0x51
#define EXAMPLE_IMU_ADDR 0x6b

// ---- Audio amplifier (NS4150B) enable: direct GPIO, active-high ----
#define EXAMPLE_PIN_NUM_AMP_EN (GPIO_NUM_15)

// ---- Power / buttons ----
// Only the BOOT button (GPIO0) exists on this board. Used to wake the screen,
// go Home, and (long-press) power off. Also the deep-sleep wake source.
#define EXAMPLE_PIN_NUM_BOOT_BTN (GPIO_NUM_0)

#endif // USER_CONFIG_H
