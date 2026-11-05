// Pull-down shade (like a phone's quick settings): drag down from the status bar,
// drag up or tap the dimmed area to close. Holds a compact brightness row, an optional
// media card and the Sleep button. Its height follows the content: the media card is
// only present while the audio provider reports an active (playing / paused) track.
//
// Layout (x relative to the 20 px side padding, y relative to the top padding of 40 px,
// i.e. just below the 32 px status bar):
//   brightness row   y   0..36   sun icon 24x24 at x 0, slider x 40..430
//   media card       y  46..154  (only while active) 440x108
//   Sleep button     y  46 (no card) or 164 (card), 200x48
//   bottom padding   18 px, the grabber sits in it
// Heights: 40 + 46 + 48 + 18 = 152 px without the card, 270 px with it (UI_SHADE_H).
#include <stdio.h>
#include <string.h>

#include "bsp_es3c35p.h"
#include "os_ui.h"
#include "ui_chrome.h"

#define SNAP_MS 200
#define MIN_BRIGHTNESS 5

#define PAD_X 20
#define PAD_TOP (UI_BAR_H + 8)
#define PAD_BOTTOM 18
#define CONTENT_W (OS_UI_W - 2 * PAD_X)   // 440
#define BRIGHT_H 36
#define SECTION_GAP 10
#define CARD_H 108
#define CARD_PAD 8
#define CARD_INNER_W (CONTENT_W - 2 * CARD_PAD)   // 424
#define SLEEP_H 48
#define POLL_MS 250

static lv_obj_t *s_shade, *s_scrim, *s_sleep_btn;
static lv_obj_t *s_card, *s_title, *s_play_lbl, *s_vol_slider, *s_bar, *s_elapsed, *s_total;
static int32_t s_h;                        // current shade height (see apply_height)
static int32_t s_y;                        // current shade top, from -s_h (closed) to 0 (open)
static int32_t s_start_touch_y, s_start_shade_y;
static bool s_dragging;
static bool s_card_shown;
static lv_timer_t *s_poll;
static bool s_poll_on;
static volatile uint8_t s_brightness = 80;
static void (*s_commit_cb)(uint8_t);

static os_ui_media_cfg_t s_media;
static volatile bool s_have_media;
// Last values pushed to the card widgets, so the 4 Hz poll only touches what changed.
static char s_shown_title[64];
static int32_t s_shown_elapsed = -1, s_shown_total = -2, s_shown_bar = -1;
static int s_shown_playing = -1;

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

void os_ui_set_media_provider(const os_ui_media_cfg_t *cfg)
{
    s_have_media = false;
    if (!cfg) return;
    s_media = *cfg;
    s_have_media = s_media.get != NULL;
}

static void poll_cb(lv_timer_t *t);

static void set_y(int32_t y)
{
    s_y = LV_CLAMP(-s_h, y, 0);
    lv_obj_set_y(s_shade, s_y);
    const int32_t progress = s_h > 0 ? (s_y + s_h) * 100 / s_h : 0;   // 0..100
    lv_obj_set_style_bg_opa(s_scrim, progress * LV_OPA_50 / 100, 0);
    lv_obj_set_hidden(s_scrim, progress == 0);

    // The media poll only runs while some part of the shade is visible.
    const bool visible = progress > 0;
    if (visible != s_poll_on) {
        s_poll_on = visible;
        if (visible) {
            lv_timer_resume(s_poll);
            poll_cb(NULL);
        } else {
            lv_timer_pause(s_poll);
        }
    }
}

// Resizes the shade for the content. A closed shade stays closed (just above the screen).
static void apply_height(bool card)
{
    int32_t sleep_y = BRIGHT_H + SECTION_GAP;
    if (card) sleep_y += CARD_H + SECTION_GAP;
    const int32_t h = PAD_TOP + sleep_y + SLEEP_H + PAD_BOTTOM;
    if (h == s_h && card == s_card_shown) return;

    const bool closed = s_y <= -s_h;
    s_h = h;
    s_card_shown = card;
    lv_obj_set_hidden(s_card, !card);
    lv_obj_set_y(s_sleep_btn, sleep_y);
    lv_obj_set_height(s_shade, h);
    set_y(closed ? -h : s_y);
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
        if (s_y <= -s_h) poll_cb(NULL);       // size the shade for the current media state first
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
            snap_to(open ? 0 : -s_h);
        }
        break;
    default:
        break;
    }
}

