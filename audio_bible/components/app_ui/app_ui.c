// app_ui — round-native LVGL UI for the Audio Bible (360x360 circular screen).
//
// Watch-style design language:
//  - Now Playing is a "watch face": a full-rim progress arc, centred book/chapter,
//    transport on a lower arc, a slim scrub + volume control.
//  - Navigation is by SWIPE (swipe right = back) with sliding screen transitions,
//    so there is no corner back-button for the round bezel to clip. The BOOT
//    button goes Home.
//  - Everything is centre-anchored and kept inside the inscribed circle (SAFE_X),
//    where the glass is widest.
//  - Library is a centred roller; chapters are a ring of tappable circles.
//
// The player/store/power logic underneath is untouched — this is the view layer.
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include "lvgl.h"
#include "esp_log.h"
#include "bsp_lvgl.h"
#include "bible_data.h"
#include "app_player.h"
#include "app_store.h"
#include "app_power.h"

static const char *TAG = "ui";

// ---- palette ----
#define COL_BG     0x05070C
#define COL_CARD   0x141A24
#define COL_CARD2  0x232B39
#define COL_TRACK  0x222934      // arc/slider track
#define ACCENT_DEFAULT 0xE8A33D
#define COL_TEXT   0xF2F3F5
#define COL_SUB    0x8A93A3

// ---- round geometry ----
// Round 360px screen, radius 180, centre (180,180). Usable half-width at a
// vertical offset dy from centre is sqrt(180^2 - dy^2). These bands are chosen so
// a SAFE_X-inset content rectangle (288 wide) and the chrome stay inside the glass:
//   battery y10 | title y48 | content 78..264 (94..264 with a back button) | pill
#define SAFE_X       36          // content inset; inner width 360-72 = 288
#define TITLE_Y      48          // screen title y (clear of the top battery)
#define CONTENT_TOP_PLAIN  78    // content start (no back button)
#define CONTENT_TOP_BACK   94    // content start when a back button is shown
#define MINIBAR_H    80          // taller now-playing bar (bigger play button)
#define MINIBAR_LIFT 18          // lift the now-playing pill off the clipped bottom
// Now-playing bar is Home-only, so non-Home pages reclaim the bottom. 280 keeps
// full-width content inside the round bezel (half-width ~150 >= 144 needed at y280).
#define CONTENT_BOT  280
// Scrolling grids (Books, Chapters, Settings) have no now-playing bar, so they run
// down toward the bottom rim, stopping just above the back button's circle (top ~302).
#define GRID_BOT     300

enum { PAGE_HOME, PAGE_BOOKS, PAGE_CHAPTERS, PAGE_PLAYER, PAGE_LIST_DETAIL, PAGE_SETTINGS };
#define LIST_FAV (-1)

typedef struct { int page; int ctx; } nav_entry_t;
static nav_entry_t nav_stack[10];
static int nav_sp;
static int cur_page;
static lv_scr_load_anim_t g_anim = LV_SCR_LOAD_ANIM_NONE;

// player-page widgets (valid only while cur_page == PAGE_PLAYER)
static lv_obj_t *pw_title, *pw_sub, *pw_arc, *pw_seek, *pw_cur, *pw_tot, *pw_play, *pw_fav, *pw_vol, *pw_sleep;
// books page
static lv_obj_t *bk_list, *bk_ot, *bk_nt;
static int       g_testament = BIBLE_OLD;
static uint32_t  g_accent_hex = ACCENT_DEFAULT;
static bool      detail_edit;
static int       detail_which;
static int       g_version_idx;   // selected index into the app_player version list
// top-layer chrome
static lv_obj_t *sb_batt, *mb_box, *mb_title, *mb_play, *mb_prog;
static lv_obj_t *modal;

static uint32_t sleep_deadline_ms;
static uint32_t last_save_ms;
static volatile bool go_home_req;

static void build_page(int page, int ctx);
static void build_list_detail(int which);
static void make_top_bars(void);
static void apply_accent(uint32_t hex);

