# AirPlay for ESPHome

An **AirPlay 2 receiver for ESPHome**, as a `media_source` platform. An ESP32
speaker built with ESPHome's `speaker_source` media player (for example one
that already plays Sendspin from Music Assistant) also shows up in the iPhone's
AirPlay picker. It can be multi-selected with HomePods and other AirPlay 2
speakers and plays in sync with them.

The protocol stack is this repository's firmware (`main/`): RTSP, HomeKit
pairing, FairPlay, PTP, ALAC/AAC and the timing engine. This directory adds an
ESPHome output backend and the media source glue around it. Nothing in `main/`
is changed for it.

> **Status: experimental.** Plays AirPlay 2 and AirPlay 1 on ESP32-S3 boards
> with PSRAM, next to Sendspin, with sync verified against an Apple TV.
> ESPHome's `media_source` API is itself new and marked experimental.

## Configuration

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/rbouteiller/airplay-esp32
      ref: main
      path: esphome/components
    components: [airplay]

media_source:
  - platform: airplay
    id: airplay_media_source
    # name: Living Room          # defaults to the node's friendly_name, the name Sendspin uses
    # output_delay: 0ms          # latency added after the ESP32 (TOSLINK receiver, AVR); negative = play later
    # model: esphome-airplay     # advertised over mDNS; see below
    # airplay_1_only: false      # classic AirPlay 1 only (RTSP on port 5000)
    # artwork: false             # receive cover art

media_player:
  - platform: speaker_source
    # ...
    media_pipeline:
      sources:
        - sendspin_media_source
        - airplay_media_source
```

The `github://` shorthand can't name a subdirectory, hence the long form with
`path:`.

**Requirements:** ESP32 (an S3 with PSRAM is recommended), the ESP-IDF
framework, ESPHome ≥ 2026.10.

**Output delay without recompiling:** a TOSLINK receiver, DAC or AVR after the
ESP32 adds delay that no sync protocol can see. The `airplay` number platform
gives one "Output delay" setting (ms, 0–500) on the device page in Home
Assistant:

```yaml
media_source:
  - platform: sendspin
    id: sendspin_media_source
    static_delay_adjustable: true   # required: otherwise Sendspin ignores the delay

number:
  - platform: airplay
    name: Output delay
```

AirPlay and Sendspin both play that much earlier, and the value is kept on the
device. `output_delay:` then only sets the value for the very first boot.
Sendspin is linked automatically when the node has a `sendspin:` hub. Music
Assistant's own Sendspin static delay follows this one; if it is changed in
Music Assistant, the speaker sets it back.

**`model`:** cosmetic. `model: "AudioAccessory5,1"` makes iOS show a HomePod
mini icon. The receiver always advertises `manufacturer=ESPHome` as well, so
Music Assistant never mistakes it for a real HomePod. If it did, it would send
all volume changes over AirPlay, even while Sendspin is playing.

**Logs:** the C core logs through ESP-IDF (`ESP_LOGx`). ESPHome re-logs that
under the tag `esp-idf` and filters it with `esp32: framework: log_level:`
(default `ERROR`), not with `logger:`. Set `log_level: INFO` to see sessions,
pairing and the timing engine.

## Layout

```
esphome/
├── components/airplay/   ESPHome external component (Python + C++). ESPHome
│                         copies only the top-level files of a component, so
│                         no subdirectories here.
├── airplay_core/         ESP-IDF component: compiles the needed main/ sources
│   │                     plus the glue in src/; media_source.py adds it with
│   │                     esp32.add_idf_component(path=...)
│   ├── include/airplay_core.h   the C API between the core and ESPHome
│   └── src/              bring-up, output backend, mDNS, device MAC, codec allocator
└── airplay_sodium/       ESP-IDF component: the libsodium modules ESPHome's
                          libsodium port leaves out
```

ESPHome clones the whole repository for a git external component, so
`airplay_core/CMakeLists.txt` can compile `main/` and `components/dac/`
straight from the checkout. It does not initialise submodules; none are
needed.

## How it works

```
 iPhone ──RTSP/HAP/FairPlay──► ┌──────────── airplay_core (ESP-IDF component, C) ────────────┐
        ──PTP (319/320)──────► │ main/: rtsp/ hap/ plist/ ptp_clock  audio_receiver → decoder │
        ──audio (UDP/TCP)────► │        engine v2: RTP timeline → scheduler (drift servo)     │
                               │ src/:  airplay_core.c          bring-up (replaces main.c)    │
                               │        mdns_airplay_esphome.c  TXT records w/o mdns_init()   │
                               │        audio_output_esphome.c  output backend ──┐            │
                               └──────────────────────────────────────────────────┼───────────┘
                                        host.write(pcm, frames, rate)             │
                               ┌──────────── components/airplay (ESPHome, C++) ───▼───────────┐
                               │ AirPlayMediaSource : media_source::MediaSource                │
                               │   write_output() ─► speaker_source media player               │
                               │   request_play_uri_("airplay://current") on session start     │
                               └──────────┬────────────────────────────────────────────────────┘
                                          ▼
                     resampler speaker ─► mixer speaker ─► i2s_audio ─► DAC / TOSLINK
                                          │
          notify_audio_played(frames, ts) ◄┘ (from the I2S DMA completion callback)
                     └──► queued = submitted − played ─► audio_output_get_pipeline_us()
                                                       ─► the timing engine keeps the sender's schedule
```

