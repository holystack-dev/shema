#include <stdio.h>
#include "i2c_bsp.h"
#include "user_config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_io_expander.h"
#include "esp_io_expander_tca9554.h"

static const char *TAG = "i2c_bsp";

// Single shared I2C bus (port 0) for the whole board: touch (CST816S),
// RTC (PCF85063), IMU (QMI8658), the ES8311/ES7210 codecs (added later by
// codec_board, which reuses this same bus handle), and the TCA9554 expander.
static i2c_master_bus_handle_t user_i2c_port0_handle = NULL;
i2c_master_dev_handle_t disp_touch_dev_handle = NULL;
i2c_master_dev_handle_t rtc_dev_handle = NULL;
i2c_master_dev_handle_t imu_dev_handle = NULL;

// TCA9554 IO-expander: drives the ST77916 (EXIO2) and CST816S (EXIO1) resets.
// Created here, before the display/touch come up, so their reset lines are ready.
esp_io_expander_handle_t io_expander = NULL;

static uint32_t i2c_data_pdMS_TICKS = 0;
static uint32_t i2c_done_pdMS_TICKS = 0;

void expander_set_level(esp_io_expander_pin_num_t pin, uint8_t level)
{
  if (io_expander) esp_io_expander_set_level(io_expander, pin, level);
}

// Active-low hardware reset pulse via the expander (release -> assert -> release).
void expander_reset_pulse(esp_io_expander_pin_num_t pin)
{
  if (!io_expander) return;
  esp_io_expander_set_level(io_expander, pin, 0);
  vTaskDelay(pdMS_TO_TICKS(10));
  esp_io_expander_set_level(io_expander, pin, 1);
  vTaskDelay(pdMS_TO_TICKS(50));
}

void i2c_master_Init(void)
{
  i2c_data_pdMS_TICKS = pdMS_TO_TICKS(5000);
  i2c_done_pdMS_TICKS = pdMS_TO_TICKS(1000);

  i2c_master_bus_config_t i2c_bus_config =
  {
    .clk_source = I2C_CLK_SRC_DEFAULT,
    .i2c_port = I2C_NUM_0,
    .scl_io_num = ESP_SCL_NUM,
    .sda_io_num = ESP_SDA_NUM,
    .glitch_ignore_cnt = 7,
    .flags = {
      .enable_internal_pullup = true,
    },
  };
  ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_config, &user_i2c_port0_handle));

  i2c_device_config_t dev_cfg =
  {
    .dev_addr_length = I2C_ADDR_BIT_LEN_7,
    .scl_speed_hz = 300000,
  };
  dev_cfg.device_address = EXAMPLE_RTC_ADDR;
  ESP_ERROR_CHECK(i2c_master_bus_add_device(user_i2c_port0_handle, &dev_cfg, &rtc_dev_handle));

  dev_cfg.device_address = EXAMPLE_IMU_ADDR;
  ESP_ERROR_CHECK(i2c_master_bus_add_device(user_i2c_port0_handle, &dev_cfg, &imu_dev_handle));

  dev_cfg.device_address = EXAMPLE_PIN_NUM_TOUCH_ADDR;
  ESP_ERROR_CHECK(i2c_master_bus_add_device(user_i2c_port0_handle, &dev_cfg, &disp_touch_dev_handle));

  // TCA9554 expander @0x20 (address pins tied to 000).
  if (esp_io_expander_new_i2c_tca9554(user_i2c_port0_handle,
        ESP_IO_EXPANDER_I2C_TCA9554_ADDRESS_000, &io_expander) != ESP_OK) {
    ESP_LOGW(TAG, "TCA9554 not found; LCD/touch reset lines unavailable");
    io_expander = NULL;
  } else {
    esp_io_expander_set_dir(io_expander, EXIO_LCD_RST | EXIO_TOUCH_RST, IO_EXPANDER_OUTPUT);
    esp_io_expander_set_level(io_expander, EXIO_LCD_RST | EXIO_TOUCH_RST, 1); // released
    ESP_LOGI(TAG, "TCA9554 ready (EXIO1=touch-rst, EXIO2=lcd-rst)");
  }
}

uint8_t i2c_writr_buff(i2c_master_dev_handle_t dev_handle,int reg,uint8_t *buf,uint8_t len)
{
  uint8_t ret;
  uint8_t *pbuf = NULL;
  ret = i2c_master_bus_wait_all_done(user_i2c_port0_handle,i2c_done_pdMS_TICKS);
  if(ret != ESP_OK)
  return ret;
  if(reg == -1)
  {
    ret = i2c_master_transmit(dev_handle,buf,len,i2c_data_pdMS_TICKS);
  }
  else
  {
    pbuf = (uint8_t*)malloc(len+1);
    pbuf[0] = reg;
    for(uint8_t i = 0; i<len; i++)
    {
      pbuf[i+1] = buf[i];
    }
    ret = i2c_master_transmit(dev_handle,pbuf,len+1,i2c_data_pdMS_TICKS);
    free(pbuf);
    pbuf = NULL;
  }
  return ret;
}
uint8_t i2c_master_write_read_dev(i2c_master_dev_handle_t dev_handle,uint8_t *writeBuf,uint8_t writeLen,uint8_t *readBuf,uint8_t readLen)
{
  uint8_t ret;
  ret = i2c_master_bus_wait_all_done(user_i2c_port0_handle,i2c_done_pdMS_TICKS);
  if(ret != ESP_OK)
  return ret;
  ret = i2c_master_transmit_receive(dev_handle,writeBuf,writeLen,readBuf,readLen,i2c_data_pdMS_TICKS);
  return ret;
}
uint8_t i2c_read_buff(i2c_master_dev_handle_t dev_handle,int reg,uint8_t *buf,uint8_t len)
{
  uint8_t ret;
  uint8_t addr = 0;
  ret = i2c_master_bus_wait_all_done(user_i2c_port0_handle,i2c_done_pdMS_TICKS);
  if(ret != ESP_OK)
  return ret;
  if( reg == -1 )
  {ret = i2c_master_receive(dev_handle, buf,len, i2c_data_pdMS_TICKS);}
  else
  {addr = (uint8_t)reg; ret = i2c_master_transmit_receive(dev_handle,&addr,1,buf,len,i2c_data_pdMS_TICKS);}
  return ret;
}

// Touch read on the shared bus (CST816S). Kept for API compatibility.
uint8_t i2c_master_touch_write_read(i2c_master_dev_handle_t dev_handle,uint8_t *writeBuf,uint8_t writeLen,uint8_t *readBuf,uint8_t readLen)
{
  uint8_t ret;
  ret = i2c_master_bus_wait_all_done(user_i2c_port0_handle,i2c_done_pdMS_TICKS);
  if(ret != ESP_OK)
  return ret;
  ret = i2c_master_transmit_receive(dev_handle,writeBuf,writeLen,readBuf,readLen,i2c_data_pdMS_TICKS);
  return ret;
}
