#pragma once
#include "Vdevice.h"
#include <array>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace Cdc {
using Bytes = std::vector<std::uint8_t>;
// Local test identity only: this project does not claim a USB-IF allocated VID.
constexpr std::uint16_t Vid = 0xFFFF, Pid = 0xCDC1;
constexpr std::size_t QueueLimit = 65536, PacketLimit = 64;
struct LineState {
    std::array<std::uint8_t, 7> coding{0x00,0xC2,0x01,0x00,0,0,8};
    std::uint16_t control = 0, break_ms = 0;
};
struct ControlReply { bool stall = true, notification = false; Bytes data; };
Vdevice::Device_Config Configuration(const std::string& serial);
ControlReply HandleControl(LineState&, const std::array<std::uint8_t, 8>& setup, const Bytes& payload);
Bytes SerialNotification(const LineState&);
std::string Hex(const Bytes&);
std::string Ascii(const Bytes&);
// A bounded FIFO. Push is all-or-nothing; no silent drop or unbounded backlog.
class ByteQueue {
    std::deque<Bytes> packets_;
    std::size_t bytes_ = 0;
public:
    bool Push(const Bytes&);
    const Bytes* Front() const noexcept;
    void Pop();
    void Clear() noexcept;
    std::size_t Size() const noexcept { return bytes_; }
};
}
