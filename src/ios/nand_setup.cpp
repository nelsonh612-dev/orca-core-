#include "orca/ios/nand_setup.hpp"

#include <algorithm>

namespace orca::ios {

namespace {
struct Entry {
    u8 type;
    std::string name;
    std::vector<u8> data;
};

constexpr u8 kBigArray = 1, kSmallArray = 2, kByte = 3, kLong = 5, kBool = 7;

std::vector<u8> be32v(u32 v) { return {static_cast<u8>(v >> 24), static_cast<u8>(v >> 16), static_cast<u8>(v >> 8), static_cast<u8>(v)}; }

bool write_file(NandFs& nand, const std::string& path, u32 uid, u16 gid, FsModes modes, std::span<const u8> data,
                std::string& error) {
    FsMeta m;
    if (nand.metadata(0, 0, path, m) == 0) return true; // keep what is there
    if (nand.create(0, 0, path, 0, modes, true) != 0) { error = "cannot create " + path; return false; }
    // the owner changes while the file is empty (IOS refuses it afterwards)
    if (nand.set_metadata(0, path, uid, gid, 0, modes) != 0) { error = "cannot set the owner of " + path; return false; }
    const s32 h = nand.open(0, 0, path, kFsWrite);
    if (h < 0 || nand.write(h, data) != static_cast<s32>(data.size()) || nand.close(h) != 0) {
        error = "cannot write " + path;
        return false;
    }
    return true;
}
} // namespace

BdAddr emulated_remote_address(unsigned index) {
    return {static_cast<u8>(index), 0x41, 0x43, 0x52, 0x4F, 0x02}; // 02:4F:52:43:41:0n, least significant first
}

std::vector<u8> make_sysconf(const NandSetup& s) {
    std::vector<Entry> e;
    // BT.DINF: count, registered[10] and active[6] {address (MSB first), name[64]}
    std::vector<u8> dinf(0x461, 0);
    const unsigned n = std::min(s.remotes, 4U);
    dinf[0] = static_cast<u8>(n);
    for (unsigned i = 0; i < n; ++i) {
        const BdAddr bd = emulated_remote_address(i);
        for (std::size_t base : {std::size_t{1} + i * 70U, std::size_t{1} + 10U * 70U + i * 70U}) {
            std::reverse_copy(bd.begin(), bd.end(), dinf.begin() + static_cast<std::ptrdiff_t>(base));
            const char name[] = "Nintendo RVL-CNT-01";
            std::copy(name, name + sizeof name - 1, dinf.begin() + static_cast<std::ptrdiff_t>(base + 6));
        }
    }
    e.push_back({kBigArray, "BT.DINF", dinf});
    e.push_back({kBigArray, "BT.CDIF", std::vector<u8>(0x205, 0)});
    e.push_back({kLong, "BT.SENS", be32v(3)});
    e.push_back({kByte, "BT.BAR", {s.sensor_bar_top}});
    e.push_back({kByte, "BT.SPKV", {0x58}});
    e.push_back({kByte, "BT.MOT", {1}});
    // nickname: UTF-16BE (at most 10 characters), byte 21 = length
    std::vector<u8> nick(22, 0);
    const std::size_t len = std::min<std::size_t>(s.nickname.size(), 10);
    for (std::size_t i = 0; i < len; ++i) nick[2 * i + 1] = static_cast<u8>(s.nickname[i]);
    nick[21] = static_cast<u8>(len);
    e.push_back({kSmallArray, "IPL.NIK", nick});
    e.push_back({kByte, "IPL.LNG", {s.language}});
    std::vector<u8> sadr(0x1008, 0);
    sadr[0] = s.country;
    e.push_back({kBigArray, "IPL.SADR", sadr});
    std::vector<u8> pc(0x4A, 0);
    pc[1] = 0x04;
    pc[2] = 0x14;
    e.push_back({kSmallArray, "IPL.PC", pc});
    e.push_back({kLong, "IPL.CB", be32v(0)});
    e.push_back({kByte, "IPL.AR", {static_cast<u8>(s.widescreen ? 1 : 0)}});
    e.push_back({kByte, "IPL.SSV", {1}});
    e.push_back({kBool, "IPL.CD", {0}});
    e.push_back({kBool, "IPL.CD2", {0}});
    e.push_back({kBool, "IPL.EULA", {1}});
    e.push_back({kByte, "IPL.UPT", {2}});
    e.push_back({kByte, "IPL.PGS", {static_cast<u8>(s.progressive ? 1 : 0)}});
    e.push_back({kByte, "IPL.E60", {static_cast<u8>(s.eurgb60 ? 1 : 0)}});
    e.push_back({kByte, "IPL.DH", {0}});
    e.push_back({kLong, "IPL.INC", be32v(8)});
    e.push_back({kLong, "IPL.FRC", be32v(0x28)});
    e.push_back({kSmallArray, "IPL.IDL", {0, 1}});
    e.push_back({kLong, "NET.WCFG", be32v(1)});
    e.push_back({kLong, "NET.CTPC", be32v(0)});
    e.push_back({kByte, "WWW.RST", {0}});
    e.push_back({kBool, "MPLS.MOVIE", {1}});

    std::vector<u8> f{'S', 'C', 'v', '0', static_cast<u8>(e.size() >> 8), static_cast<u8>(e.size())};
    const std::size_t begin = f.size() + 2U * (e.size() + 1U);
    std::vector<u8> body;
    const auto offset = [&](std::size_t o) { f.push_back(static_cast<u8>(o >> 8)); f.push_back(static_cast<u8>(o)); };
    for (const Entry& x : e) {
        offset(begin + body.size());
        body.push_back(static_cast<u8>(static_cast<std::size_t>(x.type) << 5 | (x.name.size() - 1U)));
        body.insert(body.end(), x.name.begin(), x.name.end());
        if (x.type == kBigArray) {
            body.push_back(static_cast<u8>((x.data.size() - 1U) >> 8));
            body.push_back(static_cast<u8>(x.data.size() - 1U));
        } else if (x.type == kSmallArray) {
            body.push_back(static_cast<u8>(x.data.size() - 1U));
        }
        body.insert(body.end(), x.data.begin(), x.data.end());
    }
    offset(begin + body.size());
    f.insert(f.end(), body.begin(), body.end());
    f.resize(0x4000, 0);
    std::copy_n("SCed", 4, f.end() - 4);
    return f;
}

namespace {
std::vector<u8> crypt_setting(std::span<const u8> in) {
    std::vector<u8> out(in.size());
    u32 key = 0x73B5DBFAU;
    for (std::size_t i = 0; i < in.size(); ++i) {
        out[i] = static_cast<u8>(in[i] ^ (key & 0xFFU));
        key = key << 1 | key >> 31;
    }
    return out;
}
} // namespace

RegionSettings region_settings(WiiRegion r) {
    switch (r) {
    case WiiRegion::Japan: return {"JPN", "NTSC", "JP", "LJH"};
    case WiiRegion::Europe: return {"EUR", "PAL", "EU", "LEH"};
    case WiiRegion::Korea: return {"KOR", "NTSC", "KR", "LKH"};
    case WiiRegion::Usa: break;
    }
    return {"USA", "NTSC", "US", "LU"};
}

std::vector<u8> make_setting_txt_for(WiiRegion r, const std::string& serial) {
    const RegionSettings g = region_settings(r);
    const std::string text = std::string("AREA=") + g.area + "\r\nMODEL=RVL-001(" + g.area + ")\r\nDVD=0\r\nMPCH=0x7FFE\r\nCODE=" +
                             g.code + "\r\nSERNO=" + serial + "\r\nVIDEO=" + g.video + "\r\nGAME=" + g.game + "\r\n";
    std::vector<u8> plain(0x100, 0);
    std::copy_n(text.begin(), std::min<std::size_t>(text.size(), plain.size()), plain.begin());
    return crypt_setting(plain);
}

std::vector<u8> make_setting_txt(const NandSetup& s) { return make_setting_txt_for(s.region, s.serial); }

std::string setting_value(const std::string& text, const std::string& key) {
    std::size_t pos = 0;
    while (pos < text.size()) {
        std::size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(pos, end - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind(key + "=", 0) == 0) return line.substr(key.size() + 1U);
        pos = end + 1U;
    }
    return {};
}

std::optional<std::vector<u8>> setting_txt_for_disc(NandFs& nand, WiiRegion r, const std::string& new_serial,
                                                    bool& changed, std::string& error) {
    const std::string path = "/title/00000001/00000002/data/setting.txt";
    changed = false;
    std::vector<u8> st(0x100);
    const s32 h = nand.open(0, 0, path, kFsRead);
    if (h < 0) { error = path + " is not on the NAND (" + std::to_string(h) + ")"; return std::nullopt; }
    const s32 n = nand.read(h, st);
    nand.close(h);
    if (n != 0x100) { error = "setting.txt is not 256 bytes"; return std::nullopt; }
    const std::string text = decrypt_setting_txt(st);
    std::string serial = setting_value(text, "SERNO");
    const bool bad_serial = serial.empty() || serial.find_first_not_of('0') == std::string::npos;
    const bool same_region = setting_value(text, "GAME") == region_settings(r).game &&
                             setting_value(text, "AREA") == region_settings(r).area;
    if (same_region && !bad_serial) return st;
    if (bad_serial) serial = new_serial;
    st = make_setting_txt_for(r, serial);
    const s32 w = nand.open(0, 0, path, kFsWrite);
    if (w < 0 || nand.write(w, st) != 0x100 || nand.close(w) != 0) { error = "cannot rewrite " + path; return std::nullopt; }
    changed = true;
    return st;
}

std::string decrypt_setting_txt(std::span<const u8> data) {
    const auto p = crypt_setting(data);
    const auto end = std::find(p.begin(), p.end(), u8{0});
    return std::string(p.begin(), end);
}

bool create_nand(NandFs& nand, const NandSetup& s, std::string& error) {
    const FsModes dir{3, 3, 1};
    for (const char* d : {"/sys", "/ticket", "/title", "/title/00000001", "/title/00000001/00000002",
                          "/title/00000001/00000002/data", "/title/00010000", "/title/00010001", "/title/00010004",
                          "/shared1", "/shared2", "/shared2/sys", "/tmp", "/import", "/meta"}) {
        FsMeta m;
        if (nand.metadata(0, 0, d, m) == 0) continue;
        if (nand.create(0, 0, d, 0, dir, false) != 0) { error = std::string("cannot create ") + d; return false; }
    }
    // the system menu (uid 0x1000, group 1) owns its files; games may read and write SYSCONF
    const auto sysconf = make_sysconf(s);
    if (!write_file(nand, "/shared2/sys/SYSCONF", 0x1000, 1, {3, 3, 3}, sysconf, error)) return false;
    const auto setting = make_setting_txt(s);
    return write_file(nand, "/title/00000001/00000002/data/setting.txt", 0x1000, 1, {3, 3, 1}, setting, error);
}

} // namespace orca::ios
