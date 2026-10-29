/*
 * airplay_core.c — replaces main/main.c for the ESPHome build.
 *
 * The firmware's app_main() also brings up NVS, WiFi/Ethernet, a web UI, OTA,
 * LEDs, buttons, displays and the DAC. ESPHome owns all of that, so this file
 * keeps only the AirPlay bring-up sequence from start_airplay_services() and
 * connects the core to its host through airplay_core_host_t.
 *
 * Mirrors start_airplay_services() in main/main.c; keep the two in step.
 */

#include "airplay_core.h"
#include "airplay_core_internal.h"

#include <inttypes.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "airplay_sodium.h"

/* firmware headers (main/, components/dac/) */
#include "audio_output.h"
#include "audio_receiver.h"
#include "dac.h"
#include "dacp_client.h"
#include "hap.h"
#include "mdns_airplay.h"
#include "playback_control.h"
#include "playback_events.h"
#include "ptp_clock.h"
#include "rtsp_server.h"
#include "settings.h"

static const char *TAG = "airplay_core";

static airplay_core_config_t s_config;
static char s_name[65];
static char s_model[33] = "esphome-airplay";
static bool s_initialized = false;
static bool s_infra_started = false; /* PTP/HAP/receiver: once per boot */
static bool s_running = false;

const char *airplay_core_get_name(void) {
  return s_name;
}
const char *airplay_core_get_model(void) {
  return s_model;
}

size_t airplay_core_host_write(const int16_t *pcm, size_t frames,
                               uint32_t sample_rate, uint32_t timeout_ms) {
  if (s_config.host.write == NULL) {
    return 0;
  }
  return s_config.host.write(s_config.host.ctx, pcm, frames, sample_rate,
                             timeout_ms);
}

/* ---- volume: main/ calls dac_set_volume(dB) on every volume change ------ */

static void dac_set_volume_to_host(float volume_db) {
  if (s_config.host.on_volume != NULL) {
    s_config.host.on_volume(s_config.host.ctx, volume_db);
  }
}

static const dac_ops_t s_host_dac_ops = {
    .set_volume = dac_set_volume_to_host,
    /* init/deinit/power/speaker/line-out stay NULL = no-ops */
};

/* ---- session events --------------------------------------------------- */

/* The firmware's playback events are an aggregate over all inputs (AirPlay,
 * Bluetooth, USB, Sendspin). Only AirPlay is compiled here, so the aggregate
 * is the AirPlay session state. Listeners see transitions only: repeats are
 * dropped, and a PLAYING from idle arrives as CONNECTED then PLAYING. */
static void on_playback_event(playback_source_t source, playback_event_t event,
                              const playback_event_data_t *data,
                              void *user_data) {
  (void)source;
  (void)user_data;
  if (s_config.host.on_event == NULL) {
    return;
  }
  switch (event) {
  case PLAYBACK_EVENT_CONNECTED:
    s_config.host.on_event(s_config.host.ctx,
                           AIRPLAY_CORE_EVENT_CLIENT_CONNECTED, NULL);
    break;
  case PLAYBACK_EVENT_PLAYING:
    s_config.host.on_event(s_config.host.ctx, AIRPLAY_CORE_EVENT_PLAYING, NULL);
    break;
  case PLAYBACK_EVENT_PAUSED:
    s_config.host.on_event(s_config.host.ctx, AIRPLAY_CORE_EVENT_PAUSED, NULL);
    break;
  case PLAYBACK_EVENT_DISCONNECTED:
    s_config.host.on_event(s_config.host.ctx, AIRPLAY_CORE_EVENT_DISCONNECTED,
                           NULL);
    break;
  case PLAYBACK_EVENT_METADATA: {
    if (data == NULL) {
      break;
    }
    airplay_core_metadata_t md = {0};
    strncpy(md.title, data->metadata.title, sizeof(md.title) - 1);
    strncpy(md.artist, data->metadata.artist, sizeof(md.artist) - 1);
    strncpy(md.album, data->metadata.album, sizeof(md.album) - 1);
    md.duration_secs = data->metadata.duration_secs;
    md.position_secs = data->metadata.position_secs;
    s_config.host.on_event(s_config.host.ctx, AIRPLAY_CORE_EVENT_METADATA, &md);
    break;
  }
  default:
    break;
  }
}

