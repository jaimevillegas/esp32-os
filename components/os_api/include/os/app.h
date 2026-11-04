#pragma once
// Public API seen by apps. Apps include ONLY this header (and LVGL) - never esp_*.h - so the
// same code can later be built in the SDL simulator.

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <new>

#include "lvgl.h"

namespace os {

enum Perm : uint32_t { NET = 1, AUDIO = 2, SD = 4, BLE = 8, NVS = 16, LONG_TASK = 32 };

// Topic payloads (Msg.arg):
//   Power   : bits 0-7 percent, bit 8 usb, bit 9 charging
//   Storage : 1 = microSD mounted, 0 = removed / unmounted
//   Audio   : bits 0-7 = PlayState, bits 8-15 = volume percent
enum class Topic : uint16_t { None = 0, Power, Network, Snapshot, AppState, Storage, Audio, User = 0x100 };

struct Msg {                 // 16 bytes, copied by value
  Topic    topic;
  uint16_t app;              // destination app id, 0 = broadcast
  uint32_t version;          // version of the datum in the store
  uint32_t arg;
  uint32_t ts_ms;
};

struct SysInfo {
  uint32_t int_free, int_min, int_largest;     // internal SRAM
  uint32_t psram_free, psram_min, psram_largest;
  uint32_t uptime_s;
  uint32_t ui_stack_free;                      // bytes, high-water mark of the `ui` task
  // --- added for the diagnostics app; every field has an "unavailable" value ---
  uint32_t int_total, psram_total;             // heap capacity in bytes (0 = unknown)
  uint32_t dma_free, dma_largest, dma_total;   // internal DMA-capable memory
  uint32_t app_size;                           // running firmware image bytes (0 = unknown)
  uint32_t app_slot_size;                      // OTA slot size of the running image (0 = unknown)
  uint32_t flash_size;                         // flash chip size (0 = unknown)
  uint16_t cpu_mhz;                            // current CPU frequency (0 = unknown)
  uint16_t n_tasks;                            // FreeRTOS tasks
  int16_t  temp_c10;                           // chip temperature in 0.1 C; INT16_MIN = unavailable
  int8_t   cpu_load[2];                        // % busy per core; -1 = unavailable
};

struct PowerInfo {
  uint16_t vbat_mv;
  uint8_t  percent;
  bool     usb;              // connected to a host computer
  bool     charging;         // inferred
};

// Persistent key-value store, one namespace per app (NVS).
class Kv {
 public:
  virtual ~Kv() = default;
  virtual bool getU32(const char* key, uint32_t& out) = 0;
  virtual bool setU32(const char* key, uint32_t value) = 0;
  virtual bool getStr(const char* key, char* buf, size_t cap) = 0;
  virtual bool setStr(const char* key, const char* value) = 0;
};

// Event bus: copies a Msg to every subscriber queue. Safe from any task.
class Bus {
 public:
  virtual ~Bus() = default;
  virtual bool publish(const Msg& m) = 0;
};

// Device facilities that do not need a permission.
class Device {
 public:
  virtual ~Device() = default;
  virtual uint8_t brightness() const = 0;
  virtual void setBrightness(uint8_t percent, bool persist) = 0;
  virtual void sysInfo(SysInfo& out) const = 0;
  virtual PowerInfo power() const = 0;
  virtual uint32_t nowMs() const = 0;
  virtual void requestClose() = 0;             // closes the calling app (deferred)
  virtual void sleep() = 0;                    // light sleep until the BOOT button is pressed (deferred)
};

// microSD card (Perm::SD). While mounted it is a normal VFS volume at mountPoint(): apps use
// plain stdio/POSIX calls (fopen, opendir, stat ...) on paths under it. Never cache open
// handles across a Topic::Storage "removed" event.
class Fs {
 public:
  virtual ~Fs() = default;
  virtual bool mounted() const = 0;
  virtual const char* mountPoint() const = 0;                   // e.g. "/sdcard"
  virtual bool capacity(uint64_t& total_bytes, uint64_t& free_bytes) = 0;
};

enum class PlayState : uint8_t { Stopped = 0, Playing, Paused, Error };

struct AudioStatus {
  PlayState state;
  uint32_t  pos_ms;
  uint32_t  dur_ms;          // 0 when unknown
  uint8_t   volume;          // 0..100
  char      title[64];       // file name or tag title
};

// Audio output (Perm::AUDIO). One stream at a time; the engine runs in its own task and
// publishes Topic::Audio on state changes (track end included).
class Audio {
 public:
  virtual ~Audio() = default;
  virtual bool play(const char* path) = 0;      // starts (or restarts) playback of a file
  virtual void pause() = 0;
  virtual void resume() = 0;
  virtual void stop() = 0;
  virtual bool seek(uint32_t ms) = 0;
  virtual void setVolume(uint8_t percent) = 0;  // persisted by the engine
  virtual void status(AudioStatus& out) const = 0;
};

class Net;    // not implemented yet; ctx.net() returns nullptr

class Context {              // injected; services are null when the permission is missing
 public:
  Context(Bus& bus, Kv& kv, Device& dev, std::pmr::memory_resource& mem, uint32_t perms,
          Fs* fs = nullptr, Audio* audio = nullptr)
      : bus_(bus), kv_(kv), dev_(dev), mem_(mem), perms_(perms), fs_(fs), audio_(audio) {}
  Bus& bus() { return bus_; }
  Kv& kv() { return kv_; }
  Device& device() { return dev_; }
  std::pmr::memory_resource& mem() { return mem_; }     // the app's PSRAM arena
  Net* net() { return nullptr; }
  Audio* audio() { return has(AUDIO) ? audio_ : nullptr; }
  Fs* sd() { return has(SD) ? fs_ : nullptr; }
  bool has(Perm p) const { return perms_ & p; }

