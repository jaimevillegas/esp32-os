// H1 hardware test screen: orientation arrow, corner markers, 1 px border,
// colour bars and a live touch readout.
#include "os_ui.h"

#include "lvgl.h"

static lv_obj_t *s_touch_label;

static lv_obj_t *box(lv_obj_t *parent, int x, int y, int w, int h, lv_color_t c)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, c, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_event_bubble(o, true);   // let the root screen see the press
    return o;
}

static void touch_cb(lv_event_t *e)
{
    lv_indev_t *in = lv_indev_active();
    if (!in) return;
    lv_point_t p;
    lv_indev_get_point(in, &p);
    lv_label_set_text_fmt(s_touch_label, "touch x=%d y=%d", (int)p.x, (int)p.y);
}

void os_ui_test_screen(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_add_event_cb(scr, touch_cb, LV_EVENT_PRESSING, NULL);

    // 1 px white border on the very edge: shows clipping / x4 alignment issues.
    lv_obj_t *border = lv_obj_create(scr);
    lv_obj_remove_style_all(border);
    lv_obj_set_size(border, OS_UI_W, OS_UI_H);
    lv_obj_set_style_border_width(border, 1, 0);
    lv_obj_set_style_border_color(border, lv_color_white(), 0);
    lv_obj_set_style_border_opa(border, LV_OPA_COVER, 0);
    lv_obj_set_event_bubble(border, true);

    // Corner markers: red TL, green TR, blue BL, yellow BR.
    box(scr, 0, 0, 40, 40, lv_palette_main(LV_PALETTE_RED));
    box(scr, OS_UI_W - 40, 0, 40, 40, lv_palette_main(LV_PALETTE_GREEN));
    box(scr, 0, OS_UI_H - 40, 40, 40, lv_palette_main(LV_PALETTE_BLUE));
    box(scr, OS_UI_W - 40, OS_UI_H - 40, 40, 40, lv_palette_main(LV_PALETTE_YELLOW));

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "ES3C35P  480x320\nTop-left is RED. Text must read normally.\n"
                             LV_SYMBOL_RIGHT " arrow points right, " LV_SYMBOL_UP " up");
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 50);

    // Colour bars: wrong channel order or byte swap is obvious here.
    static const lv_palette_t pal[] = { LV_PALETTE_RED, LV_PALETTE_GREEN, LV_PALETTE_BLUE, LV_PALETTE_GREY };
    for (int i = 0; i < 4; i++) {
        box(scr, 60 + i * 90, 170, 90, 50, lv_palette_main(pal[i]));
    }

    s_touch_label = lv_label_create(scr);
    lv_label_set_text(s_touch_label, "touch: none");
    lv_obj_set_style_text_color(s_touch_label, lv_color_white(), 0);
    lv_obj_align(s_touch_label, LV_ALIGN_BOTTOM_MID, 0, -50);
}
