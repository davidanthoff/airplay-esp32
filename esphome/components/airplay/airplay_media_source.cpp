#include "airplay_media_source.h"

#ifdef USE_ESP32

#include "esphome/components/network/util.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#ifdef USE_ETHERNET
#include "esphome/components/ethernet/ethernet_component.h"
#endif
#ifdef USE_WIFI
#include "esphome/components/wifi/wifi_component.h"
#endif

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>
#include <cinttypes>

namespace esphome::airplay {

static const char *const TAG = "airplay.media_source";

static constexpr char URI_PREFIX[] = "airplay://";
static constexpr char URI_CURRENT[] = "airplay://current";

// AirPlay volume: -30 dB (slider at bottom) .. 0 dB (top); -144 dB = mute.
static constexpr float AIRPLAY_VOLUME_MIN_DB = -30.0f;
static constexpr float AIRPLAY_VOLUME_MUTE_DB = -144.0f;

// The MAC ESPHome's Sendspin hub reports as its client id
// (SendspinHub::get_client_id_into_buffer, 2026.9): the Ethernet MAC whenever
// `ethernet:` is configured, the base MAC otherwise. AirPlay advertises the same
// one, so Music Assistant links the AirPlay and the Sendspin player of this
// device into one player instead of showing two.
static void get_identity_mac(uint8_t mac[6]) {
#ifdef USE_ETHERNET
  if (ethernet::global_eth_component != nullptr) {
    ethernet::global_eth_component->get_eth_mac_address_raw(mac);
    return;
  }
#endif
  get_mac_address_raw(mac);
}

void AirPlayMediaSource::setup() {
  airplay_core_config_t config{};
  config.name = this->advertised_name_.c_str();
  config.model = this->model_.c_str();
  config.output_delay_us = this->output_delay_us_;
  config.airplay_v1 = this->airplay_1_only_;
  get_identity_mac(config.device_mac);
  std::copy(std::begin(config.device_mac), std::end(config.device_mac), this->device_mac_);
  config.host.ctx = this;
  config.host.write = &AirPlayMediaSource::core_write_trampoline_;
  config.host.on_event = &AirPlayMediaSource::core_event_trampoline_;
  config.host.on_volume = &AirPlayMediaSource::core_volume_trampoline_;

  esp_err_t err = airplay_core_init(&config);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "airplay_core_init failed: %s", esp_err_to_name(err));
    this->mark_failed();
    return;
  }
  // The core itself starts in loop() once the network is up.
}

void AirPlayMediaSource::dump_config() {
  ESP_LOGCONFIG(TAG,
                "AirPlay Media Source:\n"
                "  Advertised name: %s\n"
                "  Model: %s\n"
                "  Device id (MAC): %02X:%02X:%02X:%02X:%02X:%02X\n"
                "  Output delay: %" PRId32 " us\n"
                "  AirPlay 1 only: %s",
                this->advertised_name_.c_str(), this->model_.c_str(), this->device_mac_[0], this->device_mac_[1], this->device_mac_[2],
                this->device_mac_[3], this->device_mac_[4], this->device_mac_[5], this->output_delay_us_,
                YESNO(this->airplay_1_only_));
}

// THREAD CONTEXT: main loop
void AirPlayMediaSource::loop() {
  if (!this->core_started_) {
    if (!network::is_connected()) {
      return;
    }
    esp_err_t err = airplay_core_start();
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "airplay_core_start failed: %s", esp_err_to_name(err));
      this->mark_failed();
      return;
    }
    this->core_started_ = true;
    // TODO: handle network loss / IP change. ESP-IDF mdns follows netif
    // changes by itself; check whether the RTSP server and PTP sockets survive
    // an Ethernet unplug/replug and restart the core if not.
  }

  if (this->start_output_pending_) {
    // play_uri() made us the active source, and the player reset its
    // pending-frame counter right after play_uri() returned. Only now start
    // the latency accounting and the writes, so that every frame we count as
    // submitted is also one the player will report back as played.
    this->start_output_pending_ = false;
    airplay_core_reset_output_cursor();
    if (this->get_state() == media_source::MediaSourceState::PLAYING) {
      airplay_core_set_output_active(true);
    }
  }

  uint32_t events = this->pending_events_.exchange(0, std::memory_order_acq_rel);
  if (events & EVENT_VOLUME) {
    this->sender_volume_pending_ = true;
  }
  if (events == 0 || events == EVENT_VOLUME) {
    this->apply_pending_sender_volume_();
    return;
  }

  if (events & EVENT_DISCONNECTED) {
    this->set_network_high_performance_(false);
    if (!(events & (EVENT_CONNECTED | EVENT_PLAYING))) {
      // That session is over; don't carry its volume into the next one.
      this->sender_volume_pending_ = false;
    }
  }
  // A connect or play in the same batch as a disconnect is from a newer session.
  if (events & (EVENT_CONNECTED | EVENT_PLAYING)) {
    this->set_network_high_performance_(true);
  }

  if (events & EVENT_DISCONNECTED) {
    airplay_core_set_output_active(false);
    this->pending_start_ = false;
    if (this->get_state() != media_source::MediaSourceState::IDLE) {
      this->set_state_(media_source::MediaSourceState::IDLE);
    }
    // A PLAYING that arrived in the same batch is from a newer session; fall through.
    events &= ~EVENT_PAUSED;
  }

  if (events & EVENT_PAUSED) {
    if (this->get_state() == media_source::MediaSourceState::PLAYING) {
      // TODO: decide pause behaviour. Current choice: stop feeding the
      // speaker and show PAUSED. Alternative: keep PLAYING and let the core
      // write silence (keeps the speaker chain warm, faster resume).
      airplay_core_set_output_active(false);
      this->set_state_(media_source::MediaSourceState::PAUSED);
    }
  }

  if (events & EVENT_PLAYING) {
    switch (this->get_state()) {
      case media_source::MediaSourceState::IDLE:
        if (!this->pending_start_) {
          // Ask the player to start us. It stops the active source (e.g.
          // Sendspin) first, then calls play_uri().
          this->pending_start_ = true;
          this->request_play_uri_(URI_CURRENT);
        }
        break;
      case media_source::MediaSourceState::PAUSED:
        airplay_core_set_output_active(true);
        this->set_state_(media_source::MediaSourceState::PLAYING);
        break;
      default:
        break;
    }
  }

  this->apply_pending_sender_volume_();
}

// THREAD CONTEXT: main loop
void AirPlayMediaSource::apply_pending_sender_volume_() {
  if (!this->sender_volume_pending_) {
    return;
  }
  auto state = this->get_state();
  if (state != media_source::MediaSourceState::PLAYING && state != media_source::MediaSourceState::PAUSED) {
    // Not our turn: another source (Sendspin, HTTP) may be playing, or the
    // sender only connected. Keep the value; it applies when we start playing.
    return;
  }
  this->sender_volume_pending_ = false;
  float volume_db = this->sender_volume_db_.load(std::memory_order_relaxed);
  if (volume_db <= AIRPLAY_VOLUME_MUTE_DB + 1.0f) {
    this->request_mute_(true);
    return;
  }
  // AirPlay's dB value is linear in slider position, so map it linearly.
  // The player then applies its own volume_min/volume_max.
  float volume = std::clamp((volume_db - AIRPLAY_VOLUME_MIN_DB) / -AIRPLAY_VOLUME_MIN_DB, 0.0f, 1.0f);
  this->request_mute_(false);
  this->request_volume_(volume);
}

// --- MediaSource interface ---

bool AirPlayMediaSource::can_handle(const std::string &uri) const { return uri.starts_with(URI_PREFIX); }

