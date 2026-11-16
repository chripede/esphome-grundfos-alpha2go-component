#pragma once
// Hardware-independent GENI helpers (framing, CRC, reassembly). No ESPHome dependency -> testable on a PC.
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace esphome {
namespace grundfos_alpha2go {
namespace geni {

static constexpr uint8_t SD_REQUEST = 0x27;
static constexpr uint8_t SD_REPLY = 0x24;
static constexpr uint8_t SD_ECHO = 0x30;
static constexpr uint8_t ADDR_PUMP = 0xE7;
static constexpr uint8_t ADDR_CLIENT = 0xF8;
static constexpr uint8_t ADDR_BROADCAST = 0xFF;
static constexpr uint8_t CLASS_MEASURED = 0x02;
static constexpr uint8_t CLASS_OBJECT = 0x0A;
static constexpr size_t MAX_FRAME = 260;

// Object types (reply header, see Grundfos GENI profile 52_x)
static constexpr uint16_t TYPE_MEDIA_TEMPERATURE = 534;   // Obj 93/300
static constexpr uint16_t TYPE_PROCESS_VALUES_EXT = 565;  // Obj 93/290 (Flow/Head)
static constexpr uint16_t TYPE_MOTOR_DETAILS = 256;       // Obj 87/69  (v1/v2/v3 -> same layout for offsets 0..32)
static constexpr uint16_t TYPE_OPERATION_HISTORY = 248;   // Obj 93/1   (counters)
// Layouts from the GENI profile in the GO app (test_geni_profile_30_4_1.xml), not yet verified on the ALPHA2 GO:
static constexpr uint16_t TYPE_ENERGY = 232;            // Obj 87/1 OverallEnergyConsumption: double@0, float@8, u16@12, [float,double]...
static constexpr uint16_t TYPE_OPERATION_STATUS = 303;  // Obj 86/6,7,8 OperationStatusRequest: u8 src, u8 mode, u8 ctrl, float@3
static constexpr uint16_t TYPE_AUTONIGHT_ENABLE = 333;  // Obj 90/8: bool auto_night_enabled
static constexpr uint16_t TYPE_AUTONIGHT_STATUS = 324;  // Obj 90/7: bool is_it_night
static constexpr uint16_t TYPE_AIR_DETECTION_CONFIG = 600;  // Obj 88/600: u16,u16,u16,u8, bool enabled@7
static constexpr uint16_t TYPE_AIR_DETECT_COUNTER = 984;    // Obj 88/604: u8 first 24 h, u8 after 168 h
static constexpr uint16_t TYPE_AIR_VENTING_STATUS = 971;    // Obj 88/612: u8 cmd, u8 state, u16 progress, u8 algo

// CRC-16 (poly 0x1021, init 0xFFFF, result inverted), big endian in the frame.
inline uint16_t crc16(const uint8_t *d, size_t n) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < n; i++) {
    crc ^= (uint16_t) (d[i] << 8);
    for (int k = 0; k < 8; k++)
      crc = (crc & 0x8000) ? (uint16_t) ((crc << 1) ^ 0x1021) : (uint16_t) (crc << 1);
  }
  return (uint16_t) ~crc;
}

inline size_t finish_frame(uint8_t *out) {
  // out[1] = LE is set; CRC over index 1 .. 1+LE
  size_t n = (size_t) out[1] + 1;
  uint16_t c = crc16(out + 1, n);
  out[1 + n] = (uint8_t) (c >> 8);
  out[2 + n] = (uint8_t) (c & 0xFF);
  return (size_t) out[1] + 4;
}

// Read a Class 10 object: 27 07 E7 F8 0A 03 <ID> <SubHi> <SubLo> CRC  (11 bytes)
inline size_t build_object_get(uint8_t *out, uint8_t obj_id, uint16_t sub_id) {
  out[0] = SD_REQUEST;
  out[1] = 7;
  out[2] = ADDR_PUMP;
  out[3] = ADDR_CLIENT;
  out[4] = CLASS_OBJECT;
  out[5] = 0x03;  // OS=GET (0), APDU data length 3
  out[6] = obj_id;
  out[7] = (uint8_t) (sub_id >> 8);
  out[8] = (uint8_t) (sub_id & 0xFF);
  return finish_frame(out);
}

// Write a Class 10 object (confirmed on the ALPHA2 GO, reply 0A 01 00 = status OK):
//   27 LE E7 F8 0A <0x80|n> <ID> <SubHi> <SubLo> <TypeHi> <TypeLo> <Ver> 00 00 <DataLen> <Data> CRC
// Object header as in GET replies, but without the leading status byte. OS bits 10 = SET.
// Status in the reply (first data byte): 00 OK, 01 unknown operation (OS 01), 04 object/type not found.
// Frames > BLE_CHUNK are split over several BLE writes (send_frame_). Returns frame length, 0 if too long.
static constexpr size_t BLE_CHUNK = 20;     // payload of one BLE write at MTU 23
static constexpr size_t MAX_TX_FRAME = 32;  // longest frame we send (SET 86/6 = 24 bytes)
inline size_t build_object_set(uint8_t *out, uint8_t obj_id, uint16_t sub_id, uint16_t type, uint8_t version,
                               const uint8_t *data, uint8_t len) {
  const size_t apdu = 3 + 6 + len;
  if (apdu > 0x3F || 6 + apdu + 2 > MAX_TX_FRAME) return 0;
  out[0] = SD_REQUEST;
  out[1] = (uint8_t) (4 + apdu);  // DA, SA, class, APDU header + APDU data
  out[2] = ADDR_PUMP;
  out[3] = ADDR_CLIENT;
  out[4] = CLASS_OBJECT;
  out[5] = (uint8_t) (0x80 | apdu);
  size_t i = 6;
  out[i++] = obj_id;
  out[i++] = (uint8_t) (sub_id >> 8);
  out[i++] = (uint8_t) (sub_id & 0xFF);
  out[i++] = (uint8_t) (type >> 8);
  out[i++] = (uint8_t) (type & 0xFF);
  out[i++] = version;
  out[i++] = 0x00;
  out[i++] = 0x00;
  out[i++] = len;
  for (uint8_t k = 0; k < len; k++) out[i++] = data[k];
  return finish_frame(out);
}

// EXPERIMENTAL - send any Class 10 APDU (header byte + data), for format probing:
//   27 LE E7 F8 0A <apdu...> CRC. Returns frame length, 0 if empty or too long.
inline size_t build_class10_raw(uint8_t *out, const uint8_t *apdu, size_t n) {
  if (n == 0 || 5 + n + 2 > MAX_TX_FRAME) return 0;
  out[0] = SD_REQUEST;
  out[1] = (uint8_t) (3 + n);
  out[2] = ADDR_PUMP;
  out[3] = ADDR_CLIENT;
  out[4] = CLASS_OBJECT;
  for (size_t k = 0; k < n; k++) out[5 + k] = apdu[k];
  return finish_frame(out);
}

// Class 3 command (classic GENIbus format): 27 05 E7 F8 03 81 <cmd> CRC
// OS = 10 (SET), APDU length 1. Commands per profile: 5 STOP, 6 START/AUTO, 7 REMOTE, 8 LOCAL,
// 66/67 (NIGHT_REDUCT_E/D) are rejected by the ALPHA2 GO: reply 03 81 <cmd> = ACK 10 (unknown ID).
static constexpr uint8_t CLASS_COMMAND = 0x03;
inline size_t build_command(uint8_t *out, uint8_t cmd, uint8_t da = ADDR_PUMP) {
  out[0] = SD_REQUEST;
  out[1] = 5;
  out[2] = da;
  out[3] = ADDR_CLIENT;
  out[4] = CLASS_COMMAND;
  out[5] = 0x81;
  out[6] = cmd;
  return finish_frame(out);
}

// Read Class 2 values (app's initial query): 27 LE FF F8 02 <n> <id>... CRC
inline size_t build_class2_get(uint8_t *out, const uint8_t *ids, uint8_t n, uint8_t da = ADDR_BROADCAST) {
  out[0] = SD_REQUEST;
  out[1] = (uint8_t) (4 + n);
  out[2] = da;
  out[3] = ADDR_CLIENT;
  out[4] = CLASS_MEASURED;
  out[5] = n;
  for (uint8_t i = 0; i < n; i++) out[6 + i] = ids[i];
  return finish_frame(out);
}

inline uint32_t be_u32(const uint8_t *p) {
  return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) | ((uint32_t) p[2] << 8) | (uint32_t) p[3];
}
inline uint16_t be_u16(const uint8_t *p) { return (uint16_t) ((p[0] << 8) | p[1]); }
inline float be_float(const uint8_t *p) {
  uint32_t u = be_u32(p);
  float f;
  memcpy(&f, &u, 4);
  return f;
}
inline double be_double(const uint8_t *p) {
  uint64_t u = ((uint64_t) be_u32(p) << 32) | be_u32(p + 4);
  double v;
  memcpy(&v, &u, 8);
  return v;
}

