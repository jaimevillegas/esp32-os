// Glue between the voice recorder (svc_audio/svc_rec) and the framework (os::Recorder for apps,
// Topic::Recorder events).
//
// Topic::Recorder payload: bit 0 = recording, bit 1 = error. Published on every change: start,
// end of a recording (normal, stopped by the user or failed) and start failures.
#include <cstring>

#include "core_internal.h"
#include "svc_rec.h"

namespace os::core {

namespace {

constexpr uint64_t kMinFreeBytes = 512 * 1024;

void onRecEvent(bool recording, bool error) {
  Msg m{};
  m.topic = Topic::Recorder;
  m.arg = (recording ? 1u : 0u) | (error ? 2u : 0u);
  bus().publish(m);
}

class RecorderImpl final : public Recorder {
 public:
  bool start(const char* dir) override {
    Fs* fs = fsService();
    if (!fs || !fs->mounted()) {
      onRecEvent(false, true);
      return false;
    }
    uint64_t total = 0, free_b = 0;
    if (fs->capacity(total, free_b) && free_b < kMinFreeBytes) {
      onRecEvent(false, true);
      return false;
    }
    char def[64];
    if (!dir || !*dir) {
      snprintf(def, sizeof def, "%s/Recordings", fs->mountPoint());
      dir = def;
    }
    char path[96];
    return svc_rec_start(dir, path, sizeof path);     // publishes its own events
  }

  void stop() override { svc_rec_stop(); }

  void status(RecStatus& out) const override {
    svc_rec_status_t st;
    svc_rec_get_status(&st);
    out.recording = st.recording;
    out.error = st.error;
    out.elapsed_ms = st.elapsed_ms;
    out.bytes = st.bytes;
    out.level = st.level;
    std::memcpy(out.path, st.path, sizeof out.path);
    out.path[sizeof out.path - 1] = 0;
  }
};

RecorderImpl s_rec;
bool s_started;

}  // namespace

Recorder* recorderService() { return s_started ? &s_rec : nullptr; }

void recorderStart() {
  svc_rec_init(onRecEvent);                           // cheap: everything else starts on demand
  s_started = true;
}

}  // namespace os::core
