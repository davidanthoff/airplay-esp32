#pragma once

/**
 * airplay_core — the boundary between the airplay-esp32 protocol
 * stack (plain C, ESP-IDF) and the ESPHome media source (C++).
 *
 * The core knows nothing about ESPHome. It talks to its host through the
 * callbacks in airplay_core_host_t, and the host drives it through the
 * functions below. Everything in this header is safe to call from C++.
 *
 * Threading:
 *   - host.write() is called from the core's playback task.
 *   - host.on_event() and host.on_volume() are called from core tasks
 *     (RTSP connection task). The host must marshal to its main loop.
 *   - airplay_core_notify_played() is called from the host's speaker
 *     callback task and must stay cheap (it only updates atomics).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  AIRPLAY_CORE_EVENT_CLIENT_CONNECTED = 0,
  AIRPLAY_CORE_EVENT_PLAYING,
  AIRPLAY_CORE_EVENT_PAUSED,
  AIRPLAY_CORE_EVENT_DISCONNECTED,
  AIRPLAY_CORE_EVENT_METADATA,
} airplay_core_event_t;

#define AIRPLAY_CORE_METADATA_STRING_MAX 64

typedef struct {
  char title[AIRPLAY_CORE_METADATA_STRING_MAX];
  char artist[AIRPLAY_CORE_METADATA_STRING_MAX];
  char album[AIRPLAY_CORE_METADATA_STRING_MAX];
  uint32_t duration_secs;
  uint32_t position_secs;
} airplay_core_metadata_t;

typedef enum {
  AIRPLAY_CORE_REMOTE_PLAY_PAUSE = 0,
  AIRPLAY_CORE_REMOTE_NEXT,
  AIRPLAY_CORE_REMOTE_PREVIOUS,
} airplay_core_remote_t;

typedef struct {
  /** Opaque pointer handed back to every callback. */
  void *ctx;

  /**
   * Push interleaved stereo signed 16-bit PCM into the host's audio pipeline.
   * @param frames       number of stereo frames in pcm
   * @param sample_rate  sample rate of pcm (44100 for all AirPlay audio today)
   * @param timeout_ms   how long the host may block waiting for space
   * @return frames accepted. 0 means "not ready, try the same data again".
   */
  size_t (*write)(void *ctx, const int16_t *pcm, size_t frames,
                  uint32_t sample_rate, uint32_t timeout_ms);

  /** Session state change. metadata is non-NULL only for METADATA events. */
  void (*on_event)(void *ctx, airplay_core_event_t event,
                   const airplay_core_metadata_t *metadata);

  /**
   * The sender changed the volume. AirPlay volume is in dB: -30.0 (quietest)
   * to 0.0 (full); -144.0 means mute.
   */
  void (*on_volume)(void *ctx, float volume_db);
} airplay_core_host_t;

typedef struct {
  /** Name shown in the iPhone's AirPlay picker (UTF-8). */
  const char *name;

  /**
   * Model advertised in mDNS ("model", "am"); NULL: "esphome-airplay".
   * "AudioAccessory5,1" makes iOS and Music Assistant treat the receiver as
   * a HomePod mini (main/'s default); Music Assistant then routes volume
   * to AirPlay even while Sendspin plays. main/'s /info reply keeps
   * "AudioAccessory5,1" regardless (hard-coded there).
   */
  const char *model;

  /**
   * Fixed delay, in microseconds, that the audio chain adds AFTER the point
   * where the host reports frames as played (e.g. a TOSLINK receiver or AV
   * amplifier). The timing engine plays that much earlier to compensate.
   * Negative values make it play later (fine-tuning against other speakers).
   */
  int32_t output_delay_us;

  /**
   * Advertise and speak AirPlay 1 (classic RAOP) only: no HomeKit pairing,
   * no multi-room sync, but DACP remote control works. The RTSP server then
   * listens on port 5000 instead of 7000. Stored in the firmware's NVS
   * settings; a change is written once, on the next boot with the new value.
   */
  bool airplay_v1;

  /**
   * MAC address used as the AirPlay device id (mDNS "deviceid", /info, HAP).
   * All zero: the ESP32's Wi-Fi station MAC, like the firmware. The ESPHome
   * component passes the MAC its Sendspin hub reports, so that Music
   * Assistant can recognise both as the same device.
   */
  uint8_t device_mac[6];

  airplay_core_host_t host;
} airplay_core_config_t;

/**
 * One-time initialisation: settings/NVS, volume hook, RTSP event listener.
 * Does not touch the network. Call from the host's setup().
 */
esp_err_t airplay_core_init(const airplay_core_config_t *config);

/**
 * Start PTP, HAP, the audio receiver/output, mDNS advertisement and the RTSP
 * server. Call once the network is up and the ESPHome mdns component has run
 * its setup (it owns mdns_init()).
 */
esp_err_t airplay_core_start(void);

/** Stop the RTSP server and the playback task. */
void airplay_core_stop(void);

/**
 * Tell the core whether the host is currently routing its audio to the
 * speaker. While inactive, the playback task does not pull audio from the
 * timing engine.
 */
void airplay_core_set_output_active(bool active);

/**
 * Start the output latency accounting from zero. Call when the host starts a
 * new playback and has reset its own played-frame bookkeeping (ESPHome's
 * speaker_source resets pending_frames right after play_uri() returns), and
 * before any audio of the new playback is written. Not for pause/resume.
 */
void airplay_core_reset_output_cursor(void);

/**
 * Report frames that finished playing, forwarded from ESPHome's
 * MediaSource::notify_audio_played(). This is the feedback that lets the
 * timing engine know the real output latency.
 *
 * @param timestamp_us esp_timer_get_time() when the frames finished playing
 */
void airplay_core_notify_played(uint32_t frames, int64_t timestamp_us);

/**
 * Change output_delay_us (see airplay_core_config_t) at runtime. Any thread,
 * any time, also before airplay_core_init(). The timing engine picks the new
 * value up on its next latency query and corrects for the step like for any
 * other timing error.
 */
void airplay_core_set_output_delay_us(int32_t output_delay_us);

/**
 * Drop the current AirPlay session (e.g. because Sendspin took over the
 * speaker). An AirPlay 2 sender sees the speaker disconnect. A playing
 * AirPlay 1 sender is asked to pause over DACP first, so it ends the session
 * itself and shows "paused"; the connection is dropped after 3 s if it
 * hasn't. Returns immediately in both cases.
 */
void airplay_core_disconnect_client(void);

/**
 * Send a transport command back to the sender. Works only for AirPlay 1
 * (DACP). AirPlay 2 would need MRP, which the firmware does not implement.
 * @return true if the command was sent
 */
bool airplay_core_remote_command(airplay_core_remote_t command);

#ifdef __cplusplus
}
#endif