struct Reply {
  uint16_t type;
  uint8_t version;
  const uint8_t *data;  // payload, pointer into the frame
  uint8_t len;
};

// Reassembles GENI replies from arbitrarily fragmented notifications.
// Antwort: SD LE DA SA | 0A <APDU-Len> 00 <TypeHi> <TypeLo> <Version> 00 00 <DataLen> | data | CRC(2)
class Reassembler {
 public:
  // Feeds bytes; calls cb(const Reply&) per valid Class 10 frame. Returns number of CRC errors.
  template<typename F> int feed(const uint8_t *d, size_t n, F cb) {
    int crc_errors = 0;
    if (n_ + n > sizeof(buf_)) n_ = 0;  // overflow -> discard
    if (n > sizeof(buf_)) return 0;
    memcpy(buf_ + n_, d, n);
    n_ += n;
    while (n_ >= 2) {
      if (buf_[0] != SD_REPLY && buf_[0] != SD_ECHO) {
        drop_(1);
        continue;
      }
      size_t tot = (size_t) buf_[1] + 4;
      if (tot < 8 || tot > MAX_FRAME) {
        drop_(1);
        continue;
      }
      if (n_ < tot) break;
      if (crc16(buf_ + 1, tot - 3) != (uint16_t) ((buf_[tot - 2] << 8) | buf_[tot - 1])) {
        crc_errors++;
        drop_(tot);
        continue;
      }
      if (buf_[4] == CLASS_OBJECT && tot >= 15 && buf_[6] == 0x00) {
        uint8_t dl = buf_[12];
        if ((size_t) 13 + dl + 2 == tot) {
          Reply r{(uint16_t) ((buf_[7] << 8) | buf_[8]), buf_[9], buf_ + 13, dl};
          cb(r);
        }
      }
      drop_(tot);
    }
    return crc_errors;
  }
  void reset() { n_ = 0; }

 protected:
  void drop_(size_t k) {
    if (k >= n_) {
      n_ = 0;
      return;
    }
    memmove(buf_, buf_ + k, n_ - k);
    n_ -= k;
  }
  uint8_t buf_[2 * MAX_FRAME]{};
  size_t n_{0};
};

}  // namespace geni
}  // namespace grundfos_alpha2go
}  // namespace esphome
