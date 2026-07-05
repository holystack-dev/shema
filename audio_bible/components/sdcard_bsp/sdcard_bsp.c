#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <sys/unistd.h>
#include <sys/stat.h>
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "sdcard_bsp.h"
#include "esp_log.h"
#include "esp_err.h"

// ESP32-S3-Touch-LCD-1.85C V2: SD over SDMMC 1-bit.
#define SDMMC_D0_PIN    16
#define SDMMC_CLK_PIN   14
#define SDMMC_CMD_PIN   17

static const char *TAG = "_sdcard";

EventGroupHandle_t sdcard_even_ = NULL;

sdcard_bsp_t user_sdcard_bsp;

#define SDlist "/sdcard" //目录,类似于一个标准

sdmmc_card_t *card_host = NULL;

void _sdcard_init(void)
{
  if (sdcard_even_ == NULL) sdcard_even_ = xEventGroupCreate();   // don't leak on re-init
  esp_vfs_fat_sdmmc_mount_config_t mount_config =
  {
    .format_if_mount_failed = false,       //如果挂靠失败，创建分区表并格式化SD卡
    .max_files = 5,                        //打开文件最大数
    .allocation_unit_size = 16 * 1024,     //cluster size; power-of-two (only used if formatting)
  };

  sdmmc_host_t host = SDMMC_HOST_DEFAULT();
  host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;//高速

  sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
  slot_config.width = 1;           //1线
  slot_config.clk = SDMMC_CLK_PIN;
  slot_config.cmd = SDMMC_CMD_PIN;
  slot_config.d0 = SDMMC_D0_PIN;

  ESP_ERROR_CHECK_WITHOUT_ABORT(esp_vfs_fat_sdmmc_mount(SDlist, &host, &slot_config, &mount_config, &card_host));

  if(card_host != NULL)
  {
    sdmmc_card_print_info(stdout, card_host); //把卡的信息打印出来
    user_sdcard_bsp.sdcard_size = (float)(card_host->csd.capacity)/2048/1024; //G
    xEventGroupSetBits(sdcard_even_,0x01);
  }
}

// Cleanly unmount the FAT volume and release the card handle. Call before deep sleep
// so a power cut can't land mid FAT update and leave a dirty filesystem.
void sdcard_unmount(void)
{
  if (card_host != NULL)
  {
    esp_vfs_fat_sdcard_unmount(SDlist, card_host);
    card_host = NULL;
  }
  if (sdcard_even_ != NULL) xEventGroupClearBits(sdcard_even_, 0x01);
}



/* Write data
path: Path
data: Data */ 
esp_err_t sdcard_file_write(const char *path, const char *data)
{
  esp_err_t err;
  if(card_host == NULL)
  {
    ESP_LOGE(TAG, "SD card not initialized (card == NULL)");
    return ESP_ERR_NOT_FOUND;
  }
  err = sdmmc_get_status(card_host); //First, check if there is an SD card.
  if(err != ESP_OK)
  {
    ESP_LOGE(TAG, "SD card status check failed (card not present or unresponsive)");
    return err;
  }
  FILE *f = fopen(path, "w"); //Obtain the path address
  if(f == NULL)
  {
    ESP_LOGE(TAG, "Failed to open file: %s", path);
    return ESP_ERR_NOT_FOUND;
  }
  fputs(data, f);   // NOT fprintf(f, data): caller data must not be a format string
  fclose(f);
  return ESP_OK;
}
/*
Read data
path: path */
esp_err_t sdcard_file_read(const char *path, char *buffer, size_t bufsz, size_t *out_len)
{
  esp_err_t err;
  if(out_len != NULL) *out_len = 0;
  if(buffer == NULL || bufsz == 0) return ESP_ERR_INVALID_ARG;
  if(card_host == NULL)
  {
    ESP_LOGE(TAG, "SD card not initialized (card == NULL)");
    return ESP_ERR_NOT_FOUND;
  }
  err = sdmmc_get_status(card_host); //First, check if there is an SD card.
  if(err != ESP_OK)
  {
    ESP_LOGE(TAG, "SD card status check failed (card not present or unresponsive)");
    return err;
  }
  FILE *f = fopen(path, "rb");
  if (f == NULL)
  {
    ESP_LOGE(TAG, "Failed to open file: %s", path);
    return ESP_ERR_NOT_FOUND;
  }
  fseek(f, 0, SEEK_END);              // Move the pointer to the very end.
  long len = ftell(f);               // signed: -1 on error, must not wrap to 4 GB
  fseek(f, 0, SEEK_SET);             // Move the pointer to the very beginning.
  if (len < 0) { fclose(f); return ESP_FAIL; }
  size_t want = (size_t)len < bufsz ? (size_t)len : bufsz;   // never overrun the caller buffer
  size_t got = fread((void *)buffer, 1, want, f);
  if (out_len != NULL) *out_len = got;
  fclose(f);
  return ESP_OK;
}