#include "orca/ios/bluetooth.hpp"

#include <algorithm>
#include <cstdio>

namespace orca::ios {

namespace {
constexpr u8 kEpHciEvent = 0x81, kEpAclIn = 0x82, kEpAclOut = 0x02;
constexpr u16 kAclSize = 339, kAclCount = 10;
constexpr u16 kPsmSdp = 0x01, kPsmHidControl = 0x11, kPsmHidInterrupt = 0x13;
constexpr u16 kCidSignal = 0x0001;
constexpr u16 kRemoteMtu = 185; // the MTU the remote asks for (Dolphin's value)
// Wii Remote identity (WiiBrew; LMP values and features as measured by Dolphin)
constexpr std::array<u8, 3> kRemoteClass{0x04, 0x25, 0x00}; // 0x002504: peripheral, joystick
constexpr std::array<u8, 8> kRemoteFeatures{0xBC, 0x02, 0x04, 0x38, 0x08, 0x00, 0x00, 0x00};
constexpr char kRemoteName[] = "Nintendo RVL-CNT-01";

std::string hex4(u32 v) {
    char b[8];
    std::snprintf(b, sizeof b, "%04X", v);
    return b;
}
void put16(std::vector<u8>& v, u16 x) { v.push_back(static_cast<u8>(x)); v.push_back(static_cast<u8>(x >> 8)); }
u16 le16(const u8* p) { return static_cast<u16>(p[0] | p[1] << 8); }
} // namespace

std::vector<BdAddr> paired_remotes_from_sysconf(std::span<const u8> f, std::string& error) {
    std::vector<BdAddr> out;
    if (f.size() < 8 || f[0] != 'S' || f[1] != 'C' || f[2] != 'v' || f[3] != '0') { error = "SYSCONF: no SCv0 header"; return out; }
    const u16 count = static_cast<u16>(f[4] << 8 | f[5]);
    if (6U + 2U * count > f.size()) { error = "SYSCONF: truncated offset table"; return out; }
    for (std::size_t i = 0; i < count; ++i) {
        const std::size_t off = static_cast<std::size_t>(f[6 + 2 * i]) << 8 | f[7 + 2 * i];
        if (off >= f.size()) { error = "SYSCONF: entry outside the file"; return out; }
        const u8 desc = f[off];
        const std::size_t name_len = (desc & 0x1FU) + 1U;
        if (off + 1 + name_len + 2 > f.size()) { error = "SYSCONF: truncated entry"; return out; }
        const std::string name(reinterpret_cast<const char*>(&f[off + 1]), name_len);
        if (name != "BT.DINF") continue;
        if ((desc >> 5) != 1U) { error = "SYSCONF: BT.DINF is not a big array"; return out; }
        const std::size_t len = (static_cast<std::size_t>(f[off + 1 + name_len] << 8 | f[off + 2 + name_len])) + 1U;
        const std::size_t data = off + 3 + name_len;
        if (data + len > f.size() || len < 1 + 16 * 70) { error = "SYSCONF: BT.DINF too short"; return out; }
        // registered[10] then active[6]: {address (most significant byte first), name[64]}
        const std::size_t active = data + 1 + 10 * 70;
        for (std::size_t slot = 0; slot < 4; ++slot) {
            const u8* a = &f[active + slot * 70];
            if (std::all_of(a, a + 6, [](u8 b) { return b == 0; })) continue;
            BdAddr bd{};
            std::reverse_copy(a, a + 6, bd.begin());
            out.push_back(bd);
        }
        if (out.empty()) error = "SYSCONF: BT.DINF lists no active remote";
        return out;
    }
    error = "SYSCONF: no BT.DINF entry";
    return out;
}

BluetoothDevice::BluetoothDevice(BdAddr controller, std::vector<BdAddr> remotes)
    : Device("/dev/usb/oh1/57e/305"), controller_(controller) {
    for (std::size_t i = 0; i < std::min(remotes.size(), kMaxRemotes); ++i) {
        remotes_.emplace_back();
        remotes_.back().bd = remotes[i];
    }
}

bool BluetoothDevice::stop(std::string why) {
    if (kernel_ != nullptr) kernel_->stop("IOS /dev/usb/oh1/57e/305: " + why);
    return false;
}

Result BluetoothDevice::open(Kernel& k, const Request&, const std::string&, u32) {
    kernel_ = &k;
    open_ = true;
    schedule();
    return Result::reply(err::kOk, kDefaultReplyTicks);
}

Result BluetoothDevice::close(Kernel&, const Request&) {
    open_ = false;
    ++generation_;
    scan_enable_ = 0;
    events_.clear();
    acl_.clear();
    for (const auto& p : event_buffers_) kernel_->reply(p.request, err::kOk, 0);
    for (const auto& p : acl_buffers_) kernel_->reply(p.request, err::kOk, 0);
    event_buffers_.clear();
    acl_buffers_.clear();
    for (std::size_t i = 0; i < remotes_.size(); ++i) disconnect_remote(i);
    events_.clear();
    return Result::reply(err::kOk, kDefaultReplyTicks);
}

void BluetoothDevice::schedule() {
    const u64 gen = generation_;
    kernel_->scheduler().schedule_in(kernel_->ticks_to_cycles(kUpdateTicks), "Bluetooth 200 Hz", [this, gen](u64, u64) {
        if (gen != generation_ || !open_) return;
        tick();
        schedule();
    });
}

std::optional<std::size_t> BluetoothDevice::by_handle(u16 handle) const {
    for (std::size_t i = 0; i < remotes_.size(); ++i)
        if (handle_of(i) == handle && remotes_[i].state == Link::Connected) return i;
    return std::nullopt;
}
std::optional<std::size_t> BluetoothDevice::by_address(const BdAddr& bd) const {
    for (std::size_t i = 0; i < remotes_.size(); ++i)
        if (remotes_[i].bd == bd) return i;
    return std::nullopt;
}

bool BluetoothDevice::remote_ready(std::size_t index) const {
    const Remote& r = remotes_.at(index);
    if (r.state != Link::Connected) return false;
    bool ctl = false, intr = false;
    for (const auto& [cid, ch] : r.channels) {
        if (ch.psm == kPsmHidControl && ch.complete()) ctl = true;
        if (ch.psm == kPsmHidInterrupt && ch.complete()) intr = true;
    }
    return ctl && intr;
}

void BluetoothDevice::event(u8 code, std::vector<u8> params) {
    std::vector<u8> e{code, static_cast<u8>(params.size())};
    e.insert(e.end(), params.begin(), params.end());
    events_.push_back(std::move(e));
}
void BluetoothDevice::command_complete(u16 opcode, std::vector<u8> ret) {
    std::vector<u8> p{1};
    put16(p, opcode);
    p.insert(p.end(), ret.begin(), ret.end());
    event(0x0E, std::move(p));
}
void BluetoothDevice::command_status(u16 opcode, u8 status) {
    std::vector<u8> p{status, 1};
    put16(p, opcode);
    event(0x0F, std::move(p));
}

bool BluetoothDevice::connect(std::size_t index) {
    if (index >= remotes_.size() || !open_) return false;
    Remote& r = remotes_[index];
    if (r.state != Link::Idle || (scan_enable_ & 2U) == 0U) return false;
    r.state = Link::Requested;
    std::vector<u8> p(r.bd.begin(), r.bd.end());
    p.insert(p.end(), kRemoteClass.begin(), kRemoteClass.end());
    p.push_back(1); // ACL link
    event(0x04, std::move(p));
    deliver();
    return true;
}

void BluetoothDevice::disconnect_remote(std::size_t i) {
    Remote& r = remotes_[i];
    r.state = Link::Idle;
    r.channels.clear();
    r.linking = false;
    r.hid.reset();
}

bool BluetoothDevice::hci_command(std::span<const u8> c) {
    if (c.size() < 3U || c.size() < 3U + c[2]) return stop("HCI command shorter than its header");
    const u16 op = le16(c.data());
    const u8* p = c.data() + 3;
    const u8 plen = c[2];
    const auto need = [&](u8 n) { return plen >= n ? true : stop("HCI command 0x" + hex4(op) + " too short"); };
    const auto status_only = [&] { command_complete(op, {0}); return true; };
    const auto addr = [&](const u8* q) { BdAddr b{}; std::copy_n(q, 6, b.begin()); return b; };
    switch (op) {
    case 0x0C03: // reset
        scan_enable_ = 0;
        for (std::size_t i = 0; i < remotes_.size(); ++i) disconnect_remote(i);
        return status_only();
    case 0x0C05: case 0x0C0A: case 0x0C13: case 0x0C18: case 0x0C24: case 0x0C33: case 0x0C43: case 0x0C45: case 0x0C47:
    case 0xFC4C: case 0xFC4F: // set event filter, PIN type, local name, page timeout, class, host buffer, scan types, inquiry mode; Broadcom vendor commands
        return status_only();
    case 0x0C1A: // write scan enable
        if (!need(1)) return false;
        scan_enable_ = p[0];
        return status_only();
    case 0x0C37: { // write link supervision timeout: status, handle
        if (!need(4)) return false;
        std::vector<u8> ret{0};
        put16(ret, le16(p));
        command_complete(op, std::move(ret));
        return true;
    }
    case 0x0C0D: { // read stored link key: return link keys, then the counts
        if (!need(7)) return false;
        const bool all = p[6] != 0U;
        std::vector<u8> keys{0};
        u8 n = 0;
        for (std::size_t i = 0; i < remotes_.size(); ++i) {
            if (!all && remotes_[i].bd != addr(p)) continue;
            keys.insert(keys.end(), remotes_[i].bd.begin(), remotes_[i].bd.end());
            for (int b = 0; b < 16; ++b) keys.push_back(static_cast<u8>(0xA0U + i));
            ++n;
        }
        keys[0] = n;
        if (n != 0U) event(0x15, std::move(keys));
        std::vector<u8> ret{0};
        put16(ret, 255);
        put16(ret, n);
        command_complete(op, std::move(ret));
        return true;
    }
    case 0x0C12: { // delete stored link key: none deleted (the keys belong to the emulated remotes)
        std::vector<u8> ret{0};
        put16(ret, 0);
        command_complete(op, std::move(ret));
        return true;
    }
    case 0x1001: { // local version: HCI 1.1
        std::vector<u8> ret{0, 0x03};
        put16(ret, 0x0000);
        ret.push_back(0x03);
        put16(ret, 0x000F); // Broadcom
        put16(ret, 0x0000);
        command_complete(op, std::move(ret));
        return true;
    }
    case 0x1003: command_complete(op, {0, 0xFF, 0xFF, 0x8D, 0xFE, 0x9B, 0xF9, 0x00, 0x80}); return true;
    case 0x1005: {
        std::vector<u8> ret{0};
        put16(ret, kAclSize);
        ret.push_back(64);
        put16(ret, kAclCount);
        put16(ret, 0);
        command_complete(op, std::move(ret));
        return true;
    }
    case 0x1009: {
        std::vector<u8> ret{0};
        ret.insert(ret.end(), controller_.begin(), controller_.end());
        command_complete(op, std::move(ret));
        return true;
    }
    case 0x0401: // inquiry: nothing found
        command_status(op);
        event(0x01, {0});
        return true;
    case 0x0402: return status_only(); // inquiry cancel
    case 0x0405: { // create connection
        if (!need(13)) return false;
        command_status(op);
        const BdAddr bd = addr(p);
        const auto i = by_address(bd);
        std::vector<u8> e{0x04};
        put16(e, i ? handle_of(*i) : 0);
        e.insert(e.end(), bd.begin(), bd.end());
        e.push_back(1);
        e.push_back(0);
        if (i && remotes_[*i].state == Link::Idle) {
            e[0] = 0;
            remotes_[*i].state = Link::Connected;
            remotes_[*i].hid.reset();
        }
        event(0x03, std::move(e)); // page timeout (0x04) when no such remote answers
        return true;
    }
    case 0x0409: { // accept connection
        if (!need(7)) return false;
        command_status(op);
        const BdAddr bd = addr(p);
        const auto i = by_address(bd);
        const bool ok = i && remotes_[*i].state == Link::Requested;
        if (ok && p[6] == 0U) { // become master: role change
            std::vector<u8> rc{0};
            rc.insert(rc.end(), bd.begin(), bd.end());
            rc.push_back(0);
            event(0x12, std::move(rc));
        }
        std::vector<u8> e{static_cast<u8>(ok ? 0 : 0x08)};
        put16(e, i ? handle_of(*i) : 0);
        e.insert(e.end(), bd.begin(), bd.end());
        e.push_back(1);
        e.push_back(0);
        event(0x03, std::move(e));
        if (ok) {
            remotes_[*i].state = Link::Connected;
            remotes_[*i].hid.reset();
            remotes_[*i].linking = true; // the remote opens its HID channels
        }
        return true;
    }
    case 0x040A: return stop("HCI reject connection is not emulated");
    case 0x040B: case 0x040C: { // link key (negative) reply: status, address
        if (!need(6)) return false;
        std::vector<u8> ret{0};
        ret.insert(ret.end(), p, p + 6);
        command_complete(op, std::move(ret));
        return true;
    }
    case 0x0406: { // disconnect
        if (!need(3)) return false;
        command_status(op);
        const u16 h = le16(p);
        const auto i = by_handle(h);
        std::vector<u8> e{static_cast<u8>(i ? 0 : 0x02)};
        put16(e, h);
        e.push_back(0x16); // terminated by the local host
        event(0x05, std::move(e));
        if (i) disconnect_remote(*i);
        return true;
    }
    case 0x040F: case 0x0411: case 0x041B: case 0x041D: case 0x041F: case 0x0803: {
        if (!need(2)) return false;
        command_status(op);
        const u16 h = le16(p);
        const u8 st = by_handle(h) ? 0 : 0x02;
        std::vector<u8> e{st};
        put16(e, h);
        switch (op) {
        case 0x040F: if (!need(4)) return false; put16(e, le16(p + 2)); event(0x1D, std::move(e)); break;
        case 0x0411: event(0x06, std::move(e)); break;
        case 0x041B: e.insert(e.end(), kRemoteFeatures.begin(), kRemoteFeatures.end()); event(0x0B, std::move(e)); break;
        case 0x041D: e.push_back(0x02); put16(e, 0x000F); put16(e, 0x0229); event(0x0C, std::move(e)); break;
        case 0x041F: put16(e, 0); event(0x1C, std::move(e)); break;
        default: // sniff mode: the interval is the maximum asked for
            if (!need(4)) return false;
            e.push_back(0x02);
            put16(e, le16(p + 2));
            event(0x14, std::move(e));
            break;
        }
        return true;
    }
    case 0x0419: { // remote name request
        if (!need(6)) return false;
        command_status(op);
        const BdAddr bd = addr(p);
        std::vector<u8> e{static_cast<u8>(by_address(bd) ? 0 : 0x04)};
        e.insert(e.end(), bd.begin(), bd.end());
        std::vector<u8> name(248, 0);
        std::copy(kRemoteName, kRemoteName + sizeof kRemoteName - 1, name.begin());
        e.insert(e.end(), name.begin(), name.end());
        event(0x07, std::move(e));
        return true;
    }
    case 0x080D: { // write link policy: status, handle
        if (!need(4)) return false;
        std::vector<u8> ret{0};
        put16(ret, le16(p));
        command_complete(op, std::move(ret));
        return true;
    }
    default:
        return stop("HCI command 0x" + hex4(op) + " is not emulated");
    }
}

void BluetoothDevice::acl_to_host(std::size_t remote, std::span<const u8> l2cap) {
    std::vector<u8> pkt;
    put16(pkt, static_cast<u16>(handle_of(remote) | 0x2000U)); // first packet, point to point
    put16(pkt, static_cast<u16>(l2cap.size()));
    pkt.insert(pkt.end(), l2cap.begin(), l2cap.end());
    acl_.push_back(std::move(pkt));
}

void BluetoothDevice::l2cap_to_host(std::size_t remote, u16 cid, std::span<const u8> payload) {
    std::vector<u8> l;
    put16(l, static_cast<u16>(payload.size()));
    put16(l, cid);
    l.insert(l.end(), payload.begin(), payload.end());
    acl_to_host(remote, l);
}

void BluetoothDevice::signal(std::size_t remote, u8 code, std::vector<u8> data, std::optional<u8> ident) {
    Remote& r = remotes_[remote];
    std::vector<u8> s{code, ident ? *ident : r.next_ident++};
    if (!ident && r.next_ident == 0U) r.next_ident = 1;
    put16(s, static_cast<u16>(data.size()));
    s.insert(s.end(), data.begin(), data.end());
    l2cap_to_host(remote, kCidSignal, s);
}

void BluetoothDevice::open_channel(std::size_t remote, u16 psm) {
    Remote& r = remotes_[remote];
    u16 cid = 0x40;
    while (r.channels.count(cid) != 0U) ++cid;
    r.channels[cid].psm = psm;
    std::vector<u8> d;
    put16(d, psm);
    put16(d, cid);
    signal(remote, 0x02, std::move(d));
}

void BluetoothDevice::link_channels(std::size_t remote) {
    Remote& r = remotes_[remote];
    if (!r.linking) return;
    // the control channel first, then the interrupt channel
    for (u16 psm : {kPsmHidControl, kPsmHidInterrupt}) {
        const auto it = std::find_if(r.channels.begin(), r.channels.end(), [psm](const auto& c) { return c.second.psm == psm; });
        if (it == r.channels.end()) { open_channel(remote, psm); return; }
        if (!it->second.complete()) return;
    }
    r.linking = false;
}

bool BluetoothDevice::signalling(std::size_t remote, std::span<const u8> d) {
    Remote& r = remotes_[remote];
    while (d.size() >= 4U) {
        const u8 code = d[0], ident = d[1];
        const u16 len = le16(&d[2]);
        if (d.size() < 4U + len) return stop("L2CAP signal longer than its packet");
        const u8* p = &d[4];
        switch (code) {
        case 0x02: { // connection request from the console
            if (len < 4U) return stop("L2CAP connection request too short");
            const u16 psm = le16(p), scid = le16(p + 2);
            if (psm != kPsmHidControl && psm != kPsmHidInterrupt)
                return stop("L2CAP connection to PSM 0x" + hex4(psm) + (psm == kPsmSdp ? " (SDP)" : "") + " is not emulated");
            u16 cid = 0x40;
            while (r.channels.count(cid) != 0U) ++cid;
            Channel& ch = r.channels[cid];
            ch.psm = psm;
            ch.remote_cid = scid;
            std::vector<u8> rsp;
            put16(rsp, cid);
            put16(rsp, scid);
            put16(rsp, 0); // success
            put16(rsp, 0);
            signal(remote, 0x03, std::move(rsp), ident);
            std::vector<u8> cfg;
            put16(cfg, scid);
            put16(cfg, 0);
            cfg.insert(cfg.end(), {0x01, 0x02});
            put16(cfg, kRemoteMtu);
            signal(remote, 0x04, std::move(cfg));
            break;
        }
        case 0x03: { // connection response to our request
            if (len < 8U) return stop("L2CAP connection response too short");
            const u16 dcid = le16(p), scid = le16(p + 2), result = le16(p + 4);
            const auto it = r.channels.find(scid);
            if (it == r.channels.end()) return stop("L2CAP connection response for an unknown channel");
            if (result == 1U) break; // pending: a final response follows
            if (result != 0U) return stop("the console refused the L2CAP channel (result " + std::to_string(result) + ")");
            it->second.remote_cid = dcid;
            std::vector<u8> cfg;
            put16(cfg, dcid);
            put16(cfg, 0);
            cfg.insert(cfg.end(), {0x01, 0x02});
            put16(cfg, kRemoteMtu);
            signal(remote, 0x04, std::move(cfg));
            break;
        }
        case 0x04: { // configuration request: accept every option as given
            if (len < 4U) return stop("L2CAP configuration request too short");
            const u16 dcid = le16(p);
            const auto it = r.channels.find(dcid);
            if (it == r.channels.end()) return stop("L2CAP configuration of an unknown channel");
            if (le16(p + 2) != 0U) return stop("continued L2CAP configuration is not emulated");
            std::vector<u8> rsp;
            put16(rsp, it->second.remote_cid);
            put16(rsp, 0);
            put16(rsp, 0); // success
            rsp.insert(rsp.end(), p + 4, p + len);
            signal(remote, 0x05, std::move(rsp), ident);
            it->second.their_config_done = true;
            break;
        }
        case 0x05: { // configuration response
            if (len < 6U) return stop("L2CAP configuration response too short");
            const u16 scid = le16(p), result = le16(p + 4);
            const auto it = r.channels.find(scid);
            if (it == r.channels.end()) return stop("L2CAP configuration response for an unknown channel");
            if (result != 0U) return stop("the console rejected the L2CAP configuration (result " + std::to_string(result) + ")");
            it->second.our_config_done = true;
            break;
        }
        case 0x06: { // disconnection request
            if (len < 4U) return stop("L2CAP disconnection request too short");
            const u16 dcid = le16(p), scid = le16(p + 2);
            std::vector<u8> rsp;
            put16(rsp, dcid);
            put16(rsp, scid);
            signal(remote, 0x07, std::move(rsp), ident);
            r.channels.erase(dcid);
            break;
        }
        case 0x07: break; // disconnection response
        default:
            return stop("L2CAP signal 0x" + hex4(code).substr(2) + " is not emulated");
        }
        d = d.subspan(4U + len);
    }
    link_channels(remote);
    return true;
}

bool BluetoothDevice::channel_data(std::size_t remote, Channel& ch, std::span<const u8> d) {
    if (d.empty()) return stop("empty HID transaction");
    Remote& r = remotes_[remote];
    if (ch.psm == kPsmHidControl && d[0] == 0x52) { // SET_REPORT, output
        const u8 ok = 0x00;                          // handshake: successful
        l2cap_to_host(remote, ch.remote_cid, std::span<const u8>(&ok, 1));
    } else if (!(ch.psm == kPsmHidInterrupt && d[0] == 0xA2)) { // DATA, output
        return stop("HID transaction 0x" + hex4(d[0]).substr(2) + " on PSM 0x" + hex4(ch.psm) + " is not emulated");
    }
    if (!r.hid.output_report(d.subspan(1))) return stop(r.hid.error());
    return true;
}

bool BluetoothDevice::acl_from_host(std::span<const u8> pkt) {
    if (pkt.size() < 4U) return stop("ACL packet shorter than its header");
    const u16 hdr = le16(pkt.data());
    const u16 handle = hdr & 0x0FFFU;
    if ((hdr >> 12 & 3U) != 2U) return stop("fragmented ACL packets are not emulated");
    const u16 len = le16(&pkt[2]);
    if (pkt.size() < 4U + len || len < 4U) return stop("ACL packet length does not match");
    const auto i = by_handle(handle);
    if (!i) return stop("ACL data for an unknown connection 0x" + hex4(handle));
    // Number Of Completed Packets for this one
    std::vector<u8> nc{1};
    put16(nc, handle);
    put16(nc, 1);
    event(0x13, std::move(nc));
    const u8* l = &pkt[4];
    const u16 l2len = le16(l), cid = le16(l + 2);
    if (l2len + 4U != len) return stop("L2CAP length does not match its ACL packet");
    const std::span<const u8> payload(l + 4, l2len);
    if (cid == kCidSignal) return signalling(*i, payload);
    const auto it = remotes_[*i].channels.find(cid);
    if (it == remotes_[*i].channels.end()) return stop("data for unknown L2CAP channel 0x" + hex4(cid));
    return channel_data(*i, it->second, payload);
}

void BluetoothDevice::tick() {
    for (std::size_t i = 0; i < remotes_.size(); ++i) {
        Remote& r = remotes_[i];
        if (r.state != Link::Connected) continue;
        link_channels(i);
        if (!remote_ready(i)) continue;
        r.hid.update(r.input);
        if (r.hid.failed()) { stop(r.hid.error()); return; }
        u16 intr = 0;
        for (const auto& [cid, ch] : r.channels)
            if (ch.psm == kPsmHidInterrupt) intr = ch.remote_cid;
        while (auto rep = r.hid.pop_report()) {
            std::vector<u8> d{0xA1}; // DATA, input
            d.insert(d.end(), rep->begin(), rep->end());
            l2cap_to_host(i, intr, d);
        }
    }
    deliver();
}

void BluetoothDevice::deliver() {
    if (kernel_ == nullptr) return;
    while (!events_.empty() && !event_buffers_.empty()) {
        const Pending b = event_buffers_.front();
        event_buffers_.pop_front();
        std::vector<u8> e = std::move(events_.front());
        events_.pop_front();
        if (e.size() > b.size) { stop("HCI event larger than the console's buffer"); return; }
        if (!kernel_->write_bytes(b.buffer, e)) { stop("event buffer is not in RAM"); return; }
        kernel_->reply(b.request, static_cast<s32>(e.size()), 0);
    }
    while (events_.empty() && !acl_.empty() && !acl_buffers_.empty()) {
        const Pending b = acl_buffers_.front();
        acl_buffers_.pop_front();
        std::vector<u8> a = std::move(acl_.front());
        acl_.pop_front();
        if (a.size() > b.size) { stop("ACL packet larger than the console's buffer"); return; }
        if (!kernel_->write_bytes(b.buffer, a)) { stop("ACL buffer is not in RAM"); return; }
        kernel_->reply(b.request, static_cast<s32>(a.size()), 0);
    }
}

Result BluetoothDevice::ioctlv(Kernel& k, const Request& r, u32 number, std::span<const IoVector> in,
                               std::span<const IoVector> io) {
    kernel_ = &k;
    const auto ram = [] { return Result::stop("IOS /dev/usb/oh1/57e/305: request buffer is not in RAM"); };
    switch (number) {
    case 0: { // control transfer: HCI command
        if (in.size() < 5U || io.empty() || in[4].size < 2U) return Result::stop("IOS /dev/usb/oh1/57e/305: malformed control transfer");
        std::array<u8, 2> lb{};
        if (!k.read_bytes(in[4].address, lb)) return ram();
        const u16 length = le16(lb.data());
        std::vector<u8> cmd(std::min<u32>(length, io[0].size));
        if (!k.read_bytes(io[0].address, cmd)) return ram();
        if (!hci_command(cmd)) return Result::stop(k.stop_reason());
        deliver();
        return Result::reply(length, 0);
    }
    case 1:   // bulk
    case 2: { // interrupt
        if (in.size() < 2U || io.empty()) return Result::stop("IOS /dev/usb/oh1/57e/305: malformed transfer");
        std::array<u8, 1> ep{};
        std::array<u8, 2> lb{};
        if (!k.read_bytes(in[0].address, ep) || !k.read_bytes(in[1].address, lb)) return ram();
        const u32 length = static_cast<u32>(lb[0] << 8 | lb[1]);
        const Pending buf{r.address, io[0].address, std::min(length, io[0].size)};
        // A buffer is filled at once if something is waiting (and no older buffer
        // is); otherwise it stays pending until there is something to send.
        const auto fill_now = [&](std::deque<std::vector<u8>>& q) -> Result {
            std::vector<u8> d = std::move(q.front());
            q.pop_front();
            if (d.size() > buf.size) return Result::stop("IOS /dev/usb/oh1/57e/305: packet larger than the console's buffer");
            if (!k.write_bytes(buf.buffer, d)) return ram();
            return Result::reply(static_cast<s32>(d.size()), 0);
        };
        if (number == 2) {
            if (ep[0] != kEpHciEvent) return Result::stop("IOS /dev/usb/oh1/57e/305: interrupt endpoint 0x" + hex4(ep[0]).substr(2));
            if (!events_.empty() && event_buffers_.empty()) return fill_now(events_);
            event_buffers_.push_back(buf);
            return Result::deferred();
        }
        if (ep[0] == kEpAclIn) {
            if (events_.empty() && !acl_.empty() && acl_buffers_.empty()) return fill_now(acl_);
            acl_buffers_.push_back(buf);
            return Result::deferred();
        }
        if (ep[0] != kEpAclOut) return Result::stop("IOS /dev/usb/oh1/57e/305: bulk endpoint 0x" + hex4(ep[0]).substr(2));
        std::vector<u8> pkt(std::min(length, io[0].size));
        if (!k.read_bytes(io[0].address, pkt)) return ram();
        if (!acl_from_host(pkt)) return Result::stop(k.stop_reason());
        deliver();
        return Result::reply(static_cast<s32>(pkt.size()), 0);
    }
    default:
        return Device::ioctlv(k, r, number, in, io);
    }
}

} // namespace orca::ios
