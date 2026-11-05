// Voice recorder. Two short-lived tasks, both created by svc_rec_start() and gone when the
// recording is finalised:
//
//   rec_cap  (prio 10, 4 KB)  owns the I2S RX channel and the ES8311 ADC. Reads stereo frames,
//                             keeps one channel as 16-bit mono, computes the VU peak and pushes
//                             the PCM into a 256 KB ring in PSRAM (8 s of slack at 16 kHz).
//   rec_wr   (prio 6, 6 KB)   drains the ring to the WAV file in 32 KB blocks, so SD latency
//                             never reaches the capture task. Every kPatchMs it rewrites the
//                             44-byte header with the current sizes and syncs, so a power loss or
//                             card removal still leaves a playable file.
//
// The I2S port is shared with the playback engine: svc_audio_capture_acquire() stops playback
// and removes the TX channel, the recorder then runs an RX-only master (it generates MCLK, BCLK
// and WS itself) and gives the port back at the end.
#include "svc_rec.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "bsp_es3c35p.h"
#include "codec_es8311.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "svc_audio_priv.h"

static const char *TAG = "rec";

#define PIN_MCLK GPIO_NUM_17
#define PIN_BCLK GPIO_NUM_18
#define PIN_WS GPIO_NUM_21
#define PIN_DIN GPIO_NUM_16            // ES8311 ASDOUT -> ESP32

static const uint32_t kRate = SVC_REC_RATE_HZ;
static const size_t kRing = 256 * 1024;              // power of two
static const size_t kBlock = 32 * 1024;
static const size_t kReadFrames = 640;               // 40 ms per I2S read
static const uint32_t kPatchMs = 4000;
static const uint32_t kSettleFrames = kRate / 5;     // first 200 ms are discarded (codec settling)
static const uint32_t kLockFrames = kRate / 2;       // channel choice window
static const uint32_t kPeakFrames = kRate / 10;      // VU window, 100 ms
static const uint32_t kMaxData = 0x7C000000u;        // ~1.9 GB: stop cleanly before the FAT limit
static const unsigned kMaxIndex = 9999;

static SemaphoreHandle_t g_lock;
static svc_rec_event_cb_t g_cb;

static volatile bool g_active;         // a recording (or its finalisation) is in progress
static volatile bool g_stop;           // stop requested
static volatile bool g_err;
static volatile bool g_cap_done;
static volatile uint32_t g_frames;     // frames recorded (after the settling period)
static volatile uint32_t g_bytes;      // file size so far
static volatile uint8_t g_level;
static char g_path[96];

static uint8_t *g_ring;
static uint8_t *g_block;
static int16_t *g_raw;
static int g_fd = -1;
static volatile size_t g_head, g_tail; // free-running byte counters (SPSC)
static uint32_t g_overruns;
static i2s_chan_handle_t g_rx;

static inline uint32_t nowMs() { return (uint32_t)(esp_timer_get_time() / 1000); }

static void fire(bool recording, bool error) {
    if (g_cb) g_cb(recording, error);
}

// ---- WAV --------------------------------------------------------------------------------
static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void wavHeader(uint8_t h[44], uint32_t data) {
    memcpy(h, "RIFF", 4);
    put32(h + 4, 36 + data);
    memcpy(h + 8, "WAVEfmt ", 8);
    put32(h + 16, 16);
    h[20] = 1;                                     // PCM
    h[21] = 0;
    h[22] = 1;                                     // mono
    h[23] = 0;
    put32(h + 24, kRate);
    put32(h + 28, kRate * 2);
    h[32] = 2;                                     // block align
    h[33] = 0;
    h[34] = 16;                                    // bits
    h[35] = 0;
    memcpy(h + 36, "data", 4);
    put32(h + 40, data);
}

static bool writeAll(int fd, const void *buf, size_t len) {
    const uint8_t *p = (const uint8_t *)buf;
    while (len) {
        const ssize_t w = write(fd, p, len);
        if (w <= 0) {
            ESP_LOGW(TAG, "write failed: errno %d", errno);
            return false;
        }
        p += w;
        len -= (size_t)w;
    }
    return true;
}

