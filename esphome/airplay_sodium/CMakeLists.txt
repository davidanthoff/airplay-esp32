# airplay_sodium: the libsodium modules AirPlay needs on top of ESPHome's port.
#
# ESPHome brings its own libsodium (esphome/libsodium, a libsodium 1.0.21
# port, managed component esphome__libsodium), which
# components/airplay/media_source.py always adds. It ships the full libsodium
# source tree but compiles only what ESPHome needs: noise, SHA-512 and Ed25519
# signature verification (1.10021.12, ESPHome 2026.10). AirPlay's HAP pairing
# also uses HMAC-SHA512, Ed25519 signing and key generation, and
# crypto_box_keypair(). This component compiles exactly those modules from the
# SAME source tree, so there is one libsodium (one version, no duplicate
# symbols) whether API encryption is on or off. When ESPHome bumps the port,
# compare its CMakeLists.txt with the list below.
#
# We can't use espressif/libsodium instead: with `api: encryption:` ESP-IDF
# refuses to build with both ("Requirement espressif__libsodium and
# requirement libsodium are both added as project_managed_components").
#
# Include paths and -DCONFIGURED=1 come PUBLIC from the libsodium component.

if(CMAKE_BUILD_EARLY_EXPANSION)
    idf_component_register(REQUIRES esphome__libsodium esp_hw_support)
    return()
endif()

idf_component_get_property(sodium_dir esphome__libsodium COMPONENT_DIR)
set(S ${sodium_dir}/libsodium/src/libsodium)

idf_component_register(
    SRCS
        # SHA-512 wrappers and HMAC-SHA512 (HAP HKDF, SRP, pair-verify); the
        # port compiles the SHA-512 core (cp/hash_sha512_cp.c)
        ${S}/crypto_hash/sha512/hash_sha512.c
        ${S}/crypto_auth/hmacsha512/auth_hmacsha512.c
        # Ed25519 signing and key generation (HAP long-term key); the port
        # compiles verification (ref10/open.c)
        ${S}/crypto_sign/crypto_sign.c
        ${S}/crypto_sign/ed25519/sign_ed25519.c
        ${S}/crypto_sign/ed25519/ref10/keypair.c
        ${S}/crypto_sign/ed25519/ref10/sign.c
        # crypto_box_keypair() and what crypto_box.c references
        ${S}/crypto_box/crypto_box.c
        ${S}/crypto_box/curve25519xsalsa20poly1305/box_curve25519xsalsa20poly1305.c
        ${S}/crypto_secretbox/crypto_secretbox.c
        ${S}/crypto_secretbox/xsalsa20poly1305/secretbox_xsalsa20poly1305.c
        ${S}/crypto_core/hsalsa20/core_hsalsa20.c
        ${S}/crypto_core/hsalsa20/ref2/core_hsalsa20_ref2.c
        ${S}/crypto_stream/xsalsa20/stream_xsalsa20.c
        # The ESP32 hardware RNG as libsodium's random source: the port's
        # default reads /dev/urandom, which ESP-IDF lacks, and aborts.
        randombytes_esp32.c
    INCLUDE_DIRS include
    REQUIRES esphome__libsodium esp_hw_support
)

# The port compiles its own copy of _crypto_sign_ed25519_ref10_hinit()
# (port/ed25519_ref10_hinit.c) for open.c, since it leaves sign.c out. Rename
# sign.c's identical definition, and its calls with it, so the two don't clash.
set_source_files_properties(${S}/crypto_sign/ed25519/ref10/sign.c PROPERTIES
    COMPILE_DEFINITIONS
        "_crypto_sign_ed25519_ref10_hinit=airplay_sodium_ed25519_ref10_hinit")

# Same warning suppressions as ESPHome's port uses for libsodium sources.
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-unused-function
    -Wno-unknown-pragmas
    -Wno-unused-variable
    -Wno-type-limits
)
