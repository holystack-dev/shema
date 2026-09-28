// Shared list renderer: eight reusable rows, measured wrapped text and no flex
// layout per item. Row identities stay in the model while widgets are recycled.
// Private app_ui module: call begin -> add rows -> finish under the LVGL lock.
// One active view is intentional on this device. Drop list_view.list on navigation;
// outgoing screens own their widgets until LVGL deletes them, but cannot rebind
// this shared model. Callbacks receive stable IDs through object user_data.
// Models live in PSRAM; scrolling neither allocates rows nor performs storage I/O.
#define LIST_VIEW_POOL 8
#define LIST_VIEW_MAX 154
_Static_assert(STORE_MAX_TRACKS <= LIST_VIEW_MAX && STORE_MAX_FAV_BIY <= LIST_VIEW_MAX &&
               STORE_MAX_FAV_LIB <= LIST_VIEW_MAX && STORE_MAX_HIST <= LIST_VIEW_MAX,
               "All stored lists must fit the shared row model");
_Static_assert(sizeof(lv_coord_t) >= 4, "Long virtual lists require LV_USE_LARGE_COORD");
typedef struct {
    int data;
    bool stripe;
    shema_icon_id_t symbol;
    uint32_t text_color, sub_color, edge, background;
    lv_event_cb_t callback;
    lv_coord_t y, height, title_height, sub_height;
    char title[96], subtitle[224];
} list_view_row_t;
typedef struct {
    lv_obj_t *button, *label, *subtitle, *glyph, *remove;
    int model;
    shema_icon_id_t symbol;
} list_view_widget_t;
static list_view_row_t *list_models; // one bounded PSRAM allocation, reused across screens
static struct {
    lv_obj_t *list, *spacer;
    list_view_row_t *rows;
    list_view_widget_t widgets[LIST_VIEW_POOL];
    int count, total_height, width;
    bool binding;
    lv_event_cb_t remove_callback;
} list_view;

static list_view_row_t *list_view_add(const char *title, const char *subtitle,
                                    shema_icon_id_t symbol, uint32_t text_color,
                                    bool stripe, uint32_t edge, bool action,
                                    lv_event_cb_t callback, int data)
{
    if (list_view.count >= LIST_VIEW_MAX) return NULL;
    list_view_row_t *row = &list_view.rows[list_view.count++];
    *row = (list_view_row_t){.data = data,
        .stripe = stripe, .symbol = symbol, .text_color = text_color, .sub_color = COL_SUB, .edge = edge,
        .background = action ? COL_CARD2 : COL_CARD, .callback = callback, .y = list_view.total_height};
    snprintf(row->title, sizeof row->title, "%s", title);
    snprintf(row->subtitle, sizeof row->subtitle, "%s", subtitle ? subtitle : "");
    int title_width = list_view.width - (list_view.remove_callback ? 84 : stripe ? 40 : symbol < SHEMA_ICON_COUNT ? 58 : 24);
    lv_point_t size;
    lv_txt_get_size(&size, row->title, &lv_font_montserrat_18, 0, 0, title_width, LV_TEXT_FLAG_NONE);
    row->title_height = size.y;
    int band = stripe || symbol == SHEMA_ICON_COUNT ? size.y : LV_MAX(24, size.y);
    if (list_view.remove_callback) band = LV_MAX(56, band);
    if (row->subtitle[0]) {
        lv_txt_get_size(&size, row->subtitle, stripe ? &lv_font_montserrat_16 : &lv_font_montserrat_14,
                       0, 0, list_view.width - (stripe ? 40 : 24), LV_TEXT_FLAG_NONE);
        row->sub_height = size.y;
        band += size.y + (stripe ? 3 : 6);
    }
    row->height = LV_MAX(ROW_MIN_H, band + (stripe ? 32 : 20));
    list_view.total_height += row->height + GAP;
    return row;
}

static void list_view_action(lv_event_t *e)
{
    list_view_widget_t *widget = lv_event_get_user_data(e);
    if (!list_view.list || widget->button != lv_event_get_target(e) ||
        widget->model < 0 || widget->model >= list_view.count) return;
    lv_event_cb_t callback = list_view.rows[widget->model].callback;
    if (callback) callback(e);
}

