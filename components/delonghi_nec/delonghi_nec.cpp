#include "delonghi_nec.h"

#include <cmath>
#include <cstdlib>

#include "esphome/core/log.h"

namespace esphome {
namespace delonghi_nec {

static const char *const TAG = "delonghi_nec";

namespace {

// Maps a Home Assistant fan mode onto a tracked speed. "auto" collapses to high
// when the current mode has no auto fan (fan_only).
FanSpeed fan_mode_to_speed(climate::ClimateFanMode mode, bool allow_auto) {
  switch (mode) {
    case climate::CLIMATE_FAN_LOW:
      return SPEED_LOW;
    case climate::CLIMATE_FAN_MEDIUM:
      return SPEED_MEDIUM;
    case climate::CLIMATE_FAN_HIGH:
      return SPEED_HIGH;
    case climate::CLIMATE_FAN_AUTO:
    default:
      return allow_auto ? SPEED_AUTO : SPEED_HIGH;
  }
}

}  // namespace

climate::ClimateMode DelonghiNEC::tracked_mode_() const {
  if (!this->power_on_)
    return climate::CLIMATE_MODE_OFF;
  if (this->auto_on_)
    return climate::CLIMATE_MODE_HEAT_COOL;
  switch (this->base_mode_) {
    case BASE_DRY:
      return climate::CLIMATE_MODE_DRY;
    case BASE_FAN_ONLY:
      return climate::CLIMATE_MODE_FAN_ONLY;
    case BASE_COOL:
    default:
      return climate::CLIMATE_MODE_COOL;
  }
}

climate::ClimateFanMode DelonghiNEC::tracked_fan_mode_() const {
  FanSpeed speed = this->fan_main_;
  if (this->power_on_ && !this->auto_on_) {
    if (this->base_mode_ == BASE_FAN_ONLY)
      speed = this->fan_fan_only_;
    else if (this->base_mode_ == BASE_DRY)
      speed = SPEED_AUTO;  // fan is forced to auto in dry
  }
  switch (speed) {
    case SPEED_LOW:
      return climate::CLIMATE_FAN_LOW;
    case SPEED_MEDIUM:
      return climate::CLIMATE_FAN_MEDIUM;
    case SPEED_HIGH:
      return climate::CLIMATE_FAN_HIGH;
    case SPEED_AUTO:
    default:
      return climate::CLIMATE_FAN_AUTO;
  }
}

void DelonghiNEC::publish_tracked_state_() {
  this->mode = this->tracked_mode_();
  this->target_temperature = this->temp_;
  this->fan_mode = this->tracked_fan_mode_();
  this->swing_mode = this->swing_ ? climate::CLIMATE_SWING_VERTICAL : climate::CLIMATE_SWING_OFF;
  this->preset = this->comfort_ ? climate::CLIMATE_PRESET_COMFORT : climate::CLIMATE_PRESET_NONE;
  this->publish_state();
}

void DelonghiNEC::transmit_state() {
  std::vector<uint16_t> cmds;
  const climate::ClimateMode target = this->mode;

  if (target == climate::CLIMATE_MODE_OFF) {
    if (this->power_on_) {
      cmds.push_back(CMD_POWER);
      this->power_on_ = false;
    }
    // Nothing else matters once the unit is off.
  } else {
    const bool target_auto = (target == climate::CLIMATE_MODE_HEAT_COOL);

    // Power on first; the unit resumes its previous auto/base/fan state.
    if (!this->power_on_) {
      cmds.push_back(CMD_POWER);
      this->power_on_ = true;
    }

    // Leave "auto" before touching the base mode (the home button toggles it).
    if (this->auto_on_ && !target_auto) {
      cmds.push_back(CMD_HOME);
      this->auto_on_ = false;
    }

    // Cycle the base mode with the mode button: cool -> dry -> fan_only -> cool.
    if (!target_auto) {
      BaseMode tb = this->base_mode_;
      if (target == climate::CLIMATE_MODE_COOL)
        tb = BASE_COOL;
      else if (target == climate::CLIMATE_MODE_DRY)
        tb = BASE_DRY;
      else if (target == climate::CLIMATE_MODE_FAN_ONLY)
        tb = BASE_FAN_ONLY;
      uint8_t delta = (3 + static_cast<uint8_t>(tb) - static_cast<uint8_t>(this->base_mode_)) % 3;
      for (uint8_t i = 0; i < delta; i++)
        cmds.push_back(CMD_MODE);
      this->base_mode_ = tb;
    }

    // Enter "auto" once the base mode is settled.
    if (!this->auto_on_ && target_auto) {
      cmds.push_back(CMD_HOME);
      this->auto_on_ = true;
    }

    const bool is_cool = (!this->auto_on_ && this->base_mode_ == BASE_COOL);
    const bool is_fan_only = (!this->auto_on_ && this->base_mode_ == BASE_FAN_ONLY);
    const bool is_dry = (!this->auto_on_ && this->base_mode_ == BASE_DRY);

    // Preset / silence: available everywhere except dry and fan_only.
    if (this->preset.has_value() && !is_dry && !is_fan_only) {
      const bool target_comfort = (this->preset.value() == climate::CLIMATE_PRESET_COMFORT);
      if (this->comfort_ != target_comfort) {
        cmds.push_back(CMD_SILENCE);
        this->comfort_ = target_comfort;
      }
    }

    // Fan: cycles only in cool (4 steps incl. auto) and fan_only (3 steps).
    if (this->fan_mode.has_value()) {
      if (is_cool) {
        FanSpeed tf = fan_mode_to_speed(this->fan_mode.value(), /*allow_auto=*/true);
        uint8_t delta = (4 + static_cast<uint8_t>(tf) - static_cast<uint8_t>(this->fan_main_)) % 4;
        for (uint8_t i = 0; i < delta; i++)
          cmds.push_back(CMD_FAN);
        this->fan_main_ = tf;
      } else if (is_fan_only) {
        FanSpeed tf = fan_mode_to_speed(this->fan_mode.value(), /*allow_auto=*/false);
        uint8_t delta = (3 + static_cast<uint8_t>(tf) - static_cast<uint8_t>(this->fan_fan_only_)) % 3;
        for (uint8_t i = 0; i < delta; i++)
          cmds.push_back(CMD_FAN);
        this->fan_fan_only_ = tf;
      }
    }

    // Temperature: cool mode only, one step per degree.
    if (is_cool) {
      int target_temp = static_cast<int>(lroundf(this->target_temperature));
      if (target_temp < static_cast<int>(TEMP_MIN))
        target_temp = static_cast<int>(TEMP_MIN);
      if (target_temp > static_cast<int>(TEMP_MAX))
        target_temp = static_cast<int>(TEMP_MAX);
      int delta = target_temp - this->temp_;
      uint16_t code = delta > 0 ? CMD_PLUS : CMD_MINUS;
      for (int i = 0; i < std::abs(delta); i++)
        cmds.push_back(code);
      this->temp_ = static_cast<int8_t>(target_temp);
    }

    // Swing: any active mode but auto.
    if (!this->auto_on_) {
      const bool target_swing = (this->swing_mode == climate::CLIMATE_SWING_VERTICAL);
      if (this->swing_ != target_swing) {
        cmds.push_back(CMD_SWING);
        this->swing_ = target_swing;
      }
    }
  }

  ESP_LOGD(TAG, "transmit_state: queuing %u command(s)", static_cast<unsigned>(cmds.size()));
  this->enqueue_and_send_(cmds);
}

void DelonghiNEC::enqueue_and_send_(const std::vector<uint16_t> &commands) {
  this->tx_seq_.clear();
  this->tx_index_ = 0;
  if (commands.empty())
    return;

  this->tx_seq_.emplace_back(SENTINEL_ADDR, SENTINEL_START);
  for (uint16_t c : commands)
    this->tx_seq_.emplace_back(BUTTON_ADDR, c);
  this->tx_seq_.emplace_back(SENTINEL_ADDR, SENTINEL_END);

  this->echo_suppress_ = true;
  this->send_next_();
}

void DelonghiNEC::send_next_() {
  if (this->tx_index_ >= this->tx_seq_.size()) {
    // The end sentinel has been emitted; release the echo guard.
    this->echo_suppress_ = false;
    this->tx_seq_.clear();
    return;
  }
  const auto &frame = this->tx_seq_[this->tx_index_];
  this->send_nec_(frame.first, frame.second);
  this->tx_index_++;
  this->set_timeout("delonghi_tx", SEND_INTERVAL_MS, [this]() { this->send_next_(); });
}

void DelonghiNEC::send_nec_(uint16_t address, uint16_t command) {
  remote_base::NECData data{};
  data.address = address;
  data.command = command;
  data.command_repeats = 1;
  auto call = this->transmitter_->transmit();
  remote_base::NECProtocol().encode(call.get_data(), data);
  call.set_send_times(1);
  call.set_send_wait(0);
  call.perform();
}

bool DelonghiNEC::on_receive(remote_base::RemoteReceiveData data) {
  auto decoded = remote_base::NECProtocol().decode(data);
  if (!decoded.has_value())
    return false;

  const uint16_t command = decoded->command;

  if (command == SENTINEL_START) {
    this->ext_suppress_ = true;
    return true;
  }
  if (command == SENTINEL_END) {
    this->ext_suppress_ = false;
    return true;
  }

  // Ignore our own echo and frames emitted by a cooperating transmitter.
  if (this->echo_suppress_ || this->ext_suppress_)
    return true;

  ESP_LOGD(TAG, "received remote command: 0x%04X", command);
  this->handle_remote_command_(command);
  return true;
}

void DelonghiNEC::handle_remote_command_(uint16_t command) {
  if (command == CMD_POWER) {
    this->power_on_ = !this->power_on_;
  } else if (this->power_on_) {
    const bool is_dry = (!this->auto_on_ && this->base_mode_ == BASE_DRY);
    const bool is_fan_only = (!this->auto_on_ && this->base_mode_ == BASE_FAN_ONLY);

    if (command == CMD_MODE) {
      if (this->auto_on_)
        this->auto_on_ = false;
      else
        this->base_mode_ = static_cast<BaseMode>((static_cast<uint8_t>(this->base_mode_) + 1) % 3);
    } else if (command == CMD_FAN) {
      if (!is_dry) {
        if (is_fan_only)
          this->fan_fan_only_ =
              static_cast<FanSpeed>((static_cast<uint8_t>(this->fan_fan_only_) + 1) % 3);
        else
          this->fan_main_ = static_cast<FanSpeed>((static_cast<uint8_t>(this->fan_main_) + 1) % 4);
      }
    } else if (command == CMD_PLUS) {
      if (!this->auto_on_ && this->base_mode_ == BASE_COOL && this->temp_ < static_cast<int8_t>(TEMP_MAX))
        this->temp_++;
    } else if (command == CMD_MINUS) {
      if (!this->auto_on_ && this->base_mode_ == BASE_COOL && this->temp_ > static_cast<int8_t>(TEMP_MIN))
        this->temp_--;
    } else if (command == CMD_SWING) {
      if (!this->auto_on_)
        this->swing_ = !this->swing_;
    } else if (command == CMD_HOME) {
      this->auto_on_ = !this->auto_on_;
    } else if (command == CMD_SILENCE) {
      if (!is_dry && !is_fan_only)
        this->comfort_ = !this->comfort_;
    }
    // CMD_TIMER is intentionally ignored: it has no climate representation.
  }

  this->publish_tracked_state_();
}

void DelonghiNEC::set_sync_temperature(float temperature) {
  int t = static_cast<int>(lroundf(temperature));
  if (t < static_cast<int>(TEMP_MIN))
    t = static_cast<int>(TEMP_MIN);
  if (t > static_cast<int>(TEMP_MAX))
    t = static_cast<int>(TEMP_MAX);
  this->temp_ = static_cast<int8_t>(t);
  ESP_LOGI(TAG, "manual resync: temperature = %d", t);
  this->publish_tracked_state_();
}

void DelonghiNEC::set_sync_mode(const std::string &mode) {
  if (mode == "off") {
    this->power_on_ = false;
  } else if (mode == "auto") {
    this->power_on_ = true;
    this->auto_on_ = true;
  } else if (mode == "cool" || mode == "heat") {
    // The unit has no real heat mode; treat "heat" as cool for resync purposes.
    this->power_on_ = true;
    this->auto_on_ = false;
    this->base_mode_ = BASE_COOL;
  } else if (mode == "dry") {
    this->power_on_ = true;
    this->auto_on_ = false;
    this->base_mode_ = BASE_DRY;
  } else if (mode == "fan_only") {
    this->power_on_ = true;
    this->auto_on_ = false;
    this->base_mode_ = BASE_FAN_ONLY;
  } else {
    ESP_LOGW(TAG, "manual resync: unknown mode '%s'", mode.c_str());
    return;
  }
  ESP_LOGI(TAG, "manual resync: mode = %s", mode.c_str());
  this->publish_tracked_state_();
}

void DelonghiNEC::set_sync_fan(const std::string &fan) {
  FanSpeed speed;
  if (fan == "low")
    speed = SPEED_LOW;
  else if (fan == "medium")
    speed = SPEED_MEDIUM;
  else if (fan == "high")
    speed = SPEED_HIGH;
  else if (fan == "auto")
    speed = SPEED_AUTO;
  else {
    ESP_LOGW(TAG, "manual resync: unknown fan '%s'", fan.c_str());
    return;
  }
  // Set the appropriate fan depending on current mode.
  if (!this->auto_on_ && this->base_mode_ == BASE_FAN_ONLY) {
    // In fan_only mode, set fan_fan_only_ (no auto step).
    this->fan_fan_only_ = (speed == SPEED_AUTO) ? SPEED_HIGH : speed;
  } else {
    this->fan_main_ = speed;
  }
  ESP_LOGI(TAG, "manual resync: fan = %s", fan.c_str());
  this->publish_tracked_state_();
}

void DelonghiNEC::set_sync_swing(bool swing) {
  this->swing_ = swing;
  ESP_LOGI(TAG, "manual resync: swing = %s", swing ? "on" : "off");
  this->publish_tracked_state_();
}

void DelonghiNEC::set_sync_comfort(bool comfort) {
  this->comfort_ = comfort;
  ESP_LOGI(TAG, "manual resync: comfort = %s", comfort ? "on" : "off");
  this->publish_tracked_state_();
}

}  // namespace delonghi_nec
}  // namespace esphome
