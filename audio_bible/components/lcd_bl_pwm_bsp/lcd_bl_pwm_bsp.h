#ifndef LCD_BL_PWM_BSP_H
#define LCD_BL_PWM_BSP_H

#include <stdint.h>

// Backlight duty is 8-bit and ACTIVE-HIGH on the 1.85C V2: 0 = off, 255 = full. (The
// old inverted LCD_PWM_MODE_* (0xff-N) macros were a V1 leftover where 255 confusingly
// meant "off"; removed to avoid that trap — callers pass a plain 0..255 duty.)

#ifdef __cplusplus
extern "C" {
#endif

void lcd_bl_pwm_bsp_init(uint16_t duty);
void setUpduty(uint16_t duty);

#ifdef __cplusplus
}
#endif

#endif