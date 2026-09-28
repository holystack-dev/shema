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
// than dereferenced as a wild pointer later.
static bool ref_valid(int book_idx, int chapter)
{
    return book_idx >= 0 && book_idx < BIBLE_BOOK_COUNT
        && chapter >= 1 && chapter <= BIBLE_BOOKS[book_idx].chapter_count;
}

// in-memory caches (write-through)
static track_ref_t fav[STORE_MAX_TRACKS];
static int         fav_n;

// Bible-in-a-Year favourites. Exactly one of day/intro is >= 0; the other is -1.
typedef struct { int16_t day, intro; } favbiy_t;
static favbiy_t favbiy[STORE_MAX_FAV_BIY];
static int      favbiy_n;

// Library favourites, keyed by folder rel-path + file name so they survive the card
// being re-sorted or added to (a bare index would drift onto another track).
typedef struct {
    char    dir[STORE_LIB_DIR_LEN];
    char    name[STORE_LIB_NAME_LEN];
    int16_t track;      // last known index within dir; re-resolved by name on play
    int16_t _pad;
} favlib_t;
static favlib_t favlib[STORE_MAX_FAV_LIB];
static int      favlib_n;

static void str_cpy(char *dst, int dstsz, const char *src)
{
    if (dstsz <= 0) return;
    snprintf(dst, (size_t)dstsz, "%s", src ? src : "");
}

typedef struct {
    uint8_t count;
    char    names[STORE_MAX_PLAYLISTS][STORE_NAME_LEN];
} pl_meta_t;
static pl_meta_t pl_meta;

// recent history (MRU). kind 0=BIBLE (a=book,b=chapter), 1=BIY (a=day,b=intro),
// 2=LIBRARY (b=track index; dir/name carry the identity).
// dir/name are stored inline because history reorders on every push.
typedef struct {
    int16_t  kind, a, b;
    uint32_t pos_ms;
    char     dir[STORE_LIB_DIR_LEN];
    char     name[STORE_LIB_NAME_LEN];
} hist_ref_t;
static hist_ref_t hist[STORE_MAX_HIST];
static int        hist_n;
// New key: the old "hist" blob holds 10-byte entries, and reading it as the wider
// struct would produce garbage rows. Old history is simply dropped once.
#define HIST_KEY "hist2"

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
    // BIBLE_BOOKS[] indexing.
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
    // BIY favourites: keep only entries that name exactly one of day/intro, so a
    // corrupt blob can't reach app_player_biy_day_title() with a wild index.
    blob_load("favbiy", favbiy, sizeof(favbiy), &len);
    favbiy_n = (int)(len / sizeof(favbiy_t));
    if (favbiy_n > STORE_MAX_FAV_BIY) favbiy_n = STORE_MAX_FAV_BIY;
    int yw = 0;
    for (int i = 0; i < favbiy_n; i++) {
        bool day_ok   = favbiy[i].day >= 1 && favbiy[i].day <= 365 && favbiy[i].intro < 0;
        bool intro_ok = favbiy[i].intro >= 0 && favbiy[i].day < 0;
        if (day_ok || intro_ok) favbiy[yw++] = favbiy[i];
    }
    if (yw != favbiy_n) { favbiy_n = yw; blob_save("favbiy", favbiy, (size_t)favbiy_n * sizeof(favbiy_t)); }

    // Library favourites: drop unterminated / empty-name rows before the UI sees them.
    blob_load("favlib", favlib, sizeof(favlib), &len);
    favlib_n = (int)(len / sizeof(favlib_t));
    if (favlib_n > STORE_MAX_FAV_LIB) favlib_n = STORE_MAX_FAV_LIB;
    int lw = 0;
    for (int i = 0; i < favlib_n; i++) {
        favlib[i].dir[STORE_LIB_DIR_LEN - 1] = 0;
        favlib[i].name[STORE_LIB_NAME_LEN - 1] = 0;
        if (favlib[i].name[0]) favlib[lw++] = favlib[i];
    }
    if (lw != favlib_n) { favlib_n = lw; blob_save("favlib", favlib, (size_t)favlib_n * sizeof(favlib_t)); }

    blob_load(HIST_KEY, hist, sizeof(hist), &len);
    hist_n = (int)(len / sizeof(hist_ref_t));
    if (hist_n > STORE_MAX_HIST) hist_n = STORE_MAX_HIST;
    for (int i = 0; i < hist_n; i++) {          // same termination guard for the inline strings
        hist[i].dir[STORE_LIB_DIR_LEN - 1] = 0;
        hist[i].name[STORE_LIB_NAME_LEN - 1] = 0;
    }
    ESP_LOGI(TAG, "store ready: %d/%d/%d favourites (bible/biy/library), %d playlists, %d recent",
             fav_n, favbiy_n, favlib_n, pl_meta.count, hist_n);
}

