// Book and chapter grids share a fixed pool. Even Psalms builds only the tiles
// around the viewport; model IDs, completion colours and selection stay stable.
#define GRID_VIEW_POOL 24
#define GRID_VIEW_MAX  150   // Psalms, the largest book
typedef struct {
    lv_obj_t *button, *label;
    int model;
} grid_view_widget_t;
static struct {
    lv_obj_t *list, *spacer;
    grid_view_widget_t widgets[GRID_VIEW_POOL];
    int ids[GRID_VIEW_MAX], count, cols, width, height, gap, selected, book, pool_size;
    bool chapters, binding;
    lv_event_cb_t callback;
} grid_view;

static void grid_view_action(lv_event_t *e)
{
    grid_view_widget_t *widget = lv_event_get_user_data(e);
    if (grid_view.list && widget->button == lv_event_get_target(e) && widget->model >= 0)
        grid_view.callback(e);
}

static void grid_view_refresh(void)
{
    if (!grid_view.list || grid_view.binding) return;
    grid_view.binding = true;
    int stride = grid_view.height + grid_view.gap;
    int first = LV_MAX(0, lv_obj_get_scroll_y(grid_view.list) / stride - 1) * grid_view.cols;
    int cur_b = -1, cur_c = -1;
    current_ref(&cur_b, &cur_c);
    for (int k = 0; k < grid_view.pool_size; k++) {
        grid_view_widget_t *w = &grid_view.widgets[k];
        int model = first + k;
        if (model >= grid_view.count) { lv_obj_add_flag(w->button, LV_OBJ_FLAG_HIDDEN); w->model = -1; continue; }
        if (w->model == model) continue;
        w->model = model;
        int id = grid_view.ids[model];
        bool active = grid_view.chapters ? cur_b == grid_view.book && cur_c == id : cur_b == id;
        bool done = grid_view.chapters && app_played_get_bible(grid_view.book, id, NULL);
        uint32_t bg = active ? g_accent_hex : done ? COL_DONE : grid_view.chapters ? COL_CARD2 : COL_CARD;
        lv_obj_set_style_bg_color(w->button, lv_color_hex(bg), 0);
        lv_obj_set_style_text_color(w->label, lv_color_hex(active ? COL_BG : done ? COL_DONE_T : COL_TEXT), 0);
        if (grid_view.chapters) {
            char title[8]; snprintf(title, sizeof title, "%d", id);
            lv_label_set_text(w->label, title);
            encode_play(w->button, grid_view.book, id);
        } else {
            lv_label_set_text(w->label, BIBLE_BOOKS[id].name);
            lv_obj_set_user_data(w->button, (void *)(intptr_t)id);
        }
        int row_start = model / grid_view.cols * grid_view.cols;
        int row_count = LV_MIN(grid_view.cols, grid_view.count - row_start);
        int col = model % grid_view.cols;
        int x = grid_view.chapters ? (288 - row_count * 60) * (col + 1) / (row_count + 1) + col * 60
                                   : (288 - row_count * 138 - (row_count - 1) * 8) / 2 + col * 146;
        lv_obj_set_pos(w->button, x, model / grid_view.cols * stride);
        lv_obj_clear_flag(w->button, LV_OBJ_FLAG_HIDDEN);
    }
    grid_view.binding = false;
}

static void grid_view_event(lv_event_t *e)
{
    if (lv_event_get_target(e) != grid_view.list) return;
    if (lv_event_get_code(e) == LV_EVENT_DELETE) grid_view.list = NULL;
    else if (lv_event_get_code(e) == LV_EVENT_SCROLL) grid_view_refresh();
}

static void grid_view_begin(lv_obj_t *list, bool chapters, int book, lv_event_cb_t callback)
{
    grid_view.list = NULL;
    lv_obj_remove_event_cb(list, grid_view_event);
    lv_obj_clean(list);
    memset(&grid_view, 0, sizeof grid_view);
    grid_view.list = list;
    grid_view.chapters = chapters;
    grid_view.book = book;
    grid_view.cols = chapters ? 4 : 2;
    grid_view.pool_size = grid_view.cols * 6;
    grid_view.width = chapters ? 60 : 138;
    grid_view.height = chapters ? 60 : TAP_COMPACT;
    grid_view.gap = chapters ? 12 : 8;
    grid_view.callback = callback;
    grid_view.selected = -1;
    lv_obj_set_layout(list, 0);
    lv_obj_scroll_to_y(list, 0, LV_ANIM_OFF);
    grid_view.spacer = flat(list);
    lv_obj_set_width(grid_view.spacer, 288);
    lv_obj_add_event_cb(list, grid_view_event, LV_EVENT_ALL, NULL);
}

static void grid_view_finish(void)
{
    static lv_style_t tile_style;
    static bool ready;
    if (!ready) {
        ready = true;
        lv_style_init(&tile_style);
        lv_style_set_pad_all(&tile_style, 4);
        lv_style_set_border_width(&tile_style, 0);
        lv_style_set_bg_opa(&tile_style, LV_OPA_COVER);
    }
    int rows = (grid_view.count + grid_view.cols - 1) / grid_view.cols;
    lv_obj_set_height(grid_view.spacer, rows * (grid_view.height + grid_view.gap) - grid_view.gap);
    for (int k = 0; k < grid_view.pool_size; k++) {
        grid_view_widget_t *w = &grid_view.widgets[k];
        w->model = -1;
        w->button = lv_btn_create(grid_view.spacer);
        lv_obj_set_size(w->button, grid_view.width, grid_view.height);
        lv_obj_add_style(w->button, &tile_style, 0);
        lv_obj_set_style_radius(w->button, grid_view.chapters ? LV_RADIUS_CIRCLE : SHEMA_CARD_RADIUS, 0);
        w->label = lbl(w->button, "", grid_view.chapters ? &lv_font_montserrat_20 : &lv_font_montserrat_16, COL_TEXT);
        lv_obj_set_width(w->label, lv_pct(100));
        lv_obj_set_style_text_align(w->label, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(w->label, LV_LABEL_LONG_DOT);
        lv_obj_center(w->label);
        lv_obj_add_event_cb(w->button, grid_view_action, LV_EVENT_ALL, w);
    }
    grid_view_refresh();
    lv_obj_update_layout(grid_view.list);
    if (grid_view.selected >= 0) {
        int bottom = grid_view.selected / grid_view.cols * (grid_view.height + grid_view.gap) + grid_view.height;
        lv_obj_scroll_to_y(grid_view.list, LV_MAX(0, bottom - lv_obj_get_content_height(grid_view.list)), LV_ANIM_OFF);
    }
}
