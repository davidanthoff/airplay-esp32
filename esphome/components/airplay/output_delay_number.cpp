#include "output_delay_number.h"

#if defined(USE_ESP32) && defined(USE_AIRPLAY_OUTPUT_DELAY_NUMBER)

#include "esphome/core/log.h"

#include <cmath>

namespace esphome::airplay {

static const char *const TAG = "airplay.number";

#ifdef USE_AIRPLAY_OUTPUT_DELAY_SENDSPIN
static constexpr uint32_t SENDSPIN_SYNC_INTERVAL_MS = 1000;
#endif

void OutputDelayNumber::setup() {
  this->pref_ = this->make_entity_preference<float>();
  float value;
  if (!this->pref_.load(&value)) {
    value = this->initial_value_;
  }
  value = std::round(clamp(value, this->traits.get_min_value(), this->traits.get_max_value()));
  this->apply_(static_cast<uint16_t>(value));
#ifdef USE_AIRPLAY_OUTPUT_DELAY_SENDSPIN
  // The Sendspin player does not exist before the hub's setup, and the server
  // may change the delay at any time: check once a second.
  this->set_interval("sendspin", SENDSPIN_SYNC_INTERVAL_MS, [this]() { this->sync_sendspin_(); });
#endif
}

void OutputDelayNumber::dump_config() {
  LOG_NUMBER("", "AirPlay Output Delay Number", this);
  ESP_LOGCONFIG(TAG, "  Applies to: AirPlay%s",
#ifdef USE_AIRPLAY_OUTPUT_DELAY_SENDSPIN
                " and Sendspin"
#else
                ""
#endif
  );
}

void OutputDelayNumber::control(float value) {
  value = std::round(clamp(value, this->traits.get_min_value(), this->traits.get_max_value()));
  this->pref_.save(&value);
  this->apply_(static_cast<uint16_t>(value));
  ESP_LOGI(TAG, "Output delay set to %u ms", this->delay_ms_);
}

void OutputDelayNumber::apply_(uint16_t delay_ms) {
  this->delay_ms_ = delay_ms;
  this->media_source_->set_output_delay_us(static_cast<int32_t>(delay_ms) * 1000);
#ifdef USE_AIRPLAY_OUTPUT_DELAY_SENDSPIN
  this->sync_sendspin_();
#endif
  this->publish_state(delay_ms);
}

#ifdef USE_AIRPLAY_OUTPUT_DELAY_SENDSPIN
void OutputDelayNumber::sync_sendspin_() {
  sendspin::PlayerRole *player = this->sendspin_hub_->get_player_role();
  if (player == nullptr) {
    return;
  }
  uint16_t current = player->get_output_delay_ms();
  if (current == this->delay_ms_) {
    return;
  }
  // Also the first push after boot, and a server-side change (Music Assistant
  // re-sends its stored value when the player loads). update_output_delay()
  // stores the value on the device and reports it to the server.
  ESP_LOGD(TAG, "Sendspin output delay %u ms -> %u ms", current, this->delay_ms_);
  player->update_output_delay(this->delay_ms_);
}
#endif

}  // namespace esphome::airplay

#endif  // USE_ESP32 && USE_AIRPLAY_OUTPUT_DELAY_NUMBER