void ui_shade_close_now(void)
{
    if (!s_shade) return;
    lv_anim_delete(NULL, anim_cb);
    s_dragging = false;
    set_y(-s_h);
}

// Implemented by os_core (sleep_glue.cpp); weak so os_ui also links without it.
extern void os_core_request_sleep(void) __attribute__((weak));

static void sleep_btn_cb(lv_event_t *e)
{
    ui_shade_close_now();
    if (os_core_request_sleep) os_core_request_sleep();
    else LV_LOG_WARN("sleep requested but os_core is not linked");
}

static void slider_release_cb(lv_event_t *e)
{
    if (s_commit_cb) s_commit_cb(s_brightness);
}

static void slider_cb(lv_event_t *e)
{
    os_ui_set_brightness((uint8_t)lv_slider_get_value(lv_event_get_target_obj(e)));
}

// ---- media card ------------------------------------------------------------------------------

static void set_time(lv_obj_t *lbl, int32_t sec, int32_t *shown)
{
    if (sec == *shown) return;
    *shown = sec;
    if (sec < 0) lv_label_set_text(lbl, "--:--");
    else lv_label_set_text_fmt(lbl, "%d:%02d", (int)(sec / 60), (int)(sec % 60));
}

static void poll_cb(lv_timer_t *t)
{
    os_ui_media_t m;
    memset(&m, 0, sizeof m);
    if (s_have_media) s_media.get(&m);
    const bool active = s_have_media && m.active;
    apply_height(active);
    if (!active) return;

    m.title[sizeof m.title - 1] = 0;
    if (strcmp(m.title, s_shown_title) != 0) {
        memcpy(s_shown_title, m.title, sizeof s_shown_title);
        lv_label_set_text(s_title, m.title);
    }
    if ((int)m.playing != s_shown_playing) {
        s_shown_playing = m.playing;
        lv_label_set_text(s_play_lbl, m.playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    }
    set_time(s_elapsed, (int32_t)(m.pos_ms / 1000), &s_shown_elapsed);
    set_time(s_total, m.dur_ms ? (int32_t)(m.dur_ms / 1000) : -1, &s_shown_total);
    int32_t bar = 0;
    if (m.dur_ms) bar = (int32_t)((uint64_t)LV_MIN(m.pos_ms, m.dur_ms) * 1000 / m.dur_ms);
    if (bar != s_shown_bar) {
        s_shown_bar = bar;
        lv_bar_set_value(s_bar, bar, LV_ANIM_OFF);
    }
    if (!lv_obj_has_state(s_vol_slider, LV_STATE_PRESSED) && lv_slider_get_value(s_vol_slider) != m.volume)
        lv_slider_set_value(s_vol_slider, m.volume, LV_ANIM_OFF);
}

static void prev_cb(lv_event_t *e)
{
    if (s_have_media && s_media.prev) s_media.prev();
}

static void next_cb(lv_event_t *e)
{
    if (s_have_media && s_media.next) s_media.next();
}

static void toggle_cb(lv_event_t *e)
{
    if (s_have_media && s_media.toggle) s_media.toggle();
}

static void volume_cb(lv_event_t *e)
{
    if (s_have_media && s_media.set_volume)
        s_media.set_volume((uint8_t)lv_slider_get_value(lv_event_get_target_obj(e)));
}

// Returns the button's label (used to swap the play / pause glyph).
static lv_obj_t *transport_btn(lv_obj_t *parent, const char *symbol, int32_t x, lv_event_cb_t cb, bool accent)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, 52, 40);
    lv_obj_set_pos(b, x, 28);
    lv_obj_set_style_radius(b, 12, 0);
    lv_obj_set_style_bg_color(b, accent ? lv_palette_main(LV_PALETTE_BLUE) : lv_color_make(0x32, 0x3A, 0x45), 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, symbol);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
    lv_obj_center(l);
    return l;
}

