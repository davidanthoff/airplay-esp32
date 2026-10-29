# airplay_core: the airplay-esp32 protocol stack, packaged as an ESP-IDF
# component that ESPHome pulls in via esp32.add_idf_component(path=...).
#
# The sources are the firmware's own, compiled straight from main/ and
# components/dac/ of this repository; nothing is copied. We compile only the
# protocol/audio parts and replace the pieces that assume they own the device
# (main.c, wifi.c, the I2S output backend, mdns_airplay.c) with the files in
# src/.
#
# When main/ gains or loses source files, compare this list with
# main/CMakeLists.txt.

get_filename_component(REPO ${CMAKE_CURRENT_LIST_DIR}/../.. ABSOLUTE)
set(UP ${REPO}/main)

set(MAIN_SRCS
    ${UP}/alac_magic_cookie.c
    ${UP}/settings.c
    ${UP}/playback_control.c
    ${UP}/playback_events.c              # session events (was rtsp/rtsp_events.c)
    ${UP}/dacp_client.c
    ${UP}/audio/audio_receiver.c
    ${UP}/audio/audio_stream.c
    ${UP}/audio/audio_stream_realtime.c
    ${UP}/audio/rtp_resend.c             # realtime retransmit requests
    ${UP}/audio/audio_stream_buffered.c
    ${UP}/audio/audio_decoder.c
    ${UP}/audio/audio_buffer.c
    ${UP}/audio/audio_timing.c           # anchors and latency bookkeeping
    ${UP}/audio/audio_epoch.c            # engine v2: RTP-addressed PCM timeline,
    ${UP}/audio/audio_clock_map.c        #   scheduler with the drift servo, and
    ${UP}/audio/audio_timeline.c         #   the decode worker for buffered streams
    ${UP}/audio/audio_scheduler.c
    ${UP}/audio/audio_engine_v2.c
    ${UP}/audio/audio_decode_worker.c
    ${UP}/audio/audio_crypto.c
    ${UP}/audio/audio_output_common.c    # weak defaults for optional output API
    ${UP}/rtsp/rtsp_server.c
    ${UP}/rtsp/rtsp_conn.c
    ${UP}/rtsp/rtsp_message.c
    ${UP}/rtsp/rtsp_handlers.c
    ${UP}/rtsp/rtsp_fairplay.c
    ${UP}/rtsp/rtsp_crypto.c
    ${UP}/rtsp/rtsp_rsa.c
    ${UP}/plist/plist_xml.c
    ${UP}/plist/bplist_parser.c
    ${UP}/plist/bplist_builder.c
    ${UP}/plist/base64.c
    ${UP}/hap/hap.c
    ${UP}/hap/hap_pair_verify.c
    ${UP}/hap/hap_pair_setup.c
    ${UP}/hap/hap_crypto.c
    ${UP}/hap/srp.c
    ${UP}/hap/tlv8.c
    ${UP}/network/ptp_clock.c
    ${UP}/network/ntp_clock.c
    ${UP}/network/socket_utils.c
    ${REPO}/components/dac/dac.c
)

# Deliberately NOT compiled (ESPHome owns these, or we replace them):
#   main.c                      -> src/airplay_core.c
#   network/mdns_airplay.c      -> src/mdns_airplay_esphome.c
#   audio/audio_output*.c       -> src/audio_output_esphome.c
#   audio/audio_resample.c      -> ESPHome's resampler speaker
#   network/wifi.c ethernet.c web_server.c ota.c dns_server.c log_stream.c
#   led.c buttons.c bt_coex.c audio/a2dp_sink.c
#   audio/audio_eq.c (software EQ, CONFIG_SOFTWARE_EQ)
#   sendspin/ usb/ audio/usb_audio_sink.c: the firmware's own Sendspin player
#     and USB audio. Every call into them from the files above sits behind
#     CONFIG_SENDSPIN_ENABLE / CONFIG_USB_AUDIO_SINK / CONFIG_BT_A2DP_ENABLE,
#     which stay undefined here.
#   components/{boards,display,dac_*,husb238a,spiffs_storage,...}

set(GLUE_SRCS
    src/airplay_core.c
    src/audio_output_esphome.c
    src/mdns_airplay_esphome.c
    src/codec_alloc_psram.c   # overrides esp_audio_codec's weak allocators
    src/device_mac.c          # the MAC AirPlay uses as its device id
)

idf_component_register(
    SRCS ${GLUE_SRCS} ${MAIN_SRCS}
    # Link every object, including codec_alloc_psram.c: nothing references it
    # directly (it replaces weak symbols), so the linker would skip it.
    WHOLE_ARCHIVE
    INCLUDE_DIRS include
    PRIV_INCLUDE_DIRS
        src
        ${UP}
        ${UP}/audio
        ${UP}/rtsp
        ${UP}/plist
        ${UP}/hap
        ${UP}/network
        ${REPO}/components/dac/include
    PRIV_REQUIRES
        nvs_flash
        esp_netif
        esp_event
        esp_timer
        heap
        esphome__libsodium # ESPHome's esphome/libsodium, added by media_source.py
        airplay_sodium    # the libsodium modules ESPHome's port leaves out (../airplay_sodium)
        esp_http_client   # dacp_client.c (AirPlay 1 remote control)
        esp_app_format    # esp_app_get_description() in the AirPlay 1 mDNS record
        mbedtls
        lwip
)

# The firmware takes its device id from esp_read_mac(ESP_MAC_WIFI_STA). Point
# those calls, in main/ sources only, at src/device_mac.c, which returns the
# identity MAC the host chose (the one ESPHome's Sendspin reports). The macro
# renames the esp_mac.h declaration too, so main/ compiles unchanged. Our glue
# calls airplay_core_read_mac() explicitly.
set_source_files_properties(${MAIN_SRCS} PROPERTIES
    COMPILE_DEFINITIONS "esp_read_mac=airplay_core_read_mac")

# main/ is written against -Wall without -Werror; ESPHome's flags may be
# stricter. If that ever bites, loosen only for main/ sources, e.g.:
# set_source_files_properties(${MAIN_SRCS} PROPERTIES COMPILE_OPTIONS "-Wno-error")
