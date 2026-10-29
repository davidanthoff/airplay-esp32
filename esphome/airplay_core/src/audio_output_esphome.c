/*
 * audio_output_esphome.c — an audio output backend (implements
 * main/audio/audio_output.h) that writes into ESPHome's media
 * pipeline instead of driving I2S itself.
 *
 * The firmware already selects exactly one backend at build time
 * (audio_output.c = I2S, audio_output_spdif.c, audio_output_usb.c) and keeps
 * weak defaults for the optional calls in audio_output_common.c. This file is
 * one more backend. Read main/audio/audio_output.c next to it: the structure
 * (playback task pulling from audio_output_read_source(), queue-depth cursor
 * for the timing engine) is deliberately the same.
 *
 * The one idea that matters:
 *   I2S backend:           queued = frames handed to DMA - frames DMA sent
 *   this backend:          queued = frames ESPHome accepted
 *                                   - frames ESPHome reported as played
 *                                     (MediaSource::notify_audio_played)
 * audio_output_get_pipeline_us() turns that into "how long until the next
 * sample we write is heard", which is what the timing engine's position
 * servo needs. Everything between write_output() and the I2S peripheral
 * (resampler, mixer, ring buffers, DMA) is inside that measurement. Only
 * delay after the ESP32 (TOSLINK receiver, amp DSP) is not, which is what
 * output_delay_us is for. It is signed: positive plays earlier to make up
 * for delay after the ESP32, negative plays later.
 *
 * Engine v2 renders each block for the instant
 * audio_output_get_next_playout_time_ns() returns, i.e. our pipeline_us plus
 * its own 5 ms (see MAIN_PIPELINE_LATENCY_US below), and corrects clock
 * drift by dropping or repeating single samples.
 *
 * STATUS: playback, and sync to within ~1.5 ms of an Apple TV, were verified
 * on hardware against the timing engine before engine v2. Re-measure on
 * engine v2.
 */

#include "audio_output.h"

#include <inttypes.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "airplay_core_internal.h"

static const char *TAG = "airplay_out";

/* Frames per AirPlay packet. Same value as main/audio/audio_output.c. */
#define FRAME_SAMPLES 352

/* How long one host write may block. Short, so stop/deactivate is noticed
 * quickly; the retry loop covers the backpressure case. */
#define HOST_WRITE_TIMEOUT_MS 20

/* Fallback latency model, used only when no played-frames feedback has
 * arrived yet (audio_output_get_hardware_latency_us()).
 * TODO: measure on real hardware (log pipeline_us once playing) and
 * replace this guess. */
#define HOST_NOMINAL_LATENCY_US 100000

#define PLAYBACK_TASK_STACK 4096

/* ESPHome reports played frames in batches: in S/PDIF mode every 4 DMA blocks
 * (4 x 192 frames, ~17.4 ms at 44.1 kHz), with standard I2S about once per
 * DMA buffer (~15 ms). Each report's timestamp is when its last sample left
 * the wire. Between reports, frames keep playing, so "played" is
 * extrapolated from that timestamp. Never further than this, so a stalled
 * or stopped chain cannot make the queue read short. */
#define PLAYED_EXTRAPOLATE_MAX_US 25000

/* audio_output_get_next_playout_time_ns() (audio_output_common.c,
 * OUTPUT_PIPELINE_LATENCY_US; audio_timing.c before engine v2) adds 5 ms,
 * "scheduling + I2S write delay", on top of what we return. Our measured
 * queue already covers everything from write_output() to the wire, so those
 * 5 ms would be counted twice. On hardware, before this and the extrapolation
 * above, the ESP played ~14 ms early against an Apple TV: ~9 ms from the
 * batched reports, ~5 ms from this. Keep this in step with
 * OUTPUT_PIPELINE_LATENCY_US. */
#define MAIN_PIPELINE_LATENCY_US 5000

/* On resume after a pause, keep the cursor unless nothing has been played for
 * this long. By then the speaker chain (resampler + mixer + I2S, ~0.7 s of
 * buffering plus their idle timeouts) has certainly drained, so any frames it
 * never reported as played were dropped and must not count as queued. */
#define CURSOR_IDLE_RESET_US 1500000

/* Period of the pipeline diagnostic log line while output is active. */
#define PIPELINE_LOG_INTERVAL_US 10000000

static TaskHandle_t s_task = NULL;
static volatile bool s_running = false;
static volatile bool s_active = false;
static volatile bool s_flush_requested = false;
static volatile int s_source_rate = 44100;
/* Set from the main loop (airplay_core_set_output_delay_us), read by the
 * timing engine's tasks; __atomic builtins like the counters below. */
static int32_t s_output_delay_us = 0;

/* Written by the playback task / speaker callback task; 64-bit so they never
 * wrap. Accessed with __atomic builtins like main/ does. */
static uint64_t s_submitted_frames = 0;
static uint64_t s_played_frames = 0;
static int64_t s_last_played_us = 0;

