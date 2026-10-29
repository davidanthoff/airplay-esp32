"""AirPlay 2 receiver for ESPHome.

The user-facing piece is the media source platform in ``media_source.py``::

    media_source:
      - platform: airplay
        id: airplay_media_source

This module only holds what is shared across platforms.
"""

import esphome.codegen as cg

CODEOWNERS = ["@davidanthoff"]

airplay_ns = cg.esphome_ns.namespace("airplay")
