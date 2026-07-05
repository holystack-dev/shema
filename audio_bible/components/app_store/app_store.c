#include "app_store.h"
#include <string.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "bible_data.h"

static const char *TAG = "store";
static const char *NS = "bible";

static void pl_key(char *buf, int pl);   // defined below; used by the init validator

// A (book,chapter) is usable only if it maps to a real book and an in-range chapter.
// Every UI path indexes BIBLE_BOOKS[ref.book_idx], so a ref loaded from NVS that was
// written by a different firmware revision (or corrupted) must be rejected here rather
// than dereferenced as a wild pointer later (F-04).
static bool ref_valid(int book_idx, int chapter)
{
    return book_idx >= 0 && book_idx < BIBLE_BOOK_COUNT
        && chapter >= 1 && chapter <= BIBLE_BOOKS[book_idx].chapter_count;
}

// in-memory caches (write-through)
static track_ref_t fav[STORE_MAX_TRACKS];
static int         fav_n;

typedef struct {
    uint8_t count;
    char    names[STORE_MAX_PLAYLISTS][STORE_NAME_LEN];
} pl_meta_t;
static pl_meta_t pl_meta;

static nvs_handle_t open_rw(void)
{
    nvs_handle_t h = 0;
    esp_err_t e = nvs_open(NS, NVS_READWRITE, &h);
    if (e != ESP_OK) ESP_LOGW(TAG, "nvs_open failed: %s", esp_err_to_name(e));
    return h;
}

static int32_t get_i32(const char *k, int32_t def)
{
    nvs_handle_t h = open_rw();
    if (!h) return def;
    int32_t v = def;
    nvs_get_i32(h, k, &v);
    nvs_close(h);
    return v;
}
static void set_i32(const char *k, int32_t v)
{
    nvs_handle_t h = open_rw();
    if (!h) return;
    esp_err_t e = nvs_set_i32(h, k, v);
    if (e == ESP_OK) e = nvs_commit(h);
    if (e != ESP_OK) ESP_LOGW(TAG, "nvs set '%s' failed: %s", k, esp_err_to_name(e));
    nvs_close(h);
}

static void blob_load(const char *k, void *buf, size_t bufsz, size_t *out_len)
{
    *out_len = 0;
    nvs_handle_t h = open_rw();
    if (!h) return;
    size_t len = bufsz;
    if (nvs_get_blob(h, k, buf, &len) == ESP_OK) *out_len = len;
    nvs_close(h);
}
static void blob_save(const char *k, const void *buf, size_t len)
{
    nvs_handle_t h = open_rw();
    if (!h) return;
    esp_err_t e = nvs_set_blob(h, k, buf, len);
    if (e == ESP_OK) e = nvs_commit(h);
    if (e != ESP_OK) ESP_LOGW(TAG, "nvs blob '%s' (%u B) failed: %s",
                              k, (unsigned)len, esp_err_to_name(e));
    nvs_close(h);
}

void app_store_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    size_t len = 0;
    blob_load("fav", fav, sizeof(fav), &len);
    fav_n = (int)(len / sizeof(track_ref_t));
    if (fav_n > STORE_MAX_TRACKS) fav_n = STORE_MAX_TRACKS;
    // Drop any favourite that doesn't map to a real book/chapter, then persist the
    // cleaned list so a corrupt or foreign-firmware blob can never reach the UI's
    // BIBLE_BOOKS[] indexing (F-04).
    int fw = 0;
    for (int i = 0; i < fav_n; i++)
        if (ref_valid(fav[i].book_idx, fav[i].chapter)) fav[fw++] = fav[i];
    if (fw != fav_n) { fav_n = fw; blob_save("fav", fav, fav_n * sizeof(track_ref_t)); }

    blob_load("pl_meta", &pl_meta, sizeof(pl_meta), &len);
    if (len != sizeof(pl_meta)) { memset(&pl_meta, 0, sizeof(pl_meta)); }
    if (pl_meta.count > STORE_MAX_PLAYLISTS) pl_meta.count = 0;
    // Same sanitising for each playlist's track blob.
    for (int pl = 0; pl < pl_meta.count; pl++) {
        track_ref_t tmp[STORE_MAX_TRACKS]; size_t plen;
        char k[8]; pl_key(k, pl);
        blob_load(k, tmp, sizeof(tmp), &plen);
        int n = (int)(plen / sizeof(track_ref_t));
        if (n > STORE_MAX_TRACKS) n = STORE_MAX_TRACKS;
        int pw = 0;
        for (int i = 0; i < n; i++)
            if (ref_valid(tmp[i].book_idx, tmp[i].chapter)) tmp[pw++] = tmp[i];
        if (pw != n) blob_save(k, tmp, pw * sizeof(track_ref_t));
    }
    ESP_LOGI(TAG, "store ready: %d favourites, %d playlists", fav_n, pl_meta.count);
}

