#include "stats.h"

#include <algorithm>
#include <utility>

#include "util.h"
#include "dns.h"

static js::Value topList(const std::unordered_map<std::string, uint64_t> &m, int limit,
                         const char *keyName) {
    std::vector<std::pair<std::string, uint64_t>> items(m.begin(), m.end());
    std::sort(items.begin(), items.end(), [](auto &a, auto &b) {
        if (a.second != b.second) return a.second > b.second;
        return a.first < b.first;
    });
    if ((int)items.size() > limit) items.resize((size_t)limit);
    js::Value arr = js::Value::arrV();
    for (auto &it : items) {
        js::Value o = js::Value::objV();
        o[keyName] = js::Value::strV(it.first);
        o["count"] = js::Value::numV((double)it.second);
        arr.arr.push_back(o);
    }
    return arr;
}

// 原生界面排行榜：与 JSON 版本一致的排序（次数倒序、同次数按名称升序），上限 8 条
static std::vector<UiCount> uiTopList(const std::unordered_map<std::string, uint64_t> &m, int limit) {
    std::vector<std::pair<std::string, uint64_t>> items(m.begin(), m.end());
    std::sort(items.begin(), items.end(), [](const auto &a, const auto &b) {
        if (a.second != b.second) return a.second > b.second;
        return a.first < b.first;
    });
    if ((int)items.size() > limit) items.resize((size_t)limit);
    std::vector<UiCount> out;
    out.reserve(items.size());
    for (const auto &it : items) {
        UiCount c;
        c.name = it.first;
        c.count = it.second;
        out.push_back(std::move(c));
    }
    return out;
}

void Stats::add(const Event &e) {
    std::lock_guard<std::mutex> g(mu_);
    events_.push_back(e);
    if (events_.size() > 400) events_.pop_front();

    if (e.source == "proxy") proxy_++;
    else total_++;
    // 命中率只统计 DNS 路径，代理缓存命中不计入分母，避免出现 >100%
    if (e.cached && e.source != "proxy") hits_++;
    if (e.status != "ok" && e.status != "nxdomain") failures_++;
    if (!e.cached) {
        latencySum_ += e.latencyMs;
        latencyCount_++;
    }

    if (!e.client.empty()) perApp_[e.client]++;
    perDomain_[e.domain]++;
    perProfile_[e.profileName.empty() ? e.profileId : e.profileName]++;

    uint64_t sec = e.t / 1000;
    timeline_[sec]++;
    if (timeline_.size() > 400) {
        for (auto it = timeline_.begin(); it != timeline_.end();) {
            if (sec > it->first + 300) it = timeline_.erase(it);
            else ++it;
        }
    }
}

void Stats::reset() {
    std::lock_guard<std::mutex> g(mu_);
    events_.clear();
    total_ = hits_ = failures_ = proxy_ = 0;
    latencySum_ = 0;
    latencyCount_ = 0;
    perApp_.clear();
    perDomain_.clear();
    perProfile_.clear();
    timeline_.clear();
}

uint64_t Stats::totalQueries() const {
    std::lock_guard<std::mutex> g(mu_);
    return total_;
}

