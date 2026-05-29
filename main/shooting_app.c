#include "shooting_app.h"

#include "assets_init.h"
#include "front_sight_image.h"
#include "remote_hid.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "esp_log.h"

#define DISP_W                 800
#define DISP_H                 480
#define PX_PER_CM              52
#define MOVE_STEP_PX           8
#define FAST_MOVE_STEP_PX      24
#define ROUND_TARGET_DIAM_PX   (3 * PX_PER_CM)
#define CHEST_TARGET_SIZE_PX   340
#define AIM_FRONT_W            FRONT_SIGHT_IMG_W
#define AIM_FRONT_H            FRONT_SIGHT_IMG_H
#define AIM_PEEP_MIN_DIAM      90
#define AIM_PEEP_MAX_DIAM      280
#define AIM_PEEP_STEP          12
#define FONT_14                app_assets_font_14()
#define FONT_20                app_assets_font_20()
#define FONT_24                app_assets_font_24()
#define FONT_26                app_assets_font_26()

typedef enum {
    APP_SCENE_MENU,
    APP_SCENE_AIM,
    APP_SCENE_CAL_CIRCLE,
    APP_SCENE_CAL_CHEST,
} app_scene_t;

typedef enum {
    CAL_PHASE_COLLECT,
    CAL_PHASE_SHOW_POINTS,
    CAL_PHASE_SHOW_RESULT,
} cal_phase_t;

typedef struct {
    int32_t x;
    int32_t y;
} point_i_t;

typedef struct {
    const char *title;
    app_scene_t scene;
} menu_item_t;

static const char *TAG = "shooting_app";

static lv_obj_t *s_screen;
static lv_obj_t *s_title;
static lv_obj_t *s_status;
static lv_obj_t *s_target;
static lv_display_t *s_disp;
static app_scene_t s_scene = APP_SCENE_MENU;
static uint8_t s_menu_index;

static point_i_t s_target_pos;
static point_i_t s_saved_points[4];
static uint8_t s_point_index;
static cal_phase_t s_cal_phase;
static bool s_cal_is_chest;

static lv_obj_t *s_front_sight;
static lv_obj_t *s_peep;
static point_i_t s_peep_pos;
static int32_t s_peep_diam;

static const point_i_t s_four_point_defaults[4] = {
    { DISP_W / 2, DISP_H / 2 },
    { DISP_W / 4, DISP_H / 4 },
    { (DISP_W * 3) / 4, DISP_H / 4 },
    { DISP_W / 4, (DISP_H * 3) / 4 },
};

static const menu_item_t s_menu_items[] = {
    { "1  联动瞄准模拟", APP_SCENE_AIM },
    { "2  圆靶四点校准", APP_SCENE_CAL_CIRCLE },
    { "4  胸环靶四点校准", APP_SCENE_CAL_CHEST },
};

static uint32_t isqrt32(uint32_t value)
{
    uint32_t op = value;
    uint32_t res = 0;
    uint32_t one = 1UL << 30;

    while (one > op) {
        one >>= 2;
    }

    while (one != 0) {
        if (op >= res + one) {
            op -= res + one;
            res += 2 * one;
        }
        res >>= 1;
        one >>= 2;
    }

    return res;
}

static int32_t clamp_i32(int32_t v, int32_t min_v, int32_t max_v)
{
    if (v < min_v) {
        return min_v;
    }
    if (v > max_v) {
        return max_v;
    }
    return v;
}

static void style_plain(lv_obj_t *obj)
{
    lv_obj_remove_style_all(obj);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE);
}

static lv_obj_t *make_label(lv_obj_t *parent, const char *text, int32_t x, int32_t y,
                            const lv_font_t *font, uint32_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_clear_flag(label, LV_OBJ_FLAG_SCROLLABLE);
    return label;
}

