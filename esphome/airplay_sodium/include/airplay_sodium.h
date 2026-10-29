#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Make libsodium use the ESP32 hardware RNG. Must run before sodium_init()
 * and before any other libsodium call that needs randomness; see
 * randombytes_esp32.c for why.
 */
void airplay_sodium_use_esp32_random(void);

#ifdef __cplusplus
}
#endif
