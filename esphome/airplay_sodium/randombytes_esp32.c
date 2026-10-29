/*
 * The ESP32 hardware RNG as libsodium's random source.
 *
 * ESPHome's libsodium port keeps libsodium's default randombytes backend,
 * "sysrandom", which reads /dev/urandom (getrandom() is not enabled in the
 * port). ESP-IDF has no /dev/urandom, so the first randombytes call, already
 * inside sodium_init(), fails and libsodium deliberately aborts
 * (sodium_misuse()). ESPHome's own use of the port (noise-c) never hits this
 * because noise-c brings its own RNG; AirPlay calls sodium_init() in hap_init()
 * and uses randombytes for keys, so the firmware panicked a few seconds after
 * every boot and the bootloader rolled the OTA back.
 *
 * espressif/libsodium, which the firmware uses, solves this with
 * port/randombytes_esp32.c compiled in as the default implementation. We
 * can't change the port's default, so we install the same thing at run time
 * with randombytes_set_implementation(), before sodium_init() runs.
 */

#include "airplay_sodium.h"

#include "esp_random.h"
#include "randombytes.h"
#include "sdkconfig.h"

static const char *esp32_implementation_name(void) {
  return CONFIG_IDF_TARGET;
}

static const randombytes_implementation s_esp32_implementation = {
    .implementation_name = esp32_implementation_name,
    .random = esp_random,
    .stir = NULL,
    .uniform = NULL,
    .buf = esp_fill_random,
    .close = NULL,
};

void airplay_sodium_use_esp32_random(void) {
  randombytes_set_implementation(&s_esp32_implementation);
}
