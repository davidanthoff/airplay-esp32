#pragma once

/* Glue shared between the airplay_core source files. Not part of the public
 * API in include/airplay_core.h. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_mac.h"

#include "airplay_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- airplay_core.c ------------------------------------------------------ */

/** Name from airplay_core_config_t, owned by airplay_core.c. */
const char *airplay_core_get_name(void);

/** Model advertised in mDNS, from airplay_core_config_t.model. */
const char *airplay_core_get_model(void);

/** Forward PCM to the host (wraps airplay_core_host_t.write). */
size_t airplay_core_host_write(const int16_t *pcm, size_t frames,
                               uint32_t sample_rate, uint32_t timeout_ms);

/** Log one "clock:" diagnostics line (PTP state since the previous call).
 * Called from the playback task only, next to its pipeline line. */
void airplay_core_log_clock_status(void);

/* ---- device_mac.c -------------------------------------------------------- */

/** Set the identity MAC (all zero or NULL: use the real Wi-Fi STA MAC). */
void airplay_core_set_device_mac(const uint8_t mac[6]);

/** esp_read_mac() replacement for main/ code: see device_mac.c. */
esp_err_t airplay_core_read_mac(uint8_t *mac, esp_mac_type_t type);

/* ---- audio_output_esphome.c ---------------------------------------------- */
/* The rest of the output backend is the main/audio/audio_output.h API. */

void airplay_output_configure(int32_t output_delay_us);
void airplay_output_set_active(bool active);
void airplay_output_reset_cursor(void);
void airplay_output_notify_played(uint32_t frames, int64_t timestamp_us);

/* ---- mdns_airplay_esphome.c ---------------------------------------------- */
/* Implements main/'s mdns_airplay_init() (network/mdns_airplay.h) without
 * calling mdns_init()/mdns_hostname_set(): ESPHome's mdns component owns
 * those. */

#ifdef __cplusplus
}
#endif
