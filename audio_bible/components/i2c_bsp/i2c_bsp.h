#ifndef I2C_BSP_H
#define I2C_BSP_H
#include "driver/i2c_master.h"
#include "esp_io_expander.h"

extern i2c_master_dev_handle_t disp_touch_dev_handle;
extern i2c_master_dev_handle_t rtc_dev_handle;
extern i2c_master_dev_handle_t imu_dev_handle;

// TCA9554 expander shared across the board (LCD/touch reset lines, etc.).
extern esp_io_expander_handle_t io_expander;

#ifdef __cplusplus
extern "C" {
#endif

void i2c_master_Init(void);
uint8_t i2c_writr_buff(i2c_master_dev_handle_t dev_handle,int reg,uint8_t *buf,uint8_t len);
uint8_t i2c_master_write_read_dev(i2c_master_dev_handle_t dev_handle,uint8_t *writeBuf,uint8_t writeLen,uint8_t *readBuf,uint8_t readLen);
uint8_t i2c_read_buff(i2c_master_dev_handle_t dev_handle,int reg,uint8_t *buf,uint8_t len);
uint8_t i2c_master_touch_write_read(i2c_master_dev_handle_t dev_handle,uint8_t *writeBuf,uint8_t writeLen,uint8_t *readBuf,uint8_t readLen);

// Expander helpers (no-op if the TCA9554 was not found).
void expander_set_level(esp_io_expander_pin_num_t pin, uint8_t level);
void expander_reset_pulse(esp_io_expander_pin_num_t pin);

#ifdef __cplusplus
}
#endif

#endif