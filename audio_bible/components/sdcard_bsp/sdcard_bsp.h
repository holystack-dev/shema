#ifndef SDCARD_BSP_H
#define SDCARD_BSP_H
#include <stdbool.h>
#include "driver/sdmmc_host.h"


typedef struct
{
  float sdcard_size;
}sdcard_bsp_t;

extern sdcard_bsp_t user_sdcard_bsp;
extern EventGroupHandle_t sdcard_even_;

#ifdef __cplusplus
extern "C" {
#endif


void _sdcard_init(void);          // mount once at boot (no-op retry-safe)
bool sdcard_try_mount(void);      // attempt to mount; true if mounted (idempotent). Safe to poll for hot-plug.
bool sdcard_is_mounted(void);     // true once a card is mounted at /sdcard
void sdcard_unmount(void);        // flush + unmount the FAT volume (call before deep sleep)
esp_err_t sdcard_file_write(const char *path, const char *data);
esp_err_t sdcard_file_read(const char *path, char *buffer, size_t bufsz, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif