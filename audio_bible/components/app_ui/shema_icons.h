/* Generated Shema icons. LVGL 8; no external icon/font dependency. */
#pragma once
#include "lvgl.h"
typedef enum {
    SHEMA_ICON_BOOK,
    SHEMA_ICON_YEAR,
    SHEMA_ICON_FOLDER,
    SHEMA_ICON_HEART,
    SHEMA_ICON_BACK,
    SHEMA_ICON_NEXT,
    SHEMA_ICON_PLAY,
    SHEMA_ICON_PAUSE,
    SHEMA_ICON_PREV,
    SHEMA_ICON_SKIP,
    SHEMA_ICON_MORE,
    SHEMA_ICON_RECENT,
    SHEMA_ICON_SETTINGS,
    SHEMA_ICON_MOON,
    SHEMA_ICON_VOLUME,
    SHEMA_ICON_MINUS,
    SHEMA_ICON_PLUS,
    SHEMA_ICON_SHUFFLE,
    SHEMA_ICON_CHECK,
    SHEMA_ICON_SUN,
    SHEMA_ICON_POWER,
    SHEMA_ICON_TRASH,
    SHEMA_ICON_SD,
    SHEMA_ICON_BATTERY,
    SHEMA_ICON_WARNING,
    SHEMA_ICON_HOME,
    SHEMA_ICON_AUDIO,
    SHEMA_ICON_LIST,
    SHEMA_ICON_UP,
    SHEMA_ICON_DOWN,
    SHEMA_ICON_EDIT,
    SHEMA_ICON_REPEAT,
    SHEMA_ICON_PALETTE,
    SHEMA_ICON_HEART_FILLED,
    SHEMA_ICON_MOON_FILLED,
    SHEMA_ICON_COUNT
} shema_icon_id_t;
lv_obj_t *shema_icon_create(lv_obj_t *parent, shema_icon_id_t icon, unsigned size, lv_color_t color);
void shema_icon_set_color(lv_obj_t *icon, lv_color_t color);
void shema_icon_set_symbol(lv_obj_t *icon, shema_icon_id_t id, unsigned size);
