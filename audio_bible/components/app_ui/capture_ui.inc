// Included after the page builders, only in BIBLE_SCREENSHOTS development builds.
// Exercise the real renderers with the mounted card and existing user data.
static lv_obj_t *capture_scroller;
static bool capture_check_bottom_targets(void);
static bool capture_library_benchmark(void);
static bool capture_library_rows(void);
static bool capture_shared_views(void);
static bool capture_navigation_benchmark(void);
static bool capture_back_response(void);
static bool capture_navigation_frames(void);
static bool capture_accidental_touch(void);

static void capture_tap(lv_obj_t *button)
{
    lv_event_send(button, LV_EVENT_PRESSED, NULL);
    lv_event_send(button, LV_EVENT_RELEASED, NULL);
}

// Exercise the real handlers and cancel paths without confirming a deletion.
// The hidden trigger supplies the same LVGL events/user data as a physical tap.
static bool capture_check_confirmations(void)
{
    int bible = app_store_fav_count(), year = app_store_favbiy_count();
    int library = app_store_favlib_count(), recent = app_store_hist_count();
    bool played = app_played_any(), ok = true;
    capture_scroller = NULL;
    capture_no_card = capture_empty_lists = capture_static_frame = false;
    g_anim = LV_SCR_LOAD_ANIM_NONE;
    const int contexts[] = {LIST_FAV, LIST_FAV_BIY, LIST_FAV_LIB};
    for (int i = 0; i < 4; i++) {
        build_page(i < 3 ? PAGE_LIST_DETAIL : PAGE_SETTINGS, i < 3 ? contexts[i] : 0);
        lv_obj_t *trigger = lv_btn_create(lv_scr_act());
        lv_obj_add_flag(trigger, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_user_data(trigger, (void *)(intptr_t)0);
        lv_obj_add_event_cb(trigger, i < 3 ? detail_remove_cb : clear_played_cb,
                            LV_EVENT_ALL, (void *)(intptr_t)0);
        capture_tap(trigger);
        if (!modal) { ok = false; break; }
        lv_obj_t *cardv = lv_obj_get_child(modal, 0);
        lv_obj_t *options = lv_obj_get_child(cardv, 1);
        capture_tap(lv_obj_get_child(options, 1));  // actual Cancel control
        if (modal) { ok = false; break; }
        capture_tap(trigger);
        if (!modal) { ok = false; break; }
        capture_tap(modal); // outside tap must cancel
        if (modal || bible != app_store_fav_count() || year != app_store_favbiy_count() ||
            library != app_store_favlib_count() || recent != app_store_hist_count() ||
            played != app_played_any()) { ok = false; break; }
    }
    build_page(PAGE_HOME, 0);
    ESP_LOGI(TAG, "confirmation checks: %s; favourites=%d/%d/%d recent=%d",
             ok ? "PASS" : "FAIL", app_store_fav_count(), app_store_favbiy_count(),
             app_store_favlib_count(), app_store_hist_count());
    return ok;
}

static void capture_find_scroller(lv_obj_t *obj, int *largest)
{
    if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) return;
    int range = lv_obj_get_scroll_y(obj) + lv_obj_get_scroll_bottom(obj);
    if (lv_obj_has_flag(obj, LV_OBJ_FLAG_SCROLLABLE) && range > *largest) {
        *largest = range;
        capture_scroller = obj;
    }
    for (uint32_t i = 0; i < lv_obj_get_child_cnt(obj); i++)
        capture_find_scroller(lv_obj_get_child(obj, i), largest);
}

