/*
 * mdns_airplay_esphome.c — replacement for main/network/mdns_airplay.c in the
 * ESPHome build, derived from it.
 *
 * Differences from main/network/mdns_airplay.c:
 *   - no mdns_init() / mdns_hostname_set(): ESPHome's mdns component has
 *     already done both, and a second mdns_init() returns
 *     ESP_ERR_INVALID_STATE, which main/ ESP_ERROR_CHECKs (= reboot loop).
 *   - the device id comes from airplay_core_read_mac() instead of the
 *     wifi.c helper, which we do not compile. main/ asks for the WiFi STA
 *     MAC everywhere (rtsp_handlers.c, hap_pair_verify.c), even on Ethernet;
 *     in our build those calls return the identity MAC chosen by the host
 *     (device_mac.c), and this file must use the same one.
 *   - model and manufacturer: see below.
 *
 * The TXT records must stay in sync with main/network/mdns_airplay.c
 * (features, pk, flags, ...): when that file changes, change this one too.
 */

#include "mdns_airplay.h"

#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "mdns.h"

#include "airplay_core_internal.h"
#include "hap.h"
#include "rtsp_handlers.h" /* airplay_features() */
#include "rtsp_server.h"   /* airplay_rtsp_port() */
#include "settings.h"

static const char *TAG = "mdns_airplay";

/* Only on the AirPlay 2 records; in v1 mode _airplay._tcp is not registered
 * and the classic _raop._tcp TXT has no vv. */
#define AIRPLAY_PROTOCOL_VERSION "2"
#define AIRPLAY_SOURCE_VERSION   "377.40.00"
#define AIRPLAY_FLAGS            "0x4" /* audio receiver */

/* Classic (AirPlay 1) source version: the first-generation AirPort Express. */
#define AIRPLAY_V1_SOURCE_VERSION "105.1"

#ifdef CONFIG_ENABLE_AIRPLAY_ARTWORK
#define AIRPLAY_METADATA_TYPES "0,1,2"
#else
#define AIRPLAY_METADATA_TYPES "0,2"
#endif

/* Model: configurable (airplay_core_get_model(), default "esphome-airplay").
 * main/ advertises "AudioAccessory5,1", the HomePod mini, for the speaker
 * icon in iOS, and "AirPort4,107" in v1 mode. Music Assistant maps
 * "AudioAccessory5,1" to "Apple / HomePod Mini", registers the receiver as a
 * native Apple player and then sends ALL volume changes to AirPlay, even
 * while Sendspin plays. shairport-sync advertises its own model
 * ("ShairportSync") in both modes and iOS treats it as a normal speaker.
 *
 * Manufacturer: not something iOS needs, but Music Assistant uses
 * manufacturer + model verbatim when both are present, and only treats
 * "Apple" + HomePod/Apple TV as a native Apple device. So this keeps Music
 * Assistant treating AirPlay as one protocol next to Sendspin even if the
 * model is set back to "AudioAccessory5,1". */
#define AIRPLAY_MANUFACTURER "ESPHome"

void mdns_airplay_init(void) {
  char device_id[18];
  char features_str[32];
  char service_name[80];
  char pk_str[65];
  char device_name[65];

  settings_get_device_name(device_name, sizeof(device_name));

  uint8_t mac[6];
  airplay_core_read_mac(mac,
                        ESP_MAC_WIFI_STA); /* identity MAC, see device_mac.c */
  snprintf(device_id, sizeof(device_id), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

  const uint8_t *pk = hap_get_public_key();
  for (int i = 0; i < 32; i++) {
    snprintf(pk_str + (size_t)i * 2, 3, "%02x", pk[i]);
  }

  uint64_t features = airplay_features();
  snprintf(features_str, sizeof(features_str), "0x%X,0x%X",
           (unsigned)(features & 0xFFFFFFFF), (unsigned)(features >> 32));

  /* RAOP instance name: <MAC>@<name> */
  snprintf(service_name, sizeof(service_name), "%02X%02X%02X%02X%02X%02X@%s",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], device_name);

  /* Fixed at settings_init(); the RTSP server listens on 5000 in v1 mode and
   * on 7000 otherwise (airplay_rtsp_port()). */
  const bool airplay_v1 = settings_airplay_v1();

  if (!airplay_v1) {
    mdns_txt_item_t airplay_txt[] = {
        {"deviceid", device_id},
        {"features", features_str},
        {"flags", AIRPLAY_FLAGS},
        {"model", airplay_core_get_model()},
        {"manufacturer", AIRPLAY_MANUFACTURER},
        {"pk", pk_str},
        {"pi", "00000000-0000-0000-0000-000000000000"},
        {"srcvers", AIRPLAY_SOURCE_VERSION},
        {"vv", AIRPLAY_PROTOCOL_VERSION},
        {"acl", "0"},
    };
    esp_err_t err = mdns_service_add(
        device_name, "_airplay", "_tcp", airplay_rtsp_port(), airplay_txt,
        sizeof(airplay_txt) / sizeof(airplay_txt[0]));
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "Failed to add _airplay._tcp: %s", esp_err_to_name(err));
    }
  }

  esp_err_t err_raop;
  if (airplay_v1) {
    /* Classic RAOP, mirroring shairport-sync's classic record like main/:
     * no features, no pk, no HAP pairing. main/ notes that Apple Music on
     * Windows rejects anything carrying AirPlay 2 markers. */
    mdns_txt_item_t raop_txt[] = {
        {"am", airplay_core_get_model()},
        {"manufacturer", AIRPLAY_MANUFACTURER},
        {"ch", "2"},
        {"cn", "0,1"},
        {"da", "true"},
        {"ek", "1"},
        {"et", "0,1"},
        {"fv", esp_app_get_description()->version},
        {"md", AIRPLAY_METADATA_TYPES},
        {"pw", "false"},
        {"sf", AIRPLAY_FLAGS},
        {"sr", "44100"},
        {"ss", "16"},
        {"sv", "false"},
        {"tp", "UDP"},
        {"vn", "65537"},
        {"vs", AIRPLAY_V1_SOURCE_VERSION},
        {"txtvers", "1"},
    };
    err_raop =
        mdns_service_add(service_name, "_raop", "_tcp", airplay_rtsp_port(),
                         raop_txt, sizeof(raop_txt) / sizeof(raop_txt[0]));
  } else {
    mdns_txt_item_t raop_txt[] = {
        {"am", airplay_core_get_model()},
        {"manufacturer", AIRPLAY_MANUFACTURER},
        {"cn", "0,1,2,3"},
        {"da", "true"},
        {"ek", "1"},
        {"et", "0,1,3,5"},
        {"ft", features_str},
        {"md", AIRPLAY_METADATA_TYPES},
        {"pk", pk_str},
        {"sf", AIRPLAY_FLAGS},
        {"tp", "UDP"},
        {"vn", "65537"},
        {"vs", AIRPLAY_SOURCE_VERSION},
        {"vv", AIRPLAY_PROTOCOL_VERSION},
    };
    err_raop =
        mdns_service_add(service_name, "_raop", "_tcp", airplay_rtsp_port(),
                         raop_txt, sizeof(raop_txt) / sizeof(raop_txt[0]));
  }
  if (err_raop != ESP_OK) {
    ESP_LOGE(TAG, "Failed to add _raop._tcp: %s", esp_err_to_name(err_raop));
  }

  ESP_LOGI(TAG, "Advertising AirPlay %s receiver '%s' (%s) on port %u",
           airplay_v1 ? "1" : "2", device_name, device_id,
           (unsigned)airplay_rtsp_port());
}
