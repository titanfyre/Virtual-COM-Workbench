#include "Cdc.h"
#include <algorithm>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace Cdc {
namespace {
Vdevice::Descriptor String(unsigned index, const std::string& text) {
    Vdevice::Descriptor d; d.index=index; d.language=0x409;
    d.data={static_cast<std::uint8_t>(2+text.size()*2),3};
    for (unsigned char c:text) { d.data.push_back(c); d.data.push_back(0); }
    return d;
}
std::uint16_t U16(const std::array<std::uint8_t,8>& q, unsigned p) {
    return static_cast<std::uint16_t>(q[p] | (std::uint16_t(q[p+1])<<8));
}
}
Vdevice::Device_Config Configuration(const std::string& serial) {
    if(serial.empty() || serial.size()>64) throw std::invalid_argument("Serial must contain 1..64 ASCII letters, digits or hyphens.");
    for (char c:serial) if(!((c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='-'))
        throw std::invalid_argument("Invalid USB serial character.");
    Vdevice::Device_Config c; c.delegate_controls=true; c.input_depth=128;
    Vdevice::Descriptor device; device.data={18,1,0,2,2,2,1,64,0xFF,0xFF,0xC1,0xCD,0,1,1,2,3,1};
    Vdevice::Descriptor config;
    config.data={
        9,2,67,0,2,1,0,0x80,50,
        9,4,0,0,1,2,2,1,0, // Communication interface.
        5,0x24,0,0x20,1,   // CDC header 1.20.
        5,0x24,1,0,1,      // Call management; data interface 1.
        4,0x24,2,6,        // ACM: line coding/control and SEND_BREAK.
        5,0x24,6,0,1,      // Union: master 0, slave 1.
        7,5,0x81,3,16,0,16,
        9,4,1,0,2,0x0A,0,0,0,
        7,5,0x02,2,64,0,0,
        7,5,0x82,2,64,0,0
    };
    Vdevice::Descriptor languages; languages.data={4,3,9,4};
    c.descriptors={device,config,languages,String(1,"Titanfyre"),String(2,"Virtual COM Workbench"),String(3,serial)};
    return c;
}
ControlReply HandleControl(LineState& state,const std::array<std::uint8_t,8>& q,const Bytes& payload) {
    ControlReply reply;
    if(U16(q,4)!=0) return reply;
    const auto value=U16(q,2), length=U16(q,6);
    if(q[0]==0xA1 && q[1]==0x21 && !value && length==7 && payload.empty()) {
        reply.data.assign(state.coding.begin(),state.coding.end()); reply.stall=false;
    } else if(q[0]==0x21 && q[1]==0x20 && !value && length==7 && payload.size()==7) {
        const bool baud=payload[0]||payload[1]||payload[2]||payload[3];
        const auto bits=payload[6];
        if(!baud || payload[4]>2 || payload[5]>4 || !((bits>=5&&bits<=8)||bits==16)) return reply;
        std::copy(payload.begin(),payload.end(),state.coding.begin()); reply.stall=false;
    } else if(q[0]==0x21 && q[1]==0x22 && value<=3 && !length && payload.empty()) {
        state.control=value; reply.stall=false; reply.notification=true;
    } else if(q[0]==0x21 && q[1]==0x23 && !length && payload.empty()) {
        state.break_ms=value; reply.stall=false;
    }
    return reply;
}
Bytes SerialNotification(const LineState& state) {
    // In this software cable DSR/DCD follow DTR; no physical RS-232 lines exist.
    return {0xA1,0x20,0,0,0,0,2,0,static_cast<std::uint8_t>((state.control&1)?3:0),0};
}
std::string Hex(const Bytes& bytes) {
    std::ostringstream out; out<<std::hex<<std::uppercase<<std::setfill('0');
    for(std::size_t i=0;i<bytes.size();++i) { if(i) out<<' '; out<<std::setw(2)<<unsigned(bytes[i]); }
    return out.str();
}
std::string Ascii(const Bytes& bytes) {
    std::string out; out.reserve(bytes.size());
    for(auto b:bytes) out.push_back(b>=32&&b<127?static_cast<char>(b):'.');
    return out;
}
bool ByteQueue::Push(const Bytes& bytes) {
    if(bytes.size()>QueueLimit-bytes_) return false;
    for(std::size_t p=0;p<bytes.size();p+=PacketLimit) {
        const auto n=(std::min)(PacketLimit,bytes.size()-p);
        packets_.emplace_back(bytes.begin()+p,bytes.begin()+p+n);
    }
    bytes_+=bytes.size(); return true;
}
const Bytes* ByteQueue::Front() const noexcept { return packets_.empty()?nullptr:&packets_.front(); }
void ByteQueue::Pop() { if(!packets_.empty()) { bytes_-=packets_.front().size(); packets_.pop_front(); } }
void ByteQueue::Clear() noexcept { packets_.clear(); bytes_=0; }
}