bool bible_ui_capture_scene(const char *name)
{
    if (strcmp(name, "check-confirmations") == 0) return capture_check_confirmations();
    if (strcmp(name, "check-bottom-targets") == 0) return capture_check_bottom_targets();
    if (strcmp(name, "library-benchmark") == 0) return capture_library_benchmark();
    if (strcmp(name, "check-library-rows") == 0) return capture_library_rows();
    if (strcmp(name, "check-shared-views") == 0) return capture_shared_views();
    if (strcmp(name, "navigation-benchmark") == 0) return capture_navigation_benchmark();
    if (strcmp(name, "check-back-response") == 0) return capture_back_response();
    if (strcmp(name, "check-navigation-frames") == 0) return capture_navigation_frames();
    if (strcmp(name, "check-accidental-touch") == 0) return capture_accidental_touch();
    static const struct { const char *name; int page, ctx; } routes[] = {
        {"home", PAGE_HOME, 0},
        {"books-old", PAGE_BOOKS, BIBLE_OLD},
        {"books-new", PAGE_BOOKS, BIBLE_NEW},
        {"chapters", PAGE_CHAPTERS, 0},
        {"psalms", PAGE_CHAPTERS, 22},
        {"player", PAGE_PLAYER, 0},
        {"favourites", PAGE_FAVOURITES, 0},
        {"saved-bible", PAGE_LIST_DETAIL, LIST_FAV},
        {"saved-year", PAGE_LIST_DETAIL, LIST_FAV_BIY},
        {"saved-library", PAGE_LIST_DETAIL, LIST_FAV_LIB},
        {"saved-edit", PAGE_LIST_DETAIL, LIST_FAV},
        {"recent", PAGE_RECENT, 0},
        {"year", PAGE_BIY, 0},
        {"year-days", PAGE_BIY_DAYS, 0},
        {"year-intros", PAGE_BIY_DAYS, BIY_CTX_INTROS},
        {"library", PAGE_LIB, 0},
        {"library-folder", PAGE_LIB, 0},
        {"library-nested", PAGE_LIB, 0},
        {"letters", PAGE_LIB, 0},
        {"settings", PAGE_SETTINGS, 0},
        {"versions", PAGE_SETTINGS, 0},
        {"screen-timeout", PAGE_SETTINGS, 0},
        {"power-off", PAGE_SETTINGS, 0},
        {"history-clear", PAGE_SETTINGS, 0},
        {"saved-remove", PAGE_LIST_DETAIL, LIST_FAV},
        {"sleep", PAGE_PLAYER, 0},
        {"splash", PAGE_HOME, 0},
        {"preview-no-card", PAGE_HOME, 0},
        {"preview-empty-favourites", PAGE_FAVOURITES, 0},
        {"preview-empty-saved", PAGE_LIST_DETAIL, LIST_FAV},
        {"preview-empty-recent", PAGE_RECENT, 0},
        {"preview-empty-library", PAGE_LIB, 0},
        {"preview-empty-settings", PAGE_SETTINGS, 0},
        {"preview-player-playing", PAGE_PLAYER, 0},
        {"preview-player-paused", PAGE_PLAYER, 0},
        {"preview-player-year", PAGE_PLAYER, 0},
        {"preview-player-library", PAGE_PLAYER, 0},
    };
    unsigned i;
    for (i = 0; i < sizeof routes / sizeof routes[0]; i++)
        if (strcmp(name, routes[i].name) == 0) break;
    if (i == sizeof routes / sizeof routes[0]) return false;
    capture_scroller = NULL;
    capture_static_frame = false;
    capture_no_card = strcmp(name, "preview-no-card") == 0 ||
                      strcmp(name, "preview-empty-favourites") == 0 ||
                      strcmp(name, "preview-empty-settings") == 0;
    capture_empty_lists = strncmp(name, "preview-empty-", 14) == 0;
    lv_obj_clear_flag(lv_layer_top(), LV_OBJ_FLAG_HIDDEN);
    g_anim = LV_SCR_LOAD_ANIM_NONE;
    nav_sp = routes[i].page == PAGE_HOME ? 0 : 1;
    nav_stack[0] = (nav_entry_t){PAGE_HOME, 0};
    nav_stack[nav_sp] = (nav_entry_t){routes[i].page, routes[i].ctx};
    g_set_scroll = -1;
    build_page(routes[i].page, routes[i].ctx);

    if (strcmp(name, "splash") == 0) {
        capture_static_frame = true;
        lv_obj_add_flag(lv_layer_top(), LV_OBJ_FLAG_HIDDEN);
        bible_display_preview_splash();
    } else if (strncmp(name, "preview-player-", 15) == 0) {
        // Real player widgets, static sample data. No audio or persistence calls.
        capture_static_frame = true;
        lv_obj_add_flag(mb_box, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(mb_play, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(mb_prog, LV_OBJ_FLAG_HIDDEN);
        bool playing = strcmp(name, "preview-player-paused") != 0;
        player_set_title("John", "Chapter 3");
        if (strcmp(name, "preview-player-year") == 0)
            player_set_title("Day 1: In the Beginning", "Early World");
        if (strcmp(name, "preview-player-library") == 0)
            player_set_title("Aa Viral Thumponnu Thottal Shanthamay Theerumennullam", "Malayalam Songs");
        playpause_set(pw_ic_play, pw_ic_pause, playing);
        lv_label_set_text(pw_cur, "1:48");
        lv_label_set_text(pw_tot, "4:49");
        lv_slider_set_value(pw_seek, 374, LV_ANIM_OFF);
        lv_arc_set_value(pw_arc, 374);
        shema_icon_set_symbol(pw_fav, playing ? SHEMA_ICON_HEART_FILLED : SHEMA_ICON_HEART, 24);
        shema_icon_set_symbol(pw_sleep, playing ? SHEMA_ICON_MOON_FILLED : SHEMA_ICON_MOON, 24);
        shema_icon_set_color(pw_fav, lv_color_hex(g_accent_hex));
        shema_icon_set_color(pw_sleep, lv_color_hex(g_accent_hex));
        lv_obj_set_style_bg_color(pw_favbtn, lv_color_hex(COL_CARD), 0);
        lv_obj_set_style_bg_color(pw_sleepbtn, lv_color_hex(COL_CARD), 0);
    } else if (strcmp(name, "saved-edit") == 0 || strcmp(name, "saved-remove") == 0) {
        detail_edit = true;
        build_list_detail(LIST_FAV);
        if (strcmp(name, "saved-remove") == 0)
            open_confirmation("Remove favourite?", "Remove", confirmation_cancel_cb, NULL);
    } else if (strcmp(name, "sleep") == 0) {
        open_sleep_dialog();
    } else if (strcmp(name, "versions") == 0) {
        static const char *labels[12];
        int n = app_player_versions();
        if (!n) return false;
        if (n > 12) n = 12;
        for (int j = 0; j < n; j++) labels[j] = app_player_version_name(j);
        open_picker("Version", labels, n, g_version_idx, NULL);
    } else if (strcmp(name, "screen-timeout") == 0) {
        open_picker("Screen timeout", SCRTO_LABELS, SCRTO_N,
                    opt_index(SCRTO_SECS, SCRTO_N, app_store_get_screen_timeout(30)), NULL);
    } else if (strcmp(name, "power-off") == 0) {
        open_picker("Auto power-off", PWROFF_LABELS, PWROFF_N,
                    opt_index(PWROFF_SECS, PWROFF_N, app_store_get_poweroff(300)), NULL);
    } else if (strcmp(name, "history-clear") == 0) {
        // Both buttons dismiss in capture mode; screenshots cannot erase history.
        open_confirmation("Clear played history?", "Clear everything", confirmation_cancel_cb, NULL);
    } else if (strcmp(name, "library-folder") == 0 ||
               strcmp(name, "library-nested") == 0 || strcmp(name, "letters") == 0) {
        int depth = strcmp(name, "library-nested") == 0 ? 2 : 1;
        for (int d = 0; d < depth; d++) {
            int folder = -1;
            for (int j = 0; j < app_player_lib_count(); j++)
                if (app_player_lib_is_dir(j)) { folder = j; break; }
            if (folder < 0) return false;
            snprintf(g_lib_stack[d], sizeof g_lib_stack[d], "%s", app_player_lib_name(folder));
            build_page(PAGE_LIB, d + 1);
            nav_stack[nav_sp].ctx = d + 1;
        }
        if (strcmp(name, "letters") == 0) open_letter_picker();
    }
    lv_obj_update_layout(lv_scr_act());
    lv_obj_update_layout(lv_layer_top());
    int largest = 0;
    capture_find_scroller(modal ? modal : lv_scr_act(), &largest);
    return true;
}

void bible_ui_capture_scroll(int y, bool animated)
{
    if (capture_scroller && lv_obj_is_valid(capture_scroller)) {
        lv_obj_scroll_to_y(capture_scroller, y, animated ? LV_ANIM_ON : LV_ANIM_OFF);
        lv_obj_update_layout(capture_scroller);
    }
}

void bible_ui_capture_metrics(int *y, int *max_y, int *height)
{
    *y = *max_y = 0;
    *height = 360;
    if (capture_scroller && lv_obj_is_valid(capture_scroller)) {
        *y = lv_obj_get_scroll_y(capture_scroller);
        *max_y = *y + lv_obj_get_scroll_bottom(capture_scroller);
        *height = lv_obj_get_height(capture_scroller);
    }
}

static lv_indev_data_t capture_pointer_sample;
static int capture_footer_clicks;
static void capture_pointer_read(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    (void)drv;
    *data = capture_pointer_sample;
}

bool bible_ui_capture_pointer(int x, int y, bool down)
{
    static lv_indev_drv_t driver;
    static lv_indev_t *pointer;
    if (x < 0 || x >= 360 || y < 0 || y >= 360) return false;
    if (!pointer) {
        lv_indev_drv_init(&driver);
        driver.type = LV_INDEV_TYPE_POINTER;
        driver.read_cb = capture_pointer_read;
        pointer = lv_indev_drv_register(&driver);
        if (!pointer) return false;
        lv_timer_pause(driver.read_timer);
    }
    capture_pointer_sample = (lv_indev_data_t){.point = {x, y},
        .state = down ? LV_INDEV_STATE_PR : LV_INDEV_STATE_REL};
    lv_indev_read_timer_cb(driver.read_timer);
    if (!down) {
        capture_scroller = NULL;
        int largest = 0;
        lv_obj_update_layout(lv_scr_act());
        lv_obj_update_layout(lv_layer_top());
        capture_find_scroller(modal ? modal : lv_scr_act(), &largest);
    }
    return true;
}
static void capture_footer_clicked(lv_event_t *e)
{
    (void)e;
    capture_footer_clicks++;
}
static lv_obj_t *capture_hit(int x, int y)
{
    lv_point_t point = {x, y};
    lv_obj_t *hit = lv_indev_search_obj(lv_layer_sys(), &point);
    if (!hit) hit = lv_indev_search_obj(lv_layer_top(), &point);
    if (!hit) hit = lv_indev_search_obj(lv_scr_act(), &point);
    return hit;
}
static bool capture_footer_area(lv_obj_t *expected, int width)
{
    for (int y = BOTTOM_TOUCH_Y; y < BOTTOM_TOUCH_Y + BOTTOM_TOUCH_H; y++) {
        for (int x = (360 - width) / 2; x < (360 + width) / 2; x++) {
            if (capture_hit(x, y) != expected) {
                ESP_LOGE(TAG, "footer hit missed at %d,%d", x, y);
                return false;
            }
        }
    }
    return true;
}
static void capture_pointer_tap(lv_indev_drv_t *drv, int x, int y, bool drift)
{
    capture_pointer_sample = (lv_indev_data_t){.point = {x, y}, .state = LV_INDEV_STATE_PR};
    lv_indev_read_timer_cb(drv->read_timer);
    if (drift) {
        capture_pointer_sample.point.x -= 4;
        capture_pointer_sample.point.y -= 4;
        lv_indev_read_timer_cb(drv->read_timer);
    }
    capture_pointer_sample.state = LV_INDEV_STATE_REL;
    lv_indev_read_timer_cb(drv->read_timer);
}

// Use LVGL's real layer hit search and pointer state machine. Home's audio
// callback is replaced temporarily with a counter; navigation uses real Back.
static bool capture_check_bottom_targets(void)
{
    ESP_LOGI(TAG, "footer checks: starting");
    lv_indev_drv_t driver;
    lv_indev_drv_init(&driver);
    driver.type = LV_INDEV_TYPE_POINTER;
    driver.read_cb = capture_pointer_read;
    lv_indev_t *pointer = lv_indev_drv_register(&driver);
    if (!pointer) return false;
    lv_timer_pause(driver.read_timer);
    // This runs synchronously under the UI lock, so screen animations cannot
    // advance between test taps. Exercise navigation without scheduling them.
    capture_no_animation = true;
    bible_ui_capture_scene("home");
    bool ok = capture_footer_area(mb_play, BOTTOM_TOUCH_W) && capture_hit(180, 289) == mb_box;
    lv_obj_remove_event_cb(mb_play, minibar_play_cb);
    lv_obj_add_event_cb(mb_play, capture_footer_clicked, LV_EVENT_CLICKED, NULL);
    capture_footer_clicks = 0;
    const int xs[] = {140, 150, 180, 219}, ys[] = {290, 300, 340, 353};
    for (unsigned y = 0; y < 4; y++)
        for (unsigned x = 0; x < 4; x++) capture_pointer_tap(&driver, xs[x], ys[y], false);
    capture_pointer_tap(&driver, 140, 290, true);
    ok = ok && capture_footer_clicks == 17;
    lv_obj_remove_event_cb(mb_play, capture_footer_clicked);
    lv_obj_add_event_cb(mb_play, minibar_play_cb, LV_EVENT_ALL, NULL);
    ESP_LOGI(TAG, "Home footer: %s, %d/17 pointer clicks", ok ? "PASS" : "FAIL", capture_footer_clicks);

    const char *pages[] = {"books-old", "chapters", "player", "favourites", "saved-bible",
                           "settings", "year", "year-days", "recent", "library"};
    for (unsigned i = 0; ok && i < sizeof pages / sizeof pages[0]; i++) {
        const int edges[] = {(360 - BACK_TOUCH_W) / 2, (360 + BACK_TOUCH_W) / 2 - 1};
        for (unsigned side = 0; ok && side < 2; side++) {
            bible_ui_capture_scene(pages[i]);
            lv_obj_t *back = lv_obj_get_child(lv_scr_act(), 0);
            // No UI tick between navigation and the test: Home must already be hidden.
            ok = lv_obj_has_flag(mb_play, LV_OBJ_FLAG_HIDDEN) && capture_footer_area(back, BACK_TOUCH_W);
            // Horizontal expansion must never steal touches from the content above.
            for (int x = edges[0]; ok && x <= edges[1]; x++)
                ok = capture_hit(x, BOTTOM_TOUCH_Y - 1) != back;
            if (ok) {
                capture_pointer_tap(&driver, edges[side], BOTTOM_TOUCH_Y + BOTTOM_TOUCH_H / 2, false);
                ok = cur_page == PAGE_HOME && nav_sp == 0;
            }
            // This exhaustive check rebuilds 20 pages while holding the UI lock.
            // Let the idle task run between cases, as it would between real taps.
            vTaskDelay(1);
        }
        ESP_LOGI(TAG, "Back footer %s: %s (left/right edges, unchanged top)", pages[i], ok ? "PASS" : "FAIL");
    }
    bible_ui_capture_scene("sleep");
    bool blocked = capture_hit(180, 350) == modal;
    capture_pointer_tap(&driver, 180, 350, false);
    ok = ok && blocked && !modal && cur_page == PAGE_PLAYER && nav_sp == 1;
    lv_indev_delete(pointer);
    bible_ui_capture_scene("home");
    capture_no_animation = false;
    ESP_LOGI(TAG, "footer checks: %s; Play 80x64, Back 128x64, 10 Back routes, modal interception", ok ? "PASS" : "FAIL");
    return ok;
}

static bool capture_library_window(void)
{
    if (!list_view.list || lv_obj_get_child_cnt(list_view.spacer) != LIST_VIEW_POOL) return false;
    for (int start = 0; start < list_view.count; start += LIST_VIEW_POOL - 2) {
        lv_obj_scroll_to_y(list_view.list, list_view.rows[start].y, LV_ANIM_OFF);
        lv_obj_update_layout(list_view.list);
        lv_area_t viewport;
        lv_obj_get_coords(list_view.list, &viewport);
        int scroll = lv_obj_get_scroll_y(list_view.list);
        for (int j = 0; j < list_view.count; j++) {
            const list_view_row_t *row = &list_view.rows[j];
            int top = viewport.y1 + row->y - scroll;
            int bottom = top + row->height - 1;
            if (bottom < viewport.y1 || top > viewport.y2) continue;
            list_view_widget_t *widget = NULL;
            for (int k = 0; k < LIST_VIEW_POOL; k++)
                if (list_view.widgets[k].model == j) widget = &list_view.widgets[k];
            if (!widget || strcmp(lv_label_get_text(widget->label), row->title) != 0)
                return false;
            if (strcmp(lv_label_get_text(widget->subtitle), row->subtitle) != 0) return false;
            int expected = row->data;
            if ((int)(intptr_t)lv_obj_get_user_data(widget->button) != expected) return false;
            int hit_y = (LV_MAX(top, viewport.y1) + LV_MIN(bottom, viewport.y2)) / 2;
            if (capture_hit(180, hit_y) != widget->button) {
                ESP_LOGE(TAG, "Library row hit failed: model=%d y=%d", j, hit_y);
                return false;
            }
        }
        vTaskDelay(1);
    }
    return true;
}

static bool capture_library_rows(void)
{
    capture_no_animation = true;
    bible_ui_capture_scene("library");
    static char roots[12][64];
    int roots_n = 0;
    for (int i = 0; i < app_player_lib_count() && roots_n < 12; i++)
        if (app_player_lib_is_dir(i)) snprintf(roots[roots_n++], sizeof roots[0], "%s", app_player_lib_name(i));
    int largest = -1, largest_n = -1;
    for (int i = 0; i < roots_n; i++) {
        int n = app_player_lib_browse(roots[i]);
        if (n > largest_n) { largest = i; largest_n = n; }
    }
    bool ok = largest >= 0;
    if (ok) {
        snprintf(g_lib_stack[0], sizeof g_lib_stack[0], "%s", roots[largest]);
        g_anim = LV_SCR_LOAD_ANIM_NONE;
        build_page(PAGE_LIB, 1);
        int pages = 0;
        do {
            ok = capture_library_window();
            pages++;
            if (!ok || g_lib_off + LIB_PAGE >= largest_n) break;
            list_view_widget_t *next = NULL;
            for (int k = 0; k < LIST_VIEW_POOL; k++)
                if (list_view.widgets[k].model >= 0 &&
                    list_view.rows[list_view.widgets[k].model].callback == lib_page_cb &&
                    list_view.rows[list_view.widgets[k].model].data == LIB_PAGE) next = &list_view.widgets[k];
            if (!next) { ok = false; break; }
            int old_offset = g_lib_off;
            capture_tap(next->button);  // real Next handler, no track playback
            ok = g_lib_off == old_offset + LIB_PAGE;
            vTaskDelay(1);
        } while (ok);
        // Exercise Previous as well as Next, then a letter on a distant page.
        while (ok && g_lib_off > 0) {
            lv_obj_scroll_to_y(list_view.list, 0, LV_ANIM_OFF);
            lv_obj_update_layout(list_view.list);
            list_view_widget_t *previous = NULL;
            for (int k = 0; k < LIST_VIEW_POOL; k++)
                if (list_view.widgets[k].model >= 0 &&
                    list_view.rows[list_view.widgets[k].model].callback == lib_page_cb &&
                    list_view.rows[list_view.widgets[k].model].data == -LIB_PAGE) previous = &list_view.widgets[k];
            if (!previous) { ok = false; break; }
            int old_offset = g_lib_off;
            capture_tap(previous->button);
            ok = g_lib_off == old_offset - LIB_PAGE;
            vTaskDelay(1);
        }
        for (char letter = 'Z'; ok && letter >= 'A'; letter--) {
            int target = lib_first_with_initial(letter);
            if (target < LIB_PAGE) continue;
            lv_obj_t *trigger = lv_btn_create(lv_scr_act());
            lv_obj_add_flag(trigger, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_event_cb(trigger, lib_letter_cb, LV_EVENT_ALL, (void *)(intptr_t)letter);
            capture_tap(trigger);
            ok = g_lib_off == target / LIB_PAGE * LIB_PAGE;
            bool visible = false;
            for (int k = 0; k < LIST_VIEW_POOL; k++)
                if (list_view.widgets[k].model >= 0 && list_view.rows[list_view.widgets[k].model].data == target)
                    visible = true;
            ok = ok && visible && capture_library_window();
            break;
        }
        ESP_LOGI(TAG, "Library virtual rows: %s; %s entries=%d pages=%d widgets=%d (Next, Previous, letter)",
                 ok ? "PASS" : "FAIL", roots[largest], largest_n, pages, LIST_VIEW_POOL);
    }
    bible_ui_capture_scene("home");
    capture_no_animation = false;
    return ok;
}

// Read-only profiling of root folders and returning to their parent. No audio,
// favourite or progress mutations. Root names are copied before changing caches.
static bool capture_library_benchmark(void)
{
    capture_no_animation = true;
    bible_ui_capture_scene("library");
    static char roots[12][64];
    int count = 0;
    for (int i = 0; i < app_player_lib_count() && count < 12; i++)
        if (app_player_lib_is_dir(i)) snprintf(roots[count++], sizeof roots[0], "%s", app_player_lib_name(i));
    for (int i = 0; i < count; i++) {
        snprintf(g_lib_stack[0], sizeof g_lib_stack[0], "%s", roots[i]);
        nav_stack[0] = (nav_entry_t){PAGE_LIB, 0};
        nav_stack[1] = (nav_entry_t){PAGE_LIB, 1};
        nav_sp = 1;
        g_anim = LV_SCR_LOAD_ANIM_NONE;
        build_page(PAGE_LIB, 1);
        vTaskDelay(1);
        nav_back();
        vTaskDelay(1);
    }
    bible_ui_capture_scene("home");
    capture_no_animation = false;
    return true;
}

static bool capture_grid_window(void)
{
    if (!grid_view.list || lv_obj_get_child_cnt(grid_view.spacer) != grid_view.pool_size) return false;
    int stride = grid_view.height + grid_view.gap;
    for (int start = 0; start < grid_view.count; start += grid_view.cols * 2) {
        lv_obj_scroll_to_y(grid_view.list, start / grid_view.cols * stride, LV_ANIM_OFF);
        lv_obj_update_layout(grid_view.list);
        lv_area_t viewport;
        lv_obj_get_coords(grid_view.list, &viewport);
        for (int i = 0; i < grid_view.count; i++) {
            lv_area_t area;
            lv_obj_get_coords(grid_view.spacer, &area);
            int top = area.y1 + i / grid_view.cols * stride;
            int bottom = top + grid_view.height - 1;
            if (bottom < viewport.y1 || top > viewport.y2) continue;
            grid_view_widget_t *w = NULL;
            for (int k = 0; k < grid_view.pool_size; k++) if (grid_view.widgets[k].model == i) w = &grid_view.widgets[k];
            if (!w) { ESP_LOGE(TAG, "Grid missing model=%d start=%d", i, start); return false; }
            int id = grid_view.ids[i];
            int expected = grid_view.chapters ? grid_view.book * 1000 + id : id;
            if ((int)(intptr_t)lv_obj_get_user_data(w->button) != expected) return false;
            char title[8]; snprintf(title, sizeof title, "%d", id);
            // DOT temporarily replaces characters in LVGL's backing text. Restore
            // it through the public API before comparing the full model identity.
            lv_label_set_long_mode(w->label, LV_LABEL_LONG_CLIP);
            bool text_ok = strcmp(lv_label_get_text(w->label), grid_view.chapters ? title : BIBLE_BOOKS[id].name) == 0;
            lv_label_set_long_mode(w->label, LV_LABEL_LONG_DOT);
            if (!text_ok) { ESP_LOGE(TAG, "Grid text mismatch model=%d id=%d", i, id); return false; }
            lv_obj_get_coords(w->button, &area);
            int y = (LV_MAX(area.y1, viewport.y1) + LV_MIN(area.y2, viewport.y2)) / 2;
            if (capture_hit((area.x1 + area.x2) / 2, y) != w->button) { ESP_LOGE(TAG, "Grid hit failed model=%d start=%d y=%d scroll=%d", i, start, y, (int)lv_obj_get_scroll_y(grid_view.list)); return false; }
        }
        vTaskDelay(1);
    }
    return true;
}

static bool capture_shared_views(void)
{
    capture_no_animation = true;
    bool ok = true;
    const char *routes[] = {"year", "year-intros", "recent", "saved-bible", "saved-year", "saved-library", "saved-edit"};
    for (unsigned i = 0; i < sizeof routes / sizeof routes[0]; i++) {
        bible_ui_capture_scene(routes[i]);
        bool pass = !list_view.list || capture_library_window();
        ESP_LOGI(TAG, "Shared list %s: %s rows=%d", routes[i], pass ? "PASS" : "FAIL", list_view.list ? list_view.count : 0);
        ok = ok && pass;
        vTaskDelay(1);
    }
    for (int p = 0; p < BIY_PERIOD_COUNT; p++) {
        build_page(PAGE_BIY_DAYS, p);
        int days = BIY_PERIODS[p].last_day - BIY_PERIODS[p].first_day + 1;
        bool pass = list_view.count == days + (biy_intro_for_period(p) >= 0);
        int next_day = BIY_PERIODS[p].first_day;
        for (int i = 0; i < list_view.count; i++) {
            if (list_view.rows[i].data > 0 && list_view.rows[i].data != next_day++) pass = false;
            if (list_view.rows[i].callback != biy_play_cb) pass = false;
        }
        pass = pass && capture_library_window();
        ESP_LOGI(TAG, "Shared BIY period %d: %s days=%d", p, pass ? "PASS" : "FAIL", days);
        ok = ok && pass;
        vTaskDelay(1);
    }
    bible_ui_capture_scene("books-old");
    bool books = capture_grid_window();
    populate_books(BIBLE_NEW);
    books = capture_grid_window() && books;
    // Switching the same container repeatedly must not accumulate event handlers.
    unsigned events = bk_list->spec_attr->event_dsc_cnt;
    populate_books(BIBLE_OLD);
    ESP_LOGI(TAG, "Book event handlers before=%u after=%u", events, bk_list->spec_attr->event_dsc_cnt);
    books = capture_grid_window() && bk_list->spec_attr->event_dsc_cnt == events && books;
    ESP_LOGI(TAG, "Shared book grids: %s", books ? "PASS" : "FAIL");
    ok = ok && books;
    for (int book = 0; book < BIBLE_BOOK_COUNT; book++) {
        if (BIBLE_BOOKS[book].chapter_count != 150) continue;
        build_page(PAGE_CHAPTERS, book);
        bool pass = grid_view.count == 150 && capture_grid_window();
        ESP_LOGI(TAG, "Shared Psalms grid: %s chapters=%d widgets=%d", pass ? "PASS" : "FAIL", grid_view.count, GRID_VIEW_POOL);
        ok = ok && pass;
    }
    bible_ui_capture_scene("home");
    capture_no_animation = false;
    ESP_LOGI(TAG, "Shared view checks: %s", ok ? "PASS" : "FAIL");
    return ok;
}

static bool capture_navigation_benchmark(void)
{
    capture_no_animation = true;
    const char *routes[] = {"home", "books-old", "books-new", "chapters", "psalms", "year", "year-days", "year-intros", "recent", "library", "favourites", "saved-bible", "settings"};
    for (unsigned i = 0; i < sizeof routes / sizeof routes[0]; i++) {
        bible_ui_capture_scene(routes[i]);
        nav_entry_t destination = nav_stack[nav_sp];
        g_anim = LV_SCR_LOAD_ANIM_NONE;
        build_page(PAGE_PLAYER, 0);
        nav_stack[0] = destination;
        nav_stack[1] = (nav_entry_t){PAGE_PLAYER, 0};
        nav_sp = 1;
        uint32_t start = lv_tick_get();
        nav_back();
        ESP_LOGI(TAG, "BACKPERF %s %lu ms", routes[i], (unsigned long)(lv_tick_get() - start));
        vTaskDelay(1);
    }
    bible_ui_capture_scene("home");
    capture_no_animation = false;
    return true;
}

// Keep navigation animations enabled so input lockouts during transitions are detectable.
static bool capture_back_response(void)
{
    capture_no_animation = false;
    lv_indev_drv_t driver;
    lv_indev_drv_init(&driver);
    driver.type = LV_INDEV_TYPE_POINTER;
    driver.read_cb = capture_pointer_read;
    lv_indev_t *pointer = lv_indev_drv_register(&driver);
    if (!pointer) return false;
    lv_timer_pause(driver.read_timer);
    bible_ui_capture_scene("home");
    lv_obj_remove_event_cb(mb_play, minibar_play_cb);
    lv_obj_add_event_cb(mb_play, capture_footer_clicked, LV_EVENT_CLICKED, NULL);
    capture_footer_clicks = 0;
    const char *pages[] = {"books-old", "chapters", "player", "favourites", "saved-bible",
                          "settings", "year", "year-days", "recent", "library"};
    bool ok = true;
    for (unsigned i = 0; i < sizeof pages / sizeof pages[0]; i++) {
        bible_ui_capture_scene(pages[i]);
        capture_pointer_sample = (lv_indev_data_t){.point = {180, 322}, .state = LV_INDEV_STATE_PR};
        lv_indev_read_timer_cb(driver.read_timer);
        bool pass = cur_page == PAGE_HOME && nav_sp == 0 && !lv_disp_get_default()->prev_scr;
        // Contact remains down across several input polls on the new Home page.
        for (int hold = 0; hold < 4; hold++) lv_indev_read_timer_cb(driver.read_timer);
        capture_pointer_sample.state = LV_INDEV_STATE_REL;
        lv_indev_read_timer_cb(driver.read_timer);
        pass = pass && cur_page == PAGE_HOME && capture_footer_clicks == 0;
        ESP_LOGI(TAG, "Back touch-down %s: %s (hold/release does not activate Play)", pages[i], pass ? "PASS" : "FAIL");
        ok = ok && pass;
        vTaskDelay(1);
    }
    // Forward -> Back while the destination fade is still active, followed by
    // another Back with no animation-settling wait. One contact removes one level.
    bible_ui_capture_scene("books-old");
    nav_push(PAGE_CHAPTERS, 0);
    lv_obj_update_layout(lv_scr_act());
    bool live_fade = lv_anim_get(lv_scr_act(), chrome_opacity) != NULL;
    capture_pointer_sample = (lv_indev_data_t){.point = {180, 322}, .state = LV_INDEV_STATE_PR};
    lv_indev_read_timer_cb(driver.read_timer);
    bool rapid = live_fade && cur_page == PAGE_BOOKS && nav_sp == 1 && !lv_disp_get_default()->prev_scr;
    for (int hold = 0; hold < 4; hold++) lv_indev_read_timer_cb(driver.read_timer);
    rapid = rapid && cur_page == PAGE_BOOKS && nav_sp == 1;
    capture_pointer_sample.state = LV_INDEV_STATE_REL;
    lv_indev_read_timer_cb(driver.read_timer);
    lv_obj_update_layout(lv_scr_act());
    capture_pointer_tap(&driver, 180, 322, false);
    rapid = rapid && cur_page == PAGE_HOME && nav_sp == 0 && capture_footer_clicks == 0;
    ESP_LOGI(TAG, "Back during forward fade + rapid second Back: %s", rapid ? "PASS" : "FAIL");
    ok = ok && rapid;
    lv_obj_remove_event_cb(mb_play, capture_footer_clicked);
    lv_obj_add_event_cb(mb_play, minibar_play_cb, LV_EVENT_ALL, NULL);
    lv_indev_delete(pointer);
    bible_ui_capture_scene("home");
    ESP_LOGI(TAG, "Back response checks: %s", ok ? "PASS" : "FAIL");
    return ok;
}

// Settled screenshots miss the 100ms fade. Render its start/middle/end using
// the real full-frame draw/flush path and inspect bare ground inside the glass.
// This catches a bright display backdrop exposed by translucent screen roots.
static bool capture_fade_frames(const char *route, const char *direction)
{
    lv_disp_t *disp = lv_disp_get_default();
    lv_obj_t *screen = lv_scr_act();
    bool ok = !disp->prev_scr && lv_anim_get(screen, chrome_opacity) != NULL;
    lv_anim_del(screen, chrome_opacity); // deterministic samples under the UI lock
    const int opacity[] = {160, 208, LV_OPA_COVER};
    const lv_point_t ground[] = {{20, 150}, {20, 180}, {20, 210},
                                 {339, 150}, {339, 180}, {339, 210}};
    unsigned peak = 0;
    for (unsigned f = 0; f < sizeof opacity / sizeof opacity[0]; f++) {
        chrome_opacity(screen, opacity[f]);
        lv_obj_invalidate(screen);
        lv_refr_now(disp);
        const lv_color_t *pixels = disp->driver->draw_buf->buf1;
        for (unsigned p = 0; p < sizeof ground / sizeof ground[0]; p++) {
            unsigned brightness = lv_color_brightness(pixels[ground[p].y * 360 + ground[p].x]);
            if (brightness > peak) peak = brightness;
            // RGB565 quantisation is allowed; a white-backed fade is ~100 here.
            if (brightness > 24) ok = false;
        }
        vTaskDelay(1);
    }
    ESP_LOGI(TAG, "Navigation frames %s %s: %s, ground peak=%u/255",
             route, direction, ok ? "PASS" : "FAIL", peak);
    return ok;
}

static bool capture_navigation_frames(void)
{
    capture_no_animation = false;
    const char *routes[] = {"books-old", "chapters", "player", "favourites", "saved-bible",
                           "settings", "year", "year-days", "recent", "library"};
    bool ok = true;
    for (unsigned i = 0; i < sizeof routes / sizeof routes[0]; i++) {
        bible_ui_capture_scene(routes[i]);
        nav_entry_t destination = nav_stack[nav_sp];
        bible_ui_capture_scene("home");
        nav_push(destination.page, destination.ctx);
        bool forward = capture_fade_frames(routes[i], "forward");
        nav_back();
        bool back = capture_fade_frames(routes[i], "back");
        ok = ok && forward && back;
    }
    bible_ui_capture_scene("home");
    ESP_LOGI(TAG, "Navigation frame checks: %s; 60 rendered frames, 360 ground samples",
             ok ? "PASS" : "FAIL");
    return ok;
}

static int capture_clean_taps;
static void capture_clean_tap_cb(lv_event_t *e)
{
    if (tap_is_clean(e)) capture_clean_taps++;
}
static void capture_switch_changed(lv_event_t *e)
{
    (void)e;
    capture_clean_taps++;
}
static lv_obj_t *capture_find_switch(lv_obj_t *obj)
{
    if (lv_obj_has_class(obj, &lv_switch_class)) return obj;
    for (uint32_t i = 0; i < lv_obj_get_child_cnt(obj); i++) {
        lv_obj_t *found = capture_find_switch(lv_obj_get_child(obj, i));
        if (found) return found;
    }
    return NULL;
}
static void capture_pointer_move(lv_indev_drv_t *driver, int x, int y, lv_indev_state_t state)
{
    capture_pointer_sample = (lv_indev_data_t){.point = {x, y}, .state = state};
    lv_indev_read_timer_cb(driver->read_timer);
}
static void capture_drag(lv_indev_drv_t *driver, lv_obj_t *obj, int dx, int dy, bool return_to_start)
{
    lv_area_t area;
    lv_obj_get_coords(obj, &area);
    int x = (area.x1 + area.x2) / 2, y = (area.y1 + area.y2) / 2;
    capture_pointer_move(driver, x, y, LV_INDEV_STATE_PR);
    capture_pointer_move(driver, x + dx, y + dy, LV_INDEV_STATE_PR);
    if (return_to_start) capture_pointer_move(driver, x, y, LV_INDEV_STATE_PR);
    capture_pointer_move(driver, return_to_start ? x : x + dx, return_to_start ? y : y + dy, LV_INDEV_STATE_REL);
}

static bool capture_accidental_touch(void)
{
    capture_no_animation = true;
    lv_indev_drv_t driver;
    lv_indev_drv_init(&driver);
    driver.type = LV_INDEV_TYPE_POINTER;
    driver.read_cb = capture_pointer_read;
    lv_indev_t *pointer = lv_indev_drv_register(&driver);
    if (!pointer) { capture_no_animation = false; return false; }
    lv_timer_pause(driver.read_timer);
    bool ok = true;

    bible_ui_capture_scene("home");
    capture_drag(&driver, capture_hit(84, 108), 0, 28, true);
    bool home = cur_page == PAGE_HOME;
    capture_pointer_tap(&driver, 84, 108, true);
    home = home && cur_page == PAGE_BOOKS;
    ESP_LOGI(TAG, "Touch guard Home: %s (out-and-back drag rejected, 4px drift accepted)", home ? "PASS" : "FAIL");
    ok = ok && home;

    bible_ui_capture_scene("player");
    capture_drag(&driver, pw_sleepbtn, 0, 28, false);
    bool player = !modal && cur_page == PAGE_PLAYER;
    capture_drag(&driver, pw_sleepbtn, 4, 4, false);
    player = player && modal != NULL;
    close_modal();
    ESP_LOGI(TAG, "Touch guard Player: %s (drag rejected, small drift opens sleep)", player ? "PASS" : "FAIL");
    ok = ok && player;

    bible_ui_capture_scene("home");
    lv_obj_remove_event_cb(mb_play, minibar_play_cb); // never start real playback
    lv_obj_add_event_cb(mb_play, capture_clean_tap_cb, LV_EVENT_ALL, NULL);
    capture_clean_taps = 0;
    capture_drag(&driver, mb_play, 28, 0, true);
    bool footer = capture_clean_taps == 0;
    capture_drag(&driver, mb_play, 4, 4, false);
    footer = footer && capture_clean_taps == 1;
    // A canceled contact and an unsolicited second release must do nothing.
    lv_event_send(mb_play, LV_EVENT_RELEASED, NULL);
    lv_event_send(mb_play, LV_EVENT_PRESSED, NULL);
    lv_event_send(mb_play, LV_EVENT_PRESS_LOST, NULL);
    lv_event_send(mb_play, LV_EVENT_RELEASED, NULL);
    footer = footer && capture_clean_taps == 1;
    lv_obj_remove_event_cb(mb_play, capture_clean_tap_cb);
    lv_obj_add_event_cb(mb_play, minibar_play_cb, LV_EVENT_ALL, NULL);
    ESP_LOGI(TAG, "Touch guard Play: %s (drag, jitter, cancel, duplicate release)", footer ? "PASS" : "FAIL");
    ok = ok && footer;

    bible_ui_capture_scene("settings");
    lv_obj_t *sw = capture_find_switch(lv_scr_act());
    bool switches = sw != NULL;
    if (sw) {
        lv_obj_remove_event_cb(sw, set_repeat_cb); // real switch mechanics, no NVS writes
        lv_obj_add_event_cb(sw, capture_switch_changed, LV_EVENT_VALUE_CHANGED, NULL);
        lv_obj_scroll_to_view_recursive(sw, LV_ANIM_OFF);
        lv_obj_update_layout(lv_scr_act());
        lv_area_t switch_area; lv_obj_get_coords(sw, &switch_area);
        int sx = (switch_area.x1 + switch_area.x2) / 2, sy = (switch_area.y1 + switch_area.y2) / 2;
        ESP_LOGI(TAG, "Switch test center=%d,%d hit=%d scroll=%d", sx, sy,
                 capture_hit(sx, sy) == sw, (int)lv_obj_get_scroll_y(g_set_cont));
        bool before = lv_obj_has_state(sw, LV_STATE_CHECKED);
        capture_clean_taps = 0;
        capture_drag(&driver, sw, 28, 0, true);
        switches = capture_clean_taps == 0 && before == lv_obj_has_state(sw, LV_STATE_CHECKED);
        ESP_LOGI(TAG, "Switch drag count=%d unchanged=%d", capture_clean_taps,
                 before == lv_obj_has_state(sw, LV_STATE_CHECKED));
        capture_drag(&driver, sw, 4, 4, false);
        ESP_LOGI(TAG, "Switch tap count=%d changed=%d", capture_clean_taps,
                 before != lv_obj_has_state(sw, LV_STATE_CHECKED));
        switches = switches && capture_clean_taps == 1 && before != lv_obj_has_state(sw, LV_STATE_CHECKED);
    }
    ESP_LOGI(TAG, "Touch guard Settings switch: %s (one deliberate toggle, no persistence)", switches ? "PASS" : "FAIL");
    ok = ok && switches;

    bible_ui_capture_scene("library");
    lv_obj_t *row = list_view.widgets[0].button;
    lv_area_t area; lv_obj_get_coords(row, &area);
    int x = (area.x1 + area.x2) / 2, y = (area.y1 + area.y2) / 2;
    void *identity = lv_obj_get_user_data(row);
    capture_pointer_move(&driver, x, y, LV_INDEV_STATE_PR);
    lv_obj_set_user_data(row, (void *)((intptr_t)identity + 1000)); // recycled model identity
    capture_pointer_move(&driver, x, y, LV_INDEV_STATE_REL);
    bool recycled = cur_page == PAGE_LIB && nav_sp == 1;
    if (recycled) lv_obj_set_user_data(row, identity);
    ESP_LOGI(TAG, "Touch guard recycled row: %s", recycled ? "PASS" : "FAIL");
    ok = ok && recycled;

    open_confirmation("Touch guard check", "Confirm", capture_clean_tap_cb, NULL);
    lv_obj_update_layout(lv_layer_top());
    lv_obj_t *options = lv_obj_get_child(lv_obj_get_child(modal, 0), 1);
    lv_obj_t *confirm = lv_obj_get_child(options, 0);
    capture_clean_taps = 0;
    capture_drag(&driver, confirm, 28, 0, true);
    bool dialog = capture_clean_taps == 0 && modal;
    capture_drag(&driver, confirm, 4, 4, false);
    dialog = dialog && capture_clean_taps == 1;
    close_modal();
    ESP_LOGI(TAG, "Touch guard confirmation: %s (no data mutation)", dialog ? "PASS" : "FAIL");
    ok = ok && dialog;

    lv_indev_delete(pointer);
    bible_ui_capture_scene("home");
    capture_no_animation = false;
    ESP_LOGI(TAG, "Accidental touch checks: %s", ok ? "PASS" : "FAIL");
    return ok;
}