// ---------- small helpers ----------
static void encode_play(lv_obj_t *o, int b, int c) { lv_obj_set_user_data(o, (void *)(intptr_t)(b * 1000 + c)); }
static void decode_play(lv_obj_t *o, int *b, int *c) { intptr_t v = (intptr_t)lv_obj_get_user_data(o); *b = v / 1000; *c = v % 1000; }

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
    lv_obj_set_style_bg_color(o, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_radius(o, 16, 0);
}
static void fmt_time(char *b, int sz, uint32_t ms)
{
    uint32_t s = ms / 1000;
    snprintf(b, sz, "%lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
}

// ---------- navigation ----------
static void nav_back(void)
{
    app_power_user_activity();
    if (nav_sp > 0) { nav_sp--; g_anim = LV_SCR_LOAD_ANIM_MOVE_RIGHT; build_page(nav_stack[nav_sp].page, nav_stack[nav_sp].ctx); }
}
static void nav_push(int page, int ctx)
{
    app_power_user_activity();
    if (nav_sp < 9) { nav_sp++; nav_stack[nav_sp] = (nav_entry_t){page, ctx}; }
    g_anim = LV_SCR_LOAD_ANIM_MOVE_LEFT;
    build_page(page, ctx);
}
static void nav_reset(int page, int ctx)
{
    nav_sp = 0; nav_stack[0] = (nav_entry_t){page, ctx};
    g_anim = LV_SCR_LOAD_ANIM_FADE_ON;
    build_page(page, ctx);
}
static void back_btn_cb(lv_event_t *e) { (void)e; nav_back(); }

// Back chevron in a circle, centred in the bottom rim. The visible disc is kept well
// inside the radius, but ext_click_area enlarges the touch target far past it so it's
// easy to hit down in the rim. The player has a full rim arc, so its button stays
// compact and a little higher to clear it (over_arc); every other screen uses a bigger,
// lower button in its free rim. Swipe-right also goes back everywhere.
static void add_back_btn(lv_obj_t *scr, bool over_arc)
{
    int sz  = over_arc ? 42 : 50;     // visible circle diameter
    int off = over_arc ? -16 : -6;    // lift off the bottom edge
    lv_obj_t *b = lv_btn_create(scr);
    lv_obj_set_size(b, sz, sz);
    lv_obj_align(b, LV_ALIGN_BOTTOM_MID, 0, off);
    lv_obj_set_ext_click_area(b, 24);   // big invisible touch target so it's easy to hit in the rim
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(COL_CARD2), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_center(lbl(b, LV_SYMBOL_LEFT, &lv_font_montserrat_24, g_accent_hex));
    lv_obj_add_event_cb(b, back_btn_cb, LV_EVENT_CLICKED, NULL);
}

// Swipe-right anywhere = back. Vertical swipes are left to scrolling.
static void screen_gesture_cb(lv_event_t *e)
{
    (void)e;
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) return;
    if (lv_indev_get_gesture_dir(indev) == LV_DIR_RIGHT) nav_back();
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
        // Back button lives in the bottom rim now, so the title always gets the
        // full centred width.
        lv_obj_t *t = lbl(scr, title, &lv_font_montserrat_18, COL_TEXT);
        lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
        lv_obj_set_width(t, 216);
        lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(t, LV_ALIGN_TOP_MID, 0, TITLE_Y);
    }

    // The top only needs to clear the title now (back moved to the bottom). Titled
    // pages start just under the title; titleless pages (e.g. Books) keep the larger
    // inset since they place their own top header.
    int ctop = (title && title[0]) ? CONTENT_TOP_PLAIN : CONTENT_TOP_BACK;
    lv_obj_t *cont = flat(scr);
    lv_obj_set_pos(cont, 0, ctop);
    lv_obj_set_size(cont, LV_HOR_RES, CONTENT_BOT - ctop);
    lv_obj_set_style_pad_left(cont, SAFE_X, 0);
    lv_obj_set_style_pad_right(cont, SAFE_X, 0);
    lv_obj_set_style_pad_row(cont, 8, 0);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);

    lv_scr_load_anim(scr, g_anim, 180, 0, true);
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
    int target = (int)(intptr_t)lv_event_get_user_data(e);
    if (target == PAGE_LIST_DETAIL) nav_push(PAGE_LIST_DETAIL, LIST_FAV);
    else if (target == PAGE_BOOKS) {
        int cb = -1; current_ref(&cb, NULL);    // open on the current book's testament
        nav_push(PAGE_BOOKS, (cb >= 0) ? BIBLE_BOOKS[cb].testament : BIBLE_OLD);
    }
    else nav_push(target, BIBLE_OLD);
}
static lv_obj_t *hub_btn(lv_obj_t *p, const char *icon, const char *name, int target)
{
    lv_obj_t *b = lv_btn_create(p);
    lv_obj_set_size(b, 84, 84);              // small square icon tile
    card(b);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(b, 4, 0);
    lv_obj_set_style_pad_row(b, 6, 0);
    lv_obj_add_event_cb(b, home_open_cb, LV_EVENT_CLICKED, (void *)(intptr_t)target);
    lbl(b, icon, &lv_font_montserrat_24, g_accent_hex);
    lv_obj_t *cap = lbl(b, name, &lv_font_montserrat_12, COL_TEXT);
    lv_obj_set_width(cap, lv_pct(100));
    lv_label_set_long_mode(cap, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(cap, LV_TEXT_ALIGN_CENTER, 0);
    return b;
}
// The SD card mounts /sdcard only on success, so opendir failing == no usable card.
static bool sd_present(void)
{
    DIR *d = opendir("/sdcard");
    if (d) { closedir(d); return true; }
    return false;
}
static void build_home(void)
{
    // Title shows the selected bible (version); falls back when no card is present.
    const char *home_title = (app_player_versions() > 0) ? app_player_version_short(g_version_idx) : "Audio Bible";
    lv_obj_t *c = make_page(home_title, false);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(c, 16, 0);

    if (!sd_present()) {
        // No card -> nothing can play; warn up top (the tiles below still work, e.g. Settings).
        lv_obj_t *w = lv_obj_create(c);
        lv_obj_set_width(w, lv_pct(100));
        lv_obj_set_height(w, LV_SIZE_CONTENT);
        card(w);
        lv_obj_set_style_bg_color(w, lv_color_hex(0x3A1620), 0);   // dark warning red
        lv_obj_clear_flag(w, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(w, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(w, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_all(w, 10, 0);
        lv_obj_set_style_pad_column(w, 8, 0);
        lbl(w, LV_SYMBOL_WARNING, &lv_font_montserrat_20, 0xE8A33D);
        lbl(w, "No SD card", &lv_font_montserrat_16, COL_TEXT);
    }

    // Square icon tiles in a centred row.
    lv_obj_t *row = flat(c);
    lv_obj_set_width(row, LV_SIZE_CONTENT);
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 12, 0);
    hub_btn(row, LV_SYMBOL_LIST,     "Bible",      PAGE_BOOKS);
    hub_btn(row, LV_SYMBOL_AUDIO,    "Favourites", PAGE_LIST_DETAIL);
    hub_btn(row, LV_SYMBOL_SETTINGS, "Settings",   PAGE_SETTINGS);
}

// ---------- tap vs. scroll guard ----------
// The CST816S drops move samples on fast flicks and LVGL only polls touch every
// read-period ms, so a quick scroll-flick on a grid tile can look like a clean
// press+release and fire CLICKED -> the wrong item opens. We track the finger's
// actual travel during the press and only treat it as a tap if it barely moved
// (and LVGL isn't scrolling an ancestor). Items attach this with LV_EVENT_ALL.
#define TAP_SLOP 14
static lv_coord_t tap_x0, tap_y0;
static bool tap_moved;
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
    switch (lv_event_get_code(e)) {
    case LV_EVENT_PRESSED: {
        lv_point_t p = {0, 0};
        if (in) lv_indev_get_point(in, &p);
        tap_x0 = p.x; tap_y0 = p.y;
        // Pressing while the list is still gliding should stop the scroll, not open
        // the item under the finger.
        tap_moved = (in && lv_indev_get_scroll_obj(in) != NULL);
        return false;
    }
    case LV_EVENT_PRESSING:
        tap_track();
        return false;
    case LV_EVENT_RELEASED:
        tap_track();                                    // include the release point
        return !tap_moved && !(in && lv_indev_get_scroll_obj(in));
    default:
        return false;
    }
}

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
    lv_obj_clean(bk_list);
    int cur_b = -1; current_ref(&cur_b, NULL);
    lv_obj_t *sel = NULL;
    for (int i = 0; i < BIBLE_BOOK_COUNT; i++) {
        if (BIBLE_BOOKS[i].testament != testament) continue;
        bool here = (i == cur_b);              // selected/playing book -> highlighted
        // Two-column tiles; tapping one opens it directly. A drag scrolls the grid
        // without firing the click, so there's no accidental open.
        lv_obj_t *it = lv_btn_create(bk_list);
        lv_obj_set_size(it, 136, 46);          // two per row inside the 288px inner width
        card(it);
        if (here) lv_obj_set_style_bg_color(it, lv_color_hex(g_accent_hex), 0);
        lv_obj_set_style_radius(it, 12, 0);
        lv_obj_set_style_pad_all(it, 4, 0);
        lv_obj_t *t = lbl(it, BIBLE_BOOKS[i].name, &lv_font_montserrat_14, here ? COL_BG : COL_TEXT);
        lv_obj_set_width(t, lv_pct(100));
        lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(t);
        lv_obj_set_user_data(it, (void *)(intptr_t)i);
        lv_obj_add_event_cb(it, book_item_cb, LV_EVENT_ALL, NULL);
        if (here) sel = it;
    }
    bool ot = (testament == BIBLE_OLD);
    lv_obj_set_style_bg_color(bk_ot, lv_color_hex(ot ? g_accent_hex : COL_CARD2), 0);
    lv_obj_set_style_text_color(lv_obj_get_child(bk_ot, 0), lv_color_hex(ot ? COL_BG : COL_SUB), 0);
    lv_obj_set_style_bg_color(bk_nt, lv_color_hex(!ot ? g_accent_hex : COL_CARD2), 0);
    lv_obj_set_style_text_color(lv_obj_get_child(bk_nt, 0), lv_color_hex(!ot ? COL_BG : COL_SUB), 0);
    // Scroll the selected book into view (layout must be resolved first).
    if (sel) { lv_obj_update_layout(bk_list); lv_obj_scroll_to_view(sel, LV_ANIM_OFF); }
    else lv_obj_scroll_to_y(bk_list, 0, LV_ANIM_OFF);
}
static void seg_ot_cb(lv_event_t *e) { (void)e; populate_books(BIBLE_OLD); }
static void seg_nt_cb(lv_event_t *e) { (void)e; populate_books(BIBLE_NEW); }
static lv_obj_t *seg_chip(lv_obj_t *p, const char *t, lv_event_cb_t cb)
{
    lv_obj_t *b = lv_btn_create(p);
    lv_obj_set_flex_grow(b, 1);
    lv_obj_set_height(b, 30);
    lv_obj_set_style_radius(b, 15, 0);
    lv_obj_center(lbl(b, t, &lv_font_montserrat_14, COL_SUB));
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    return b;
}
static void build_books(int testament)
{
    lv_obj_t *c = make_page(NULL, true);          // no "Bible" title bar; back chevron + swipe-back stay
    lv_obj_t *scr = lv_obj_get_parent(c);

    // No now-playing bar on this page, so run the scrolling grid down toward the
    // bottom rim instead of stopping at the shared CONTENT_BOT (which reserves the
    // Home-only bar's space). Books has no title, so its content top is CONTENT_TOP_BACK.
    lv_obj_set_height(c, GRID_BOT - CONTENT_TOP_BACK);

    // Old/New segment sits where the title used to be (centred, clear of the corner
    // back chevron) so the entire content column goes to the book grid.
    lv_obj_t *seg = flat(scr);
    lv_obj_set_size(seg, 170, 32);
    lv_obj_align(seg, LV_ALIGN_TOP_MID, 0, 46);
    lv_obj_set_flex_flow(seg, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(seg, 8, 0);
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
    nav_push(PAGE_PLAYER, 0);
}
static void build_chapters(int book_idx)
{
    lv_obj_t *c = make_page(BIBLE_BOOKS[book_idx].name, true);
    lv_obj_set_height(c, GRID_BOT - CONTENT_TOP_PLAIN);   // run the chapter grid down toward the bottom rim (titled page)

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

    int cur_b = -1, cur_c = -1; current_ref(&cur_b, &cur_c);

    char buf[8];
    lv_obj_t *sel = NULL;
    for (int ch = 1; ch <= BIBLE_BOOKS[book_idx].chapter_count; ch++) {
        lv_obj_t *b = lv_btn_create(grid);
        lv_obj_set_size(b, 60, 60);
        lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(b, 0, 0);
        bool active = (cur_b == book_idx && cur_c == ch);   // selected/playing chapter
        lv_obj_set_style_bg_color(b, lv_color_hex(active ? g_accent_hex : COL_CARD2), 0);
        snprintf(buf, sizeof(buf), "%d", ch);
        lv_obj_center(lbl(b, buf, &lv_font_montserrat_20, active ? COL_BG : COL_TEXT));
        encode_play(b, book_idx, ch);
        lv_obj_add_event_cb(b, chapter_item_cb, LV_EVENT_ALL, NULL);
        if (active) sel = b;
    }
    if (sel) { lv_obj_update_layout(grid); lv_obj_scroll_to_view(sel, LV_ANIM_OFF); }
}

// ---------- modal (sleep timer) ----------
static void close_modal(void) { if (modal) { lv_obj_del(modal); modal = NULL; } }
static void modal_bg_cb(lv_event_t *e) { if (lv_event_get_target(e) == modal) close_modal(); }
static void set_sleep(int minutes)
{
    sleep_deadline_ms = minutes ? (esp_log_timestamp() + (uint32_t)minutes * 60u * 1000u) : 0;
}
static void sleep_opt_cb(lv_event_t *e)
{
    if (!tap_is_clean(e)) return;
    set_sleep((int)(intptr_t)lv_event_get_user_data(e));
    close_modal();
}
static void open_sleep_dialog(void)
{
    close_modal();
    modal = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(modal);
    lv_obj_set_size(modal, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(modal, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(modal, LV_OPA_70, 0);
    lv_obj_add_flag(modal, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(modal, modal_bg_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *cardv = lv_obj_create(modal);
    lv_obj_set_size(cardv, 190, LV_SIZE_CONTENT);
    lv_obj_center(cardv);
    card(cardv);
    lv_obj_set_flex_flow(cardv, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(cardv, 14, 0);
    lv_obj_set_style_pad_row(cardv, 8, 0);
    lv_obj_set_style_max_height(cardv, 290, 0);   // stays inside the circle; scrolls if taller
    lv_obj_set_scroll_dir(cardv, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(cardv, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t *h = lbl(cardv, "Sleep timer", &lv_font_montserrat_16, COL_TEXT);
    lv_obj_set_style_pad_bottom(h, 4, 0);
    static const int mins[] = {0, 15, 30, 45, 60, 90, 120};
    static const char *names[] = {"Off", "15 min", "30 min", "45 min", "60 min", "90 min", "120 min"};
    for (int i = 0; i < 7; i++) {
        lv_obj_t *b = lv_btn_create(cardv);
        lv_obj_set_width(b, lv_pct(100));
        lv_obj_set_height(b, 40);
        lv_obj_set_style_bg_color(b, lv_color_hex(COL_CARD2), 0);
        lv_obj_set_style_radius(b, 12, 0);
        lv_obj_center(lbl(b, names[i], &lv_font_montserrat_16, COL_TEXT));
        lv_obj_add_event_cb(b, sleep_opt_cb, LV_EVENT_ALL, (void *)(intptr_t)mins[i]);
    }
}

// ---------- PLAYER (arc watch-face) ----------
static void p_play_cb(lv_event_t *e)  { (void)e; app_player_toggle_pause(); app_power_user_activity(); }
static void p_next_cb(lv_event_t *e)  { (void)e; app_player_next(); app_power_user_activity(); }
static void p_prev_cb(lv_event_t *e)  { (void)e; app_player_prev(); app_power_user_activity(); }
static void p_seek_cb(lv_event_t *e)
{
    player_status_t st; app_player_get_status(&st);
    uint32_t ms = (uint32_t)((uint64_t)lv_slider_get_value(lv_event_get_target(e)) * st.dur_ms / 1000);
    app_player_seek_ms(ms);
    app_power_user_activity();
}
static void p_vol_cb(lv_event_t *e)
{
    int v = lv_slider_get_value(lv_event_get_target(e));
    app_player_set_volume(v);
    app_store_set_volume(v);
}
static void p_fav_cb(lv_event_t *e)
{
    (void)e;
    player_status_t st; app_player_get_status(&st);
    if (st.book_idx >= 0) app_store_fav_toggle(st.book_idx, st.chapter);
}
static void p_sleep_cb(lv_event_t *e) { (void)e; open_sleep_dialog(); }

static lv_obj_t *circ_btn(lv_obj_t *p, const char *sym, lv_event_cb_t cb, int sz, uint32_t bg, uint32_t fg, const lv_font_t *f)
{
    lv_obj_t *b = lv_btn_create(p);
    lv_obj_set_size(b, sz, sz);
    lv_obj_set_style_bg_color(b, lv_color_hex(bg), 0);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_center(lbl(b, sym, f, fg));
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    return b;
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

    // Title + chapter, stacked & centred near the top of the dial.
    lv_obj_t *g1 = flat(scr);
    lv_obj_set_size(g1, 216, LV_SIZE_CONTENT);
    lv_obj_align(g1, LV_ALIGN_TOP_MID, 0, 66);   // top bar is gone -> lift the title up
    lv_obj_set_flex_flow(g1, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(g1, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(g1, 4, 0);
    pw_title = lbl(g1, "-", &lv_font_montserrat_20, COL_TEXT);
    lv_obj_set_width(pw_title, 216);
    lv_label_set_long_mode(pw_title, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(pw_title, LV_TEXT_ALIGN_CENTER, 0);
    pw_sub = lbl(g1, "-", &lv_font_montserrat_18, g_accent_hex);

    // Slim scrub slider at the centre (handles its own drag → never triggers
    // swipe-back), with current/total time above it.
    pw_seek = lv_slider_create(scr);
    lv_obj_set_size(pw_seek, 150, 6);
    lv_obj_align(pw_seek, LV_ALIGN_CENTER, 0, -34);
    lv_slider_set_range(pw_seek, 0, 1000);
    lv_obj_set_style_bg_color(pw_seek, lv_color_hex(COL_TRACK), LV_PART_MAIN);
    lv_obj_set_style_bg_color(pw_seek, lv_color_hex(g_accent_hex), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(pw_seek, lv_color_hex(g_accent_hex), LV_PART_KNOB);
    lv_obj_add_event_cb(pw_seek, p_seek_cb, LV_EVENT_RELEASED, NULL);
    pw_cur = lbl(scr, "0:00", &lv_font_montserrat_14, COL_SUB);
    lv_obj_align_to(pw_cur, pw_seek, LV_ALIGN_OUT_TOP_LEFT, 0, -4);
    pw_tot = lbl(scr, "0:00", &lv_font_montserrat_14, COL_SUB);
    lv_obj_align_to(pw_tot, pw_seek, LV_ALIGN_OUT_TOP_RIGHT, 0, -4);

    // Transport on a lower arc: sleep · prev · play/pause · next · fav.
    lv_obj_t *trans = flat(scr);
    lv_obj_set_size(trans, 296, 64);
    lv_obj_align(trans, LV_ALIGN_CENTER, 0, 33);
    lv_obj_set_flex_flow(trans, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(trans, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *slb = circ_btn(trans, LV_SYMBOL_BELL, p_sleep_cb, 40, COL_CARD2, COL_SUB, &lv_font_montserrat_20);
    pw_sleep = lv_obj_get_child(slb, 0);   // bell glyph -> turns accent when a timer is set
    circ_btn(trans, LV_SYMBOL_PREV, p_prev_cb, 46, COL_CARD2, COL_TEXT, &lv_font_montserrat_24);
    lv_obj_t *pb = circ_btn(trans, LV_SYMBOL_PLAY, p_play_cb, 64, g_accent_hex, COL_BG, &lv_font_montserrat_28);
    pw_play = lv_obj_get_child(pb, 0);
    circ_btn(trans, LV_SYMBOL_NEXT, p_next_cb, 46, COL_CARD2, COL_TEXT, &lv_font_montserrat_24);
    lv_obj_t *fb = circ_btn(trans, LV_SYMBOL_PLUS, p_fav_cb, 40, COL_CARD2, COL_SUB, &lv_font_montserrat_20);
    pw_fav = lv_obj_get_child(fb, 0);

    // Volume slider — same 150x6 dimensions as the seek slider for a consistent look.
    pw_vol = lv_slider_create(scr);
    lv_obj_set_size(pw_vol, 150, 6);
    lv_obj_align(pw_vol, LV_ALIGN_CENTER, 0, 100);
    lv_slider_set_range(pw_vol, 0, 100);
    lv_slider_set_value(pw_vol, app_player_get_volume(), LV_ANIM_OFF);
    lv_obj_set_style_bg_color(pw_vol, lv_color_hex(COL_TRACK), LV_PART_MAIN);
    lv_obj_set_style_bg_color(pw_vol, lv_color_hex(g_accent_hex), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(pw_vol, lv_color_hex(g_accent_hex), LV_PART_KNOB);
    lv_obj_add_event_cb(pw_vol, p_vol_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_t *vicon = lbl(scr, LV_SYMBOL_VOLUME_MAX, &lv_font_montserrat_14, COL_SUB);
    lv_obj_align_to(vicon, pw_vol, LV_ALIGN_OUT_LEFT_MID, -10, 0);

    lv_scr_load_anim(scr, g_anim, 180, 0, true);
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
    lv_obj_set_style_pad_row(list, 6, 0);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
    return list;
}
static void style_row(lv_obj_t *it)
{
    lv_obj_set_style_bg_color(it, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_bg_opa(it, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(it, 0, 0);
    lv_obj_set_style_radius(it, 12, 0);
    lv_obj_set_style_text_color(it, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_height(it, 50);
    lv_obj_set_flex_align(it, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
}
static int detail_count(int which) { return (which == LIST_FAV) ? app_store_fav_count() : app_store_pl_track_count(which); }
static bool detail_get(int which, int i, track_ref_t *tr) { return (which == LIST_FAV) ? app_store_fav_get(i, tr) : app_store_pl_track_get(which, i, tr); }
static void detail_item_cb(lv_event_t *e)
{
    if (!tap_is_clean(e)) return;
    int b, ch; decode_play(lv_event_get_target(e), &b, &ch);
    app_player_play(b, ch); app_store_set_last(b, ch, 0);
    nav_push(PAGE_PLAYER, 0);
}
static void detail_remove_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    track_ref_t tr;
    if (detail_get(detail_which, i, &tr)) {
        if (detail_which == LIST_FAV) app_store_fav_toggle(tr.book_idx, tr.chapter);
        else app_store_pl_remove(detail_which, i);
    }
    if (detail_count(detail_which) == 0) detail_edit = false;
    build_list_detail(detail_which);
}
static void detail_edit_cb(lv_event_t *e) { (void)e; detail_edit = !detail_edit; build_list_detail(detail_which); }
static void build_list_detail(int which)
{
    detail_which = which;
    g_anim = LV_SCR_LOAD_ANIM_NONE;
    lv_obj_t *c = make_page(which == LIST_FAV ? "Favourites" : "Playlist", true);
    int n = detail_count(which);

    if (n > 0) {
        lv_obj_t *hdr = flat(c);
        lv_obj_set_size(hdr, lv_pct(100), 30);
        lv_obj_t *eb = lv_btn_create(hdr);
        lv_obj_set_size(eb, 84, 30);
        lv_obj_align(eb, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_style_bg_color(eb, lv_color_hex(detail_edit ? g_accent_hex : COL_CARD2), 0);
        lv_obj_set_style_radius(eb, 12, 0);
        lv_obj_center(lbl(eb, detail_edit ? LV_SYMBOL_OK " Done" : LV_SYMBOL_EDIT " Edit",
                          &lv_font_montserrat_14, detail_edit ? COL_BG : COL_TEXT));
        lv_obj_add_event_cb(eb, detail_edit_cb, LV_EVENT_CLICKED, NULL);
    }

    lv_obj_t *list = new_scroll_list(c);
    for (int i = 0; i < n; i++) {
        track_ref_t tr;
        if (!detail_get(which, i, &tr)) continue;
        char buf[40]; snprintf(buf, sizeof(buf), "%s %d", BIBLE_BOOKS[tr.book_idx].name, tr.chapter);
        lv_obj_t *it = lv_list_add_btn(list, detail_edit ? NULL : LV_SYMBOL_AUDIO, buf);
        style_row(it);
        if (detail_edit) {
            lv_obj_t *rm = lv_btn_create(it);
            lv_obj_set_size(rm, 38, 38);
            lv_obj_align(rm, LV_ALIGN_RIGHT_MID, 0, 0);
            lv_obj_set_style_bg_color(rm, lv_color_hex(0xC0392B), 0);
            lv_obj_set_style_radius(rm, 10, 0);
            lv_obj_center(lbl(rm, LV_SYMBOL_TRASH, &lv_font_montserrat_16, COL_TEXT));
            lv_obj_add_event_cb(rm, detail_remove_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        } else {
            encode_play(it, tr.book_idx, tr.chapter);
            lv_obj_add_event_cb(it, detail_item_cb, LV_EVENT_ALL, NULL);
        }
    }
    if (n == 0) {
        lv_obj_set_flex_align(c, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_t *em = lbl(c, "No favourites yet.\nAdd one from the player's heart button.",
                           &lv_font_montserrat_16, COL_SUB);
        lv_obj_set_width(em, lv_pct(92));
        lv_label_set_long_mode(em, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(em, LV_TEXT_ALIGN_CENTER, 0);
    }
}

// ---------- SETTINGS ----------
static void set_bright_cb(lv_event_t *e)
{
    int v = lv_slider_get_value(lv_event_get_target(e));
    app_power_set_brightness(v); app_store_set_brightness(v);
}
static void set_repeat_cb(lv_event_t *e)
{
    bool on = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    app_player_set_repeat_all(on); app_store_set_repeat_all(on);
}
static lv_obj_t *setting_card(lv_obj_t *c, const char *icon, const char *title)
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
    lv_obj_set_style_pad_column(hdr, 8, 0);
    lbl(hdr, icon, &lv_font_montserrat_18, g_accent_hex);
    lbl(hdr, title, &lv_font_montserrat_16, COL_TEXT);
    return cd;
}
static void theme_swatch_cb(lv_event_t *e) { if (!tap_is_clean(e)) return; apply_accent((uint32_t)(intptr_t)lv_event_get_user_data(e)); }
static const int  SCRTO_SECS[]  = { 15, 30, 60, 120, 300, 0 };
static const char *const SCRTO_LABELS[]  = {"15 sec", "30 sec", "1 min", "2 min", "5 min", "Never"};
static const int  PWROFF_SECS[] = { 0, 120, 300, 600, 1800 };
static const char *const PWROFF_LABELS[] = {"Off", "2 min", "5 min", "10 min", "30 min"};
static int opt_index(const int *vals, int n, int v) { for (int i = 0; i < n; i++) if (vals[i] == v) return i; return 0; }

// Round-safe option picker: a centred modal (like the sleep dialog) instead of
// an lv_dropdown, whose pop-up list gets clipped by the round bezel.
static lv_obj_t *g_scrto_val, *g_pwroff_val, *g_version_val;
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
    close_modal();
    modal = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(modal);
    lv_obj_set_size(modal, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(modal, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(modal, LV_OPA_70, 0);
    lv_obj_add_flag(modal, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(modal, modal_bg_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *cardv = lv_obj_create(modal);
    lv_obj_set_size(cardv, 200, LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(cardv, 286, 0);   // fits inside the circle
    lv_obj_center(cardv);
    card(cardv);
    lv_obj_set_flex_flow(cardv, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(cardv, 14, 0);
    lv_obj_set_style_pad_row(cardv, 8, 0);
    lv_obj_set_scroll_dir(cardv, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(cardv, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t *h = lbl(cardv, title, &lv_font_montserrat_16, COL_TEXT);
    lv_obj_set_style_pad_bottom(h, 4, 0);
    for (int i = 0; i < n; i++) {
        lv_obj_t *b = lv_btn_create(cardv);
        lv_obj_set_width(b, lv_pct(100));
        lv_obj_set_height(b, 40);
        bool sel = (i == cur);
        lv_obj_set_style_bg_color(b, lv_color_hex(sel ? g_accent_hex : COL_CARD2), 0);
        lv_obj_set_style_radius(b, 12, 0);
        lv_obj_center(lbl(b, labels[i], &lv_font_montserrat_16, sel ? COL_BG : COL_TEXT));
        lv_obj_add_event_cb(b, pick_opt_cb, LV_EVENT_ALL, (void *)(intptr_t)i);
    }
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
static void scrto_row_cb(lv_event_t *e) { if (!tap_is_clean(e)) return; open_picker("Screen timeout", SCRTO_LABELS, 6, opt_index(SCRTO_SECS, 6, app_store_get_screen_timeout(30)), apply_scrto); }
static void pwroff_row_cb(lv_event_t *e) { if (!tap_is_clean(e)) return; open_picker("Auto power-off", PWROFF_LABELS, 5, opt_index(PWROFF_SECS, 5, app_store_get_poweroff(300)), apply_pwroff); }

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
    if (n > 12) n = 12;
    static const char *labels[12];
    for (int i = 0; i < n; i++) labels[i] = app_player_version_name(i);
    open_picker("Version", labels, n, g_version_idx, apply_version);
}

// A tappable value row inside a setting card; opens the picker on tap.
static lv_obj_t *value_row(lv_obj_t *cd, const char *cur_text, lv_event_cb_t cb)
{
    lv_obj_t *b = lv_btn_create(cd);
    lv_obj_set_width(b, lv_pct(100));
    lv_obj_set_height(b, 40);
    lv_obj_set_style_bg_color(b, lv_color_hex(COL_CARD2), 0);
    lv_obj_set_style_radius(b, 12, 0);
    lv_obj_t *v = lbl(b, cur_text, &lv_font_montserrat_16, COL_TEXT);
    lv_obj_center(v);
    lv_obj_add_event_cb(b, cb, LV_EVENT_ALL, NULL);
    return v;
}
static void build_settings(void)
{
    lv_obj_t *c = make_page("Settings", true);
    lv_obj_set_height(c, GRID_BOT - CONTENT_TOP_PLAIN);   // use the bottom rim (no now-playing bar here; titled page)
    lv_obj_set_style_pad_row(c, 12, 0);
    lv_obj_set_style_pad_bottom(c, 8, 0);                // scroll runway so the last card clears the rim
    lv_obj_add_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(c, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(c, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t *vc = setting_card(c, LV_SYMBOL_AUDIO, "Version");
    int nver = app_player_versions();
    g_version_val = value_row(vc, nver > 0 ? app_player_version_name(g_version_idx) : "No SD card", version_row_cb);

    lv_obj_t *bc = setting_card(c, LV_SYMBOL_SETTINGS, "Brightness");
    lv_obj_t *bs = lv_slider_create(bc);
    lv_obj_set_width(bs, lv_pct(100));
    lv_slider_set_range(bs, 45, 100);
    lv_slider_set_value(bs, app_store_get_brightness(80), LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bs, lv_color_hex(g_accent_hex), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(bs, lv_color_hex(g_accent_hex), LV_PART_KNOB);
    lv_obj_add_event_cb(bs, set_bright_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *rc = setting_card(c, LV_SYMBOL_LOOP, "Repeat all");
    lv_obj_t *sw = lv_switch_create(rc);
    if (app_store_get_repeat_all(false)) lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(sw, lv_color_hex(g_accent_hex), LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw, set_repeat_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *sc = setting_card(c, LV_SYMBOL_EYE_OPEN, "Screen timeout");
    g_scrto_val = value_row(sc, SCRTO_LABELS[opt_index(SCRTO_SECS, 6, app_store_get_screen_timeout(30))], scrto_row_cb);

    lv_obj_t *pc = setting_card(c, LV_SYMBOL_POWER, "Auto power-off");
    g_pwroff_val = value_row(pc, PWROFF_LABELS[opt_index(PWROFF_SECS, 5, app_store_get_poweroff(300))], pwroff_row_cb);

    lv_obj_t *tc = setting_card(c, LV_SYMBOL_TINT, "Theme colour");
    lv_obj_t *sw2 = flat(tc);
    lv_obj_set_width(sw2, lv_pct(100));
    lv_obj_set_height(sw2, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(sw2, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(sw2, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(sw2, 10, 0);
    static const uint32_t palette[] = {0xE8A33D, 0x4A90D9, 0x3FB984, 0xA068D8, 0xD9534F, 0x2DB6B6};
    for (int i = 0; i < 6; i++) {
        lv_obj_t *sb = lv_btn_create(sw2);
        lv_obj_set_size(sb, 36, 36);
        lv_obj_set_style_radius(sb, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(sb, lv_color_hex(palette[i]), 0);
        bool sel = (palette[i] == g_accent_hex);
        lv_obj_set_style_border_width(sb, sel ? 3 : 0, 0);
        lv_obj_set_style_border_color(sb, lv_color_hex(COL_TEXT), 0);
        lv_obj_add_event_cb(sb, theme_swatch_cb, LV_EVENT_ALL, (void *)(intptr_t)palette[i]);
    }
}

// ---------- dispatch ----------
static void build_page(int page, int ctx)
{
    close_modal();
    cur_page = page;
    pw_title = NULL;
    switch (page) {
    case PAGE_HOME:        build_home(); break;
    case PAGE_BOOKS:       build_books(ctx); break;
    case PAGE_CHAPTERS:    build_chapters(ctx); break;
    case PAGE_PLAYER:      build_player(); break;
    case PAGE_LIST_DETAIL: detail_edit = false; build_list_detail(ctx); break;
    case PAGE_SETTINGS:    build_settings(); break;
    }
}

static void apply_accent(uint32_t hex)
{
    g_accent_hex = hex;
    app_store_set_theme((int)hex);
    lv_disp_t *disp = lv_disp_get_default();
    lv_theme_t *th = lv_theme_default_init(disp, lv_color_hex(hex), lv_color_hex(0x6b7280),
                                           true, &lv_font_montserrat_16);
    lv_disp_set_theme(disp, th);
    lv_obj_clean(lv_layer_top());
    make_top_bars();
    g_anim = LV_SCR_LOAD_ANIM_NONE;
    build_page(nav_stack[nav_sp].page, nav_stack[nav_sp].ctx);
}

// ---------- top-layer chrome (battery + now-playing pill) ----------
static void minibar_cb(lv_event_t *e)
{
    (void)e;
    player_status_t st; app_player_get_status(&st);
    if (st.state == PLAYER_STOPPED && st.book_idx < 0) {
        int b, ch; uint32_t pos;
        if (app_store_get_last(&b, &ch, &pos)) { app_player_play(b, ch); if (pos > 3000) app_player_seek_ms(pos); }
    }
    if (cur_page != PAGE_PLAYER) nav_push(PAGE_PLAYER, 0);
}
static void minibar_play_cb(lv_event_t *e)
{
    (void)e;
    player_status_t st; app_player_get_status(&st);
    if (st.state == PLAYER_STOPPED && st.book_idx < 0) {
        int b, ch; uint32_t pos;
        if (app_store_get_last(&b, &ch, &pos)) { app_player_play(b, ch); if (pos > 3000) app_player_seek_ms(pos); }
    } else {
        app_player_toggle_pause();
    }
}
static void make_top_bars(void)
{
    lv_obj_t *top = lv_layer_top();
    lv_obj_clear_flag(top, LV_OBJ_FLAG_CLICKABLE);

    // Battery at 12 o'clock, placed directly on the top layer (no small status
    // container that would clip it), low enough to clear the player's rim arc.
    sb_batt = lbl(top, LV_SYMBOL_BATTERY_FULL " --%", &lv_font_montserrat_14, COL_TEXT);
    lv_obj_align(sb_batt, LV_ALIGN_TOP_MID, 0, 22);   // a few px clear of the player's rim arc

    // Bottom "now playing": a progress arc hugging the bottom rim (follows the
    // circle, so no clipped rectangular corners), with a compact title + play
    // button centred just inside it. Tapping the title opens the player.
    mb_prog = lv_arc_create(top);
    lv_obj_set_size(mb_prog, 352, 352);
    lv_obj_center(mb_prog);
    lv_arc_set_rotation(mb_prog, 0);
    lv_arc_set_bg_angles(mb_prog, 35, 145);     // bottom rim arc
    lv_arc_set_mode(mb_prog, LV_ARC_MODE_REVERSE);  // fill left -> right
    lv_arc_set_range(mb_prog, 0, 1000);
    lv_arc_set_value(mb_prog, 0);
    lv_obj_remove_style(mb_prog, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(mb_prog, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(mb_prog, 6, LV_PART_MAIN);
    lv_obj_set_style_arc_width(mb_prog, 6, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(mb_prog, lv_color_hex(COL_TRACK), LV_PART_MAIN);
    lv_obj_set_style_arc_color(mb_prog, lv_color_hex(g_accent_hex), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(mb_prog, true, LV_PART_INDICATOR);

    mb_box = flat(top);
    lv_obj_set_size(mb_box, 220, MINIBAR_H);
    lv_obj_align(mb_box, LV_ALIGN_BOTTOM_MID, 0, -MINIBAR_LIFT);
    lv_obj_add_flag(mb_box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(mb_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(mb_box, minibar_cb, LV_EVENT_CLICKED, NULL);

    mb_title = lbl(mb_box, "Nothing playing", &lv_font_montserrat_16, COL_TEXT);
    lv_obj_set_width(mb_title, 200);
    lv_label_set_long_mode(mb_title, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(mb_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(mb_title, LV_ALIGN_TOP_MID, 0, 0);

    lv_obj_t *mp = circ_btn(mb_box, LV_SYMBOL_PLAY, minibar_play_cb, 52, g_accent_hex, COL_BG, &lv_font_montserrat_24);
    lv_obj_align(mp, LV_ALIGN_BOTTOM_MID, 0, 0);
    mb_play = lv_obj_get_child(mp, 0);
}

static void ui_tick(lv_timer_t *t)
{
    (void)t;
    if (go_home_req) { go_home_req = false; nav_reset(PAGE_HOME, 0); }

    int pct = app_power_battery_pct();
    const char *sym = pct > 80 ? LV_SYMBOL_BATTERY_FULL : pct > 55 ? LV_SYMBOL_BATTERY_3 :
                      pct > 30 ? LV_SYMBOL_BATTERY_2 : pct > 10 ? LV_SYMBOL_BATTERY_1 : LV_SYMBOL_BATTERY_EMPTY;
    char b[24];
    snprintf(b, sizeof(b), "%s %d%%", sym, pct);
    lv_label_set_text(sb_batt, b);

    player_status_t st;
    app_player_get_status(&st);
    bool playing = (st.state == PLAYER_PLAYING);
    int prog = (st.dur_ms > 0) ? (int)((uint64_t)st.pos_ms * 1000 / st.dur_ms) : 0;

    // now-playing bar (pill + rim arc) shown only on Home
    if (cur_page == PAGE_HOME) { lv_obj_clear_flag(mb_box, LV_OBJ_FLAG_HIDDEN); lv_obj_clear_flag(mb_prog, LV_OBJ_FLAG_HIDDEN); }
    else { lv_obj_add_flag(mb_box, LV_OBJ_FLAG_HIDDEN); lv_obj_add_flag(mb_prog, LV_OBJ_FLAG_HIDDEN); }
    lv_label_set_text(mb_play, playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    lv_arc_set_value(mb_prog, prog);
    if (st.book_idx >= 0) {
        char m[40]; snprintf(m, sizeof(m), "%s %d", BIBLE_BOOKS[st.book_idx].name, st.chapter);
        lv_label_set_text(mb_title, m);
    } else {
        int lb, lc; uint32_t lp;
        if (app_store_get_last(&lb, &lc, &lp)) { char m[40]; snprintf(m, sizeof(m), "%s %d", BIBLE_BOOKS[lb].name, lc); lv_label_set_text(mb_title, m); }
        else lv_label_set_text(mb_title, "Nothing playing");
    }

    // player watch-face live update
    if (cur_page == PAGE_PLAYER && pw_title) {
        if (st.book_idx >= 0) {
            lv_label_set_text(pw_title, BIBLE_BOOKS[st.book_idx].name);
            char s[16]; snprintf(s, sizeof(s), "Chapter %d", st.chapter);
            lv_label_set_text(pw_sub, s);
            bool isfav = app_store_is_fav(st.book_idx, st.chapter);
            lv_label_set_text(pw_fav, isfav ? LV_SYMBOL_OK : LV_SYMBOL_PLUS);  // tick when saved, + to add
            lv_obj_set_style_text_color(pw_fav, lv_color_hex(isfav ? g_accent_hex : COL_SUB), 0);
        }
        lv_obj_set_style_text_color(pw_sleep, lv_color_hex(sleep_deadline_ms ? g_accent_hex : COL_SUB), 0);
        lv_label_set_text(pw_play, playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
        lv_arc_set_value(pw_arc, prog);
        if (!lv_obj_has_state(pw_seek, LV_STATE_PRESSED)) lv_slider_set_value(pw_seek, prog, LV_ANIM_OFF);
        char a[16], d[16];
        fmt_time(a, sizeof(a), st.pos_ms);
        fmt_time(d, sizeof(d), st.dur_ms);
        lv_label_set_text(pw_cur, a);
        lv_label_set_text(pw_tot, d);
    }

    uint32_t nowms = esp_log_timestamp();
    if (playing && nowms - last_save_ms > 5000) {
        last_save_ms = nowms;
        app_store_set_last(st.book_idx, st.chapter, st.pos_ms);
    }
    if (sleep_deadline_ms && nowms >= sleep_deadline_ms) {
        sleep_deadline_ms = 0;
        if (st.state == PLAYER_PLAYING) app_player_toggle_pause();
    }
}

static void boot_home_cb(void) { go_home_req = true; }

void bible_app_start(void)
{
    ESP_LOGI(TAG, "UI start (360x360 round, watch-style)");
    g_accent_hex = (uint32_t)app_store_get_theme(ACCENT_DEFAULT);
    lv_disp_t *disp = lv_disp_get_default();
    lv_theme_t *th = lv_theme_default_init(disp, lv_color_hex(g_accent_hex), lv_color_hex(0x6b7280),
                                           true, &lv_font_montserrat_16);
    lv_disp_set_theme(disp, th);

    app_player_set_repeat_all(app_store_get_repeat_all(false));
    app_player_set_volume(app_store_get_volume(70));

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
    }

    app_power_set_boot_cb(boot_home_cb);
    make_top_bars();
    lv_timer_create(ui_tick, 400, NULL);
    nav_reset(PAGE_HOME, 0);
}