// THREAD CONTEXT: main loop
bool AirPlayMediaSource::play_uri(const std::string &uri) {
  this->pending_start_ = false;
  if (!this->is_ready() || this->is_failed() || !this->has_listener()) {
    return false;
  }
  if (this->get_state() != media_source::MediaSourceState::IDLE) {
    ESP_LOGE(TAG, "Cannot play '%s': source is busy", uri.c_str());
    return false;
  }
  if (uri != URI_CURRENT) {
    // Nothing else makes sense: AirPlay is pushed by the sender, it cannot be
    // pulled by URL. (A user calling media_player.play_media with an airplay://
    // URL lands here.)
    ESP_LOGW(TAG, "Only '%s' is supported, got '%s'", URI_CURRENT, uri.c_str());
    return false;
  }

  // TODO: if the sender disconnected between request_play_uri_() and now,
  // we would start an empty session. The core then writes silence until the
  // next session; acceptable for now, but consider checking a "session active"
  // flag from the core here.
  //
  // Output starts in the next loop(), not here: set_state_(PLAYING) makes us
  // the player's active source immediately, but the player only resets its
  // pending-frame counter after we return. Frames written in between would
  // never be reported as played and would read as queued forever, making all
  // later audio play early.
  this->start_output_pending_ = true;
  this->set_state_(media_source::MediaSourceState::PLAYING);
  return true;
}

// THREAD CONTEXT: main loop
void AirPlayMediaSource::handle_command(media_source::MediaSourceCommand command) {
  switch (command) {
    case media_source::MediaSourceCommand::STOP:
      if (this->pending_start_) {
        // The player may STOP before play_uri() while switching sources. Same
        // guard as the Sendspin source.
        return;
      }
      ESP_LOGD(TAG, "STOP: releasing the speaker and dropping the AirPlay session");
      airplay_core_set_output_active(false);
      // The sender will show the speaker as disconnected. Without this the
      // iPhone would keep streaming into a source nobody listens to.
      airplay_core_disconnect_client();
      if (this->get_state() != media_source::MediaSourceState::IDLE) {
        this->set_state_(media_source::MediaSourceState::IDLE);
      }
      break;
    case media_source::MediaSourceCommand::PLAY:
    case media_source::MediaSourceCommand::PAUSE:
      if (!airplay_core_remote_command(AIRPLAY_CORE_REMOTE_PLAY_PAUSE)) {
        ESP_LOGW(TAG, "Play/pause from ESPHome needs an AirPlay 1 session (no MRP support for AirPlay 2)");
      }
      break;
    case media_source::MediaSourceCommand::NEXT:
      airplay_core_remote_command(AIRPLAY_CORE_REMOTE_NEXT);
      break;
    case media_source::MediaSourceCommand::PREVIOUS:
      airplay_core_remote_command(AIRPLAY_CORE_REMOTE_PREVIOUS);
      break;
    default:
      break;  // repeat/shuffle/playlist commands have no AirPlay equivalent
  }
}

// THREAD CONTEXT: main loop (orchestrator -> source)
void AirPlayMediaSource::notify_volume_changed(float volume) {
  // TODO: AirPlay 2 has no way to push a volume change back to the sender
  // (needs MRP). With AirPlay 1 we could use DACP (dacp_send_volume). For now
  // the iPhone slider and the ESPHome volume can drift apart when changed
  // from Home Assistant.
  (void)volume;
}

void AirPlayMediaSource::notify_mute_changed(bool is_muted) { (void)is_muted; }

// THREAD CONTEXT: speaker playback callback task. Must stay cheap.
void AirPlayMediaSource::notify_audio_played(uint32_t frames, int64_t timestamp) {
  airplay_core_notify_played(frames, timestamp);
}

// --- airplay_core callbacks ---

// THREAD CONTEXT: airplay_core playback task. May block up to timeout_ms.
size_t AirPlayMediaSource::on_core_write_(const int16_t *pcm, size_t frames, uint32_t sample_rate,
                                          uint32_t timeout_ms) {
  if (!this->has_listener() || this->get_state() != media_source::MediaSourceState::PLAYING) {
    vTaskDelay(pdMS_TO_TICKS(timeout_ms));
    return 0;
  }
  audio::AudioStreamInfo stream_info(16, 2, sample_rate);
  const size_t bytes = stream_info.frames_to_bytes(frames);
  size_t written = this->write_output(reinterpret_cast<const uint8_t *>(pcm), bytes, timeout_ms, stream_info);
  return stream_info.bytes_to_frames(written);
}

