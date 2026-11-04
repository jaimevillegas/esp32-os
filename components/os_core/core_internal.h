#pragma once
// Private to os_core.

#include <cstddef>
#include <cstdint>
#include <memory_resource>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "multi_heap.h"
#include "os/app.h"

namespace os::core {

// Memory arena in PSRAM. Everything an app allocates through ctx.mem() lives here and is
// released by discarding the whole arena.
class Arena final : public std::pmr::memory_resource {
 public:
  bool init(size_t bytes);
  void deinit();
  bool live() const { return block_ != nullptr; }
  size_t used() const;
  size_t capacity() const { return cap_; }

 private:
  void* do_allocate(size_t n, size_t align) override;
  void do_deallocate(void* p, size_t, size_t) override;
  bool do_is_equal(const std::pmr::memory_resource& o) const noexcept override { return this == &o; }

  void* block_ = nullptr;
  size_t cap_ = 0;
  multi_heap_handle_t heap_ = nullptr;
  SemaphoreHandle_t mu_ = nullptr;
};

// NVS-backed Kv. Opened lazily; `partition` "nvs" for system settings, "nvs_app" for apps.
class NvsKv final : public Kv {
 public:
  void configure(const char* partition, const char* ns);
  bool getU32(const char* key, uint32_t& out) override;
  bool setU32(const char* key, uint32_t value) override;
  bool getStr(const char* key, char* buf, size_t cap) override;
  bool setStr(const char* key, const char* value) override;

 private:
  bool open(bool write);
  char partition_[16] = {};
  char ns_[16] = {};
  uint32_t handle_ = 0;
  bool opened_ = false, opened_rw_ = false;
};

// ---- bus ------------------------------------------------------------------------------
void busInit();
Bus& bus();
void busPumpUi();                 // ui task: dispatches queued events to the foreground app
bool busReceiveSvc(Msg& m, uint32_t timeout_ms);

// ---- registry ---------------------------------------------------------------------------
size_t appCount();
const Manifest& appManifest(size_t idx);

// ---- device -----------------------------------------------------------------------------
Device& device();
NvsKv& systemKv();

// ---- app manager ------------------------------------------------------------------------
struct Slot {
  const Manifest* m = nullptr;
  uint16_t id = 0;               // 1-based; 0 means "all" in Msg.app
  Arena arena;
  NvsKv kv;
  App* app = nullptr;
  Service* svc = nullptr;
  Context* ctx = nullptr;
  uint32_t next_tick = 0;
  bool tick_pending = false;
};

void managerInit();
size_t slotCount();
Slot* slotAt(size_t idx);
bool ensureArena(Slot& s);
void launch(size_t idx);
void closeForeground();
void closeForegroundAsync();
Slot* foreground();

// ---- feature glue (one file each, owned by the feature) -----------------------------------
Fs* fsService();                 // storage_glue.cpp: nullptr until the storage service exists
Audio* audioService();           // audio_glue.cpp
void storageStart();             // storage_glue.cpp: called once before the UI starts
void audioStart();               // audio_glue.cpp:   called once before the UI starts
void requestSleep();             // sleep_glue.cpp:   any task; performs the sleep on the ui task

// ---- launcher / services ----------------------------------------------------------------
lv_obj_t* launcherBuild();       // creates the home screen, returns it
lv_obj_t* launcherScreen();
void startServices();            // creates services of background apps and the svc task

}  // namespace os::core
