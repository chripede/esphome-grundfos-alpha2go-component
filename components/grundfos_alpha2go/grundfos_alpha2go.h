#pragma once

#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/components/ble_client/ble_client.h"
#include "esphome/components/esp32_ble_tracker/esp32_ble_tracker.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/switch/switch.h"
#include "esphome/components/select/select.h"
#include "esphome/components/number/number.h"
#include "esphome/components/button/button.h"
#include "geni.h"

#include <cmath>
#include <vector>

#ifdef USE_ESP32

#include <esp_gap_ble_api.h>
#include <esp_gattc_api.h>

namespace esphome {
namespace grundfos_alpha2go {

namespace espbt = esphome::esp32_ble_tracker;

// Setpoint unit per control mode: pressure (Pa), flow (m3/s), speed (1/min)
enum SetpointKind : uint8_t { SP_UNKNOWN = 0, SP_PRESSURE = 1, SP_FLOW = 2, SP_SPEED = 3 };
enum class SwitchKind : uint8_t { PUMP = 0, NIGHT_SETBACK = 1, AIR_DETECTION = 2 };
enum class ButtonKind : uint8_t { VENTING_START = 0, VENTING_STOP = 1 };

class GrundfosAlpha2Go : public ble_client::BLEClientNode, public PollingComponent {
  SUB_SENSOR(flow)
  SUB_SENSOR(head)
  SUB_SENSOR(power)
  SUB_SENSOR(voltage)
  SUB_SENSOR(dc_voltage)
  SUB_SENSOR(current)
  SUB_SENSOR(speed)
  SUB_SENSOR(aux_temperature)
  SUB_SENSOR(media_temperature)
  SUB_SENSOR(pcb_temperature)
  SUB_SENSOR(controlbox_temperature)
  SUB_SENSOR(starts)
  SUB_SENSOR(operating_hours)
  SUB_SENSOR(power_on_hours)
  SUB_SENSOR(energy)          // 87/1 system_energy (J) -> kWh
  SUB_SENSOR(operation_mode)  // 86/7 operation_mode (0=Auto, 1=Stop, 2=Min, 3=Max, ...)
  SUB_SENSOR(control_mode)    // 86/7 control_mode (1=PP, 2=CS, 5=AutoAdapt, 13..15=AutoAdapt variants, ...)
  SUB_SENSOR(setpoint)        // 86/7 set_point, raw SI value (Pa or m3/s)
  SUB_SENSOR(setpoint_head)   // 86/7 set_point in m, pressure control modes only
  SUB_SENSOR(setpoint_flow)   // 86/7 set_point in m3/h, flow control mode only
  SUB_SENSOR(setpoint_speed)  // 86/7 set_point in 1/min, constant speed only
  SUB_SENSOR(setpoint_actual) // 86/8 current target in m, pressure control modes only
  SUB_TEXT_SENSOR(operation_mode)  // operation_mode as text
  SUB_TEXT_SENSOR(control_mode)    // control_mode as text
  SUB_SENSOR(air_detections)       // 88/604 air detections after 168 operating hours
  SUB_TEXT_SENSOR(venting_status)  // 88/612 venting: inactive / manual / automatic
  SUB_BINARY_SENSOR(night_setback_enabled)  // 90/8 night setback enabled
  SUB_BINARY_SENSOR(night_mode)             // 90/7 currently in night mode
  SUB_BINARY_SENSOR(air_detection_enabled)  // 88/600 continuous air detection enabled
  SUB_BINARY_SENSOR(venting_active)         // 88/612 venting running
  SUB_BINARY_SENSOR(connected)              // connected, pump being polled

 public:
  // Controls (switch/select/number/button platforms)
  void set_pump_switch(switch_::Switch *s) { this->pump_switch_ = s; }
  void set_night_setback_switch(switch_::Switch *s) { this->night_setback_switch_ = s; }
  void set_air_detection_switch(switch_::Switch *s) { this->air_detection_switch_ = s; }
  void set_control_mode_select(select::Select *s) { this->control_mode_select_ = s; }
  void set_setpoint_head_number(number::Number *n) { this->setpoint_head_number_ = n; }
  void set_setpoint_flow_number(number::Number *n) { this->setpoint_flow_number_ = n; }
  void set_setpoint_speed_number(number::Number *n) { this->setpoint_speed_number_ = n; }
  void set_venting_buttons_present() { this->venting_buttons_present_ = true; }

