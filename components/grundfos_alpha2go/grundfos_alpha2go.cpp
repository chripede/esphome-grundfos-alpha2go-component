#include "grundfos_alpha2go.h"
#include <cstring>
#include <string>
#include "esphome/core/log.h"
#include "esphome/components/esp32_ble/ble.h"
#include <esp_task_wdt.h>
#include "esphome/core/helpers.h"
#include "esphome/core/hal.h"
#include "esphome/core/controller_registry.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

#ifdef USE_ESP32

namespace esphome {
namespace grundfos_alpha2go {

static const char *const TAG = "grundfos_alpha2go";

// Hex dump for logs. ESPHome 2026.11 removed the std::string format_hex_pretty(); format_hex_pretty_to()
// exists in 2026.10 and later, so this builds on both.
static std::string hex_str_(const uint8_t *data, size_t len) {
  std::string out(len * 3 + 1, '\0');
  format_hex_pretty_to(&out[0], out.size(), data, len, '.');
  out.resize(std::strlen(out.c_str()));
  return out;
}

static const uint32_t ENC_TIMEOUT_MS = 1500;    // wait this long for AUTH_CMPL, then continue anyway
static const uint32_t CCCD_TIMEOUT_MS = 2000;
static const uint32_t REPLY_TIMEOUT_MS = 1000;  // same as the GO app (GENI_TIMEOUT)
static const uint32_t STACK_RESET_MIN_GAP_MS = 30000;  // restart BLE stack at most every 30 s
static const uint32_t BLE_RESET_WDT_MS = 30000;        // task watchdog during stack restart
static const uint32_t AUTH_FAIL_DISCONNECT_MS = 5000; // disconnect by then at the latest after rejected encryption
static const uint32_t MIN_TX_GAP_MS = 30;       // pump replies after ~80 ms; one request at a time

uint32_t GrundfosAlpha2Go::t_last_stack_reset_ = 0;

static const espbt::ESPBTUUID GENI_SERVICE = espbt::ESPBTUUID::from_uint16(0xFE5D);
static const espbt::ESPBTUUID GENI_CHAR = espbt::ESPBTUUID::from_raw("859cffd1-036e-432a-aa28-1a0085b87ba9");

void GrundfosAlpha2Go::dump_config() {
  ESP_LOGCONFIG(TAG, "Grundfos ALPHA2 GO");
  LOG_SENSOR("  ", "Flow", this->flow_sensor_);
  LOG_SENSOR("  ", "Head", this->head_sensor_);
  LOG_SENSOR("  ", "Power", this->power_sensor_);
  LOG_SENSOR("  ", "Voltage", this->voltage_sensor_);
  LOG_SENSOR("  ", "DC voltage", this->dc_voltage_sensor_);
  LOG_SENSOR("  ", "Current", this->current_sensor_);
  LOG_SENSOR("  ", "Speed", this->speed_sensor_);
  LOG_SENSOR("  ", "Aux temperature", this->aux_temperature_sensor_);
  LOG_SENSOR("  ", "Media temperature", this->media_temperature_sensor_);
  LOG_SENSOR("  ", "PCB temperature", this->pcb_temperature_sensor_);
  LOG_SENSOR("  ", "Controlbox temperature", this->controlbox_temperature_sensor_);
  LOG_SENSOR("  ", "Starts", this->starts_sensor_);
  LOG_SENSOR("  ", "Operating hours", this->operating_hours_sensor_);
  LOG_SENSOR("  ", "Power-on hours", this->power_on_hours_sensor_);
  LOG_SENSOR("  ", "Energy", this->energy_sensor_);
  LOG_SENSOR("  ", "Operation mode", this->operation_mode_sensor_);
  LOG_SENSOR("  ", "Control mode", this->control_mode_sensor_);
  LOG_SENSOR("  ", "Setpoint (raw, SI)", this->setpoint_sensor_);
  LOG_SENSOR("  ", "Setpoint head", this->setpoint_head_sensor_);
  LOG_SENSOR("  ", "Setpoint flow", this->setpoint_flow_sensor_);
  LOG_SENSOR("  ", "Setpoint speed", this->setpoint_speed_sensor_);
  LOG_SENSOR("  ", "Setpoint actual", this->setpoint_actual_sensor_);
  LOG_SENSOR("  ", "Setpoint actual speed", this->setpoint_actual_speed_sensor_);
  LOG_TEXT_SENSOR("  ", "Control source", this->control_source_text_sensor_);
  LOG_TEXT_SENSOR("  ", "Operation mode text", this->operation_mode_text_sensor_);
  LOG_TEXT_SENSOR("  ", "Control mode text", this->control_mode_text_sensor_);
  LOG_SENSOR("  ", "Air detections", this->air_detections_sensor_);
  LOG_TEXT_SENSOR("  ", "Venting status", this->venting_status_text_sensor_);
  LOG_BINARY_SENSOR("  ", "Night setback enabled", this->night_setback_enabled_binary_sensor_);
  LOG_BINARY_SENSOR("  ", "Night mode", this->night_mode_binary_sensor_);
  LOG_BINARY_SENSOR("  ", "Air detection enabled", this->air_detection_enabled_binary_sensor_);
  LOG_BINARY_SENSOR("  ", "Venting active", this->venting_active_binary_sensor_);
  LOG_BINARY_SENSOR("  ", "Connected", this->connected_binary_sensor_);
  LOG_SWITCH("  ", "Pump", this->pump_switch_);
  LOG_SWITCH("  ", "Night setback", this->night_setback_switch_);
  LOG_SWITCH("  ", "Air detection", this->air_detection_switch_);
  LOG_SELECT("  ", "Control mode", this->control_mode_select_);
  LOG_NUMBER("  ", "Setpoint head", this->setpoint_head_number_);
  LOG_NUMBER("  ", "Setpoint flow", this->setpoint_flow_number_);
  LOG_NUMBER("  ", "Setpoint speed", this->setpoint_speed_number_);
}

static constexpr float PA_PER_M = 9806.65f;  // 1 m water column
static constexpr uint8_t SOURCE_AIR_VENTING = 111;  // ControlSource AirVenting (seen on HK1 during "vent pump only")

// Queries in send order
struct RequestDef {
  uint16_t bit;
  uint8_t obj;
  uint16_t sub;
  const char *name;
};
static const RequestDef REQUESTS[] = {
    {1 << 0, 87, 69, "87/69"},   {1 << 1, 93, 290, "93/290"},  {1 << 2, 93, 1, "93/1"},
    {1 << 3, 93, 300, "93/300"}, {1 << 4, 87, 1, "87/1"},      {1 << 5, 86, 6, "86/6"},
    {1 << 6, 86, 7, "86/7"},     {1 << 7, 86, 8, "86/8"},      {1 << 8, 90, 8, "90/8"},
    {1 << 9, 90, 7, "90/7"},     {1 << 10, 88, 600, "88/600"}, {1 << 11, 88, 604, "88/604"},
    {1 << 12, 88, 612, "88/612"}, {1 << 13, 86, 9, "86/9"},
};

const char *GrundfosAlpha2Go::req_name_(uint16_t req) {
  if (req == REQ_WRITE) return "SET";
  for (const auto &r : REQUESTS)
    if (r.bit == req) return r.name;
  return "?";
}

// Operation mode (enum OperationMode in the GENI profile): name for the log, text for HA
struct OperationModeDef {
  const char *name;
  const char *text;
};
static const OperationModeDef OPERATION_MODES[] = {
    {"Auto", "Auto"},           {"Stop", "Stop"},
    {"Min", "Minimum"},         {"Max", "Maximum"},
    {"UserDefined", "User defined"},       {"ForcedStart", "Forced start"},
    {"NoCmd", "No command"},   {"Hand", "Hand"},
    {"Standby", "Standby"},     {"StopWithoutDelay", "Stop immediately"},
    {"EmergencyRun", "Emergency run"},             {"UserDefined2", "User defined 2"},
    {"SetPumpsToLocalMode", "Local mode"},
};
static const OperationModeDef *operation_mode_def(uint8_t v) {
  return v < sizeof(OPERATION_MODES) / sizeof(OPERATION_MODES[0]) ? &OPERATION_MODES[v] : nullptr;
}
static const char *operation_mode_name(uint8_t v) {
  auto *m = operation_mode_def(v);
  return m != nullptr ? m->name : "?";
}

// Control mode (enum ControlMode). kind: set_point unit -> pressure (Pa), flow (m3/s), speed (1/min)
struct ControlModeDef {
  uint8_t code;
  const char *name;
  const char *text;
  SetpointKind kind;
};
// Texts follow the GO app terminology. In the app AutoAdapt is enabled within proportional or constant
// pressure; the pump reports that as 13 or 14 respectively (verified on HK1).
static const ControlModeDef CONTROL_MODES[] = {
    {0, "ConstantPressure", "Constant pressure", SP_PRESSURE},  // Pa verified on HK1 (write test)
    {1, "ProportionalPressure", "Proportional pressure", SP_PRESSURE},  // Pa verified on HK1
    {2, "ConstantSpeed", "Constant speed", SP_SPEED},       // 1/min verified on HK1
    {5, "AutoAdapt", "AutoAdapt", SP_PRESSURE},
    {6, "ConstantTemperature", "Constant temperature", SP_UNKNOWN},
    {7, "ClosedLoopSensor", "Closed loop sensor", SP_UNKNOWN},
    {8, "ConstantFlow", "Constant flow", SP_FLOW},  // m3/s verified on HK1
    {9, "ConstantLevel", "Constant level", SP_UNKNOWN},
    {10, "FlowAdapt", "FlowAdapt", SP_FLOW},
    {11, "ConstantDifferentialPressure", "Constant differential pressure", SP_PRESSURE},
    {12, "ConstantDifferentialTemperature", "Constant differential temperature", SP_UNKNOWN},
    {13, "AutoAdaptRadiator", "Proportional pressure (AutoAdapt)", SP_PRESSURE},    // Pa verified on HK1
    {14, "AutoAdaptUnderfloor", "Constant pressure (AutoAdapt)", SP_PRESSURE},  // Pa verified on HK1
    {15, "AutoAdaptRadiatorAndUnderfloor", "AutoAdapt radiator + underfloor", SP_PRESSURE},
    {24, "UserDefined", "User defined", SP_UNKNOWN},
    {26, "ProportionalDifferentialPressure", "Proportional differential pressure", SP_PRESSURE},
    {254, "None", "None", SP_UNKNOWN},
};
static const ControlModeDef *control_mode_def(uint8_t v) {
  for (const auto &m : CONTROL_MODES)
    if (m.code == v) return &m;
  return nullptr;
}
static const char *control_mode_name(uint8_t v) {
  auto *m = control_mode_def(v);
  return m != nullptr ? m->name : "?";
}
// ControlSource (86/6-86/9 @0). 0/1/2/104 named in the GENI profile (per upstream README); 111 observed upstream
// during "vent pump only". 123 observed on a 25-75 with external PWM selected in the GO app: the 86/8 speed
// setpoint followed every PWM change (84 % duty -> 1650, 61 % -> 2742, 46 % -> 3494, 10 % -> 5214 rpm).
// The profile's own name for 123 is unconfirmed.
static const char *control_source_text(uint8_t src) {
  switch (src) {
    case 0: return "Not used";
    case 1: return "Panel / app";
    case 2: return "Bus";
    case 104: return "AutoNight";
    case 111: return "Air venting";
    case 123: return "External PWM";
    default: return nullptr;
  }
}
static SetpointKind setpoint_kind(uint8_t ctrl) {
  auto *m = control_mode_def(ctrl);
  return m != nullptr ? m->kind : SP_UNKNOWN;
}

// ---- Writes ----
// Control modes of the select, same order as CONTROL_MODE_OPTIONS in select.py
static const uint8_t SELECT_CONTROL_CODES[] = {1, 0, 13, 14, 8, 2};
static constexpr size_t NUM_SELECT_CONTROL_CODES = sizeof(SELECT_CONTROL_CODES) / sizeof(SELECT_CONTROL_CODES[0]);
// Default values when switching to a control mode whose setpoint has not been read this session
static constexpr float DEFAULT_SP_PRESSURE = 2.0f * PA_PER_M;  // 2 m
static constexpr float DEFAULT_SP_FLOW = 1.0f / 3600.0f;       // 1 m3/h
static constexpr float DEFAULT_SP_SPEED = 3000.0f;             // 1/min
static constexpr uint8_t MODE_STOP = 1;
static constexpr uint8_t CMD_STOP = 5, CMD_START = 6;

static bool is_autoadapt(uint8_t ctrl) { return ctrl == 5 || (ctrl >= 13 && ctrl <= 15); }

bool GrundfosAlpha2Go::queue_write_(const uint8_t *frame, size_t len, const char *label) {
  if (this->stage_ != Stage::POLLING) {
    ESP_LOGW(TAG, "%s: not sent - pump not connected", label);
    return false;
  }
  if (len == 0 || len > geni::MAX_TX_FRAME) {
    ESP_LOGE(TAG, "%s: not sent - frame too long", label);
    return false;
  }
  if (this->write_count_ >= WRITE_QUEUE) {
    ESP_LOGW(TAG, "%s: not sent - write queue full", label);
    return false;
  }
  auto &w = this->writes_[(this->write_head_ + this->write_count_) % WRITE_QUEUE];
  memcpy(w.buf, frame, len);
  w.len = (uint8_t) len;
  w.label = label;
  this->write_count_++;
  ESP_LOGD(TAG, "%s: queued", label);
  return true;
}

// After reply or timeout: read back the effect immediately
void GrundfosAlpha2Go::finish_write_(const char *why) {
  ESP_LOGD(TAG, "%s: %s - reading back", this->write_label_, why);
  this->inflight_ = false;
  this->last_req_ = 0;
  this->pending_mask_ |= REQ_AFTER_WRITE;
}

// Check reply: Class 3 -> APDU ACK (top 2 bits) must be 0; Class 10 -> status byte must also be 0
void GrundfosAlpha2Go::check_write_reply_(const uint8_t *d, size_t n) {
  if (n < 6 || d[0] != geni::SD_REPLY) {
    ESP_LOGW(TAG, "%s: unexpected reply %s", this->write_label_, hex_str_(d, n).c_str());
    return;
  }
  const uint8_t cls = d[4], hdr = d[5], ack = hdr >> 6;
  const uint8_t status = (cls == geni::CLASS_OBJECT && (hdr & 0x3F) >= 1 && n > 6) ? d[6] : 0;
  if (ack == 0 && status == 0) {
    ESP_LOGI(TAG, "%s: accepted", this->write_label_);
  } else {
    // ACK 1/2/3: unknown class / unknown ID / operation not allowed;
    // status 1: unknown operation, 4: object/type not found
    ESP_LOGE(TAG, "%s: rejected by pump (class %u, ack %u, status %u): %s", this->write_label_, cls, ack, status,
             hex_str_(d, n).c_str());
  }
}

bool GrundfosAlpha2Go::queue_set_operation_(uint8_t mode, uint8_t ctrl, float setpoint_si, const char *label) {
  // 86/6 OperationStatusRequest (type 303 v1): u8 source, u8 mode, u8 control, float setpoint (SI, Big Endian)
  uint8_t d[7] = {this->op_src_, mode, ctrl};
  uint32_t u;
  memcpy(&u, &setpoint_si, 4);
  d[3] = (uint8_t) (u >> 24);
  d[4] = (uint8_t) (u >> 16);
  d[5] = (uint8_t) (u >> 8);
  d[6] = (uint8_t) u;
  uint8_t f[geni::MAX_TX_FRAME];
  const size_t n = geni::build_object_set(f, 86, 6, geni::TYPE_OPERATION_STATUS, 1, d, sizeof(d));
  ESP_LOGI(TAG, "%s: mode=%u control=%u (%s) setpoint=%g", label, mode, ctrl, control_mode_name(ctrl), setpoint_si);
  return this->queue_write_(f, n, label);
}

bool GrundfosAlpha2Go::write_switch(SwitchKind kind, bool state) {
  uint8_t f[geni::MAX_TX_FRAME];
  switch (kind) {
    case SwitchKind::PUMP: {
      // Class 3 START (= Auto) / STOP; acts on the local request 86/6 like the GO app
      const size_t n = geni::build_command(f, state ? CMD_START : CMD_STOP);
      return this->queue_write_(f, n, state ? "Pump start" : "Pump stop");
    }
    case SwitchKind::NIGHT_SETBACK: {
      const uint8_t v = state ? 1 : 0;
      const size_t n = geni::build_object_set(f, 90, 8, geni::TYPE_AUTONIGHT_ENABLE, 1, &v, 1);
      return this->queue_write_(f, n, state ? "Night setback on" : "Night setback off");
    }
    case SwitchKind::AIR_DETECTION: {
      // 88/600 is written in full: last read values, only "enabled" (byte 7) changed
      if (!this->air_cfg_valid_) {
        ESP_LOGW(TAG, "Air detection: not sent - configuration (88/600) not read yet");
        return false;
      }
      uint8_t d[8];
      memcpy(d, this->air_cfg_, sizeof(d));
      d[7] = state ? 1 : 0;
      const size_t n = geni::build_object_set(f, 88, 600, geni::TYPE_AIR_DETECTION_CONFIG, 1, d, sizeof(d));
      return this->queue_write_(f, n, state ? "Air detection on" : "Air detection off");
    }
  }
  return false;
}

bool GrundfosAlpha2Go::write_control_mode(uint8_t ctrl) {
  if (!this->op_valid_) {
    ESP_LOGW(TAG, "Control mode: not sent - operation status (86/6) not read yet");
    return false;
  }
  const SetpointKind kind = setpoint_kind(ctrl);
  float sp;
  if (kind == setpoint_kind(this->op_ctrl_) && !is_autoadapt(this->op_ctrl_)) {
    sp = this->op_sp_;  // same unit (e.g. proportional -> constant pressure): keep setpoint
  } else if (std::isfinite(this->last_sp_[kind])) {
    sp = this->last_sp_[kind];
  } else {
    sp = kind == SP_FLOW ? DEFAULT_SP_FLOW : kind == SP_SPEED ? DEFAULT_SP_SPEED : DEFAULT_SP_PRESSURE;
  }
  // Under AutoAdapt the pump sets its own setpoint; the one sent is ignored (verified on HK1)
  return this->queue_set_operation_(this->op_mode_, ctrl, sp, "Control mode");
}

bool GrundfosAlpha2Go::write_setpoint(SetpointKind kind, float value) {
  if (!this->op_valid_) {
    ESP_LOGW(TAG, "Setpoint: not sent - operation status (86/6) not read yet");
    return false;
  }
  if (setpoint_kind(this->op_ctrl_) != kind) {
    ESP_LOGW(TAG, "Setpoint: not sent - does not match control mode %s", control_mode_name(this->op_ctrl_));
    this->publish_controls_();
    return false;
  }
  if (is_autoadapt(this->op_ctrl_)) {
    ESP_LOGW(TAG, "Setpoint: not sent - AutoAdapt sets the setpoint itself");
    this->publish_controls_();
    return false;
  }
  const float si = kind == SP_PRESSURE ? value * PA_PER_M : kind == SP_FLOW ? value / 3600.0f : value;
  return this->queue_set_operation_(this->op_mode_, this->op_ctrl_, si, "Setpoint");
}

bool GrundfosAlpha2Go::press_button(ButtonKind kind) {
  // 88/612 SystemAirVentingCommandAndStatus (type 971 v1): u8 cmd (0 Stop, 1 StartManual), rest 0
  const bool start = kind == ButtonKind::VENTING_START;
  const uint8_t d[5] = {(uint8_t) (start ? 1 : 0), 0, 0, 0, 0};
  uint8_t f[geni::MAX_TX_FRAME];
  const size_t n = geni::build_object_set(f, 88, 612, geni::TYPE_AIR_VENTING_STATUS, 1, d, sizeof(d));
  return this->queue_write_(f, n, start ? "System venting start" : "System venting stop");
}

void GrundfosAlpha2Go::send_raw_class10(const std::vector<uint8_t> &apdu) {
  uint8_t f[geni::MAX_TX_FRAME];
  const size_t n = geni::build_class10_raw(f, apdu.data(), apdu.size());
  this->queue_write_(f, n, "Raw class 10");
}

// Update controls from the local request (86/6)
void GrundfosAlpha2Go::publish_controls_() {
  if (!this->op_valid_) return;
  if (this->pump_switch_ != nullptr) this->pump_switch_->publish_state(this->op_mode_ != MODE_STOP);
  if (this->control_mode_select_ != nullptr) {
    for (size_t i = 0; i < NUM_SELECT_CONTROL_CODES; i++)
      if (SELECT_CONTROL_CODES[i] == this->op_ctrl_) this->control_mode_select_->publish_state(i);
  }
  const SetpointKind kind = setpoint_kind(this->op_ctrl_);
  if (this->setpoint_head_number_ != nullptr)
    this->setpoint_head_number_->publish_state(kind == SP_PRESSURE ? this->op_sp_ / PA_PER_M : NAN);
  if (this->setpoint_flow_number_ != nullptr)
    this->setpoint_flow_number_->publish_state(kind == SP_FLOW ? this->op_sp_ * 3600.0f : NAN);
  if (this->setpoint_speed_number_ != nullptr)
    this->setpoint_speed_number_->publish_state(kind == SP_SPEED ? this->op_sp_ : NAN);
}

void GrundfosSwitch::write_state(bool state) { this->parent_->write_switch(this->kind_, state); }
void GrundfosControlModeSelect::control(size_t index) {
  if (index < NUM_SELECT_CONTROL_CODES) this->parent_->write_control_mode(SELECT_CONTROL_CODES[index]);
}
void GrundfosSetpointNumber::control(float value) { this->parent_->write_setpoint(this->kind_, value); }
void GrundfosButton::press_action() { this->parent_->press_button(this->kind_); }

void GrundfosAlpha2Go::setup() {
  this->publish_bin_(this->connected_binary_sensor_, false);
  if (this->flow_sensor_ != nullptr || this->head_sensor_ != nullptr) this->wanted_mask_ |= REQ_FLOW;
  if (this->power_sensor_ != nullptr || this->voltage_sensor_ != nullptr || this->dc_voltage_sensor_ != nullptr ||
      this->current_sensor_ != nullptr || this->speed_sensor_ != nullptr || this->aux_temperature_sensor_ != nullptr)
    this->wanted_mask_ |= REQ_ELECTRIC;
  if (this->starts_sensor_ != nullptr || this->operating_hours_sensor_ != nullptr ||
      this->power_on_hours_sensor_ != nullptr)
    this->wanted_mask_ |= REQ_COUNTERS;
  if (this->media_temperature_sensor_ != nullptr || this->pcb_temperature_sensor_ != nullptr ||
      this->controlbox_temperature_sensor_ != nullptr)
    this->wanted_mask_ |= REQ_TEMPERATURE;
  if (this->energy_sensor_ != nullptr) this->wanted_mask_ |= REQ_ENERGY;
  // 86/6 and 86/8 only for comparison in the log; sensors come from 86/7
  if (this->operation_mode_sensor_ != nullptr || this->control_mode_sensor_ != nullptr ||
      this->setpoint_sensor_ != nullptr || this->setpoint_head_sensor_ != nullptr ||
      this->setpoint_flow_sensor_ != nullptr || this->setpoint_speed_sensor_ != nullptr ||
      this->setpoint_actual_sensor_ != nullptr || this->setpoint_actual_speed_sensor_ != nullptr ||
      this->control_source_text_sensor_ != nullptr ||
      this->operation_mode_text_sensor_ != nullptr || this->control_mode_text_sensor_ != nullptr)
    this->wanted_mask_ |= REQ_OP_LOCAL | REQ_OP_PRIO | REQ_OP_REALIZED;
  if (this->night_setback_enabled_binary_sensor_ != nullptr) this->wanted_mask_ |= REQ_NIGHT_ENABLE;
  if (this->night_mode_binary_sensor_ != nullptr) this->wanted_mask_ |= REQ_NIGHT_STATUS;
  if (this->air_detection_enabled_binary_sensor_ != nullptr) this->wanted_mask_ |= REQ_AIR_CONFIG;
  if (this->air_detections_sensor_ != nullptr) this->wanted_mask_ |= REQ_AIR_COUNTER;
  if (this->venting_active_binary_sensor_ != nullptr || this->venting_status_text_sensor_ != nullptr)
    this->wanted_mask_ |= REQ_VENTING | REQ_OP_REALIZED | REQ_OP_TEMP;  // 86/8, 86/9 for "vent pump only"
  // Controls
  if (this->pump_switch_ != nullptr || this->control_mode_select_ != nullptr ||
      this->setpoint_head_number_ != nullptr || this->setpoint_flow_number_ != nullptr ||
      this->setpoint_speed_number_ != nullptr)
    this->wanted_mask_ |= REQ_OP_LOCAL;
  if (this->night_setback_switch_ != nullptr) this->wanted_mask_ |= REQ_NIGHT_ENABLE;
  if (this->air_detection_switch_ != nullptr) this->wanted_mask_ |= REQ_AIR_CONFIG;
  if (this->venting_buttons_present_) this->wanted_mask_ |= REQ_VENTING;
}

void GrundfosAlpha2Go::reset_state_() {
  this->stage_ = Stage::IDLE;
  this->handles_ready_ = false;
  this->enc_ok_ = false;
  this->auth_failed_ = false;
  this->cccd_tries_ = 0;
  this->hello_sent_ = 0;
  this->inflight_ = false;
  this->last_req_ = 0;
  this->pending_mask_ = 0;
  this->write_count_ = 0;  // unsent writes are dropped on disconnect
  this->op_valid_ = false;
  this->air_cfg_valid_ = false;
  this->system_venting_state_ = 0;
  this->pump_venting_ = false;
  this->rx_.reset();
}

void GrundfosAlpha2Go::publish_(sensor::Sensor *s, float value) {
  if (s != nullptr && std::isfinite(value)) s->publish_state(value);
}

void GrundfosAlpha2Go::publish_or_nan_(sensor::Sensor *s, float value) {
  if (s != nullptr) s->publish_state(std::isfinite(value) ? value : NAN);
}

void GrundfosAlpha2Go::publish_text_(text_sensor::TextSensor *s, const char *value) {
  if (s != nullptr) s->publish_state(value);
}

void GrundfosAlpha2Go::publish_bin_(binary_sensor::BinarySensor *s, bool value) {
  if (s != nullptr) s->publish_state(value);
}

// Venting: system+pump from 88/612, pump-only from 86/8 (control source AirVenting)
void GrundfosAlpha2Go::publish_venting_() {
  const uint8_t sys = this->system_venting_state_;
  this->publish_bin_(this->venting_active_binary_sensor_, sys != 0 || this->pump_venting_);
  if (this->venting_status_text_sensor_ == nullptr) return;
  char buf[32];
  const char *text;
  if (sys == 1) {
    text = "System and pump (manual)";
  } else if (sys == 2) {
    text = "System and pump (automatic)";
  } else if (sys != 0) {
    snprintf(buf, sizeof(buf), "Unknown (%u)", sys);
    text = buf;
  } else if (this->pump_venting_) {
    text = "Pump only";
  } else {
    text = "Inactive";
  }
  this->publish_text_(this->venting_status_text_sensor_, text);
}

// After a disconnect set all entities to "unknown" (Home Assistant: missing_state). The ESPHome API has no
// per-entity "unavailable"; that only exists when the whole ESP is offline.
void GrundfosAlpha2Go::publish_all_unknown_() {
  sensor::Sensor *all[] = {this->flow_sensor_,
                           this->head_sensor_,
                           this->power_sensor_,
                           this->voltage_sensor_,
                           this->dc_voltage_sensor_,
                           this->current_sensor_,
                           this->speed_sensor_,
                           this->aux_temperature_sensor_,
                           this->media_temperature_sensor_,
                           this->pcb_temperature_sensor_,
                           this->controlbox_temperature_sensor_,
                           this->starts_sensor_,
                           this->operating_hours_sensor_,
                           this->power_on_hours_sensor_,
                           this->energy_sensor_,
                           this->operation_mode_sensor_,
                           this->control_mode_sensor_,
                           this->setpoint_sensor_,
                           this->setpoint_head_sensor_,
                           this->setpoint_flow_sensor_,
                           this->setpoint_speed_sensor_,
                           this->setpoint_actual_sensor_,
                           this->setpoint_actual_speed_sensor_,
                           this->air_detections_sensor_};
  for (auto *s : all)
    if (s != nullptr) s->publish_state(NAN);

  binary_sensor::BinarySensor *bins[] = {this->night_setback_enabled_binary_sensor_, this->night_mode_binary_sensor_,
                                         this->air_detection_enabled_binary_sensor_,
                                         this->venting_active_binary_sensor_};
  for (auto *b : bins)
    if (b != nullptr) b->invalidate_state();

  text_sensor::TextSensor *texts[] = {this->operation_mode_text_sensor_, this->control_mode_text_sensor_,
                                      this->venting_status_text_sensor_, this->control_source_text_sensor_};
  for (auto *t : texts) {
    if (t == nullptr || !t->has_state()) continue;
    t->set_has_state(false);
#ifdef USE_CONTROLLER_REGISTRY
    ControllerRegistry::notify_text_sensor_update(t);
#endif
  }

  switch_::Switch *switches[] = {this->pump_switch_, this->night_setback_switch_, this->air_detection_switch_};
  for (auto *sw : switches)
    if (sw != nullptr) static_cast<GrundfosSwitch *>(sw)->invalidate();

  if (this->control_mode_select_ != nullptr && this->control_mode_select_->has_state()) {
    this->control_mode_select_->set_has_state(false);
#ifdef USE_CONTROLLER_REGISTRY
    ControllerRegistry::notify_select_update(this->control_mode_select_);
#endif
  }
  number::Number *numbers[] = {this->setpoint_head_number_, this->setpoint_flow_number_, this->setpoint_speed_number_};
  for (auto *n : numbers) {
    if (n == nullptr || !n->has_state()) continue;
    n->set_has_state(false);
#ifdef USE_CONTROLLER_REGISTRY
    ControllerRegistry::notify_number_update(n);
#endif
  }
}

void GrundfosSwitch::invalidate() {
  if (!this->has_state()) return;
  this->publish_dedup_.next_unknown();  // otherwise the same state would be dropped after reconnecting
  this->set_has_state(false);
#ifdef USE_CONTROLLER_REGISTRY
  ControllerRegistry::notify_switch_update(this);
#endif
}

void GrundfosAlpha2Go::gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) {
  const uint8_t *bda = this->parent_->get_remote_bda();
  switch (event) {
    case ESP_GAP_BLE_SEC_REQ_EVT:  // pump requests security itself
      if (memcmp(param->ble_security.ble_req.bd_addr, bda, 6) == 0)
        ESP_LOGD(TAG, "[%s] Security request from pump", this->parent_->address_str());
      return;
    case ESP_GAP_BLE_KEY_EVT:  // only on a new pairing
      if (memcmp(param->ble_security.ble_key.bd_addr, bda, 6) == 0)
        ESP_LOGI(TAG, "[%s] New key received from pump (type 0x%02x) - new pairing", this->parent_->address_str(),
                 param->ble_security.ble_key.key_type);
      return;
    case ESP_GAP_BLE_REMOVE_BOND_DEV_COMPLETE_EVT:
      if (memcmp(param->remove_bond_dev_cmpl.bd_addr, bda, 6) == 0)
        ESP_LOGW(TAG, "[%s] Bond removed from NVS (status %d)", this->parent_->address_str(),
                 (int) param->remove_bond_dev_cmpl.status);
      return;
    case ESP_GAP_BLE_AUTH_CMPL_EVT:
      break;
    default:
      return;
  }
  const auto &ac = param->ble_security.auth_cmpl;
  if (memcmp(ac.bd_addr, bda, 6) != 0) return;
  bool stored = false;
  const uint8_t bonds = this->bond_count_(&stored);
  if (ac.success) {
    ESP_LOGD(TAG, "[%s] Link encrypted (key_present=%d key_type=0x%02x auth_mode=0x%02x, bonds in NVS: %u, pump: %s)",
             this->parent_->address_str(), ac.key_present, ac.key_type, ac.auth_mode, bonds, YESNO(stored));
    this->enc_ok_ = true;
    this->auth_fail_count_ = 0;
    return;
  }
  // reason 82 = ESP_AUTH_SMP_PAIR_NOT_SUPPORT: the ESP started a NEW pairing instead of encrypting with the stored
  // LTK, and the pump rejects it. Bluedroid only re-pairs when the key is missing from RAM
  // (btm_ble_set_encryption). The bond is still in NVS: in several places Bluedroid removes device entries
  // from RAM only (e.g. bta_dm on pairing failures as central with
  // CONFIG_BT_BLE_SMP_REMOVE_BOND_ON_PAIR_FAIL_AS_CENTRAL=y; with reason 82 the NVS entry stays).
  // From then on every connection fails until the stack restarts and reloads the bonds from NVS.
  this->auth_failed_ = true;
  this->auth_fail_count_++;
  const int reason = ac.fail_reason;
  if (reason == ESP_AUTH_SMP_PAIR_NOT_SUPPORT && stored) {
    ESP_LOGW(TAG,
             "[%s] Encryption rejected (reason=82, attempt %u): bond is in NVS, but the BLE stack started a new pairing "
             "-> restarting the BLE stack to reload the bond",
             this->parent_->address_str(), this->auth_fail_count_);
    this->stack_reset_pending_ = true;
  } else {
    ESP_LOGW(TAG, "[%s] Encryption rejected (reason=%d, attempt %u, bonds in NVS: %u, pump: %s). %s",
             this->parent_->address_str(), reason, this->auth_fail_count_, bonds, YESNO(stored),
             stored ? "" : "Not paired: press the Bluetooth button on the pump while the ESP connects.");
  }
}

uint8_t GrundfosAlpha2Go::bond_count_(bool *stored) {
  *stored = false;
  int n = esp_ble_get_bond_device_num();
  if (n <= 0) return 0;
  std::vector<esp_ble_bond_dev_t> list(n);
  if (esp_ble_get_bond_device_list(&n, list.data()) != ESP_OK) return 0;
  for (int i = 0; i < n; i++)
    if (memcmp(list[i].bd_addr, this->parent_->get_remote_bda(), 6) == 0) *stored = true;
  return (uint8_t) n;
}

void GrundfosAlpha2Go::gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                                           esp_ble_gattc_cb_param_t *param) {
  switch (event) {
    case ESP_GATTC_CONNECT_EVT: {
      if (memcmp(param->connect.remote_bda, this->parent_->get_remote_bda(), 6) != 0) return;
      this->reset_state_();
      {
        bool stored = false;
        const uint8_t bonds = this->bond_count_(&stored);
        ESP_LOGD(TAG, "[%s] Connected, requesting encryption (bonds in NVS: %u, pump: %s)",
                 this->parent_->address_str(), bonds, YESNO(stored));
      }
      // The pump drops unencrypted clients after ~2 s -> encrypt immediately (stored bond).
      esp_err_t r = esp_ble_set_encryption(param->connect.remote_bda, ESP_BLE_SEC_ENCRYPT);
      if (r != ESP_OK) ESP_LOGW(TAG, "esp_ble_set_encryption failed (%d)", (int) r);
      this->stage_ = Stage::WAIT_ENC;
      this->t_stage_ = millis();
      break;
    }
    case ESP_GATTC_SEARCH_CMPL_EVT: {
      auto *chr = this->parent_->get_characteristic(GENI_SERVICE, GENI_CHAR);
      if (chr == nullptr) {
        // After rejected encryption or a disconnect ESPHome sometimes still reports service discovery
        // as complete with no services -> not an error then
        if (this->stage_ == Stage::IDLE || this->auth_failed_) {
          ESP_LOGD(TAG, "[%s] Service discovery finished without GENI service (connection not usable)",
                   this->parent_->address_str());
        } else {
          ESP_LOGE(TAG, "[%s] GENI characteristic not found - not a Grundfos pump with Bluetooth GENI?",
                   this->parent_->address_str());
        }
        break;
      }
      auto *cccd = chr->get_descriptor(0x2902);
      if (cccd == nullptr) {
        ESP_LOGE(TAG, "[%s] CCCD of GENI characteristic not found", this->parent_->address_str());
        break;
      }
      this->data_handle_ = chr->handle;
      this->cccd_handle_ = cccd->handle;
      this->handles_ready_ = true;
      ESP_LOGD(TAG, "[%s] GENI handles: data=0x%04x cccd=0x%04x", this->parent_->address_str(), this->data_handle_,
               this->cccd_handle_);
      auto status = this->parent_->register_for_notify(this->data_handle_);
      if (status != ESP_OK) ESP_LOGW(TAG, "register_for_notify failed (%d)", (int) status);
      break;
    }
    case ESP_GATTC_REG_FOR_NOTIFY_EVT: {
      // Handles are stored -> GATT cache may be released.
      this->node_state = espbt::ClientState::ESTABLISHED;
      break;
    }
    case ESP_GATTC_WRITE_DESCR_EVT: {
      if (param->write.handle != this->cccd_handle_) break;
      // ESPHome sometimes writes the CCCD itself in register_for_notify -> ignore the second confirmation,
      // otherwise the initial query restarts while a query is already running.
      if (this->stage_ != Stage::WAIT_CCCD) break;
      if (param->write.status == ESP_GATT_OK) {
        ESP_LOGD(TAG, "[%s] Notifications enabled", this->parent_->address_str());
        this->stage_ = Stage::HELLO;
        this->t_stage_ = millis();
      } else {
        ESP_LOGW(TAG, "[%s] CCCD write failed (status %d) - will retry", this->parent_->address_str(),
                 (int) param->write.status);
      }
      break;
    }
    case ESP_GATTC_NOTIFY_EVT: {
      if (param->notify.handle != this->data_handle_) break;
      ESP_LOGVV(TAG, "RX %d bytes", (int) param->notify.value_len);
      if (this->inflight_ && this->last_req_ == REQ_WRITE) {  // reply to a write
        this->check_write_reply_(param->notify.value, param->notify.value_len);
        this->finish_write_("reply received");
      } else if (this->inflight_ && (this->last_req_ & REQ_DIAG_MASK))  // including replies the parser discards
        ESP_LOGV(TAG, "%s raw RX: %s", req_name_(this->last_req_),
                 hex_str_(param->notify.value, param->notify.value_len).c_str());
      int crc_errors = this->rx_.feed(param->notify.value, param->notify.value_len,
                                      [this](const geni::Reply &r) { this->handle_reply_(r); });
      if (crc_errors > 0) ESP_LOGW(TAG, "%d GENI frame(s) dropped: CRC error", crc_errors);
      break;
    }
    case ESP_GATTC_DISCONNECT_EVT: {
      if (memcmp(param->disconnect.remote_bda, this->parent_->get_remote_bda(), 6) != 0) break;
      // reason: 0x08 Supervision Timeout, 0x13 disconnected by pump, 0x16 disconnected by ESP, 0x3D MIC failure
      ESP_LOGI(TAG, "[%s] Disconnected (reason 0x%02x, stage %u, encrypted %s)", this->parent_->address_str(),
               (unsigned) param->disconnect.reason, (unsigned) this->stage_, YESNO(this->enc_ok_));
      this->node_state = espbt::ClientState::IDLE;
      this->reset_state_();
      this->publish_all_unknown_();
      this->publish_bin_(this->connected_binary_sensor_, false);
      break;
    }
    default:
      break;
  }
}