// Rewrites the header with the current sizes and syncs; leaves the file position at the end.
static bool patchHeader(uint32_t data) {
    uint8_t h[44];
    wavHeader(h, data);
    if (lseek(g_fd, 0, SEEK_SET) != 0) return false;
    if (!writeAll(g_fd, h, sizeof h)) return false;
    if (lseek(g_fd, (off_t)(44 + data), SEEK_SET) < 0) return false;
    return fsync(g_fd) == 0;
}

// ---- capture task -------------------------------------------------------------------------
static bool captureSetup() {
    if (!svc_audio_capture_acquire()) return false;
    i2c_master_bus_handle_t bus = bsp_i2c_get();
    if (!bus) {
        ESP_LOGW(TAG, "I2C bus not available");
        return false;
    }
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    cc.dma_desc_num = 6;
    cc.dma_frame_num = 320;
    if (i2s_new_channel(&cc, nullptr, &g_rx) != ESP_OK) {
        g_rx = nullptr;
        return false;
    }
    i2s_std_config_t sc = {};
    sc.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(kRate);
    sc.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    sc.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
    sc.gpio_cfg.mclk = PIN_MCLK;
    sc.gpio_cfg.bclk = PIN_BCLK;
    sc.gpio_cfg.ws = PIN_WS;
    sc.gpio_cfg.dout = I2S_GPIO_UNUSED;
    sc.gpio_cfg.din = PIN_DIN;
    if (i2s_channel_init_std_mode(g_rx, &sc) != ESP_OK || i2s_channel_enable(g_rx) != ESP_OK) {
        i2s_del_channel(g_rx);
        g_rx = nullptr;
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(10));                 // MCLK must run while the codec starts
    if (codec_es8311_init(bus) != ESP_OK || codec_es8311_enable_capture() != ESP_OK) {
        ESP_LOGW(TAG, "codec setup failed");
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(30));
    ESP_LOGI(TAG, "capture ready at %u Hz", (unsigned)kRate);
    return true;
}

static void captureTask(void *) {
    const bool ok = captureSetup();
    if (!ok) g_err = true;

    uint32_t seen = 0;                             // frames received, including the settling ones
    uint32_t peak = 0, peak_n = 0;
    uint64_t e_l = 0, e_r = 0;
    uint32_t timeouts = 0;
    int16_t mono[kReadFrames];

    while (ok && !g_stop && !g_err) {
        size_t n = 0;
        const esp_err_t e = i2s_channel_read(g_rx, g_raw, kReadFrames * 4, &n, 500);
        if (e == ESP_ERR_TIMEOUT) {
            if (++timeouts > 6) {
                ESP_LOGW(TAG, "no data from the codec");
                g_err = true;
            }
            continue;
        }
        if (e != ESP_OK) {
            ESP_LOGW(TAG, "i2s read: %s", esp_err_to_name(e));
            g_err = true;
            break;
        }
        timeouts = 0;
        const size_t frames = n / 4;
        if (!frames) continue;

        // The ES8311 sends the ADC on the left slot (and possibly the right one); keep the
        // slot with more energy, decided over the first half second.
        if (seen < kLockFrames) {
            for (size_t i = 0; i < frames; i++) {
                e_l += (uint32_t)abs(g_raw[2 * i]);
                e_r += (uint32_t)abs(g_raw[2 * i + 1]);
            }
        }
        const size_t ch = e_r > e_l ? 1 : 0;
        size_t skip = 0;
        if (seen < kSettleFrames) {
            skip = kSettleFrames - seen;
            if (skip > frames) skip = frames;
        }
        seen += (uint32_t)frames;
        const size_t cnt = frames - skip;
        if (!cnt) continue;
        for (size_t i = 0; i < cnt; i++) {
            const int16_t v = g_raw[2 * (i + skip) + ch];
            mono[i] = v;
            const uint32_t a = (uint32_t)abs(v);
            if (a > peak) peak = a;
        }
        peak_n += (uint32_t)cnt;
        if (peak_n >= kPeakFrames) {
            const float f = sqrtf((float)peak / 32768.0f);       // gentle curve: speech fills the bar
            g_level = (uint8_t)(f >= 1.0f ? 100 : (int)(f * 100.0f));
            peak = peak_n = 0;
        }

        const size_t bytes = cnt * 2;
        const size_t head = g_head;
        if (kRing - (head - g_tail) < bytes) {
            g_overruns++;                                        // the writer cannot keep up
        } else {
            const size_t off = head & (kRing - 1);
            const size_t n1 = kRing - off < bytes ? kRing - off : bytes;
            memcpy(g_ring + off, mono, n1);
            if (bytes > n1) memcpy(g_ring, (const uint8_t *)mono + n1, bytes - n1);
            __sync_synchronize();
            g_head = head + bytes;
        }
        g_frames = g_frames + (uint32_t)cnt;
    }

    if (g_rx) {
        i2s_channel_disable(g_rx);
        i2s_del_channel(g_rx);
        g_rx = nullptr;
    }
    svc_audio_capture_release();
    g_level = 0;
    if (g_overruns) ESP_LOGW(TAG, "%u ring overruns (samples dropped)", (unsigned)g_overruns);
    g_cap_done = true;
    vTaskDelete(nullptr);
}

