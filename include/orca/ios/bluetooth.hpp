// Orca Core — IOS /dev/usb/oh1/57e/305: the Wii's internal Bluetooth module
// (a USB HCI controller) with up to four emulated Wii Remotes.
//
// Public documentation used: Bluetooth Core Specification 1.1/2.0 (HCI
// commands and events, ACL packets, L2CAP signalling), the Bluetooth HID
// profile (transaction headers), USB (setup packets) and WiiBrew
// ("/dev/usb/oh1", "Wiimote", "SYSCONF"); Dolphin's BTEmu and WiimoteDevice as
// information for what the Wii's Bluetooth stack expects.
//
// USB requests (ioctlv): 0 control transfer = HCI command (in: request type,
// request, value, index, length (little-endian, as in a USB setup packet),
// unknown; io: the command); 1 bulk transfer on endpoint 0x02 (ACL data
// from the console) or 0x82 (a buffer for ACL data to the console);
// 2 interrupt transfer on endpoint 0x81 (a buffer for an HCI event). Buffers
// for the console stay pending until there is something to send; events go
// before ACL data.
//
// The controller: HCI version 1.1, 10 ACL buffers of 339 bytes, no SCO; its
// address and the remotes' addresses come from the configuration (the
// console's paired remotes, SYSCONF BT.DINF). It holds a link key for every
// emulated remote (reported by Read Stored Link Key) and authenticates them
// directly. A remote connects when the frontend asks (System::connect_remote,
// as pressing a button), if the host enabled page scan; it then opens the HID
// control (PSM 0x11) and interrupt (PSM 0x13) channels itself. Its reports
// travel on the interrupt channel at 200 Hz (the rate Dolphin uses for
// emulated remotes).
//
// Each Number Of Completed Packets event follows the ACL packet it counts.
// Inquiry finds no device (pairing new remotes is not emulated). Stops with
// a reason: unknown HCI commands, reject connection, fragmented ACL
// packets, SDP and other L2CAP channels, unknown L2CAP signals, HID
// transactions other than SET_REPORT(output) and DATA(output), and any Wii
// Remote error (see wiimote.hpp).
#pragma once
#include "orca/ios/kernel.hpp"
#include "orca/wiimote/wiimote.hpp"

#include <array>
#include <deque>
#include <map>
#include <optional>
#include <vector>

namespace orca::ios {

using BdAddr = std::array<u8, 6>; // as sent over HCI (least significant byte first)

class BluetoothDevice final : public Device {
public:
    static constexpr std::size_t kMaxRemotes = 4;
    static constexpr u64 kUpdateTicks = 60750000ULL / 200U; // 200 Hz in timebase ticks

    BluetoothDevice(BdAddr controller, std::vector<BdAddr> remotes);

    Result open(Kernel& k, const Request& r, const std::string& path, u32 mode) override;
    Result close(Kernel& k, const Request& r) override;
    Result ioctlv(Kernel& k, const Request& r, u32 number, std::span<const IoVector> in,
                  std::span<const IoVector> io) override;

    std::size_t remote_count() const noexcept { return remotes_.size(); }
    // Frontend: the remote asks to connect (false if it cannot now: no page
    // scan, already connected or connecting).
    bool connect(std::size_t index);
    void set_input(std::size_t index, const wiimote::InputState& in) { remotes_.at(index).input = in; }
    wiimote::Wiimote& remote(std::size_t index) { return remotes_.at(index).hid; }
    bool remote_connected(std::size_t index) const { return remotes_.at(index).state == Link::Connected; }
    bool remote_ready(std::size_t index) const; // both HID channels configured

    // One 200 Hz step (also scheduled by itself while the device is open).
    void tick();

private:
    enum class Link : u8 { Idle, Requested, Connected };
    struct Channel {
        u16 psm{0};
        u16 remote_cid{0};      // the console's channel ID (0 until connected)
        bool our_config_done{false}, their_config_done{false};
        bool complete() const { return remote_cid != 0 && our_config_done && their_config_done; }
    };
    struct Remote {
        BdAddr bd{};
        Link state{Link::Idle};
        wiimote::Wiimote hid;
        wiimote::InputState input;
        std::map<u16, Channel> channels; // by our channel ID
        u8 next_ident{1};
        bool linking{false};
    };
    struct Pending { u32 request; u32 buffer; u32 size; };

    void event(u8 code, std::vector<u8> params);
    void command_complete(u16 opcode, std::vector<u8> ret);
    void command_status(u16 opcode, u8 status = 0);
    bool hci_command(std::span<const u8> cmd);
    bool acl_from_host(std::span<const u8> packet);
    void acl_to_host(std::size_t remote, std::span<const u8> l2cap);
    void l2cap_to_host(std::size_t remote, u16 cid, std::span<const u8> payload);
    void signal(std::size_t remote, u8 code, std::vector<u8> data, std::optional<u8> ident = std::nullopt);
    bool signalling(std::size_t remote, std::span<const u8> data);
    bool channel_data(std::size_t remote, Channel& ch, std::span<const u8> data);
    void open_channel(std::size_t remote, u16 psm);
    void link_channels(std::size_t remote);
    void disconnect_remote(std::size_t remote);
    void deliver();
    void schedule();
    std::optional<std::size_t> by_handle(u16 handle) const;
    std::optional<std::size_t> by_address(const BdAddr& bd) const;
    static u16 handle_of(std::size_t remote) { return static_cast<u16>(0x100U + remote); }
    bool stop(std::string why);

    Kernel* kernel_{nullptr};
    BdAddr controller_;
    std::vector<Remote> remotes_;
    u8 scan_enable_{0};
    std::deque<std::vector<u8>> events_, acl_;
    std::deque<Pending> event_buffers_, acl_buffers_;
    bool open_{false};
    u64 generation_{0}; // invalidates the scheduled tick after close
};

// BT.DINF in SYSCONF: the addresses of the paired remotes, in the order the
// console lists them (each stored most significant byte first; returned in
// HCI order). Empty on a malformed or missing entry (error says why).
std::vector<BdAddr> paired_remotes_from_sysconf(std::span<const u8> sysconf, std::string& error);

} // namespace orca::ios