The port plugs into seams the firmware already has:

1. **Output backend.** The firmware picks one of `audio_output.c` (I2S),
   `audio_output_spdif.c` or `audio_output_usb.c` at build time and keeps
   weak defaults in `audio_output_common.c`. `src/audio_output_esphome.c` is
   one more backend.
2. **Latency feedback.** The timing engine asks
   `audio_output_get_pipeline_us()` how long until the next sample written is
   heard. The I2S backend answers "frames handed to DMA − frames DMA sent". We
   answer "frames ESPHome accepted − frames ESPHome reported played", from
   `MediaSource::notify_audio_played()`. The resampler, mixer, ring buffers
   and DMA are all inside that measurement. Only delay after the ESP32 is not,
   which is what `output_delay` covers. ESPHome reports played frames in
   batches, so `played` is extrapolated between reports, and the 5 ms the
   engine adds for the I2S write (`OUTPUT_PIPELINE_LATENCY_US`) is taken back
   off because our measurement already contains it.
3. **Volume.** The firmware calls `dac_set_volume(dB)` through a registered
   `dac_ops_t`; ours forward to the media player. The sender's volume is
   applied only while AirPlay is the active source: senders push their stored
   volume as soon as they connect, even without playing.
4. **Session state.** `playback_events_register()` gives
   CONNECTED/PLAYING/PAUSED/DISCONNECTED/METADATA (transitions only). Core
   callbacks arrive on FreeRTOS tasks and are marshalled to ESPHome's main
   loop, the same pattern as ESPHome's Sendspin media source.

### Not compiled, or replaced

| `main/` | In the ESPHome build |
|---|---|
| `main.c` | `src/airplay_core.c` mirrors `start_airplay_services()`. Keep the two in step. |
| `network/mdns_airplay.c` | `src/mdns_airplay_esphome.c`: ESPHome has already called `mdns_init()`, and a second call returns `ESP_ERR_INVALID_STATE`, which `main/` treats as fatal. Keep the TXT records in step with `mdns_airplay.c`. |
| `audio/audio_output*.c` | `src/audio_output_esphome.c` |
| `audio/audio_resample.c` | ESPHome's resampler speaker |
| Wi-Fi, Ethernet, web UI, OTA, LEDs, buttons, display, Bluetooth, USB audio, the firmware's own Sendspin, software EQ | ESPHome, or not used. Calls into them from the compiled files sit behind Kconfig symbols that stay undefined. |

When `main/` gains or loses source files, compare `main/CMakeLists.txt` with
`airplay_core/CMakeLists.txt`.

### Things ESPHome does differently from the firmware

- **`malloc` and PSRAM.** The firmware builds with
  `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=1024` (every `malloc` above 1 KB goes to
  PSRAM). ESPHome uses `CONFIG_SPIRAM_USE_CAPS_ALLOC`, so plain `malloc` only
  returns internal RAM. The AAC decoder then failed to open.
  `src/codec_alloc_psram.c` applies the firmware's 1 KB rule to
  `esp_audio_codec` only. The timeline and decode jobs already ask for PSRAM
  explicitly. Task stacks stay in internal RAM.
- **libsodium.** With `api: encryption:`, ESPHome brings its own libsodium
  port (`esphome/libsodium`), which can't coexist with `espressif/libsodium`.
  We use ESPHome's (same version, taken from its noise component), and
  `airplay_sodium` compiles the modules HAP needs that the port leaves out:
  SHA-512, HMAC-SHA512, Ed25519 signatures and `crypto_box`. The port's
  default random source reads `/dev/urandom`, which ESP-IDF lacks, and makes
  `sodium_init()` abort; `airplay_sodium` installs the hardware RNG first.
- **Device id.** The firmware uses the Wi-Fi station MAC as the AirPlay device
  id. ESPHome's Sendspin reports the Ethernet MAC whenever `ethernet:` is
  configured, and Music Assistant merges the AirPlay and Sendspin players of a
  device by MAC. So `main/` sources are compiled with
  `esp_read_mac=airplay_core_read_mac`, which returns the MAC Sendspin
  reports (`src/device_mac.c`).
- **Task priorities.** The playback task keeps
  `AUDIO_PLAYBACK_TASK_PRIORITY` (9), above the receiver and decode tasks;
  otherwise playback drifts late.

## Known limitations

- **Remote control** (play/pause/next from Home Assistant) works only for
  AirPlay 1 sessions, over DACP. AirPlay 2 needs MRP, which the firmware does
  not implement.
- **Stopping a session** (Sendspin takes over): there is no public
  "disconnect" in `rtsp_server.c`, so the server is stopped and restarted.
  AirPlay 2 senders let go. AirPlay 1 senders get a DACP pause first, since
  they stream over UDP and would not notice.
- **Flush** (seek, skip): audio already queued in ESPHome's speaker chain
  can't be removed through the `MediaSource` API, so it plays out; the timing
  engine drops the late frames that follow.
- **Any new AirPlay connection ends the current session**, including Music
  Assistant's AirPlay provider probing the receiver. If Music Assistant should
  use Sendspin for the speaker anyway, disable its AirPlay protocol player.
