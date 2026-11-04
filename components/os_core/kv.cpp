#include <cstring>

#include "core_internal.h"
#include "nvs.h"

namespace os::core {

void NvsKv::configure(const char* partition, const char* ns) {
  strlcpy(partition_, partition, sizeof partition_);
  strlcpy(ns_, ns, sizeof ns_);        // NVS namespaces are limited to 15 characters
}

bool NvsKv::open(bool write) {
  if (opened_ && (opened_rw_ || !write)) return true;
  if (opened_) {
    nvs_close(handle_);
    opened_ = false;
  }
  nvs_handle_t h;
  esp_err_t e = nvs_open_from_partition(partition_, ns_, write ? NVS_READWRITE : NVS_READONLY, &h);
  if (e != ESP_OK) return false;       // read-only open of a namespace that does not exist yet
  handle_ = h;
  opened_ = true;
  opened_rw_ = write;
  return true;
}

bool NvsKv::getU32(const char* key, uint32_t& out) {
  return open(false) && nvs_get_u32(handle_, key, &out) == ESP_OK;
}

bool NvsKv::setU32(const char* key, uint32_t value) {
  return open(true) && nvs_set_u32(handle_, key, value) == ESP_OK && nvs_commit(handle_) == ESP_OK;
}

bool NvsKv::getStr(const char* key, char* buf, size_t cap) {
  size_t len = cap;
  return open(false) && nvs_get_str(handle_, key, buf, &len) == ESP_OK;
}

bool NvsKv::setStr(const char* key, const char* value) {
  return open(true) && nvs_set_str(handle_, key, value) == ESP_OK && nvs_commit(handle_) == ESP_OK;
}

}  // namespace os::core