/* All-zero frame used when the receiver has nothing for us. */
static int16_t s_silence[FRAME_SAMPLES * 2];

static void reset_cursor(void) {
  __atomic_store_n(&s_submitted_frames, 0, __ATOMIC_RELAXED);
  __atomic_store_n(&s_played_frames, 0, __ATOMIC_RELAXED);
  __atomic_store_n(&s_last_played_us, 0, __ATOMIC_RELAXED);
}

/* Push all frames to the host, retrying while it is not ready. Returns false
 * if we were stopped/deactivated before everything was accepted. */
static bool write_all(const int16_t *pcm, size_t frames, uint32_t rate) {
  size_t done = 0;
  while (done < frames) {
    if (!s_running || !s_active) {
      return false;
    }
    size_t n = airplay_core_host_write(pcm + done * 2, frames - done, rate,
                                       HOST_WRITE_TIMEOUT_MS);
    if (n > 0) {
      done += n;
      __atomic_add_fetch(&s_submitted_frames, (uint64_t)n, __ATOMIC_RELAXED);
    }
    /* n == 0: the host blocked for HOST_WRITE_TIMEOUT_MS (not yet the active
     * source, or reconfiguring the speaker for a new stream format). Retry
     * the same data. The timing engine's servo absorbs the delay. */
  }
  return true;
}