// --- settings ---
int  app_store_get_volume(int def)     { return get_i32("vol", def); }
void app_store_set_volume(int v)       { set_i32("vol", v); }
int  app_store_get_brightness(int def) { return get_i32("bright", def); }
void app_store_set_brightness(int v)   { set_i32("bright", v); }
bool app_store_get_repeat_all(bool def){ return get_i32("repeat", def) != 0; }
void app_store_set_repeat_all(bool v)  { set_i32("repeat", v ? 1 : 0); }
int  app_store_get_theme(int def)      { return get_i32("theme", def); }
void app_store_set_theme(int hex)      { set_i32("theme", hex); }
int  app_store_get_screen_timeout(int def) { return get_i32("scrto", def); }
void app_store_set_screen_timeout(int s)   { set_i32("scrto", s); }
int  app_store_get_poweroff(int def)   { return get_i32("pwroff", def); }
void app_store_set_poweroff(int s)     { set_i32("pwroff", s); }

void app_store_get_version(char *out, int out_sz, const char *def)
{
    if (out_sz <= 0) return;
    out[0] = 0;
    nvs_handle_t h = open_rw();
    size_t len = out_sz;
    if (!h || nvs_get_str(h, "ver", out, &len) != ESP_OK) {
        strncpy(out, def, out_sz - 1);
        out[out_sz - 1] = 0;
    }
    if (h) nvs_close(h);
}
void app_store_set_version(const char *path)
{
    nvs_handle_t h = open_rw();
    if (!h) return;
    nvs_set_str(h, "ver", path);
    esp_err_t e = nvs_commit(h);
    if (e != ESP_OK) ESP_LOGW(TAG, "nvs version save failed: %s", esp_err_to_name(e));
    nvs_close(h);
}

// --- resume ---
bool app_store_get_last(int *book_idx, int *chapter, uint32_t *pos_ms)
{
    int b = get_i32("last_b", -1);
    if (b < 0 || b >= BIBLE_BOOK_COUNT) return false;   // reject stale/foreign index (F-04)
    int c = get_i32("last_c", 1);
    int cc = BIBLE_BOOKS[b].chapter_count;
    if (c < 1) c = 1; else if (c > cc) c = cc;          // clamp chapter into range
    *book_idx = b;
    *chapter  = c;
    *pos_ms   = (uint32_t)get_i32("last_ms", 0);
    return true;
}
void app_store_set_last(int book_idx, int chapter, uint32_t pos_ms)
{
    nvs_handle_t h = open_rw();
    if (!h) return;
    nvs_set_i32(h, "last_b", book_idx);
    nvs_set_i32(h, "last_c", chapter);
    nvs_set_i32(h, "last_ms", (int32_t)pos_ms);
    esp_err_t e = nvs_commit(h);
    if (e != ESP_OK) ESP_LOGW(TAG, "nvs resume save failed: %s", esp_err_to_name(e));
    nvs_close(h);
}

// --- favourites ---
int  app_store_fav_count(void) { return fav_n; }
bool app_store_fav_get(int i, track_ref_t *out)
{
    if (i < 0 || i >= fav_n) return false;
    *out = fav[i];
    return true;
}
bool app_store_is_fav(int book_idx, int chapter)
{
    for (int i = 0; i < fav_n; i++)
        if (fav[i].book_idx == book_idx && fav[i].chapter == chapter) return true;
    return false;
}
void app_store_fav_toggle(int book_idx, int chapter)
{
    for (int i = 0; i < fav_n; i++) {
        if (fav[i].book_idx == book_idx && fav[i].chapter == chapter) {
            for (int j = i; j < fav_n - 1; j++) fav[j] = fav[j + 1];
            fav_n--;
            blob_save("fav", fav, fav_n * sizeof(track_ref_t));
            return;
        }
    }
    if (fav_n >= STORE_MAX_TRACKS) return;
    fav[fav_n].book_idx = book_idx;
    fav[fav_n].chapter = chapter;
    fav_n++;
    blob_save("fav", fav, fav_n * sizeof(track_ref_t));
}

