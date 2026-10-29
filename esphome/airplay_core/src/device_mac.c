/*
 * device_mac.c — the MAC address AirPlay uses as this receiver's identity.
 *
 * main/ reads esp_read_mac(ESP_MAC_WIFI_STA) wherever it needs the device
 * id: the mDNS "deviceid", /info, HAP pair-verify, the settings defaults.
 * ESPHome's Sendspin hub identifies the same board by a different MAC
 * whenever `ethernet:` is configured: the Ethernet MAC, which on the ESP32-S3
 * is the base MAC + 3. Music Assistant links the AirPlay and the Sendspin
 * player of one device by MAC. With two different MACs, the speaker shows up
 * twice, once as an AirPlay player and once as a Sendspin player.
 *
 * The host passes the MAC it wants (airplay_core_config_t.device_mac). Every
 * main/ source file is compiled with esp_read_mac redefined to
 * airplay_core_read_mac (CMakeLists.txt), so ESP_MAC_WIFI_STA requests
 * return that MAC. All other MAC types, and all code outside main/, still see
 * the real values. This file itself is not compiled with the redefinition.
 */

#include <stdbool.h>
#include <string.h>

#include "esp_mac.h"

#include "airplay_core_internal.h"

static uint8_t s_device_mac[6];
static bool s_device_mac_set = false;

void airplay_core_set_device_mac(const uint8_t mac[6]) {
  static const uint8_t zero[6] = {0};
  if (mac == NULL || memcmp(mac, zero, sizeof(zero)) == 0) {
    s_device_mac_set = false;
    return;
  }
  memcpy(s_device_mac, mac, sizeof(s_device_mac));
  s_device_mac_set = true;
}

esp_err_t airplay_core_read_mac(uint8_t *mac, esp_mac_type_t type) {
  if (type == ESP_MAC_WIFI_STA && s_device_mac_set && mac != NULL) {
    memcpy(mac, s_device_mac, sizeof(s_device_mac));
    return ESP_OK;
  }
  return esp_read_mac(mac, type);
}
