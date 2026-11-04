// Pull-down shade (like a phone's quick settings): drag down from the status bar,
// drag up or tap the dimmed area to close. Holds the brightness slider.
#include "bsp_es3c35p.h"
#include "os_ui.h"
#include "ui_chrome.h"

#define SNAP_MS 200
#define MIN_BRIGHTNESS 5

static lv_obj_t *s_shade, *s_scrim, *s_value;
static int32_t s_y = -UI_SHADE_H;          // current shade top, from -UI_SHADE_H (closed) to 0 (open)
static int32_t s_start_touch_y, s_start_shade_y;
static bool s_dragging;
static volatile uint8_t s_brightness = 80;
static void (*s_commit_cb)(uint8_t);

uint8_t os_ui_get_brightness(void) { return s_brightness; }

void os_ui_preset_brightness(uint8_t pct)
{
    s_brightness = LV_CLAMP(MIN_BRIGHTNESS, pct, 100);
}

void os_ui_set_brightness_commit_cb(void (*cb)(uint8_t pct)) { s_commit_cb = cb; }

void os_ui_set_brightness(uint8_t pct)
{
    if (pct < MIN_BRIGHTNESS) pct = MIN_BRIGHTNESS;
    if (pct > 100) pct = 100;
    s_brightness = pct;
    bsp_backlight_set(pct);
}

static void set_y(int32_t y)
{
    s_y = LV_CLAMP(-UI_SHADE_H, y, 0);
    lv_obj_set_y(s_shade, s_y);
    const int32_t progress = (s_y + UI_SHADE_H) * 100 / UI_SHADE_H;   // 0..100
    lv_obj_set_style_bg_opa(s_scrim, progress * LV_OPA_50 / 100, 0);
    lv_obj_set_hidden(s_scrim, progress == 0);
}

static void anim_cb(void *var, int32_t v) { set_y(v); }

static void snap_to(int32_t target)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_exec_cb(&a, anim_cb);
    lv_anim_set_values(&a, s_y, target);
    lv_anim_set_duration(&a, SNAP_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

// Shared by the status bar (opens), the shade body (closes) and the scrim (closes on tap).
void ui_shade_drag_event(lv_event_t *e)
{
    lv_indev_t *in = lv_indev_active();
    if (!in) return;
    lv_point_t p;
    lv_indev_get_point(in, &p);

    switch (lv_event_get_code(e)) {
    case LV_EVENT_PRESSED:
        lv_anim_delete(NULL, anim_cb);
        s_dragging = true;
        s_start_touch_y = p.y;
        s_start_shade_y = s_y;
        break;
    case LV_EVENT_PRESSING:
        if (s_dragging) set_y(s_start_shade_y + (p.y - s_start_touch_y));
        break;
    case LV_EVENT_RELEASED:
    case LV_EVENT_PRESS_LOST:
        if (!s_dragging) break;
        s_dragging = false;
        {
            const int32_t moved = s_y - s_start_shade_y;
            const bool was_open = s_start_shade_y == 0;
            bool open;
            if (!was_open) open = moved > 40;             // pulled down far enough
            else open = moved > -40;                      // not pushed up far enough
            if (!was_open && moved >= -3 && moved <= 3) open = false;   // plain tap on the bar
            snap_to(open ? 0 : -UI_SHADE_H);
        }
        break;
    default:
        break;
    }
}

static void slider_release_cb(lv_event_t *e)
{
    if (s_commit_cb) s_commit_cb(s_brightness);
}

static void slider_cb(lv_event_t *e)
{
    lv_obj_t *s = lv_event_get_target_obj(e);
    int32_t v = lv_slider_get_value(s);
    os_ui_set_brightness((uint8_t)v);
    lv_label_set_text_fmt(s_value, "%d%%", (int)v);
}

void ui_shade_create(void)
{
    lv_obj_t *top = lv_layer_top();

    s_scrim = lv_obj_create(top);
    lv_obj_remove_style_all(s_scrim);
    lv_obj_set_size(s_scrim, OS_UI_W, OS_UI_H);
    lv_obj_set_style_bg_color(s_scrim, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_scrim, LV_OPA_TRANSP, 0);
    lv_obj_set_clickable(s_scrim, true);
    lv_obj_add_event_cb(s_scrim, ui_shade_drag_event, LV_EVENT_ALL, NULL);
    lv_obj_set_hidden(s_scrim, true);

    s_shade = lv_obj_create(top);
    lv_obj_remove_style_all(s_shade);
    lv_obj_set_size(s_shade, OS_UI_W, UI_SHADE_H);
    lv_obj_set_pos(s_shade, 0, -UI_SHADE_H);
    lv_obj_set_style_bg_color(s_shade, lv_color_make(0x1C, 0x21, 0x28), 0);
    lv_obj_set_style_bg_opa(s_shade, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_shade, 18, 0);
    lv_obj_set_style_pad_all(s_shade, 20, 0);
    lv_obj_set_style_pad_top(s_shade, UI_BAR_H + 14, 0);
    lv_obj_set_scrollable(s_shade, false);
    lv_obj_set_clickable(s_shade, true);
    lv_obj_add_event_cb(s_shade, ui_shade_drag_event, LV_EVENT_ALL, NULL);

    lv_obj_t *title = lv_label_create(s_shade);
    lv_label_set_text(title, "Brightness");
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_event_bubble(title, true);

    s_value = lv_label_create(s_shade);
    lv_label_set_text_fmt(s_value, "%d%%", s_brightness);
    lv_obj_set_style_text_color(s_value, lv_palette_lighten(LV_PALETTE_GREY, 1), 0);
    lv_obj_set_style_text_font(s_value, &lv_font_montserrat_20, 0);
    lv_obj_align(s_value, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_set_event_bubble(s_value, true);

    lv_obj_t *slider = lv_slider_create(s_shade);
    lv_slider_set_range(slider, MIN_BRIGHTNESS, 100);
    lv_slider_set_value(slider, s_brightness, LV_ANIM_OFF);
    lv_obj_set_size(slider, OS_UI_W - 40 - 10, 24);
    lv_obj_align(slider, LV_ALIGN_TOP_MID, 0, 50);
    lv_obj_set_ext_click_area(slider, 16);
    lv_obj_add_event_cb(slider, slider_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(slider, slider_release_cb, LV_EVENT_RELEASED, NULL);

    lv_obj_t *grab = lv_obj_create(s_shade);
    lv_obj_remove_style_all(grab);
    lv_obj_set_size(grab, 48, 5);
    lv_obj_set_style_bg_color(grab, lv_palette_main(LV_PALETTE_GREY), 0);
    lv_obj_set_style_bg_opa(grab, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(grab, 3, 0);
    lv_obj_align(grab, LV_ALIGN_BOTTOM_MID, 0, 4);
    lv_obj_set_event_bubble(grab, true);

    set_y(-UI_SHADE_H);
}
