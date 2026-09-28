// app_ui — round-native LVGL UI for the Audio Bible (360x360 circular screen).
//
// The "Shema" design language (design/shema-v2): a near-black ground, champagne accent,
// soft-cornered cards, and original vector icons rendered as anti-aliased images (shema_icons.c)
// rather than a glyph font or bitmaps. Colours and geometry come from shema_theme.h so
// the firmware and the design pack cannot drift apart.
//
//  - Now Playing is a "watch face": a full-rim progress arc, centred title/context,
//    a scrub bar flanked by elapsed/total, five transport controls on the widest band,
//    and volume with a numeric readout below them.
//  - Navigation is by SWIPE (swipe right = back, optional) plus a back button centred in
//    the BOTTOM rim, where the round bezel has room and the title keeps full width. The
//    BOOT button goes Home.
//  - Everything is centre-anchored and kept inside the inscribed circle (SAFE_X), where
//    the glass is widest; modal cards derive their own height cap from that circle.
//  - Lists scroll and carry a primary line plus 14px context metadata; chapters are a
//    grid of tappable circles coloured by playing / finished / unheard.
//
// This is the view layer; playback, storage and power live in their own components.
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <dirent.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_app_desc.h"
#include "bsp_lvgl.h"
#include "bible_data.h"
#include "biy_data.h"
#include "app_player.h"
#include "app_store.h"
#include "app_played.h"
#include "app_power.h"
#include "sdcard_bsp.h"
#include "shema_theme.h"
#include "shema_icons.h"

static const char *TAG = "ui";
static bool tap_is_clean(lv_event_t *e);

#if BIBLE_SCREENSHOTS
// Transient visual fixtures for branches unavailable with the inserted card.
// They never change player/store data and are labelled as previews in the gallery.
static bool capture_no_card, capture_empty_lists, capture_static_frame, capture_no_animation;
#else
#define capture_no_card false
#define capture_empty_lists false
#define capture_no_animation false
#endif

// ---- palette (Shema design tokens) ----
// The named colours come from shema_theme.h so the firmware and the design pack cannot
// drift apart; COL_* are local aliases for those tokens.
#define COL_BG     SHEMA_BG              // 0x080D13 page background
#define COL_CARD   SHEMA_CARD            // 0x17212B resting card / list row
#define COL_CARD2  SHEMA_CARD_RAISED     // 0x23303C raised control inside a card
#define COL_TRACK  SHEMA_TRACK           // 0x33404A arc/slider track
// Default accent (champagne). A stored accent from app_store_get_theme() takes precedence.
#define ACCENT_DEFAULT SHEMA_ACCENT_DEFAULT   // 0xE8BE78
#define COL_TEXT   SHEMA_TEXT            // 0xF7F1E6, ~14:1 on COL_CARD
// Secondary text: ~8.5:1 contrast on COL_CARD, clearly subordinate to COL_TEXT.
#define COL_SUB    SHEMA_TEXT_SECONDARY  // 0xAFBDC7
// Already listened to: a dim desaturated green that reads as "done" without competing
// with the accent. DONE fills a finished chapter circle; DONE_T is the text/tick on it
// and on finished list rows.
#define COL_DONE   0x2B4A3C
#define COL_DONE_T SHEMA_DONE            // 0xA9CBB2
#define COL_DANGER SHEMA_DANGER          // 0xD9534F — destructive actions, empty battery

// ---- round geometry ----
// Round 360px screen, radius 180, centre (180,180). Usable half-width at a
// vertical offset dy from centre is sqrt(180^2 - dy^2). These bands are chosen so
// a SAFE_X-inset content rectangle (288 wide) and the chrome stay inside the glass:
//   battery y10 | title y48 | content 78..264 (94..264 with a back button) | pill
#define SAFE_X       36          // content inset; inner width 360-72 = 288
#define TITLE_Y      48          // screen title y (clear of the top battery)
#define CONTENT_TOP_PLAIN  78    // content start (no back button)
#define CONTENT_TOP_BACK   94    // content start when a back button is shown
#define BOTTOM_TOUCH_W 80
#define BACK_TOUCH_W  128       // 24px farther on each side; same vertical bounds
#define BOTTOM_TOUCH_H 64
#define BOTTOM_TOUCH_Y 290       // fixed top edge; keep clear of content above
#define MINIBAR_TEXT_H 40
#define MINIBAR_TEXT_Y (BOTTOM_TOUCH_Y - MINIBAR_TEXT_H)
// Now-playing bar is Home-only, so non-Home pages reclaim the bottom. 280 keeps
// full-width content inside the round bezel (half-width ~150 >= 144 needed at y280).
#define CONTENT_BOT  280
// Keep the existing tile-grid height, then two metadata lines above the footer.
#define HOME_BOT     (MINIBAR_TEXT_Y - 4)
// Scrolling grids (Books, Chapters, Settings) have no now-playing bar, so they run
// down toward the footer, stopping before its full touch rectangle.
#define GRID_BOT     BOTTOM_TOUCH_Y
// Lists reserve this much bottom padding so the last row can scroll clear of the
// container edge; a fade of the same height sits over it.
#define LIST_FADE_H  28
// LVGL only searches an object's children when the touch point is inside that object's
// own coords (lv_indev_search_obj), so an ext_click_area is truncated wherever it spills
// past its parent. Layout boxes are sized to contain their children's touch areas.
//
// Tap targets: TAP_PREF where the geometry allows; TAP_COMPACT is the floor for controls
// that share a row. Controls drawn smaller than their touch area use ext_click_area.
#define TAP_PREF     SHEMA_TOUCH_PREFERRED   // 64
#define TAP_COMPACT  SHEMA_TOUCH_COMPACT     // 56
#define GAP          SHEMA_TOUCH_GAP         // 8 — default spacing between controls

// ---- Now Playing title ----
// The title sits in a fixed band above the time row, so its line count is bounded.
#define PLAYER_TITLE_W     224
#define PLAYER_TITLE_LINES 2
// Transport row: sleep · prev · play · next · fav, laid out SPACE_BETWEEN inside
// PLAYER_TRANS_W. Sizes are the DRAWN diameters; the two outer buttons and prev/next
// reach their touch size through ext_click_area (see circ_btn). The widths are chosen
// so every hit rectangle stays inside both the 180px glass radius and the rim arc, and
// so no two hit rectangles overlap:
//   drawn   48 + 56 + 72 + 56 + 48 = 280, four 6px gaps -> 304
//   touched 56 + 60 + 72 + 60 + 56, gaps fully consumed but not crossed
#define PLAYER_TRANS_W  304
#define PLAYER_TRANS_H  76
#define PLAYER_TRANS_Y  25       // offset from the screen centre
#define PLAYER_BTN_SIDE 48       // sleep / favourite
#define PLAYER_BTN_SKIP 56       // prev / next
#define PLAYER_BTN_PLAY 72       // play / pause

enum { PAGE_HOME, PAGE_BOOKS, PAGE_CHAPTERS, PAGE_PLAYER, PAGE_LIST_DETAIL, PAGE_SETTINGS,
       PAGE_BIY, PAGE_BIY_DAYS, PAGE_RECENT, PAGE_LIB, PAGE_FAVOURITES };
// Favourites are three separate collections because the three content types are keyed
// differently (book/chapter, day/intro, folder+file). PAGE_FAVOURITES is the hub; each
// layer then reuses the list-detail page via one of these ctx values.
#define LIST_FAV       (-1)   // Bible favourites
#define LIST_FAV_BIY   (-3)   // Bible-in-a-Year favourites
#define LIST_FAV_LIB   (-4)   // Library favourites
#define IS_FAV_LIST(w) ((w) == LIST_FAV || (w) == LIST_FAV_BIY || (w) == LIST_FAV_LIB)
#define BIY_CTX_INTROS (-2)   // ctx for PAGE_BIY_DAYS = the "Intros & Checkpoints" list

typedef struct { int page; int ctx; } nav_entry_t;
static nav_entry_t nav_stack[10];
#define NAV_MAX ((int)(sizeof(nav_stack) / sizeof(nav_stack[0])))
static int nav_sp;
static int cur_page;
static lv_scr_load_anim_t g_anim = LV_SCR_LOAD_ANIM_NONE;
static bool startup_transition;
#define NAV_TRANSITION_MS 100
#define STARTUP_TRANSITION_MS 360

// player-page widgets (valid only while cur_page == PAGE_PLAYER)
static lv_obj_t *pw_title, *pw_sub, *pw_arc, *pw_seek, *pw_cur, *pw_tot, *pw_vol;
// Play/pause are two stacked icons in the same button (pw_playbtn); state changes
// toggle their visibility.
static lv_obj_t *pw_playbtn, *pw_ic_play, *pw_ic_pause;
static lv_obj_t *pw_favbtn, *pw_fav;      // favourite: button (tint) + heart icon (colour)
static lv_obj_t *pw_sleepbtn, *pw_sleep;  // sleep timer: button + moon icon
static lv_obj_t *pw_volpct;   // numeric volume, right of the slider
// books page
static lv_obj_t *bk_list, *bk_ot, *bk_nt;
static int       g_testament = BIBLE_OLD;
static uint32_t  g_accent_hex = ACCENT_DEFAULT;
static bool      detail_edit;
static int       detail_which;
static int       g_version_idx;   // selected index into the app_player version list
static int       g_biy_lang;       // Bible-in-a-Year: selected language index into the scanned BIY/<lang> list
// top-layer chrome. mb_sub is the now-playing bar's second line (context + times);
// mb_ic_play / mb_ic_pause are the same stacked-icon trick as the player's button.
static lv_obj_t *sb_batt, *mb_box, *mb_title, *mb_sub, *mb_prog, *mb_play;
static lv_obj_t *mb_ic_play, *mb_ic_pause;
static lv_obj_t *modal;
static lv_obj_t *pg_fade;   // bottom fade of the current page (owned by the screen)
static int       pg_ctop;   // content-area top y of the current page (see page_content_bottom)

static uint32_t sleep_deadline_ms;
static bool     sleep_active;        // separate flag: a deadline of 0 is a valid time
static int      sleep_minutes;       // last chosen duration, for the dialog's selected row
static uint32_t last_save_ms;
static volatile bool go_home_req;
static volatile bool sd_ready_req;   // set by the SD-monitor task when a card is hot-plugged

static void build_page(int page, int ctx);
static void build_list_detail(int which);
static void make_top_bars(void);
static void apply_accent(uint32_t hex);
static void minibar_play_cb(lv_event_t *e);

// ---- ui_tick change-detection ----
// lv_label_set_text always invalidates in LVGL 8, and with full_refresh=1 any
// invalidation forces a full 360x360 render + chunked flush. ui_tick runs at 2.5 Hz,
// so re-setting unchanged text/colour/visibility would repaint the whole screen forever
// (even all night behind an "off" panel). Cache the last applied value and only touch a
// widget when it actually changes. Caches are cleared on every page (re)build and on the
// screen-off -> on edge, since those recreate or repaint the widgets.
static char lc_batt[24], lc_mbtitle[64], lc_mbsub[64];
// Sized to match status_title()'s title[80]/sub[80] buffers so truncated titles still
// compare equal to their cache.
static char lc_pwtitle[80], lc_pwsub[80], lc_pwcur[16], lc_pwtot[16];
// Icon state caches; -1 = not applied yet (reset on every page rebuild).
static int  lc_pwplaying = -1, lc_mbplaying = -1;
static int  lc_fav = -1, lc_sleep = -1, lc_mbvis = -1, lc_rimvis = -1;
static int  lc_mbacc = -1, lc_pwacc = -1, lc_favvis = -1; // BIY: arc/sub colour + Fav-button visibility caches
static int  lc_battlow = -1;                              // battery gauge colour (normal / near-empty)
static void reset_label_caches(void)
{
    lc_batt[0] = lc_mbtitle[0] = lc_mbsub[0] = 0;
    lc_pwtitle[0] = lc_pwsub[0] = lc_pwcur[0] = lc_pwtot[0] = 0;
    lc_pwplaying = lc_mbplaying = -1;
    lc_fav = lc_sleep = lc_mbvis = lc_rimvis = -1;
    lc_mbacc = lc_pwacc = lc_favvis = -1;
    // apply_accent() rebuilds sb_batt at COL_TEXT, so reset its colour cache too or the
    // near-empty warning colour is not re-applied.
    lc_battlow = -1;
}
static void label_set(lv_obj_t *o, char *cache, size_t cachesz, const char *s)
{
    if (strcmp(cache, s) != 0) { snprintf(cache, cachesz, "%s", s); lv_label_set_text(o, s); }
}

// ---------- small helpers ----------
static void encode_play(lv_obj_t *o, int b, int c) { lv_obj_set_user_data(o, (void *)(intptr_t)(b * 1000 + c)); }
static void decode_play(lv_obj_t *o, int *b, int *c) { intptr_t v = (intptr_t)lv_obj_get_user_data(o); *b = v / 1000; *c = v % 1000; }

// An invisible layout container.
//
// Left clickable (the lv_obj_create default): LVGL starts a scroll from the hit-test
// object and walks up for a scrollable ancestor, so non-clickable boxes would block
// scrolling. Overlapping reaches resolve by creation order: later children win.
static lv_obj_t *flat(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}
static lv_obj_t *lbl(lv_obj_t *p, const char *t, const lv_font_t *f, uint32_t col)
{
    lv_obj_t *l = lv_label_create(p);
    lv_label_set_text(l, t);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(col), 0);
    return l;
}
static void card(lv_obj_t *o)
{
    static lv_style_t style;
    static bool ready;
    if (!ready) {
        ready = true;
        lv_style_init(&style);
        lv_style_set_bg_color(&style, lv_color_hex(COL_CARD));
        lv_style_set_bg_opa(&style, LV_OPA_COVER);
        lv_style_set_border_width(&style, 0);
        lv_style_set_radius(&style, SHEMA_CARD_RADIUS);
    }
    lv_obj_add_style(o, &style, 0);
}