  // Writes (called by the entities). Returns false if nothing is sent.
  bool write_switch(SwitchKind kind, bool state);
  bool write_control_mode(uint8_t ctrl);
  bool write_setpoint(SetpointKind kind, float value);  // value in the entity's unit (m, m3/h, 1/min)
  bool press_button(ButtonKind kind);

  // Diagnostics: send any Class 10 APDU (header byte + data), reply is logged
  void send_raw_class10(const std::vector<uint8_t> &apdu);

  void setup() override;
  void loop() override;
  void update() override;
  void dump_config() override;
  void gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                           esp_ble_gattc_cb_param_t *param) override;
  void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) override;

 protected:
  enum class Stage : uint8_t {
    IDLE,        // not connected
    WAIT_ENC,    // encryption requested, waiting for AUTH_CMPL and service discovery
    WAIT_CCCD,   // CCCD written, waiting for confirmation
    HELLO,       // send the GO app's initial query
    POLLING,     // normal operation
  };
  enum Request : uint16_t {
    REQ_ELECTRIC = 1 << 0,       // 87/69
    REQ_FLOW = 1 << 1,           // 93/290
    REQ_COUNTERS = 1 << 2,       // 93/1
    REQ_TEMPERATURE = 1 << 3,    // 93/300
    REQ_ENERGY = 1 << 4,         // 87/1
    REQ_OP_LOCAL = 1 << 5,       // 86/6 local request (app/button) -> controls
    REQ_OP_PRIO = 1 << 6,        // 86/7 prioritised request -> sensors
    REQ_OP_REALIZED = 1 << 7,    // 86/8 realised request
    REQ_NIGHT_ENABLE = 1 << 8,   // 90/8 night setback enabled
    REQ_NIGHT_STATUS = 1 << 9,   // 90/7 currently in night mode
    REQ_AIR_CONFIG = 1 << 10,    // 88/600 air detection config
    REQ_AIR_COUNTER = 1 << 11,   // 88/604 air detection counter
    REQ_VENTING = 1 << 12,       // 88/612 venting status
    REQ_OP_TEMP = 1 << 13,       // 86/9 temporary control (candidate for "vent pump only")
  };
  // Log raw notifications of these objects at VERBOSE
  static constexpr uint16_t REQ_DIAG_MASK = REQ_ENERGY | REQ_OP_LOCAL | REQ_OP_PRIO | REQ_OP_REALIZED |
                                            REQ_NIGHT_ENABLE | REQ_NIGHT_STATUS | REQ_AIR_CONFIG |
                                            REQ_AIR_COUNTER | REQ_VENTING | REQ_OP_TEMP;
  // Rarely changing settings: only every SLOW_EVERY-th update() (and after connecting)
  static constexpr uint16_t REQ_SLOW_MASK = REQ_NIGHT_ENABLE | REQ_AIR_CONFIG | REQ_AIR_COUNTER;
  static constexpr uint8_t SLOW_EVERY = 6;
  // Read back immediately after a write
  static constexpr uint16_t REQ_AFTER_WRITE = REQ_OP_LOCAL | REQ_OP_PRIO | REQ_OP_REALIZED | REQ_NIGHT_ENABLE |
                                              REQ_AIR_CONFIG | REQ_VENTING | REQ_OP_TEMP | REQ_ELECTRIC;
  static constexpr uint16_t REQ_WRITE = 1u << 15;  // pseudo-bit for last_req_ (not in REQUESTS)
  static const char *req_name_(uint16_t req);

  void reset_state_();
  void write_cccd_();
  void send_frame_(const uint8_t *frame, size_t len);
  void send_next_request_();
  void handle_reply_(const geni::Reply &r);
  void check_write_reply_(const uint8_t *d, size_t n);
  void publish_(sensor::Sensor *s, float value);
  void publish_or_nan_(sensor::Sensor *s, float value);  // NaN is sent too (value currently not applicable)
  void publish_text_(text_sensor::TextSensor *s, const char *value);
  void publish_bin_(binary_sensor::BinarySensor *s, bool value);
  void publish_venting_();
  void publish_controls_();  // update controls from 86/6
  void publish_all_unknown_();  // after disconnect: all entities to "unknown"