static void list_view_remove(lv_event_t *e)
{
    list_view_widget_t *widget = lv_event_get_user_data(e);
    if (list_view.list && widget->remove == lv_event_get_target(e) &&
        widget->model >= 0 && list_view.remove_callback) list_view.remove_callback(e);
}

static void list_view_refresh(void)
{
    if (!list_view.list || list_view.binding) return;
    list_view.binding = true;
    int y = lv_obj_get_scroll_y(list_view.list), first = 0;
    while (first < list_view.count && list_view.rows[first].y + list_view.rows[first].height < y) first++;
    first = LV_MAX(0, first - 1);
    first = LV_MIN(first, LV_MAX(0, list_view.count - LIST_VIEW_POOL));
    for (int i = 0; i < LIST_VIEW_POOL; i++) {
        list_view_widget_t *widget = &list_view.widgets[i];
        int model = first + i;
        if (model >= list_view.count) { lv_obj_add_flag(widget->button, LV_OBJ_FLAG_HIDDEN); continue; }
        if (widget->model == model) continue;
        widget->model = model;
        const list_view_row_t *row = &list_view.rows[model];
        lv_label_set_text(widget->label, row->title);
        lv_label_set_text(widget->subtitle, row->subtitle);
        if (row->symbol == SHEMA_ICON_COUNT) {
            if (widget->glyph) lv_obj_add_flag(widget->glyph, LV_OBJ_FLAG_HIDDEN);
        } else {
            uint32_t tint = row->text_color == COL_DONE_T ? COL_DONE_T : g_accent_hex;
            if (row->symbol != widget->symbol) {
                if (widget->glyph) shema_icon_set_symbol(widget->glyph, row->symbol, 24);
                else widget->glyph = icon(widget->button, row->symbol, 24, tint);
                widget->symbol = row->symbol;
            }
            shema_icon_set_color(widget->glyph, lv_color_hex(tint));
            lv_obj_clear_flag(widget->glyph, LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_set_style_bg_color(widget->button, lv_color_hex(row->background), 0);
        lv_obj_set_style_border_width(widget->button, row->stripe ? 6 : 0, 0);
        lv_obj_set_style_border_color(widget->button, lv_color_hex(row->edge), 0);
        lv_obj_set_style_text_color(widget->label, lv_color_hex(row->text_color), 0);
        lv_obj_set_style_text_color(widget->subtitle, lv_color_hex(row->sub_color), 0);
        lv_obj_set_style_text_font(widget->subtitle, row->stripe ? &lv_font_montserrat_16 : &lv_font_montserrat_14, 0);
        lv_obj_set_user_data(widget->button, (void *)(intptr_t)row->data);
        lv_obj_set_pos(widget->button, 0, row->y);
        lv_obj_set_height(widget->button, row->height);
        int text_x = row->stripe ? 16 : row->symbol < SHEMA_ICON_COUNT ? 46 : 12;
        int text_w = list_view.width - (list_view.remove_callback ? 84 : row->stripe ? 40 : row->symbol < SHEMA_ICON_COUNT ? 58 : 24);
        int band = row->stripe || row->symbol == SHEMA_ICON_COUNT ? row->title_height : LV_MAX(24, row->title_height);
        if (list_view.remove_callback) band = LV_MAX(56, band);
        if (widget->remove) {
            lv_obj_set_pos(widget->remove, list_view.width - 56, 10 + (band - 44) / 2);
            lv_obj_set_user_data(widget->remove, (void *)(intptr_t)row->data);
        }
        int top = row->sub_height || row->stripe ? 10 : (row->height - band) / 2;
        lv_obj_set_pos(widget->label, text_x, top + (band - row->title_height) / 2);
        lv_obj_set_size(widget->label, text_w, row->title_height);
        lv_obj_set_pos(widget->subtitle, row->stripe ? 16 : 12, top + band + (row->stripe ? 3 : 6));
        lv_obj_set_size(widget->subtitle, list_view.width - (row->stripe ? 40 : 24), row->sub_height);
        if (row->sub_height) lv_obj_clear_flag(widget->subtitle, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(widget->subtitle, LV_OBJ_FLAG_HIDDEN);
        if (widget->glyph && row->symbol < SHEMA_ICON_COUNT)
            lv_obj_set_pos(widget->glyph, 12, top + (band - 24) / 2);
        lv_obj_clear_flag(widget->button, LV_OBJ_FLAG_HIDDEN);
    }
    list_view.binding = false;
}

static void list_view_event(lv_event_t *e)
{
    if (lv_event_get_target(e) != list_view.list) return;
    if (lv_event_get_code(e) == LV_EVENT_DELETE) list_view.list = NULL;
    else if (lv_event_get_code(e) == LV_EVENT_SCROLL) list_view_refresh();
}

static bool list_view_begin(lv_obj_t *list)
{
    memset(&list_view, 0, sizeof list_view);
    if (!list_models) list_models = heap_caps_calloc(LIST_VIEW_MAX, sizeof *list_models, MALLOC_CAP_SPIRAM);
    if (!list_models) return false;
    list_view.rows = list_models;
    list_view.list = list;
    lv_obj_set_layout(list, 0);
    lv_obj_update_layout(list);
    list_view.width = lv_obj_get_content_width(list);
    list_view.spacer = flat(list);
    lv_obj_set_width(list_view.spacer, lv_pct(100));
    lv_obj_set_pos(list_view.spacer, 0, 0);
    lv_obj_add_event_cb(list, list_view_event, LV_EVENT_ALL, NULL);
    return true;
}

static void list_view_finish(int initial_row)
{
    static lv_style_t row_style;
    static bool ready;
    if (!ready) {
        ready = true;
        lv_style_init(&row_style);
        lv_style_set_pad_all(&row_style, 0);
        lv_style_set_radius(&row_style, SHEMA_CARD_RADIUS);
        lv_style_set_border_side(&row_style, LV_BORDER_SIDE_LEFT);
        lv_style_set_bg_opa(&row_style, LV_OPA_COVER);
    }
    lv_obj_set_height(list_view.spacer, LV_MAX(0, list_view.total_height - GAP));
    for (int i = 0; i < LIST_VIEW_POOL; i++) {
        list_view_widget_t *widget = &list_view.widgets[i];
        widget->model = -1;
        widget->symbol = SHEMA_ICON_COUNT;
        widget->button = lv_btn_create(list_view.spacer);
        lv_obj_add_style(widget->button, &row_style, 0);
        lv_obj_set_width(widget->button, lv_pct(100));
        widget->label = lbl(widget->button, "", &lv_font_montserrat_18, COL_TEXT);
        widget->subtitle = lbl(widget->button, "", &lv_font_montserrat_14, COL_SUB);
        lv_label_set_long_mode(widget->label, LV_LABEL_LONG_WRAP);
        lv_label_set_long_mode(widget->subtitle, LV_LABEL_LONG_WRAP);
        lv_obj_add_event_cb(widget->button, list_view_action, LV_EVENT_ALL, widget);
        if (list_view.remove_callback) {
            widget->remove = lv_btn_create(widget->button);
            lv_obj_set_size(widget->remove, 44, 44);
            lv_obj_set_ext_click_area(widget->remove, 6);
            lv_obj_set_style_radius(widget->remove, 12, 0);
            lv_obj_set_style_bg_color(widget->remove, lv_color_hex(0x3A2024), 0);
            icon_mid(widget->remove, SHEMA_ICON_TRASH, 24, 0xFFACA3);
            lv_obj_add_event_cb(widget->remove, list_view_remove, LV_EVENT_ALL, widget);
        }
    }
    list_view_refresh();
    lv_obj_update_layout(list_view.list);
    if (initial_row >= 0 && initial_row < list_view.count)
        lv_obj_scroll_to_y(list_view.list, list_view.rows[initial_row].y, LV_ANIM_OFF);
}
