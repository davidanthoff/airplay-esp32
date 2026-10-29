"""``number: - platform: airplay``: one output delay for AirPlay and Sendspin.

The delay after the ESP32 (TOSLINK receiver, DAC, AVR) belongs to the hardware
chain, not to a protocol, so this one Home Assistant setting feeds both:

- AirPlay: the media source's output delay, changed at runtime.
- Sendspin (when linked): the player's output delay (the YAML calls it the
  static delay), set through PlayerRole::update_output_delay(). sendspin-cpp
  stores it on the device and reports it to the server; Music Assistant
  stores the reported value in its own player setting.

The value is kept on the device. On the very first boot it is the media
source's ``output_delay``.
"""

import esphome.codegen as cg
from esphome.components import number
import esphome.config_validation as cv
from esphome.const import (
    CONF_ID,
    CONF_PLATFORM,
    DEVICE_CLASS_DURATION,
    ENTITY_CATEGORY_CONFIG,
    UNIT_MILLISECOND,
)
from esphome.core import CORE
import esphome.final_validate as fv
from esphome.types import ConfigType

from . import airplay_ns
from .media_source import CONF_OUTPUT_DELAY, AirPlayMediaSource

CONF_AIRPLAY_ID = "airplay_id"
CONF_SENDSPIN_ID = "sendspin_id"
CONF_STATIC_DELAY_ADJUSTABLE = "static_delay_adjustable"

# Sendspin's static delay is 0..5000 ms and cannot be negative; the AirPlay
# media source accepts up to 500 ms.
MAX_DELAY_MS = 500

OutputDelayNumber = airplay_ns.class_(
    "OutputDelayNumber", number.Number, cg.Component
)


def _sendspin_hub_type():
    # Imported lazily so configs without Sendspin never load its module.
    from esphome.components.sendspin import SendspinHub

    return SendspinHub


def _sendspin_id_schema(value):
    return cv.use_id(_sendspin_hub_type())(value)


CONFIG_SCHEMA = (
    number.number_schema(
        OutputDelayNumber,
        icon="mdi:timer-sand",
        entity_category=ENTITY_CATEGORY_CONFIG,
        device_class=DEVICE_CLASS_DURATION,
        unit_of_measurement=UNIT_MILLISECOND,
    )
    .extend(
        {
            cv.GenerateID(CONF_AIRPLAY_ID): cv.use_id(AirPlayMediaSource),
            # Linked automatically when the node has a Sendspin hub.
            cv.OnlyWith(CONF_SENDSPIN_ID, "sendspin"): _sendspin_id_schema,
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
)


def _final_validate(config: ConfigType) -> ConfigType:
    full = fv.full_config.get()
    airplay = [
        c
        for c in full.get("media_source", [])
        if c.get(CONF_PLATFORM) == "airplay" and c[CONF_ID] == config[CONF_AIRPLAY_ID]
    ]
    if airplay and airplay[0][CONF_OUTPUT_DELAY].total_microseconds < 0:
        raise cv.Invalid(
            f"With the airplay number, '{CONF_OUTPUT_DELAY}' is only the first-boot "
            "value and must not be negative: Sendspin's static delay cannot be."
        )
    if CONF_SENDSPIN_ID not in config:
        return config
    sendspin_sources = [
        c for c in full.get("media_source", []) if c.get(CONF_PLATFORM) == "sendspin"
    ]
    if not sendspin_sources:
        raise cv.Invalid(
            "The airplay number is linked to Sendspin, but there is no "
            "'media_source: - platform: sendspin'. Add one, or set sendspin_id "
            "to a hub that has one.",
            [CONF_SENDSPIN_ID],
        )
    if not all(c.get(CONF_STATIC_DELAY_ADJUSTABLE) for c in sendspin_sources):
        raise cv.Invalid(
            "The airplay number sets Sendspin's static delay, which sendspin-cpp "
            f"ignores unless the Sendspin media source has "
            f"'{CONF_STATIC_DELAY_ADJUSTABLE}: true'.",
            [CONF_SENDSPIN_ID],
        )
    return config


FINAL_VALIDATE_SCHEMA = _final_validate


def _first_boot_ms(airplay_id) -> float:
    for c in CORE.config.get("media_source", []):
        if c.get(CONF_PLATFORM) == "airplay" and c[CONF_ID] == airplay_id:
            return c[CONF_OUTPUT_DELAY].total_microseconds / 1000.0
    return 0.0


async def to_code(config: ConfigType) -> None:
    var = await number.new_number(
        config, min_value=0, max_value=MAX_DELAY_MS, step=1
    )
    await cg.register_component(var, config)
    cg.add_define("USE_AIRPLAY_OUTPUT_DELAY_NUMBER")

    media_source = await cg.get_variable(config[CONF_AIRPLAY_ID])
    cg.add(var.set_media_source(media_source))
    cg.add(var.set_initial_value(_first_boot_ms(config[CONF_AIRPLAY_ID])))

    if CONF_SENDSPIN_ID in config:
        cg.add_define("USE_AIRPLAY_OUTPUT_DELAY_SENDSPIN")
        hub = await cg.get_variable(config[CONF_SENDSPIN_ID])
        cg.add(var.set_sendspin_hub(hub))
