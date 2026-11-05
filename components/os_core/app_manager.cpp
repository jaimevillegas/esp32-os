#include <cstdio>
#include <cstring>
#include <new>

#include "core_internal.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "lvgl.h"
#include "os_ui.h"

namespace os::core {

static const char* TAG = "apps";

static Slot* s_slots;
static size_t s_count;
static Slot* s_fg;
static lv_obj_t* s_fg_screen;

// Heap levels recorded at launch to report drift after closing.
static long s_int_before, s_psram_before;

size_t slotCount() { return s_count; }
Slot* slotAt(size_t idx) { return idx < s_count ? &s_slots[idx] : nullptr; }
Slot* foreground() { return s_fg; }

void managerInit() {
  s_count = appCount();
  s_slots = new Slot[s_count ? s_count : 1];
  for (size_t i = 0; i < s_count; i++) {
    Slot& s = s_slots[i];
    s.m = &appManifest(i);
    s.id = (uint16_t)(i + 1);
    char ns[16];
    snprintf(ns, sizeof ns, "a.%.12s", s.m->id);
    s.kv.configure("nvs_app", ns);
  }
}

// The arena of an app with a background Service lives for the whole uptime; the others only
// while the app is open.
bool ensureArena(Slot& s) {
  if (s.arena.live()) return true;
  if (!s.arena.init(s.m->psram_arena)) {
    ESP_LOGE(TAG, "no PSRAM for the arena of %s", s.m->id);
    return false;
  }
  s.ctx = new (s.arena.allocate(sizeof(Context), alignof(Context)))
      Context(bus(), s.kv, device(), s.arena, s.m->perms,
              Services{fsService(), audioService(), netService(), bleService(), ledService(),
                       recorderService()});
  return true;
}

void launch(size_t idx) {
  Slot* s = slotAt(idx);
  if (!s || s_fg) return;
  if (!ensureArena(*s)) return;

  s_int_before = (long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  s_psram_before = (long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

  s->app = s->m->make(s->arena);
  s_fg = s;

  lv_obj_t* scr = lv_obj_create(nullptr);
  lv_obj_set_style_bg_color(scr, lv_color_make(0x0E, 0x11, 0x16), 0);
  lv_obj_set_scrollable(scr, false);
  lv_obj_t* root = lv_obj_create(scr);
  lv_obj_remove_style_all(root);
  lv_obj_set_pos(root, 0, OS_UI_BAR_H);
  lv_obj_set_size(root, OS_UI_W, OS_UI_H - OS_UI_BAR_H);
  lv_obj_set_style_pad_all(root, 12, 0);
  lv_obj_set_scrollable(root, false);
  s_fg_screen = scr;

  s->app->onCreate(*s->ctx);
  s->app->onStart(root);
  lv_screen_load(scr);
  os_ui_set_title(s->m->name, closeForeground);
  s->app->onResume();
  ESP_LOGI(TAG, "opened %s (arena %u/%u B)", s->m->id, (unsigned)s->arena.used(),
           (unsigned)s->arena.capacity());
}

void closeForeground() {
  Slot* s = s_fg;
  if (!s) return;
  s->app->onPause();
  s->app->onStop();
  lv_screen_load(launcherScreen());
  lv_obj_delete(s_fg_screen);          // removes every LVGL object of the app
  s_fg_screen = nullptr;
  os_ui_set_title("ES3C35P", nullptr);
  s->app->onDestroy();
  s->app->~App();
  s->arena.deallocate(s->app, 0);
  s->app = nullptr;
  s_fg = nullptr;

  if (!s->m->background) {
    s->ctx->~Context();
    s->arena.deinit();                 // discards everything the app allocated
    s->ctx = nullptr;
  }

  // Leak check: compare the heaps before the launch and after the close.
  const long di = (long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL) - s_int_before;
  const long dp = (long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM) - s_psram_before;
  const bool bad = di < -1024 || dp < -4096;
  if (bad) {
    ESP_LOGW(TAG, "closed %s: internal drift %ld B, PSRAM drift %ld B  <- possible leak", s->m->id, di, dp);
  } else {
    ESP_LOGI(TAG, "closed %s: internal drift %ld B, PSRAM drift %ld B", s->m->id, di, dp);
  }
}

static void asyncClose(void*) { closeForeground(); }
void closeForegroundAsync() { lv_async_call(asyncClose, nullptr); }

}  // namespace os::core
