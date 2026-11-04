// Glue between svc_storage and the framework (Fs for apps, Topic::Storage events).
#include "core_internal.h"
#include "os_ui.h"
#include "svc_storage.h"

namespace os::core {

class SdFs final : public Fs {
 public:
  bool mounted() const override { return svc_storage_mounted(); }
  const char* mountPoint() const override { return svc_storage_mount_point(); }
  bool capacity(uint64_t& total_bytes, uint64_t& free_bytes) override {
    return svc_storage_capacity(&total_bytes, &free_bytes);
  }
};

static SdFs s_fs;

// Runs in the storage task.
static void onStorageChange(bool mounted) {
  os_ui_set_sd_present(mounted);
  Msg m{};
  m.topic = Topic::Storage;
  m.app = 0;
  m.arg = mounted ? 1 : 0;
  bus().publish(m);
}

Fs* fsService() { return &s_fs; }
void storageStart() { svc_storage_start(onStorageChange); }

}  // namespace os::core