/* ---- public API -------------------------------------------------------- */

esp_err_t airplay_core_init(const airplay_core_config_t *config) {
  if (s_initialized) {
    return ESP_OK;
  }
  ESP_RETURN_ON_FALSE(config != NULL && config->name != NULL,
                      ESP_ERR_INVALID_ARG, TAG, "config and name required");

  s_config = *config;
  strncpy(s_name, config->name, sizeof(s_name) - 1);
  s_config.name = s_name;
  if (config->model != NULL && config->model[0] != '\0') {
    strncpy(s_model, config->model, sizeof(s_model) - 1);
    s_model[sizeof(s_model) - 1] = '\0';
  }
  s_config.model = s_model;

  /* NVS is already initialised by ESPHome. The firmware keeps its own data
   * (pairing keys, volume, name) in the "airplay" NVS namespace, which does
   * not collide with ESPHome's. */
  /* Before anything in main/ can ask for the device id (settings, HAP, mDNS).
   */
  airplay_core_set_device_mac(config->device_mac);

  /* Before hap_init() calls sodium_init(): ESPHome's libsodium port would
   * otherwise read /dev/urandom, which ESP-IDF lacks, and abort the firmware
   * (see airplay_sodium/randombytes_esp32.c). */
  airplay_sodium_use_esp32_random();

  ESP_RETURN_ON_ERROR(settings_init(), TAG, "settings_init failed");

  /* AirPlay 1 or 2 is a firmware setting in NVS (its web UI sets it), and
   * settings_init() is the only place that applies it. Store the YAML choice
   * only when it differs, then load it again. Must happen before anything
   * reads settings_airplay_v1(): RTSP port, mDNS records, /info. */
  if (settings_airplay_v1() != config->airplay_v1) {
    ESP_RETURN_ON_ERROR(settings_set_airplay_v1(config->airplay_v1), TAG,
                        "settings_set_airplay_v1 failed");
    ESP_RETURN_ON_ERROR(settings_init(), TAG, "settings_init failed");
  }

  /* rtsp_handlers.c (/info) and mdns read the name from settings. Only write
   * when it changed, to avoid a flash write on every boot. */
  char stored[sizeof(s_name)] = {0};
  if (settings_get_device_name(stored, sizeof(stored)) != ESP_OK ||
      strcmp(stored, s_name) != 0) {
    settings_set_device_name(s_name);
  }

  ESP_RETURN_ON_ERROR(playback_control_init(), TAG,
                      "playback_control_init failed");

  dac_register(&s_host_dac_ops);
  dac_init(NULL);

  airplay_output_configure(config->output_delay_us);

  if (playback_events_register(on_playback_event, NULL) != 0) {
    ESP_LOGE(TAG, "playback_events_register failed (listener table full)");
    return ESP_FAIL;
  }

  s_initialized = true;
  return ESP_OK;
}

esp_err_t airplay_core_start(void) {
  ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG,
                      "airplay_core_init() first");
  if (s_running) {
    return ESP_OK;
  }

  /* Mirrors start_airplay_services() in main/main.c. */
  if (!s_infra_started) {
    ESP_RETURN_ON_ERROR(ptp_clock_init(), TAG, "ptp_clock_init failed");
    ESP_RETURN_ON_ERROR(hap_init(), TAG, "hap_init failed");
    ESP_RETURN_ON_ERROR(audio_receiver_init(), TAG,
                        "audio_receiver_init failed");
    ESP_RETURN_ON_ERROR(audio_output_init(), TAG, "audio_output_init failed");
    mdns_airplay_init(); /* our version: see mdns_airplay_esphome.c */
    s_infra_started = true;
  }

  audio_output_start();
  ESP_RETURN_ON_ERROR(rtsp_server_start(), TAG, "rtsp_server_start failed");
  s_running = true;
  /* As main.c does: route playback_control's play/pause/volume to AirPlay. */
  playback_control_set_source(PLAYBACK_SOURCE_AIRPLAY);
  ESP_LOGI(TAG, "AirPlay %s receiver '%s' started",
           settings_airplay_v1() ? "1" : "2", s_name);
  return ESP_OK;
}

