// Wii Remote HID model: reports, memory, I2C devices (camera, speaker, Nunchuk).
#include "orca/wiimote/wiimote.hpp"
#include "test_framework.hpp"

#include <algorithm>
#include <vector>

using namespace orca;
using namespace orca::wiimote;

namespace {
std::vector<std::vector<u8>> drain(Wiimote& w) {
    std::vector<std::vector<u8>> v;
    while (auto r = w.pop_report()) v.push_back(*r);
    return v;
}
bool send(Wiimote& w, std::vector<u8> r) {
    r.resize(std::max<std::size_t>(r.size(), 22), 0);
    return w.output_report(r);
}
// write report 0x16: space (0 EEPROM, 1 registers), 24-bit address, data
bool write_mem(Wiimote& w, u8 space, u32 addr, std::vector<u8> data) {
    std::vector<u8> r{0x16, static_cast<u8>(space << 2), static_cast<u8>(addr >> 16), static_cast<u8>(addr >> 8),
                      static_cast<u8>(addr), static_cast<u8>(data.size())};
    r.insert(r.end(), data.begin(), data.end());
    return send(w, r);
}
bool read_mem(Wiimote& w, u8 space, u32 addr, u16 size) {
    return send(w, {0x17, static_cast<u8>(space << 2), static_cast<u8>(addr >> 16), static_cast<u8>(addr >> 8),
                    static_cast<u8>(addr), static_cast<u8>(size >> 8), static_cast<u8>(size)});
}
} // namespace

TEST_CASE(wiimote_core_reports_modes_and_acks) {
    Wiimote w;
    InputState in;
    w.update(in);
    auto r = drain(w);
    CHECK_EQ(r.size(), 1U);                       // first core report
    CHECK(r[0] == std::vector<u8>({0x30, 0x00, 0x00}));
    w.update(in);
    CHECK(drain(w).empty());                      // not continuous: nothing changed
    in.buttons = button::kA | button::kUp | 0x2000; // a bit outside the mask is dropped
    w.update(in);
    r = drain(w);
    CHECK_EQ(r.size(), 1U);
    CHECK(r[0] == std::vector<u8>({0x30, 0x08, 0x08}));
    // LEDs with acknowledge, rumble bit
    CHECK(send(w, {0x11, 0x13}));
    CHECK_EQ(w.leds(), 1U);
    CHECK(w.rumble());
    r = drain(w);
    CHECK(r.size() == 1U && r[0] == std::vector<u8>({0x22, 0x08, 0x08, 0x11, 0x00}));
    // continuous core + accelerometer
    CHECK(send(w, {0x12, 0x04, 0x31}));
    in.accel_x = 0x203; in.accel_y = 0x1FE; in.accel_z = 0x26A;
    w.update(in);
    w.update(in);
    r = drain(w);
    CHECK_EQ(r.size(), 2U);
    // x LSBs (3) in byte 1 bits 5-6, y bit 1 (1) in byte 2 bit 5, z bit 1 (1) in byte 2 bit 6
    CHECK(r[0] == std::vector<u8>({0x31, 0x08 | 0x60, 0x08 | 0x20 | 0x40, 0x80, 0x7F, 0x9A}));
    // status report
    CHECK(send(w, {0x15, 0x00}));
    r = drain(w);
    CHECK(r.size() == 1U && r[0] == std::vector<u8>({0x20, 0x08, 0x08, 0x10, 0, 0, 0xC0}));
    in.battery = 0x10;
    w.update(in);
    drain(w);
    CHECK(send(w, {0x15, 0x00}));
    CHECK_EQ(drain(w)[0][3], 0x11U); // battery low
    // invalid / not emulated
    Wiimote a;
    CHECK(!send(a, {0x12, 0x00, 0x3E}));
    CHECK(a.error().find("interleaved") != std::string::npos);
    Wiimote b;
    CHECK(!send(b, {0x1B, 0x00}));
    CHECK(b.error().find("1B") != std::string::npos);
}