void GrundfosAlpha2Go::write_cccd_() {
  uint8_t on[] = {0x01, 0x00};
  auto rc = esp_ble_gattc_write_char_descr(this->parent_->get_gattc_if(), this->parent_->get_conn_id(),
                                           this->cccd_handle_, sizeof(on), on, ESP_GATT_WRITE_TYPE_RSP,
                                           ESP_GATT_AUTH_REQ_NONE);
  if (rc != ESP_OK) ESP_LOGW(TAG, "CCCD write request failed (%d)", (int) rc);
  this->cccd_tries_++;
  this->t_stage_ = millis();
}

void GrundfosAlpha2Go::send_frame_(const uint8_t *frame, size_t len) {
  // Send long frames in BLE_CHUNK-byte pieces (the way the pump splits its replies)
  uint8_t tmp[geni::BLE_CHUNK];
  for (size_t off = 0; off < len; off += geni::BLE_CHUNK) {
    const size_t n = std::min(geni::BLE_CHUNK, len - off);
    memcpy(tmp, frame + off, n);
    auto st = esp_ble_gattc_write_char(this->parent_->get_gattc_if(), this->parent_->get_conn_id(), this->data_handle_,
                                       n, tmp, ESP_GATT_WRITE_TYPE_NO_RSP, ESP_GATT_AUTH_REQ_NONE);
    if (st != ESP_OK) ESP_LOGW(TAG, "write failed (%d)", (int) st);
  }
  this->t_last_tx_ = millis();
}

