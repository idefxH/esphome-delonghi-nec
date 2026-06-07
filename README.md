# esphome-delonghi-nec

An [ESPHome](https://esphome.io/) external component to control a **De'Longhi
Pinguino** portable air conditioner over infrared.

Unlike the better-known De'Longhi A/C protocol (a 64-bit stateful frame), the
Pinguino remote speaks plain **32-bit NEC** where every button is a fixed code
and the unit cycles/increments its own state internally. There is no way to send
an absolute "set temperature to 24°C" command — you can only press *temperature
up* and *temperature down*.

This component works around that by **tracking the unit's state on the ESP** and
translating a Home Assistant request into the right sequence of button presses.

## How it works

- The component inherits from `climate_ir::ClimateIR` and exposes a normal
  Home Assistant climate entity.
- It keeps an internal model of the unit (power, mode, temperature, fan, swing,
  silence/comfort preset).
- On every change requested from Home Assistant, `transmit_state()` diffs the
  requested state against the tracked state and queues only the NEC frames
  needed to close the gap — e.g. going from 24 → 26 °C queues *temperature up*
  twice.
- Frames are sent **one every 300 ms** using a non-blocking timeout so the unit
  has time to register each toggle.
- A connected `remote_receiver` lets the component also follow the **physical
  remote**: incoming NEC frames are decoded and the tracked state is updated and
  republished to Home Assistant.

### Echo suppression

While the component is transmitting it wraps its frames between two sentinel
codes on a dedicated NEC address (`0x002F`):

| Frame   | Meaning                          |
| ------- | -------------------------------- |
| `0x9999`| start of a controller burst      |
| `0xFFFF`| end of a controller burst        |

During its own burst the component ignores everything it receives, and the
sentinels let a **second cooperating ESP** ignore the burst too. This prevents
the receiver from mistaking the controller's own output for a remote keypress.

## Supported features

| Climate field | Values                                            |
| ------------- | ------------------------------------------------- |
| Mode          | off, cool, dry, fan_only, heat_cool (*auto*)      |
| Temperature   | 16–32 °C, 1 °C steps (cool mode only)             |
| Fan           | low, medium, high, auto                           |
| Swing         | off, vertical                                     |
| Preset        | none, comfort (*silence*)                         |

Notes that mirror the real unit's behaviour:

- Temperature only changes in **cool** mode.
- Fan cycles `low → medium → high → auto` in cool, and `low → medium → high` in
  fan_only (no auto). In dry mode the fan is forced to auto.
- The silence/comfort preset is unavailable in dry and fan_only.

## Installation

```yaml
external_components:
  - source: github://idefxH/esphome-delonghi-nec
    components: [delonghi_nec]
```

See [`example.yaml`](example.yaml) for a complete configuration.

## Configuration

```yaml
remote_transmitter:
  id: trsmt
  pin: GPIO26
  carrier_duty_percent: 50%

remote_receiver:
  id: rcvr
  pin:
    number: GPIO32
    inverted: true
    mode:
      input: true
      pullup: true
  tolerance: 55%

climate:
  - platform: delonghi_nec
    id: clim
    name: "Climatisation"
    transmitter_id: trsmt
    receiver_id: rcvr
```

All options from
[`climate_ir`](https://esphome.io/components/climate/climate_ir.html) are
supported (`sensor`, `receiver_id`, etc.).

## Manual resync

Because the protocol is stateless, the ESP's idea of the unit can drift from
reality (power cut, a frame lost to interference, the remote used while the ESP
was offline). The component exposes three setters to realign it **without
sending any IR**:

```cpp
void set_sync_temperature(float temperature);  // 16–32
void set_sync_mode(const std::string &mode);   // off/cool/heat/fan_only/dry/auto
void set_sync_fan(const std::string &fan);     // low/medium/high/auto
```

`example.yaml` wires these to template `number` and `select` entities so they
show up in Home Assistant:

```yaml
number:
  - platform: template
    name: "Clim resync temperature"
    min_value: 16
    max_value: 32
    step: 1
    optimistic: true
    set_action:
      - lambda: 'id(clim)->set_sync_temperature(x);'

select:
  - platform: template
    name: "Clim resync mode"
    options: ["off", "cool", "heat", "fan_only", "dry", "auto"]
    optimistic: true
    set_action:
      - lambda: 'id(clim)->set_sync_mode(x);'
```

## NEC codes

The button codes (NEC command on address `0xFF1F`):

| Button   | Hex      | Decimal |
| -------- | -------- | ------- |
| power    | `0x7C83` | 31875   |
| mode     | `0x3EC1` | 16065   |
| fan      | `0x3FC0` | 16320   |
| temp +   | `0x7B84` | 31620   |
| temp −   | `0x7F80` | 32640   |
| timer    | `0x7E1D` | 32285   |
| swing    | `0x7A85` | 31365   |
| home     | `0x7D82` | 32130   |
| silence  | `0x7986` | 31110   |

## License

MIT
