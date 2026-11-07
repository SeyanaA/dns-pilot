#include "proc.h"

#include "util.h"

template <typename Table, typename Row>
static void collectPorts(DWORD family, std::unordered_map<uint16_t, uint32_t> &out) {
    DWORD size = 0;
    DWORD r = GetExtendedUdpTable(nullptr, &size, FALSE, family, UDP_TABLE_OWNER_PID, 0);
    if (r != ERROR_INSUFFICIENT_BUFFER) return;
    auto *buf = (uint8_t *)malloc(size);
    if (!buf) return;
    if (GetExtendedUdpTable(buf, &size, FALSE, family, UDP_TABLE_OWNER_PID, 0) == NO_ERROR) {
        auto *t = (Table *)buf;
        for (DWORD i = 0; i < t->dwNumEntries; i++) {
            uint16_t port = (uint16_t)ntohs((u_short)t->table[i].dwLocalPort);
            if (!out.count(port)) out[port] = t->table[i].dwOwningPid;
        }
    }
    free(buf);
}

static void collectTcp(DWORD family, std::unordered_map<uint16_t, uint32_t> &out) {
    DWORD size = 0;
    DWORD r = GetExtendedTcpTable(nullptr, &size, FALSE, family, TCP_TABLE_OWNER_PID_ALL, 0);
    if (r != ERROR_INSUFFICIENT_BUFFER) return;
    auto *buf = (uint8_t *)malloc(size);
    if (!buf) return;
    if (GetExtendedTcpTable(buf, &size, FALSE, family, TCP_TABLE_OWNER_PID_ALL, 0) == NO_ERROR) {
        auto *t = (MIB_TCPTABLE_OWNER_PID *)buf;
        if (family == AF_INET) {
            for (DWORD i = 0; i < t->dwNumEntries; i++) {
                uint16_t port = (uint16_t)ntohs((u_short)t->table[i].dwLocalPort);
                if (!out.count(port)) out[port] = t->table[i].dwOwningPid;
            }
        } else {
            auto *t6 = (MIB_TCP6TABLE_OWNER_PID *)buf;
            for (DWORD i = 0; i < t6->dwNumEntries; i++) {
                uint16_t port = (uint16_t)ntohs((u_short)t6->table[i].dwLocalPort);
                if (!out.count(port)) out[port] = t6->table[i].dwOwningPid;
            }
        }
    }
    free(buf);
}

void ProcTracker::refreshLocked(bool force) {
    uint64_t now = util::nowMs();
    if (!force && now - lastRefresh_ < 400) return;
    lastRefresh_ = now;
    udp4_.clear();
    udp6_.clear();
    tcp4_.clear();
    tcp6_.clear();
    collectPorts<MIB_UDPTABLE_OWNER_PID, MIB_UDPROW_OWNER_PID>(AF_INET, udp4_);
    collectPorts<MIB_UDP6TABLE_OWNER_PID, MIB_UDP6ROW_OWNER_PID>(AF_INET6, udp6_);
    collectTcp(AF_INET, tcp4_);
    collectTcp(AF_INET6, tcp6_);
}

std::string ProcTracker::exeOfLocked(uint32_t pid) {
    if (pid == 0) return "";
    uint64_t now = util::nowMs();
    auto it = exeCache_.find(pid);
    if (it != exeCache_.end() && now - it->second.second < 5000) return it->second.first;
    std::string name;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (h) {
        wchar_t buf[MAX_PATH * 2];
        DWORD n = (DWORD)(sizeof(buf) / sizeof(buf[0]));
        if (QueryFullProcessImageNameW(h, 0, buf, &n)) {
            std::string full = util::u8(std::wstring(buf, n));
            size_t pos = full.find_last_of("\\/");
            name = pos == std::string::npos ? full : full.substr(pos + 1);
        }
        CloseHandle(h);
    }
    exeCache_[pid] = {name, now};
    if (exeCache_.size() > 2048) exeCache_.clear();
    return name;
}

ProcInfo ProcTracker::udpOwner(uint16_t localPort, int family) {
    ProcInfo out;
    std::lock_guard<std::mutex> g(mu_);
    refreshLocked(false);
    auto &map = (family == AF_INET6) ? udp6_ : udp4_;
    auto it = map.find(localPort);
    if (it == map.end()) {
        refreshLocked(true);
        it = map.find(localPort);
    }
    if (it != map.end()) {
        out.pid = it->second;
        out.exe = exeOfLocked(out.pid);
    }
    return out;
}

ProcInfo ProcTracker::tcpOwner(uint16_t localPort, int family) {
    ProcInfo out;
    std::lock_guard<std::mutex> g(mu_);
    refreshLocked(false);
    auto &map = (family == AF_INET6) ? tcp6_ : tcp4_;
    auto it = map.find(localPort);
    if (it == map.end()) {
        refreshLocked(true);
        it = map.find(localPort);
    }
    if (it != map.end()) {
        out.pid = it->second;
        out.exe = exeOfLocked(out.pid);
    }
    return out;
}