// --- playlists ---
static void pl_key(char *buf, int pl) { snprintf(buf, 8, "pl%d", pl); }

int  app_store_pl_count(void) { return pl_meta.count; }
bool app_store_pl_name(int pl, char *out, int out_sz)
{
    if (pl < 0 || pl >= pl_meta.count) return false;
    snprintf(out, out_sz, "%s", pl_meta.names[pl]);
    return true;
}
int app_store_pl_create(const char *name)
{
    if (pl_meta.count >= STORE_MAX_PLAYLISTS) return -1;
    int idx = pl_meta.count;
    snprintf(pl_meta.names[idx], STORE_NAME_LEN, "%s", name);
    pl_meta.count++;
    blob_save("pl_meta", &pl_meta, sizeof(pl_meta));
    char k[8]; pl_key(k, idx);
    blob_save(k, NULL, 0);
    return idx;
}
void app_store_pl_delete(int pl)
{
    if (pl < 0 || pl >= pl_meta.count) return;
    char k[8];
    // shift playlist track blobs down
    for (int i = pl; i < pl_meta.count - 1; i++) {
        track_ref_t tmp[STORE_MAX_TRACKS]; size_t len;
        char ksrc[8]; pl_key(ksrc, i + 1);
        blob_load(ksrc, tmp, sizeof(tmp), &len);
        pl_key(k, i);
        blob_save(k, tmp, len);
        memcpy(pl_meta.names[i], pl_meta.names[i + 1], STORE_NAME_LEN);
    }
    pl_meta.count--;
    pl_key(k, pl_meta.count);
    blob_save(k, NULL, 0);
    blob_save("pl_meta", &pl_meta, sizeof(pl_meta));
}
int app_store_pl_track_count(int pl)
{
    if (pl < 0 || pl >= pl_meta.count) return 0;
    track_ref_t tmp[STORE_MAX_TRACKS]; size_t len;
    char k[8]; pl_key(k, pl);
    blob_load(k, tmp, sizeof(tmp), &len);
    return (int)(len / sizeof(track_ref_t));
}
bool app_store_pl_track_get(int pl, int i, track_ref_t *out)
{
    if (pl < 0 || pl >= pl_meta.count) return false;
    track_ref_t tmp[STORE_MAX_TRACKS]; size_t len;
    char k[8]; pl_key(k, pl);
    blob_load(k, tmp, sizeof(tmp), &len);
    int n = (int)(len / sizeof(track_ref_t));
    if (i < 0 || i >= n) return false;
    *out = tmp[i];
    return true;
}
void app_store_pl_add(int pl, int book_idx, int chapter)
{
    if (pl < 0 || pl >= pl_meta.count) return;
    track_ref_t tmp[STORE_MAX_TRACKS]; size_t len;
    char k[8]; pl_key(k, pl);
    blob_load(k, tmp, sizeof(tmp), &len);
    int n = (int)(len / sizeof(track_ref_t));
    if (n >= STORE_MAX_TRACKS) return;
    tmp[n].book_idx = book_idx;
    tmp[n].chapter = chapter;
    n++;
    blob_save(k, tmp, n * sizeof(track_ref_t));
}
void app_store_pl_remove(int pl, int i)
{
    if (pl < 0 || pl >= pl_meta.count) return;
    track_ref_t tmp[STORE_MAX_TRACKS]; size_t len;
    char k[8]; pl_key(k, pl);
    blob_load(k, tmp, sizeof(tmp), &len);
    int n = (int)(len / sizeof(track_ref_t));
    if (i < 0 || i >= n) return;
    for (int j = i; j < n - 1; j++) tmp[j] = tmp[j + 1];
    n--;
    blob_save(k, tmp, n * sizeof(track_ref_t));
}