TEST_CASE(wiimote_memory_reads_writes_and_errors) {
    Wiimote w;
    InputState in;
    w.update(in);
    drain(w);
    // accelerometer calibration: 0 g = 0x80, 1 g = 0x9A, checksum 0x55 + sum
    CHECK(read_mem(w, 0, 0x16, 0x14));
    CHECK(read_mem(w, 0, 0, 1) );               // busy while a read runs
    auto r = drain(w);
    CHECK(r.size() == 1U && r[0] == std::vector<u8>({0x22, 0, 0, 0x17, 4}));
    w.update(in);
    w.update(in);
    w.update(in);
    r = drain(w);
    CHECK_EQ(r.size(), 2U);                     // 16 + 4 bytes, one reply per period
    CHECK_EQ(r[0][0], 0x21U);
    CHECK_EQ(r[0][3], 0xF0U);                   // 16 bytes, no error
    CHECK_EQ(r[0][5], 0x16U);
    CHECK_EQ(r[0][6], 0x80U);
    CHECK_EQ(r[0][10], 0x9AU);
    const u8 sum = static_cast<u8>(0x55 + 3 * 0x80 + 3 * 0x9A);
    CHECK_EQ(r[0][15], sum);
    CHECK_EQ(r[1][3], 0x30U);                   // 4 bytes
    CHECK_EQ(r[1][5], 0x26U);
    // EEPROM write and read back; outside the usable EEPROM: error 8
    CHECK(write_mem(w, 0, 0x1000, {1, 2, 3}));
    r = drain(w);
    CHECK(r.size() == 1U && r[0][3] == 0x16U && r[0][4] == 0U);
    CHECK_EQ(w.eeprom()[0x1001], 2U);
    CHECK(write_mem(w, 0, 0x16FF, {1, 2}));
    CHECK_EQ(drain(w)[0][4], 8U);
    CHECK(read_mem(w, 0, 0x16F8, 0x10));
    w.update(in);
    r = drain(w);
    CHECK_EQ(r[0][3], 0xF8U);                   // error 8, size nibble 0xF
    // registers of absent devices: no answer (7); the IR camera is off: 7 too
    CHECK(write_mem(w, 1, 0xA60000, {0x55}));
    CHECK_EQ(drain(w)[0][4], 7U);
    CHECK(write_mem(w, 1, 0xB00030, {0x08}));
    CHECK_EQ(drain(w)[0][4], 7U);
    CHECK(write_mem(w, 3, 0, {0}));
    CHECK_EQ(drain(w)[0][4], 6U);
    CHECK(!write_mem(w, 0, 0, {}));             // empty write
}

TEST_CASE(wiimote_ir_camera_modes) {
    Wiimote w;
    InputState in;
    in.points[0] = {0x123, 0x2BC, 3};
    in.points[1] = {0x3FE, 0x001, 5};
    w.update(in);
    drain(w);
    CHECK(send(w, {0x13, 0x04}));
    CHECK(send(w, {0x1A, 0x04}));
    CHECK(w.ir_enabled());
    CHECK(write_mem(w, 1, 0xB00030, {0x01}));
    CHECK(write_mem(w, 1, 0xB00033, {0x03}));   // extended
    auto r = drain(w);
    CHECK(r.size() == 2U && r[0][4] == 0U && r[1][4] == 0U);
    // tracking not enabled yet (0x30 must be 8): 0xFF
    CHECK(send(w, {0x12, 0x04, 0x33}));
    w.update(in);
    r = drain(w);
    CHECK(std::all_of(r[0].begin() + 6, r[0].end(), [](u8 b) { return b == 0xFF; }));
    CHECK(write_mem(w, 1, 0xB00030, {0x08}));
    drain(w);
    w.update(in);
    r = drain(w);
    CHECK_EQ(r[0].size(), 18U);
    const std::vector<u8> ext(r[0].begin() + 6, r[0].end());
    CHECK(ext == std::vector<u8>({0x23, 0xBC, 0x80 | 0x10 | 3, 0xFE, 0x01, 0x30 | 5, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}));
    // basic mode in report 0x37 (10 bytes after the accelerometer)
    CHECK(write_mem(w, 1, 0xB00033, {0x01}));
    CHECK(send(w, {0x12, 0x04, 0x37}));
    drain(w);
    w.update(in);
    r = drain(w);
    const std::vector<u8> basic(r[0].begin() + 6, r[0].begin() + 16);
    // objects 1 and 2 share byte 2; absent objects 3 and 4 read 0x3FF
    CHECK(basic == std::vector<u8>({0x23, 0xBC, 0x80 | 0x10 | 0x00 | 0x03, 0xFE, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}));
    // camera off: the report carries 0xFF
    CHECK(send(w, {0x13, 0x00}));
    w.update(in);
    r = drain(w);
    CHECK(std::all_of(r[0].begin() + 6, r[0].begin() + 16, [](u8 b) { return b == 0xFF; }));
}