static lv_obj_t *make_rect(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h,
                           uint32_t fill, lv_opa_t fill_opa, uint32_t border,
                           int32_t border_w, int32_t radius)
{
    lv_obj_t *obj = lv_obj_create(parent);
    style_plain(obj);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_bg_color(obj, lv_color_hex(fill), 0);
    lv_obj_set_style_bg_opa(obj, fill_opa, 0);
    lv_obj_set_style_border_color(obj, lv_color_hex(border), 0);
    lv_obj_set_style_border_width(obj, border_w, 0);
    lv_obj_set_style_radius(obj, radius, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    return obj;
}

static lv_obj_t *make_circle(lv_obj_t *parent, int32_t cx, int32_t cy, int32_t diam,
                             uint32_t fill, lv_opa_t fill_opa, uint32_t border, int32_t border_w)
{
    return make_rect(parent, cx - diam / 2, cy - diam / 2, diam, diam,
                     fill, fill_opa, border, border_w, LV_RADIUS_CIRCLE);
}

static void set_obj_center(lv_obj_t *obj, int32_t cx, int32_t cy, int32_t w, int32_t h)
{
    lv_obj_set_size(obj, w, h);
    lv_obj_set_pos(obj, cx - w / 2, cy - h / 2);
}

static void set_screen_bg(uint32_t color)
{
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
}

static void focus_main_screen(void)
{
    lv_group_t *group = lv_group_get_default();
    if (group) {
        lv_group_add_obj(group, s_screen);
        lv_group_focus_obj(s_screen);
        lv_group_set_editing(group, true);
    }
}

static void clear_scene(void)
{
    lv_obj_clean(s_screen);
    s_title = NULL;
    s_status = NULL;
    s_target = NULL;
    s_front_sight = NULL;
    s_peep = NULL;
}

static void update_status(const char *text)
{
    if (s_status) {
        lv_label_set_text(s_status, text);
    }
}

static void load_menu(void);
static void load_aim_scene(void);
static void load_cal_scene(bool chest);

static void menu_item_event_cb(lv_event_t *e);

static void menu_refresh(void)
{
    clear_scene();
    set_screen_bg(0x151a1f);
    s_scene = APP_SCENE_MENU;

    make_label(s_screen, "射击训练", 30, 24, FONT_26, 0xffffff);
    make_label(s_screen, "摇杆或C/D选择   O确认", 34, 62, FONT_20, 0x9fb1c1);

    for (uint8_t i = 0; i < (uint8_t)(sizeof(s_menu_items) / sizeof(s_menu_items[0])); i++) {
        int32_t y = 128 + i * 78;
        uint32_t fill = (i == s_menu_index) ? 0x2d5f83 : 0x232a31;
        uint32_t border = (i == s_menu_index) ? 0x9ad7ff : 0x404953;
        lv_obj_t *row = make_rect(s_screen, 70, y, 660, 54, fill, LV_OPA_COVER, border, 2, 6);
        lv_obj_t *label = make_label(row, s_menu_items[i].title, 22, 15, FONT_20,
                                     i == s_menu_index ? 0xffffff : 0xc8d1dc);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, menu_item_event_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
        (void)label;
    }
}

static void load_menu(void)
{
    menu_refresh();
}

static bool aim_is_aligned(void)
{
    const int32_t front_cx = DISP_W / 2;
    const int32_t front_cy = DISP_H / 2;
    const int32_t dx = abs(s_peep_pos.x - front_cx);
    const int32_t dy = abs(s_peep_pos.y - front_cy);
    const int32_t peep_r = s_peep_diam / 2;
    const int32_t front_rx = AIM_FRONT_W / 2;
    const int32_t front_ry = AIM_FRONT_H / 2;

    return peep_r >= dx + front_rx + 6 && peep_r >= dy + front_ry + 6;
}

static void aim_update(void)
{
    set_obj_center(s_peep, s_peep_pos.x, s_peep_pos.y, s_peep_diam, s_peep_diam);

    if (aim_is_aligned()) {
        lv_obj_set_style_text_color(s_status, lv_color_hex(0x70ff98), 0);
        update_status("已瞄准");
    } else {
        lv_obj_set_style_text_color(s_status, lv_color_hex(0xffffff), 0);
        update_status("未瞄准");
    }
}

static void create_front_sight(void)
{
    const int32_t cx = DISP_W / 2;
    const int32_t cy = DISP_H / 2;

    s_front_sight = lv_image_create(s_screen);
    lv_image_set_src(s_front_sight, FRONT_SIGHT_IMAGE_SRC);
    lv_obj_set_pos(s_front_sight, cx - FRONT_SIGHT_IMG_W / 2,
                   cy - FRONT_SIGHT_IMG_H / 2);
    lv_obj_clear_flag(s_front_sight, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_front_sight, LV_OBJ_FLAG_SCROLLABLE);
}

static void load_aim_scene(void)
{
    clear_scene();
    set_screen_bg(0x245c38);
    s_scene = APP_SCENE_AIM;

    s_title = make_label(s_screen, "1 联动瞄准", 18, 16, FONT_24, 0xffffff);
    s_status = make_label(s_screen, "未瞄准", 650, 18, FONT_20, 0xffffff);
    make_label(s_screen, "摇杆移动   C放大   D缩小   ESC",
               20, 446, FONT_20, 0xe3f3e6);

    s_peep_pos.x = DISP_W / 2 - 135;
    s_peep_pos.y = DISP_H / 2 - 52;
    s_peep_diam = 150;

    create_front_sight();

    /* Rear peep is the movable/zoomable outer ring, so create it last to keep it on top. */
    s_peep = make_circle(s_screen, s_peep_pos.x, s_peep_pos.y, s_peep_diam,
                         0x245c38, LV_OPA_TRANSP, 0x050505, 10);
    aim_update();
}

static void four_point_target_default(void)
{
    s_target_pos = s_four_point_defaults[s_point_index];
}

static void clamp_circle_target(void)
{
    int32_t r = ROUND_TARGET_DIAM_PX / 2;
    s_target_pos.x = clamp_i32(s_target_pos.x, r, DISP_W - r);
    s_target_pos.y = clamp_i32(s_target_pos.y, r, DISP_H - r);
}

static void clamp_chest_target(void)
{
    s_target_pos.x = clamp_i32(s_target_pos.x, 0, DISP_W - 1);
    s_target_pos.y = clamp_i32(s_target_pos.y, 0, DISP_H - 1);
}

static void target_apply_pos(void)
{
    if (!s_target) {
        return;
    }

    if (s_cal_is_chest) {
        clamp_chest_target();
        set_obj_center(s_target, s_target_pos.x, s_target_pos.y,
                       CHEST_TARGET_SIZE_PX, CHEST_TARGET_SIZE_PX);
    } else {
        clamp_circle_target();
        set_obj_center(s_target, s_target_pos.x, s_target_pos.y,
                       ROUND_TARGET_DIAM_PX, ROUND_TARGET_DIAM_PX);
    }
}

static void update_cal_status(void)
{
    char buf[96];
    if (s_cal_phase == CAL_PHASE_COLLECT) {
        snprintf(buf, sizeof(buf), "第%u/4点  X:%ld Y:%ld",
                 (unsigned)(s_point_index + 1),
                 (long)s_target_pos.x,
                 (long)s_target_pos.y);
    } else if (s_cal_phase == CAL_PHASE_SHOW_POINTS) {
        snprintf(buf, sizeof(buf), "已显示保存点   O计算");
    } else {
        snprintf(buf, sizeof(buf), "O重置   ESC");
    }
    update_status(buf);
}

static void draw_round_target(void)
{
    s_target = make_circle(s_screen, s_target_pos.x, s_target_pos.y, ROUND_TARGET_DIAM_PX,
                           0xffffff, LV_OPA_COVER, 0xffffff, 0);
}

static void draw_chest_target(void)
{
    const int32_t size = CHEST_TARGET_SIZE_PX;
    s_target = lv_obj_create(s_screen);
    style_plain(s_target);
    lv_obj_set_size(s_target, size, size);
    lv_obj_set_style_bg_opa(s_target, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_target, 0, 0);
    lv_obj_set_style_pad_all(s_target, 0, 0);

    make_circle(s_target, size / 2, 72, 64, 0xc8b58b, LV_OPA_COVER, 0x1f1f1f, 2);
    make_rect(s_target, 74, 96, 192, 210, 0xc8b58b, LV_OPA_COVER, 0x1f1f1f, 3, 34);
    make_rect(s_target, 48, 154, 244, 130, 0xc8b58b, LV_OPA_COVER, 0x1f1f1f, 3, 28);

    const uint32_t ring_colors[] = { 0x111111, 0xffffff, 0x111111, 0xffffff, 0x111111 };
    const int32_t ring_diams[] = { 190, 154, 118, 82, 46 };
    for (uint8_t i = 0; i < 5; i++) {
        make_circle(s_target, size / 2, size / 2, ring_diams[i],
                    0xffffff, LV_OPA_TRANSP, ring_colors[i], 4);
    }
    make_circle(s_target, size / 2, size / 2, 12, 0xff3333, LV_OPA_COVER, 0xffffff, 2);
    make_label(s_target, "10", size / 2 + 12, size / 2 - 9, FONT_14, 0x111111);

    target_apply_pos();
}

static void create_cal_base(bool chest)
{
    clear_scene();
    set_screen_bg(chest ? 0x1f4e2c : 0x000000);
    s_scene = chest ? APP_SCENE_CAL_CHEST : APP_SCENE_CAL_CIRCLE;
    s_cal_is_chest = chest;

    s_title = make_label(s_screen, chest ? "4 胸环靶四点校准" : "2 圆靶四点校准",
                         18, 16, FONT_24, 0xffffff);
    s_status = make_label(s_screen, "", 430, 18, FONT_20, 0xffffff);
    make_label(s_screen, "A右  B左  C上  D下  O确认  ESC",
               20, 446, FONT_20, 0xdde7ef);

    if (chest) {
        draw_chest_target();
    } else {
        draw_round_target();
    }
    target_apply_pos();
    update_cal_status();
}

static void load_cal_scene(bool chest)
{
    s_point_index = 0;
    s_cal_phase = CAL_PHASE_COLLECT;
    s_cal_is_chest = chest;
    for (uint8_t i = 0; i < 4; i++) {
        s_saved_points[i].x = 0;
        s_saved_points[i].y = 0;
    }
    four_point_target_default();
    create_cal_base(chest);
}

static void mark_saved_point(point_i_t p, uint8_t idx)
{
    make_circle(s_screen, p.x, p.y, 16, 0xffffff, LV_OPA_COVER, 0xffffff, 0);
    make_circle(s_screen, p.x, p.y, 9, 0x111111, LV_OPA_COVER, 0x111111, 0);

    char label_buf[4];
    snprintf(label_buf, sizeof(label_buf), "%u", (unsigned)(idx + 1));
    lv_obj_t *label = make_label(s_screen, label_buf, p.x + 12, p.y - 18,
                                 FONT_14, 0xffffff);
    (void)label;
}

static void show_saved_points(void)
{
    clear_scene();
    set_screen_bg(s_cal_is_chest ? 0x1f4e2c : 0x000000);
    s_title = make_label(s_screen, s_cal_is_chest ? "4 已保存胸环中心" : "2 已保存圆靶点位",
                         18, 16, FONT_24, 0xffffff);
    s_status = make_label(s_screen, "", 500, 18, FONT_20, 0xffffff);
    make_label(s_screen, "O计算   ESC", 20, 446, FONT_20, 0xdde7ef);

    for (uint8_t i = 0; i < 4; i++) {
        mark_saved_point(s_saved_points[i], i);
    }
    s_cal_phase = CAL_PHASE_SHOW_POINTS;
    update_cal_status();
}

static void show_score(void)
{
    int32_t sum_x = 0;
    int32_t sum_y = 0;
    for (uint8_t i = 0; i < 4; i++) {
        sum_x += s_saved_points[i].x;
        sum_y += s_saved_points[i].y;
    }
    const int32_t avg_x = sum_x / 4;
    const int32_t avg_y = sum_y / 4;

    uint32_t max_dist = 0;
    uint32_t point_dist[4] = { 0 };
    for (uint8_t i = 0; i < 4; i++) {
        int32_t dx = s_saved_points[i].x - avg_x;
        int32_t dy = s_saved_points[i].y - avg_y;
        uint32_t dist = isqrt32((uint32_t)(dx * dx + dy * dy));
        point_dist[i] = dist;
        if (point_dist[i] > max_dist) {
            max_dist = dist;
        }
    }

    clear_scene();
    set_screen_bg(0x111820);
    s_title = make_label(s_screen, "校准结果", 28, 28, FONT_26, 0xffffff);
    s_status = make_label(s_screen, "", 520, 28, FONT_20, 0xffffff);

    char line[128];
    snprintf(line, sizeof(line), "中心点：X %ld   Y %ld", (long)avg_x, (long)avg_y);
    make_label(s_screen, line, 70, 130, FONT_24, 0xcfe4ff);

    snprintf(line, sizeof(line), "最大分散误差：%lu px", (unsigned long)max_dist);
    make_label(s_screen, line, 70, 184, FONT_26, 0xffffff);

    snprintf(line, sizeof(line), "第1点误差：%lu px   第2点误差：%lu px",
             (unsigned long)point_dist[0], (unsigned long)point_dist[1]);
    make_label(s_screen, line, 70, 238, FONT_20, 0xdde7ef);

    snprintf(line, sizeof(line), "第3点误差：%lu px   第4点误差：%lu px",
             (unsigned long)point_dist[2], (unsigned long)point_dist[3]);
    make_label(s_screen, line, 70, 278, FONT_20, 0xdde7ef);

    make_label(s_screen, "O重置   ESC", 70, 346, FONT_20, 0x9fb1c1);
    s_cal_phase = CAL_PHASE_SHOW_RESULT;
    update_cal_status();
}

static void save_current_point(void)
{
    s_saved_points[s_point_index] = s_target_pos;
    ESP_LOGI(TAG, "Saved point %u: x=%ld y=%ld",
             (unsigned)(s_point_index + 1), (long)s_target_pos.x, (long)s_target_pos.y);

    if (s_point_index < 3) {
        s_point_index++;
        four_point_target_default();
        target_apply_pos();
        update_cal_status();
    } else {
        show_saved_points();
    }
}

static void move_target(int32_t dx, int32_t dy)
{
    s_target_pos.x += dx;
    s_target_pos.y += dy;
    target_apply_pos();
    update_cal_status();
}

static void enter_selected_menu_item(void)
{
    app_scene_t next_scene = s_menu_items[s_menu_index].scene;
    if (next_scene == APP_SCENE_AIM) {
        load_aim_scene();
    } else if (next_scene == APP_SCENE_CAL_CIRCLE) {
        load_cal_scene(false);
    } else if (next_scene == APP_SCENE_CAL_CHEST) {
        load_cal_scene(true);
    }
}

static void menu_item_event_cb(lv_event_t *e)
{
    s_menu_index = (uint8_t)(uintptr_t)lv_event_get_user_data(e);
    enter_selected_menu_item();
}

static uint32_t normalize_menu_key(uint32_t key)
{
    switch (key) {
    case REMOTE_KEY_LOOKBON_JOY_UP:
    case REMOTE_KEY_LOOKBON_BTN_C:
        return LV_KEY_UP;
    case REMOTE_KEY_LOOKBON_JOY_DOWN:
    case REMOTE_KEY_LOOKBON_BTN_D:
        return LV_KEY_DOWN;
    case REMOTE_KEY_LOOKBON_JOY_LEFT:
    case REMOTE_KEY_LOOKBON_BTN_B:
        return LV_KEY_LEFT;
    case REMOTE_KEY_LOOKBON_JOY_RIGHT:
    case REMOTE_KEY_LOOKBON_BTN_A:
        return LV_KEY_RIGHT;
    case REMOTE_KEY_LOOKBON_BTN_O:
        return LV_KEY_ENTER;
    case REMOTE_KEY_LOOKBON_BTN_BACK:
        return LV_KEY_ESC;
    default:
        return key;
    }
}

static uint32_t normalize_aim_key(uint32_t key)
{
    switch (key) {
    case REMOTE_KEY_LOOKBON_JOY_UP:
        return LV_KEY_UP;
    case REMOTE_KEY_LOOKBON_JOY_DOWN:
        return LV_KEY_DOWN;
    case REMOTE_KEY_LOOKBON_JOY_LEFT:
        return LV_KEY_LEFT;
    case REMOTE_KEY_LOOKBON_JOY_RIGHT:
        return LV_KEY_RIGHT;
    case REMOTE_KEY_LOOKBON_BTN_C:
        return LV_KEY_NEXT;
    case REMOTE_KEY_LOOKBON_BTN_D:
        return LV_KEY_PREV;
    case REMOTE_KEY_LOOKBON_BTN_BACK:
        return LV_KEY_ESC;
    default:
        return key;
    }
}

static uint32_t normalize_cal_key(uint32_t key)
{
    switch (key) {
    case REMOTE_KEY_LOOKBON_JOY_UP:
    case REMOTE_KEY_LOOKBON_BTN_C:
        return LV_KEY_UP;
    case REMOTE_KEY_LOOKBON_JOY_DOWN:
    case REMOTE_KEY_LOOKBON_BTN_D:
        return LV_KEY_DOWN;
    case REMOTE_KEY_LOOKBON_JOY_LEFT:
    case REMOTE_KEY_LOOKBON_BTN_B:
        return LV_KEY_LEFT;
    case REMOTE_KEY_LOOKBON_JOY_RIGHT:
    case REMOTE_KEY_LOOKBON_BTN_A:
        return LV_KEY_RIGHT;
    case REMOTE_KEY_LOOKBON_BTN_O:
        return LV_KEY_ENTER;
    case REMOTE_KEY_LOOKBON_BTN_BACK:
        return LV_KEY_ESC;
    default:
        return key;
    }
}

static uint32_t normalize_scene_key(uint32_t key)
{
    if (s_scene == APP_SCENE_MENU) {
        return normalize_menu_key(key);
    }
    if (s_scene == APP_SCENE_AIM) {
        return normalize_aim_key(key);
    }
    return normalize_cal_key(key);
}

static void handle_menu_key(uint32_t key)
{
    const uint8_t item_count = (uint8_t)(sizeof(s_menu_items) / sizeof(s_menu_items[0]));
    if (key == LV_KEY_UP || key == LV_KEY_PREV || key == LV_KEY_LEFT) {
        s_menu_index = (s_menu_index == 0) ? (item_count - 1) : (s_menu_index - 1);
        menu_refresh();
    } else if (key == LV_KEY_DOWN || key == LV_KEY_NEXT || key == LV_KEY_RIGHT) {
        s_menu_index = (s_menu_index + 1) % item_count;
        menu_refresh();
    } else if (key == LV_KEY_ENTER) {
        enter_selected_menu_item();
    }
}

static void handle_aim_key(uint32_t key)
{
    int32_t step = MOVE_STEP_PX;
    if (key == LV_KEY_UP) {
        s_peep_pos.y -= step;
    } else if (key == LV_KEY_DOWN) {
        s_peep_pos.y += step;
    } else if (key == LV_KEY_LEFT) {
        s_peep_pos.x -= step;
    } else if (key == LV_KEY_RIGHT) {
        s_peep_pos.x += step;
    } else if (key == LV_KEY_NEXT) {
        s_peep_diam += AIM_PEEP_STEP;
    } else if (key == LV_KEY_PREV) {
        s_peep_diam -= AIM_PEEP_STEP;
    } else if (key == LV_KEY_ENTER) {
        s_peep_pos.x = DISP_W / 2 - 135;
        s_peep_pos.y = DISP_H / 2 - 52;
        s_peep_diam = 150;
    } else if (key == LV_KEY_ESC) {
        load_menu();
        return;
    }

    s_peep_pos.x = clamp_i32(s_peep_pos.x, 0, DISP_W);
    s_peep_pos.y = clamp_i32(s_peep_pos.y, 0, DISP_H);
    s_peep_diam = clamp_i32(s_peep_diam, AIM_PEEP_MIN_DIAM, AIM_PEEP_MAX_DIAM);
    aim_update();
}

static void handle_cal_key(uint32_t key)
{
    if (key == LV_KEY_ESC) {
        load_menu();
        return;
    }

    if (s_cal_phase == CAL_PHASE_SHOW_POINTS) {
        if (key == LV_KEY_ENTER) {
            show_score();
        }
        return;
    }

    if (s_cal_phase == CAL_PHASE_SHOW_RESULT) {
        if (key == LV_KEY_ENTER) {
            load_cal_scene(s_cal_is_chest);
        }
        return;
    }

    int32_t step = MOVE_STEP_PX;
    if (key == LV_KEY_NEXT || key == LV_KEY_PREV) {
        step = FAST_MOVE_STEP_PX;
    }

    switch (key) {
    case LV_KEY_UP:
        move_target(0, -step);
        break;
    case LV_KEY_DOWN:
        move_target(0, step);
        break;
    case LV_KEY_LEFT:
        move_target(-step, 0);
        break;
    case LV_KEY_RIGHT:
        move_target(step, 0);
        break;
    case LV_KEY_ENTER:
        save_current_point();
        break;
    default:
        break;
    }
}

static void screen_key_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_KEY) {
        return;
    }

    uint32_t key = lv_event_get_key(e);
    key = normalize_scene_key(key);

    if (s_scene == APP_SCENE_MENU) {
        handle_menu_key(key);
    } else if (s_scene == APP_SCENE_AIM) {
        handle_aim_key(key);
    } else {
        handle_cal_key(key);
    }
}