void GrundfosAlpha2Go::send_next_request_() {
  for (const auto &r : REQUESTS) {
    if (!(this->pending_mask_ & r.bit)) continue;
    this->pending_mask_ &= ~r.bit;
    uint8_t f[16];
    size_t n = geni::build_object_get(f, r.obj, r.sub);
    ESP_LOGVV(TAG, "TX %s: %s", r.name, hex_str_(f, n).c_str());
    this->last_req_ = r.bit;
    this->inflight_ = true;
    this->send_frame_(f, n);
    return;
  }
}

void GrundfosAlpha2Go::loop() {
  uint32_t now = millis();
  if (this->stack_reset_pending_) {
    this->stack_reset_pending_ = false;
    if (this->t_last_stack_reset_ != 0 && now - this->t_last_stack_reset_ < STACK_RESET_MIN_GAP_MS) {
      ESP_LOGD(TAG, "BLE stack restarted %us ago - not again yet", (unsigned) ((now - this->t_last_stack_reset_) / 1000));
    } else if (esp32_ble::global_ble != nullptr) {
      // Affects all BLE functions of this ESP (tracker, other BLE clients); other components keep running.
      // Sequence: disconnect first, then tear down the stack. Teardown (esp_bluedroid_disable/deinit,
      // controller) blocks the main task for several seconds; on the ESP32 the normal task watchdog applies
      // (5 s, ESPHome only extends it for ESP32-Hosted). So temporarily extend the watchdog first.
      this->t_last_stack_reset_ = now;
      ESP_LOGW(TAG, "Restarting BLE stack to reload bonds from NVS");
      this->parent_->disconnect();
      this->set_timeout("ble_disable", 1500, []() {
        esp_task_wdt_config_t cfg = {.timeout_ms = BLE_RESET_WDT_MS, .idle_core_mask = 0, .trigger_panic = true};
        esp_task_wdt_reconfigure(&cfg);
        // The actual teardown then runs in esp32_ble::loop() ("esp32_ble took a long time" is normal there)
        if (esp32_ble::global_ble != nullptr) esp32_ble::global_ble->disable();
      });
      this->set_timeout("ble_enable", 4000, []() {
        if (esp32_ble::global_ble != nullptr) esp32_ble::global_ble->enable();
      });
      this->set_timeout("ble_wdt_restore", 15000, []() {
        esp_task_wdt_config_t cfg = {.timeout_ms = CONFIG_ESP_TASK_WDT_TIMEOUT_S * 1000u, .idle_core_mask = 0,
                                     .trigger_panic = true};
        esp_task_wdt_reconfigure(&cfg);
        ESP_LOGD(TAG, "Task watchdog restored to %u s", (unsigned) CONFIG_ESP_TASK_WDT_TIMEOUT_S);
      });
    }
  }
  switch (this->stage_) {
    case Stage::WAIT_ENC:
      // Encryption rejected: unencrypted, the pump doesn't reply and disconnects soon -> send nothing,
      // wait for the disconnect; ESPHome then reconnects automatically.
      if (this->auth_failed_) {
        if (now - this->t_stage_ > AUTH_FAIL_DISCONNECT_MS) {  // pump didn't disconnect itself -> start over
          ESP_LOGD(TAG, "[%s] Disconnecting after rejected encryption", this->parent_->address_str());
          this->parent_->disconnect();
          this->t_stage_ = now;
        }
        break;
      }
      if (this->handles_ready_ && (this->enc_ok_ || now - this->t_stage_ > ENC_TIMEOUT_MS)) {
        if (!this->enc_ok_) ESP_LOGW(TAG, "No AUTH_CMPL seen, continuing anyway");
        this->stage_ = Stage::WAIT_CCCD;
        this->write_cccd_();
      }
      break;
    case Stage::WAIT_CCCD:
      if (now - this->t_stage_ > CCCD_TIMEOUT_MS && this->cccd_tries_ < 3) this->write_cccd_();
      break;
    case Stage::HELLO: {
      // GO app initial query (twice): Class 2, IDs 148/149/150 = unit_family/type/version
      if (this->hello_sent_ < 2 && now - this->t_last_tx_ >= MIN_TX_GAP_MS) {
        static const uint8_t ids[] = {148, 149, 150};
        uint8_t f[16];
        size_t n = geni::build_class2_get(f, ids, sizeof(ids));
        this->send_frame_(f, n);
        this->hello_sent_++;
      } else if (this->hello_sent_ >= 2 && now - this->t_last_tx_ >= MIN_TX_GAP_MS) {
        this->stage_ = Stage::POLLING;
        ESP_LOGI(TAG, "[%s] Connected, polling", this->parent_->address_str());
        this->publish_bin_(this->connected_binary_sensor_, true);
        this->pending_mask_ |= this->wanted_mask_;
      }
      break;
    }
    case Stage::POLLING:
      if (this->inflight_ && now - this->t_last_tx_ > REPLY_TIMEOUT_MS) {
        if (this->last_req_ == REQ_WRITE) {
          ESP_LOGW(TAG, "%s: no reply from pump", this->write_label_);
          this->finish_write_("timeout");
        } else {
          ESP_LOGD(TAG, "Reply timeout (%s)", req_name_(this->last_req_));
          this->inflight_ = false;
        }
      }
      if (!this->inflight_ && this->write_count_ != 0 && now - this->t_last_tx_ >= MIN_TX_GAP_MS) {
        const auto &w = this->writes_[this->write_head_];
        this->write_head_ = (this->write_head_ + 1) % WRITE_QUEUE;
        this->write_count_--;
        this->write_label_ = w.label;
        ESP_LOGD(TAG, "%s: TX %s", w.label, hex_str_(w.buf, w.len).c_str());
        this->last_req_ = REQ_WRITE;
        this->inflight_ = true;
        this->send_frame_(w.buf, w.len);
        break;
      }
      if (!this->inflight_ && this->pending_mask_ != 0 && now - this->t_last_tx_ >= MIN_TX_GAP_MS)
        this->send_next_request_();
      break;
    default:
      break;
  }
}

