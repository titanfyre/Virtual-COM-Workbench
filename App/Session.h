#pragma once
#include "Cdc.h"
#include <array>
#include <memory>
#include <string>
#include <vector>

namespace Serial {
struct Snapshot {
    bool running=false, ready=false;
    std::string message="Create a pair to connect two applications through real Windows COM ports.";
    std::array<std::wstring,2> ports;
    std::array<std::uint64_t,2> received{}, forwarded{};
    std::array<std::size_t,2> pending{};
    std::vector<std::string> log;
};
class Session {
public:
    Session(); ~Session();
    Session(const Session&)=delete; Session& operator=(const Session&)=delete;
    void Start(bool loopback=false);
    void Stop();
    Snapshot Read();
private:
    struct Impl; std::unique_ptr<Impl> impl_;
};
// Uses only the two newly created ports. Existing physical ports are never opened.
int SmokeTest(const std::wstring& report);
bool Elevated();
}
