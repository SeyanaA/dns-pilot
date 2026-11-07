#include "dns.h"

#include <functional>

#include "util.h"

static uint16_t rd16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t rd32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static bool skipName(const uint8_t *p, size_t n, size_t &off) {
    while (true) {
        if (off >= n) return false;
        uint8_t len = p[off];
        if (len == 0) { off++; return true; }
        if ((len & 0xC0) == 0xC0) {
            if (off + 1 >= n) return false;
            off += 2;
            return true;
        }
        if (len & 0xC0) return false;
        if (off + 1 + len > n) return false;
        off += 1 + len;
    }
}

static bool readName(const uint8_t *p, size_t n, size_t &off, std::string &out) {
    size_t pos = off;
    bool jumped = false;
    int jumps = 0;
    std::string s;
    while (true) {
        if (pos >= n) return false;
        uint8_t len = p[pos];
        if (len == 0) {
            if (!jumped) off = pos + 1;
            break;
        }
        if ((len & 0xC0) == 0xC0) {
            if (pos + 1 >= n) return false;
            size_t ptr = ((size_t)(len & 0x3F) << 8) | p[pos + 1];
            if (!jumped) off = pos + 2;
            jumped = true;
            if (++jumps > 32 || ptr >= n) return false;
            pos = ptr;
            continue;
        }
        if (len & 0xC0) return false;
        if (pos + 1 + len > n) return false;
        if (!s.empty()) s += '.';
        s.append((const char *)p + pos + 1, len);
        pos += 1 + len;
        if (s.size() > 512) return false;
    }
    out = s;
    return true;
}

const char *dnsTypeName(uint16_t t) {
    static thread_local char buf[24];
    switch (t) {
        case 1: return "A";
        case 2: return "NS";
        case 5: return "CNAME";
        case 6: return "SOA";
        case 12: return "PTR";
        case 15: return "MX";
        case 16: return "TXT";
        case 28: return "AAAA";
        case 33: return "SRV";
        case 41: return "OPT";
        case 43: return "DS";
        case 46: return "RRSIG";
        case 64: return "SVCB";
        case 65: return "HTTPS";
        case 255: return "ANY";
        default:
            snprintf(buf, sizeof(buf), "TYPE%u", (unsigned)t);
            return buf;
    }
}

uint16_t dnsGetId(const uint8_t *p, size_t n) { return n >= 2 ? rd16(p) : 0; }
void dnsSetId(uint8_t *p, size_t n, uint16_t id) {
    if (n >= 2) { p[0] = (uint8_t)(id >> 8); p[1] = (uint8_t)id; }
}

bool dnsParseQuestion(const uint8_t *p, size_t n, DnsQuestion &q) {
    if (n < 12) return false;
    if (rd16(p + 4) < 1) return false;
    size_t off = 12;
    if (!readName(p, n, off, q.name)) return false;
    if (off + 4 > n) return false;
    q.qtype = rd16(p + off);
    q.qclass = rd16(p + off + 2);
    return !q.name.empty();
}

bool dnsAnalyze(const uint8_t *p, size_t n, DnsMeta &m) {
    if (n < 12) return false;
    m.id = rd16(p);
    m.flags = rd16(p + 2);
    m.qdcount = rd16(p + 4);
    m.ancount = rd16(p + 6);
    m.nscount = rd16(p + 8);
    m.arcount = rd16(p + 10);
    m.isResponse = (m.flags & 0x8000) != 0;
    m.truncated = (m.flags & 0x0200) != 0;
    m.rcode = (uint16_t)(m.flags & 0x000F);

    size_t off = 12;
    for (uint16_t i = 0; i < m.qdcount; i++) {
        if (!skipName(p, n, off) || off + 4 > n) return false;
        off += 4;
    }

    uint32_t minAnswer = UINT32_MAX, minAuthority = UINT32_MAX;
    uint32_t total = (uint32_t)m.ancount + m.nscount + m.arcount;
    for (uint32_t i = 0; i < total; i++) {
        if (!skipName(p, n, off)) return false;
        if (off + 10 > n) return false;
        uint16_t type = rd16(p + off);
        uint32_t ttl = rd32(p + off + 4);
        uint16_t rdlen = rd16(p + off + 8);
        off += 10;
        if (off + rdlen > n) return false;
        if (type != 41) {
            if (i < m.ancount) {
                if (ttl < minAnswer) minAnswer = ttl;
            } else if (i < (uint32_t)m.ancount + m.nscount) {
                if (ttl < minAuthority) minAuthority = ttl;
            }
        }
        off += rdlen;
    }
    m.minTtl = (minAnswer != UINT32_MAX) ? minAnswer : (minAuthority != UINT32_MAX ? minAuthority : 0);
    return true;
}