TEST_CASE(wiimote_nunchuk_connection_and_data) {
    Wiimote w;
    InputState in;
    w.update(in);
    drain(w);
    CHECK(send(w, {0x12, 0x00, 0x32}));
    in.nunchuk = NunchukState{};
    in.nunchuk->stick_x = 0x90;
    in.nunchuk->accel_x = 0x201;
    in.nunchuk->c = true;
    w.update(in);
    auto r = drain(w);
    CHECK(r.size() == 1U && r[0][0] == 0x20U && (r[0][3] & 2U) != 0U); // status: extension connected
    CHECK_EQ(w.reporting_mode(), 0U);
    w.update(in);
    CHECK(drain(w).empty());                    // reporting disabled until a mode is set
    // identify: 6 bytes at 0xA400FA
    CHECK(read_mem(w, 1, 0xA400FA, 6));
    w.update(in);
    r = drain(w);
    CHECK(std::vector<u8>(r[0].begin() + 6, r[0].begin() + 12) == std::vector<u8>({0, 0, 0xA4, 0x20, 0, 0}));
    // calibration checksums
    CHECK(read_mem(w, 1, 0xA40020, 16));
    w.update(in);
    r = drain(w);
    u8 sum = 0x55;
    for (int i = 6; i < 20; ++i) sum = static_cast<u8>(sum + r[0][static_cast<std::size_t>(i)]);
    CHECK_EQ(r[0][20], sum);
    CHECK_EQ(r[0][21], static_cast<u8>(sum + 0x55));
    // "new" initialisation (no encryption), then data in report 0x32
    CHECK(write_mem(w, 1, 0xA400F0, {0x55}));
    CHECK(write_mem(w, 1, 0xA400FB, {0x00}));
    CHECK(send(w, {0x12, 0x00, 0x32}));
    drain(w);
    w.update(in);
    r = drain(w);
    CHECK(std::vector<u8>(r[0].begin() + 3, r[0].begin() + 9) ==
          std::vector<u8>({0x90, 0x80, 0x80, 0x80, 0xB3, static_cast<u8>(0x01 << 2 | 0x01)})); // C pressed, Z released
    // disconnect: status again
    in.nunchuk.reset();
    w.update(in);
    r = drain(w);
    CHECK(r.size() == 1U && r[0][0] == 0x20U && (r[0][3] & 2U) == 0U);
    // encrypted mode with the zero key: every byte is (x - 0x17) ^ 0x17
    Wiimote e;
    in.nunchuk = NunchukState{};
    e.update(in);
    drain(e);
    CHECK(write_mem(e, 1, 0xA400F0, {0xAA}));
    CHECK(write_mem(e, 1, 0xA40040, std::vector<u8>(16, 0)));
    CHECK(read_mem(e, 1, 0xA400FA, 6));
    e.update(in);
    r = drain(e);
    const u8 id[6] = {0, 0, 0xA4, 0x20, 0, 0};
    for (int i = 0; i < 6; ++i)
        CHECK_EQ(r.back()[static_cast<std::size_t>(6 + i)], static_cast<u8>(static_cast<u8>(id[i] - 0x17) ^ 0x17));
    CHECK(!e.failed());
    // another key: explicit stop
    CHECK(write_mem(e, 1, 0xA40040, {1}));
    CHECK(read_mem(e, 1, 0xA40000, 6));
    e.update(in);
    CHECK(e.failed());
    CHECK(e.error().find("non-zero key") != std::string::npos);
}

TEST_CASE(wiimote_speaker_adpcm) {
    Wiimote w;
    InputState in;
    w.update(in);
    drain(w);
    CHECK(send(w, {0x14, 0x04}));
    CHECK(send(w, {0x19, 0x04}));               // muted while configuring
    // 4-bit ADPCM, 12 MHz / 2000 = 6000 Hz, volume 0x7F, playback and input on
    CHECK(write_mem(w, 1, 0xA20001, {0x00, 0x00, 0xD0, 0x07, 0x7F, 0x0C, 0x0E}));
    CHECK(write_mem(w, 1, 0xA20008, {0x01, 0x01}));
    CHECK(send(w, {0x19, 0x00}));
    drain(w);
    CHECK_EQ(w.speaker_sample_rate(), 6000U);
    std::vector<u8> rep{0x18, static_cast<u8>(4 << 3), 0x17, 0x9F, 0x70, 0x08};
    CHECK(send(w, rep));
    const auto s = w.take_speaker_samples();
    CHECK_EQ(s.size(), 8U);
    // model of the Yamaha ADPCM decoder
    s32 pred = 0, step = 127;
    const s32 diff[16] = {1, 3, 5, 7, 9, 11, 13, 15, -1, -3, -5, -7, -9, -11, -13, -15};
    const s32 scale[16] = {230, 230, 230, 230, 307, 409, 512, 614, 230, 230, 230, 230, 307, 409, 512, 614};
    std::vector<s16> expect;
    for (u8 v : {0x17, 0x9F, 0x70, 0x08})
        for (u8 nib : {static_cast<u8>(v >> 4), static_cast<u8>(v & 0xF)}) {
            pred = std::clamp(pred + step * diff[nib] / 8, -32768, 32767);
            step = std::clamp((step * scale[nib]) >> 8, 127, 24576);
            expect.push_back(static_cast<s16>(pred));
        }
    CHECK(s == expect);
    // muted: decoded but not played
    CHECK(send(w, {0x19, 0x04}));
    CHECK(send(w, rep));
    CHECK(w.take_speaker_samples().empty());
    // unknown format: explicit stop
    CHECK(send(w, {0x19, 0x00}));
    CHECK(write_mem(w, 1, 0xA20002, {0x60}));
    CHECK(!send(w, rep));
    CHECK(w.error().find("format") != std::string::npos);
}

TEST_CASE(wiimote_pointer_mapping) {
    auto p = pointer_points(0.0F, 0.0F);
    CHECK(p[0].x == 432 && p[1].x == 592 && p[0].y == 384 && p[1].y == 384 && p[0].size == 3);
    CHECK(!p[2].visible() && !p[3].visible());
    p = pointer_points(0.5F, -0.5F); // pointing right and down: the dots move left and up in the image
    CHECK(p[0].x == 176 && p[1].x == 336 && p[0].y == 192);
    p = pointer_points(1.0F, 0.0F); // the left dot leaves the image
    CHECK(!p[0].visible() && p[1].visible() && p[1].x == 80);
}

int main() { return orca::test::run_all("wiimote"); }
