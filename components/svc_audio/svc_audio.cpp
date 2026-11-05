// Audio engine. Two tasks, both pinned to core 0 (the UI lives on core 1):
//
//   audio  (prio 8, 8 KB stack)  reads the file in 32 KB blocks, decodes MP3/WAV into PCM and
//                                pushes it into a 64 KB ring in PSRAM.
//   aout   (prio 9, 4 KB stack)  drains the ring, applies the volume and writes to I2S. It also
//                                owns the power amplifier and the I2S channel state: both are
//                                switched off 400 ms after the last sample (no hiss, and the
//                                I2S clock does not prevent light sleep).
//
// State machine and positions are protected by `g_mu`; the ring by `g_ring_mu`.
#include "svc_audio.h"

#include <fcntl.h>
#include <math.h>
#include <string.h>
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
#include "mp3dec.h"
#include "svc_audio_priv.h"

static const char *TAG = "audio";

// ---- hardware ---------------------------------------------------------------------------
#define PIN_MCLK GPIO_NUM_17
#define PIN_BCLK GPIO_NUM_18
#define PIN_WS GPIO_NUM_21
// Data direction verified against the vendor demo (Example_16_music: I2S_DOUT = 15).
#define PIN_DOUT GPIO_NUM_15           // ESP32 -> ES8311 DSDIN
#define PIN_DIN GPIO_NUM_16            // ES8311 ASDOUT -> ESP32 (microphone, unused for now)
#define PIN_PA GPIO_NUM_1             // power amplifier, active LOW

// Output level ceiling. The codec DAC is fixed at -3 dB (0xBF = 0 dB, 0.5 dB per step) and the
// user volume only attenuates from there, so volume 100 can never exceed this level. It was
// -9 dB (0xAD) at first; +6 dB was needed for the small on-board speaker. Raise towards 0xBF for
// more level, lower it if loud tracks start to distort.
#define SVC_DAC_CAP_REG 0xB9
// Attenuation range of the volume curve (dB below the cap at volume 1).
#define SVC_VOL_RANGE_DB 40.0f

#define DMA_DESC_NUM 4
#define DMA_FRAMES 240

// ---- sizes ------------------------------------------------------------------------------
static const size_t kInBuf = 32 * 1024;
static const size_t kRing = 64 * 1024;
static const size_t kPcmBytes = 1152 * 2 * sizeof(int16_t);   // one stereo MPEG-1 frame
static const size_t kOutChunk = 1024;                          // bytes per I2S write (256 frames)

// ---- shared state -----------------------------------------------------------------------
static SemaphoreHandle_t g_mu;
static SemaphoreHandle_t g_ring_mu;
static svc_audio_event_cb_t g_cb;
static svc_audio_status_t g_st;
static uint32_t g_start_ms;                 // position at the last (re)start of the stream
static uint32_t g_rate = 44100;             // sample rate of the stream being played
static volatile uint32_t g_frames_out;      // frames written to I2S since g_start_ms
static volatile uint32_t g_epoch;           // bumped on every ring flush
static volatile uint32_t g_play_gen;        // bumped on play() and stop()
static char g_path[256];
static bool g_cmd_play;
static volatile bool g_seek_req;
static uint32_t g_seek_ms;
static volatile bool g_paused;
static volatile uint32_t g_gain_q15;

static bool g_started;                      // buffers and tasks exist
static uint8_t *g_inbuf;
static uint8_t *g_ring;
static int16_t *g_pcm;
static size_t g_head, g_tail, g_used;
static TaskHandle_t g_dec_task;

// I2S / codec (touched by the audio task, and by the aout task for enable/disable only).
static i2s_chan_handle_t g_tx;
static volatile bool g_hw_ready;
static uint32_t g_hw_rate;
static volatile bool g_hold_req, g_hold_ack;
static volatile bool g_audio_busy;          // the audio task is inside runTrack()
static volatile bool g_capture;             // the recorder owns the I2S port

// ---- small helpers ----------------------------------------------------------------------
static inline uint32_t nowMs() { return (uint32_t)(esp_timer_get_time() / 1000); }

static uint32_t gainFor(uint8_t v) {
    if (v == 0) return 0;
    if (v > 100) v = 100;
    const float db = -SVC_VOL_RANGE_DB * (1.0f - (float)v / 100.0f);
    return (uint32_t)(32767.0f * powf(10.0f, db / 20.0f));
}

static void pa(bool on) { gpio_set_level(PIN_PA, on ? 0 : 1); }

