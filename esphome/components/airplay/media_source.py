"""``media_source: - platform: airplay``

Modelled on ESPHome's own media sources (esphome/components/audio_http/
media_source.py and sendspin/media_source/__init__.py).
"""

from pathlib import Path

import esphome.codegen as cg
from esphome.components import esp32, media_source, network, socket, wifi
from esphome.components.noise import LIBSODIUM_VERSION
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_MODEL, CONF_NAME
from esphome.core import CORE
from esphome.types import ConfigType

from . import airplay_ns

CODEOWNERS = ["@davidanthoff"]
DEPENDENCIES = ["network"]
AUTO_LOAD = ["audio", "mdns"]

CONF_OUTPUT_DELAY = "output_delay"
CONF_AIRPLAY_1_ONLY = "airplay_1_only"
CONF_ARTWORK = "artwork"

AirPlayMediaSource = airplay_ns.class_(
    "AirPlayMediaSource", cg.Component, media_source.MediaSource
)

# <repo>/esphome/components/airplay/media_source.py -> <repo>/esphome/airplay_core
# ESPHome clones the whole repository for external_components, so the sibling
# directories, and the firmware's main/ that airplay_core compiles, are present
# both for git and local sources.
AIRPLAY_CORE_DIR = Path(__file__).resolve().parents[2] / "airplay_core"
AIRPLAY_SODIUM_DIR = Path(__file__).resolve().parents[2] / "airplay_sodium"


def _consume_sockets(config: ConfigType) -> ConfigType:
    """Reserve lwIP sockets so ESPHome sizes CONFIG_LWIP_MAX_SOCKETS for us.

    Estimates from reading main/ (not yet checked against lwIP stats):
      TCP listen: RTSP :7000 (:5000 in AirPlay 1 mode), AirPlay 2 event port,
                  buffered-audio port
      TCP:        RTSP client (current + old slot), event conn, buffered-audio conn
      UDP:        realtime data/control/timing, PTP 319 + 320, NTP
    Plus one TCP for the DACP HTTP client (AirPlay 1 remote control).
    """
    socket.consume_sockets(3, "airplay", socket.SocketType.TCP_LISTEN)(config)
    socket.consume_sockets(5, "airplay", socket.SocketType.TCP)(config)
    socket.consume_sockets(6, "airplay", socket.SocketType.UDP)(config)
    return config


def _default_name(config: ConfigType) -> ConfigType:
    """Default the AirPlay name to the node's friendly_name.

    That is the name ESPHome's Sendspin hub announces (sendspin_hub.cpp,
    build_client_config_()), so the speaker has one name in Music Assistant and
    in the AirPlay picker. Sendspin sends an empty name when there is no
    friendly_name; we use the node name then. CORE.friendly_name is set by
    preload_core_config() before any component is validated.
    """
    if CONF_NAME in config:
        return config
    name = CORE.friendly_name or CORE.name
    if len(name) > 64:
        raise cv.Invalid(
            f"Default AirPlay name '{name}' is longer than 64 characters; "
            "set 'name' explicitly",
            [CONF_NAME],
        )
    config[CONF_NAME] = name
    return config


def _request_networking(config: ConfigType) -> ConfigType:
    # Same as Sendspin: bigger lwIP buffers for streaming audio, and the
    # runtime WiFi APIs the C++ side uses to switch power save off and pause
    # roaming scans while a sender is connected (request_high_performance(),
    # request_roaming_suppression()). No effect on Ethernet-only configs.
    network.require_high_performance_networking()
    wifi.enable_runtime_power_save_control()
    wifi.enable_runtime_roaming_suppression()
    return config


