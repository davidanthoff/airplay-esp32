#pragma once

#include "esphome/core/defines.h"

// ESPHome compiles every top-level file of this component, so this one must
// build without the number component too (see number.py).
#if defined(USE_ESP32) && defined(USE_AIRPLAY_OUTPUT_DELAY_NUMBER)

#include "esphome/components/number/number.h"
#include "esphome/core/component.h"
#include "esphome/core/preferences.h"

#include "airplay_media_source.h"

#ifdef USE_AIRPLAY_OUTPUT_DELAY_SENDSPIN
#include "esphome/components/sendspin/sendspin_hub.h"
#endif

#include <cstdint>

namespace esphome::airplay {

/// @brief One "output delay" setting, in ms, for the hardware after the ESP32.
///
/// Feeds the AirPlay media source's output delay and, when linked, Sendspin's
/// static delay. Both mean "play this much earlier".
///
/// This entity is the only authority. If the Sendspin server sets a different
/// static delay (Music Assistant re-sends its stored value when the player
/// loads), it is set back, and Music Assistant then stores the reported value.
/// Main loop only.
class OutputDelayNumber final : public number::Number, public Component {
 public:
  void setup() override;
  void dump_config() override;

  void set_media_source(AirPlayMediaSource *media_source) { this->media_source_ = media_source; }
  void set_initial_value(float initial_value) { this->initial_value_ = initial_value; }
#ifdef USE_AIRPLAY_OUTPUT_DELAY_SENDSPIN
  void set_sendspin_hub(sendspin_::SendspinHub *hub) { this->sendspin_hub_ = hub; }
#endif

 protected:
  void control(float value) override;
  void apply_(uint16_t delay_ms);
#ifdef USE_AIRPLAY_OUTPUT_DELAY_SENDSPIN
  /// Polled: pushes the delay into Sendspin once its player exists, and sets
  /// it back after a server-side change.
  void sync_sendspin_();
  sendspin_::SendspinHub *sendspin_hub_{nullptr};
#endif

  AirPlayMediaSource *media_source_{nullptr};
  float initial_value_{0.0f};
  uint16_t delay_ms_{0};
  ESPPreferenceObject pref_;
};

}  // namespace esphome::airplay

#endif  // USE_ESP32 && USE_AIRPLAY_OUTPUT_DELAY_NUMBER
