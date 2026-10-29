/*
 * codec_alloc_psram.c — keep esp_audio_codec's working memory out of internal
 * RAM.
 *
 * The AAC and ALAC decoders (espressif/esp_audio_codec, binary-only) allocate
 * through media_lib_module_malloc() / media_lib_module_calloc(). The library
 * ships weak defaults that call plain malloc() / calloc().
 *
 * The airplay-esp32 firmware builds with CONFIG_SPIRAM_USE_MALLOC and
 * CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=1024, so every allocation above 1 KB
 * lands in PSRAM. ESPHome's psram component uses CONFIG_SPIRAM_USE_CAPS_ALLOC
 * instead: plain malloc() only ever returns internal RAM. With ESPHome's
 * speaker chain running, internal RAM is down to ~60 KB with a largest block
 * of ~31 KB, and the AAC decoder fails to open ("There is no enough memory for
 * AAC buffer"), so AirPlay plays silence.
 *
 * These strong definitions override the weak ones and apply the firmware's rule
 * to the codec only, leaving ESPHome's global allocator alone. Both prefer
 * lists fall back to the other memory type. media_lib_free()'s default calls
 * free(), which handles both, so it is not overridden.
 *
 * The component is linked with WHOLE_ARCHIVE (CMakeLists.txt): the weak
 * definitions already satisfy the codec's references, so without it the
 * linker would never pull this object out of the archive.
 */

#include <stddef.h>

#include "esp_heap_caps.h"

/* Same threshold as the firmware's CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL. */
#define CODEC_INTERNAL_MAX_BYTES 1024

#define CAPS_PSRAM    (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define CAPS_INTERNAL (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)

void *media_lib_module_malloc(const char *module, size_t size) {
  (void)module;
  if (size > CODEC_INTERNAL_MAX_BYTES) {
    return heap_caps_malloc_prefer(size, 2, CAPS_PSRAM, CAPS_INTERNAL);
  }
  return heap_caps_malloc_prefer(size, 2, CAPS_INTERNAL, CAPS_PSRAM);
}

void *media_lib_module_calloc(const char *module, size_t nmemb, size_t size) {
  (void)module;
  size_t total;
  if (__builtin_mul_overflow(nmemb, size, &total)) {
    return NULL;
  }
  if (total > CODEC_INTERNAL_MAX_BYTES) {
    return heap_caps_calloc_prefer(nmemb, size, 2, CAPS_PSRAM, CAPS_INTERNAL);
  }
  return heap_caps_calloc_prefer(nmemb, size, 2, CAPS_INTERNAL, CAPS_PSRAM);
}
