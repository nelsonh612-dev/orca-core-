// Orca Core — IOS /dev/net/kd/request, the WiiConnect24 daemon (KD), on a console
// without network: Orca has no network stack, so the daemon's scheduler never runs
// and nothing is ever downloaded or sent. Requests (WiiBrew "/dev/net/kd/request";
// Dolphin's NetKDRequestDevice studied as information) answered from that state:
//   0x01 SUSPEND_SCHEDULER, 0x03 RESUME_SCHEDULER: output word 0 (no error); the
//        suspend depth is counted (resume below zero stops)
//   0x02 EXEC_TRY_SUSPEND_SCHEDULER: reply 0
//   0x1E GET_SCHEDULER_STAT: zeros (no scheduler errors recorded)
//   0x28 REQUEST_SHUTDOWN: reply 0 (no sockets to close)
// Every other request (sockets, mail, downloads, user IDs, time triggers) needs
// the network or the WiiConnect24 configuration and stops explicitly naming it.
#pragma once
#include "orca/ios/kernel.hpp"

namespace orca::ios {

class NetKdRequest final : public Device {
public:
    NetKdRequest() : Device("/dev/net/kd/request") {}
    Result open(Kernel& k, const Request& r, const std::string& path, u32 mode) override;
    Result close(Kernel& k, const Request& r) override;
    Result ioctl(Kernel& k, const Request& r, u32 number, IoVector in, IoVector out) override;
    int suspend_depth() const noexcept { return suspended_; }

private:
    int suspended_{0};
};

} // namespace orca::ios