void airplay_core_stop(void) {
  if (!s_running) {
    return;
  }
  rtsp_server_stop();
  audio_output_stop();
  playback_control_set_source(PLAYBACK_SOURCE_NONE);
  s_running = false;
}

void airplay_core_set_output_active(bool active) {
  airplay_output_set_active(active);
}

void airplay_core_reset_output_cursor(void) {
  airplay_output_reset_cursor();
}

void airplay_core_notify_played(uint32_t frames, int64_t timestamp_us) {
  airplay_output_notify_played(frames, timestamp_us);
}

void airplay_core_set_output_delay_us(int32_t output_delay_us) {
  airplay_output_configure(output_delay_us);
}

/* TODO: rtsp_server.c has no public "drop the current client" call; the
 * client slots are static in rtsp_server.c. Restarting the server works
 * but also closes the listening socket for a moment. Consider adding a
 * small rtsp_server_disconnect_clients() to rtsp_server.c (it would reuse
 * signal_old_client_stop()). */
static void drop_client_now(void) {
  rtsp_server_stop();
  if (rtsp_server_start() != ESP_OK) {
    ESP_LOGE(TAG, "rtsp_server_start failed after disconnect");
  }
}

/* An AirPlay 1 sender streams over UDP and does not notice a dropped RTSP
 * connection: the iPhone keeps showing "playing" (seen on hardware
 * 2026-09-29). So ask it to pause over DACP. It then ends the session itself
 * (FLUSH + TEARDOWN, as for a pause from its own UI) and shows "paused".
 * Drop the connection anyway if it hasn't within DACP_RELEASE_TIMEOUT_MS.
 *
 * dacp_client.c only offers the playpause toggle, so this runs only while the
 * session is playing; on a paused sender it would resume playback. It runs in
 * its own task: the DACP request goes out on dacp_client.c's worker task, and
 * clearing the session (which dropping the connection does) before that would
 * make the worker skip it. */
#define DACP_RELEASE_TIMEOUT_MS 3000
#define DACP_RELEASE_POLL_MS    100
#define DACP_RELEASE_TASK_STACK 4096 /* internal RAM, like main/'s tasks */

static volatile bool s_release_pending = false;

static void dacp_release_task(void *arg) {
  (void)arg;
  dacp_send_playpause();
  int waited_ms = 0;
  while (waited_ms < DACP_RELEASE_TIMEOUT_MS && playback_events_any_playing()) {
    vTaskDelay(pdMS_TO_TICKS(DACP_RELEASE_POLL_MS));
    waited_ms += DACP_RELEASE_POLL_MS;
  }
  if (!playback_events_any_playing()) {
    ESP_LOGI(TAG, "AirPlay 1 sender paused after %d ms (DACP)", waited_ms);
  } else if (s_running) {
    ESP_LOGW(TAG, "AirPlay 1 sender still playing after %d ms; dropping it",
             waited_ms);
    drop_client_now();
  }
  s_release_pending = false;
  vTaskDelete(NULL);
}

void airplay_core_disconnect_client(void) {
  if (!s_running) {
    return;
  }
  if (dacp_is_active() && playback_events_any_playing()) {
    if (s_release_pending) {
      return;
    }
    s_release_pending = true;
    if (xTaskCreate(dacp_release_task, "airplay_release",
                    DACP_RELEASE_TASK_STACK, NULL, 5, NULL) == pdPASS) {
      return;
    }
    s_release_pending = false;
    ESP_LOGE(TAG, "Could not start the DACP release task");
  }
  drop_client_now();
}

