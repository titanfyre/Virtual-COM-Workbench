#include "SelfTest.h"
#include "Cdc.h"
#include <stdexcept>

namespace Serial {
unsigned SelfTest() {
    unsigned checks=0;
    const auto check=[&](bool pass){++checks;if(!pass)throw std::runtime_error("Offline CDC assertion failed at check "+std::to_string(checks));};
    const auto config=Cdc::Configuration("VCW-OFFLINE-A");
    check(config.delegate_controls&&config.input_depth==128);
    check(config.descriptors[0].data[4]==2&&config.descriptors[0].data[5]==2);
    check(config.descriptors[1].data.size()==67);
    check(static_cast<bool>(Vdevice::Validate_Device(config)));
    for(const auto& bad:{std::string{},std::string("bad\\id"),std::string(65,'A')}) {
        bool threw=false;try{Cdc::Configuration(bad);}catch(const std::invalid_argument&){threw=true;}check(threw);
    }
    Cdc::LineState line;
    check(Cdc::HandleControl(line,{0xA1,0x21,0,0,0,0,7,0},{}).data==Cdc::Bytes({0,0xC2,1,0,0,0,8}));
    check(!Cdc::HandleControl(line,{0x21,0x20,0,0,0,0,7,0},{0x80,0x25,0,0,0,0,8}).stall);
    check(line.coding[0]==0x80&&line.coding[1]==0x25);
    check(Cdc::HandleControl(line,{0x21,0x20,0,0,0,0,7,0},{0,0,0,0,0,0,8}).stall);
    check(Cdc::HandleControl(line,{0x21,0x20,0,0,0,0,7,0},{1,0,0,0,0,0,9}).stall);
    check(Cdc::HandleControl(line,{0x21,0x20,0,0,0,0,7,0},{1,0,0,0,3,0,8}).stall);
    check(Cdc::HandleControl(line,{0x21,0x20,0,0,0,0,7,0},{1,0,0,0,0,5,8}).stall);
    check(Cdc::HandleControl(line,{0xA1,0x21,0,0,1,0,7,0},{}).stall);
    check(Cdc::HandleControl(line,{0xA1,0x21,0,0,0,0,6,0},{}).stall);
    check(Cdc::HandleControl(line,{0x21,0x22,4,0,0,0,0,0},{}).stall);
    check(!Cdc::HandleControl(line,{0x21,0x22,3,0,0,0,0,0},{}).stall&&line.control==3);
    check(Cdc::SerialNotification(line)==Cdc::Bytes({0xA1,0x20,0,0,0,0,2,0,3,0}));
    check(!Cdc::HandleControl(line,{0x21,0x23,0xFF,0xFF,0,0,0,0},{}).stall&&line.break_ms==65535);
    for(unsigned request=0;request<256;++request)if(request!=0x20&&request!=0x21&&request!=0x22&&request!=0x23)
        check(Cdc::HandleControl(line,{0x21,static_cast<std::uint8_t>(request),0,0,0,0,0,0},{}).stall);
    Cdc::ByteQueue queue;
    check(queue.Push(Cdc::Bytes(Cdc::QueueLimit,0x55))&&queue.Size()==Cdc::QueueLimit);
    check(!queue.Push({1})&&queue.Size()==Cdc::QueueLimit);
    std::size_t total=0;while(const auto* front=queue.Front()){check(front->size()<=64);total+=front->size();queue.Pop();}
    check(total==Cdc::QueueLimit&&queue.Size()==0);
    queue.Push({1,2,3});queue.Clear();check(queue.Front()==nullptr&&queue.Size()==0);
    for(unsigned round=0;round<10000;++round) {
        Cdc::Bytes payload(1+round%4096);for(std::size_t i=0;i<payload.size();++i)payload[i]=static_cast<std::uint8_t>((i+round)&255);
        check(queue.Push(payload));Cdc::Bytes restored;
        while(const auto* front=queue.Front()){restored.insert(restored.end(),front->begin(),front->end());queue.Pop();}
        check(restored==payload&&queue.Size()==0);
    }
    check(Cdc::Hex({0,10,255})=="00 0A FF");check(Cdc::Ascii({0,10,65,255})=="..A.");
    return checks;
}
}
