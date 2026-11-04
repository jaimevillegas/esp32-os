#pragma once
// Background part of the Music app: owns the playlist and the "what plays next" logic, so the
// queue keeps advancing while the UI is closed. The UI reads and controls it through this class.
#include <dirent.h>

#include <cstddef>
#include <cstdint>
#include <mutex>

#include "os/app.h"

namespace apps::music {

constexpr size_t kMaxTracks = 256;
constexpr size_t kPoolBytes = 32 * 1024;      // file names of the playlist
constexpr size_t kMaxPath = 200;

enum class Repeat : uint8_t { Off, All, One };

struct Snapshot {
  bool sd;                 // a card is mounted
  bool scanning;           // the playlist is being built (count is 0 meanwhile)
  size_t count;
  int current;             // playlist index of the current track, -1 = none
  bool shuffle;
  Repeat repeat;
  uint32_t version;        // bumps whenever the playlist or the current track changes
};

class MusicService final : public os::Service {
 public:
  void onCreate(os::Context& ctx) override;
  void onEvent(const os::Msg& m) override;
  uint32_t onTick(uint32_t now_ms) override;

  // ---- UI side (any task; short critical sections, never blocks on I/O) ----
  Snapshot snapshot();
  void displayName(size_t index, char* out, size_t cap);   // file name without extension
  bool playIndex(size_t index);
  void togglePlay();       // play / pause / resume depending on the engine state
  void next();
  void prev();
  void stop();
  void toggleShuffle();
  void cycleRepeat();

 private:
  struct Entry {
    uint32_t path_off;     // path relative to the mount point, in `pool_`
    uint16_t name_off;     // offset of the file name inside that path
  };
  struct PendingDir {
    char path[kMaxPath];
    uint8_t max_depth;     // how many more levels may be entered below this directory
  };
  static constexpr size_t kMaxDirs = 24;

  // Unlocked helpers (caller holds mu_).
  void startScan();
  void scanSlice(uint32_t budget_ms);
  void finishScan();
  void clearList();
  void buildOrder(bool keep_current);
  bool startPos(int pos);
  void advanceAfterEnd();
  void checkEngine();
  bool addTrack(const char* dir_rel, const char* file);
  const char* pathOf(const Entry& e) const { return pool_ + e.path_off; }
  uint32_t rnd();

  os::Context* ctx_ = nullptr;
  std::mutex mu_;

  char* pool_ = nullptr;
  size_t pool_used_ = 0;
  Entry* entries_ = nullptr;
  uint16_t* order_ = nullptr;
  size_t count_ = 0;
  int pos_ = -1;                 // position in order_
  bool shuffle_ = false;
  Repeat repeat_ = Repeat::Off;
  bool expecting_ = false;       // a track started by us is (supposed to be) playing
  uint8_t errors_ = 0;           // consecutive failed tracks while auto-advancing
  uint32_t version_ = 1;
  uint32_t rng_ = 2463534242u;

  bool sd_ = false;
  bool scanning_ = false;
  PendingDir queue_[kMaxDirs];
  size_t q_head_ = 0, q_tail_ = 0;
  DIR* dir_ = nullptr;
  char dir_path_[kMaxPath] = {};
  uint8_t dir_depth_ = 0;
};

// Valid after the service was created (null before).
MusicService* service();

}  // namespace apps::music