 private:
  Bus& bus_;
  Kv& kv_;
  Device& dev_;
  std::pmr::memory_resource& mem_;
  uint32_t perms_;
  Fs* fs_;
  Audio* audio_;
};

class App {
 public:
  virtual ~App() = default;
  virtual void onCreate(Context&) {}          // once; reserve data
  virtual void onStart(lv_obj_t* root) = 0;   // build the UI under root (ui task)
  virtual void onResume() {}                  // visible and interactive
  virtual void onEvent(const Msg&) {}         // ui task, foreground only
  virtual void onPause() {}
  virtual void onStop() {}                    // the system deletes root afterwards
  virtual void onDestroy() {}                 // free everything; the arena is discarded
};

class Service {              // background part, runs in the `svc` task; keep handlers < 20 ms
 public:
  virtual ~Service() = default;
  virtual void onCreate(Context&) {}
  virtual void onEvent(const Msg&) = 0;
  virtual uint32_t onTick(uint32_t now_ms) { return UINT32_MAX; }   // ms until the next tick
};

struct Manifest {
  const char* id;            // "settings" (also the NVS namespace suffix, <= 12 chars)
  const char* name;          // launcher text
  const void* icon;          // lv_image_dsc_t in flash (optional)
  const char* glyph;         // LV_SYMBOL_* fallback used when icon is null
  uint32_t    perms;
  uint32_t    psram_arena;   // bytes reserved while the app (or its service) lives
  uint32_t    sram_budget;   // internal SRAM cap (0 = none); checked in the diag build
  bool        background;    // has a Service
  App*      (*make)(std::pmr::memory_resource&);
  Service*  (*make_service)(std::pmr::memory_resource&);   // nullptr when !background
};

// Constructs T inside the app arena.
template <class T>
App* makeApp(std::pmr::memory_resource& m) {
  void* p = m.allocate(sizeof(T), alignof(T));
  return new (p) T();
}
template <class T>
Service* makeService(std::pmr::memory_resource& m) {
  void* p = m.allocate(sizeof(T), alignof(T));
  return new (p) T();
}

// printf-style log line (INFO), tagged with the app id.
void logf(const char* tag, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

}  // namespace os