// ---------- Shema icon artwork ----------
// shema_icon_create() returns an lv_img holding an anti-aliased alpha mask, not a
// label: recolour with shema_icon_set_color(), and never assume lv_obj_get_child(btn, 0)
// is a label.
//
// The artwork sizes are 24 / 28 / 32 only (anything else silently falls back to 24).
// Tiles, controls and lists share the same family. Each icon is one image object
// backed by a static flash mask, so long lists retain their bounded memory cost.
static lv_obj_t *icon(lv_obj_t *p, shema_icon_id_t id, unsigned size, uint32_t col)
{
    return shema_icon_create(p, id, size, lv_color_hex(col));
}
// Same, centred in its parent — the usual case for an icon inside a round button.
static lv_obj_t *icon_mid(lv_obj_t *p, shema_icon_id_t id, unsigned size, uint32_t col)
{
    lv_obj_t *o = icon(p, id, size, col);
    if (o) lv_obj_center(o);
    return o;
}
static void fmt_time(char *b, int sz, uint32_t ms)
{
    uint32_t s = ms / 1000;
    snprintf(b, sz, "%lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
}

// Case-insensitive substring (montserrat has no strcasestr; titles are ASCII).
static bool ci_strstr(const char *h, const char *n)
{
    if (!n[0]) return false;
    for (; *h; h++) {
        const char *a = h, *b = n;
        while (*a && *b && tolower((unsigned char)*a) == tolower((unsigned char)*b)) { a++; b++; }
        if (!*b) return true;
    }
    return false;
}
// "Genesis 12-13 | Job 1-2 | Proverbs 1:1-7" for a BIY day (ASCII '|' separator).
static void biy_readings_str(int day, char *buf, int sz)
{
    const biy_reading_t *r = biy_reading(day);
    if (!r) { buf[0] = 0; return; }
    if (r->second && r->second[0]) snprintf(buf, sz, "%s  |  %s  |  %s", r->first, r->second, r->psalm);
    else                           snprintf(buf, sz, "%s  |  %s", r->first, r->psalm);
}
// One-line label for the mini-bar (Bible chapter / BIY day or intro).
static void status_line(const player_status_t *st, char *buf, int sz)
{
    if (st->kind == TRACK_BIY) {
        if (st->biy_intro >= 0) snprintf(buf, sz, "%s", app_player_biy_intro_title(st->biy_intro));
        else snprintf(buf, sz, "Day %d  %s", st->biy_day, app_player_biy_day_title(st->biy_day));
    } else if (st->kind == TRACK_LIB) {
        snprintf(buf, sz, "%s", app_player_lib_playing_name());
    } else if (st->book_idx >= 0) {
        snprintf(buf, sz, "%s %d", BIBLE_BOOKS[st->book_idx].name, st->chapter);
    } else buf[0] = 0;
}
// Big title + subtitle for the Now Playing watch face.
static void status_title(const player_status_t *st, char *title, int tsz, char *sub, int ssz)
{
    if (st->kind == TRACK_BIY) {
        if (st->biy_intro >= 0) { snprintf(title, tsz, "%s", app_player_biy_intro_title(st->biy_intro)); snprintf(sub, ssz, "Bible in a Year"); }
        else {
            const char *t = app_player_biy_day_title(st->biy_day);
            snprintf(title, tsz, "Day %d%s%s", st->biy_day, t[0] ? "  " : "", t);
            int per = (st->biy_day >= 1) ? biy_period_of(st->biy_day) : -1;
            snprintf(sub, ssz, "%s", per >= 0 ? BIY_PERIODS[per].name : "Bible in a Year");
        }
    } else if (st->kind == TRACK_LIB) {
        snprintf(title, tsz, "%s", app_player_lib_playing_name());
        // Shuffle is shown in the subtitle; the transport row has no room for a sixth button.
        if (app_player_lib_shuffling())
            snprintf(sub, ssz, LV_SYMBOL_SHUFFLE "  %s", app_player_lib_playing_folder());
        else
            snprintf(sub, ssz, "%s", app_player_lib_playing_folder());
    } else if (st->book_idx >= 0) {
        snprintf(title, tsz, "%s", BIBLE_BOOKS[st->book_idx].name);
        snprintf(sub, ssz, "Chapter %d", st->chapter);
    } else { title[0] = 0; sub[0] = 0; }
}
// Accent colour for the now-playing arc: the BIY period colour, else the theme accent.
static uint32_t status_accent(const player_status_t *st)
{
    if (st->kind == TRACK_BIY && st->biy_day >= 1) {
        int per = biy_period_of(st->biy_day);
        if (per >= 0) return BIY_PERIODS[per].color;
    }
    return g_accent_hex;
}

// ---------- theme: strip widget shadows ----------
// The default theme gives every lv_btn a shadow, and with LV_SHADOW_CACHE_SIZE=0 each is
// re-rendered every frame. Remove them globally via the theme's apply callback, using one
// shared style so no per-object local style is allocated.
static lv_style_t         st_noshadow, st_pressed;
static lv_color_filter_dsc_t pressed_filter;
static lv_style_transition_dsc_t pressed_transition;
static lv_theme_apply_cb_t base_apply;
static lv_color_t pressed_color(const lv_color_filter_dsc_t *filter, lv_color_t color, lv_opa_t opa)
{
    (void)filter;
    return lv_color_lighten(color, opa);
}
static void theme_apply_flat(lv_theme_t *th, lv_obj_t *obj)
{
    if (base_apply) base_apply(th, obj);
    lv_obj_add_style(obj, &st_noshadow, 0);
    if (lv_obj_has_class(obj, &lv_btn_class))
        lv_obj_add_style(obj, &st_pressed, LV_STATE_PRESSED);
}
static void install_flat_theme(lv_theme_t *th)
{
    static bool style_ready;
    if (!style_ready) {
        style_ready = true;
        lv_style_init(&st_noshadow);
        lv_style_set_shadow_width(&st_noshadow, 0);
        // Shared lighten-on-press highlight: every tile, row and action gives the same
        // immediate tap response.
        lv_color_filter_dsc_init(&pressed_filter, pressed_color);
        lv_style_init(&st_pressed);
        lv_style_set_color_filter_dsc(&st_pressed, &pressed_filter);
        lv_style_set_color_filter_opa(&st_pressed, 55);
        static const lv_style_prop_t pressed_props[] = {LV_STYLE_COLOR_FILTER_OPA, 0};
        lv_style_transition_dsc_init(&pressed_transition, pressed_props, lv_anim_path_linear, 0, 0, NULL);
        lv_style_set_transition(&st_pressed, &pressed_transition);
    }
    // lv_theme_default_init() hands back the same static theme each time it is called
    // (accent changes re-init it), so guard against chaining our wrapper onto itself.
    if (th && th->apply_cb != theme_apply_flat) {
        base_apply = th->apply_cb;
        th->apply_cb = theme_apply_flat;
    }
}

// ---------- favourites dispatch ----------
// One place that knows which of the three favourite layers the playing track belongs
// to, so the player button, the hub counts and the lists all agree.
static bool cur_is_fav(const player_status_t *st)
{
    switch (st->kind) {
    case TRACK_BIY: return app_store_is_favbiy(st->biy_day, st->biy_intro);
    case TRACK_LIB: return app_store_is_favlib(app_player_lib_playing_dir(),
                                               app_player_lib_playing_name());
    default:        return st->book_idx >= 0 && app_store_is_fav(st->book_idx, st->chapter);
    }
}
static void cur_fav_toggle(const player_status_t *st)
{
    switch (st->kind) {
    case TRACK_BIY:
        app_store_favbiy_toggle(st->biy_day, st->biy_intro);
        break;
    case TRACK_LIB:
        app_store_favlib_toggle(app_player_lib_playing_dir(),
                                app_player_lib_playing_name(), st->lib_idx);
        break;
    default:
        if (st->book_idx >= 0) app_store_fav_toggle(st->book_idx, st->chapter);
        break;
    }
}
// A Library favourite stores the file NAME, not just its index: adding a file re-sorts
// the folder and would silently move a stored index onto a different track. Re-resolve
// by name on play, falling back to the remembered index only if the name is gone.
static int lib_resolve(const char *dir, const char *name, int fallback)
{
    int n = app_player_lib_browse(dir);
    for (int i = 0; i < n; i++)
        if (!app_player_lib_is_dir(i) && strcmp(app_player_lib_name(i), name) == 0)
            return app_player_lib_track_of(i);
    return fallback;
}

// One transition policy for every route. Startup fades the top-layer chrome with
// Home so the battery/footer never pop in ahead of the new screen.
static void chrome_opacity(void *layer, int32_t opacity)
{
    lv_obj_set_style_opa(layer, (lv_opa_t)opacity, 0);
}
static void present_screen(lv_obj_t *screen)
{
    bool startup = startup_transition;
    uint32_t duration = g_anim == LV_SCR_LOAD_ANIM_NONE ? 0 : startup ? STARTUP_TRANSITION_MS : NAV_TRANSITION_MS;
    if (!startup && duration) {
        // LVGL screen-load animations disable ALL input while prev_scr exists.
        // Load the destination immediately and fade its artwork instead: Back
        // retains its fixed hit target and can accept the very next touch.
        lv_scr_load_anim(screen, LV_SCR_LOAD_ANIM_NONE, 0, 0, true);
        lv_anim_t fade;
        lv_anim_init(&fade);
        lv_anim_set_var(&fade, screen);
        lv_anim_set_exec_cb(&fade, chrome_opacity);
        lv_anim_set_values(&fade, 160, LV_OPA_COVER);
        lv_anim_set_time(&fade, duration);
        lv_anim_set_path_cb(&fade, lv_anim_path_ease_out);
        lv_anim_start(&fade);
        return;
    }
    lv_scr_load_anim(screen, g_anim, duration, 0, true);
    if (!startup) return;
    startup_transition = false;
    lv_anim_t *fade = lv_anim_get(screen, NULL);
    if (fade) lv_anim_set_path_cb(fade, lv_anim_path_ease_out);
    lv_anim_t chrome;
    lv_anim_init(&chrome);
    lv_anim_set_var(&chrome, lv_layer_top());
    lv_anim_set_exec_cb(&chrome, chrome_opacity);
    lv_anim_set_values(&chrome, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_time(&chrome, duration);
    lv_anim_set_path_cb(&chrome, lv_anim_path_ease_out);
    lv_anim_start(&chrome);
}

// ---------- navigation ----------
static void nav_back(void)
{
#if BIBLE_SCREENSHOTS
    ESP_LOGI(TAG, "TOUCH Back action: depth=%d page=%d", nav_sp, cur_page);
#endif
    app_power_user_activity();
    if (nav_sp > 0) {
        nav_sp--;
        g_anim = capture_no_animation ? LV_SCR_LOAD_ANIM_NONE : LV_SCR_LOAD_ANIM_MOVE_RIGHT;
        build_page(nav_stack[nav_sp].page, nav_stack[nav_sp].ctx);
    }
}
static void nav_push(int page, int ctx)
{
#if BIBLE_SCREENSHOTS
    ESP_LOGI(TAG, "TOUCH navigation: depth=%d page=%d -> %d ctx=%d", nav_sp, cur_page, page, ctx);
#endif
    app_power_user_activity();
    if (nav_sp < NAV_MAX - 1) { nav_sp++; nav_stack[nav_sp] = (nav_entry_t){page, ctx}; }
    g_anim = capture_no_animation ? LV_SCR_LOAD_ANIM_NONE : LV_SCR_LOAD_ANIM_MOVE_LEFT;
    build_page(page, ctx);
}
static void nav_reset(int page, int ctx)
{
    nav_sp = 0; nav_stack[0] = (nav_entry_t){page, ctx};
    g_anim = LV_SCR_LOAD_ANIM_FADE_ON;
    build_page(page, ctx);
}
static void back_btn_cb(lv_event_t *e)
{
    (void)e;
    if (nav_sp <= 0) return;
    // Back is a dedicated navigation target, not a scrollable row. Act on the
    // initial contact, then consume the rest of that contact so a hold/drift
    // cannot activate Back again (or Home's Play) on the destination screen.
    lv_indev_t *input = lv_indev_get_act();
    if (input) lv_indev_wait_release(input);
    app_player_click();  // this press deliberately never reaches CLICKED
    nav_back();
}

static void bottom_press_feedback_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *disc = lv_event_get_user_data(e);
#if BIBLE_SCREENSHOTS
    if (code == LV_EVENT_PRESSED || code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST || code == LV_EVENT_CLICKED) {
        lv_point_t point = {0, 0};
        lv_indev_t *input = lv_indev_get_act();
        if (input) lv_indev_get_point(input, &point);
        ESP_LOGI(TAG, "TOUCH %s event=%d point=%d,%d scrolling=%d", lv_event_get_target(e) == mb_play ? "Play" : "Back",
                 code, (int)point.x, (int)point.y, input && lv_indev_get_scroll_obj(input) != NULL);
    }
#endif
    if (code == LV_EVENT_PRESSED)
        lv_obj_add_state(disc, LV_STATE_PRESSED);
    else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST)
        lv_obj_clear_state(disc, LV_STATE_PRESSED);
}