// ---- writer task --------------------------------------------------------------------------
static void writerTask(void *) {
    uint32_t written = 0;
    uint32_t last_patch = nowMs();
    bool io_ok = true;

    for (;;) {
        const bool finishing = g_cap_done;
        __sync_synchronize();
        const size_t avail = g_head - g_tail;
        if (avail >= kBlock || (finishing && avail > 0)) {
            const size_t n = avail < kBlock ? avail : kBlock;
            const size_t off = g_tail & (kRing - 1);
            const size_t n1 = kRing - off < n ? kRing - off : n;
            memcpy(g_block, g_ring + off, n1);
            if (n > n1) memcpy(g_block + n1, g_ring, n - n1);
            g_tail = g_tail + n;
            if (!writeAll(g_fd, g_block, n)) {
                io_ok = false;
                break;
            }
            written += (uint32_t)n;
            g_bytes = 44 + written;
            if (nowMs() - last_patch >= kPatchMs) {
                last_patch = nowMs();
                if (!patchHeader(written)) {
                    io_ok = false;
                    break;
                }
            }
            if (written >= kMaxData) g_stop = true;                // file full: normal end
            continue;
        }
        if (finishing) break;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (!io_ok) {                                                  // card removed / full / write error
        g_err = true;
        g_stop = true;
    }
    while (!g_cap_done) vTaskDelay(pdMS_TO_TICKS(10));

    // Finalise: best effort even after an I/O error.
    patchHeader(written);
    close(g_fd);
    g_fd = -1;
    const bool err = g_err;
    if (written == 0) unlink(g_path);                              // nothing recorded: no empty file
    ESP_LOGI(TAG, "%s: %u bytes%s", g_path, (unsigned)written, err ? " (error)" : "");

    heap_caps_free(g_ring);
    heap_caps_free(g_block);
    heap_caps_free(g_raw);
    g_ring = g_block = nullptr;
    g_raw = nullptr;
    g_active = false;
    fire(false, err);
    vTaskDelete(nullptr);
}

// ---- public API ---------------------------------------------------------------------------
extern "C" void svc_rec_init(svc_rec_event_cb_t cb) {
    g_cb = cb;
    if (!g_lock) g_lock = xSemaphoreCreateMutex();
}

// Highest rec_NNNN.wav index present in `dir`, 0 if none.
static unsigned highestIndex(const char *dir) {
    unsigned hi = 0;
    DIR *d = opendir(dir);
    if (!d) return 0;
    while (struct dirent *de = readdir(d)) {
        unsigned idx;
        char tail[8];
        if (strncasecmp(de->d_name, "rec_", 4) == 0 && sscanf(de->d_name + 4, "%u%7s", &idx, tail) == 2 &&
            strcasecmp(tail, ".wav") == 0 && idx > hi) {
            hi = idx;
        }
    }
    closedir(d);
    return hi;
}

static bool startFail(const char *why) {
    ESP_LOGW(TAG, "cannot start: %s", why);
    heap_caps_free(g_ring);
    heap_caps_free(g_block);
    heap_caps_free(g_raw);
    g_ring = g_block = nullptr;
    g_raw = nullptr;
    if (g_fd >= 0) {
        close(g_fd);
        g_fd = -1;
        unlink(g_path);
    }
    g_err = true;
    g_active = false;
    fire(false, true);
    return false;
}

extern "C" bool svc_rec_start(const char *dir, char *path_out, size_t cap) {
    if (!g_lock || !dir || !*dir) return false;
    xSemaphoreTake(g_lock, portMAX_DELAY);
    if (g_active) {
        xSemaphoreGive(g_lock);
        return false;
    }
    g_active = true;
    g_err = false;
    g_stop = false;
    g_cap_done = false;
    g_frames = 0;
    g_bytes = 0;
    g_level = 0;
    g_head = 0;
    g_tail = 0;
    g_overruns = 0;
    g_path[0] = 0;

    bool ok = false;
    do {
        if (mkdir(dir, 0777) != 0 && errno != EEXIST) {
            startFail("cannot create the folder");
            break;
        }
        const unsigned idx = highestIndex(dir) + 1;
        if (idx > kMaxIndex) {
            startFail("no free file name");
            break;
        }
        const size_t dl = strlen(dir);
        const bool slash = dir[dl - 1] == '/';
        const int pn = snprintf(g_path, sizeof g_path, "%s%srec_%04u.wav", dir, slash ? "" : "/", idx);
        if (pn <= 0 || (size_t)pn >= sizeof g_path) {
            g_path[0] = 0;
            startFail("path too long");
            break;
        }
        g_ring = (uint8_t *)heap_caps_malloc(kRing, MALLOC_CAP_SPIRAM);
        g_block = (uint8_t *)heap_caps_malloc(kBlock, MALLOC_CAP_SPIRAM);
        g_raw = (int16_t *)heap_caps_malloc(kReadFrames * 4, MALLOC_CAP_SPIRAM);
        if (!g_ring || !g_block || !g_raw) {
            startFail("out of memory");
            break;
        }
        g_fd = open(g_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        uint8_t h[44];
        wavHeader(h, 0);
        if (g_fd < 0 || !writeAll(g_fd, h, sizeof h)) {
            startFail("cannot create the file");
            break;
        }
        g_bytes = 44;
        if (xTaskCreatePinnedToCore(writerTask, "rec_wr", 6144, nullptr, 6, nullptr, 0) != pdPASS) {
            startFail("no writer task");
            break;
        }
        if (xTaskCreatePinnedToCore(captureTask, "rec_cap", 4096, nullptr, 10, nullptr, 0) != pdPASS) {
            // The writer is already running: let it finalise the empty file.
            g_err = true;
            g_cap_done = true;
            ok = false;
            break;
        }
        ok = true;
    } while (0);
    xSemaphoreGive(g_lock);
    if (ok) {
        if (path_out && cap) strlcpy(path_out, g_path, cap);
        fire(true, false);
    }
    return ok;
}

extern "C" void svc_rec_stop(void) {
    if (g_active) g_stop = true;
}

extern "C" void svc_rec_get_status(svc_rec_status_t *out) {
    memset(out, 0, sizeof *out);
    out->recording = g_active;
    out->error = g_err;
    out->elapsed_ms = (uint32_t)((uint64_t)g_frames * 1000 / kRate);
    out->bytes = g_bytes;
    out->level = g_active ? g_level : 0;
    strlcpy(out->path, g_path, sizeof out->path);
}