bool dnsRewriteTtls(uint8_t *p, size_t n, uint32_t dec) {
    if (n < 12 || dec == 0) return n >= 12;
    uint16_t qd = rd16(p + 4);
    uint16_t an = rd16(p + 6);
    uint16_t ns = rd16(p + 8);
    uint16_t ar = rd16(p + 10);
    size_t off = 12;
    for (uint16_t i = 0; i < qd; i++) {
        if (!skipName(p, n, off) || off + 4 > n) return false;
        off += 4;
    }
    uint32_t total = (uint32_t)an + ns + ar;
    for (uint32_t i = 0; i < total; i++) {
        if (!skipName(p, n, off)) return false;
        if (off + 10 > n) return false;
        uint16_t type = rd16(p + off);
        uint16_t rdlen = rd16(p + off + 8);
        if (type != 41) {
            uint32_t ttl = rd32(p + off + 4);
            uint32_t nt = ttl > dec ? ttl - dec : 0;
            wr32(p + off + 4, nt);
        }
        off += 10 + rdlen;
        if (off > n) return false;
    }
    return true;
}

bool dnsParseAnswerIps(const uint8_t *p, size_t n, std::vector<std::string> &ips) {
    if (n < 12) return false;
    uint16_t qd = rd16(p + 4), an = rd16(p + 6);
    size_t off = 12;
    for (uint16_t i = 0; i < qd; i++) {
        if (!skipName(p, n, off) || off + 4 > n) return false;
        off += 4;
    }
    for (uint16_t i = 0; i < an; i++) {
        if (!skipName(p, n, off)) return false;
        if (off + 10 > n) return false;
        uint16_t type = rd16(p + off);
        uint16_t cls = rd16(p + off + 2);
        uint16_t rdlen = rd16(p + off + 8);
        size_t rdOff = off + 10;
        if (rdOff + rdlen > n) return false;
        char buf[INET6_ADDRSTRLEN] = {0};
        if (cls == 1 && type == 1 && rdlen == 4) {
            InetNtopA(AF_INET, p + rdOff, buf, sizeof(buf));
            ips.push_back(buf);
        } else if (cls == 1 && type == 28 && rdlen == 16) {
            InetNtopA(AF_INET6, p + rdOff, buf, sizeof(buf));
            ips.push_back(buf);
        }
        off = rdOff + rdlen;
    }
    return true;
}

void DnsCache::configure(bool enabled, int minTtl, int maxTtl) {
    std::lock_guard<std::mutex> g(mu_);
    enabled_ = enabled;
    if (minTtl < 1) minTtl = 1;
    if (maxTtl < minTtl) maxTtl = minTtl;
    minTtl_ = minTtl;
    maxTtl_ = maxTtl;
    if (!enabled_) map_.clear();
}

bool DnsCache::get(const std::string &key, std::vector<uint8_t> &out, uint32_t &ageSec) {
    std::lock_guard<std::mutex> g(mu_);
    if (!enabled_) { misses_++; return false; }
    auto it = map_.find(key);
    if (it == map_.end()) { misses_++; return false; }
    uint64_t elapsed = (util::nowMs() - it->second.storedMs) / 1000;
    if (elapsed >= it->second.ttl) {
        map_.erase(it);
        misses_++;
        return false;
    }
    out = it->second.pkt;
    ageSec = (uint32_t)elapsed;
    if (elapsed > 0) dnsRewriteTtls(out.data(), out.size(), (uint32_t)elapsed);
    hits_++;
    return true;
}

void DnsCache::put(const std::string &key, const std::vector<uint8_t> &pkt, uint32_t ttl) {
    std::lock_guard<std::mutex> g(mu_);
    if (!enabled_ || pkt.size() < 12) return;
    uint32_t t = ttl;
    if (t < (uint32_t)minTtl_) t = (uint32_t)minTtl_;
    if (t > (uint32_t)maxTtl_) t = (uint32_t)maxTtl_;
    if (t == 0) t = (uint32_t)minTtl_;

    if (map_.size() > 4096) {
        uint64_t now = util::nowMs();
        for (auto it = map_.begin(); it != map_.end();) {
            if ((now - it->second.storedMs) / 1000 >= it->second.ttl) it = map_.erase(it);
            else ++it;
        }
        while (map_.size() > 4096) map_.erase(map_.begin());
    }
    DnsCacheEntry e;
    e.pkt = pkt;
    e.storedMs = util::nowMs();
    e.ttl = t;
    map_[key] = std::move(e);
}

void DnsCache::clear() {
    std::lock_guard<std::mutex> g(mu_);
    map_.clear();
}

size_t DnsCache::size() const {
    std::lock_guard<std::mutex> g(mu_);
    return map_.size();
}

uint64_t DnsCache::hits() const {
    std::lock_guard<std::mutex> g(mu_);
    return hits_;
}

uint64_t DnsCache::misses() const {
    std::lock_guard<std::mutex> g(mu_);
    return misses_;
}
