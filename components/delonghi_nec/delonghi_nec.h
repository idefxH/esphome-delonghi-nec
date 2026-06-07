#pragma once

#include <utility>
#include <vector>

#include "esphome/components/climate_ir/climate_ir.h"
#include "esphome/components/remote_base/nec_protocol.h"

namespace esphome {
namespace delonghi_nec {

// Temperature range exposed to Home Assistant (cool setpoint).
static constexpr float TEMP_MIN = 16.0f;
static constexpr float TEMP_MAX = 32.0f;

// Delay between two consecutive IR frames. The unit needs a moment to digest
// each toggle, so frames are spread out with a non-blocking timeout.
static constexpr uint32_t SEND_INTERVAL_MS = 300;

// NEC address used for the actual remote buttons.
static constexpr uint16_t BUTTON_ADDR = 0xFF1F;

// Dedicated address + commands used as transmission markers. They let a second
// cooperating ESP (and this unit itself) know that the frames in between were
// emitted by the controller and must not be treated as a real keypress.
static constexpr uint16_t SENTINEL_ADDR = 0x002F;
static constexpr uint16_t SENTINEL_START = 0x9999;
static constexpr uint16_t SENTINEL_END = 0xFFFF;

// Pinguino remote button codes (NEC command, address 0xFF1F).
static constexpr uint16_t CMD_POWER = 0x7C83;    // 31875
static constexpr uint16_t CMD_MODE = 0x3EC1;     // 16065
static constexpr uint16_t CMD_FAN = 0x3FC0;      // 16320
static constexpr uint16_t CMD_PLUS = 0x7B84;     // 31620
static constexpr uint16_t CMD_MINUS = 0x7F80;    // 32640
static constexpr uint16_t CMD_TIMER = 0x7E1D;    // 32285
static constexpr uint16_t CMD_SWING = 0x7A85;    // 31365
static constexpr uint16_t CMD_HOME = 0x7D82;     // 32130
static constexpr uint16_t CMD_SILENCE = 0x7986;  // 31110

// The three modes the unit cycles through with the "mode" button.
enum BaseMode : uint8_t {
  BASE_COOL = 0,
  BASE_DRY = 1,
  BASE_FAN_ONLY = 2,
};

// Fan speeds. "auto" only exists outside fan_only mode.
enum FanSpeed : uint8_t {
  SPEED_LOW = 0,
  SPEED_MEDIUM = 1,
  SPEED_HIGH = 2,
  SPEED_AUTO = 3,
};

class DelonghiNEC : public climate_ir::ClimateIR {
 public:
  DelonghiNEC()
      : climate_ir::ClimateIR(
            TEMP_MIN, TEMP_MAX, 1.0f, /*supports_dry=*/true, /*supports_fan_only=*/true,
            {climate::CLIMATE_FAN_AUTO, climate::CLIMATE_FAN_LOW, climate::CLIMATE_FAN_MEDIUM,
             climate::CLIMATE_FAN_HIGH},
            {climate::CLIMATE_SWING_OFF, climate::CLIMATE_SWING_VERTICAL},
            {climate::CLIMATE_PRESET_NONE, climate::CLIMATE_PRESET_COMFORT}) {}

  // Manual resync helpers. They overwrite the tracked state without emitting any
  // IR, so the controller's idea of the unit can be realigned when they drift
  // apart (power loss, missed frame, remote used out of range, ...).
  void set_sync_temperature(float temperature);
  void set_sync_mode(const std::string &mode);
  void set_sync_fan(const std::string &fan);

 protected:
  // Diff the requested Home Assistant state against the tracked state and queue
  // the toggle commands required to close the gap.
  void transmit_state() override;

  // Decode an incoming NEC frame and mirror physical remote presses into the
  // tracked state.
  bool on_receive(remote_base::RemoteReceiveData data) override;

  void handle_remote_command_(uint16_t command);
  void publish_tracked_state_();

  // Transmission queue handling.
  void enqueue_and_send_(const std::vector<uint16_t> &commands);
  void send_next_();
  void send_nec_(uint16_t address, uint16_t command);

  // Translation between the tracked state and the climate component fields.
  climate::ClimateMode tracked_mode_() const;
  climate::ClimateFanMode tracked_fan_mode_() const;

  // --- Tracked physical state of the AC unit ---
  bool power_on_{false};
  bool auto_on_{false};
  BaseMode base_mode_{BASE_COOL};
  int8_t temp_{24};
  FanSpeed fan_main_{SPEED_AUTO};       // fan used in cool / auto
  FanSpeed fan_fan_only_{SPEED_HIGH};   // fan used in fan_only (no "auto")
  bool swing_{false};
  bool comfort_{false};

  // While true, received frames are our own echo and must be ignored.
  bool echo_suppress_{false};
  // Set/cleared by the start/end sentinels of another transmitter.
  bool ext_suppress_{false};

  // Pending (address, command) frames being flushed one every SEND_INTERVAL_MS.
  std::vector<std::pair<uint16_t, uint16_t>> tx_seq_;
  size_t tx_index_{0};
};

}  // namespace delonghi_nec
}  // namespace esphome
