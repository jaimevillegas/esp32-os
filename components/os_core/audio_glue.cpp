// Glue between svc_audio and the framework (Audio for apps, Topic::Audio events).
//
// Topic::Audio payload: bits 0-7 PlayState, bits 8-15 volume, bit 16 = the track played to its
// end (state is Stopped). Events are published on state changes only, never on volume changes
// (a slider drag would flood the bus queues and could drop the end-of-track event).
#include "core_internal.h"
#include "sdkconfig.h"
#include "esp_log.h"
#include "freertos/task.h"
#include "freertos/FreeRTOS.h"
#include "esp_timer.h"
#include "os_ui.h"
#include "svc_audio.h"

namespace os::core {

static const uint32_t kDefaultVolume = 60;
static const uint64_t kPersistDelayUs = 1500 * 1000;

static esp_timer_handle_t s_persist_timer;
static volatile uint8_t s_volume;

static void onPersist(void*) { systemKv().setU32("volume", s_volume); }

static void onAudioEvent(const svc_audio_status_t* st, bool ended) {
  Msg m{};
  m.topic = Topic::Audio;
  m.arg = (uint32_t)st->state | ((uint32_t)st->volume << 8) | (ended ? (1u << 16) : 0u);
  bus().publish(m);
}

class AudioImpl final : public Audio {
 public:
  bool play(const char* path) override { return svc_audio_play(path); }
  void pause() override { svc_audio_pause(); }
  void resume() override { svc_audio_resume(); }
  void stop() override { svc_audio_stop(); }
  bool seek(uint32_t ms) override { return svc_audio_seek(ms); }

  void setVolume(uint8_t percent) override {
    if (percent > 100) percent = 100;
    svc_audio_set_volume(percent);
    s_volume = percent;
    // Debounced: a slider drag must not write flash on every step.
    if (s_persist_timer) {
      esp_timer_stop(s_persist_timer);
      esp_timer_start_once(s_persist_timer, kPersistDelayUs);
    }
  }

  void status(AudioStatus& out) const override {
    svc_audio_status_t st;
    svc_audio_get_status(&st);
    out.state = (PlayState)st.state;
    out.pos_ms = st.pos_ms;
    out.dur_ms = st.dur_ms;
    out.volume = st.volume;
    __builtin_memcpy(out.title, st.title, sizeof out.title);
    out.title[sizeof out.title - 1] = 0;
  }
};

static AudioImpl s_audio;
static bool s_started;

// ---- shade media card provider (os_ui calls these from the ui task) ----------------------------

static void shadeGet(os_ui_media_t* out) {
  svc_audio_status_t st;
  svc_audio_get_status(&st);
  out->active = st.state == SVC_AUDIO_PLAYING || st.state == SVC_AUDIO_PAUSED;
  out->playing = st.state == SVC_AUDIO_PLAYING;
  __builtin_memcpy(out->title, st.title, sizeof out->title);
  out->title[sizeof out->title - 1] = 0;
  out->pos_ms = st.pos_ms;
  out->dur_ms = st.dur_ms;
  out->volume = st.volume;
}

static void shadeToggle() {
  svc_audio_status_t st;
  svc_audio_get_status(&st);
  if (st.state == SVC_AUDIO_PLAYING) svc_audio_pause();
  else if (st.state == SVC_AUDIO_PAUSED) svc_audio_resume();
}

// The playlist lives in the Music app's background service, so prev / next go over the bus.
static void shadeMedia(uint32_t arg) {
  Msg m{};
  m.topic = Topic::Media;
  m.arg = arg;
  bus().publish(m);
}
static void shadePrev() { shadeMedia(1); }
static void shadeNext() { shadeMedia(2); }
static void shadeSetVolume(uint8_t pct) { s_audio.setVolume(pct); }

Audio* audioService() { return s_started ? &s_audio : nullptr; }

#ifdef CONFIG_SVC_AUDIO_BOOT_BEEP
static void bootBeepTask(void*) {
  vTaskDelay(pdMS_TO_TICKS(4000));
  ESP_LOGW("audio", "boot beep: playing the 440 Hz test tone");
  svc_audio_play("@tone");
  vTaskDelete(nullptr);
}
#endif

void audioStart() {
  uint32_t v;
  if (!systemKv().getU32("volume", v) || v > 100) v = kDefaultVolume;
  s_volume = (uint8_t)v;
  svc_audio_init(onAudioEvent, (uint8_t)v);        // cheap: heavy parts start at the first play
  const esp_timer_create_args_t args = {.callback = onPersist, .name = "vol_save"};
  esp_timer_create(&args, &s_persist_timer);
  s_started = true;
  static const os_ui_media_cfg_t kShadeMedia = {shadeGet, shadePrev, shadeToggle, shadeNext, shadeSetVolume};
  os_ui_set_media_provider(&kShadeMedia);
#ifdef CONFIG_SVC_AUDIO_BOOT_BEEP
  xTaskCreate(bootBeepTask, "beep", 3072, nullptr, 3, nullptr);
#endif
}

}  // namespace os::core
