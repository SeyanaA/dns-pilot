// DNS 报文解析 / TTL 处理 / 缓存
#pragma once
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

struct DnsQuestion {
    std::string name;
    uint16_t qtype = 0;
    uint16_t qclass = 1;
};

struct DnsMeta {
    uint16_t id = 0;
    uint16_t flags = 0;
    uint16_t qdcount = 0, ancount = 0, nscount = 0, arcount = 0;
    uint16_t rcode = 0;
    bool isResponse = false;
    bool truncated = false;
    uint32_t minTtl = 0;
};

struct DnsCacheEntry {
    std::vector<uint8_t> pkt;
    uint64_t storedMs = 0;
    uint32_t ttl = 0;
};

const char *dnsTypeName(uint16_t t);

bool dnsParseQuestion(const uint8_t *p, size_t n, DnsQuestion &q);
bool dnsAnalyze(const uint8_t *p, size_t n, DnsMeta &m);
bool dnsRewriteTtls(uint8_t *p, size_t n, uint32_t dec);
bool dnsParseAnswerIps(const uint8_t *p, size_t n, std::vector<std::string> &ips);
uint16_t dnsGetId(const uint8_t *p, size_t n);
void dnsSetId(uint8_t *p, size_t n, uint16_t id);

class DnsCache {
  public:
    void configure(bool enabled, int minTtl, int maxTtl);
    bool get(const std::string &key, std::vector<uint8_t> &out, uint32_t &ageSec);
    void put(const std::string &key, const std::vector<uint8_t> &pkt, uint32_t ttl);
    void clear();
    size_t size() const;
    uint64_t hits() const;
    uint64_t misses() const;

  private:
    mutable std::mutex mu_;
    std::unordered_map<std::string, DnsCacheEntry> map_;
    bool enabled_ = true;
    int minTtl_ = 30;
    int maxTtl_ = 3600;
    uint64_t hits_ = 0;
    uint64_t misses_ = 0;
};
