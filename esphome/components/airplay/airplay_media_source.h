#pragma once

#include "esphome/core/defines.h"

#ifdef USE_ESP32

#include "esphome/components/audio/audio.h"
#include "esphome/components/media_source/media_source.h"
#include "esphome/core/component.h"

#include <airplay_core.h>

#include <atomic>
#include <cstdint>
#include <string>

namespace esphome::airplay {

/// @brief AirPlay 2 receiver as an ESPHome media source.
///
/// A "smart" source in the MediaSource sense: it starts itself. When an
/// iPhone begins streaming, it asks the speaker_source media player to play
/// "airplay://current" (request_play_uri_), exactly the way the Sendspin
/// media source does with "sendspin://current". The player stops whatever
/// else is playing and calls play_uri() on us.
///
/// Reference implementation for every pattern in here:
///   esphome/components/sendspin/media_source/sendspin_media_source.cpp
///
/// Thread contexts (as in the Sendspin source):
///   - main loop: play_uri, handle_command, notify_volume/mute_changed, loop
///   - airplay_core playback task: on_core_write_ (calls write_output)
///   - airplay_core RTSP task: on_core_event_, on_core_volume_
///   - speaker callback task: notify_audio_played
class AirPlayMediaSource final : public Component, public media_source::MediaSource {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  /// After ESPHome's mdns component (AFTER_CONNECTION), which owns mdns_init().
  float get_setup_priority() const override { return setup_priority::AFTER_CONNECTION - 1.0f; }

  void set_advertised_name(const std::string &name) { this->advertised_name_ = name; }
  void set_model(const std::string &model) { this->model_ = model; }
  /// From codegen, and at runtime from the airplay number platform (main loop).
  void set_output_delay_us(int32_t delay_us) {
    this->output_delay_us_ = delay_us;
    airplay_core_set_output_delay_us(delay_us);
  }
  int32_t get_output_delay_us() const { return this->output_delay_us_; }
  void set_airplay_1_only(bool airplay_1_only) { this->airplay_1_only_ = airplay_1_only; }

  // --- MediaSource interface ---
  bool play_uri(const std::string &uri) override;
  void handle_command(media_source::MediaSourceCommand command) override;
  bool can_handle(const std::string &uri) const override;
  /// Like Sendspin: we own transport (the sender does), so the player routes
  /// next/prev/stop to us instead of running its own playlist.
  bool has_internal_playlist() const override { return true; }

  void notify_volume_changed(float volume) override;
  void notify_mute_changed(bool is_muted) override;
  void notify_audio_played(uint32_t frames, int64_t timestamp) override;

 protected:
  // --- callbacks from airplay_core (C trampolines -> these) ---
  size_t on_core_write_(const int16_t *pcm, size_t frames, uint32_t sample_rate, uint32_t timeout_ms);
  void on_core_event_(airplay_core_event_t event, const airplay_core_metadata_t *metadata);
  void on_core_volume_(float volume_db);

  static size_t core_write_trampoline_(void *ctx, const int16_t *pcm, size_t frames, uint32_t sample_rate,
                                       uint32_t timeout_ms);
  static void core_event_trampoline_(void *ctx, airplay_core_event_t event, const airplay_core_metadata_t *metadata);
  static void core_volume_trampoline_(void *ctx, float volume_db);

  /// Events arrive on core tasks; loop() applies them on the main loop,
  /// because set_state_() must only run there.
  enum PendingEvent : uint32_t {
    EVENT_PLAYING = 1 << 0,
    EVENT_PAUSED = 1 << 1,
    EVENT_DISCONNECTED = 1 << 2,
    EVENT_CONNECTED = 1 << 3,
    EVENT_VOLUME = 1 << 4,
  };
  std::atomic<uint32_t> pending_events_{0};

  /// The sender's volume (dB) as last received on the RTSP task.
  std::atomic<float> sender_volume_db_{0.0f};
  /// Set when a sender volume arrived that is not applied yet. It is applied
  /// only while AirPlay is the active source (PLAYING/PAUSED), so a sender
  /// that merely connects (the Apple TV, Music Assistant's AirPlay provider)
  /// cannot change the volume of whatever else is playing. Main loop only.
  bool sender_volume_pending_{false};
  void apply_pending_sender_volume_();

  /// Keep WiFi out of power save while a sender is connected, as Sendspin does
  /// while it streams. Realtime AirPlay audio and the PTP/NTP timing packets
  /// are UDP; with the modem dozing between beacons they arrive late or not at
  /// all. Main loop only.
  void set_network_high_performance_(bool enable);
  bool network_high_performance_{false};

  std::string advertised_name_;
  std::string model_;
  int32_t output_delay_us_{0};
  bool airplay_1_only_{false};
  /// AirPlay device id; the same MAC ESPHome's Sendspin hub reports.
  uint8_t device_mac_[6]{};

  bool core_started_{false};
  /// Set when we asked the player to start us and play_uri() has not come
  /// yet. Same dedup/race guard as SendspinMediaSource::pending_start_: the
  /// player may send STOP to a busy source before calling play_uri().
  bool pending_start_{false};
  /// Set by play_uri(); loop() then resets the output cursor and starts
  /// writing (see the comment in play_uri()).
  bool start_output_pending_{false};
};

}  // namespace esphome::airplay

#endif  // USE_ESP32
