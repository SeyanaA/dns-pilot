// 源端口 → 进程归因（UDP/TCP，IPv4/IPv6）
#pragma once
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

struct ProcInfo {
    uint32_t pid = 0;
    std::string exe;
};

class ProcTracker {
  public:
    ProcInfo udpOwner(uint16_t localPort, int family);
    ProcInfo tcpOwner(uint16_t localPort, int family);

  private:
    void refreshLocked(bool force);
    std::string exeOfLocked(uint32_t pid);

    std::mutex mu_;
    uint64_t lastRefresh_ = 0;
    std::unordered_map<uint16_t, uint32_t> udp4_, udp6_, tcp4_, tcp6_;
    std::unordered_map<uint32_t, std::pair<std::string, uint64_t>> exeCache_;
};