static void screen_pointer_event_cb(lv_event_t *e)
{
    if (s_scene == APP_SCENE_MENU) {
        return;
    }

    if (s_scene != APP_SCENE_AIM && s_cal_phase != CAL_PHASE_COLLECT) {
        return;
    }

    lv_indev_t *indev = lv_event_get_indev(e);
    if (indev == NULL) {
        return;
    }

    lv_point_t p;
    lv_indev_get_point(indev, &p);
    if (s_scene == APP_SCENE_AIM) {
        s_peep_pos.x = clamp_i32(p.x, 0, DISP_W);
        s_peep_pos.y = clamp_i32(p.y, 0, DISP_H);
        aim_update();
    } else {
        s_target_pos.x = clamp_i32(p.x, 0, DISP_W - 1);
        s_target_pos.y = clamp_i32(p.y, 0, DISP_H - 1);
        target_apply_pos();
        update_cal_status();
    }
}

void shooting_app_start(lv_display_t *disp)
{
    s_disp = disp;
    (void)s_disp;

    if (s_screen) {
        lv_obj_delete(s_screen);
        s_screen = NULL;
    }

    s_screen = lv_obj_create(NULL);
    lv_obj_set_size(s_screen, DISP_W, DISP_H);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_screen, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_screen, screen_key_event_cb, LV_EVENT_KEY, NULL);
    lv_obj_add_event_cb(s_screen, screen_pointer_event_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_screen, screen_pointer_event_cb, LV_EVENT_PRESSING, NULL);
    focus_main_screen();

    s_menu_index = 0;
    load_menu();
    lv_scr_load(s_screen);
    focus_main_screen();
}
