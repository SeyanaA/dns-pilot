// 运行时统计与事件环（内存）
#pragma once
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "json.h"

struct Event {
    uint64_t t = 0;
    std::string source;      // dns / proxy
    std::string client;      // exe 名，未知时为空
    uint32_t pid = 0;
    std::string domain;
    uint16_t qtype = 1;
    std::string profileId;
    std::string profileName;
    std::string upstream;
    std::string ruleType;    // domain / app / none
    std::string ruleMatch;
    std::string status;      // ok / nxdomain / servfail / timeout / error / blocked
    double latencyMs = 0;
    bool cached = false;
};

// 原生界面使用的统计快照（不经过 JSON，避免每帧序列化开销）
struct UiEvent {
    uint64_t t = 0;
    std::string source;      // dns / proxy
    std::string client;
    std::string domain;
    std::string qtype;
    std::string profileName;
    std::string upstream;
    std::string ruleType;    // domain / app / none
    std::string status;
    uint32_t pid = 0;
    double latencyMs = 0;
    bool cached = false;
};
struct UiCount {
    std::string name;
    uint64_t count = 0;
};
struct UiStats {
    uint64_t total = 0, cacheHits = 0, failures = 0, proxyConns = 0;
    double hitRate = 0, avgLatency = 0;
    std::vector<UiEvent> events;          // oldest -> newest, last eventLimit
    std::vector<UiCount> topDomains;      // desc, max 8
    std::vector<UiCount> topApps;         // desc, max 8
    std::vector<UiCount> topProfiles;     // desc, max 8
    std::vector<uint32_t> timeline;       // exactly 60 buckets, oldest -> newest, per second
    uint32_t timelineMax = 0;
};

class Stats {
  public:
    void add(const Event &e);
    void reset();
    js::Value snapshot(int eventLimit = 200) const;
    UiStats view(int eventLimit = 200) const;
    uint64_t totalQueries() const;

  private:
    mutable std::mutex mu_;
    std::deque<Event> events_;
    uint64_t total_ = 0;
    uint64_t hits_ = 0;
    uint64_t failures_ = 0;
    uint64_t proxy_ = 0;
    double latencySum_ = 0;
    uint64_t latencyCount_ = 0;
    std::unordered_map<std::string, uint64_t> perApp_, perDomain_, perProfile_;
    std::unordered_map<uint64_t, uint32_t> timeline_;  // 秒 -> 次数
};
