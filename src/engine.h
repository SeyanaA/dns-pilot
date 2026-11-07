// 解析引擎：本机 DNS 服务、规则路由、上游优先/回退、缓存
#pragma once
#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "dns.h"
#include "model.h"
#include "proc.h"
#include "stats.h"
#include "util.h"

#include <mutex>
#include <unordered_map>

struct UpstreamResult {
    bool ok = false;
    std::vector<uint8_t> pkt;
    std::string server;
    std::string status = "timeout";
    uint16_t rcode = 0;
};

struct ResolveResult {
    bool ok = false;
    std::vector<uint8_t> pkt;
    std::vector<std::string> ips;
    std::string profileId;
    std::string profileName;
    std::string upstream;
    std::string ruleType = "none";
    std::string ruleMatch;
    std::string status = "error";
    double latencyMs = 0;
    bool cached = false;
    uint16_t rcode = 0;
    uint16_t qtype = 1;
};

class Engine {
  public:
    Engine(ConfigStore &cfg, Stats &stats, ProcTracker &proc, DnsCache &cache);
    ~Engine();

    bool start(std::string &err);
    void stop();
    bool ready() const;
    bool ipv6Ready() const { return udp6_ != INVALID_SOCKET && tcp6_ != INVALID_SOCKET; }
    uint16_t port() const { return port_; }
    bool selfTest(std::string &err);
    void flushCache();
    void applyRuntimeSettings();
    size_t cacheSize() const { return cache_.size(); }

    ResolveResult lookupPacket(const std::vector<uint8_t> &query, const std::string &profileId, bool useCache);
    ResolveResult lookupName(const std::string &domain, uint16_t qtype, const std::string &profileId,
                             bool useCache);
    ResolveResult resolveForHost(const std::string &host, const std::string &exe, uint32_t pid, uint16_t qtype);
    ResolveResult resolveTest(const std::string &domain, const std::string &profileId);

  private:
    void udpLoop(SOCKET s, int family);
    void tcpLoop(SOCKET s, int family);
    void handleTcpClient(SOCKET client, int family);
    std::vector<uint8_t> handleQuery(const std::vector<uint8_t> &query, const std::string &clientExe,
                                     uint32_t pid, const char *source);
    UpstreamResult forward(const std::vector<uint8_t> &query, const Profile &p, int timeoutMs);
    void emitDnsEvent(const DnsQuestion &q, const ProfileChoice &choice, const ResolveResult &r,
                      const std::string &exe, uint32_t pid);
    std::string profileNameOf(const std::string &id) const;
    void markServer(const std::string &server, bool ok);
    std::vector<std::string> orderedServers(const std::vector<std::string> &servers);

    ConfigStore &cfg_;
    Stats &stats_;
    ProcTracker &proc_;
    DnsCache &cache_;
    util::WorkerPool pool_;

    std::atomic<bool> running_{false};
    SOCKET udp4_ = INVALID_SOCKET, udp6_ = INVALID_SOCKET, tcp4_ = INVALID_SOCKET, tcp6_ = INVALID_SOCKET;
    std::vector<std::thread> threads_;
    uint16_t port_ = 53;
    std::string listen_ = "127.0.0.1";

    // 上游健康：失败次数与冷却截止时间，避免每查都等死服务器
    struct ServerHealth {
        int fails = 0;
        uint64_t retryAt = 0;
    };
    std::mutex healthMu_;
    std::unordered_map<std::string, ServerHealth> health_;
};