// A real button owns the touch area; its smaller disc is only artwork. Back is wider
// than Play without growing toward the content.
static lv_obj_t *bottom_control(lv_obj_t *parent, int touch_width, int diameter, uint32_t color,
                                lv_event_cb_t cb, lv_event_code_t trigger, lv_obj_t **disc_out)
{
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, touch_width, BOTTOM_TOUCH_H);
    lv_obj_set_pos(b, (LV_HOR_RES - touch_width) / 2, BOTTOM_TOUCH_Y);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_CHAIN |
                        LV_OBJ_FLAG_SCROLL_ON_FOCUS | LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_t *disc = flat(b);
    lv_obj_set_size(disc, diameter, diameter);
    lv_obj_center(disc);
    lv_obj_clear_flag(disc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_radius(disc, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(disc, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(disc, LV_OPA_COVER, 0);
    lv_obj_add_style(disc, &st_pressed, LV_STATE_PRESSED);
    lv_obj_add_event_cb(b, bottom_press_feedback_cb, LV_EVENT_ALL, disc);
    lv_obj_add_event_cb(b, cb, trigger, NULL);
    *disc_out = disc;
    return b;
}

// Back extends 24px farther left/right than Home's Play target. Its top/bottom
// stay fixed; the player's smaller visible disc still clears the rim arc.
static void add_back_btn(lv_obj_t *scr, bool over_arc)
{
    lv_obj_t *disc;
    bottom_control(scr, BACK_TOUCH_W, over_arc ? 48 : 56, COL_CARD2,
                   back_btn_cb, LV_EVENT_PRESSED, &disc);
    icon_mid(disc, SHEMA_ICON_BACK, 24, g_accent_hex);
}

// Swipe-right anywhere = back. Vertical swipes are left to scrolling.
// The setting is cached because this fires on every swipe; Settings updates it.
static bool g_swipe_back = true;

static void screen_gesture_cb(lv_event_t *e)
{
    (void)e;
    if (!g_swipe_back) return;
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) return;
    if (lv_indev_get_gesture_dir(indev) == LV_DIR_RIGHT) {
        lv_indev_wait_release(indev);
        nav_back();
    }
}

// Soft bottom edge for scrolling pages: a stack of strips whose opacity ramps to the
// page background (LVGL 8 gradients do not interpolate opacity). Never clickable.
static lv_obj_t *add_bottom_fade(lv_obj_t *scr, int bottom_y)
{
    // Strips tile exactly (overlapping semi-transparent rows composite twice and show a
    // seam), and the ramp is quadratic so steps are finest where the fade meets content.
    const int n = LIST_FADE_H / 2;                  // 2px strips
    lv_obj_t *f = flat(scr);
    lv_obj_clear_flag(f, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(f, LV_HOR_RES, LIST_FADE_H);
    lv_obj_set_pos(f, 0, bottom_y - LIST_FADE_H);
    for (int i = 0; i < n; i++) {
        int y0 = (i * LIST_FADE_H) / n;             // exact tiling: strip i ends where i+1 starts
        int y1 = ((i + 1) * LIST_FADE_H) / n;
        lv_obj_t *s = flat(f);
        lv_obj_clear_flag(s, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(s, LV_HOR_RES, y1 - y0);
        lv_obj_set_pos(s, 0, y0);
        lv_obj_set_style_bg_color(s, lv_color_hex(COL_BG), 0);
        lv_obj_set_style_bg_opa(s, (lv_opa_t)((255 * i * i) / ((n - 1) * (n - 1))), 0);
    }
    return f;
}

// Pin the top fade to the scroller and show it once the content has scrolled.
static void top_fade_scroll_cb(lv_event_t *e)
{
    lv_obj_t *scroller = lv_event_get_target(e);
    lv_obj_t *fade = lv_event_get_user_data(e);
    if (!lv_obj_is_valid(fade)) return;
    lv_area_t area, screen;
    lv_obj_get_coords(scroller, &area);
    lv_obj_get_coords(lv_obj_get_screen(scroller), &screen);
    lv_obj_set_pos(fade, area.x1 - screen.x1, area.y1 - screen.y1);
    bool show = lv_obj_get_scroll_y(scroller) > 0;
    bool hidden = lv_obj_has_flag(fade, LV_OBJ_FLAG_HIDDEN);
    if (show && hidden) lv_obj_clear_flag(fade, LV_OBJ_FLAG_HIDDEN);
    if (!show && !hidden) lv_obj_add_flag(fade, LV_OBJ_FLAG_HIDDEN);
}
// Bottom padding so the last row can scroll clear of the fade, plus a top fade.
static void scroll_room(lv_obj_t *o)
{
    lv_obj_set_style_pad_bottom(o, LIST_FADE_H, 0);
    // Twelve pixels soften partially scrolled rows under the header without
    // covering the text of a fully visible card. Event-driven; no extra timer.
    lv_obj_t *fade = flat(lv_obj_get_screen(o));
    lv_obj_set_size(fade, LV_HOR_RES, 12);
    lv_obj_add_flag(fade, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(fade, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    for (int i = 0; i < 6; i++) {
        lv_obj_t *strip = flat(fade);
        lv_obj_set_size(strip, LV_HOR_RES, 2);
        lv_obj_set_pos(strip, 0, i * 2);
        lv_obj_clear_flag(strip, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_color(strip, lv_color_hex(COL_BG), 0);
        lv_obj_set_style_bg_opa(strip, (lv_opa_t)(255 * (5-i) * (5-i) / 25), 0);
    }
    lv_obj_add_event_cb(o, top_fade_scroll_cb, LV_EVENT_SCROLL, fade);
}

// "Nothing here" page body: a dimmed icon over a centred sentence. Re-centres the
// page's own content container, so call it instead of new_scroll_list(), not after.
static void empty_state(lv_obj_t *c, shema_icon_id_t ic, const char *text)
{
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(c, 14, 0);
    lv_obj_t *badge = flat(c);
    lv_obj_set_size(badge, 56, 56);
    lv_obj_set_style_bg_color(badge, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(badge, LV_RADIUS_CIRCLE, 0);
    icon_mid(badge, ic, 32, g_accent_hex);
    lv_obj_t *copy = flat(c);
    lv_obj_set_size(copy, lv_pct(92), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(copy, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(copy, 6, 0);
    const char *body = strchr(text, '\n');
    char title[80];
    snprintf(title, sizeof title, "%.*s", body ? (int)(body - text) : (int)strlen(text), text);
    lv_obj_t *heading = lbl(copy, title, &lv_font_montserrat_18, COL_TEXT);
    lv_obj_set_width(heading, lv_pct(100));
    lv_obj_set_style_text_align(heading, LV_TEXT_ALIGN_CENTER, 0);
    if (body && body[1]) {
        lv_obj_t *em = lbl(copy, body + 1, &lv_font_montserrat_16, COL_SUB);
        lv_obj_set_width(em, lv_pct(100));
        lv_label_set_long_mode(em, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(em, LV_TEXT_ALIGN_CENTER, 0);
    }
}

// Pages without the now-playing bar extend their content past CONTENT_BOT; the fade
// follows. The top comes from pg_ctop, not lv_obj_get_y(), which is 0 before layout.
static void page_content_bottom(lv_obj_t *cont, int bottom_y)
{
    lv_obj_set_height(cont, bottom_y - pg_ctop);
    if (pg_fade) lv_obj_set_y(pg_fade, bottom_y - LIST_FADE_H);
}

// Fresh round screen with optional back button + centred title; swipe-back too.
// Returns a flex-column content container inset to the circular safe area, whose
// top clears the back button so nothing overlaps.
static lv_obj_t *make_page(const char *title, bool show_back)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(scr, screen_gesture_cb, LV_EVENT_GESTURE, NULL);

    if (show_back) add_back_btn(scr, false);

    if (title && title[0]) {
        // Back lives in the bottom rim, so the title gets the full centred width: 232px
        // is the glass width at the title's top edge (y=48 -> half-width 122).
        lv_obj_t *t = lbl(scr, title, &lv_font_montserrat_20, COL_TEXT);
        lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
        lv_obj_set_width(t, 232);
        lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(t, LV_ALIGN_TOP_MID, 0, TITLE_Y);
    }

    // The top only needs to clear the title. Titleless pages (e.g. Books) keep the larger
    // inset since they place their own top header.
    int ctop = (title && title[0]) ? CONTENT_TOP_PLAIN : CONTENT_TOP_BACK;
    pg_ctop = ctop;
    lv_obj_t *cont = flat(scr);
    lv_obj_set_pos(cont, 0, ctop);
    lv_obj_set_size(cont, LV_HOR_RES, CONTENT_BOT - ctop);
    lv_obj_set_style_pad_left(cont, SAFE_X, 0);
    lv_obj_set_style_pad_right(cont, SAFE_X, 0);
    lv_obj_set_style_pad_row(cont, 8, 0);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);

    // Created after cont so it draws over the scrolling content. It sits well above the
    // back button's circle (top ~304), so that stays crisp.
    pg_fade = add_bottom_fade(scr, CONTENT_BOT);

    present_screen(scr);
    return cont;
}

// ---------- HOME (hub) ----------
// Currently-selected book/chapter: the playing one, else the last-played (resume).
static bool current_ref(int *book, int *chapter)
{
    player_status_t st; app_player_get_status(&st);
    if (st.book_idx >= 0) { if (book) *book = st.book_idx; if (chapter) *chapter = st.chapter; return true; }
    int lb, lc; uint32_t lp;
    if (app_store_get_last(&lb, &lc, &lp)) { if (book) *book = lb; if (chapter) *chapter = lc; return true; }
    return false;
}
static void home_open_cb(lv_event_t *e)
{
    if (!tap_is_clean(e)) return;
    int target = (int)(intptr_t)lv_event_get_user_data(e);
    if (target == PAGE_FAVOURITES) nav_push(PAGE_FAVOURITES, 0);
    else if (target == PAGE_BOOKS) {
        int cb = -1; current_ref(&cb, NULL);    // open on the current book's testament
        nav_push(PAGE_BOOKS, (cb >= 0) ? BIBLE_BOOKS[cb].testament : BIBLE_OLD);
    }
    else nav_push(target, BIBLE_OLD);
}
static lv_obj_t *hub_btn(lv_obj_t *p, shema_icon_id_t ic, const char *name, int target)
{
    lv_obj_t *b = lv_btn_create(p);
    // With no Home heading, two square rows fit from y66 to y246. Three 84px
    // tiles and two 12px gutters stay inside the round glass, including corners.
    // The width also keeps the 14px "Favourites" caption on one line.
    lv_obj_set_size(b, 84, 84);
    card(b);
    lv_obj_set_style_radius(b, SHEMA_TILE_RADIUS, 0);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(b, 4, 0);
    lv_obj_set_style_pad_row(b, 6, 0);
    lv_obj_add_event_cb(b, home_open_cb, LV_EVENT_ALL, (void *)(intptr_t)target);
    icon(b, ic, 32, g_accent_hex);
    lv_obj_t *cap = lbl(b, name, &lv_font_montserrat_14, COL_TEXT);
    lv_obj_set_width(cap, lv_pct(100));
    // Pin to one line (LONG_DOT alone wraps first) so long captions ellipsise and every
    // tile keeps its icon on the same baseline.
    lv_obj_set_height(cap, lv_font_get_line_height(&lv_font_montserrat_14));
    lv_label_set_long_mode(cap, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(cap, LV_TEXT_ALIGN_CENTER, 0);
    return b;
}
// The SD card mounts /sdcard only on success, so opendir failing == no usable card.
static bool sd_present(void)
{
    if (capture_no_card) return false;
    DIR *d = opendir("/sdcard");
    if (d) { closedir(d); return true; }
    return false;
}
static void build_home(void)
{
    // Home is a launcher with no heading; the version picker lives in Settings. The
    // first row's corners fit r180.
    lv_obj_t *c = make_page(NULL, false);
    pg_ctop = 66;
    lv_obj_set_y(c, pg_ctop);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(c, 16, 0);

    // Home never scrolls, so it has no bottom fade; the now-playing bar owns that space.
    if (pg_fade) { lv_obj_del(pg_fade); pg_fade = NULL; }
    // Stop the tile grid a gap short of the now-playing bar; the tiles centre in the
    // remaining height (66..246 with two rows).
    page_content_bottom(c, HOME_BOT);

    if (!sd_present()) {
        // No card: show a warning banner above the tiles; Settings stays reachable.
        lv_obj_t *w = lv_obj_create(c);
        lv_obj_set_width(w, lv_pct(100));
        lv_obj_set_height(w, LV_SIZE_CONTENT);
        card(w);
        lv_obj_set_style_bg_color(w, lv_color_hex(0x33161C), 0);   // dark warning red
        lv_obj_clear_flag(w, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(w, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(w, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_all(w, 10, 0);
        lv_obj_set_style_pad_column(w, 10, 0);
        icon(w, SHEMA_ICON_SD, 24, COL_DANGER);
        lv_obj_t *wt = lbl(w, "No SD card", &lv_font_montserrat_16, COL_TEXT);
        lv_obj_set_flex_grow(wt, 1);
    }

    // Square icon tiles, wrapping 3 per row inside the 288px inner width.
    lv_obj_t *row = flat(c);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 12, 0);
    lv_obj_set_style_pad_row(row, 12, 0);
    if (!capture_no_card && app_player_versions() > 0)  hub_btn(row, SHEMA_ICON_BOOK, "Bible", PAGE_BOOKS);
    if (!capture_no_card && app_player_biy_langs() > 0) hub_btn(row, SHEMA_ICON_YEAR, "In a Year", PAGE_BIY);
    if (!capture_no_card && app_player_has_library())  hub_btn(row, SHEMA_ICON_FOLDER, "Library", PAGE_LIB);
    // Recent is always shown (it starts empty); Bible, In a Year and Library appear only
    // when the card has that content.
    hub_btn(row, SHEMA_ICON_RECENT,   "Recent",     PAGE_RECENT);
    hub_btn(row, SHEMA_ICON_HEART,    "Favourites", PAGE_FAVOURITES);
    hub_btn(row, SHEMA_ICON_SETTINGS, "Settings",   PAGE_SETTINGS);
}

// ---------- tap vs. scroll guard ----------
// The CST816S drops move samples on fast flicks and LVGL only polls touch every
// read-period ms, so a quick scroll-flick on a grid tile can look like a clean
// press+release and fire CLICKED on the wrong item. Track the finger's travel during
// the press and treat it as a tap only if it barely moved and no ancestor is scrolling.
// Items attach this with LV_EVENT_ALL.
#define TAP_SLOP 14
static lv_coord_t tap_x0, tap_y0;
static bool tap_moved;
static lv_obj_t *tap_owner;
static void *tap_identity;
static void tap_track(void)
{
    lv_indev_t *in = lv_indev_get_act();
    if (!in) return;
    lv_point_t p; lv_indev_get_point(in, &p);
    if (LV_ABS(p.x - tap_x0) > TAP_SLOP || LV_ABS(p.y - tap_y0) > TAP_SLOP) tap_moved = true;
}
// True only on a clean tap (RELEASED with little travel, no ancestor scroll);
// otherwise records the press start / movement and returns false.
static bool tap_is_clean(lv_event_t *e)
{
    lv_indev_t *in = lv_indev_get_act();
    lv_obj_t *target = lv_event_get_target(e);
    switch (lv_event_get_code(e)) {
    case LV_EVENT_PRESSED: {
        lv_point_t p = {0, 0};
        if (in) lv_indev_get_point(in, &p);
        tap_x0 = p.x; tap_y0 = p.y;
        tap_owner = target;
        tap_identity = lv_obj_get_user_data(target);
        // Pressing while the list is still gliding should stop the scroll, not open
        // the item under the finger.
        tap_moved = (in && lv_indev_get_scroll_obj(in) != NULL);
        return false;
    }
    case LV_EVENT_PRESSING:
        if (tap_owner == target) tap_track();
        return false;
    case LV_EVENT_PRESS_LOST:
    case LV_EVENT_DELETE:
        if (tap_owner == target) tap_owner = NULL;
        return false;
    case LV_EVENT_RELEASED: {
        if (tap_owner != target) return false;
        tap_owner = NULL; // one action per press, even if release is delivered twice
        tap_track();                                    // include the release point
        bool clean = !tap_moved && tap_identity == lv_obj_get_user_data(target) &&
                     !(in && lv_indev_get_scroll_obj(in));
        // Emit before an action can delete its screen. LVGL's later CLICKED
        // event may never arrive, and rejected drags should not sound accepted.
        if (clean) app_player_click();
        return clean;
    }
    default:
        return false;
    }
}

// LVGL's built-in checkable switch toggles before our RELEASED handler. Own the
// toggle so scrolling across a switch uses the same clean-tap rule as list rows.
static void guarded_switch_cb(lv_event_t *e)
{
    if (!tap_is_clean(e)) return;
    lv_obj_t *obj = lv_event_get_target(e);
    if (lv_obj_has_state(obj, LV_STATE_CHECKED)) lv_obj_clear_state(obj, LV_STATE_CHECKED);
    else lv_obj_add_state(obj, LV_STATE_CHECKED);
    lv_event_send(obj, LV_EVENT_VALUE_CHANGED, NULL);
}
static void guard_switch(lv_obj_t *obj)
{
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_CHECKABLE | LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_add_event_cb(obj, guarded_switch_cb, LV_EVENT_ALL, NULL);
}

#include "virtual_grid.inc"

// ---------- BOOKS (two-column grid) ----------
static void book_item_cb(lv_event_t *e)
{
    if (!tap_is_clean(e)) return;
    int book = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
    nav_push(PAGE_CHAPTERS, book);
}
static void populate_books(int testament)
{
    g_testament = testament;
    grid_view_begin(bk_list, false, 0, book_item_cb);
    int cur_b = -1; current_ref(&cur_b, NULL);
    for (int i = 0; i < BIBLE_BOOK_COUNT; i++) {
        if (BIBLE_BOOKS[i].testament != testament) continue;
        if (i == cur_b) grid_view.selected = grid_view.count;
        grid_view.ids[grid_view.count++] = i;
    }
    grid_view_finish();
    bool ot = (testament == BIBLE_OLD);
    lv_obj_set_style_bg_color(bk_ot, lv_color_hex(ot ? g_accent_hex : COL_CARD2), 0);
    lv_obj_set_style_text_color(lv_obj_get_child(bk_ot, 0), lv_color_hex(ot ? COL_BG : COL_SUB), 0);
    lv_obj_set_style_bg_color(bk_nt, lv_color_hex(!ot ? g_accent_hex : COL_CARD2), 0);
    lv_obj_set_style_text_color(lv_obj_get_child(bk_nt, 0), lv_color_hex(!ot ? COL_BG : COL_SUB), 0);

}
static void seg_ot_cb(lv_event_t *e) { if (!tap_is_clean(e)) return; populate_books(BIBLE_OLD); }
static void seg_nt_cb(lv_event_t *e) { if (!tap_is_clean(e)) return; populate_books(BIBLE_NEW); }
// Chips are wired on LV_EVENT_ALL, not LV_EVENT_CLICKED, because every handler passed here
// gates on tap_is_clean() — and that only reports a clean tap on LV_EVENT_RELEASED. On
// CLICKED it returns false, so a chip registered that way would never fire.
static lv_obj_t *seg_chip(lv_obj_t *p, const char *t, lv_event_cb_t cb)
{
    lv_obj_t *b = lv_btn_create(p);
    lv_obj_set_flex_grow(b, 1);
    // Old/New (and the BIY language chips) are primary controls: 48 drawn + 4 of reach
    // is TAP_COMPACT, and the 8px column gap between chips means the two hit rectangles
    // meet without crossing.
    lv_obj_set_height(b, 48);
    lv_obj_set_style_radius(b, 24, 0);
    lv_obj_set_ext_click_area(b, 4);
    lv_obj_set_style_bg_color(b, lv_color_hex(COL_CARD2), 0);
    lv_obj_center(lbl(b, t, &lv_font_montserrat_16, COL_SUB));
    lv_obj_add_event_cb(b, cb, LV_EVENT_ALL, NULL);   // see the note above: NOT _CLICKED
    return b;
}
static void build_books(int testament)
{
    lv_obj_t *c = make_page(NULL, true);          // no "Bible" title bar; back chevron + swipe-back stay
    lv_obj_t *scr = lv_obj_get_parent(c);
    // Chips occupy y40..95 including their reach; leave a real 8px gap before
    // the grid so a clipped top-row tap cannot land on the segment above it.
    pg_ctop = 104;
    lv_obj_set_y(c, pg_ctop);

    // No now-playing bar on this page, so run the scrolling grid down toward the
    // bottom rim instead of stopping at the shared CONTENT_BOT (which reserves the
    // Home-only bar's space). Books has no title, so its content top is CONTENT_TOP_BACK.
    page_content_bottom(c, GRID_BOT);

    // Old/New segment replaces the title, so the whole content column goes to the book grid.
    lv_obj_t *seg = flat(scr);
    // TAP_COMPACT tall so the box contains the chips' 4px reach each way (see the
    // ext_click_area note at the top of the file).
    lv_obj_set_size(seg, 200, TAP_COMPACT);
    lv_obj_align(seg, LV_ALIGN_TOP_MID, 0, 40);
    lv_obj_set_flex_align(seg, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_flex_flow(seg, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(seg, GAP, 0);
    bk_ot = seg_chip(seg, "Old", seg_ot_cb);
    bk_nt = seg_chip(seg, "New", seg_nt_cb);

    bk_list = lv_obj_create(c);                    // two-column wrapping grid of book tiles
    lv_obj_remove_style_all(bk_list);
    lv_obj_set_width(bk_list, lv_pct(100));
    lv_obj_set_flex_grow(bk_list, 1);
    lv_obj_set_flex_flow(bk_list, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(bk_list, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(bk_list, 8, 0);
    lv_obj_set_style_pad_column(bk_list, 8, 0);
    lv_obj_set_scroll_dir(bk_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(bk_list, LV_SCROLLBAR_MODE_OFF);
    scroll_room(bk_list);

    populate_books(testament);
}

// ---------- CHAPTERS (ring of circles) ----------
static void chapter_item_cb(lv_event_t *e)
{
    if (!tap_is_clean(e)) return;
    int b, ch;
    decode_play(lv_event_get_target(e), &b, &ch);
    app_player_play(b, ch);
    app_store_set_last(b, ch, 0);
    app_store_hist_push(0, b, ch, 0);
    nav_push(PAGE_PLAYER, 0);
}
static void build_chapters(int book_idx)
{
    lv_obj_t *c = make_page(BIBLE_BOOKS[book_idx].name, true);
    page_content_bottom(c, GRID_BOT);   // run the chapter grid down toward the bottom rim (titled page)

    lv_obj_t *grid = lv_obj_create(c);
    lv_obj_remove_style_all(grid);
    lv_obj_set_width(grid, lv_pct(100));
    lv_obj_set_flex_grow(grid, 1);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(grid, 12, 0);
    lv_obj_set_style_pad_top(grid, 4, 0);
    lv_obj_set_scroll_dir(grid, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(grid, LV_SCROLLBAR_MODE_OFF);
    scroll_room(grid);

    grid_view_begin(grid, true, book_idx, chapter_item_cb);
    int cur_b = -1, cur_c = -1; current_ref(&cur_b, &cur_c);
    for (int ch = 1; ch <= BIBLE_BOOKS[book_idx].chapter_count; ch++) {
        if (cur_b == book_idx && cur_c == ch) grid_view.selected = grid_view.count;
        grid_view.ids[grid_view.count++] = ch;
    }
    grid_view_finish();
}

// ---------- modal (sleep timer) ----------
static void close_modal(void) { if (modal) { lv_obj_del(modal); modal = NULL; } }
static void modal_bg_cb(lv_event_t *e)
{
    if (lv_event_get_target(e) == modal && tap_is_clean(e)) close_modal();
}
// Swipe-right closes the modal. The modal lives on lv_layer_top, so its gestures never
// reach the screen's swipe-back handler. Follows the g_swipe_back setting.
static void modal_gesture_cb(lv_event_t *e)
{
    (void)e;
    if (!g_swipe_back) return;
    lv_indev_t *indev = lv_indev_get_act();
    if (indev && lv_indev_get_gesture_dir(indev) == LV_DIR_RIGHT) {
        lv_indev_wait_release(indev);
        close_modal();
    }
}
static void set_sleep(int minutes)
{
    sleep_minutes = minutes;        // remembered so the dialog can show the active choice
    if (minutes > 0) {
        sleep_deadline_ms = esp_log_timestamp() + (uint32_t)minutes * 60u * 1000u;
        sleep_active = true;
    } else {
        sleep_active = false;
    }
}
static void sleep_opt_cb(lv_event_t *e)
{
    if (!tap_is_clean(e)) return;
    set_sleep((int)(intptr_t)lv_event_get_user_data(e));
    close_modal();
}
// ---------- shared option-modal scaffolding ----------
// The sleep timer, the settings pickers, the letter jump and the clear-history
// confirmation are all the same widget, so they share this.
//
// The card height snaps to a whole number of rows so the scroll clip always lands in
// the gap between rows; the runway below lets the remaining rows scroll fully into view.
#define MODAL_W        240
#define MODAL_ROW_H    56    // full-width pill at TAP_COMPACT
#define MODAL_ROW_GAP  6
#define MODAL_PAD      12
#define MODAL_ROWS_VIS 4

// How many whole rows a centred card of this width can show inside the round glass:
// half-height sqrt(r^2 - halfw^2), less corner-radius relief and a bezel margin.
static int modal_fit_rows(int w, int row_h, int want)
{
    const float inset = (float)SHEMA_CARD_RADIUS * 0.3f;   // rounded-corner relief
    const float r     = (float)SHEMA_SCREEN_DIAMETER / 2.0f - 4.0f;  // bezel margin
    float hw = (float)w / 2.0f - inset;
    if (hw >= r) return 1;                                 // absurd width: fail safe
    float hh = sqrtf(r * r - hw * hw) + inset;
    int avail = (int)(2.0f * hh) - MODAL_PAD * 2
              - lv_font_get_line_height(&lv_font_montserrat_18) - MODAL_ROW_GAP;
    int rows = (avail + MODAL_ROW_GAP) / (row_h + MODAL_ROW_GAP);
    if (rows < 1) rows = 1;
    return rows < want ? rows : want;
}

static lv_obj_t *modal_card_ex(const char *title, int w, int row_h, int want_rows)
{
    close_modal();
    modal = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(modal);
    lv_obj_set_size(modal, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(modal, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(modal, LV_OPA_70, 0);
    lv_obj_add_flag(modal, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(modal, modal_bg_cb, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(modal, modal_gesture_cb, LV_EVENT_GESTURE, NULL);

    lv_obj_t *cardv = lv_obj_create(modal);
    lv_obj_set_width(cardv, w);
    lv_obj_set_height(cardv, LV_SIZE_CONTENT);
    card(cardv);
    lv_obj_set_style_bg_color(cardv, lv_color_hex(COL_CARD), 0);
    lv_obj_set_flex_flow(cardv, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(cardv, MODAL_PAD, 0);
    lv_obj_set_style_pad_row(cardv, MODAL_ROW_GAP, 0);
    lv_obj_clear_flag(cardv, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *h = lbl(cardv, title, &lv_font_montserrat_18, COL_TEXT);
    lv_obj_set_width(h, lv_pct(100));
    lv_label_set_long_mode(h, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(h, LV_TEXT_ALIGN_CENTER, 0);

    int rows = modal_fit_rows(w, row_h, want_rows);
    // Keep the heading fixed and scroll only the options.
    lv_obj_t *options = flat(cardv);
    lv_obj_set_size(options, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(options, rows * row_h + (rows - 1) * MODAL_ROW_GAP, 0);
    lv_obj_set_flex_flow(options, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(options, MODAL_ROW_GAP, 0);
    lv_obj_add_flag(options, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(options, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(options, LV_SCROLLBAR_MODE_OFF);
    lv_obj_center(cardv);
    return options;
}
static lv_obj_t *modal_card(const char *title)
{
    return modal_card_ex(title, MODAL_W, MODAL_ROW_H, MODAL_ROWS_VIS);
}

// A modal row with an explicit text colour — the destructive rows need one, and setting
// it on the button does nothing because lbl() writes the colour onto the label itself.
static lv_obj_t *modal_row_col(lv_obj_t *cardv, const char *text, bool sel, uint32_t fg,
                               lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = lv_btn_create(cardv);
    lv_obj_set_width(b, lv_pct(100));
    lv_obj_set_height(b, MODAL_ROW_H);
    lv_obj_set_style_bg_color(b, lv_color_hex(sel ? g_accent_hex : COL_CARD2), 0);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_hor(b, 12, 0);
    lv_obj_set_style_pad_ver(b, 6, 0);
    // Reserve the tick's width once so two-line version names stay readable.
    int text_w = MODAL_W - MODAL_PAD * 2 - 24 - (sel ? 32 : 0);
    lv_point_t text_size;
    lv_txt_get_size(&text_size, text, &lv_font_montserrat_16, 0, 0, text_w, LV_TEXT_FLAG_NONE);
    int max_h = lv_font_get_line_height(&lv_font_montserrat_16) * 2;
    lv_obj_t *t = lbl(b, text, &lv_font_montserrat_16, fg);
    lv_obj_set_width(t, text_w);
    lv_obj_set_height(t, text_size.y > max_h ? max_h : text_size.y);
    lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(t, LV_ALIGN_LEFT_MID, sel ? 32 : 0, 0);
    if (sel) {
        // Tick as well as the accent fill, so selection is not a colour-only cue.
        lv_obj_t *ck = icon(b, SHEMA_ICON_CHECK, 24, COL_BG);
        if (ck) lv_obj_align(ck, LV_ALIGN_LEFT_MID, 0, 0);
    }
    lv_obj_add_event_cb(b, cb, LV_EVENT_ALL, ud);
    return b;
}
static lv_obj_t *modal_row(lv_obj_t *cardv, const char *text, bool sel, lv_event_cb_t cb, void *ud)
{
    return modal_row_col(cardv, text, sel, sel ? COL_BG : COL_TEXT, cb, ud);
}

static void confirmation_cancel_cb(lv_event_t *e)
{
    if (tap_is_clean(e)) close_modal();
}
static void open_confirmation(const char *title, const char *action, lv_event_cb_t confirm, void *ud)
{
    lv_obj_t *options = modal_card(title);
    lv_obj_t *destructive = modal_row_col(options, action, false, 0xFFACA3, confirm, ud);
    lv_obj_set_style_bg_color(destructive, lv_color_hex(0x3A2024), 0);
    modal_row(options, "Cancel", false, confirmation_cancel_cb, NULL);
}

static void open_sleep_dialog(void)
{
    lv_obj_t *cardv = modal_card("Sleep timer");
    static const int mins[] = {0, 15, 30, 45, 60, 90, 120};
    static const char *names[] = {"Off", "15 min", "30 min", "45 min", "60 min", "90 min", "120 min"};
    // Highlight the active choice, as the settings pickers do.
    int cur = sleep_active ? sleep_minutes : 0;
    lv_obj_t *sel_row = NULL;
    for (int i = 0; i < 7; i++) {
        lv_obj_t *r = modal_row(cardv, names[i], mins[i] == cur, sleep_opt_cb, (void *)(intptr_t)mins[i]);
        if (mins[i] == cur) sel_row = r;
    }
    // Seven choices in a three-row window: open scrolled to the running one.
    if (sel_row) { lv_obj_update_layout(cardv); lv_obj_scroll_to_view(sel_row, LV_ANIM_OFF); }
}

// ---------- PLAYER (arc watch-face) ----------
static void p_play_cb(lv_event_t *e)  { if (!tap_is_clean(e)) return; app_player_toggle_pause(); app_power_user_activity(); }
static void p_next_cb(lv_event_t *e)  { if (!tap_is_clean(e)) return; app_player_next(); app_power_user_activity(); }
static void p_prev_cb(lv_event_t *e)  { if (!tap_is_clean(e)) return; app_player_prev(); app_power_user_activity(); }
static void p_seek_cb(lv_event_t *e)
{
    app_player_click();
    player_status_t st; app_player_get_status(&st);
    uint32_t ms = (uint32_t)((uint64_t)lv_slider_get_value(lv_event_get_target(e)) * st.dur_ms / 1000);
    app_player_seek_ms(ms);
    app_power_user_activity();
}
static void p_vol_cb(lv_event_t *e)
{
    int v = lv_slider_get_value(lv_event_get_target(e));
    app_player_set_volume(v);                        // live-apply every drag step
    // Update the readout on every step so it tracks the thumb while dragging.
    if (pw_volpct) { char vb[8]; snprintf(vb, sizeof vb, "%d%%", v); lv_label_set_text(pw_volpct, vb); }
    if (lv_event_get_code(e) == LV_EVENT_RELEASED) {
        app_player_click();
        app_store_set_volume(v);                     // persist once, on release
    }
}
// Freeze the target while the dialog is open: playback may advance before the
// user confirms, especially for a Library item whose identity includes its path.
static player_status_t pending_fav;
static char pending_fav_dir[STORE_LIB_DIR_LEN], pending_fav_name[STORE_LIB_NAME_LEN];
static void p_fav_remove_confirm_cb(lv_event_t *e)
{
    if (!tap_is_clean(e)) return;
    close_modal();
    switch (pending_fav.kind) {
    case TRACK_BIY:
        if (app_store_is_favbiy(pending_fav.biy_day, pending_fav.biy_intro))
            app_store_favbiy_toggle(pending_fav.biy_day, pending_fav.biy_intro);
        break;
    case TRACK_LIB:
        if (app_store_is_favlib(pending_fav_dir, pending_fav_name))
            app_store_favlib_toggle(pending_fav_dir, pending_fav_name, pending_fav.lib_idx);
        break;
    default:
        if (pending_fav.book_idx >= 0 && app_store_is_fav(pending_fav.book_idx, pending_fav.chapter))
            app_store_fav_toggle(pending_fav.book_idx, pending_fav.chapter);
        break;
    }
    app_power_user_activity();
}
static void p_fav_cb(lv_event_t *e)
{
    if (!tap_is_clean(e)) return;
    player_status_t st; app_player_get_status(&st);
    if (cur_is_fav(&st)) {
        pending_fav = st;
        snprintf(pending_fav_dir, sizeof pending_fav_dir, "%s", app_player_lib_playing_dir());
        snprintf(pending_fav_name, sizeof pending_fav_name, "%s", app_player_lib_playing_name());
        open_confirmation("Remove favourite?", "Remove", p_fav_remove_confirm_cb, NULL);
    } else {
        cur_fav_toggle(&st);      // adding a favourite remains immediate
    }
    app_power_user_activity();
}
static void p_sleep_cb(lv_event_t *e) { if (!tap_is_clean(e)) return; open_sleep_dialog(); }

// A round icon button. `draw` is the visible circle; `touch` is the hit size, made up
// with ext_click_area. Callers space neighbours by at least the sum of their reaches.
static lv_obj_t *circ_btn(lv_obj_t *p, shema_icon_id_t ic, unsigned icsz, lv_event_cb_t cb,
                          int draw, int touch, uint32_t bg, uint32_t fg)
{
    lv_obj_t *b = lv_btn_create(p);
    lv_obj_set_size(b, draw, draw);
    if (touch > draw) lv_obj_set_ext_click_area(b, (touch - draw + 1) / 2);
    lv_obj_set_style_bg_color(b, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    icon_mid(b, ic, icsz, fg);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_ALL, NULL);
    return b;
}

// Play/pause as vector artwork. Both icons are created and the inactive one is hidden,
// so the 2.5 Hz state swap is a flag toggle rather than object churn.
static void playpause_icons(lv_obj_t *btn, unsigned icsz, uint32_t fg,
                            lv_obj_t **out_play, lv_obj_t **out_pause)
{
    *out_play  = icon_mid(btn, SHEMA_ICON_PLAY,  icsz, fg);
    *out_pause = icon_mid(btn, SHEMA_ICON_PAUSE, icsz, fg);
    lv_obj_add_flag(*out_pause, LV_OBJ_FLAG_HIDDEN);
}
static void playpause_set(lv_obj_t *ic_play, lv_obj_t *ic_pause, bool playing)
{
    if (!ic_play || !ic_pause) return;
    if (playing) { lv_obj_add_flag(ic_play, LV_OBJ_FLAG_HIDDEN);  lv_obj_clear_flag(ic_pause, LV_OBJ_FLAG_HIDDEN); }
    else         { lv_obj_add_flag(ic_pause, LV_OBJ_FLAG_HIDDEN); lv_obj_clear_flag(ic_play,  LV_OBJ_FLAG_HIDDEN); }
}

// Set the Now Playing title/subtitle, clamping the title to at most PLAYER_TITLE_LINES.
// Measured rather than pinned, so short titles keep their own height.
static void player_set_title(const char *title, const char *sub)
{
    if (!pw_title) return;
    const lv_font_t *f = &lv_font_montserrat_28;
    lv_point_t hero_size;
    lv_txt_get_size(&hero_size, title, f, 0, 0, PLAYER_TITLE_W, LV_TEXT_FLAG_NONE);
    if (hero_size.y > lv_font_get_line_height(f)) f = &lv_font_montserrat_20;
    lv_obj_set_style_text_font(pw_title, f, 0);
    int maxh = lv_font_get_line_height(f) * PLAYER_TITLE_LINES;
    lv_point_t sz;
    lv_txt_get_size(&sz, title, f, 0, 0, PLAYER_TITLE_W, LV_TEXT_FLAG_NONE);
    bool clamp = sz.y > maxh;
    lv_label_set_long_mode(pw_title, clamp ? LV_LABEL_LONG_DOT : LV_LABEL_LONG_WRAP);
    lv_obj_set_height(pw_title, clamp ? maxh : LV_SIZE_CONTENT);
    lv_label_set_text(pw_title, title);
    lv_label_set_text(pw_sub, sub ? sub : "");
}

static void build_player(void)
{
    // Custom full-circle screen (no title bar / content frame).
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(scr, screen_gesture_cb, LV_EVENT_GESTURE, NULL);
    add_back_btn(scr, true);   // player has a rim arc -> compact, arc-clearing back
    // The player does not use make_page(), so clear pg_fade (it belonged to the previous,
    // now-deleted screen).
    pg_fade = NULL;

    // Rim progress arc (visual only — seeking is the slim slider below).
    pw_arc = lv_arc_create(scr);
    lv_obj_set_size(pw_arc, 348, 348);
    lv_obj_center(pw_arc);
    lv_arc_set_rotation(pw_arc, 270);
    lv_arc_set_bg_angles(pw_arc, 0, 360);
    lv_arc_set_range(pw_arc, 0, 1000);
    lv_arc_set_value(pw_arc, 0);
    lv_obj_remove_style(pw_arc, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(pw_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(pw_arc, 8, LV_PART_MAIN);
    lv_obj_set_style_arc_width(pw_arc, 8, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(pw_arc, lv_color_hex(COL_TRACK), LV_PART_MAIN);
    lv_obj_set_style_arc_color(pw_arc, lv_color_hex(g_accent_hex), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(pw_arc, true, LV_PART_INDICATOR);

    // Title + subtitle, centred near the top of the dial. 224 wide at y=44 fits the glass;
    // two title lines plus the subtitle end at y=111, above the seek bar's reach.
    lv_obj_t *g1 = flat(scr);
    lv_obj_set_size(g1, PLAYER_TITLE_W, LV_SIZE_CONTENT);
    lv_obj_align(g1, LV_ALIGN_TOP_MID, 0, 44);
    lv_obj_set_flex_flow(g1, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(g1, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(g1, 2, 0);
    pw_title = lbl(g1, "", &lv_font_montserrat_20, COL_TEXT);
    lv_obj_set_width(pw_title, PLAYER_TITLE_W);
    lv_label_set_long_mode(pw_title, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(pw_title, LV_TEXT_ALIGN_CENTER, 0);
    // Subtitle (period / book / folder name) is always one line.
    pw_sub = lbl(g1, "", &lv_font_montserrat_18, g_accent_hex);
    lv_obj_set_width(pw_sub, PLAYER_TITLE_W);
    lv_obj_set_height(pw_sub, lv_font_get_line_height(&lv_font_montserrat_18));
    lv_label_set_long_mode(pw_sub, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(pw_sub, LV_TEXT_ALIGN_CENTER, 0);
    player_set_title("Nothing playing", "");

    // Scrub slider (handles its own drag, so it never triggers swipe-back), flanked by
    // elapsed/total. ext_click_area 20 turns the 8px track into a 48px band (y 118..166).
    pw_seek = lv_slider_create(scr);
    lv_obj_set_size(pw_seek, 168, 8);
    lv_obj_align(pw_seek, LV_ALIGN_CENTER, 0, -38);
    lv_slider_set_range(pw_seek, 0, 1000);
    lv_obj_set_style_bg_color(pw_seek, lv_color_hex(COL_TRACK), LV_PART_MAIN);
    lv_obj_set_style_bg_color(pw_seek, lv_color_hex(g_accent_hex), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(pw_seek, lv_color_hex(g_accent_hex), LV_PART_KNOB);
    lv_obj_set_style_pad_all(pw_seek, 4, LV_PART_KNOB);
    lv_obj_set_ext_click_area(pw_seek, 20);
    lv_obj_add_event_cb(pw_seek, p_seek_cb, LV_EVENT_RELEASED, NULL);
    pw_cur = lbl(scr, "0:00", &lv_font_montserrat_16, COL_SUB);
    lv_obj_align_to(pw_cur, pw_seek, LV_ALIGN_OUT_LEFT_MID, -8, 0);
    pw_tot = lbl(scr, "0:00", &lv_font_montserrat_16, COL_SUB);
    lv_obj_align_to(pw_tot, pw_seek, LV_ALIGN_OUT_RIGHT_MID, 8, 0);

    // Transport on a lower arc: sleep · prev · play/pause · next · fav. SPACE_BETWEEN
    // (not SPACE_EVENLY) puts all 24px of slack into the four inter-button gaps, which
    // keeps the touch reaches from crossing.
    lv_obj_t *trans = flat(scr);
    // 4px horizontal padding keeps the outer buttons' reach inside this box.
    lv_obj_set_size(trans, PLAYER_TRANS_W + 8, PLAYER_TRANS_H);
    lv_obj_align(trans, LV_ALIGN_CENTER, 0, PLAYER_TRANS_Y);
    lv_obj_set_style_pad_hor(trans, 4, 0);
    lv_obj_set_flex_flow(trans, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(trans, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    // Moon, not a bell: this arms a sleep timer, it is not an alarm.
    pw_sleepbtn = circ_btn(trans, SHEMA_ICON_MOON, 24, p_sleep_cb,
                           PLAYER_BTN_SIDE, TAP_COMPACT, COL_CARD2, COL_SUB);
    pw_sleep = lv_obj_get_child(pw_sleepbtn, 0);   // icon container -> accent when a timer runs
    circ_btn(trans, SHEMA_ICON_PREV, 28, p_prev_cb,
             PLAYER_BTN_SKIP, PLAYER_BTN_SKIP + 4, COL_CARD2, COL_TEXT);

    pw_playbtn = lv_btn_create(trans);
    lv_obj_set_size(pw_playbtn, PLAYER_BTN_PLAY, PLAYER_BTN_PLAY);
    lv_obj_set_style_bg_color(pw_playbtn, lv_color_hex(g_accent_hex), 0);
    lv_obj_set_style_bg_opa(pw_playbtn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(pw_playbtn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_shadow_width(pw_playbtn, 0, 0);
    playpause_icons(pw_playbtn, 32, COL_BG, &pw_ic_play, &pw_ic_pause);
    lv_obj_add_event_cb(pw_playbtn, p_play_cb, LV_EVENT_ALL, NULL);

    circ_btn(trans, SHEMA_ICON_SKIP, 28, p_next_cb,
             PLAYER_BTN_SKIP, PLAYER_BTN_SKIP + 4, COL_CARD2, COL_TEXT);
    pw_favbtn = circ_btn(trans, SHEMA_ICON_HEART, 24, p_fav_cb,
                         PLAYER_BTN_SIDE, TAP_COMPACT, COL_CARD2, COL_SUB);
    pw_fav = lv_obj_get_child(pw_favbtn, 0);

    // Volume: icon · slider · numeric readout, below the transport and above the back button.
    pw_vol = lv_slider_create(scr);
    lv_obj_set_size(pw_vol, 160, 8);
    lv_obj_align(pw_vol, LV_ALIGN_CENTER, 0, 88);
    lv_slider_set_range(pw_vol, 0, 100);
    lv_slider_set_value(pw_vol, app_player_get_volume(), LV_ANIM_OFF);
    lv_obj_set_style_bg_color(pw_vol, lv_color_hex(COL_TRACK), LV_PART_MAIN);
    lv_obj_set_style_bg_color(pw_vol, lv_color_hex(g_accent_hex), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(pw_vol, lv_color_hex(g_accent_hex), LV_PART_KNOB);
    lv_obj_set_style_pad_all(pw_vol, 4, LV_PART_KNOB);
    // The touch band ends at y287, leaving a gap before Back starts at y294.
    // Hit regions do not overlap; target selection does not depend on draw order.
    lv_obj_set_ext_click_area(pw_vol, 16);
    lv_obj_add_event_cb(pw_vol, p_vol_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(pw_vol, p_vol_cb, LV_EVENT_RELEASED, NULL);   // persist on release
    lv_obj_t *vicon = icon(scr, SHEMA_ICON_VOLUME, 24, COL_SUB);
    lv_obj_align_to(vicon, pw_vol, LV_ALIGN_OUT_LEFT_MID, -8, 0);
    char vb[8]; snprintf(vb, sizeof vb, "%d%%", app_player_get_volume());
    pw_volpct = lbl(scr, vb, &lv_font_montserrat_14, COL_SUB);
    lv_obj_align_to(pw_volpct, pw_vol, LV_ALIGN_OUT_RIGHT_MID, 8, 0);

    present_screen(scr);
}

// ---------- FAVOURITES / list detail ----------
static lv_obj_t *new_scroll_list(lv_obj_t *c)
{
    lv_obj_t *list = lv_list_create(c);
    lv_obj_set_width(list, lv_pct(100));
    lv_obj_set_flex_grow(list, 1);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_style_pad_row(list, GAP, 0);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
    // Vertical only: rows cannot drag sideways, and horizontal swipes stay free for
    // swipe-back.
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    scroll_room(list);
    return list;
}
// Every list row is at least TAP_COMPACT tall: three rows fit the 222px of a titled
// page, with a partial fourth under the fade.
#define ROW_MIN_H  TAP_COMPACT
static void style_row(lv_obj_t *it)
{
    lv_obj_set_style_bg_color(it, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_bg_opa(it, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(it, 0, 0);
    lv_obj_set_style_radius(it, SHEMA_CARD_RADIUS, 0);
    lv_obj_set_style_text_color(it, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_height(it, ROW_MIN_H);
    lv_obj_set_flex_align(it, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
}
#include "virtual_list.inc"

// ---------- FAVOURITES hub (Bible / In a Year / Library) ----------
static void fav_hub_cb(lv_event_t *e)
{
    if (!tap_is_clean(e)) return;
    nav_push(PAGE_LIST_DETAIL, (int)(intptr_t)lv_event_get_user_data(e));
}
// Hub rows: icon first (lv_list_add_btn would add its own), then title, then count.
static void fav_hub_row(lv_obj_t *list, shema_icon_id_t ic, const char *name, int n, int ctx)
{
    lv_obj_t *it = lv_list_add_btn(list, NULL, name);
    lv_obj_t *glyph = icon(it, ic, 24, g_accent_hex);
    lv_obj_move_to_index(glyph, 0);
    style_row(it);
    lv_obj_set_style_pad_hor(it, 12, 0);
    lv_obj_set_style_pad_column(it, 10, 0);
    lv_obj_set_style_text_font(lv_obj_get_child(it, 1), &lv_font_montserrat_18, 0);
    // Trailing count; lv_list_add_btn gives the text label flex-grow, so this sits hard right.
    char cnt[12];
    snprintf(cnt, sizeof cnt, "%d", n);
    lbl(it, cnt, &lv_font_montserrat_16, n ? g_accent_hex : COL_SUB);
    lv_obj_add_event_cb(it, fav_hub_cb, LV_EVENT_ALL, (void *)(intptr_t)ctx);
}
static void build_favourites(void)
{
    lv_obj_t *c = make_page("Favourites", true);
    page_content_bottom(c, GRID_BOT);
    // As on Home, a layer is offered only when the card carries that content.
    bool bible = !capture_no_card && app_player_versions() > 0;
    bool biy   = !capture_no_card && app_player_biy_langs() > 0;
    bool lib   = !capture_no_card && app_player_has_library();
    // With no card at all, all three gates close. The Favourites tile is on Home
    // unconditionally (it is not a capability, it merely starts empty), so this page
    // shows an empty state rather than a blank page.
    if (!bible && !biy && !lib) {
        empty_state(c, SHEMA_ICON_HEART,
                    "Nothing to save yet.\nFavourites appear here once the card has audio on it.");
        return;
    }
    lv_obj_t *list = new_scroll_list(c);
    if (bible) fav_hub_row(list, SHEMA_ICON_BOOK,   "Bible",     app_store_fav_count(),    LIST_FAV);
    if (biy)   fav_hub_row(list, SHEMA_ICON_YEAR,   "In a Year", app_store_favbiy_count(), LIST_FAV_BIY);
    if (lib)   fav_hub_row(list, SHEMA_ICON_FOLDER, "Library",   app_store_favlib_count(), LIST_FAV_LIB);
}

static int detail_count(int which)
{
    if (capture_empty_lists) return 0;
    switch (which) {
    case LIST_FAV:     return app_store_fav_count();
    case LIST_FAV_BIY: return app_store_favbiy_count();
    case LIST_FAV_LIB: return app_store_favlib_count();
    default:           return app_store_pl_track_count(which);
    }
}
static bool detail_get(int which, int i, track_ref_t *tr) { return (which == LIST_FAV) ? app_store_fav_get(i, tr) : app_store_pl_track_get(which, i, tr); }
static void detail_item_cb(lv_event_t *e)
{
    if (!tap_is_clean(e)) return;
    int b, ch; decode_play(lv_event_get_target(e), &b, &ch);
    app_player_play(b, ch); app_store_set_last(b, ch, 0);
    app_store_hist_push(0, b, ch, 0);
    nav_push(PAGE_PLAYER, 0);
}
// Play a favourite from the BIY or Library layer (the Bible layer uses detail_item_cb).
static void fav_alt_item_cb(lv_event_t *e)
{
    if (!tap_is_clean(e)) return;
    int i = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
    if (detail_which == LIST_FAV_BIY) {
        int day = -1, intro = -1;
        if (!app_store_favbiy_get(i, &day, &intro)) return;
        if (intro >= 0) app_player_play_biy_intro(intro); else app_player_play_biy(day);
        app_store_set_last_biy(day, intro, 0);
        app_store_hist_push(1, day, intro, 0);
    } else {
        char dir[STORE_LIB_DIR_LEN], name[STORE_LIB_NAME_LEN];
        int track = -1;
        if (!app_store_favlib_get(i, dir, sizeof dir, name, sizeof name, &track)) return;
        track = lib_resolve(dir, name, track);
        if (track < 0) return;
        app_player_play_lib(dir, track);
        app_store_set_last_lib(dir, track, name, 0);
        app_store_hist_push_lib(dir, name, track, 0);
    }
    nav_push(PAGE_PLAYER, 0);
}
static int pending_detail_which;
static void detail_remove_confirm_cb(lv_event_t *e)
{
    if (!tap_is_clean(e)) return;
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    close_modal();
    if (cur_page != PAGE_LIST_DETAIL || detail_which != pending_detail_which ||
        i < 0 || i >= detail_count(detail_which)) return;
    if (detail_which == LIST_FAV_BIY) {
        app_store_favbiy_remove(i);
    } else if (detail_which == LIST_FAV_LIB) {
        app_store_favlib_remove(i);
    } else {
        track_ref_t tr;
        if (detail_get(detail_which, i, &tr)) {
            if (detail_which == LIST_FAV) app_store_fav_toggle(tr.book_idx, tr.chapter);
            else app_store_pl_remove(detail_which, i);
        }
    }
    if (detail_count(detail_which) == 0) detail_edit = false;
    g_anim = LV_SCR_LOAD_ANIM_NONE;   // redrawn in place, so it must not slide
    build_list_detail(detail_which);
}
static void detail_remove_cb(lv_event_t *e)
{
    if (!tap_is_clean(e)) return;   // a flick starting on the trash icon must not delete
    pending_detail_which = detail_which;
    open_confirmation(IS_FAV_LIST(detail_which) ? "Remove favourite?" : "Remove track?",
                      "Remove", detail_remove_confirm_cb, lv_obj_get_user_data(lv_event_get_target(e)));
}
// Toggling Edit redraws the same page without a transition. Set here, not in
// build_list_detail(), so the MOVE_LEFT from the Favourites hub still animates.
static void detail_edit_cb(lv_event_t *e)
{
    if (!tap_is_clean(e)) return;
    detail_edit = !detail_edit;
    g_anim = LV_SCR_LOAD_ANIM_NONE;
    build_list_detail(detail_which);
}
static void build_list_detail(int which)
{
    detail_which = which;
    const char *ptitle = "Playlist";
    if (which == LIST_FAV)          ptitle = "Bible";
    else if (which == LIST_FAV_BIY) ptitle = "In a Year";
    else if (which == LIST_FAV_LIB) ptitle = "Library";
    lv_obj_t *c = make_page(ptitle, true);
    page_content_bottom(c, GRID_BOT);
    int n = detail_count(which);

    // Handle empty before creating the list: new_scroll_list() has flex_grow 1 and would
    // push the empty state off-centre.
    if (n == 0) {
        empty_state(c, IS_FAV_LIST(which) ? SHEMA_ICON_HEART : SHEMA_ICON_FOLDER,
                    IS_FAV_LIST(which)
                        ? "Nothing saved yet.\nUse the heart on the player to keep a passage close."
                        : "This playlist is empty.");
        return;
    }

    // Edit/Done: 112x44 drawn with 6px of reach is 124x56, alone on its row so nothing
    // is near enough to clip.
    lv_obj_t *hdr = flat(c);
    lv_obj_set_size(hdr, lv_pct(100), TAP_COMPACT);   // holds the button AND its reach
    lv_obj_t *eb = lv_btn_create(hdr);
    lv_obj_set_size(eb, 112, 44);
    lv_obj_align(eb, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_ext_click_area(eb, 6);
    lv_obj_set_style_bg_color(eb, lv_color_hex(detail_edit ? g_accent_hex : COL_CARD2), 0);
    lv_obj_set_style_radius(eb, 22, 0);
    lv_obj_set_style_shadow_width(eb, 0, 0);
    lv_obj_set_flex_flow(eb, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(eb, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(eb, 10, 0);
    lv_obj_set_style_pad_column(eb, 6, 0);
    icon(eb, detail_edit ? SHEMA_ICON_CHECK : SHEMA_ICON_EDIT, 24, detail_edit ? COL_BG : COL_TEXT);
    lbl(eb, detail_edit ? "Done" : "Edit", &lv_font_montserrat_16, detail_edit ? COL_BG : COL_TEXT);
    lv_obj_add_event_cb(eb, detail_edit_cb, LV_EVENT_ALL, NULL);

    lv_obj_t *list = new_scroll_list(c);
    if (!list_view_begin(list)) { lv_obj_del(list); empty_state(c, SHEMA_ICON_WARNING, "Unable to open list."); return; }
    if (detail_edit) list_view.remove_callback = detail_remove_cb;
    for (int i = 0; i < n; i++) {
        track_ref_t tr;
        char buf[96], sub[96];
        shema_icon_id_t sym = SHEMA_ICON_BOOK;
        sub[0] = 0;

        // Each layer names its rows differently; the edit/remove chrome below is shared.
        // The second line carries the context the title alone cannot: which folder a
        // Library track came from, which part of the plan a day belongs to.
        if (which == LIST_FAV_BIY) {
            int day = -1, intro = -1;
            if (!app_store_favbiy_get(i, &day, &intro)) continue;
            if (intro >= 0) {
                snprintf(buf, sizeof buf, "%s", app_player_biy_intro_title(intro));
                snprintf(sub, sizeof sub, "Bible in a Year");
            } else {
                const char *t = app_player_biy_day_title(day);
                snprintf(buf, sizeof buf, "Day %d%s%s", day, t[0] ? "  " : "", t);
                int per = (day >= 1) ? biy_period_of(day) : -1;
                snprintf(sub, sizeof sub, "%s", per >= 0 ? BIY_PERIODS[per].name : "Bible in a Year");
            }
            sym = SHEMA_ICON_YEAR;
        } else if (which == LIST_FAV_LIB) {
            sym = SHEMA_ICON_AUDIO;
            char dir[STORE_LIB_DIR_LEN];
            if (!app_store_favlib_get(i, dir, sizeof dir, buf, sizeof buf, NULL)) continue;
            snprintf(sub, sizeof sub, "%s", dir[0] ? dir : "Library");
        } else {
            if (!detail_get(which, i, &tr)) continue;
            snprintf(buf, sizeof(buf), "%s %d", BIBLE_BOOKS[tr.book_idx].name, tr.chapter);
            if (which == LIST_FAV && app_played_get_bible(tr.book_idx, tr.chapter, NULL))
                snprintf(sub, sizeof sub, LV_SYMBOL_OK "  Finished");
        }

        int data = i;
        lv_event_cb_t callback = NULL;
        if (!detail_edit) {
            if (which == LIST_FAV_BIY || which == LIST_FAV_LIB) callback = fav_alt_item_cb;
            else { callback = detail_item_cb; data = tr.book_idx * 1000 + tr.chapter; }
        }
        list_view_add(buf, sub, detail_edit ? SHEMA_ICON_COUNT : sym, COL_TEXT,
                      false, 0, false, callback, data);
    }
    list_view_finish(-1);
}

// ---------- SETTINGS ----------
// Live handles into the page, so a picker or a slider can update the value shown on the
// card without rebuilding the whole screen (and losing the scroll position with it).
// All are cleared by build_page() before any page is built, so none can dangle.
static lv_obj_t *g_scrto_val, *g_pwroff_val, *g_version_val, *g_bright_val;
// Settings restores its scroll position across rebuilds (theme change, history cleared).
static lv_obj_t *g_set_cont;            // the live Settings scroll container, or NULL
static int       g_set_scroll = -1;     // pending restore offset; -1 = start at the top
static void settings_keep_scroll(void)
{
    if (g_set_cont) g_set_scroll = lv_obj_get_scroll_y(g_set_cont);
}

// Brightness range, mirroring app_power_set_brightness()'s clamp (the panel is
// unreadable below 45).
#define BRIGHT_MIN 45
#define BRIGHT_MAX 100

static void set_bright_cb(lv_event_t *e)
{
    int v = lv_slider_get_value(lv_event_get_target(e));
    app_power_set_brightness(v);                     // live-apply every drag step
    // Track the thumb with the readout, same as the player's volume percentage.
    if (g_bright_val) { char b[16]; snprintf(b, sizeof b, "%d%%", v); lv_label_set_text(g_bright_val, b); }
    if (lv_event_get_code(e) == LV_EVENT_RELEASED) {
        app_player_click();
        app_store_set_brightness(v);                 // persist once, on release
    }
}
static void set_repeat_cb(lv_event_t *e)
{
    bool on = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    app_player_set_repeat_all(on); app_store_set_repeat_all(on);
}
// A Settings card: accent icon + title, then whatever control the caller adds. The
// header row is returned via *out_hdr when the caller wants to put the current value on
// the same line as the label (switches, which have no separate value row).
static lv_obj_t *setting_card_hdr(lv_obj_t *c, shema_icon_id_t ic, const char *title, lv_obj_t **out_hdr)
{
    lv_obj_t *cd = lv_obj_create(c);
    lv_obj_set_width(cd, lv_pct(100));
    lv_obj_set_height(cd, LV_SIZE_CONTENT);
    card(cd);
    lv_obj_clear_flag(cd, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(cd, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(cd, 14, 0);
    lv_obj_set_style_pad_row(cd, 12, 0);

    // Header row: accent icon + title, matching the home tiles' icon/label style.
    lv_obj_t *hdr = flat(cd);
    lv_obj_set_width(hdr, lv_pct(100));
    lv_obj_set_height(hdr, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(hdr, 10, 0);
    icon(hdr, ic, 24, g_accent_hex);
    lv_obj_t *t = lbl(hdr, title, &lv_font_montserrat_18, COL_TEXT);
    lv_obj_set_width(t, 0);
    lv_obj_set_flex_grow(t, 1);
    lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
    if (out_hdr) *out_hdr = hdr;
    return cd;
}
static lv_obj_t *setting_card(lv_obj_t *c, shema_icon_id_t ic, const char *title)
{
    return setting_card_hdr(c, ic, title, NULL);
}
// Changing the accent re-themes and rebuilds the page, so hold the scroll position.
static void theme_swatch_cb(lv_event_t *e)
{
    if (!tap_is_clean(e)) return;
    settings_keep_scroll();
    apply_accent((uint32_t)(intptr_t)lv_event_get_user_data(e));
}
// Screen-timeout and auto-power-off choices; counts derive from the tables.
static const int  SCRTO_SECS[]  = { 15, 30, 60, 120, 300, 0 };
static const char *const SCRTO_LABELS[]  = {"15 sec", "30 sec", "1 min", "2 min", "5 min", "Never"};
#define SCRTO_N  ((int)(sizeof(SCRTO_SECS) / sizeof(SCRTO_SECS[0])))
static const int  PWROFF_SECS[] = { 0, 120, 300, 600, 1800 };
static const char *const PWROFF_LABELS[] = {"Off", "2 min", "5 min", "10 min", "30 min"};
#define PWROFF_N ((int)(sizeof(PWROFF_SECS) / sizeof(PWROFF_SECS[0])))
static int opt_index(const int *vals, int n, int v) { for (int i = 0; i < n; i++) if (vals[i] == v) return i; return 0; }

// Round-safe option picker: a centred modal (like the sleep dialog) instead of
// an lv_dropdown, whose pop-up list gets clipped by the round bezel.
static void (*g_pick_apply)(int idx);

static void pick_opt_cb(lv_event_t *e)
{
    if (!tap_is_clean(e)) return;
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    void (*fn)(int) = g_pick_apply;
    close_modal();
    if (fn) fn(idx);
}
static void open_picker(const char *title, const char *const labels[], int n, int cur, void (*apply)(int))
{
    g_pick_apply = apply;
    lv_obj_t *cardv = modal_card(title);
    lv_obj_t *sel_row = NULL;
    for (int i = 0; i < n; i++) {
        lv_obj_t *b = modal_row(cardv, labels[i], i == cur, pick_opt_cb, (void *)(intptr_t)i);
        if (i == cur) sel_row = b;
    }
    // Open scrolled to the current value (the version list can run to 12 entries).
    if (sel_row) { lv_obj_update_layout(cardv); lv_obj_scroll_to_view(sel_row, LV_ANIM_OFF); }
}
static void apply_scrto(int idx)
{
    int s = SCRTO_SECS[idx];
    app_power_set_screen_timeout(s); app_store_set_screen_timeout(s);
    if (g_scrto_val) lv_label_set_text(g_scrto_val, SCRTO_LABELS[idx]);
}
static void apply_pwroff(int idx)
{
    int s = PWROFF_SECS[idx];
    app_power_set_idle_off(s > 0, s > 0 ? s : 300); app_store_set_poweroff(s);
    if (g_pwroff_val) lv_label_set_text(g_pwroff_val, PWROFF_LABELS[idx]);
}
// Both pickers are modals over the live Settings page, so the page is never rebuilt and
// its scroll position survives the round trip on its own.
static void scrto_row_cb(lv_event_t *e) { if (!tap_is_clean(e)) return; open_picker("Screen timeout", SCRTO_LABELS, SCRTO_N, opt_index(SCRTO_SECS, SCRTO_N, app_store_get_screen_timeout(30)), apply_scrto); }
static void pwroff_row_cb(lv_event_t *e) { if (!tap_is_clean(e)) return; open_picker("Auto power-off", PWROFF_LABELS, PWROFF_N, opt_index(PWROFF_SECS, PWROFF_N, app_store_get_poweroff(300)), apply_pwroff); }

static void apply_version(int idx)
{
    if (idx < 0 || idx >= app_player_versions()) return;
    g_version_idx = idx;
    app_player_set_base(app_player_version_dir(idx));
    app_store_set_version(app_player_version_dir(idx));
    if (g_version_val) lv_label_set_text(g_version_val, app_player_version_name(idx));
    // re-open the current chapter from the new version so the change is heard now
    player_status_t st; app_player_get_status(&st);
    if (st.book_idx >= 0) app_player_play(st.book_idx, st.chapter);
}
static void version_row_cb(lv_event_t *e)
{
    if (!tap_is_clean(e)) return;
    int n = app_player_versions();
    if (n <= 0) return;
    static const char *labels[12];
    int cap = (int)(sizeof(labels) / sizeof(labels[0]));
    if (n > cap) { ESP_LOGW(TAG, "version picker: %d versions, showing first %d", n, cap); n = cap; }
    for (int i = 0; i < n; i++) labels[i] = app_player_version_name(i);
    open_picker("Version", labels, n, g_version_idx, apply_version);
}

// A tappable value row inside a setting card: current value plus a chevron; opens the
// picker on tap.
static lv_obj_t *value_row(lv_obj_t *cd, const char *cur_text, lv_event_cb_t cb)
{
    lv_obj_t *b = lv_btn_create(cd);
    lv_obj_set_width(b, lv_pct(100));
    lv_obj_set_height(b, TAP_COMPACT);
    lv_obj_set_style_bg_color(b, lv_color_hex(COL_CARD2), 0);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_pad_left(b, 14, 0);
    lv_obj_set_style_pad_right(b, 12, 0);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *v = lbl(b, cur_text, &lv_font_montserrat_18, COL_TEXT);
    lv_obj_set_width(v, 0);
    lv_obj_set_flex_grow(v, 1);
    lv_label_set_long_mode(v, LV_LABEL_LONG_DOT);
    icon(b, SHEMA_ICON_NEXT, 24, COL_SUB);
    lv_obj_add_event_cb(b, cb, LV_EVENT_ALL, NULL);
    return v;
}
static void set_swipe_cb(lv_event_t *e)
{
    bool on = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    g_swipe_back = on;
    app_store_set_swipe_back(on);
}

// ---------- clear played history ----------
// Requires confirmation: clears every resume point and tick across all three collections.
// The confirm row is red; tapping the background dismisses.
static void clear_played_do_cb(lv_event_t *e)
{
    if (!tap_is_clean(e)) return;
    close_modal();
    app_played_clear_all();             // only here, and only on the explicit confirm row
    settings_keep_scroll();
    g_anim = LV_SCR_LOAD_ANIM_NONE;
    build_page(PAGE_SETTINGS, 0);      // redraw so the button greys itself out
}
static void clear_played_cb(lv_event_t *e)
{
    if (!tap_is_clean(e)) return;
    // Confirm / Cancel. Clears resume points and ticks only; no files are touched.
    open_confirmation("Clear played history?", "Clear everything", clear_played_do_cb, NULL);
}

static void build_settings(void)
{
    lv_obj_t *c = make_page("Settings", true);
    page_content_bottom(c, GRID_BOT);   // use the bottom rim (no now-playing bar here; titled page)
    lv_obj_set_style_pad_row(c, 12, 0);
    scroll_room(c);
    lv_obj_add_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(c, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(c, LV_SCROLLBAR_MODE_OFF);

    g_set_cont = c;

    // 1. Version — populated from the card, disabled-looking when there is none.
    lv_obj_t *vc = setting_card(c, SHEMA_ICON_BOOK, "Version");
    int nver = capture_no_card ? 0 : app_player_versions();
    g_version_val = value_row(vc, nver > 0 ? app_player_version_name(g_version_idx) : "No SD card", version_row_cb);
    if (nver <= 0) lv_obj_set_style_text_color(g_version_val, lv_color_hex(COL_SUB), 0);

    // 2. Brightness, BRIGHT_MIN..BRIGHT_MAX, with a numeric readout.
    lv_obj_t *bhdr = NULL;
    lv_obj_t *bc = setting_card_hdr(c, SHEMA_ICON_SUN, "Brightness", &bhdr);
    // The stored value is raw; clamp it so the readout matches the thumb and the panel.
    int bright = app_store_get_brightness(80);
    if (bright < BRIGHT_MIN) bright = BRIGHT_MIN; else if (bright > BRIGHT_MAX) bright = BRIGHT_MAX;
    char bb[16]; snprintf(bb, sizeof bb, "%d%%", bright);
    g_bright_val = lbl(bhdr, bb, &lv_font_montserrat_16, COL_SUB);
    lv_obj_t *bs = lv_slider_create(bc);
    lv_obj_set_width(bs, lv_pct(100));
    lv_obj_set_height(bs, 10);
    lv_slider_set_range(bs, BRIGHT_MIN, BRIGHT_MAX);
    lv_slider_set_value(bs, bright, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bs, lv_color_hex(COL_TRACK), LV_PART_MAIN);
    lv_obj_set_style_bg_color(bs, lv_color_hex(g_accent_hex), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(bs, lv_color_hex(g_accent_hex), LV_PART_KNOB);
    lv_obj_set_style_pad_all(bs, 6, LV_PART_KNOB);
    lv_obj_set_ext_click_area(bs, 16);          // 10px track, 42px of reach
    lv_obj_add_event_cb(bs, set_bright_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(bs, set_bright_cb, LV_EVENT_RELEASED, NULL);   // persist on release

    // 3. Wrap Bible playback from Revelation back to Genesis 1 (and Prev from Genesis 1
    // back to the end). Bible only; BIY and Library are unaffected.
    lv_obj_t *rhdr = NULL;
    setting_card_hdr(c, SHEMA_ICON_REPEAT, "Loop Bible", &rhdr);
    lv_obj_set_height(rhdr, TAP_COMPACT);       // contains the switch's vertical reach
    lv_obj_t *sw = lv_switch_create(rhdr);
    guard_switch(sw);
    lv_obj_set_size(sw, 56, 30);
    lv_obj_set_ext_click_area(sw, 13);          // 56x30 drawn, 82x56 touched
    if (app_store_get_repeat_all(false)) lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(sw, lv_color_hex(COL_TRACK), LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw, lv_color_hex(g_accent_hex), LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw, set_repeat_cb, LV_EVENT_VALUE_CHANGED, NULL);

    // 4. Swipe-back, on by default. Can be disabled for users who drag when they mean to
    // tap; the back button remains on every screen.
    lv_obj_t *ghdr = NULL;
    setting_card_hdr(c, SHEMA_ICON_BACK, "Swipe to go back", &ghdr);
    lv_obj_set_height(ghdr, TAP_COMPACT);
    lv_obj_t *gsw = lv_switch_create(ghdr);
    guard_switch(gsw);
    lv_obj_set_size(gsw, 56, 30);
    lv_obj_set_ext_click_area(gsw, 13);
    if (g_swipe_back) lv_obj_add_state(gsw, LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(gsw, lv_color_hex(COL_TRACK), LV_PART_MAIN);
    lv_obj_set_style_bg_color(gsw, lv_color_hex(g_accent_hex), LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_add_event_cb(gsw, set_swipe_cb, LV_EVENT_VALUE_CHANGED, NULL);

    // 5/6. Screen timeout (six choices) and auto power-off (five) — both read their
    // stored value and show it here rather than only inside the picker.
    lv_obj_t *sc = setting_card(c, SHEMA_ICON_MOON, "Screen timeout");
    g_scrto_val = value_row(sc, SCRTO_LABELS[opt_index(SCRTO_SECS, SCRTO_N, app_store_get_screen_timeout(30))], scrto_row_cb);

    lv_obj_t *pc = setting_card(c, SHEMA_ICON_POWER, "Auto power-off");
    g_pwroff_val = value_row(pc, PWROFF_LABELS[opt_index(PWROFF_SECS, PWROFF_N, app_store_get_poweroff(300))], pwroff_row_cb);

    // 7. Theme colour: champagne (default) first, then six alternatives. A stored
    // preference is shown as selected, since g_accent_hex is read from the store at boot.
    lv_obj_t *tc = setting_card(c, SHEMA_ICON_PALETTE, "Theme colour");
    lv_obj_t *sw2 = flat(tc);
    lv_obj_set_width(sw2, lv_pct(100));
    lv_obj_set_height(sw2, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(sw2, LV_FLEX_FLOW_ROW_WRAP);
    // CENTER rather than SPACE_BETWEEN: the seven swatches wrap 4 + 3, and SPACE_BETWEEN
    // would push the second row out to the card's edges. The fixed 20px column gap keeps
    // the touch reaches apart.
    lv_obj_set_flex_align(sw2, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(sw2, 20, 0);
    lv_obj_set_style_pad_row(sw2, 12, 0);
    lv_obj_set_style_pad_ver(sw2, 6, 0);        // contains the top/bottom rows' reach
    static const uint32_t palette[] = {
        ACCENT_DEFAULT,                                                      // champagne (default)
        0xE8A33D, 0x4A90D9, 0x3FB984, 0xA068D8, 0xD9534F, 0x2DB6B6,          // alternatives
    };
    const int npal = (int)(sizeof(palette) / sizeof(palette[0]));
    for (int i = 0; i < npal; i++) {
        // 44 drawn + 6 of reach = TAP_COMPACT, inside a 20px column gap: the invisible
        // hit circles stay 8px apart. Four fit across the card's 260px of inner width.
        lv_obj_t *sb = lv_btn_create(sw2);
        lv_obj_set_size(sb, 44, 44);
        lv_obj_set_ext_click_area(sb, 6);
        lv_obj_set_style_radius(sb, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(sb, lv_color_hex(palette[i]), 0);
        lv_obj_set_style_bg_opa(sb, LV_OPA_COVER, 0);
        lv_obj_set_style_shadow_width(sb, 0, 0);
        bool sel = (palette[i] == g_accent_hex);
        lv_obj_set_style_border_width(sb, sel ? 3 : 0, 0);
        lv_obj_set_style_border_color(sb, lv_color_hex(COL_TEXT), 0);
        // Tick as well as the ring, so selection is not a colour-only cue.
        if (sel) icon_mid(sb, SHEMA_ICON_CHECK, 24, COL_BG);
        lv_obj_add_event_cb(sb, theme_swatch_cb, LV_EVENT_ALL, (void *)(intptr_t)palette[i]);
    }

    // 8. Clear played history: irreversible, so it sits low on the page behind a
    // confirmation, and greys out (rather than hiding) when there is nothing to clear.
    // It forgets ticks and resume points; it deletes no audio.
    lv_obj_t *phc = setting_card(c, SHEMA_ICON_TRASH, "Played history");
    bool any = !capture_empty_lists && app_played_any();
    lv_obj_t *pb = lv_btn_create(phc);
    lv_obj_set_width(pb, lv_pct(100));
    lv_obj_set_height(pb, TAP_COMPACT);
    lv_obj_set_style_radius(pb, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(pb, lv_color_hex(COL_CARD2), 0);
    lv_obj_set_style_shadow_width(pb, 0, 0);
    lv_obj_center(lbl(pb, any ? "Clear all" : "Nothing to clear", &lv_font_montserrat_18,
                      any ? 0xFFACA3 : COL_SUB));
    if (any) lv_obj_add_event_cb(pb, clear_played_cb, LV_EVENT_ALL, NULL);

    // 9. Firmware build, so the installed version can be identified without tools.
    // scroll_room() above lets it scroll fully clear of the bottom fade.
    const esp_app_desc_t *desc = esp_app_get_description();
    char fw[96];
    snprintf(fw, sizeof fw, "Shema by holystack.dev\nFirmware %s\n%s", desc->version, desc->date);
    lv_obj_t *fwl = lbl(c, fw, &lv_font_montserrat_14, COL_SUB);
    lv_obj_set_width(fwl, lv_pct(100));
    lv_obj_set_style_text_align(fwl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(fwl, 4, 0);

    // Restore the scroll position after a rebuild (theme change, history cleared).
    if (g_set_scroll > 0) {
        lv_obj_update_layout(c);
        lv_obj_scroll_to_y(c, g_set_scroll, LV_ANIM_OFF);
    }
    g_set_scroll = -1;
}

// ---------- BIBLE IN A YEAR ----------
static void biy_period_cb(lv_event_t *e)
{
    if (!tap_is_clean(e)) return;
    nav_push(PAGE_BIY_DAYS, (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e)));
}
static void biy_play_cb(lv_event_t *e)
{
    if (!tap_is_clean(e)) return;
    int v = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
    if (v >= 1) { app_player_play_biy(v); app_store_set_last_biy(v, -1, 0); app_store_hist_push(1, v, -1, 0); }
    else { int in = -v - 1; app_player_play_biy_intro(in); app_store_set_last_biy(-1, in, 0); app_store_hist_push(1, -1, in, 0); }
    nav_push(PAGE_PLAYER, 0);
}
static void biy_set_lang(int i)
{
    g_biy_lang = i;
    app_store_set_biy_lang(app_player_biy_lang_name(i));   // persist by folder name, not index
    app_player_biy_set_lang(i);
    g_anim = LV_SCR_LOAD_ANIM_NONE;
    build_page(PAGE_BIY, 0);
}
// One handler for all language chips; the chip's user_data carries its index.
static void biy_lang_cb(lv_event_t *e)
{
    if (!tap_is_clean(e)) return;
    biy_set_lang((int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e)));
}

// The intro/checkpoint episode that belongs to a period (title contains the period
// name), or -1. EN/ML carry different intro sets, so this is per-language.
static int biy_intro_for_period(int period)
{
    const char *pn = BIY_PERIODS[period].name;
    if (strncmp(pn, "The ", 4) == 0) pn += 4;     // "The Church" -> "Church"
    int n = app_player_biy_intro_count();
    for (int i = 0; i < n; i++) if (ci_strstr(app_player_biy_intro_title(i), pn)) return i;
    return -1;
}
static bool biy_intro_is_section(int i)
{
    for (int p = 0; p < BIY_PERIOD_COUNT; p++) if (biy_intro_for_period(p) == i) return true;
    return false;
}
// How many intro/checkpoint episodes are NOT already shown inside a period section, i.e.
// exactly what the "Intros & Checkpoints" page will list. Used by both the Settings row
// gate and the page itself, so the row never opens a blank page.
static int biy_loose_intro_count(void)
{
    int n = 0;
    for (int i = 0; i < app_player_biy_intro_count(); i++) {
        const char *t = app_player_biy_intro_title(i);
        if (t[0] && !biy_intro_is_section(i)) n++;
    }
    return n;
}
static void build_biy(void)
{
    // Languages are the BIY/<lang> folders found on the card (labelled by folder name).
    // Keep the selection in range and scan it; a single-language card needs no toggle.
    int nlang = app_player_biy_langs();
    if (g_biy_lang < 0 || g_biy_lang >= nlang) g_biy_lang = 0;
    app_player_biy_set_lang(g_biy_lang);

    lv_obj_t *c = make_page("Bible in a Year", true);
    page_content_bottom(c, GRID_BOT);

    if (nlang > 1) {                               // language toggle only when >1 folder present
        lv_obj_t *seg = flat(c);                   // one chip per language (folder name = label)
        lv_obj_set_width(seg, lv_pct(100));
        lv_obj_set_height(seg, TAP_COMPACT);       // room for the chips AND their reach
        lv_obj_set_flex_flow(seg, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(seg, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(seg, GAP, 0);
        for (int i = 0; i < nlang; i++) {
            lv_obj_t *chip = seg_chip(seg, app_player_biy_lang_name(i), biy_lang_cb);
            lv_obj_set_user_data(chip, (void *)(intptr_t)i);
            if (i == g_biy_lang) {
                lv_obj_set_style_bg_color(chip, lv_color_hex(g_accent_hex), 0);
                lv_obj_set_style_text_color(lv_obj_get_child(chip, 0), lv_color_hex(COL_BG), 0);
            } else {
                lv_obj_set_style_bg_color(chip, lv_color_hex(COL_CARD2), 0);
            }
        }
    }

    lv_obj_t *list = new_scroll_list(c);
    if (!list_view_begin(list)) { lv_obj_del(list); empty_state(c, SHEMA_ICON_WARNING, "Unable to open list."); return; }
    for (int p = 0; p < BIY_PERIOD_COUNT; p++) {
        char r[24]; snprintf(r, sizeof r, "Days %d-%d", BIY_PERIODS[p].first_day, BIY_PERIODS[p].last_day);
        list_view_add(BIY_PERIODS[p].name, r, SHEMA_ICON_COUNT, COL_TEXT,
                      true, BIY_PERIODS[p].color, false, biy_period_cb, p);
    }
    if (biy_loose_intro_count() > 0) {
        list_view_add("Intros & Checkpoints", NULL, SHEMA_ICON_AUDIO, COL_TEXT,
                      false, 0, false, biy_period_cb, BIY_CTX_INTROS);
    }
    list_view_finish(-1);
}
static void build_biy_days(int period)
{
    app_player_biy_set_lang(g_biy_lang);
    bool intros = (period == BIY_CTX_INTROS);
    lv_obj_t *c = make_page(intros ? "Intros & Checkpoints" : BIY_PERIODS[period].name, true);
    page_content_bottom(c, GRID_BOT);
    // Backstop: build_biy() gates the row on the same count, but never render a blank page.
    if (intros && biy_loose_intro_count() == 0) {
        empty_state(c, SHEMA_ICON_YEAR, "Nothing here.");
        return;
    }
    lv_obj_t *list = new_scroll_list(c);
    if (!list_view_begin(list)) { lv_obj_del(list); empty_state(c, SHEMA_ICON_WARNING, "Unable to open list."); return; }
    player_status_t st; app_player_get_status(&st);

    if (intros) {
        // Only episodes not already shown inside a section (checkpoints + preamble).
        for (int i = 0; i < app_player_biy_intro_count(); i++) {
            const char *t = app_player_biy_intro_title(i);
            if (!t[0] || biy_intro_is_section(i)) continue;
            list_view_add(t, NULL, SHEMA_ICON_AUDIO, COL_TEXT,
                          false, 0, false, biy_play_cb, -(i + 1));
        }
        list_view_finish(-1);
        return;
    }

    uint32_t color = BIY_PERIODS[period].color;
    int pintro = biy_intro_for_period(period);     // this period's intro, shown first
    if (pintro >= 0) {
        list_view_add("Introduction", app_player_biy_intro_title(pintro), SHEMA_ICON_COUNT,
                      g_accent_hex, true, color, false, biy_play_cb, -(pintro + 1));
    }
    for (int day = BIY_PERIODS[period].first_day; day <= BIY_PERIODS[period].last_day; day++) {
        bool active = (st.kind == TRACK_BIY && st.biy_day == day);
        const char *t = app_player_biy_day_title(day);
        // Tick ahead of the day number for anything already heard end to end.
        bool done = app_played_get_biy(day, -1, NULL);
        char head[88];
        if (done) snprintf(head, sizeof head, LV_SYMBOL_OK " Day %d  %s", day, t);
        else      snprintf(head, sizeof head, "Day %d  %s", day, t);
        char rd[80]; biy_readings_str(day, rd, sizeof rd);
        list_view_add(head, rd, SHEMA_ICON_COUNT,
                      active ? g_accent_hex : (done ? COL_DONE_T : COL_TEXT),
                      true, color, false, biy_play_cb, day);
    }
    list_view_finish(-1);
}

// ---------- RECENT (listening history, both collections) ----------
static void play_ref(int kind, int a, int b, uint32_t pos)
{
    if (kind == 1) {
        if (b >= 0) app_player_play_biy_intro(b); else app_player_play_biy(a);
        app_store_set_last_biy(a, b, pos);
    } else { app_player_play(a, b); app_store_set_last(a, b, pos); }
    if (pos > 3000) app_player_seek_ms(pos);
    app_store_hist_push(kind, a, b, pos);
    nav_push(PAGE_PLAYER, 0);
}
static void recent_item_cb(lv_event_t *e)
{
    if (!tap_is_clean(e)) return;
    int i = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));

    // Library entries carry a folder+file identity rather than a numeric ref.
    char dir[STORE_LIB_DIR_LEN], name[STORE_LIB_NAME_LEN];
    int track = -1;
    if (app_store_hist_get_lib(i, dir, sizeof dir, name, sizeof name, &track)) {
        int kind, a, b; uint32_t pos = 0;
        app_store_hist_get(i, &kind, &a, &b, &pos);
        track = lib_resolve(dir, name, track);
        if (track < 0) return;
        app_player_play_lib(dir, track);
        app_store_set_last_lib(dir, track, name, pos);
        if (pos > 3000) app_player_seek_ms(pos);
        app_store_hist_push_lib(dir, name, track, pos);
        nav_push(PAGE_PLAYER, 0);
        return;
    }

    int kind, a, b; uint32_t pos;
    if (app_store_hist_get(i, &kind, &a, &b, &pos)) play_ref(kind, a, b, pos);
}
static void build_recent(void)
{
    lv_obj_t *c = make_page("Recent", true);
    page_content_bottom(c, GRID_BOT);
    int n = capture_empty_lists ? 0 : app_store_hist_count();
    if (n == 0) {
        empty_state(c, SHEMA_ICON_RECENT, "Nothing played yet.\nWhat you listen to appears here.");
        return;
    }
    lv_obj_t *list = new_scroll_list(c);
    if (!list_view_begin(list)) { lv_obj_del(list); empty_state(c, SHEMA_ICON_WARNING, "Unable to open list."); return; }
    for (int i = 0; i < n; i++) {
        int kind, a, b; uint32_t pos;
        if (!app_store_hist_get(i, &kind, &a, &b, &pos)) continue;
        char buf[96];
        // Context and state go into separate buffers joined once; a self-referential
        // snprintf fails -Werror=format-truncation. SEP is 7 bytes, so the result fits `sub`.
        char ctx[STORE_LIB_DIR_LEN] = "";   // the longest context is a Library folder path
        char state[32] = "";
        char sub[sizeof ctx + sizeof state + 8];
        shema_icon_id_t sym = SHEMA_ICON_BOOK;
        bool done = false;
        if (kind == 2) {
            // Library: the stored file name IS the label (no BIBLE_BOOKS lookup to do).
            char dir[STORE_LIB_DIR_LEN];
            if (!app_store_hist_get_lib(i, dir, sizeof dir, buf, sizeof buf, NULL)) continue;
            sym = SHEMA_ICON_AUDIO;
            snprintf(ctx, sizeof ctx, "%s", dir[0] ? dir : "Library");
        } else {
            player_status_t st = {0};
            st.kind = kind;
            if (kind == 1) { st.book_idx = -1; st.biy_day = a; st.biy_intro = b; }
            else           { st.book_idx = a;  st.chapter = b; st.biy_day = -1; st.biy_intro = -1; }
            status_line(&st, buf, sizeof buf);
            if (kind == 1) {
                sym  = SHEMA_ICON_YEAR;
                done = app_played_get_biy(a, b, NULL);
                int per = (a >= 1) ? biy_period_of(a) : -1;
                snprintf(ctx, sizeof ctx, "%s", per >= 0 ? BIY_PERIODS[per].name : "Bible in a Year");
            } else {
                done = app_played_get_bible(a, b, NULL);
            }
        }
        // State: resumes part-way, starts again, or replays (recent_item_cb seeks to the
        // resume point).
        if (pos > 3000) {
            char at[16]; fmt_time(at, sizeof at, pos);
            snprintf(state, sizeof state, "Resume at %s", at);
        } else if (done) {
            snprintf(state, sizeof state, LV_SYMBOL_OK "  Finished");
        }
        if (ctx[0] && state[0]) snprintf(sub, sizeof sub, "%s  " LV_SYMBOL_BULLET "  %s", ctx, state);
        else                    snprintf(sub, sizeof sub, "%s%s", ctx, state);
        list_view_row_t *row = list_view_add(buf, sub, sym, COL_TEXT,
                                          false, 0, false, recent_item_cb, i);
        if (row) row->sub_color = done ? COL_DONE_T : COL_SUB;
    }
    list_view_finish(-1);
}

// ---------- LIBRARY (nested folder browser: LIBRARY/<folder>/.../<track>.mp3) ----------
#define LIB_DEPTH_MAX 8
#define LIB_PAGE      150             // entries rendered per screen (caps LVGL object count)
// Folder size above which the alphabet jump row appears.
#define LIB_JUMP_MIN  100
static char g_lib_stack[LIB_DEPTH_MAX][64];   // path components of the current browse location
static int  g_lib_depth;                       // 0 = LIBRARY root
static int  g_lib_off;                          // first entry shown in the current window
static bool g_lib_keepoff;                       // preserve g_lib_off across a same-folder rebuild (paging)
static int  g_lib_jumpto = -1;                   // entry to scroll into view after a letter jump
static void build_lib(int depth);

// Join the active path prefix (stack[0..depth-1]) into a rel path under LIBRARY.
static void lib_relpath(char *out, int sz, int depth)
{
    out[0] = 0;
    for (int i = 0; i < depth && i < LIB_DEPTH_MAX; i++) {
        if (i && (int)strlen(out) < sz - 1) strncat(out, "/", sz - strlen(out) - 1);
        strncat(out, g_lib_stack[i], sz - strlen(out) - 1);
    }
}
static void lib_folder_cb(lv_event_t *e)
{
    if (!tap_is_clean(e)) return;
    int i = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
    if (g_lib_depth >= LIB_DEPTH_MAX) return;                       // don't descend past the cap
    snprintf(g_lib_stack[g_lib_depth], sizeof g_lib_stack[0], "%s", app_player_lib_name(i));
    nav_push(PAGE_LIB, g_lib_depth + 1);                            // ctx = new depth; back restores it
}
static void lib_track_cb(lv_event_t *e)
{
    if (!tap_is_clean(e)) return;
    int i = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
    int trk = app_player_lib_track_of(i);
    if (trk < 0) return;
    char rel[192]; lib_relpath(rel, sizeof rel, g_lib_depth);
    app_player_play_lib(rel, trk);
    app_store_set_last_lib(rel, trk, app_player_lib_name(i), 0);
    app_store_hist_push_lib(rel, app_player_lib_name(i), trk, 0);
    nav_push(PAGE_PLAYER, 0);
}
// ---------- Library: jump to a letter ----------
// The listing is sorted, so the jump scans for the first entry with each initial.
static int lib_first_with_initial(char want)
{
    int n = app_player_lib_count();
    for (int i = 0; i < n; i++) {
        const char *nm = app_player_lib_name(i);
        if (!nm || !nm[0]) continue;
        char c = (char)toupper((unsigned char)nm[0]);
        if (want == '#') {                       // digits and symbols bucket
            if (!isalpha((unsigned char)c)) return i;
        } else if (c == want) {
            return i;
        }
    }
    return -1;
}
static void lib_letter_cb(lv_event_t *e)
{
    if (!tap_is_clean(e)) return;
    char want = (char)(intptr_t)lv_event_get_user_data(e);
    int at = lib_first_with_initial(want);
    close_modal();
    if (at < 0) return;                          // nothing under that letter: leave the list put
    g_lib_off = (at / LIB_PAGE) * LIB_PAGE;      // page containing it
    g_lib_jumpto = at;                           // row to scroll into view after the rebuild
    g_lib_keepoff = true;
    g_anim = LV_SCR_LOAD_ANIM_NONE;
    build_lib(g_lib_depth);
}
#define LETTER_W   240   // four 48px letters fit with real gaps
#define LETTER_H   48
static void open_letter_picker(void)
{
    // Four columns of circular letter keys share the option-card width. The shorter
    // 48px keys fit four rows; modal_card_ex keeps the outer card inside the bezel.
    lv_obj_t *cardv = modal_card_ex("Jump to", LETTER_W, LETTER_H, 4);
    static const char *LETTERS = "ABCDEFGHIJKLMNOPQRSTUVWXYZ#";
    const int per_row = 4;   // 4 x 48px inside 216px of inner width = 8px between each
    lv_obj_t *rowo = NULL;
    for (int i = 0; LETTERS[i]; i++) {
        if (i % per_row == 0) {
            rowo = flat(cardv);
            lv_obj_set_size(rowo, lv_pct(100), LETTER_H);
            lv_obj_set_flex_flow(rowo, LV_FLEX_FLOW_ROW);
            // START with a fixed 8px gap, not SPACE_BETWEEN, so a short last row stays
            // under the columns above.
            lv_obj_set_flex_align(rowo, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_column(rowo, GAP, 0);
        }
        lv_obj_t *b = lv_btn_create(rowo);
        lv_obj_set_size(b, LETTER_H, LETTER_H);
        lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(COL_CARD2), 0);
        lv_obj_set_style_shadow_width(b, 0, 0);
        char t[2] = { LETTERS[i], 0 };
        lv_obj_center(lbl(b, t, &lv_font_montserrat_18, COL_TEXT));
        lv_obj_add_event_cb(b, lib_letter_cb, LV_EVENT_ALL, (void *)(intptr_t)LETTERS[i]);
    }
}
static void lib_jump_cb(lv_event_t *e) { if (!tap_is_clean(e)) return; open_letter_picker(); }

// Shuffle this folder and everything nested under it.
static void lib_shuffle_cb(lv_event_t *e)
{
    if (!tap_is_clean(e)) return;
    char rel[192]; lib_relpath(rel, sizeof rel, g_lib_depth);
    app_player_play_lib_shuffled(rel);
    nav_push(PAGE_PLAYER, 0);
}
// Prev/Next pager: user_data carries the offset delta (+/- LIB_PAGE); rebuild the same
// folder in place (no nav push), keeping g_lib_off.
static void lib_page_cb(lv_event_t *e)
{
    if (!tap_is_clean(e)) return;
    g_lib_off += (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
    if (g_lib_off < 0) g_lib_off = 0;
    g_lib_keepoff = true;
    g_anim = LV_SCR_LOAD_ANIM_NONE;
    build_lib(g_lib_depth);
}
enum { LIB_ROW_ENTRY, LIB_ROW_SHUFFLE, LIB_ROW_JUMP, LIB_ROW_PREV, LIB_ROW_NEXT };
_Static_assert(LIB_PAGE + 3 <= LIST_VIEW_MAX, "Library page and action rows must fit");
static void library_add_row(int type, int entry, bool done)
{
    char title[96];
    shema_icon_id_t symbol;
    lv_event_cb_t callback;
    int data = entry;
    uint32_t text_color = done ? COL_DONE_T : COL_TEXT;
    switch (type) {
    case LIB_ROW_ENTRY:
        snprintf(title, sizeof title, "%s", app_player_lib_name(entry));
        symbol = done ? SHEMA_ICON_CHECK : app_player_lib_is_dir(entry) ? SHEMA_ICON_FOLDER : SHEMA_ICON_AUDIO;
        callback = app_player_lib_is_dir(entry) ? lib_folder_cb : lib_track_cb;
        break;
    case LIB_ROW_SHUFFLE:
        snprintf(title, sizeof title, "Shuffle all"); symbol = SHEMA_ICON_SHUFFLE; callback = lib_shuffle_cb; break;
    case LIB_ROW_JUMP:
        snprintf(title, sizeof title, "Jump to a letter"); symbol = SHEMA_ICON_LIST; callback = lib_jump_cb; break;
    case LIB_ROW_PREV:
        snprintf(title, sizeof title, "Previous %d", LIB_PAGE); symbol = SHEMA_ICON_UP; callback = lib_page_cb; data = -LIB_PAGE; break;
    default:
        snprintf(title, sizeof title, "Next %d", LV_MIN(entry, LIB_PAGE)); symbol = SHEMA_ICON_DOWN; callback = lib_page_cb; data = LIB_PAGE; break;
    }
    list_view_add(title, NULL, symbol, text_color,
                  false, 0, type != LIB_ROW_ENTRY, callback, data);
}
// depth comes from nav ctx: 0 = root, N = N levels deep (path = g_lib_stack[0..N-1]).
// Large folders are paged, and only visible rows own LVGL objects.
static void build_lib(int depth)
{
#if BIBLE_SCREENSHOTS
    uint32_t perf_start = lv_tick_get(), perf_metadata = 0;
#endif
    if (depth < 0) depth = 0; else if (depth > LIB_DEPTH_MAX) depth = LIB_DEPTH_MAX;
    g_lib_depth = depth;
    if (g_lib_keepoff) g_lib_keepoff = false; else g_lib_off = 0;   // fresh nav starts at the top
    char rel[192]; lib_relpath(rel, sizeof rel, depth);
    app_player_lib_browse(rel);
#if BIBLE_SCREENSHOTS
    uint32_t perf_browsed = lv_tick_get();
#endif

    lv_obj_t *c = make_page(depth ? g_lib_stack[depth - 1] : "Library", true);
    page_content_bottom(c, GRID_BOT);
    int n = app_player_lib_count();

    if (capture_empty_lists) n = 0;
    // Empty folder: show an explicit empty state.
    if (n <= 0) {
        empty_state(c, SHEMA_ICON_FOLDER, "Nothing here.");
        g_lib_jumpto = -1;
        return;
    }

    lv_obj_t *list = new_scroll_list(c);
    if (!list_view_begin(list)) { lv_obj_del(list); empty_state(c, SHEMA_ICON_WARNING, "Unable to open list."); return; }
    if (g_lib_off >= n) g_lib_off = 0;
    int end = g_lib_off + LIB_PAGE; if (end > n) end = n;

    // Shuffle this folder and its sub-folders. Not offered at the LIBRARY root, where
    // the entries are unrelated categories.
    if (g_lib_depth > 0 && g_lib_off == 0 && n > 0) {
        library_add_row(LIB_ROW_SHUFFLE, 0, false);
    }
    // Alphabet jump for large folders, on every page (unlike Shuffle).
    if (n > LIB_JUMP_MIN) {
        library_add_row(LIB_ROW_JUMP, n, false);
    }
    if (g_lib_off > 0) {                                            // "Previous" pager row
        library_add_row(LIB_ROW_PREV, 0, false);
    }
    for (int i = g_lib_off; i < end; i++) {
        bool dir = app_player_lib_is_dir(i);
        // Finished tracks show a tick and the done colour; a folder does only when every
        // track beneath it is finished.
        char track_rel[192];
#if BIBLE_SCREENSHOTS
        uint32_t perf_row = lv_tick_get();
#endif
        bool done = dir ? app_player_lib_folder_done(i)
                        : (app_player_lib_rel(i, track_rel, sizeof track_rel) && app_played_get_lib(track_rel, NULL));
#if BIBLE_SCREENSHOTS
        perf_metadata += lv_tick_get() - perf_row;
#endif
        library_add_row(LIB_ROW_ENTRY, i, done);
    }
    if (end < n) {                                                  // "Next" pager row
        library_add_row(LIB_ROW_NEXT, n - end, false);
    }
    int initial_row = -1;
    for (int i = 0; g_lib_jumpto >= 0 && i < list_view.count; i++)
        if (list_view.rows[i].data == g_lib_jumpto &&
            (list_view.rows[i].callback == lib_folder_cb || list_view.rows[i].callback == lib_track_cb)) initial_row = i;
    list_view_finish(initial_row);
    g_lib_jumpto = -1;
#if BIBLE_SCREENSHOTS
    uint32_t perf_built = lv_tick_get();
    lv_obj_update_layout(lv_scr_act());
    ESP_LOGI(TAG, "LIBPERF %s entries=%d browse=%lu metadata=%lu widgets=%lu layout=%lu total=%lu ms",
             rel[0] ? rel : "(root)", n, (unsigned long)(perf_browsed - perf_start),
             (unsigned long)perf_metadata, (unsigned long)(perf_built - perf_browsed - perf_metadata),
             (unsigned long)(lv_tick_get() - perf_built), (unsigned long)(lv_tick_get() - perf_start));
#endif
}

// ---------- dispatch ----------
static void build_page(int page, int ctx)
{
#if BIBLE_SCREENSHOTS
    uint32_t perf_start = lv_tick_get();
#endif
    grid_view.list = NULL;
    list_view.list = NULL;   // old list scroll/delete events cannot rebind a new page
    close_modal();
    reset_label_caches();   // rebuilt widgets must be refreshed on the next tick
    cur_page = page;
    // Hide Home's top-layer hit targets immediately on navigation. Waiting for
    // the 400ms UI tick lets them intercept the first Back tap on the new page.
    if (mb_box && mb_play) {
        if (page == PAGE_HOME) {
            lv_obj_clear_flag(mb_box, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(mb_play, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(mb_box, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(mb_play, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(mb_prog, LV_OBJ_FLAG_HIDDEN);
            lc_rimvis = 0;
        }
        lc_mbvis = page == PAGE_HOME;
    }
    // Drop every cross-page handle before the old screen is deleted, so nothing points
    // at a freed widget.
    pw_title = NULL;
    g_set_cont = NULL;
    g_scrto_val = g_pwroff_val = g_version_val = g_bright_val = NULL;
    if (page != PAGE_SETTINGS) g_set_scroll = -1;   // arriving fresh starts at the top
    switch (page) {
    case PAGE_HOME:        build_home(); break;
    case PAGE_BOOKS:       build_books(ctx); break;
    case PAGE_CHAPTERS:    build_chapters(ctx); break;
    case PAGE_PLAYER:      build_player(); break;
    case PAGE_LIST_DETAIL: detail_edit = false; build_list_detail(ctx); break;
    case PAGE_SETTINGS:    build_settings(); break;
    case PAGE_BIY:         build_biy(); break;
    case PAGE_BIY_DAYS:    build_biy_days(ctx); break;
    case PAGE_RECENT:      build_recent(); break;
    case PAGE_LIB:         build_lib(ctx); break;
    case PAGE_FAVOURITES:  build_favourites(); break;
    }
#if BIBLE_SCREENSHOTS
    lv_obj_update_layout(lv_scr_act());
    ESP_LOGI(TAG, "UIPERF page=%d ctx=%d build=%lu ms", page, ctx, (unsigned long)(lv_tick_get() - perf_start));
#endif
}

// Kept out of release firmware, including its strings and route table.
#if BIBLE_SCREENSHOTS
#include "capture_ui.inc"
#endif

static void apply_accent(uint32_t hex)
{
    g_accent_hex = hex;
    app_store_set_theme((int)hex);
    lv_disp_t *disp = lv_disp_get_default();
    lv_theme_t *th = lv_theme_default_init(disp, lv_color_hex(hex), lv_color_hex(0x6b7280),
                                           true, &lv_font_montserrat_16);
    install_flat_theme(th);        // strip per-widget shadows before anything is built
    lv_disp_set_theme(disp, th);
    close_modal();                  // delete + NULL any open modal BEFORE clearing the layer,
    lv_obj_clean(lv_layer_top());   // else lv_obj_clean frees it and leaves `modal` dangling
    make_top_bars();
    g_anim = LV_SCR_LOAD_ANIM_NONE;
    build_page(nav_stack[nav_sp].page, nav_stack[nav_sp].ctx);
}

// ---------- top-layer chrome (battery + now-playing pill) ----------
static bool nothing_loaded(const player_status_t *st)
{
    return st->book_idx < 0 && st->biy_day < 0 && st->biy_intro < 0 && st->lib_idx < 0;
}
// Resume the last-played track (BIBLE, BIY, or LIBRARY) from its saved position (no nav).
static void resume_last(void)
{
    // The play calls below only queue a command, so the player's "stopped" status can
    // lag by tens of ms and a double tap would restart the track. Latch the intent for 1.5 s.
    static uint32_t last_req_ms;
    uint32_t now = lv_tick_get();
    if (last_req_ms && (uint32_t)(now - last_req_ms) < 1500) return;
    last_req_ms = now;

    int k = app_store_get_last_kind();
    uint32_t pos = 0;
    if (k == 0) { int b, ch; if (!app_store_get_last(&b, &ch, &pos)) return; app_player_play(b, ch); }
    else if (k == 1) { int day, in; if (!app_store_get_last_biy(&day, &in, &pos)) return;
                       if (in >= 0) app_player_play_biy_intro(in); else app_player_play_biy(day); }
    else if (k == 2) { char dir[192]; int idx;
                       if (!app_store_get_last_lib(dir, sizeof dir, &idx, NULL, 0, &pos)) return;
                       app_player_play_lib(dir, idx); }
    else return;
    if (pos > 3000) app_player_seek_ms(pos);
}
static void minibar_cb(lv_event_t *e)
{
    if (!tap_is_clean(e)) return;
    player_status_t st; app_player_get_status(&st);
    if (st.state == PLAYER_STOPPED && nothing_loaded(&st)) resume_last();
    if (cur_page != PAGE_PLAYER) nav_push(PAGE_PLAYER, 0);
}
static void minibar_play_cb(lv_event_t *e)
{
    if (!tap_is_clean(e)) return;
    player_status_t st; app_player_get_status(&st);
    if (st.state == PLAYER_STOPPED && nothing_loaded(&st)) resume_last();
    else app_player_toggle_pause();
}
// The now-playing bar's two lines.
//
// Line 1 is what is playing; line 2 is the context it is playing in plus the clock.
// Both buffers are always written, so the caller can compare against its cache
// unconditionally.
static void minibar_lines(const player_status_t *st, char *l1, int n1, char *l2, int n2)
{
    l1[0] = l2[0] = 0;
    if (!nothing_loaded(st)) {
        status_line(st, l1, n1);
        char ctx[48] = "";
        if (st->kind == TRACK_BIY) {
            int per = (st->biy_day >= 1) ? biy_period_of(st->biy_day) : -1;
            snprintf(ctx, sizeof ctx, "%s", per >= 0 ? BIY_PERIODS[per].name : "Bible in a Year");
        } else if (st->kind == TRACK_LIB) {
            snprintf(ctx, sizeof ctx, "%s", app_player_lib_playing_folder());
        } else if (app_player_versions() > 0) {
            snprintf(ctx, sizeof ctx, "%s", app_player_version_short(g_version_idx));
        }
        if (st->dur_ms > 0) {
            char a[16], d[16];
            fmt_time(a, sizeof a, st->pos_ms);
            fmt_time(d, sizeof d, st->dur_ms);
            if (ctx[0]) snprintf(l2, n2, "%s  " LV_SYMBOL_BULLET "  %s / %s", ctx, a, d);
            else        snprintf(l2, n2, "%s / %s", a, d);
        } else {
            snprintf(l2, n2, "%s", ctx);
        }
        return;
    }

    // Nothing loaded: offer the saved resume point, named the same way the player names it.
    switch (app_store_get_last_kind()) {
    case 0: {
        int b, c; uint32_t p;
        if (app_store_get_last(&b, &c, &p)) {
            snprintf(l1, n1, "%s %d", BIBLE_BOOKS[b].name, c);
            if (p > 3000) { char a[16]; fmt_time(a, sizeof a, p); snprintf(l2, n2, "Resume at %s", a); }
            else snprintf(l2, n2, "Tap to play");
            return;
        }
        break;
    }
    case 1: {
        int day, in; uint32_t p;
        if (app_store_get_last_biy(&day, &in, &p)) {
            if (in >= 0) snprintf(l1, n1, "%s", app_player_biy_intro_title(in));
            else         snprintf(l1, n1, "Day %d", day);
            if (p > 3000) { char a[16]; fmt_time(a, sizeof a, p); snprintf(l2, n2, "Bible in a Year  " LV_SYMBOL_BULLET "  Resume at %s", a); }
            else snprintf(l2, n2, "Bible in a Year");
            return;
        }
        break;
    }
    case 2: {
        char nm[STORE_LIB_NAME_LEN]; uint32_t p = 0;
        if (app_store_get_last_lib(NULL, 0, NULL, nm, sizeof nm, &p) && nm[0]) {
            snprintf(l1, n1, "%s", nm);
            if (p > 3000) { char a[16]; fmt_time(a, sizeof a, p); snprintf(l2, n2, "Library  " LV_SYMBOL_BULLET "  Resume at %s", a); }
            else snprintf(l2, n2, "Library");
            return;
        }
        break;
    }
    default: break;
    }
    snprintf(l1, n1, "Nothing playing");
    snprintf(l2, n2, "Choose something to listen to");
}

static void make_top_bars(void)
{
    lv_obj_t *top = lv_layer_top();
    lv_obj_clear_flag(top, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    // Battery at 12 o'clock on the top layer, clear of the player's rim arc. The LVGL
    // battery symbol shows the fill level.
    sb_batt = lbl(top, LV_SYMBOL_BATTERY_FULL " --%", &lv_font_montserrat_16, COL_TEXT);
    lv_obj_align(sb_batt, LV_ALIGN_TOP_MID, 0, 22);   // a few px clear of the player's rim arc

    // Bottom "now playing": a progress arc hugging the bottom rim (follows the
    // circle, so no clipped rectangular corners), with a compact title + play
    // button centred just inside it. Tapping the title opens the player.
    mb_prog = lv_arc_create(top);
    lv_obj_set_size(mb_prog, 352, 352);
    lv_obj_center(mb_prog);
    // Full rim ring starting at 12 o'clock, matching the player's arc.
    lv_arc_set_rotation(mb_prog, 270);
    lv_arc_set_bg_angles(mb_prog, 0, 360);
    lv_arc_set_range(mb_prog, 0, 1000);
    lv_arc_set_value(mb_prog, 0);
    lv_obj_remove_style(mb_prog, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(mb_prog, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(mb_prog, 6, LV_PART_MAIN);
    lv_obj_set_style_arc_width(mb_prog, 6, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(mb_prog, lv_color_hex(COL_TRACK), LV_PART_MAIN);
    lv_obj_set_style_arc_color(mb_prog, lv_color_hex(g_accent_hex), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(mb_prog, true, LV_PART_INDICATOR);

    // Metadata and play are sibling targets. The text box ends at y289; the
    // footer starts at y290, so neither clips nor steals the other's taps.
    mb_box = flat(top);
    lv_obj_set_size(mb_box, 224, MINIBAR_TEXT_H);
    lv_obj_set_pos(mb_box, (LV_HOR_RES - 224) / 2, MINIBAR_TEXT_Y);
    lv_obj_add_flag(mb_box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(mb_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(mb_box, minibar_cb, LV_EVENT_ALL, NULL);

    mb_title = lbl(mb_box, "Nothing playing", &lv_font_montserrat_18, COL_TEXT);
    lv_obj_set_width(mb_title, 216);
    lv_label_set_long_mode(mb_title, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(mb_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(mb_title, LV_ALIGN_TOP_MID, 0, 0);

    // Second line: context + clock, 14px metadata.
    mb_sub = lbl(mb_box, "", &lv_font_montserrat_14, COL_SUB);
    lv_obj_set_width(mb_sub, 216);
    lv_label_set_long_mode(mb_sub, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(mb_sub, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align_to(mb_sub, mb_title, LV_ALIGN_OUT_BOTTOM_MID, 0, 2);

    lv_obj_t *disc;
    mb_play = bottom_control(top, BOTTOM_TOUCH_W, 56, g_accent_hex,
                             minibar_play_cb, LV_EVENT_ALL, &disc);
    playpause_icons(disc, 24, COL_BG, &mb_ic_play, &mb_ic_pause);
}

static void ui_tick(lv_timer_t *t)
{
    (void)t;
#if BIBLE_SCREENSHOTS
    if (capture_static_frame) return;
#endif
    if (go_home_req) { go_home_req = false; nav_reset(PAGE_HOME, 0); }
    // Card hot-plugged: content is already rescanned; rebuild the "No SD card" Home
    // screen. Other pages pick it up on the next navigation.
    if (sd_ready_req) { sd_ready_req = false; if (cur_page == PAGE_HOME) nav_reset(PAGE_HOME, 0); }

    player_status_t st;
    app_player_get_status(&st);
    bool playing = (st.state == PLAYER_PLAYING);

    // Skip all rendering while the panel is off: otherwise LVGL still renders full frames
    // to PSRAM at 2.5 Hz (all night during screen-off playback). The panel repaints on
    // wake via bible_display_sleep; refresh the caches on the off->on edge so the first
    // visible tick redraws everything.
    bool off = app_power_screen_is_off();
    static bool was_off;
    if (was_off && !off) reset_label_caches();
    was_off = off;

    if (!off) {
        int pct = app_power_battery_pct();
        const char *sym = pct > 80 ? LV_SYMBOL_BATTERY_FULL : pct > 55 ? LV_SYMBOL_BATTERY_3 :
                          pct > 30 ? LV_SYMBOL_BATTERY_2 : pct > 10 ? LV_SYMBOL_BATTERY_1 : LV_SYMBOL_BATTERY_EMPTY;
        char b[32];
        // No charging indicator: charge status is not wired to a GPIO, and inferring it
        // from the voltage trend is not reliable enough to display.
        snprintf(b, sizeof(b), "%s %d%%", sym, pct);
        label_set(sb_batt, lc_batt, sizeof(lc_batt), b);
        // Turn the gauge red near empty, ahead of the low-battery auto-shutdown.
        int low = app_power_battery_low() ? 1 : 0;
        if (lc_battlow != low) {
            lc_battlow = low;
            lv_obj_set_style_text_color(sb_batt, lv_color_hex(low ? COL_DANGER : COL_TEXT), 0);
        }

        int prog = (st.dur_ms > 0) ? (int)((uint64_t)st.pos_ms * 1000 / st.dur_ms) : 0;

        // now-playing bar (pill + rim arc) shown only on Home — toggle only on change,
        // since add/clear HIDDEN invalidates (and full_refresh makes that a full frame).
        int mbvis = (cur_page == PAGE_HOME) ? 1 : 0;
        if (lc_mbvis != mbvis) {
            lc_mbvis = mbvis;
            if (mbvis) {
                lv_obj_clear_flag(mb_box, LV_OBJ_FLAG_HIDDEN);
                lv_obj_clear_flag(mb_play, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(mb_box, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(mb_play, LV_OBJ_FLAG_HIDDEN);
            }
        }
        // A progress ring carries information only when a track is loaded. Avoid
        // framing the idle home screen with a heavy, permanently empty grey ring.
        int rimvis = mbvis && !nothing_loaded(&st);
        if (lc_rimvis != rimvis) {
            lc_rimvis = rimvis;
            if (rimvis) lv_obj_clear_flag(mb_prog, LV_OBJ_FLAG_HIDDEN);
            else lv_obj_add_flag(mb_prog, LV_OBJ_FLAG_HIDDEN);
        }
        if (lc_mbplaying != (int)playing) {
            lc_mbplaying = playing;
            playpause_set(mb_ic_play, mb_ic_pause, playing);
        }
        // BIY period colour for the rim arc (else theme accent); cached, style sets invalidate.
        int mbacc = (int)status_accent(&st);
        if (lc_mbacc != mbacc) { lc_mbacc = mbacc; lv_obj_set_style_arc_color(mb_prog, lv_color_hex(mbacc), LV_PART_INDICATOR); }
        lv_arc_set_value(mb_prog, prog);   // lv_arc_set_value early-returns if unchanged
        char m1[64], m2[64];
        minibar_lines(&st, m1, sizeof m1, m2, sizeof m2);
        label_set(mb_title, lc_mbtitle, sizeof(lc_mbtitle), m1);
        label_set(mb_sub,   lc_mbsub,   sizeof(lc_mbsub),   m2);

        // player watch-face live update
        if (cur_page == PAGE_PLAYER && pw_title) {
            if (!nothing_loaded(&st)) {
                char title[80], sub[80];
                status_title(&st, title, sizeof title, sub, sizeof sub);
                // Re-clamping measures the wrapped text, so keep it behind the cache guard.
                if (strcmp(lc_pwtitle, title) != 0 || strcmp(lc_pwsub, sub) != 0) {
                    snprintf(lc_pwtitle, sizeof(lc_pwtitle), "%s", title);
                    snprintf(lc_pwsub, sizeof(lc_pwsub), "%s", sub);
                    player_set_title(title, sub);
                }
                int acc = (int)status_accent(&st);             // BIY period colour (else accent)
                if (lc_pwacc != acc) {
                    lc_pwacc = acc;
                    lv_obj_set_style_text_color(pw_sub, lv_color_hex(acc), 0);
                    lv_obj_set_style_arc_color(pw_arc, lv_color_hex(acc), LV_PART_INDICATOR);
                }
                // Favourite button is shown for any loaded track.
                if (lc_favvis != 1) {
                    lc_favvis = 1;
                    lv_obj_clear_flag(pw_favbtn, LV_OBJ_FLAG_HIDDEN);
                }
                // Saved state uses two cues: an accent heart and an accent-toned button.
                int isfav = cur_is_fav(&st) ? 1 : 0;
                if (lc_fav != isfav) {
                    lc_fav = isfav;
                    shema_icon_set_symbol(pw_fav, isfav ? SHEMA_ICON_HEART_FILLED : SHEMA_ICON_HEART, 24);
                    shema_icon_set_color(pw_fav, lv_color_hex(isfav ? g_accent_hex : COL_SUB));
                    lv_obj_set_style_bg_color(pw_favbtn, lv_color_hex(isfav ? 0x3A3221 : COL_CARD2), 0);
                }
            } else if (strcmp(lc_pwtitle, "Nothing playing") != 0) {
                // Nothing loaded: say so, rather than leaving the placeholder dashes
                // the page was built with (or a stale title from the last track).
                snprintf(lc_pwtitle, sizeof(lc_pwtitle), "Nothing playing");
                lc_pwsub[0] = '\0';
                player_set_title("Nothing playing", "");
            }
            // An armed sleep timer lights the moon AND its button, the same two-channel
            // treatment the favourite button gets.
            int slp = sleep_active ? 1 : 0;
            if (lc_sleep != slp) {
                lc_sleep = slp;
                shema_icon_set_symbol(pw_sleep, slp ? SHEMA_ICON_MOON_FILLED : SHEMA_ICON_MOON, 24);
                shema_icon_set_color(pw_sleep, lv_color_hex(slp ? g_accent_hex : COL_SUB));
                lv_obj_set_style_bg_color(pw_sleepbtn, lv_color_hex(slp ? 0x3A3221 : COL_CARD2), 0);
            }
            if (lc_pwplaying != (int)playing) {
                lc_pwplaying = playing;
                playpause_set(pw_ic_play, pw_ic_pause, playing);
            }
            lv_arc_set_value(pw_arc, prog);
            if (!lv_obj_has_state(pw_seek, LV_STATE_PRESSED)) lv_slider_set_value(pw_seek, prog, LV_ANIM_OFF);
            char a[16], d[16];
            fmt_time(a, sizeof(a), st.pos_ms);
            fmt_time(d, sizeof(d), st.dur_ms);
            label_set(pw_cur, lc_pwcur, sizeof(lc_pwcur), a);
            label_set(pw_tot, lc_pwtot, sizeof(lc_pwtot), d);
        }
    }

    // These run even while the panel is off (audio may still be playing).
    uint32_t nowms = esp_log_timestamp();
    if (playing && (int32_t)(nowms - last_save_ms) > 5000) {
        last_save_ms = nowms;
        if (st.kind == TRACK_BIY) { app_store_set_last_biy(st.biy_day, st.biy_intro, st.pos_ms); app_store_hist_push(1, st.biy_day, st.biy_intro, st.pos_ms); }
        else if (st.kind == TRACK_LIB) {
            app_store_set_last_lib(app_player_lib_playing_dir(), st.lib_idx, app_player_lib_playing_name(), st.pos_ms);
            app_store_hist_push_lib(app_player_lib_playing_dir(), app_player_lib_playing_name(), st.lib_idx, st.pos_ms);
        }
        else { app_store_set_last(st.book_idx, st.chapter, st.pos_ms); app_store_hist_push(0, st.book_idx, st.chapter, st.pos_ms); }
    }
    // Signed-delta comparison so the deadline is wrap-safe and a deadline of 0 is a valid
    // time rather than a "disabled" sentinel.
    if (sleep_active && (int32_t)(nowms - sleep_deadline_ms) >= 0) {
        sleep_active = false;
        if (st.state == PLAYER_PLAYING) app_player_toggle_pause();
    }
}

static void boot_home_cb(void) { go_home_req = true; }

// Scan a (freshly) mounted card: pick the audio version + BIY language. Pure
// data work (no LVGL), so it is safe to run off the LVGL task — e.g. from the
// SD-monitor task on hot-plug. Idempotent.
static void sd_content_rescan(void)
{
    // Pick the audio version: the saved one if still present, else the first found.
    int nver = app_player_versions();
    if (nver > 0) {
        char saved[96];
        app_store_get_version(saved, sizeof saved, "");
        int sel = saved[0] ? app_player_find_version(saved) : -1;
        if (sel < 0)                                   // default preference: POC Dramatized
            for (int i = 0; i < nver; i++)
                if (strstr(app_player_version_dir(i), "POC_Dramatized")) { sel = i; break; }
        g_version_idx = (sel >= 0) ? sel : 0;
        app_player_set_base(app_player_version_dir(g_version_idx));
        app_store_set_version(app_player_version_dir(g_version_idx));   // persist so Settings reflects it
    }
    char biyl[24]; app_store_get_biy_lang(biyl, sizeof biyl, "");
    g_biy_lang = app_player_biy_find_lang(biyl);   // -1 if the stored language isn't on this card
    if (g_biy_lang < 0) g_biy_lang = 0;            // default to the first available language
    app_player_biy_set_lang(g_biy_lang);
}

// Poll for card insertion when none was present at boot. Runs at low priority
// off the LVGL/audio tasks; on success it rescans content (off-thread) then
// signals ui_tick to rebuild the screen, and exits. Card removal is not handled.
static void sd_monitor_task(void *arg)
{
    (void)arg;
    while (!sdcard_is_mounted()) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        if (sdcard_try_mount()) {
            ESP_LOGI(TAG, "SD card inserted -> mounted, rescanning content");
            sd_content_rescan();
            sd_ready_req = true;      // ui_tick rebuilds the screen under the LVGL lock
            break;
        }
    }
    vTaskDelete(NULL);
}

void bible_app_start(void)
{
    ESP_LOGI(TAG, "UI start (360x360 round, watch-style)");
    g_accent_hex = (uint32_t)app_store_get_theme(ACCENT_DEFAULT);
    g_swipe_back = app_store_get_swipe_back(true);   // on by default; see screen_gesture_cb
    lv_disp_t *disp = lv_disp_get_default();
    // Root opacity transitions expose the display backdrop. LVGL defaults it
    // to white; match the page ground so navigation never flashes the panel.
    lv_disp_set_bg_color(disp, lv_color_hex(COL_BG));
    lv_disp_set_bg_opa(disp, LV_OPA_COVER);
    lv_theme_t *th = lv_theme_default_init(disp, lv_color_hex(g_accent_hex), lv_color_hex(0x6b7280),
                                           true, &lv_font_montserrat_16);
    install_flat_theme(th);        // strip per-widget shadows before anything is built
    lv_disp_set_theme(disp, th);

    app_player_set_repeat_all(app_store_get_repeat_all(false));
    app_player_set_volume(app_store_get_volume(70));

    sd_content_rescan();

    // No card at boot? Watch for one and load it without a reboot.
    if (!sdcard_is_mounted())
        xTaskCreate(sd_monitor_task, "sdmon", 4096, NULL, 3, NULL);

    app_power_set_boot_cb(boot_home_cb);
    make_top_bars();
    lv_obj_set_style_opa(lv_layer_top(), LV_OPA_TRANSP, 0);
    startup_transition = true;
    nav_reset(PAGE_HOME, 0);
    ui_tick(NULL); // initial labels/state are ready before the first transition frame
    lv_timer_create(ui_tick, 400, NULL);
}
