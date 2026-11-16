// Test of the hardware-independent parser: g++ -std=c++17 -I../components/grundfos_alpha2go test_geni.cpp && ./a.out
#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>
#include "geni.h"
using namespace esphome::grundfos_alpha2go::geni;

static std::vector<uint8_t> hex(const char *s) {
  std::vector<uint8_t> v;
  unsigned x;
  while (*s) {
    if (*s == ' ') { s++; continue; }
    sscanf(s, "%2x", &x); v.push_back((uint8_t) x); s += 2;
  }
  return v;
}
static std::vector<uint8_t> reply(uint16_t type, uint8_t ver, const std::vector<uint8_t> &data) {
  std::vector<uint8_t> f = {SD_REPLY, (uint8_t) (2 + 2 + 7 + data.size()), ADDR_CLIENT, ADDR_PUMP, CLASS_OBJECT,
                            (uint8_t) (7 + data.size()), 0x00, (uint8_t) (type >> 8), (uint8_t) type, ver, 0, 0,
                            (uint8_t) data.size()};
  f.insert(f.end(), data.begin(), data.end());
  uint16_t c = crc16(f.data() + 1, f.size() - 1);
  f.push_back(c >> 8); f.push_back(c & 0xFF);
  return f;
}
static std::vector<uint8_t> floats(std::initializer_list<float> l) {
  std::vector<uint8_t> v;
  for (float f : l) { uint32_t u; memcpy(&u, &f, 4); for (int i = 3; i >= 0; i--) v.push_back(u >> (8 * i)); }
  return v;
}
static std::vector<uint8_t> tobytes(const uint8_t *p, size_t n) { return std::vector<uint8_t>(p, p + n); }