static void playback_task(void *arg) {
  (void)arg;
  int16_t *pcm =
      heap_caps_malloc((size_t)(FRAME_SAMPLES + 1) * 2 * sizeof(int16_t),
                       MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (pcm == NULL) {
    ESP_LOGE(TAG, "Failed to allocate playback buffer");
    s_task = NULL;
    vTaskDelete(NULL);
    return;
  }

  int64_t last_log_us = 0;
  while (s_running) {
    if (!s_active) {
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }

    if (s_flush_requested) {
      s_flush_requested = false;
      /* TODO: the I2S backend flushes the DMA ring here. We cannot remove audio
       * that is already queued inside ESPHome's speaker chain through the
       * MediaSource API. Right now stale audio (up to the chain's buffer
       * depth) plays out after a seek/pause and the timing engine drops the
       * late frames that follow. Options: accept it; stop the speaker
       * on flush (check that HA/MA don't show a stop); or keep the chain
       * shallow (the resampler/mixer buffer_duration). */
    }

    int64_t now_us = esp_timer_get_time();
    if (now_us - last_log_us >= PIPELINE_LOG_INTERVAL_US) {
      last_log_us = now_us;
      uint32_t pipeline_us = 0;
      bool measured = audio_output_get_pipeline_us(NULL, &pipeline_us);
      ESP_LOGI(TAG,
               "pipeline=%" PRIu32 " ms (%s) submitted=%" PRIu64
               " played=%" PRIu64,
               pipeline_us / 1000, measured ? "measured" : "no feedback yet",
               __atomic_load_n(&s_submitted_frames, __ATOMIC_RELAXED),
               __atomic_load_n(&s_played_frames, __ATOMIC_RELAXED));
      airplay_core_log_clock_status();
    }

    uint32_t rate = (uint32_t)s_source_rate;
    /* Same as audio_receiver_read() here: only the firmware's Sendspin player,
     * which we do not compile, installs another source. */
    size_t frames = audio_output_read_source(pcm, FRAME_SAMPLES + 1);
    if (frames > 0) {
      write_all(pcm, frames, rate);
    } else {
      /* Receiver underflow or still waiting for the first frame's play time:
       * the I2S backend keeps the DMA clocked with silence so the playout
       * position stays continuous. Do the same, so the host's played-frames
       * counter keeps advancing and pipeline_us stays meaningful. */
      write_all(s_silence, FRAME_SAMPLES, rate);
    }
  }

  heap_caps_free(pcm);
  s_task = NULL;
  vTaskDelete(NULL);
}

/* ---- glue called from airplay_core.c --------------------------------- */

void airplay_output_configure(int32_t output_delay_us) {
  __atomic_store_n(&s_output_delay_us, output_delay_us, __ATOMIC_RELAXED);
}

void airplay_output_set_active(bool active) {
  if (active && !s_active) {
    /* Resume after a pause. Do NOT simply reset: after a quick pause/resume
     * (Apple Music does one on many track changes) the speaker chain is still
     * playing out up to ~0.7 s of earlier audio. Its played-frame reports
     * would then count against a zeroed "submitted", the queue would read as
     * empty while it is not, and every later frame would play that much late
     * for the rest of the session. Only reset once the chain has gone idle. */
    int64_t last = __atomic_load_n(&s_last_played_us, __ATOMIC_RELAXED);
    if (last != 0 && esp_timer_get_time() - last > CURSOR_IDLE_RESET_US) {
      reset_cursor();
    }
  }
  s_active = active;
}

void airplay_output_reset_cursor(void) {
  /* New playback: the host (speaker_source) has just reset its own
   * pending-frame counter, so from here on it reports exactly the frames we
   * write. Start from zero at the same point. */
  reset_cursor();
}

void airplay_output_notify_played(uint32_t frames, int64_t timestamp_us) {
  __atomic_add_fetch(&s_played_frames, (uint64_t)frames, __ATOMIC_RELAXED);
  __atomic_store_n(&s_last_played_us, timestamp_us, __ATOMIC_RELAXED);
}

/* ---- main/audio/audio_output.h API ----------------------------------- */

esp_err_t audio_output_init(void) {
  memset(s_silence, 0, sizeof(s_silence));
  reset_cursor();
  return ESP_OK;
}

void audio_output_start(void) {
  if (s_task != NULL) {
    return;
  }
  s_running = true;
  /* The I2S backend pins playback to a core and gives it
   * AUDIO_PLAYBACK_TASK_PRIORITY so it outranks the receiver tasks (see the
   * comment in audio_output.h). Keep that. The stack stays in internal RAM (see
   * spiram_task.h). */
  BaseType_t ok = xTaskCreatePinnedToCore(
      playback_task, "airplay_play", PLAYBACK_TASK_STACK, NULL,
      AUDIO_PLAYBACK_TASK_PRIORITY, &s_task, tskNO_AFFINITY);
  if (ok != pdPASS) {
    ESP_LOGE(TAG, "Failed to create playback task");
    s_running = false;
    s_task = NULL;
  }
}

void audio_output_stop(void) {
  s_running = false;
  /* The task notices within one HOST_WRITE_TIMEOUT_MS / 10 ms tick and
   * deletes itself. */
}

void audio_output_flush(void) {
  s_flush_requested = true;
}

esp_err_t audio_output_write(const void *data, size_t bytes, TickType_t wait) {
  /* Only used by the firmware's Bluetooth A2DP path, which is not compiled. */
  (void)data;
  (void)bytes;
  (void)wait;
  return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t audio_output_write_pcm(const void *data, size_t bytes,
                                 int32_t volume_q15, TickType_t wait) {
  /* Only used by the firmware's USB and Bluetooth sinks, which are not
   * compiled. Defined here so the weak default in audio_output_common.c,
   * which feeds led.c's VU meter, is not linked. */
  (void)data;
  (void)bytes;
  (void)volume_q15;
  (void)wait;
  return ESP_ERR_NOT_SUPPORTED;
}

void audio_output_set_sample_rate(uint32_t rate) {
  /* Only used by the firmware's Bluetooth path. ESPHome's speaker chain adapts
   * to whatever rate we pass with each write. */
  (void)rate;
}

void audio_output_set_source_rate(int rate) {
  if (rate > 0 && rate != s_source_rate) {
    ESP_LOGI(TAG, "Source sample rate %d Hz", rate);
    s_source_rate = rate;
    /* No resampling here: the host's resampler speaker converts to the
     * speaker rate. The cursor math below uses the source rate, which is
     * right as long as notify_audio_played() reports frames at the rate we
     * wrote them (check this if you ever see a non-44.1 kHz stream). */
  }
}

uint32_t audio_output_get_hardware_latency_us(void) {
  int64_t us = (int64_t)HOST_NOMINAL_LATENCY_US +
               __atomic_load_n(&s_output_delay_us, __ATOMIC_RELAXED);
  return us > 0 ? (uint32_t)us : 0;
}

bool audio_output_get_pipeline_us(int64_t *now_us, uint32_t *pipeline_us) {
  int64_t last_played_us = __atomic_load_n(&s_last_played_us, __ATOMIC_RELAXED);
  if (last_played_us == 0) {
    /* No feedback yet (host not playing): let the timing engine use the
     * modelled latency instead. */
    return false;
  }
  uint64_t submitted = __atomic_load_n(&s_submitted_frames, __ATOMIC_RELAXED);
  uint64_t played = __atomic_load_n(&s_played_frames, __ATOMIC_RELAXED);
  int64_t now = esp_timer_get_time();
  uint32_t rate = (uint32_t)s_source_rate;

  /* Frames that have left the wire since the last report. */
  int64_t since_us = now - last_played_us;
  if (since_us < 0) {
    since_us = 0;
  } else if (since_us > PLAYED_EXTRAPOLATE_MAX_US) {
    since_us = PLAYED_EXTRAPOLATE_MAX_US;
  }
  played += ((uint64_t)since_us * rate) / 1000000ULL;

  uint64_t queued = submitted > played ? submitted - played : 0;
  if (now_us != NULL) {
    *now_us = now;
  }
  if (pipeline_us != NULL) {
    int64_t us = (int64_t)((queued * 1000000ULL) / rate) +
                 __atomic_load_n(&s_output_delay_us, __ATOMIC_RELAXED) -
                 MAIN_PIPELINE_LATENCY_US;
    *pipeline_us = us > 0 ? (uint32_t)us : 0;
  }
  return true;
}