// Changes the state under the lock; returns true when the callback must be fired.
static bool setStateLocked(svc_audio_state_t s) {
    if (g_st.state == s) return false;
    g_st.state = s;
    return true;
}

static void notify(bool changed, bool ended) {
    if (!changed || !g_cb) return;
    svc_audio_status_t st;
    svc_audio_get_status(&st);
    g_cb(&st, ended);
}

// Sets the state only if `gen` is still the current command.
static void setStateIfCurrent(uint32_t gen, svc_audio_state_t s, bool ended) {
    bool ch = false;
    xSemaphoreTake(g_mu, portMAX_DELAY);
    if (gen == g_play_gen) {
        ch = setStateLocked(s);
        if (s != SVC_AUDIO_PLAYING && s != SVC_AUDIO_PAUSED) g_paused = false;
    }
    xSemaphoreGive(g_mu);
    notify(ch, ended);
}

// ---- ring buffer ------------------------------------------------------------------------
static void ringFlush() {
    xSemaphoreTake(g_ring_mu, portMAX_DELAY);
    g_head = g_tail = g_used = 0;
    g_epoch = g_epoch + 1;
    g_frames_out = 0;
    xSemaphoreGive(g_ring_mu);
}

// Producer: waits for room. Returns false when the data must be dropped (new command, seek).
static bool ringWrite(uint32_t gen, const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    for (;;) {
        xSemaphoreTake(g_ring_mu, portMAX_DELAY);
        if (gen != g_play_gen || g_seek_req) {
            xSemaphoreGive(g_ring_mu);
            return false;
        }
        if (kRing - g_used >= len) {
            size_t n1 = kRing - g_head;
            if (n1 > len) n1 = len;
            memcpy(g_ring + g_head, p, n1);
            if (len > n1) memcpy(g_ring, p + n1, len - n1);
            g_head = (g_head + len) % kRing;
            g_used += len;
            xSemaphoreGive(g_ring_mu);
            return true;
        }
        xSemaphoreGive(g_ring_mu);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static bool ringEmpty() {
    xSemaphoreTake(g_ring_mu, portMAX_DELAY);
    const bool e = g_used == 0;
    xSemaphoreGive(g_ring_mu);
    return e;
}

static size_t ringRead(void *dst, size_t max, uint32_t *epoch) {
    xSemaphoreTake(g_ring_mu, portMAX_DELAY);
    size_t n = g_used < max ? g_used : max;
    n &= ~(size_t)3;                       // whole stereo frames only
    if (n) {
        size_t n1 = kRing - g_tail;
        if (n1 > n) n1 = n;
        memcpy(dst, g_ring + g_tail, n1);
        if (n > n1) memcpy((uint8_t *)dst + n1, g_ring, n - n1);
        g_tail = (g_tail + n) % kRing;
        g_used -= n;
    }
    *epoch = g_epoch;
    xSemaphoreGive(g_ring_mu);
    return n;
}

// ---- output task ------------------------------------------------------------------------
static void outTask(void *) {
    int16_t chunk[kOutChunk / sizeof(int16_t)];
    bool i2s_on = false, pa_on = false;
    uint32_t last_audio = nowMs();
    int32_t cur_gain = (int32_t)g_gain_q15;

    for (;;) {
        if (g_hold_req) {                  // the audio task wants to reconfigure the clock
            if (pa_on) { pa(false); pa_on = false; }
            if (i2s_on) { i2s_channel_disable(g_tx); i2s_on = false; }
            g_hold_ack = true;
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }
        uint32_t ep = 0;
        const size_t n = (g_paused || !g_hw_ready) ? 0 : ringRead(chunk, sizeof chunk, &ep);
        if (!n) {
            if ((pa_on || i2s_on) && nowMs() - last_audio > 400) {
                if (pa_on) { pa(false); pa_on = false; }
                if (i2s_on) { i2s_channel_disable(g_tx); i2s_on = false; }
            }
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        last_audio = nowMs();
        if (!i2s_on) {
            if (i2s_channel_enable(g_tx) != ESP_OK) { vTaskDelay(pdMS_TO_TICKS(50)); continue; }
            i2s_on = true;
            vTaskDelay(pdMS_TO_TICKS(20));     // let the clocks settle on silence
        }
        if (!pa_on) {
            pa(true);
            pa_on = true;
            vTaskDelay(pdMS_TO_TICKS(5));
        }
        // Volume with a linear ramp across the chunk (no zipper noise).
        const size_t frames = n / 4;
        const int32_t tgt = (int32_t)g_gain_q15;
        const int32_t step = (tgt - cur_gain) / (int32_t)frames;
        int32_t g = cur_gain;
        for (size_t i = 0; i < frames; i++) {
            g += step;
            chunk[2 * i] = (int16_t)(((int32_t)chunk[2 * i] * g) >> 15);
            chunk[2 * i + 1] = (int16_t)(((int32_t)chunk[2 * i + 1] * g) >> 15);
        }
        cur_gain = tgt;
        size_t written = 0;
        i2s_channel_write(g_tx, chunk, n, &written, 200);
        if (ep == g_epoch) g_frames_out = g_frames_out + written / 4;
    }
}

// ---- hardware bring-up (lazy, audio task) -----------------------------------------------
static bool hwInit() {
    if (g_hw_ready) return true;
    i2c_master_bus_handle_t bus = bsp_i2c_get();
    if (!bus) { ESP_LOGW(TAG, "I2C bus not available"); return false; }

    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    cc.dma_desc_num = DMA_DESC_NUM;
    cc.dma_frame_num = DMA_FRAMES;
    cc.auto_clear_after_cb = true;
    if (i2s_new_channel(&cc, &g_tx, nullptr) != ESP_OK) { g_tx = nullptr; return false; }

    i2s_std_config_t sc = {};
    sc.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(44100);
    sc.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    sc.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
    sc.gpio_cfg.mclk = PIN_MCLK;
    sc.gpio_cfg.bclk = PIN_BCLK;
    sc.gpio_cfg.ws = PIN_WS;
    sc.gpio_cfg.dout = PIN_DOUT;
    sc.gpio_cfg.din = PIN_DIN;
    if (i2s_channel_init_std_mode(g_tx, &sc) != ESP_OK || i2s_channel_enable(g_tx) != ESP_OK) {
        i2s_del_channel(g_tx);
        g_tx = nullptr;
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(10));                       // MCLK must run while the codec starts
    const esp_err_t e = codec_es8311_init(bus);
    codec_es8311_set_dac_volume(SVC_DAC_CAP_REG);
    i2s_channel_disable(g_tx);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "codec init failed: %s", esp_err_to_name(e));
        i2s_del_channel(g_tx);
        g_tx = nullptr;
        return false;
    }
    g_hw_rate = 44100;
    g_hw_ready = true;
    ESP_LOGI(TAG, "I2S + ES8311 ready");
    return true;
}

// Switches the I2S clock to `rate` (output task parked, ring already flushed).
static void setFormat(uint32_t rate) {
    g_rate = rate;
    if (rate == g_hw_rate) return;
    g_hold_ack = false;
    g_hold_req = true;
    for (int i = 0; i < 100 && !g_hold_ack; i++) vTaskDelay(pdMS_TO_TICKS(2));
    i2s_std_clk_config_t clk = I2S_STD_CLK_DEFAULT_CONFIG(rate);
    clk.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    if (i2s_channel_reconfig_std_clock(g_tx, &clk) == ESP_OK) g_hw_rate = rate;
    else ESP_LOGW(TAG, "cannot set %u Hz", (unsigned)rate);
    g_hold_req = false;
}

// ---- decoding ---------------------------------------------------------------------------
enum Result { kDone, kAborted, kFailed };

static bool aborted(uint32_t gen) { return gen != g_play_gen; }

static void setDuration(uint32_t gen, uint32_t dur_ms) {
    xSemaphoreTake(g_mu, portMAX_DELAY);
    if (gen == g_play_gen) g_st.dur_ms = dur_ms;
    xSemaphoreGive(g_mu);
}

// Consumes a pending seek: the caller moves the file, this flushes and clears the request.
static bool takeSeek(uint32_t *ms) {
    bool r = false;
    xSemaphoreTake(g_mu, portMAX_DELAY);
    if (g_seek_req) {
        *ms = g_seek_ms;
        r = true;
    }
    xSemaphoreGive(g_mu);
    return r;
}

static void finishSeek() {
    ringFlush();
    xSemaphoreTake(g_mu, portMAX_DELAY);
    g_seek_req = false;
    xSemaphoreGive(g_mu);
}

static int readFull(int fd, void *dst, size_t len) {
    size_t got = 0;
    while (got < len) {
        const ssize_t r = read(fd, (uint8_t *)dst + got, len - got);
        if (r < 0) return -1;
        if (r == 0) break;
        got += (size_t)r;
    }
    return (int)got;
}

// ---- MP3 ----------------------------------------------------------------------------------
struct Mp3Info {
    uint32_t audio_start = 0;
    uint32_t audio_bytes = 0;
    uint32_t dur_ms = 0;
    uint32_t kbps = 0;
    bool has_toc = false;
    uint8_t toc[100];
};

static const uint16_t kBr1[16] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0};
static const uint16_t kBr2[16] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0};
static const uint32_t kSr1[3] = {44100, 48000, 32000};

static uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }

// Skips ID3v2, finds the first frame and reads the Xing/Info header when present.
static void probeMp3(int fd, off_t fsize, Mp3Info &mi) {
    uint8_t h[10];
    lseek(fd, 0, SEEK_SET);
    if (readFull(fd, h, 10) == 10 && !memcmp(h, "ID3", 3)) {
        const uint32_t sz = ((h[6] & 0x7F) << 21) | ((h[7] & 0x7F) << 14) | ((h[8] & 0x7F) << 7) | (h[9] & 0x7F);
        mi.audio_start = 10 + sz + ((h[5] & 0x10) ? 10 : 0);
    }
    if (mi.audio_start > (uint32_t)fsize) mi.audio_start = 0;
    mi.audio_bytes = (uint32_t)fsize - mi.audio_start;

    uint8_t *b = g_inbuf;                          // scratch, the decoder refills it afterwards
    lseek(fd, mi.audio_start, SEEK_SET);
    const int n = readFull(fd, b, 4096);
    for (int i = 0; i + 4 <= n; i++) {
        if (b[i] != 0xFF || (b[i + 1] & 0xE0) != 0xE0) continue;
        const int ver = (b[i + 1] >> 3) & 3;       // 3 = MPEG1, 2 = MPEG2, 0 = MPEG2.5
        const int layer = (b[i + 1] >> 1) & 3;     // 1 = layer III
        const int bri = b[i + 2] >> 4;
        const int sri = (b[i + 2] >> 2) & 3;
        if (ver == 1 || layer != 1 || bri == 0 || bri == 15 || sri == 3) continue;
        const bool mpeg1 = ver == 3;
        const uint32_t rate = kSr1[sri] >> (mpeg1 ? 0 : (ver == 2 ? 1 : 2));
        const bool mono = (b[i + 3] >> 6) == 3;
        mi.kbps = mpeg1 ? kBr1[bri] : kBr2[bri];
        const uint32_t spf = mpeg1 ? 1152 : 576;
        const int side = mpeg1 ? (mono ? 17 : 32) : (mono ? 9 : 17);
        const int xo = i + 4 + ((b[i + 1] & 1) ? 0 : 2) + side;
        if (xo + 8 <= n && (!memcmp(b + xo, "Xing", 4) || !memcmp(b + xo, "Info", 4))) {
            const uint32_t flags = be32(b + xo + 4);
            int o = xo + 8;
            if ((flags & 1) && o + 4 <= n) {
                const uint32_t frames = be32(b + o);
                mi.dur_ms = (uint32_t)((uint64_t)frames * spf * 1000 / rate);
                o += 4;
            }
            if ((flags & 2) && o + 4 <= n) {
                const uint32_t bytes = be32(b + o);
                if (bytes) mi.audio_bytes = bytes;
                o += 4;
            }
            if ((flags & 4) && o + 100 <= n) {
                memcpy(mi.toc, b + o, 100);
                mi.has_toc = true;
            }
        }
        if (!mi.dur_ms && mi.kbps) mi.dur_ms = (uint32_t)((uint64_t)mi.audio_bytes * 8 / mi.kbps);
        return;
    }
}

