#include "core_internal.h"

#include <cstdio>
#include <cstdlib>

#include "esp_heap_caps.h"
#include "esp_log.h"

namespace os::core {

static const char* TAG = "arena";

bool Arena::init(size_t bytes) {
  if (block_) return true;
  block_ = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
  if (!block_) return false;
  heap_ = multi_heap_register(block_, bytes);
  mu_ = xSemaphoreCreateMutex();
  cap_ = bytes;
  return heap_ && mu_;
}

void Arena::deinit() {
  if (mu_) vSemaphoreDelete(mu_);
  heap_caps_free(block_);        // the multi_heap metadata lives inside the block
  block_ = nullptr;
  heap_ = nullptr;
  mu_ = nullptr;
  cap_ = 0;
}

size_t Arena::used() const {
  if (!heap_) return 0;
  multi_heap_info_t info;
  multi_heap_get_info(heap_, &info);
  return info.total_allocated_bytes;
}

void* Arena::do_allocate(size_t n, size_t align) {
  xSemaphoreTake(mu_, portMAX_DELAY);
  void* p = multi_heap_aligned_alloc(heap_, n, align < 4 ? 4 : align);
  xSemaphoreGive(mu_);
  if (!p) {
    // Exceptions are disabled; an exhausted arena is a bug of the app: fail loudly.
    ESP_LOGE(TAG, "arena exhausted (%u of %u bytes used, asked %u)", (unsigned)used(),
             (unsigned)cap_, (unsigned)n);
    abort();
  }
  return p;
}

void Arena::do_deallocate(void* p, size_t, size_t) {
  xSemaphoreTake(mu_, portMAX_DELAY);
  multi_heap_free(heap_, p);
  xSemaphoreGive(mu_);
}

}  // namespace os::core