// --- recent history (MRU) ---
int app_store_hist_count(void) { return hist_n; }
bool app_store_hist_get(int i, int *kind, int *a, int *b, uint32_t *pos_ms)
{
    if (i < 0 || i >= hist_n) return false;
    *kind = hist[i].kind; *a = hist[i].a; *b = hist[i].b; *pos_ms = hist[i].pos_ms;
    return true;
}
bool app_store_hist_get_lib(int i, char *dir, int dir_sz, char *name, int name_sz, int *track)
{
    if (i < 0 || i >= hist_n || hist[i].kind != 2) return false;
    if (dir)   str_cpy(dir, dir_sz, hist[i].dir);
    if (name)  str_cpy(name, name_sz, hist[i].name);
    if (track) *track = hist[i].b;
    return true;
}

// Insert `e` at the front, dropping any existing entry that matches it per `same`.
// Position-only updates to the front entry stay in RAM (no flash write); the last*
// resume keys restore position after power-off.
static void hist_insert(const hist_ref_t *e, bool (*same)(const hist_ref_t *, const hist_ref_t *))
{
    if (hist_n > 0 && same(&hist[0], e)) {
        hist[0].pos_ms = e->pos_ms;
        return;                                  // already at the front: no reorder, no write
    }
    int w = 0;                                   // drop any existing entry for this item
    for (int r = 0; r < hist_n; r++)
        if (!same(&hist[r], e)) hist[w++] = hist[r];
    hist_n = w;
    if (hist_n > STORE_MAX_HIST - 1) hist_n = STORE_MAX_HIST - 1;
    for (int r = hist_n; r > 0; r--) hist[r] = hist[r - 1];   // shift down, insert at front
    hist[0] = *e;
    hist_n++;
    blob_save(HIST_KEY, hist, (size_t)hist_n * sizeof(hist_ref_t));
}

static bool hist_same_ref(const hist_ref_t *x, const hist_ref_t *y)
{
    return x->kind == y->kind && x->a == y->a && x->b == y->b;
}
static bool hist_same_lib(const hist_ref_t *x, const hist_ref_t *y)
{
    return x->kind == 2 && y->kind == 2
        && strcmp(x->dir, y->dir) == 0 && strcmp(x->name, y->name) == 0;
}

void app_store_hist_push(int kind, int a, int b, uint32_t pos_ms)
{
    hist_ref_t e = { (int16_t)kind, (int16_t)a, (int16_t)b, pos_ms, {0}, {0} };
    hist_insert(&e, hist_same_ref);
}

void app_store_hist_push_lib(const char *dir, const char *name, int track, uint32_t pos_ms)
{
    hist_ref_t e = { 2, 0, (int16_t)track, pos_ms, {0}, {0} };
    str_cpy(e.dir, sizeof e.dir, dir);
    str_cpy(e.name, sizeof e.name, name);
    hist_insert(&e, hist_same_lib);
}

// --- settings ---
int  app_store_get_volume(int def)     { return get_i32("vol", def); }
void app_store_set_volume(int v)       { set_i32("vol", v); }
int  app_store_get_brightness(int def) { return get_i32("bright", def); }
void app_store_set_brightness(int v)   { set_i32("bright", v); }
bool app_store_get_repeat_all(bool def){ return get_i32("repeat", def) != 0; }
void app_store_set_repeat_all(bool v)  { set_i32("repeat", v ? 1 : 0); }
bool app_store_get_swipe_back(bool def){ return get_i32("swipeb", def) != 0; }
void app_store_set_swipe_back(bool v)  { set_i32("swipeb", v ? 1 : 0); }
int  app_store_get_theme(int def)      { return get_i32("theme", def); }
void app_store_set_theme(int hex)      { set_i32("theme", hex); }
int  app_store_get_screen_timeout(int def) { return get_i32("scrto", def); }
void app_store_set_screen_timeout(int s)   { set_i32("scrto", s); }
int  app_store_get_poweroff(int def)   { return get_i32("pwroff", def); }
void app_store_set_poweroff(int s)     { set_i32("pwroff", s); }

