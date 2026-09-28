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

#define SDlist "/sdcard"  // mount point

sdmmc_card_t *card_host = NULL;

// Attempt to mount /sdcard. Idempotent: returns true immediately if already
// mounted. On a failed attempt (no card present) esp_vfs_fat_sdmmc_mount
// cleans up the SDMMC host before returning, so this is safe to call again
// later — that's what makes card hot-plug (insert-without-reboot) work.
bool sdcard_try_mount(void)
{
  if(card_host != NULL) return true;          // already mounted

  if (sdcard_even_ == NULL) sdcard_even_ = xEventGroupCreate();   // don't leak on re-init
  esp_vfs_fat_sdmmc_mount_config_t mount_config =
  {
    .format_if_mount_failed = false,       // never format the card
    .max_files = 5,                        // max open files
    .allocation_unit_size = 16 * 1024,     //cluster size; power-of-two (only used if formatting)
  };

  sdmmc_host_t host = SDMMC_HOST_DEFAULT();
  host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;

  sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
  slot_config.width = 1;           // 1-bit bus
  slot_config.clk = SDMMC_CLK_PIN;
  slot_config.cmd = SDMMC_CMD_PIN;
  slot_config.d0 = SDMMC_D0_PIN;

  esp_err_t err = esp_vfs_fat_sdmmc_mount(SDlist, &host, &slot_config, &mount_config, &card_host);
  if(err != ESP_OK)
  {
    card_host = NULL;            // mount only sets *out_card on success
    ESP_LOGD(TAG, "SD mount attempt failed: %s", esp_err_to_name(err));
    return false;
  }

  sdmmc_card_print_info(stdout, card_host);
  user_sdcard_bsp.sdcard_size = (float)(card_host->csd.capacity)/2048/1024; //G
  if(sdcard_even_) xEventGroupSetBits(sdcard_even_,0x01);
  return true;
}

bool sdcard_is_mounted(void)
{
  return card_host != NULL;
}

void _sdcard_init(void)
{
  if(!sdcard_try_mount())
    ESP_LOGW(TAG, "no SD card at boot; will auto-mount when one is inserted");
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