static uint32_t mp3SeekOffset(const Mp3Info &mi, uint32_t ms) {
    if (mi.dur_ms && ms >= mi.dur_ms) ms = mi.dur_ms - 1;
    if (mi.has_toc && mi.dur_ms) {
        const float pct = 100.0f * (float)ms / (float)mi.dur_ms;
        int idx = (int)pct;
        if (idx > 99) idx = 99;
        const float a = mi.toc[idx];
        const float b = idx < 99 ? mi.toc[idx + 1] : 256.0f;
        const float f = (a + (b - a) * (pct - (float)idx)) / 256.0f;
        return mi.audio_start + (uint32_t)(f * (float)mi.audio_bytes);
    }
    return mi.audio_start + (uint32_t)((uint64_t)ms * mi.kbps / 8);
}

static Result playMp3(int fd, off_t fsize, uint32_t gen) {
    Mp3Info mi;
    probeMp3(fd, fsize, mi);
    setDuration(gen, mi.dur_ms);

    HMP3Decoder dec = MP3InitDecoder();
    if (!dec) { ESP_LOGW(TAG, "mp3 decoder alloc failed"); return kFailed; }

    lseek(fd, mi.audio_start, SEEK_SET);
    uint8_t *rp = g_inbuf;
    int bl = 0;
    bool eof = false, draining = false, first = true;
    int errs = 0;
    uint32_t frames_ok = 0;
    uint32_t drain_since = 0;
    Result res = kDone;

    for (;;) {
        if (aborted(gen)) { res = kAborted; break; }
        uint32_t sk;
        if (takeSeek(&sk)) {
            lseek(fd, mp3SeekOffset(mi, sk), SEEK_SET);
            rp = g_inbuf;
            bl = 0;
            eof = draining = false;
            first = true;                          // resync after the jump
            finishSeek();
            continue;
        }
        if (g_paused) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
        if (draining) {
            if (!ringEmpty()) { drain_since = 0; vTaskDelay(pdMS_TO_TICKS(10)); continue; }
            if (!drain_since) drain_since = nowMs();
            if (nowMs() - drain_since < 80) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }   // DMA tail
            break;
        }

        if (bl < 2048 && !eof) {
            if (bl > 0 && rp != g_inbuf) memmove(g_inbuf, rp, (size_t)bl);
            rp = g_inbuf;
            const int r = readFull(fd, g_inbuf + bl, kInBuf - (size_t)bl);
            if (r < 0) { res = kFailed; break; }
            if (r == 0) eof = true;
            bl += r;
        }
        if (bl < 4) { if (eof) { draining = true; continue; } continue; }

        const int off = MP3FindSyncWord(rp, bl);
        if (off < 0) {
            if (eof) { draining = true; continue; }
            rp += bl - 3;                          // keep a possible partial header
            bl = 3;
            continue;
        }
        rp += off;
        bl -= off;

        uint8_t *before = rp;
        const int err = MP3Decode(dec, &rp, &bl, g_pcm, 0);
        if (err == ERR_MP3_NONE) {
            MP3FrameInfo fi;
            MP3GetLastFrameInfo(dec, &fi);
            if (fi.samprate <= 0 || fi.outputSamps <= 0 || fi.bitsPerSample != 16) { errs++; continue; }
            errs = 0;
            frames_ok++;
            size_t frames = (size_t)fi.outputSamps / (fi.nChans > 0 ? fi.nChans : 1);
            if (fi.nChans == 1) {                  // mono -> stereo, in place, backwards
                for (size_t i = frames; i-- > 0;) g_pcm[2 * i] = g_pcm[2 * i + 1] = g_pcm[i];
            }
            if (first) {
                first = false;
                setFormat((uint32_t)fi.samprate);
            }
            if (!ringWrite(gen, g_pcm, frames * 4)) continue;     // aborted or seek
        } else if (err == ERR_MP3_INDATA_UNDERFLOW) {
            if (eof) draining = true;
            else if (rp == before && bl >= 2048) { rp++; bl--; }   // never spin
        } else {
            if (rp == before) { rp++; bl--; }      // guarantee progress
            if (++errs > 400) { ESP_LOGW(TAG, "mp3: too many decode errors"); res = kFailed; break; }
        }
    }
    MP3FreeDecoder(dec);
    if (res == kDone && frames_ok == 0) res = kFailed;
    return res;
}