static void media_card_create(lv_obj_t *shade)
{
    s_card = lv_obj_create(shade);
    lv_obj_remove_style_all(s_card);
    lv_obj_set_size(s_card, CONTENT_W, CARD_H);
    lv_obj_set_pos(s_card, 0, BRIGHT_H + SECTION_GAP);
    lv_obj_set_style_bg_color(s_card, lv_color_make(0x26, 0x2D, 0x37), 0);
    lv_obj_set_style_bg_opa(s_card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_card, 12, 0);
    lv_obj_set_style_pad_all(s_card, CARD_PAD, 0);
    lv_obj_set_scrollable(s_card, false);
    lv_obj_remove_flag(s_card, LV_OBJ_FLAG_CLICKABLE);   // touches fall through to the shade (drag to close)

    // Track title, one line, ellipsized.
    s_title = lv_label_create(s_card);
    lv_label_set_long_mode(s_title, LV_LABEL_LONG_DOT);
    lv_obj_set_size(s_title, CARD_INNER_W, 24);
    lv_obj_set_pos(s_title, 0, 0);
    lv_obj_set_style_text_color(s_title, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_title, &lv_font_montserrat_20, 0);
    lv_label_set_text(s_title, "");

    // Transport: previous / play-pause / next (40 px high, y 28..68).
    transport_btn(s_card, LV_SYMBOL_PREV, 0, prev_cb, false);
    s_play_lbl = transport_btn(s_card, LV_SYMBOL_PLAY, 60, toggle_cb, true);
    transport_btn(s_card, LV_SYMBOL_NEXT, 120, next_cb, false);

    // Volume: speaker glyph + 120 px slider on the right.
    lv_obj_t *spk = lv_label_create(s_card);
    lv_label_set_text(spk, LV_SYMBOL_VOLUME_MID);
    lv_obj_set_style_text_color(spk, lv_palette_lighten(LV_PALETTE_GREY, 1), 0);
    lv_obj_set_style_text_font(spk, &lv_font_montserrat_20, 0);
    lv_obj_set_pos(spk, CARD_INNER_W - 130 - 34, 37);

    s_vol_slider = lv_slider_create(s_card);
    lv_slider_set_range(s_vol_slider, 0, 100);
    lv_obj_set_size(s_vol_slider, 120, 12);
    lv_obj_set_pos(s_vol_slider, CARD_INNER_W - 125, 42);
    lv_obj_set_ext_click_area(s_vol_slider, 14);
    lv_obj_add_event_cb(s_vol_slider, volume_cb, LV_EVENT_VALUE_CHANGED, NULL);

    // Progress: elapsed | thin bar | total (16 px high, y 76..92).
    s_elapsed = lv_label_create(s_card);
    lv_obj_set_size(s_elapsed, 44, 16);
    lv_obj_set_pos(s_elapsed, 0, 76);
    lv_obj_set_style_text_font(s_elapsed, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_elapsed, lv_palette_lighten(LV_PALETTE_GREY, 1), 0);
    lv_label_set_text(s_elapsed, "0:00");

    s_bar = lv_bar_create(s_card);
    lv_bar_set_range(s_bar, 0, 1000);
    lv_obj_set_size(s_bar, CARD_INNER_W - 104, 6);
    lv_obj_set_pos(s_bar, 52, 81);

    s_total = lv_label_create(s_card);
    lv_obj_set_size(s_total, 44, 16);
    lv_obj_set_pos(s_total, CARD_INNER_W - 44, 76);
    lv_obj_set_style_text_align(s_total, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_font(s_total, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_total, lv_palette_lighten(LV_PALETTE_GREY, 1), 0);
    lv_label_set_text(s_total, "--:--");
}

// ---- sun icon (24x24: 10 px disc plus 8 rays) -------------------------------------------------

static const lv_point_precise_t s_rays[8][2] = {
    {{12, 1}, {12, 5}},  {{12, 19}, {12, 23}}, {{1, 12}, {5, 12}},  {{19, 12}, {23, 12}},
    {{17, 7}, {20, 4}},  {{7, 7}, {4, 4}},     {{17, 17}, {20, 20}}, {{7, 17}, {4, 20}},
};

static lv_obj_t *sun_icon_create(lv_obj_t *parent)
{
    const lv_color_t c = lv_palette_main(LV_PALETTE_AMBER);
    lv_obj_t *icon = lv_obj_create(parent);
    lv_obj_remove_style_all(icon);
    lv_obj_set_size(icon, 24, 24);
    lv_obj_remove_flag(icon, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *disc = lv_obj_create(icon);
    lv_obj_remove_style_all(disc);
    lv_obj_set_size(disc, 10, 10);
    lv_obj_set_pos(disc, 7, 7);
    lv_obj_set_style_radius(disc, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(disc, c, 0);
    lv_obj_set_style_bg_opa(disc, LV_OPA_COVER, 0);
    lv_obj_remove_flag(disc, LV_OBJ_FLAG_CLICKABLE);

    for (int i = 0; i < 8; i++) {
        lv_obj_t *ray = lv_line_create(icon);
        lv_line_set_points(ray, s_rays[i], 2);
        lv_obj_set_style_line_width(ray, 2, 0);
        lv_obj_set_style_line_color(ray, c, 0);
        lv_obj_set_style_line_rounded(ray, true, 0);
        lv_obj_remove_flag(ray, LV_OBJ_FLAG_CLICKABLE);
    }
    return icon;
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
    lv_obj_set_style_pad_left(s_shade, PAD_X, 0);
    lv_obj_set_style_pad_right(s_shade, PAD_X, 0);
    lv_obj_set_style_pad_top(s_shade, PAD_TOP, 0);
    lv_obj_set_style_pad_bottom(s_shade, PAD_BOTTOM, 0);
    lv_obj_set_scrollable(s_shade, false);
    lv_obj_set_clickable(s_shade, true);
    lv_obj_add_event_cb(s_shade, ui_shade_drag_event, LV_EVENT_ALL, NULL);

    // Brightness row: sun icon + slider, 36 px high.
    lv_obj_t *sun = sun_icon_create(s_shade);
    lv_obj_set_pos(sun, 0, 6);

    lv_obj_t *slider = lv_slider_create(s_shade);
    lv_slider_set_range(slider, MIN_BRIGHTNESS, 100);
    lv_slider_set_value(slider, s_brightness, LV_ANIM_OFF);
    lv_obj_set_size(slider, CONTENT_W - 40 - 10, 20);
    lv_obj_set_pos(slider, 40, 8);
    lv_obj_set_ext_click_area(slider, 8);
    lv_obj_add_event_cb(slider, slider_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(slider, slider_release_cb, LV_EVENT_RELEASED, NULL);

    media_card_create(s_shade);

    s_sleep_btn = lv_button_create(s_shade);
    lv_obj_set_size(s_sleep_btn, 200, SLEEP_H);
    lv_obj_set_x(s_sleep_btn, (CONTENT_W - 200) / 2);
    lv_obj_set_style_bg_color(s_sleep_btn, lv_palette_darken(LV_PALETTE_RED, 2), 0);
    lv_obj_set_style_radius(s_sleep_btn, 14, 0);
    lv_obj_add_event_cb(s_sleep_btn, sleep_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *sleep_lbl = lv_label_create(s_sleep_btn);
    lv_label_set_text(sleep_lbl, LV_SYMBOL_POWER "  Sleep");
    lv_obj_set_style_text_font(sleep_lbl, &lv_font_montserrat_20, 0);
    lv_obj_center(sleep_lbl);

    lv_obj_t *grab = lv_obj_create(s_shade);
    lv_obj_remove_style_all(grab);
    lv_obj_set_size(grab, 48, 5);
    lv_obj_set_style_bg_color(grab, lv_palette_main(LV_PALETTE_GREY), 0);
    lv_obj_set_style_bg_opa(grab, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(grab, 3, 0);
    lv_obj_align(grab, LV_ALIGN_BOTTOM_MID, 0, PAD_BOTTOM - 4);   // sits in the bottom padding
    lv_obj_set_event_bubble(grab, true);

    // Poll timer: created paused, runs only while the shade is (partly) visible.
    s_poll = lv_timer_create(poll_cb, POLL_MS, NULL);
    lv_timer_pause(s_poll);
    s_poll_on = false;

    s_h = 0;
    s_y = 0;
    s_card_shown = true;          // force the first apply_height to lay everything out
    s_shown_title[0] = 0;
    apply_height(false);          // sets s_h and parks the closed shade above the screen
}