int main() {
  uint8_t f[16];
  // Request frames: byte-identical to ESPHome alpha3 (ALPHA3 objects) and the Lasse-source capture (ALPHA2 GO)
  assert(tobytes(f, build_object_get(f, 93, 289)) == hex("27 07 e7 f8 0a 03 5d 01 21 52 1f"));
  assert(tobytes(f, build_object_get(f, 87, 69)) == hex("27 07 e7 f8 0a 03 57 00 45 8a cd"));
  assert(tobytes(f, build_object_get(f, 93, 290)) == hex("27 07 e7 f8 0a 03 5d 01 22 62 7c"));
  assert(tobytes(f, build_object_get(f, 93, 1)) == hex("27 07 e7 f8 0a 03 5d 00 01 45 4c"));
  const uint8_t ids[] = {148, 149, 150};
  assert(tobytes(f, build_class2_get(f, ids, 3)) == hex("27 07 ff f8 02 03 94 95 96 4f 91"));
  assert(build_object_get(f, 93, 300) == 11 && tobytes(f, 11) == hex("27 07 e7 f8 0a 03 5d 01 2c 83 b2"));

  // Reply identifiers from the capture: header bytes 5..12
  auto fh = reply(565, 2, std::vector<uint8_t>(36));
  assert(std::vector<uint8_t>(fh.begin() + 5, fh.begin() + 13) == hex("2b 00 02 35 02 00 00 24"));
  auto eh = reply(256, 3, std::vector<uint8_t>(41));
  assert(std::vector<uint8_t>(eh.begin() + 5, eh.begin() + 13) == hex("30 00 01 00 03 00 00 29"));
  auto ch = reply(248, 2, std::vector<uint8_t>(28));
  assert(std::vector<uint8_t>(ch.begin() + 5, ch.begin() + 13) == hex("23 00 00 f8 02 00 00 1c"));

  // Reassembly with fragmented notifications (7/20/1 bytes), garbage before, broken frame in between
  // Values from the hardware log (ALPHA2 GO HK1, 15.11.2026): @0 m3/s, @4 Pa, @24 m3/h, @28 m, @32 kv
  auto flow = reply(565, 2, floats({0.000469495f, 20716.4f, NAN, NAN, NAN, NAN, 1.690f, 2.113f, 3.696f}));
  auto elec = reply(256, 3, floats({235.2f, 324.0f, 0.169f, 37.88f, 38.f, 4542.f, NAN, NAN, 51.0f, 0.f}));
  std::vector<uint8_t> hist(28, 0);
  hist[3] = 6;                                   // starts = 6
  uint32_t op = 31510844; for (int i = 0; i < 4; i++) hist[8 + i] = op >> (24 - 8 * i);
  auto cnt = reply(248, 2, hist);
  auto bad = flow; bad[20] ^= 0xFF;              // CRC error
  std::vector<uint8_t> stream = {0x00, 0x11, 0x22};  // garbage
  for (auto *v : {&bad, &flow, &elec, &cnt}) stream.insert(stream.end(), v->begin(), v->end());

  for (size_t chunk : {1u, 7u, 20u, 64u}) {
    Reassembler r;
    int n_flow = 0, n_elec = 0, n_cnt = 0, errs = 0;
    float q = 0, h = 0, q_calc = 0, h_calc = 0, u = 0, p = 0, rpm = 0, aux = 0, starts = 0, hours = 0;
    for (size_t i = 0; i < stream.size(); i += chunk) {
      errs += r.feed(stream.data() + i, std::min(chunk, stream.size() - i), [&](const Reply &rp) {
        if (rp.type == TYPE_PROCESS_VALUES_EXT) {
          n_flow++;
          q = be_float(rp.data + 24); h = be_float(rp.data + 28);                              // direct
          q_calc = be_float(rp.data) * 3600.f; h_calc = be_float(rp.data + 4) / 9806.65f;     // fallback
        }
        if (rp.type == TYPE_MOTOR_DETAILS) { n_elec++; u = be_float(rp.data); p = be_float(rp.data + 12); rpm = be_float(rp.data + 20); aux = be_float(rp.data + 32); }
        if (rp.type == TYPE_OPERATION_HISTORY) { n_cnt++; starts = be_u32(rp.data); hours = be_u32(rp.data + 8) / 3600.f; }
      });
    }
    assert(errs == 1 && n_flow == 1 && n_elec == 1 && n_cnt == 1);
    assert(fabsf(q - 1.690f) < 1e-4 && fabsf(h - 2.113f) < 1e-4);
    assert(fabsf(q_calc - q) < 1e-3 && fabsf(h_calc - h) < 1e-3);  // fallback conversion matches @24/@28
    assert(fabsf(u - 235.2f) < 1e-3 && fabsf(p - 37.88f) < 1e-3 && rpm == 4542.f && aux == 51.0f);
    assert(starts == 6 && fabsf(hours - 8752.97f) < 0.05f);
  }
  // Real 87/1 frame (type 232) from the hardware log, in three notifications as received
  {
    const uint8_t n1[] = {0x24, 0x25, 0xF8, 0xE7, 0x0A, 0x21, 0x00, 0x00, 0xE8, 0x01,
                          0x00, 0x00, 0x1A, 0x41, 0xB0, 0x9F, 0x60, 0x5D, 0xFC, 0x40};
    const uint8_t n2[] = {0xA0, 0x41, 0xB0, 0x4F, 0x00, 0x00, 0x01, 0x41, 0xB0, 0x4F,
                          0x00, 0x41, 0xB0, 0x9F, 0x60, 0x5D, 0xFC, 0x40, 0xA0, 0x29};
    const uint8_t n3[] = {0xB1};
    Reassembler r;
    int got = 0, errs = 0;
    double joule = 0, pump_joule = 0;
    float pw = 0, pump_pw = 0;
    uint16_t pumps = 0;
    auto cb = [&](const Reply &rp) {
      got++;
      assert(rp.type == TYPE_ENERGY && rp.version == 1 && rp.len == 26);
      joule = be_double(rp.data);
      pw = be_float(rp.data + 8);
      pumps = be_u16(rp.data + 12);
      pump_pw = be_float(rp.data + 14);
      pump_joule = be_double(rp.data + 18);
    };
    errs += r.feed(n1, sizeof(n1), cb);
    errs += r.feed(n2, sizeof(n2), cb);
    errs += r.feed(n3, sizeof(n3), cb);
    assert(errs == 0 && got == 1);
    assert(fabs(joule - 278880350.0) < 1.0 && fabs(joule / 3.6e6 - 77.4668) < 1e-4);
    assert(fabsf(pw - 22.04f) < 0.01f && pumps == 1 && pump_pw == pw && pump_joule == joule);
  }

  // Type 303 (OperationStatusRequest), real 86/7 frames from the hardware log:
  // u8 source, u8 mode, u8 control, float setpoint (SI: Pa or m3/s)
  {
    struct Case {
      std::vector<uint8_t> frame;
      uint8_t ctrl;
      float sp;
    };
    const Case cases[] = {
        // Proportional pressure, setpoint 24509.8 Pa = 2.50 m
        {{0x24, 0x12, 0xF8, 0xE7, 0x0A, 0x0E, 0x00, 0x01, 0x2F, 0x01, 0x00, 0x00,
          0x07, 0x01, 0x00, 0x01, 0x46, 0xBF, 0x7B, 0x9C, 0x64, 0x6D}, 1, 24509.8f},
        // Constant flow, setpoint 0.000277778 m3/s = 1.00 m3/h
        {{0x24, 0x12, 0xF8, 0xE7, 0x0A, 0x0E, 0x00, 0x01, 0x2F, 0x01, 0x00, 0x00,
          0x07, 0x01, 0x00, 0x08, 0x39, 0x91, 0xA2, 0xB4, 0xCD, 0x98}, 8, 0.000277778f},
        // Constant speed, setpoint 4059.8 1/min (measured speed 4063-4068)
        {{0x24, 0x12, 0xF8, 0xE7, 0x0A, 0x0E, 0x00, 0x01, 0x2F, 0x01, 0x00, 0x00,
          0x07, 0x01, 0x00, 0x02, 0x45, 0x7D, 0xBC, 0xCD, 0x9C, 0xE3}, 2, 4059.8f},
    };
    for (const auto &c : cases) {
      Reassembler r;
      int got = 0;
      int errs = r.feed(c.frame.data(), c.frame.size(), [&](const Reply &rp) {
        got++;
        assert(rp.type == TYPE_OPERATION_STATUS && rp.version == 1 && rp.len == 7);
        assert(rp.data[0] == 1 && rp.data[1] == 0 && rp.data[2] == c.ctrl);  // Panel, Auto
        assert(fabsf(be_float(rp.data + 3) - c.sp) / c.sp < 1e-5f);
      });
      assert(errs == 0 && got == 1);
    }
    assert(fabsf(24509.8f / 9806.65f - 2.4993f) < 1e-3f);   // -> setpoint_head
    assert(fabsf(0.000277778f * 3600.0f - 1.0f) < 1e-4f);  // -> setpoint_flow
  }

  // Short replies (1-8 bytes) from the settings objects: length check in the reassembler (13 + dl + 2 == tot)
  {
    const struct {
      uint16_t type;
      std::vector<uint8_t> data;
    } cases[] = {
        {TYPE_AUTONIGHT_ENABLE, {1}},
        {TYPE_AUTONIGHT_STATUS, {0}},
        {TYPE_AIR_DETECTION_CONFIG, {0x00, 0x0A, 0x00, 0x14, 0x00, 0x03, 0x1E, 0x01}},
        {TYPE_AIR_DETECT_COUNTER, {2, 5}},
        {TYPE_AIR_VENTING_STATUS, {0, 2, 0x00, 0x32, 3}},
    };
    for (const auto &c : cases) {
      auto f = reply(c.type, 1, c.data);
      Reassembler r;
      int got = 0;
      r.feed(f.data(), f.size(), [&](const Reply &rp) {
        got++;
        assert(rp.type == c.type && rp.len == c.data.size());
        assert(memcmp(rp.data, c.data.data(), c.data.size()) == 0);
      });
      assert(got == 1);
    }
  }

  // SET builder against the telegram the pump accepted with status 00 (probe 4, 90/8 = 0)
  {
    const uint8_t off = 0, on = 1;
    uint8_t f[MAX_TX_FRAME + 4];
    size_t n = build_object_set(f, 90, 8, TYPE_AUTONIGHT_ENABLE, 1, &off, 1);
    const uint8_t ref[] = {0x27, 0x0E, 0xE7, 0xF8, 0x0A, 0x8A, 0x5A, 0x00, 0x08,
                           0x01, 0x4D, 0x01, 0x00, 0x00, 0x01, 0x00, 0xF1, 0xFC};
    assert(n == sizeof(ref) && memcmp(f, ref, n) == 0);
    n = build_object_set(f, 90, 8, TYPE_AUTONIGHT_ENABLE, 1, &on, 1);
    assert(n == 18 && f[15] == 0x01 && crc16(f + 1, f[1] + 1) == (uint16_t) ((f[n - 2] << 8) | f[n - 1]));
    uint8_t big[16] = {0};
    assert(build_object_set(f, 1, 1, 1, 1, big, 3) == 20);
    // 86/6 = current values (src 0, Auto, PP, 24509.8 Pa): 24 bytes -> two BLE writes
    const uint8_t op[] = {0x00, 0x00, 0x01, 0x46, 0xBF, 0x7B, 0x9C};
    n = build_object_set(f, 86, 6, TYPE_OPERATION_STATUS, 1, op, 7);
    const uint8_t opref[] = {0x27, 0x14, 0xE7, 0xF8, 0x0A, 0x90, 0x56, 0x00, 0x06, 0x01, 0x2F, 0x01,
                             0x00, 0x00, 0x07, 0x00, 0x00, 0x01, 0x46, 0xBF, 0x7B, 0x9C};
    assert(n == 24 && memcmp(f, opref, sizeof(opref)) == 0);
    assert(crc16(f + 1, f[1] + 1) == (uint16_t) ((f[n - 2] << 8) | f[n - 1]));
    assert(build_object_set(f, 1, 1, 1, 1, big, 16) == 0);  // 33 bytes -> too long
  }

  // Class 3 command STOP: 27 05 E7 F8 03 81 05 CRC (9 bytes)
  {
    uint8_t f[16];
    size_t n = build_command(f, 5);
    const uint8_t s[] = {0x27, 0x05, 0xE7, 0xF8, 0x03, 0x81, 0x05};
    assert(n == 9 && memcmp(f, s, sizeof(s)) == 0);
    assert(crc16(f + 1, f[1] + 1) == (uint16_t) ((f[7] << 8) | f[8]));
  }

  // Raw APDU: must match the SET and GET builders; empty/too long -> 0
  {
    uint8_t f[MAX_TX_FRAME + 4], g[MAX_TX_FRAME + 4];
    const uint8_t apdu[] = {0x8A, 0x5A, 0x00, 0x08, 0x01, 0x4D, 0x01, 0x00, 0x00, 0x01, 0x00};
    const uint8_t off = 0;
    size_t n = build_class10_raw(f, apdu, sizeof(apdu));
    size_t m = build_object_set(g, 90, 8, TYPE_AUTONIGHT_ENABLE, 1, &off, 1);
    assert(n == 18 && n == m && memcmp(f, g, n) == 0);
    const uint8_t get[] = {0x03, 0x5A, 0x00, 0x08};
    uint8_t h[16];
    build_object_get(h, 90, 8);
    assert(build_class10_raw(f, get, 4) == 11 && memcmp(f, h, 11) == 0);
    uint8_t big[26] = {0};
    assert(build_class10_raw(f, big, 0) == 0 && build_class10_raw(f, big, 25) == 32);
    assert(build_class10_raw(f, big, 26) == 0);
  }

  printf("all tests OK\n");
}