// ---- WAV ----------------------------------------------------------------------------------
static uint32_t le32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint32_t le16(const uint8_t *p) { return p[0] | (p[1] << 8); }

static Result playWav(int fd, off_t fsize, uint32_t gen) {
    uint8_t h[12];
    if (readFull(fd, h, 12) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) return kFailed;
    uint32_t rate = 0, ch = 0, bits = 0, tag = 0, data_off = 0, data_size = 0;
    off_t pos = 12;
    while (pos + 8 <= fsize) {
        uint8_t ck[8];
        lseek(fd, pos, SEEK_SET);
        if (readFull(fd, ck, 8) != 8) break;
        const uint32_t sz = le32(ck + 4);
        if (!memcmp(ck, "fmt ", 4) && sz >= 16) {
            uint8_t f[16];
            if (readFull(fd, f, 16) != 16) break;
            tag = le16(f);
            ch = le16(f + 2);
            rate = le32(f + 4);
            bits = le16(f + 14);
        } else if (!memcmp(ck, "data", 4)) {
            data_off = (uint32_t)pos + 8;
            data_size = sz;
            break;
        }
        pos += 8 + (off_t)sz + (sz & 1);
    }
    if (!data_off || (tag != 1 && tag != 0xFFFE) || bits != 16 || (ch != 1 && ch != 2) || rate < 8000 ||
        rate > 48000) {
        ESP_LOGW(TAG, "wav: unsupported (tag %u, %u ch, %u bit, %u Hz)", (unsigned)tag, (unsigned)ch,
                 (unsigned)bits, (unsigned)rate);
        return kFailed;
    }
    if (data_size == 0 || data_off + (uint64_t)data_size > (uint64_t)fsize) data_size = (uint32_t)fsize - data_off;
    const uint32_t align = ch * 2;
    data_size -= data_size % align;
    setDuration(gen, (uint32_t)((uint64_t)(data_size / align) * 1000 / rate));
    setFormat(rate);

    uint32_t consumed = 0;                         // bytes of the data chunk already read
    lseek(fd, data_off, SEEK_SET);
    bool draining = false;
    uint32_t drain_since = 0;
    const size_t step = 2048 / align * align;      // input bytes per iteration
    for (;;) {
        if (aborted(gen)) return kAborted;
        uint32_t sk;
        if (takeSeek(&sk)) {
            uint64_t frames = (uint64_t)sk * rate / 1000;
            if (frames * align >= data_size) frames = data_size / align ? data_size / align - 1 : 0;
            consumed = (uint32_t)(frames * align);
            lseek(fd, data_off + consumed, SEEK_SET);
            draining = false;
            finishSeek();
            continue;
        }
        if (g_paused) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
        if (draining) {
            if (!ringEmpty()) { drain_since = 0; vTaskDelay(pdMS_TO_TICKS(10)); continue; }
            if (!drain_since) drain_since = nowMs();
            if (nowMs() - drain_since < 80) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }
            return kDone;
        }
        size_t want = step;
        if (want > data_size - consumed) want = data_size - consumed;
        if (!want) { draining = true; continue; }
        const int r = readFull(fd, g_inbuf, want);
        if (r <= 0) { if (r < 0) return kFailed; draining = true; continue; }
        const size_t frames = (size_t)r / align;
        consumed += (uint32_t)(frames * align);
        const int16_t *in = (const int16_t *)g_inbuf;
        if (ch == 1) {
            for (size_t i = 0; i < frames; i++) g_pcm[2 * i] = g_pcm[2 * i + 1] = in[i];
        } else {
            memcpy(g_pcm, in, frames * 4);
        }
        ringWrite(gen, g_pcm, frames * 4);
    }
}

