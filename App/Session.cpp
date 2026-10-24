#include "Session.h"
#include <windows.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <devguid.h>
#include <objbase.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <type_traits>

namespace Serial {
namespace {
using Clock=std::chrono::steady_clock;
std::string Error(const char* stage,const Vdevice::Result& result) {
    return std::string(stage)+": "+result.message+" (Windows "+std::to_string(result.error)+").";
}
template<class T,class=void>struct Trial{static bool Expired(const T&){return false;}};
template<class T>struct Trial<T,std::void_t<decltype(std::declval<const T&>().trial_expired)>> {
    static bool Expired(const T& value){return value.trial_expired;}
};
std::wstring Upper(std::wstring text) { for(auto& c:text) c=static_cast<wchar_t>(towupper(c)); return text; }
std::wstring Instance(DEVINST dev) {
    wchar_t value[MAX_DEVICE_ID_LEN]{};
    return CM_Get_Device_IDW(dev,value,MAX_DEVICE_ID_LEN,0)==CR_SUCCESS?value:L"";
}
bool Owns(DEVINST dev,const std::string& serial) {
    const auto wanted=Upper(std::wstring(serial.begin(),serial.end()));
    for(unsigned depth=0;depth<12;++depth) {
        const auto id=Upper(Instance(dev)); const auto slash=id.find_last_of(L'\\');
        if(id.rfind(L"USB\\VID_FFFF&PID_CDC1",0)==0 && slash!=std::wstring::npos && id.substr(slash+1)==wanted) return true;
        DEVINST parent=0; if(CM_Get_Parent(&parent,dev,0)!=CR_SUCCESS) break; dev=parent;
    }
    return false;
}
std::array<std::wstring,2> Ports(const std::array<std::string,2>& serials) {
    std::array<std::wstring,2> result;
    const auto list=SetupDiGetClassDevsW(&GUID_DEVCLASS_PORTS,nullptr,nullptr,DIGCF_PRESENT);
    if(list==INVALID_HANDLE_VALUE) return result;
    struct Guard { HDEVINFO h; ~Guard(){SetupDiDestroyDeviceInfoList(h);} } guard{list};
    SP_DEVINFO_DATA info{}; info.cbSize=sizeof(info);
    for(DWORD n=0;n<1024 && SetupDiEnumDeviceInfo(list,n,&info);++n) {
        ULONG state=0,problem=0;
        if(CM_Get_DevNode_Status(&state,&problem,info.DevInst,0)!=CR_SUCCESS || !(state&DN_STARTED) || problem) continue;
        for(unsigned side=0;side<2;++side) if(Owns(info.DevInst,serials[side])) {
            const auto key=SetupDiOpenDevRegKey(list,&info,DICS_FLAG_GLOBAL,0,DIREG_DEV,KEY_READ);
            if(key==INVALID_HANDLE_VALUE) continue;
            wchar_t name[64]{}; DWORD kind=0,size=sizeof(name);
            const auto status=RegQueryValueExW(key,L"PortName",nullptr,&kind,reinterpret_cast<BYTE*>(name),&size);RegCloseKey(key);
            if(status!=ERROR_SUCCESS||kind!=REG_SZ||size<10||size>sizeof(name)||name[(size/sizeof(wchar_t))-1]!=0) continue;
            const std::wstring port=name;bool valid=port.rfind(L"COM",0)==0&&port.size()>3;
            for(std::size_t p=3;p<port.size();++p) if(port[p]<L'0'||port[p]>L'9') valid=false;
            if(valid) result[side]=port;
        }
    }
    return result;
}
std::string Identity() {
    GUID guid{};if(FAILED(CoCreateGuid(&guid))) throw std::runtime_error("Unable to create a unique USB serial.");
    std::ostringstream out;out<<"VCW-"<<std::hex<<std::uppercase<<std::setfill('0');
    const auto* bytes=reinterpret_cast<const unsigned char*>(&guid);
    for(unsigned n=0;n<sizeof(guid);++n) out<<std::setw(2)<<unsigned(bytes[n]);
    return out.str();
}
bool Backpressure(std::uint32_t error) {
    return error==ERROR_BUSY||error==ERROR_NOT_READY||error==ERROR_RETRY||error==ERROR_NO_SYSTEM_RESOURCES||error==ERROR_DEVICE_NOT_AVAILABLE;
}
std::string PortText(const std::wstring& value) { std::string out;for(auto c:value)out.push_back(static_cast<char>(c));return out; }
void Require(bool valid,const std::string& message) { if(!valid) throw std::runtime_error(message); }
struct Handle {
    HANDLE value=INVALID_HANDLE_VALUE;
    ~Handle(){if(value!=INVALID_HANDLE_VALUE) CloseHandle(value);}
    Handle()=default; Handle(const Handle&)=delete;Handle& operator=(const Handle&)=delete;
};
void OpenPort(Handle& handle,const std::wstring& port) {
    handle.value=CreateFileW((L"\\\\.\\"+port).c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,0,nullptr);
    Require(handle.value!=INVALID_HANDLE_VALUE,"Open exact owned COM port failed: "+std::to_string(GetLastError()));
    DCB dcb{};dcb.DCBlength=sizeof(dcb);Require(GetCommState(handle.value,&dcb)!=FALSE,"GetCommState failed.");
    dcb.BaudRate=115200;dcb.ByteSize=8;dcb.StopBits=ONESTOPBIT;dcb.Parity=NOPARITY;dcb.fBinary=TRUE;
    dcb.fOutxCtsFlow=FALSE;dcb.fOutxDsrFlow=FALSE;dcb.fDtrControl=DTR_CONTROL_ENABLE;dcb.fRtsControl=RTS_CONTROL_ENABLE;
    dcb.fOutX=dcb.fInX=FALSE;dcb.fAbortOnError=FALSE;
    Require(SetCommState(handle.value,&dcb)!=FALSE,"SetCommState failed: "+std::to_string(GetLastError()));
    COMMTIMEOUTS timeouts{};timeouts.ReadIntervalTimeout=MAXDWORD;timeouts.ReadTotalTimeoutConstant=30;timeouts.WriteTotalTimeoutConstant=2000;
    Require(SetCommTimeouts(handle.value,&timeouts)!=FALSE,"SetCommTimeouts failed.");
    Require(PurgeComm(handle.value,PURGE_RXCLEAR|PURGE_TXCLEAR)!=FALSE,"Purge owned port failed.");
}
Cdc::Bytes Transfer(HANDLE source,HANDLE target,const Cdc::Bytes& payload) {
    DWORD sent=0;
    Require(WriteFile(source,payload.data(),static_cast<DWORD>(payload.size()),&sent,nullptr)!=FALSE&&sent==payload.size(),"Binary WriteFile failed: "+std::to_string(GetLastError()));
    Cdc::Bytes got; const auto deadline=Clock::now()+std::chrono::seconds(5);
    while(got.size()<payload.size()&&Clock::now()<deadline) {
        std::array<std::uint8_t,4096> buffer{};DWORD read=0;
        Require(ReadFile(target,buffer.data(),static_cast<DWORD>((std::min)(buffer.size(),payload.size()-got.size())),&read,nullptr)!=FALSE,"Binary ReadFile failed: "+std::to_string(GetLastError()));
        got.insert(got.end(),buffer.begin(),buffer.begin()+read);
    }
    Require(got==payload,"Binary roundtrip timed out or changed bytes."); return got;
}
std::string Json(const std::string& s) {
    std::string out="\"";for(unsigned char c:s) { if(c=='"'||c=='\\') {out+='\\';out+=static_cast<char>(c);}else if(c>=32&&c<127)out+=static_cast<char>(c);else out+=' '; }return out+'"';
}
}
bool Elevated() {
    HANDLE raw=nullptr; if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&raw)) return false;
    Handle token;token.value=raw;TOKEN_ELEVATION value{};DWORD size=0;
    return GetTokenInformation(token.value,TokenElevation,&value,sizeof(value),&size)&&value.TokenIsElevated;
}
struct Session::Impl {
    std::mutex mutex;std::thread worker;std::atomic<bool> stop{true};Snapshot snapshot;
    void Publish(const Snapshot& s) { std::lock_guard<std::mutex> lock(mutex); snapshot=s; }
    void Run(bool loopback) noexcept {
        Snapshot s;s.running=true;s.message="Creating a unique USB CDC ACM pair...";Publish(s);
        try {
            const auto identity=Identity();const std::array<std::string,2> serials{identity+"-A",identity+"-B"};
            std::array<Vdevice::Device,2> devices;
            // Device destructors are the final cleanup if any step throws.
            for(unsigned n=0;n<2;++n) {
                if(stop.load()) throw std::runtime_error("Creation cancelled.");
                devices[n]=Vdevice::Create_Device(Cdc::Configuration(serials[n]));
                if(!devices[n]) throw std::runtime_error("Create CDC port "+std::to_string(n+1)+": "+devices[n].message+" (Windows "+std::to_string(devices[n].error)+"). Install Vdevice and reopen live mode as administrator.");
            }
            std::array<Cdc::LineState,2> lines;std::array<Cdc::ByteQueue,2> pending;
            std::array<bool,2> notification{};auto inventory=Clock::now(),publish=Clock::now();
            const auto enumerationDeadline=Clock::now()+std::chrono::seconds(25);
            bool initialized=false;
            while(!stop.load()) {
                if(Clock::now()>=inventory) {
                    inventory=Clock::now()+std::chrono::milliseconds(250);
                    const auto info=Vdevice::Get_Info();
                    if(!info) throw std::runtime_error(Error("Vdevice status",info));
                    if(Trial<Vdevice::Info>::Expired(info)) throw std::runtime_error("Vdevice one-hour demo allowance expired. Restart Windows or request a production license from titancsa on Discord.");
                    if(info.quiescing||info.removing) throw std::runtime_error("Vdevice is stopping; recreate the pair after setup/restart.");
                    for(const auto& device:devices) {
                        const auto it=std::find_if(info.devices.begin(),info.devices.end(),[&](const auto& d){return d.id==device.Id();});
                        if(it!=info.devices.end()&&it->error) throw std::runtime_error("Windows reported a device error for an owned virtual port: "+std::to_string(it->error));
                    }
                    // usbser can suspend while no client owns the port. A started,
                    // exact COM identity can be opened to wake it; D0 is not a
                    // prerequisite for offering the COM name to the client.
                    s.ports=Ports(serials);s.ready=!s.ports[0].empty()&&!s.ports[1].empty()&&s.ports[0]!=s.ports[1];
                    if(s.ready) { initialized=true;s.message=loopback?"Loopback ready: each owned COM port echoes its own writes.":"Cable ready: writes to A arrive at B, and writes to B arrive at A."; }
                    else if(initialized) throw std::runtime_error("Windows removed an owned COM interface. Stop and create a fresh pair.");
                    else if(Clock::now()>enumerationDeadline) throw std::runtime_error("USB CDC enumeration timed out. No exact owned COM interfaces appeared. Check Vdevice setup, restart requirements and Device Manager.");
                    else s.message="Waiting for Windows usbser.sys and exact USB serial identities...";
                }
                for(unsigned target=0;target<2;++target) {
                    if(notification[target]) {
                        const auto result=Vdevice::Send_Input(devices[target],0x81,Cdc::SerialNotification(lines[target]));
                        if(result)notification[target]=false;else if(!Backpressure(result.error))throw std::runtime_error(Error("Serial status notification",result));
                    }
                    for(unsigned budget=0;budget<128&&!stop.load();++budget) {
                        const auto* packet=pending[target].Front();if(!packet)break;
                        const auto result=Vdevice::Send_Input(devices[target],0x82,*packet);
                        if(!result) { if(Backpressure(result.error))break;throw std::runtime_error(Error("Forward USB bulk data",result)); }
                        s.forwarded[target]+=packet->size();pending[target].Pop();
                    }
                    s.pending[target]=pending[target].Size();
                }
                const auto output=Vdevice::Get_Output(2);
                if(output) {
                    unsigned source=2;for(unsigned n=0;n<2;++n)if(output.device_id==devices[n].Id()&&output.device_token==devices[n].token)source=n;
                    if(source<2) {
                        if(output.kind==Vdevice::Output_Kind::Control) {
                            const auto response=Cdc::HandleControl(lines[source],output.setup,output.data);
                            std::string row=std::string("CTRL ")+(source?"B ":"A ")+Cdc::Hex(Cdc::Bytes(output.setup.begin(),output.setup.end()))+" / "+std::to_string(output.data.size())+" payload bytes -> "+(response.stall?"STALL":"ACK");
                            if(s.log.size()>=120)s.log.erase(s.log.begin());s.log.push_back(std::move(row));
                            const auto result=Vdevice::Respond_Output(output,response.data,response.stall);
                            if(!result)throw std::runtime_error(Error("CDC control response",result));
                            if(response.notification)notification[source]=true;
                        } else if(output.kind==Vdevice::Output_Kind::Data&&output.endpoint==0x02&&!output.data.empty()) {
                            const auto target=loopback?source:1-source;
                            if(!pending[target].Push(output.data))throw std::runtime_error("Cable queue reached 64 KiB. The receiving application is not draining data; pair stopped without silently dropping bytes.");
                            s.received[source]+=output.data.size();
                            const auto n=(std::min)(std::size_t(48),output.data.size());const Cdc::Bytes preview(output.data.begin(),output.data.begin()+n);
                            std::string row=std::string(source?"B":"A")+" -> "+(target?"B":"A")+"  "+std::to_string(output.data.size())+" bytes  "+Cdc::Hex(preview)+"  |"+Cdc::Ascii(preview)+"|";
                            if(n<output.data.size())row+=" ...";
                            if(s.log.size()>=120)s.log.erase(s.log.begin());s.log.push_back(std::move(row));
                        }
                    }
                } else if(!output.timed_out&&output.error!=ERROR_TIMEOUT&&output.error!=ERROR_NO_MORE_ITEMS&&output.error!=ERROR_NO_DATA)throw std::runtime_error(Error("Receive USB output",output));
                if(Clock::now()>=publish) { publish=Clock::now()+std::chrono::milliseconds(50);Publish(s); }
            }
            for(auto& device:devices) if(device) { const auto result=Vdevice::Delete_Device(device);if(!result)throw std::runtime_error(Error("Remove owned virtual COM port",result)); }
            s.message="Stopped. Both owned virtual USB devices have been removed.";
        } catch(const std::exception& e) { s.message=std::string("Stopped: ")+e.what(); }
        catch(...) { s.message="Stopped after an unexpected error; owned virtual devices were released."; }
        s.running=s.ready=false;Publish(s);
    }
};
Session::Session():impl_(std::make_unique<Impl>()){}
Session::~Session(){Stop();}
void Session::Start(bool loopback) { Stop();impl_->stop=false;{std::lock_guard<std::mutex> lock(impl_->mutex);impl_->snapshot=Snapshot{};impl_->snapshot.running=true;impl_->snapshot.message="Starting virtual cable...";}impl_->worker=std::thread([this,loopback]{impl_->Run(loopback);}); }
void Session::Stop() { impl_->stop=true;if(impl_->worker.joinable())impl_->worker.join(); }
Snapshot Session::Read(){std::lock_guard<std::mutex> lock(impl_->mutex);return impl_->snapshot;}
int SmokeTest(const std::wstring& report) {
    std::string error;Snapshot snapshot;std::uint64_t bytes=0;double roundtripMs=0;
    try {
        Require(Elevated(),"Smoke test requires an elevated process. Run it only in an authorized test VM.");
        Session session;session.Start();const auto deadline=Clock::now()+std::chrono::seconds(30);
        do { snapshot=session.Read();if(snapshot.ready||!snapshot.running)break;std::this_thread::sleep_for(std::chrono::milliseconds(20)); }while(Clock::now()<deadline);
        Require(snapshot.ready,snapshot.message);
        Handle a,b;OpenPort(a,snapshot.ports[0]);OpenPort(b,snapshot.ports[1]);
        Cdc::Bytes payload(4096);for(std::size_t n=0;n<payload.size();++n)payload[n]=static_cast<std::uint8_t>((n*73+19)&255);
        const auto begin=Clock::now();Transfer(a.value,b.value,payload);bytes+=payload.size();
        std::reverse(payload.begin(),payload.end());Transfer(b.value,a.value,payload);bytes+=payload.size();
        roundtripMs=std::chrono::duration<double,std::milli>(Clock::now()-begin).count();snapshot=session.Read();
        // Close client handles before deleting the USB devices.
        CloseHandle(a.value);a.value=INVALID_HANDLE_VALUE;CloseHandle(b.value);b.value=INVALID_HANDLE_VALUE;
        session.Stop();
        Require(session.Read().message=="Stopped. Both owned virtual USB devices have been removed.",session.Read().message);
    } catch(const std::exception& e) {error=e.what();}
    std::ofstream out(std::filesystem::path(report),std::ios::binary|std::ios::trunc);
    if(!out)return 2;
    const auto a=PortText(snapshot.ports[0]), b=PortText(snapshot.ports[1]);
    out<<"{\n  \"schema\": 1,\n  \"passed\": "<<(error.empty()?"true":"false")<<",\n  \"port_a\": "<<Json(a)<<",\n  \"port_b\": "<<Json(b)<<",\n  \"binary_bytes_verified\": "<<bytes<<",\n  \"two_transfers_ms\": "<<roundtripMs<<",\n  \"measurement_scope\": \"Two application-level serial transfers; not physical UART latency\",\n  \"error\": "<<Json(error)<<",\n  \"trace\": [";
    for(std::size_t n=0;n<snapshot.log.size();++n){if(n)out<<',';out<<'\n'<<"    "<<Json(snapshot.log[n]);}
    out<<"\n  ]\n}\n";
    return out.good()?(error.empty()?0:1):2;
}
}