js::Value Stats::snapshot(int eventLimit) const {
    std::lock_guard<std::mutex> g(mu_);
    js::Value v = js::Value::objV();
    v["total"] = js::Value::numV((double)total_);
    v["cacheHits"] = js::Value::numV((double)hits_);
    v["failures"] = js::Value::numV((double)failures_);
    v["proxyConns"] = js::Value::numV((double)proxy_);
    double hitRate = total_ > 0 ? (double)hits_ / (double)total_ : 0;
    v["hitRate"] = js::Value::numV(hitRate);
    v["avgLatency"] = js::Value::numV(latencyCount_ ? latencySum_ / (double)latencyCount_ : 0);
    v["perApp"] = topList(perApp_, 12, "name");
    v["perDomain"] = topList(perDomain_, 12, "name");
    v["perProfile"] = topList(perProfile_, 12, "name");

    uint64_t nowSec = util::nowMs() / 1000;
    js::Value tl = js::Value::arrV();
    for (int i = 59; i >= 0; i--) {
        uint64_t sec = nowSec - (uint64_t)i;
        auto it = timeline_.find(sec);
        js::Value o = js::Value::objV();
        o["t"] = js::Value::numV((double)sec);
        o["count"] = js::Value::numV(it == timeline_.end() ? 0 : (double)it->second);
        tl.arr.push_back(o);
    }
    v["timeline"] = tl;

    js::Value ev = js::Value::arrV();
    size_t start = events_.size() > (size_t)eventLimit ? events_.size() - (size_t)eventLimit : 0;
    for (size_t i = start; i < events_.size(); i++) {
        const Event &e = events_[i];
        js::Value o = js::Value::objV();
        o["t"] = js::Value::numV((double)e.t);
        o["source"] = js::Value::strV(e.source);
        o["client"] = js::Value::strV(e.client);
        o["pid"] = js::Value::numV((double)e.pid);
        o["domain"] = js::Value::strV(e.domain);
        o["qtype"] = js::Value::strV(dnsTypeName(e.qtype));
        o["profileId"] = js::Value::strV(e.profileId);
        o["profileName"] = js::Value::strV(e.profileName);
        o["upstream"] = js::Value::strV(e.upstream);
        o["ruleType"] = js::Value::strV(e.ruleType);
        o["ruleMatch"] = js::Value::strV(e.ruleMatch);
        o["status"] = js::Value::strV(e.status);
        o["latencyMs"] = js::Value::numV(e.latencyMs);
        o["cached"] = js::Value::boolV(e.cached);
        ev.arr.push_back(std::move(o));
    }
    v["events"] = ev;
    return v;
}

UiStats Stats::view(int eventLimit) const {
    std::lock_guard<std::mutex> g(mu_);
    UiStats s;
    s.total = total_;
    s.cacheHits = hits_;
    s.failures = failures_;
    s.proxyConns = proxy_;
    s.hitRate = total_ > 0 ? (double)hits_ / (double)total_ : 0;
    s.avgLatency = latencyCount_ ? latencySum_ / (double)latencyCount_ : 0;
    s.topDomains = uiTopList(perDomain_, 8);
    s.topApps = uiTopList(perApp_, 8);
    s.topProfiles = uiTopList(perProfile_, 8);

    // 与 snapshot() 相同的时间窗口：最后 60 个整秒，最旧 -> 最新
    uint64_t nowSec = util::nowMs() / 1000;
    s.timeline.assign(60, 0);
    for (int i = 0; i < 60; i++) {
        uint64_t sec = nowSec >= (uint64_t)(59 - i) ? nowSec - (uint64_t)(59 - i) : 0;
        auto it = timeline_.find(sec);
        uint32_t n = it == timeline_.end() ? 0 : it->second;
        s.timeline[(size_t)i] = n;
        if (n > s.timelineMax) s.timelineMax = n;
    }

    int limit = eventLimit < 0 ? 0 : eventLimit;
    size_t start = events_.size() > (size_t)limit ? events_.size() - (size_t)limit : 0;
    s.events.reserve(events_.size() - start);
    for (size_t i = start; i < events_.size(); i++) {
        const Event &e = events_[i];
        UiEvent u;
        u.t = e.t;
        u.source = e.source;
        u.client = e.client;
        u.domain = e.domain;
        u.qtype = dnsTypeName(e.qtype);
        // 配置改名或只有 id 时回退到 id，保证界面始终有可显示的名称
        u.profileName = e.profileName.empty() ? e.profileId : e.profileName;
        u.upstream = e.upstream;
        u.ruleType = e.ruleType;
        u.status = e.status;
        u.pid = e.pid;
        u.latencyMs = e.latencyMs;
        u.cached = e.cached;
        s.events.push_back(std::move(u));
    }
    return s;
}