// ---- audio task -------------------------------------------------------------------------
static bool endsWithNoCase(const char *s, const char *ext) {
    const size_t n = strlen(s), e = strlen(ext);
    return n >= e && strcasecmp(s + n - e, ext) == 0;
}

// Hardware test: a 440 Hz sine for 1.5 s pushed through the normal ring/I2S/codec path.
static Result playTone(uint32_t gen) {
    const uint32_t rate = 44100, total = rate * 3 / 2;
    setDuration(gen, 1500);
    setFormat(rate);
    int16_t buf[512];                               // 256 stereo frames
    float phase = 0;
    const float inc = 2.0f * (float)M_PI * 440.0f / (float)rate;
    for (uint32_t done = 0; done < total;) {
        if (aborted(gen)) return kAborted;
        for (int i = 0; i < 256; i++) {
            const int16_t v = (int16_t)(sinf(phase) * 16000.0f);
            buf[2 * i] = buf[2 * i + 1] = v;
            phase += inc;
            if (phase > 2.0f * (float)M_PI) phase -= 2.0f * (float)M_PI;
        }
        if (!ringWrite(gen, buf, sizeof buf)) return kAborted;
        done += 256;
    }
    uint32_t quiet_since = 0;
    for (;;) {                                      // wait for the ring to drain
        if (aborted(gen)) return kAborted;
        if (!ringEmpty()) { quiet_since = 0; vTaskDelay(pdMS_TO_TICKS(10)); continue; }
        if (!quiet_since) quiet_since = nowMs();
        if (nowMs() - quiet_since >= 80) return kDone;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static void runTrack(const char *path, uint32_t gen) {
    if (!hwInit()) { setStateIfCurrent(gen, SVC_AUDIO_ERROR, false); return; }
    if (!strcmp(path, "@tone")) {
        ringFlush();
        const Result r = playTone(gen);
        if (r == kDone) setStateIfCurrent(gen, SVC_AUDIO_STOPPED, true);
        return;
    }
    struct stat sb;
    const int fd = stat(path, &sb) == 0 ? open(path, O_RDONLY) : -1;
    if (fd < 0) {
        ESP_LOGW(TAG, "cannot open %s", path);
        setStateIfCurrent(gen, SVC_AUDIO_ERROR, false);
        return;
    }
    ringFlush();
    const Result r = endsWithNoCase(path, ".wav") ? playWav(fd, sb.st_size, gen)
                                                  : playMp3(fd, sb.st_size, gen);
    close(fd);
    if (r == kDone) setStateIfCurrent(gen, SVC_AUDIO_STOPPED, true);
    else if (r == kFailed) setStateIfCurrent(gen, SVC_AUDIO_ERROR, false);
}

static void audioTask(void *) {
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        for (;;) {
            char path[sizeof g_path];
            uint32_t gen;
            xSemaphoreTake(g_mu, portMAX_DELAY);
            const bool go = g_cmd_play;
            g_cmd_play = false;
            gen = g_play_gen;
            memcpy(path, g_path, sizeof path);
            if (go) g_audio_busy = true;           // set under the lock: capture_acquire() waits on it
            xSemaphoreGive(g_mu);
            if (!go) break;
            runTrack(path, gen);
            g_audio_busy = false;
        }
    }
}

static bool startLocked() {
    if (g_started) return true;
    g_inbuf = (uint8_t *)heap_caps_malloc(kInBuf, MALLOC_CAP_SPIRAM);
    g_ring = (uint8_t *)heap_caps_malloc(kRing, MALLOC_CAP_SPIRAM);
    g_pcm = (int16_t *)heap_caps_malloc(kPcmBytes, MALLOC_CAP_SPIRAM);
    TaskHandle_t out = nullptr;
    bool ok = g_inbuf && g_ring && g_pcm;
    ok = ok && xTaskCreatePinnedToCore(audioTask, "audio", 8192, nullptr, 8, &g_dec_task, 0) == pdPASS;
    ok = ok && xTaskCreatePinnedToCore(outTask, "aout", 4096, nullptr, 9, &out, 0) == pdPASS;
    if (!ok) {
        ESP_LOGE(TAG, "cannot start the audio engine");
        if (g_dec_task) { vTaskDelete(g_dec_task); g_dec_task = nullptr; }
        heap_caps_free(g_inbuf); heap_caps_free(g_ring); heap_caps_free(g_pcm);
        g_inbuf = g_ring = nullptr;
        g_pcm = nullptr;
        return false;
    }
    g_started = true;
    return true;
}

// ---- public API -------------------------------------------------------------------------
extern "C" void svc_audio_init(svc_audio_event_cb_t cb, uint8_t volume) {
    g_mu = xSemaphoreCreateMutex();
    g_ring_mu = xSemaphoreCreateMutex();
    g_cb = cb;
    g_st.volume = volume > 100 ? 100 : volume;
    g_gain_q15 = gainFor(g_st.volume);
    gpio_config_t io = {};
    io.pin_bit_mask = 1ULL << PIN_PA;
    io.mode = GPIO_MODE_OUTPUT;
    gpio_set_level(PIN_PA, 1);                     // amplifier off
    gpio_config(&io);
    gpio_set_level(PIN_PA, 1);
}

extern "C" bool svc_audio_play(const char *path) {
    if (!path || !*path || strlen(path) >= sizeof g_path) return false;
    xSemaphoreTake(g_mu, portMAX_DELAY);
    if (g_capture) {                               // the microphone owns the I2S port
        xSemaphoreGive(g_mu);
        return false;
    }
    if (!startLocked()) {
        xSemaphoreGive(g_mu);
        return false;
    }
    g_play_gen = g_play_gen + 1;
    strlcpy(g_path, path, sizeof g_path);
    g_cmd_play = true;
    g_paused = false;
    g_seek_req = false;
    g_start_ms = 0;
    g_st.pos_ms = 0;
    g_st.dur_ms = 0;
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    strlcpy(g_st.title, base, sizeof g_st.title);
    char *dot = strrchr(g_st.title, '.');
    if (dot && dot != g_st.title) *dot = 0;
    setStateLocked(SVC_AUDIO_PLAYING);
    xSemaphoreGive(g_mu);
    ringFlush();
    xTaskNotifyGive(g_dec_task);
    notify(true, false);               // also when the track changes while already playing
    return true;
}

extern "C" void svc_audio_pause(void) {
    xSemaphoreTake(g_mu, portMAX_DELAY);
    bool ch = false;
    if (g_st.state == SVC_AUDIO_PLAYING) {
        g_paused = true;
        ch = setStateLocked(SVC_AUDIO_PAUSED);
    }
    xSemaphoreGive(g_mu);
    notify(ch, false);
}

extern "C" void svc_audio_resume(void) {
    xSemaphoreTake(g_mu, portMAX_DELAY);
    bool ch = false;
    if (g_st.state == SVC_AUDIO_PAUSED) {
        g_paused = false;
        ch = setStateLocked(SVC_AUDIO_PLAYING);
    }
    xSemaphoreGive(g_mu);
    notify(ch, false);
}

extern "C" void svc_audio_stop(void) {
    if (!g_mu) return;
    xSemaphoreTake(g_mu, portMAX_DELAY);
    g_play_gen = g_play_gen + 1;
    g_cmd_play = false;
    g_paused = false;
    g_seek_req = false;
    g_start_ms = 0;
    g_st.pos_ms = 0;
    const bool ch = setStateLocked(SVC_AUDIO_STOPPED);
    xSemaphoreGive(g_mu);
    if (g_started) ringFlush();
    notify(ch, false);
}

// ---- hand-over to the recorder ------------------------------------------------------------
extern "C" bool svc_audio_capture_acquire(void) {
    if (!g_mu) return false;
    xSemaphoreTake(g_mu, portMAX_DELAY);
    if (g_capture) {
        xSemaphoreGive(g_mu);
        return false;
    }
    g_capture = true;                              // from now on play() is refused
    g_play_gen = g_play_gen + 1;
    g_cmd_play = false;
    g_paused = false;
    g_seek_req = false;
    g_start_ms = 0;
    g_st.pos_ms = 0;
    const bool ch = setStateLocked(SVC_AUDIO_STOPPED);
    const bool started = g_started;
    xSemaphoreGive(g_mu);

    bool ok = true;
    if (started) {
        ringFlush();
        for (int i = 0; i < 150 && g_audio_busy; i++) vTaskDelay(pdMS_TO_TICKS(10));
        if (g_audio_busy) {
            ok = false;
        } else {
            g_hold_ack = false;
            g_hold_req = true;                     // parks aout: PA off, TX disabled
            for (int i = 0; i < 100 && !g_hold_ack; i++) vTaskDelay(pdMS_TO_TICKS(2));
            ok = g_hold_ack;
        }
    }
    if (ok) {
        g_hw_ready = false;
        if (g_tx) {                                // the recorder builds its own RX channel
            i2s_channel_disable(g_tx);             // harmless error if already disabled
            i2s_del_channel(g_tx);
            g_tx = nullptr;
        }
        g_hw_rate = 0;
    } else {
        ESP_LOGW(TAG, "audio engine did not go idle for the recorder");
        g_hold_req = false;
        g_capture = false;
    }
    pa(false);
    notify(ch, false);
    return ok;
}

extern "C" void svc_audio_capture_release(void) {
    g_hold_req = false;
    g_capture = false;
}

extern "C" bool svc_audio_seek(uint32_t ms) {
    xSemaphoreTake(g_mu, portMAX_DELAY);
    const bool ok = (g_st.state == SVC_AUDIO_PLAYING || g_st.state == SVC_AUDIO_PAUSED) && g_st.dur_ms > 0;
    if (ok) {
        if (ms >= g_st.dur_ms) ms = g_st.dur_ms - 1;
        g_seek_ms = ms;
        g_start_ms = ms;
        g_seek_req = true;
    }
    xSemaphoreGive(g_mu);
    if (ok) ringFlush();                           // also zeroes the played-frames counter
    return ok;
}

extern "C" void svc_audio_set_volume(uint8_t percent) {
    if (percent > 100) percent = 100;
    g_gain_q15 = gainFor(percent);
    if (g_mu) xSemaphoreTake(g_mu, portMAX_DELAY);
    g_st.volume = percent;
    if (g_mu) xSemaphoreGive(g_mu);
}

extern "C" void svc_audio_get_status(svc_audio_status_t *out) {
    xSemaphoreTake(g_mu, portMAX_DELAY);
    *out = g_st;
    if (g_st.state == SVC_AUDIO_PLAYING || g_st.state == SVC_AUDIO_PAUSED) {
        out->pos_ms = g_start_ms + (uint32_t)((uint64_t)g_frames_out * 1000 / (g_rate ? g_rate : 44100));
        if (out->dur_ms && out->pos_ms > out->dur_ms) out->pos_ms = out->dur_ms;
    }
    xSemaphoreGive(g_mu);
}
