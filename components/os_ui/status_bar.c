// Status bar: title on the left; USB, charge and battery icons plus percentage on the right.
#include "os_power.h"
#include "os_ui.h"
#include "ui_chrome.h"

static lv_obj_t *s_sd, *s_usb, *s_charge, *s_bat, *s_pct;
static volatile bool s_sd_present;
static bool s_sd_shown;
static lv_obj_t *s_title_btn, *s_title_back, *s_title_name;
static void (*s_back_cb)(void);
static os_power_state_t s_last;
static bool s_have_last;

static const char *battery_glyph(uint8_t pct)
{
    if (pct >= 88) return LV_SYMBOL_BATTERY_FULL;
    if (pct >= 63) return LV_SYMBOL_BATTERY_3;
    if (pct >= 38) return LV_SYMBOL_BATTERY_2;
    if (pct >= 13) return LV_SYMBOL_BATTERY_1;
    return LV_SYMBOL_BATTERY_EMPTY;
}

void os_ui_set_sd_present(bool present) { s_sd_present = present; }

static void refresh(lv_timer_t *t)
{
    if (s_sd_present != s_sd_shown) {
        s_sd_shown = s_sd_present;
        lv_obj_set_hidden(s_sd, !s_sd_shown);
    }
    os_power_state_t st = os_power_get();
    if (st.vbat_mv == 0) return;                          // no sample yet
    if (s_have_last && st.percent == s_last.percent && st.usb == s_last.usb &&
        st.charging == s_last.charging) {
        return;
    }
    s_last = st;
    s_have_last = true;

    lv_color_t col = st.charging ? lv_palette_main(LV_PALETTE_GREEN)
                   : st.percent <= 15 ? lv_palette_main(LV_PALETTE_RED)
                   : lv_color_white();
    lv_label_set_text(s_bat, battery_glyph(st.percent));
    lv_obj_set_style_text_color(s_bat, col, 0);
    lv_label_set_text_fmt(s_pct, "%u%%", st.percent);
    lv_obj_set_style_text_color(s_pct, col, 0);
    lv_obj_set_hidden(s_charge, !st.charging);
    lv_obj_set_hidden(s_usb, !st.usb);
}

static void title_click_cb(lv_event_t *e)
{
    if (s_back_cb) s_back_cb();
}

void os_ui_set_title(const char *name, void (*back_cb)(void))
{
    s_back_cb = back_cb;
    lv_label_set_text(s_title_name, name);
    lv_obj_set_hidden(s_title_back, back_cb == NULL);
    lv_obj_set_clickable(s_title_btn, back_cb != NULL);
    // Without a back action the title lets drags through to the bar (opens the shade).
    lv_obj_set_event_bubble(s_title_btn, back_cb == NULL);
    lv_obj_set_event_bubble(s_title_back, true);
    lv_obj_set_event_bubble(s_title_name, true);
    lv_obj_set_style_text_color(s_title_name, back_cb ? lv_color_white() : lv_palette_lighten(LV_PALETTE_GREY, 1), 0);
}

static lv_obj_t *icon(lv_obj_t *parent, const char *txt)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_color(l, lv_color_white(), 0);
    return l;
}

void ui_status_bar_create(void)
{
    lv_obj_t *bar = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, OS_UI_W, UI_BAR_H);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(bar, 10, 0);
    lv_obj_set_scrollable(bar, false);
    lv_obj_set_clickable(bar, true);
    lv_obj_add_event_cb(bar, ui_shade_drag_event, LV_EVENT_ALL, NULL);

    s_title_btn = lv_obj_create(bar);
    lv_obj_remove_style_all(s_title_btn);
    lv_obj_set_size(s_title_btn, LV_SIZE_CONTENT, UI_BAR_H);
    lv_obj_align(s_title_btn, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_flex_flow(s_title_btn, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_title_btn, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_title_btn, 8, 0);
    lv_obj_set_style_pad_hor(s_title_btn, 4, 0);
    lv_obj_add_event_cb(s_title_btn, title_click_cb, LV_EVENT_CLICKED, NULL);
    s_title_back = icon(s_title_btn, LV_SYMBOL_LEFT);
    s_title_name = icon(s_title_btn, "");
    lv_obj_set_style_text_color(s_title_name, lv_palette_lighten(LV_PALETTE_GREY, 1), 0);
    os_ui_set_title("ES3C35P", NULL);

    lv_obj_t *right = lv_obj_create(bar);
    lv_obj_remove_style_all(right);
    lv_obj_set_size(right, LV_SIZE_CONTENT, UI_BAR_H);
    lv_obj_align(right, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(right, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(right, 8, 0);
    lv_obj_set_clickable(right, false);
    lv_obj_set_event_bubble(right, true);

    s_sd = icon(right, LV_SYMBOL_SD_CARD);
    lv_obj_set_hidden(s_sd, true);
    lv_obj_set_event_bubble(s_sd, true);
    s_usb = icon(right, LV_SYMBOL_USB);
    s_charge = icon(right, LV_SYMBOL_CHARGE);
    s_bat = icon(right, LV_SYMBOL_BATTERY_EMPTY);
    s_pct = icon(right, "--%");
    lv_obj_set_style_text_color(s_charge, lv_palette_main(LV_PALETTE_GREEN), 0);
    lv_obj_set_hidden(s_usb, true);
    lv_obj_set_hidden(s_charge, true);
    lv_obj_set_event_bubble(s_usb, true);
    lv_obj_set_event_bubble(s_charge, true);
    lv_obj_set_event_bubble(s_bat, true);
    lv_obj_set_event_bubble(s_pct, true);

    lv_timer_create(refresh, 1000, NULL);
    refresh(NULL);
}