// --- audio version (BIBLE/<LANG>/<VERSION> path) ---
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

// --- Bible-in-a-Year language: SD folder name of the chosen BIY/<lang> ---
void app_store_get_biy_lang(char *out, int out_sz, const char *def)
{
    if (out_sz <= 0) return;
    out[0] = 0;
    nvs_handle_t h = open_rw();
    size_t len = out_sz;
    if (!h || nvs_get_str(h, "biylang", out, &len) != ESP_OK) {
        strncpy(out, def, out_sz - 1);
        out[out_sz - 1] = 0;
    }
    if (h) nvs_close(h);
}
void app_store_set_biy_lang(const char *name)
{
    nvs_handle_t h = open_rw();
    if (!h) return;
    nvs_set_str(h, "biylang", name ? name : "");
    esp_err_t e = nvs_commit(h);
    if (e != ESP_OK) ESP_LOGW(TAG, "nvs biy lang save failed: %s", esp_err_to_name(e));
    nvs_close(h);
}

// --- resume (last played) ---
// last_k discriminates the collection: 0 = BIBLE (last_b=book_idx, last_c=chapter),
// 1 = BIY (last_b=day, last_c=intro), 2 = LIBRARY (lib_dir/lib_trk strings,
// last_c = index). Saves without last_k are treated as BIBLE.
int app_store_get_last_kind(void)
{
    int k = get_i32("last_k", -1);
    if (k < 0) return (get_i32("last_b", -1) >= 0) ? 0 : -1;
    return k;
}
bool app_store_get_last(int *book_idx, int *chapter, uint32_t *pos_ms)
{
    if (app_store_get_last_kind() != 0) return false;
    int b = get_i32("last_b", -1);
    if (b < 0 || b >= BIBLE_BOOK_COUNT) return false;   // reject stale/foreign index
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
    nvs_set_i32(h, "last_k", 0);
    nvs_set_i32(h, "last_b", book_idx);
    nvs_set_i32(h, "last_c", chapter);
    nvs_set_i32(h, "last_ms", (int32_t)pos_ms);
    esp_err_t e = nvs_commit(h);
    if (e != ESP_OK) ESP_LOGW(TAG, "nvs resume save failed: %s", esp_err_to_name(e));
    nvs_close(h);
}
bool app_store_get_last_biy(int *day, int *intro, uint32_t *pos_ms)
{
    if (app_store_get_last_kind() != 1) return false;
    *day    = get_i32("last_b", -1);
    *intro  = get_i32("last_c", -1);
    *pos_ms = (uint32_t)get_i32("last_ms", 0);
    return true;
}
void app_store_set_last_biy(int day, int intro, uint32_t pos_ms)
{
    nvs_handle_t h = open_rw();
    if (!h) return;
    nvs_set_i32(h, "last_k", 1);
    nvs_set_i32(h, "last_b", day);
    nvs_set_i32(h, "last_c", intro);
    nvs_set_i32(h, "last_ms", (int32_t)pos_ms);
    nvs_commit(h);
    nvs_close(h);
}
void app_store_set_last_lib(const char *dir, int idx, const char *name, uint32_t pos_ms)
{
    nvs_handle_t h = open_rw();
    if (!h) return;
    nvs_set_i32(h, "last_k", 2);
    nvs_set_str(h, "lib_dir", dir ? dir : "");
    nvs_set_i32(h, "last_c", idx);
    nvs_set_str(h, "lib_trk", name ? name : "");
    nvs_set_i32(h, "last_ms", (int32_t)pos_ms);
    nvs_commit(h);
    nvs_close(h);
}
// dir/name are optional (pass NULL to skip). Returns false unless a LIBRARY track is saved.
bool app_store_get_last_lib(char *dir, int dir_sz, int *idx, char *name, int name_sz, uint32_t *pos_ms)
{
    if (app_store_get_last_kind() != 2) return false;
    nvs_handle_t h = open_rw();
    if (dir && dir_sz > 0) {
        dir[0] = 0; size_t len = dir_sz;
        if (!h || nvs_get_str(h, "lib_dir", dir, &len) != ESP_OK) dir[0] = 0;
    }
    if (name && name_sz > 0) {
        name[0] = 0; size_t len = name_sz;
        if (h) nvs_get_str(h, "lib_trk", name, &len);
    }
    if (h) nvs_close(h);
    if (idx)    *idx    = get_i32("last_c", -1);
    if (pos_ms) *pos_ms = (uint32_t)get_i32("last_ms", 0);
    return true;
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

// --- favourites: Bible in a Year ---
// A BIY item is either a numbered day or an intro/checkpoint, never both, so the pair
// (day,intro) with the unused half set to -1 identifies it uniquely.
int app_store_favbiy_count(void) { return favbiy_n; }
bool app_store_favbiy_get(int i, int *day, int *intro)
{
    if (i < 0 || i >= favbiy_n) return false;
    if (day)   *day   = favbiy[i].day;
    if (intro) *intro = favbiy[i].intro;
    return true;
}
static int favbiy_find(int day, int intro)
{
    for (int i = 0; i < favbiy_n; i++)
        if (favbiy[i].day == day && favbiy[i].intro == intro) return i;
    return -1;
}
bool app_store_is_favbiy(int day, int intro) { return favbiy_find(day, intro) >= 0; }
void app_store_favbiy_remove(int i)
{
    if (i < 0 || i >= favbiy_n) return;
    for (int j = i; j < favbiy_n - 1; j++) favbiy[j] = favbiy[j + 1];
    favbiy_n--;
    blob_save("favbiy", favbiy, (size_t)favbiy_n * sizeof(favbiy_t));
}
void app_store_favbiy_toggle(int day, int intro)
{
    int at = favbiy_find(day, intro);
    if (at >= 0) { app_store_favbiy_remove(at); return; }
    if (favbiy_n >= STORE_MAX_FAV_BIY) return;
    favbiy[favbiy_n].day   = (int16_t)day;
    favbiy[favbiy_n].intro = (int16_t)intro;
    favbiy_n++;
    blob_save("favbiy", favbiy, (size_t)favbiy_n * sizeof(favbiy_t));
}

// --- favourites: Library ---
int app_store_favlib_count(void) { return favlib_n; }
bool app_store_favlib_get(int i, char *dir, int dir_sz, char *name, int name_sz, int *track)
{
    if (i < 0 || i >= favlib_n) return false;
    if (dir)   str_cpy(dir, dir_sz, favlib[i].dir);
    if (name)  str_cpy(name, name_sz, favlib[i].name);
    if (track) *track = favlib[i].track;
    return true;
}
static int favlib_find(const char *dir, const char *name)
{
    if (!dir || !name) return -1;
    for (int i = 0; i < favlib_n; i++)
        if (strcmp(favlib[i].dir, dir) == 0 && strcmp(favlib[i].name, name) == 0) return i;
    return -1;
}
bool app_store_is_favlib(const char *dir, const char *name) { return favlib_find(dir, name) >= 0; }
void app_store_favlib_remove(int i)
{
    if (i < 0 || i >= favlib_n) return;
    for (int j = i; j < favlib_n - 1; j++) favlib[j] = favlib[j + 1];
    favlib_n--;
    blob_save("favlib", favlib, (size_t)favlib_n * sizeof(favlib_t));
}
void app_store_favlib_toggle(const char *dir, const char *name, int track)
{
    if (!dir || !name || !name[0]) return;
    int at = favlib_find(dir, name);
    if (at >= 0) { app_store_favlib_remove(at); return; }
    if (favlib_n >= STORE_MAX_FAV_LIB) return;
    memset(&favlib[favlib_n], 0, sizeof(favlib_t));
    str_cpy(favlib[favlib_n].dir, STORE_LIB_DIR_LEN, dir);
    str_cpy(favlib[favlib_n].name, STORE_LIB_NAME_LEN, name);
    favlib[favlib_n].track = (int16_t)track;
    favlib_n++;
    blob_save("favlib", favlib, (size_t)favlib_n * sizeof(favlib_t));
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