  // Write queue: frames wait until no query is in flight
  struct PendingWrite {
    uint8_t buf[geni::MAX_TX_FRAME];
    uint8_t len;
    const char *label;  // static text
  };
  static constexpr uint8_t WRITE_QUEUE = 4;
  bool queue_write_(const uint8_t *frame, size_t len, const char *label);
  bool queue_set_operation_(uint8_t mode, uint8_t ctrl, float setpoint_si, const char *label);
  void finish_write_(const char *why);
  PendingWrite writes_[WRITE_QUEUE]{};
  uint8_t write_head_{0};
  uint8_t write_count_{0};
  const char *write_label_{""};  // write currently being sent

  Stage stage_{Stage::IDLE};
  uint16_t data_handle_{0};
  uint16_t cccd_handle_{0};
  bool handles_ready_{false};
  bool enc_ok_{false};
  bool auth_failed_{false};      // encryption rejected on this connection
  uint8_t auth_fail_count_{0};   // consecutive rejections across connections
  bool stack_reset_pending_{false};  // restart BLE stack (bond in NVS but no longer in the stack's RAM)
  // Shared by all pumps: the restart affects the whole BLE stack, so once for all
  static uint32_t t_last_stack_reset_;
  uint8_t bond_count_(bool *stored);  // number of bonds in NVS; *stored = bond exists for this pump
  uint8_t cccd_tries_{0};
  uint8_t hello_sent_{0};
  uint32_t t_stage_{0};
  uint32_t t_last_tx_{0};
  bool inflight_{false};
  uint16_t last_req_{0};  // last query sent (only one is ever in flight)
  uint16_t wanted_mask_{0};
  uint16_t pending_mask_{0};
  uint8_t update_count_{0};
  uint8_t system_venting_state_{0};  // 88/612: 0 inactive, 1 manual, 2 automatic
  bool pump_venting_{false};         // 86/8 control source AirVenting
  geni::Reassembler rx_;

  // State for the controls
  bool venting_buttons_present_{false};
  bool op_valid_{false};  // 86/6 read
  uint8_t op_src_{0}, op_mode_{0}, op_ctrl_{0};
  float op_sp_{0};
  float last_sp_[4]{NAN, NAN, NAN, NAN};  // last seen setpoint per SetpointKind (SI)
  bool air_cfg_valid_{false};
  uint8_t air_cfg_[8]{};  // 88/600 raw, for toggling "enabled" (byte 7)
  switch_::Switch *pump_switch_{nullptr};
  switch_::Switch *night_setback_switch_{nullptr};
  switch_::Switch *air_detection_switch_{nullptr};
  select::Select *control_mode_select_{nullptr};
  number::Number *setpoint_head_number_{nullptr};
  number::Number *setpoint_flow_number_{nullptr};
  number::Number *setpoint_speed_number_{nullptr};
};

// ---- Entities ----
class GrundfosSwitch : public switch_::Switch, public Parented<GrundfosAlpha2Go> {
 public:
  void set_kind(SwitchKind kind) { this->kind_ = kind; }
  void invalidate();  // state "unknown"; the next publish_state() is guaranteed to go through

 protected:
  void write_state(bool state) override;
  SwitchKind kind_{SwitchKind::PUMP};
};

// Options in the order of CONTROL_MODE_OPTIONS in select.py
class GrundfosControlModeSelect : public select::Select, public Parented<GrundfosAlpha2Go> {
 protected:
  void control(size_t index) override;
};

class GrundfosSetpointNumber : public number::Number, public Parented<GrundfosAlpha2Go> {
 public:
  void set_kind(SetpointKind kind) { this->kind_ = kind; }

 protected:
  void control(float value) override;
  SetpointKind kind_{SP_PRESSURE};
};

class GrundfosButton : public button::Button, public Parented<GrundfosAlpha2Go> {
 public:
  void set_kind(ButtonKind kind) { this->kind_ = kind; }

 protected:
  void press_action() override;
  ButtonKind kind_{ButtonKind::VENTING_START};
};

}  // namespace grundfos_alpha2go
}  // namespace esphome

#endif
