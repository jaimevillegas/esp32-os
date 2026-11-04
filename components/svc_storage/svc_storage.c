#include "svc_storage.h"

#include <string.h>

#include "driver/sdmmc_host.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdmmc_cmd.h"

#define MOUNT_POINT "/sdcard"

// Board pins (no card-detect, no power switch).
#define PIN_CLK 5
#define PIN_CMD 4
#define PIN_D0 6
#define PIN_D1 7
#define PIN_D2 2
#define PIN_D3 3

#define PROBE_NOCARD_MS 1500    // poll period while no card answers
#define PROBE_BADFS_MS 8000     // card answers but cannot be mounted (unformatted / corrupt)
#define CHECK_MS 1000           // status poll while mounted
#define RECHECK_MS 250          // confirmation poll after a failed status

static const char *TAG = "storage";

static svc_storage_cb_t s_cb;
static TaskHandle_t s_task;
static sdmmc_card_t *s_card;             // task-private
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static volatile bool s_mounted;
static uint64_t s_total, s_free;         // guarded by s_lock

typedef enum { FAIL_NONE, FAIL_NOCARD, FAIL_BADFS } fail_kind_t;

static void quiet_driver_logs(void) {
  // The IDF drivers log errors on every failed probe; we report state changes ourselves.
  static const char *const tags[] = {"sdmmc_common", "sdmmc_init", "sdmmc_cmd",   "sdmmc_sd",
                                     "sdmmc_req",    "sdmmc_io",   "sdmmc_mmc",   "vfs_fat_sdmmc",
                                     "diskio_sdmmc", "sdmmc_periph", "sdmmc_host", "vfs_fat"};
  for (size_t i = 0; i < sizeof tags / sizeof tags[0]; i++) esp_log_level_set(tags[i], ESP_LOG_NONE);
}

static esp_err_t do_mount(sdmmc_card_t **out) {
  sdmmc_host_t host = SDMMC_HOST_DEFAULT();
  host.max_freq_khz = SDMMC_FREQ_DEFAULT;                  // 20 MHz: robust with unknown wiring
  sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
  slot.width = 4;
  slot.clk = PIN_CLK;
  slot.cmd = PIN_CMD;
  slot.d0 = PIN_D0;
  slot.d1 = PIN_D1;
  slot.d2 = PIN_D2;
  slot.d3 = PIN_D3;
  slot.cd = SDMMC_SLOT_NO_CD;
  slot.wp = SDMMC_SLOT_NO_WP;
  slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

  esp_vfs_fat_mount_config_t mc = {
      .format_if_mount_failed = false,       // never format a user's card
      .max_files = 4,
      .allocation_unit_size = 0,
      .disk_status_check_enable = false,
      .use_one_fat = false,
  };
  return esp_vfs_fat_sdmmc_mount(MOUNT_POINT, &host, &slot, &mc, out);
}

static void set_state(bool mounted, uint64_t total, uint64_t free_b) {
  taskENTER_CRITICAL(&s_lock);
  s_mounted = mounted;
  s_total = total;
  s_free = free_b;
  taskEXIT_CRITICAL(&s_lock);
  if (s_cb) s_cb(mounted);
}

static void on_mounted(void) {
  uint64_t total = (uint64_t)s_card->csd.capacity * s_card->csd.sector_size;
  uint64_t free_b = 0;
  uint64_t vt = 0, vf = 0;
  // FatFs scans the FAT once to count free clusters (cached afterwards).
  if (esp_vfs_fat_info(MOUNT_POINT, &vt, &vf) == ESP_OK) {
    if (vt) total = vt;
    free_b = vf;
  }
  ESP_LOGI(TAG, "card mounted: %s, %llu MB, %llu MB free, int free %u B, stack hw %u B",
           s_card->cid.name, (unsigned long long)(total >> 20), (unsigned long long)(free_b >> 20),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)uxTaskGetStackHighWaterMark(NULL));
  set_state(true, total, free_b);
}

static void on_removed(void) {
  ESP_LOGW(TAG, "card removed, unmounting");
  set_state(false, 0, 0);                                  // tell the system first
  esp_vfs_fat_sdcard_unmount(MOUNT_POINT, s_card);
  s_card = NULL;
}

static void storage_task(void *arg) {
  (void)arg;
  fail_kind_t last_fail = FAIL_NONE;
  bool quiet = false;                                       // suppress the next "no card" line

  for (;;) {
    if (!s_mounted) {
      esp_err_t err = do_mount(&s_card);
      if (err == ESP_OK) {
        last_fail = FAIL_NONE;
        on_mounted();
        continue;
      }
      s_card = NULL;
      // ESP_FAIL = card initialized but the filesystem could not be mounted.
      fail_kind_t kind = (err == ESP_FAIL) ? FAIL_BADFS : FAIL_NOCARD;
      if (kind != last_fail) {                              // log only on category change
        if (kind == FAIL_BADFS)
          ESP_LOGW(TAG, "card present but cannot be mounted (not FAT?), retrying every %d s",
                   PROBE_BADFS_MS / 1000);
        else if (!quiet)
          ESP_LOGI(TAG, "no card (%s), polling", esp_err_to_name(err));
        last_fail = kind;
      }
      quiet = true;
      vTaskDelay(pdMS_TO_TICKS(kind == FAIL_BADFS ? PROBE_BADFS_MS : PROBE_NOCARD_MS));
    } else {
      vTaskDelay(pdMS_TO_TICKS(CHECK_MS));
      if (sdmmc_get_status(s_card) == ESP_OK) continue;
      vTaskDelay(pdMS_TO_TICKS(RECHECK_MS));                // filter a transient error
      if (sdmmc_get_status(s_card) == ESP_OK) continue;
      on_removed();
      last_fail = FAIL_NOCARD;                              // the removal line already said it
    }
  }
}

void svc_storage_start(svc_storage_cb_t cb) {
  if (s_task) return;
  s_cb = cb;
  quiet_driver_logs();
  xTaskCreatePinnedToCore(storage_task, "storage", 4608, NULL, 2, &s_task, 0);
}

bool svc_storage_mounted(void) { return s_mounted; }

const char *svc_storage_mount_point(void) { return MOUNT_POINT; }

bool svc_storage_capacity(uint64_t *total_bytes, uint64_t *free_bytes) {
  bool ok;
  taskENTER_CRITICAL(&s_lock);
  ok = s_mounted;
  if (total_bytes) *total_bytes = s_total;
  if (free_bytes) *free_bytes = s_free;
  taskEXIT_CRITICAL(&s_lock);
  return ok;
}