// THREAD CONTEXT: airplay_core RTSP task
void AirPlayMediaSource::on_core_event_(airplay_core_event_t event, const airplay_core_metadata_t *metadata) {
  switch (event) {
    case AIRPLAY_CORE_EVENT_CLIENT_CONNECTED:
      this->pending_events_.fetch_or(EVENT_CONNECTED, std::memory_order_acq_rel);
      break;
    case AIRPLAY_CORE_EVENT_PLAYING:
      this->pending_events_.fetch_or(EVENT_PLAYING, std::memory_order_acq_rel);
      break;
    case AIRPLAY_CORE_EVENT_PAUSED:
      this->pending_events_.fetch_or(EVENT_PAUSED, std::memory_order_acq_rel);
      break;
    case AIRPLAY_CORE_EVENT_DISCONNECTED:
      this->pending_events_.fetch_or(EVENT_DISCONNECTED, std::memory_order_acq_rel);
      break;
    case AIRPLAY_CORE_EVENT_METADATA:
      // TODO: expose title/artist/album (text_sensor platform, like
      // sendspin's). Copy into a mutex-protected struct here, publish in loop().
      // Progress updates arrive as METADATA events with empty text fields.
      if (metadata != nullptr && (metadata->artist[0] != '\0' || metadata->title[0] != '\0')) {
        ESP_LOGD(TAG, "Now playing: %s - %s", metadata->artist, metadata->title);
      }
      break;
    default:
      break;
  }
}

// THREAD CONTEXT: airplay_core RTSP task. Only record the value; loop() decides
// on the main loop whether it applies (apply_pending_sender_volume_()).
void AirPlayMediaSource::on_core_volume_(float volume_db) {
  this->sender_volume_db_.store(volume_db, std::memory_order_relaxed);
  this->pending_events_.fetch_or(EVENT_VOLUME, std::memory_order_acq_rel);
}

// THREAD CONTEXT: main loop
void AirPlayMediaSource::set_network_high_performance_(bool enable) {
  if (enable == this->network_high_performance_) {
    return;
  }
  this->network_high_performance_ = enable;
  ESP_LOGD(TAG, "%s WiFi high-performance mode", enable ? "Requesting" : "Releasing");
#ifdef USE_WIFI
  if (wifi::global_wifi_component == nullptr) {
    return;
  }
#ifdef USE_WIFI_RUNTIME_POWER_SAVE
  if (enable) {
    wifi::global_wifi_component->request_high_performance();
  } else {
    wifi::global_wifi_component->release_high_performance();
  }
#endif
#ifdef USE_WIFI_RUNTIME_ROAMING_SUPPRESSION
  if (enable) {
    wifi::global_wifi_component->request_roaming_suppression();
  } else {
    wifi::global_wifi_component->release_roaming_suppression();
  }
#endif
#endif  // USE_WIFI
}

// --- trampolines ---

size_t AirPlayMediaSource::core_write_trampoline_(void *ctx, const int16_t *pcm, size_t frames, uint32_t sample_rate,
                                                  uint32_t timeout_ms) {
  return static_cast<AirPlayMediaSource *>(ctx)->on_core_write_(pcm, frames, sample_rate, timeout_ms);
}

void AirPlayMediaSource::core_event_trampoline_(void *ctx, airplay_core_event_t event,
                                                const airplay_core_metadata_t *metadata) {
  static_cast<AirPlayMediaSource *>(ctx)->on_core_event_(event, metadata);
}

void AirPlayMediaSource::core_volume_trampoline_(void *ctx, float volume_db) {
  static_cast<AirPlayMediaSource *>(ctx)->on_core_volume_(volume_db);
}

}  // namespace esphome::airplay

#endif  // USE_ESP32