CONFIG_SCHEMA = cv.All(
    media_source.media_source_schema(AirPlayMediaSource)
    .extend(
        {
            # Name in the iOS AirPlay picker. Defaults to the node's
            # friendly_name, the same name Sendspin uses; see _default_name().
            cv.Optional(CONF_NAME): cv.All(cv.string_strict, cv.Length(max=64)),
            # Model advertised over mDNS (TXT "model" / "am"). Cosmetic:
            # "AudioAccessory5,1" (the firmware's default) gets the HomePod mini
            # icon in iOS. The mDNS glue also sends manufacturer=ESPHome, which
            # keeps Music Assistant from treating the receiver as a real HomePod
            # (see mdns_airplay_esphome.c). Default follows shairport-sync's
            # neutral "ShairportSync".
            cv.Optional(CONF_MODEL, default="esphome-airplay"): cv.All(
                cv.string_strict, cv.Length(min=1, max=32)
            ),
            # Delay added AFTER the ESP32 (TOSLINK receiver, AVR DSP). The timing
            # engine plays this much earlier. Negative values make it play
            # later. Tune against another AirPlay 2 speaker (HomePod, Apple TV).
            cv.Optional(CONF_OUTPUT_DELAY, default="0ms"): cv.All(
                cv.time_period_microseconds,
                cv.Range(
                    min=cv.TimePeriod(milliseconds=-200),
                    max=cv.TimePeriod(milliseconds=500),
                ),
            ),
            # AirPlay 1 (classic RAOP) only, on port 5000. Applied at boot.
            cv.Optional(CONF_AIRPLAY_1_ONLY, default=False): cv.boolean,
            cv.Optional(CONF_ARTWORK, default=False): cv.boolean,
        }
    )
    .extend(cv.COMPONENT_SCHEMA),
    cv.only_on_esp32,
    _default_name,
    _consume_sockets,
    _request_networking,
)


async def to_code(config: ConfigType) -> None:
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await media_source.register_media_source(var, config)

    cg.add(var.set_advertised_name(config[CONF_NAME]))
    cg.add(var.set_model(config[CONF_MODEL]))
    cg.add(var.set_output_delay_us(config[CONF_OUTPUT_DELAY].total_microseconds))
    cg.add(var.set_airplay_1_only(config[CONF_AIRPLAY_1_ONLY]))

    # The AirPlay protocol stack: the firmware's main/ sources + C glue, built
    # as a local ESP-IDF component (airplay_core/CMakeLists.txt). ESPHome writes it
    # into the project's idf_component.yml as a `path:` dependency and adds it
    # to the main component's REQUIRES, so airplay_core.h is on the include path.
    esp32.add_idf_component(name="airplay_core", path=str(AIRPLAY_CORE_DIR))

    # libsodium for the AirPlay crypto (HAP, ChaCha20-Poly1305, Ed25519,
    # SHA-512). Use the same port, version and mechanism as ESPHome's noise
    # component rather than espressif/libsodium: with `api: encryption:` both
    # would be in the build, and ESP-IDF refuses to pick one. Declared here
    # too so it is present without API encryption. The version comes from
    # noise itself, so the two cannot drift apart on an ESPHome upgrade.
    esp32.add_idf_component(name="esphome/libsodium", ref=LIBSODIUM_VERSION)
    # ESPHome's port compiles only what noise needs; airplay_sodium compiles the
    # modules AirPlay needs on top, from the same source tree.
    esp32.add_idf_component(name="airplay_sodium", path=str(AIRPLAY_SODIUM_DIR))

    # Kconfig symbol declared in airplay_core/Kconfig (same name as in main/Kconfig.projbuild).
    esp32.add_idf_sdkconfig_option(
        "CONFIG_ENABLE_AIRPLAY_ARTWORK", config[CONF_ARTWORK]
    )

    # ESPHome excludes these IDF components by default (DEFAULT_EXCLUDED_IDF_COMPONENTS);
    # dacp_client.c needs the HTTP client.
    esp32.include_builtin_idf_component("esp_http_client")
    esp32.include_builtin_idf_component("esp-tls")

    cg.add_define("USE_AIRPLAY")