bool airplay_core_remote_command(airplay_core_remote_t command) {
  if (!dacp_is_active()) {
    return false; /* AirPlay 2 session: no DACP, and MRP is not implemented */
  }
  switch (command) {
  case AIRPLAY_CORE_REMOTE_PLAY_PAUSE:
    dacp_send_playpause();
    return true;
  case AIRPLAY_CORE_REMOTE_NEXT:
    dacp_send_next();
    return true;
  case AIRPLAY_CORE_REMOTE_PREVIOUS:
    dacp_send_prev();
    return true;
  default:
    return false;
  }
}

/* ---- clock diagnostics --------------------------------------------------- */

/* Diagnostics for the PTP clock the timing engine follows.
 * A click test on 2026-09-29 (old iPhone as sender, grouped with an Apple TV)
 * drifted 18 ppm while the engine saw no error. Suspected: the PTP offset
 * stopped updating after SETPEERS changed the group. This line shows it:
 *   syncs/followups  SYNC/FOLLOW_UP messages accepted since the last line.
 *                    ptp_clock.c drops messages from any clock other than the
 *                    expected master, and does not count those, so +0 while
 *                    playing means the master went quiet or was replaced.
 *   rate             change of the filtered offset (master - local) over the
 *                    interval. It follows the ESP crystal against the master,
 *                    so it should be a steady non-zero value (tens of ppm).
 *                    Exactly 0 with +0 syncs means the offset is frozen.
 *                    Negative: the ESP's clock runs fast.
 *   raw-filt         last raw sample minus the filtered offset.
 * ptp_clock.c is not touched; everything comes from ptp_clock.h. Runs on
 * the playback task only, so the static state needs no locking. */
void airplay_core_log_clock_status(void) {
  static bool s_have_prev = false;
  static uint64_t s_prev_master;
  static ptp_stats_t s_prev;
  static int64_t s_prev_us;

  ptp_stats_t st;
  ptp_clock_get_stats(&st);
  const uint64_t master = ptp_clock_get_master_clock_id();
  const bool locked = ptp_clock_is_locked();
  const int64_t now_us = esp_timer_get_time();

  /* Deltas only make sense against the same master and without a reset in
   * between (ptp_clock_set_master_clock_id() / ptp_clock_clear()). */
  const bool comparable = s_have_prev && master == s_prev_master &&
                          st.sync_count >= s_prev.sync_count &&
                          now_us > s_prev_us;
  if (comparable) {
    const double rate_ppm =
        (double)(st.filtered_offset_ns - s_prev.filtered_offset_ns) * 1000.0 /
        (double)(now_us - s_prev_us);
    ESP_LOGI(TAG,
             "clock: master=%016llx locked=%d syncs=+%" PRIu32
             " followups=+%" PRIu32 " rate=%+.1f ppm raw-filt=%+lld us"
             " outliers=+%" PRIu32 " locked_for=%" PRIu32 " s",
             (unsigned long long)master, locked,
             st.sync_count - s_prev.sync_count,
             st.followup_count - s_prev.followup_count, rate_ppm,
             (long long)((st.last_offset_ns - st.filtered_offset_ns) / 1000),
             st.outlier_count - s_prev.outlier_count, st.lock_time_ms / 1000);
  } else {
    ESP_LOGI(TAG,
             "clock: master=%016llx locked=%d syncs=%" PRIu32
             " followups=%" PRIu32 " (new master or reset; rates next line)",
             (unsigned long long)master, locked, st.sync_count,
             st.followup_count);
  }

  s_have_prev = true;
  s_prev_master = master;
  s_prev = st;
  s_prev_us = now_us;
}
