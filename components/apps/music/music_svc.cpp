#include "music_svc.h"

#include <strings.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <new>
#include <utility>

namespace apps::music {

static MusicService* g_service = nullptr;
MusicService* service() { return g_service; }

static bool hasMusicExt(const char* name) {
  const char* dot = strrchr(name, '.');
  return dot && (strcasecmp(dot, ".mp3") == 0 || strcasecmp(dot, ".wav") == 0);
}

// ---- lifecycle --------------------------------------------------------------------------------

void MusicService::onCreate(os::Context& ctx) {
  ctx_ = &ctx;
  auto& mem = ctx.mem();
  pool_ = static_cast<char*>(mem.allocate(kPoolBytes, 4));
  entries_ = static_cast<Entry*>(mem.allocate(kMaxTracks * sizeof(Entry), alignof(Entry)));
  order_ = static_cast<uint16_t*>(mem.allocate(kMaxTracks * sizeof(uint16_t), alignof(uint16_t)));
  g_service = this;
  rng_ ^= ctx.device().nowMs() * 2654435761u;
  if (ctx.sd() && ctx.sd()->mounted()) {
    std::lock_guard<std::mutex> lk(mu_);
    startScan();
  }
}

uint32_t MusicService::rnd() {
  rng_ ^= rng_ << 13;
  rng_ ^= rng_ >> 17;
  rng_ ^= rng_ << 5;
  return rng_;
}

// ---- scanning ---------------------------------------------------------------------------------

void MusicService::clearList() {
  count_ = 0;
  pool_used_ = 0;
  pos_ = -1;
  version_++;
}

void MusicService::startScan() {
  if (dir_) { closedir(dir_); dir_ = nullptr; }
  clearList();
  q_head_ = q_tail_ = 0;
  os::Fs* fs = ctx_->sd();
  sd_ = fs && fs->mounted();
  scanning_ = false;
  if (!sd_) return;
  const char* mp = fs->mountPoint();
  // Root: files plus one level of folders. Music/: files plus two levels.
  snprintf(queue_[q_tail_].path, kMaxPath, "%s", mp);
  queue_[q_tail_++].max_depth = 1;
  snprintf(queue_[q_tail_].path, kMaxPath, "%s/Music", mp);
  queue_[q_tail_++].max_depth = 2;
  scanning_ = true;
}

bool MusicService::addTrack(const char* dir_path, const char* file) {
  if (count_ >= kMaxTracks) return false;
  const size_t mp_len = strlen(ctx_->sd()->mountPoint());
  const char* rel_dir = dir_path + mp_len;
  while (*rel_dir == '/') rel_dir++;
  char rel[kMaxPath + 64];
  const int n = snprintf(rel, sizeof rel, "%s%s%s", rel_dir, *rel_dir ? "/" : "", file);
  if (n <= 0 || (size_t)n >= sizeof rel || (size_t)n >= kMaxPath) return false;
  if (pool_used_ + (size_t)n + 1 > kPoolBytes) return false;
  Entry& e = entries_[count_];
  e.path_off = (uint32_t)pool_used_;
  e.name_off = (uint16_t)(n - (int)strlen(file));
  memcpy(pool_ + pool_used_, rel, (size_t)n + 1);
  pool_used_ += (size_t)n + 1;
  count_++;
  return true;
}

void MusicService::scanSlice(uint32_t budget_ms) {
  const uint32_t t0 = ctx_->device().nowMs();
  while (scanning_) {
    if (!dir_) {
      if (q_head_ >= q_tail_) { finishScan(); return; }
      PendingDir& pd = queue_[q_head_++];
      dir_ = opendir(pd.path);
      if (!dir_) continue;
      snprintf(dir_path_, kMaxPath, "%s", pd.path);
      dir_depth_ = pd.max_depth;
    }
    const struct dirent* de = readdir(dir_);
    if (!de) { closedir(dir_); dir_ = nullptr; }
    else if (de->d_name[0] != '.') {
      if (de->d_type == DT_DIR) {
        const bool is_root = strcmp(dir_path_, ctx_->sd()->mountPoint()) == 0;
        const bool skip = (is_root && strcasecmp(de->d_name, "Music") == 0) ||
                          strcasecmp(de->d_name, "System Volume Information") == 0;
        if (!skip && dir_depth_ > 0 && q_tail_ < kMaxDirs) {
          PendingDir& pd = queue_[q_tail_];
          const int n = snprintf(pd.path, kMaxPath, "%s/%s", dir_path_, de->d_name);
          if (n > 0 && (size_t)n < kMaxPath) {
            pd.max_depth = (uint8_t)(dir_depth_ - 1);
            q_tail_++;
          }
        }
      } else if (hasMusicExt(de->d_name)) {
        addTrack(dir_path_, de->d_name);
      }
    }
    if (ctx_->device().nowMs() - t0 >= budget_ms) return;
  }
}

void MusicService::finishScan() {
  scanning_ = false;
  std::sort(entries_, entries_ + count_, [this](const Entry& a, const Entry& b) {
    return strcasecmp(pool_ + a.path_off, pool_ + b.path_off) < 0;
  });
  buildOrder(false);
  pos_ = -1;
  version_++;
  os::logf("music", "scan done: %u tracks", (unsigned)count_);
}

// ---- queue ------------------------------------------------------------------------------------

void MusicService::buildOrder(bool keep_current) {
  const int cur = (pos_ >= 0 && (size_t)pos_ < count_) ? order_[pos_] : -1;
  for (size_t i = 0; i < count_; i++) order_[i] = (uint16_t)i;
  if (!shuffle_) {
    pos_ = cur;
    return;
  }
  size_t start = 0;
  if (keep_current && cur >= 0) {
    std::swap(order_[0], order_[cur]);
    start = 1;
  }
  for (size_t i = count_; i > start + 1; i--) {
    const size_t j = start + rnd() % (i - start);
    std::swap(order_[i - 1], order_[j]);
  }
  pos_ = (keep_current && cur >= 0) ? 0 : -1;
}

bool MusicService::startPos(int pos) {
  os::Audio* au = ctx_->audio();
  if (!au || pos < 0 || (size_t)pos >= count_) return false;
  char full[kMaxPath + 16];
  snprintf(full, sizeof full, "%s/%s", ctx_->sd()->mountPoint(), pathOf(entries_[order_[pos]]));
  pos_ = pos;
  version_++;
  expecting_ = au->play(full);
  return expecting_;
}

void MusicService::advanceAfterEnd() {
  if (count_ == 0) { expecting_ = false; return; }
  if (repeat_ == Repeat::One && errors_ == 0) { startPos(pos_); return; }
  int p = pos_ + 1;
  if ((size_t)p >= count_) {
    if (repeat_ == Repeat::Off) { expecting_ = false; return; }
    if (shuffle_) buildOrder(false);
    p = 0;
  }
  startPos(p);
}

// Looks at the real engine state; used for both bus events and the polling fallback, so a
// dropped or duplicated event can neither stall nor double-advance the queue.
void MusicService::checkEngine() {
  os::Audio* au = ctx_->audio();
  if (!au || !expecting_) return;
  os::AudioStatus st;
  au->status(st);
  if (st.state == os::PlayState::Stopped) {
    errors_ = 0;
    advanceAfterEnd();
  } else if (st.state == os::PlayState::Error) {
    if (++errors_ < 3 && count_ > 1) advanceAfterEnd();
    else expecting_ = false;
  }
}

void MusicService::onEvent(const os::Msg& m) {
  if (m.topic == os::Topic::Media) {          // transport request from the shade (1 = prev, 2 = next)
    if (m.arg == 1) prev();                    // both ignore an empty playlist and take the lock
    else if (m.arg == 2) next();
    return;
  }
  std::lock_guard<std::mutex> lk(mu_);
  if (m.topic == os::Topic::Audio) {
    version_++;
    checkEngine();
  } else if (m.topic == os::Topic::Storage) {
    if (m.arg == 0) {
      expecting_ = false;
      if (os::Audio* au = ctx_->audio()) au->stop();
      if (dir_) { closedir(dir_); dir_ = nullptr; }
      scanning_ = false;
      sd_ = false;
      clearList();
    } else {
      startScan();
    }
  }
}

uint32_t MusicService::onTick(uint32_t) {
  std::lock_guard<std::mutex> lk(mu_);
  if (scanning_) {
    scanSlice(8);
    return 5;
  }
  checkEngine();
  return 250;
}

// ---- UI side ----------------------------------------------------------------------------------

Snapshot MusicService::snapshot() {
  std::lock_guard<std::mutex> lk(mu_);
  Snapshot s{};
  s.sd = sd_;
  s.scanning = scanning_;
  s.count = scanning_ ? 0 : count_;
  s.current = (pos_ >= 0 && (size_t)pos_ < count_ && !scanning_) ? order_[pos_] : -1;
  s.shuffle = shuffle_;
  s.repeat = repeat_;
  s.version = version_;
  return s;
}

void MusicService::displayName(size_t index, char* out, size_t cap) {
  out[0] = 0;
  std::lock_guard<std::mutex> lk(mu_);
  if (index >= count_ || cap == 0) return;
  const Entry& e = entries_[index];
  snprintf(out, cap, "%s", pathOf(e) + e.name_off);
  char* dot = strrchr(out, '.');
  if (dot && dot != out) *dot = 0;
}

bool MusicService::playIndex(size_t index) {
  std::lock_guard<std::mutex> lk(mu_);
  if (scanning_ || index >= count_) return false;
  errors_ = 0;
  int pos = -1;
  for (size_t i = 0; i < count_; i++) {
    if (order_[i] == index) { pos = (int)i; break; }
  }
  return startPos(pos);
}

void MusicService::togglePlay() {
  std::lock_guard<std::mutex> lk(mu_);
  os::Audio* au = ctx_->audio();
  if (!au || scanning_) return;
  os::AudioStatus st;
  au->status(st);
  if (st.state == os::PlayState::Playing) {
    au->pause();
  } else if (st.state == os::PlayState::Paused) {
    au->resume();
  } else if (count_ > 0) {
    errors_ = 0;
    startPos(pos_ >= 0 ? pos_ : 0);
  }
}

void MusicService::next() {
  std::lock_guard<std::mutex> lk(mu_);
  if (scanning_ || count_ == 0) return;
  errors_ = 0;
  startPos(pos_ + 1 >= (int)count_ ? 0 : pos_ + 1);
}

void MusicService::prev() {
  std::lock_guard<std::mutex> lk(mu_);
  os::Audio* au = ctx_->audio();
  if (scanning_ || count_ == 0 || !au) return;
  errors_ = 0;
  os::AudioStatus st;
  au->status(st);
  const bool active = st.state == os::PlayState::Playing || st.state == os::PlayState::Paused;
  if (active && st.pos_ms > 3000 && pos_ >= 0) {
    startPos(pos_);                                  // restart the current track
    return;
  }
  startPos(pos_ <= 0 ? (int)count_ - 1 : pos_ - 1);
}

void MusicService::stop() {
  std::lock_guard<std::mutex> lk(mu_);
  expecting_ = false;
  if (os::Audio* au = ctx_->audio()) au->stop();
  version_++;
}

void MusicService::toggleShuffle() {
  std::lock_guard<std::mutex> lk(mu_);
  shuffle_ = !shuffle_;
  buildOrder(true);
  version_++;
}

void MusicService::cycleRepeat() {
  std::lock_guard<std::mutex> lk(mu_);
  repeat_ = repeat_ == Repeat::Off ? Repeat::All : repeat_ == Repeat::All ? Repeat::One : Repeat::Off;
  version_++;
}

}  // namespace apps::music