void GrundfosAlpha2Go::update() {
  if (this->stage_ != Stage::POLLING) return;
  uint16_t mask = this->wanted_mask_;
  if (++this->update_count_ < SLOW_EVERY) {
    mask &= ~REQ_SLOW_MASK;  // settings only every SLOW_EVERY-th time (always after connecting)
  } else {
    this->update_count_ = 0;
  }
  this->pending_mask_ |= mask;
}

void GrundfosAlpha2Go::handle_reply_(const geni::Reply &r) {
  const uint16_t req = this->inflight_ ? this->last_req_ : 0;  // which query this reply belongs to
  this->inflight_ = false;
  const uint8_t *d = r.data;
  ESP_LOGV(TAG, "Reply type=%u version=%u len=%u", r.type, r.version, r.len);
  switch (r.type) {
    case geni::TYPE_PROCESS_VALUES_EXT:  // 93/290, v2, 36 bytes
      if (r.len >= 32) {
        // Pump supplies flow/head already converted (verified on ALPHA2 GO HK1)
        this->publish_(this->flow_sensor_, geni::be_float(d + 24));  // m3/h
        this->publish_(this->head_sensor_, geni::be_float(d + 28));  // m
      } else if (r.len >= 8) {
        // Fallback for shorter replies: @0 in m3/s, @4 in Pa
        this->publish_(this->flow_sensor_, geni::be_float(d + 0) * 3600.0f);   // m3/s -> m3/h
        this->publish_(this->head_sensor_, geni::be_float(d + 4) / 9806.65f);  // Pa -> m water column
      }
      if (r.len >= 36) {
        ESP_LOGV(TAG, "565: f0=%g f4=%g f8=%g f12=%g f16=%g f20=%g", geni::be_float(d + 0), geni::be_float(d + 4),
                 geni::be_float(d + 8), geni::be_float(d + 12), geni::be_float(d + 16), geni::be_float(d + 20));
        ESP_LOGV(TAG, "565: flow_m3h=%.3f head_m=%.3f kv=%.3f (Felder @24/28/32)", geni::be_float(d + 24),
                 geni::be_float(d + 28), geni::be_float(d + 32));
      }
      break;
    case geni::TYPE_MOTOR_DETAILS:  // 87/69, v1 (37 B) / v2/v3 (41 B)
      if (r.len >= 24) {
        this->publish_(this->voltage_sensor_, geni::be_float(d + 0));
        this->publish_(this->dc_voltage_sensor_, geni::be_float(d + 4));
        this->publish_(this->current_sensor_, geni::be_float(d + 8));
        this->publish_(this->power_sensor_, geni::be_float(d + 12));
        this->publish_(this->speed_sensor_, geni::be_float(d + 20));
      }
      if (r.len >= 36) this->publish_(this->aux_temperature_sensor_, geni::be_float(d + 32));
      if (r.len >= 36) {
        ESP_LOGV(TAG, "256: ac=%g dc=%g i=%g p=%g pdc=%g n=%g t24=%g t28=%g t32=%g", geni::be_float(d + 0),
                 geni::be_float(d + 4), geni::be_float(d + 8), geni::be_float(d + 12), geni::be_float(d + 16),
                 geni::be_float(d + 20), geni::be_float(d + 24), geni::be_float(d + 28), geni::be_float(d + 32));
      }
      break;
    case geni::TYPE_OPERATION_HISTORY:  // 93/1, v2, 28 bytes
      ESP_LOGV(TAG, "248: %s", hex_str_(d, r.len).c_str());
      if (r.len >= 12) {
        if (this->starts_sensor_ != nullptr) this->starts_sensor_->publish_state((float) geni::be_u32(d + 0));
        if (this->operating_hours_sensor_ != nullptr)
          this->operating_hours_sensor_->publish_state((float) geni::be_u32(d + 8) / 3600.0f);
      }
      if (r.len >= 28 && this->power_on_hours_sensor_ != nullptr)
        this->power_on_hours_sensor_->publish_state((float) geni::be_u32(d + 24) / 3600.0f);
      break;
    case geni::TYPE_MEDIA_TEMPERATURE:  // 93/300
      ESP_LOGV(TAG, "534: %s", hex_str_(d, r.len).c_str());
      if (r.len >= 12) {
        ESP_LOGV(TAG, "534: f0=%g f4=%g f8=%g", geni::be_float(d + 0), geni::be_float(d + 4), geni::be_float(d + 8));
      }
      if (r.len >= 12) {
        this->publish_(this->media_temperature_sensor_, geni::be_float(d + 0));
        this->publish_(this->pcb_temperature_sensor_, geni::be_float(d + 4));
        this->publish_(this->controlbox_temperature_sensor_, geni::be_float(d + 8));
      }
      break;
    case geni::TYPE_ENERGY: {  // 87/1, v1: double@0 J, float@8 W, u16@12 n, n x {float W, double J}
      if (r.len < 12) break;
      const double joule = geni::be_double(d + 0);
      ESP_LOGV(TAG, "232: system_energy=%.0f J (%.4f kWh) system_power=%.2f W", joule, joule / 3.6e6,
               geni::be_float(d + 8));
      if (r.len >= 14) {
        const uint16_t pumps = geni::be_u16(d + 12);
        for (uint16_t i = 0; i < pumps && 14u + 12u * (i + 1u) <= r.len; i++)
          ESP_LOGV(TAG, "232: pump %u power=%.2f W energy=%.0f J", i, geni::be_float(d + 14 + 12 * i),
                   geni::be_double(d + 18 + 12 * i));
      }
      if (std::isfinite(joule) && joule >= 0) this->publish_(this->energy_sensor_, (float) (joule / 3.6e6));
      break;
    }
    case geni::TYPE_OPERATION_STATUS: {  // 86/6, 86/7, 86/8 (same type -> matched via the query)
      if (r.len < 7) break;
      const uint8_t src = d[0], mode = d[1], ctrl = d[2];
      const float sp = geni::be_float(d + 3);
      ESP_LOGD(TAG, "%s: source=%u mode=%u (%s) control=%u (%s) setpoint=%g", req_name_(req), src, mode,
               operation_mode_name(mode), ctrl, control_mode_name(ctrl), sp);
      const SetpointKind kind = setpoint_kind(ctrl);
      if (req == REQ_OP_LOCAL) {
        this->op_valid_ = true;
        this->op_src_ = src;
        this->op_mode_ = mode;
        this->op_ctrl_ = ctrl;
        this->op_sp_ = sp;
        // Don't remember AutoAdapt setpoints: switching back should use our own setpoint
        if (kind != SP_UNKNOWN && std::isfinite(sp) && !is_autoadapt(ctrl)) this->last_sp_[kind] = sp;
        this->publish_controls_();
      } else if (req == REQ_OP_PRIO) {
        this->publish_(this->operation_mode_sensor_, mode);
        this->publish_(this->control_mode_sensor_, ctrl);
        this->publish_(this->setpoint_sensor_, sp);
        // Fixed-unit sensors; NaN if the unit doesn't apply to the current control mode
        this->publish_or_nan_(this->setpoint_head_sensor_, kind == SP_PRESSURE ? sp / PA_PER_M : NAN);
        this->publish_or_nan_(this->setpoint_flow_sensor_, kind == SP_FLOW ? sp * 3600.0f : NAN);
        this->publish_or_nan_(this->setpoint_speed_sensor_, kind == SP_SPEED ? sp : NAN);
        auto *om = operation_mode_def(mode);
        auto *cm = control_mode_def(ctrl);
        char buf[24];
        if (om == nullptr) snprintf(buf, sizeof(buf), "Unknown (%u)", mode);
        this->publish_text_(this->operation_mode_text_sensor_, om != nullptr ? om->text : buf);
        if (cm == nullptr) snprintf(buf, sizeof(buf), "Unknown (%u)", ctrl);
        this->publish_text_(this->control_mode_text_sensor_, cm != nullptr ? cm->text : buf);
      } else if (req == REQ_OP_REALIZED) {
        this->publish_or_nan_(this->setpoint_actual_sensor_, kind == SP_PRESSURE ? sp / PA_PER_M : NAN);
        this->publish_or_nan_(this->setpoint_actual_speed_sensor_, kind == SP_SPEED ? sp : NAN);
        if (this->control_source_text_sensor_ != nullptr) {
          char sbuf[24];
          const char *st = control_source_text(src);
          if (st == nullptr) {
            snprintf(sbuf, sizeof(sbuf), "Unknown (%u)", src);
            st = sbuf;
          }
          this->publish_text_(this->control_source_text_sensor_, st);
        }
        // "Vent pump only" does not appear in 88/612 but as control source AirVenting (111) in 86/8
        this->pump_venting_ = (src == SOURCE_AIR_VENTING);
        this->publish_venting_();
      }
      break;
    }
    case geni::TYPE_AUTONIGHT_ENABLE:  // 90/8
      if (r.len < 1) break;
      ESP_LOGD(TAG, "90/8: auto_night_enabled=%u", d[0]);
      this->publish_bin_(this->night_setback_enabled_binary_sensor_, d[0] != 0);
      if (this->night_setback_switch_ != nullptr) this->night_setback_switch_->publish_state(d[0] != 0);
      break;
    case geni::TYPE_AUTONIGHT_STATUS:  // 90/7
      if (r.len < 1) break;
      ESP_LOGD(TAG, "90/7: is_it_night=%u", d[0]);
      this->publish_bin_(this->night_mode_binary_sensor_, d[0] != 0);
      break;
    case geni::TYPE_AIR_DETECTION_CONFIG:  // 88/600
      if (r.len < 8) break;
      ESP_LOGD(TAG, "88/600: speed_change=%u pressure_change=%u sensitivity=%u ignore_start=%u enabled=%u",
               geni::be_u16(d + 0), geni::be_u16(d + 2), geni::be_u16(d + 4), d[6], d[7]);
      this->publish_bin_(this->air_detection_enabled_binary_sensor_, d[7] != 0);
      memcpy(this->air_cfg_, d, sizeof(this->air_cfg_));
      this->air_cfg_valid_ = true;
      if (this->air_detection_switch_ != nullptr) this->air_detection_switch_->publish_state(d[7] != 0);
      break;
    case geni::TYPE_AIR_DETECT_COUNTER:  // 88/604
      if (r.len < 2) break;
      ESP_LOGD(TAG, "88/604: detects_first_24h=%u detects_after_168h=%u", d[0], d[1]);
      this->publish_(this->air_detections_sensor_, d[1]);
      break;
    case geni::TYPE_AIR_VENTING_STATUS: {  // 88/612
      if (r.len < 5) break;
      ESP_LOGD(TAG, "88/612: cmd=%u state=%u progress=%u algorithm=%u", d[0], d[1], geni::be_u16(d + 2), d[4]);
      this->system_venting_state_ = d[1];
      this->publish_venting_();
      break;
    }
    default:
      ESP_LOGD(TAG, "Unknown reply type %u (version %u, %u bytes): %s", r.type, r.version, r.len,
               hex_str_(d, r.len).c_str());
      break;
  }
}

}  // namespace grundfos_alpha2go
}  // namespace esphome

#endif
