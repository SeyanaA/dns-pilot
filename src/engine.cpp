#include "engine.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

using util::nowMs;

static std::string rcodeStatus(uint16_t rcode) {
    switch (rcode) {
        case 0: return "ok";
        case 2: return "servfail";
        case 3: return "nxdomain";
        case 5: return "refused";
        default: return "error";
    }
}

static void put16(std::vector<uint8_t> &v, size_t off, uint16_t x) {
    if (off + 1 < v.size()) {
        v[off] = (uint8_t)(x >> 8);
        v[off + 1] = (uint8_t)x;
    }
}

static bool sameEndpoint(const sockaddr *a, const sockaddr *b) {
    if (a->sa_family != b->sa_family) return false;
    if (a->sa_family == AF_INET) {
        auto *x = (const sockaddr_in *)a;
        auto *y = (const sockaddr_in *)b;
        return x->sin_port == y->sin_port && x->sin_addr.s_addr == y->sin_addr.s_addr;
    }
    if (a->sa_family == AF_INET6) {
        auto *x = (const sockaddr_in6 *)a;
        auto *y = (const sockaddr_in6 *)b;
        return x->sin6_port == y->sin6_port &&
               memcmp(&x->sin6_addr, &y->sin6_addr, sizeof(in6_addr)) == 0;
    }
    return false;
}

static bool sendAll(SOCKET s, const uint8_t *data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        int n = send(s, (const char *)data + sent, (int)(len - sent), 0);
        if (n <= 0) return false;
        sent += (size_t)n;
    }
    return true;
}

static bool recvAll(SOCKET s, uint8_t *data, size_t len) {
    size_t got = 0;
    while (got < len) {
        int n = recv(s, (char *)data + got, (int)(len - got), 0);
        if (n <= 0) return false;
        got += (size_t)n;
    }
    return true;
}

static bool udpQuery(const sockaddr_storage &ss, socklen_t slen, const std::vector<uint8_t> &q,
                     uint16_t wantId, int timeoutMs, std::vector<uint8_t> &out) {
    SOCKET s = socket(((const sockaddr *)&ss)->sa_family, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return false;
    bool ok = false;
    uint64_t deadline = nowMs() + (uint64_t)timeoutMs;
    uint64_t nextRetry = nowMs() + 350;
    int retries = 0;
    if (sendto(s, (const char *)q.data(), (int)q.size(), 0, (const sockaddr *)&ss, slen) == (int)q.size()) {
        uint8_t buf[65535];
        while (nowMs() < deadline) {
            uint64_t now = nowMs();
            if (retries < 2 && now >= nextRetry) {
                // 超时窗口内重传，抵御 UDP 丢包
                sendto(s, (const char *)q.data(), (int)q.size(), 0, (const sockaddr *)&ss, slen);
                retries++;
                nextRetry = now + 350;
                continue;
            }
            uint64_t waitEnd = deadline;
            if (retries < 2 && nextRetry < waitEnd) waitEnd = nextRetry;
            int remain = (int)(waitEnd - now);
            if (remain <= 0) continue;
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(s, &fds);
            timeval tv{remain / 1000, (remain % 1000) * 1000};
            int r = select(0, &fds, nullptr, nullptr, &tv);
            if (r <= 0) continue;
            sockaddr_storage from{};
            int fl = sizeof(from);
            int n = recvfrom(s, (char *)buf, sizeof(buf), 0, (sockaddr *)&from, &fl);
            if (n < 12) continue;
            if (dnsGetId(buf, (size_t)n) != wantId) continue;
            if (!sameEndpoint((sockaddr *)&from, (const sockaddr *)&ss)) continue;
            out.assign(buf, buf + n);
            ok = true;
            break;
        }
    }
    closesocket(s);
    return ok;
}

static bool tcpQuery(const sockaddr_storage &ss, socklen_t slen, const std::vector<uint8_t> &q,
                     uint16_t wantId, int timeoutMs, std::vector<uint8_t> &out) {
    SOCKET s = socket(((const sockaddr *)&ss)->sa_family, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return false;
    u_long nonBlocking = 1;
    ioctlsocket(s, FIONBIO, &nonBlocking);
    bool connected = false;
    int rc = connect(s, (const sockaddr *)&ss, slen);
    if (rc == 0) {
        connected = true;
    } else if (WSAGetLastError() == WSAEWOULDBLOCK) {
        fd_set wfds, efds;
        FD_ZERO(&wfds);
        FD_ZERO(&efds);
        FD_SET(s, &wfds);
        FD_SET(s, &efds);
        timeval tv{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
        if (select(0, nullptr, &wfds, &efds, &tv) > 0) {
            int err = 0;
            int len = sizeof(err);
            getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&err, &len);
            connected = (err == 0);
        }
    }
    if (!connected) {
        closesocket(s);
        return false;
    }
    nonBlocking = 0;
    ioctlsocket(s, FIONBIO, &nonBlocking);
    DWORD tv = (DWORD)timeoutMs;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&tv, sizeof(tv));

    bool ok = false;
    uint8_t lenbuf[2] = {(uint8_t)(q.size() >> 8), (uint8_t)(q.size() & 0xFF)};
    if (sendAll(s, lenbuf, 2) && sendAll(s, q.data(), q.size())) {
        uint8_t rlen[2];
        if (recvAll(s, rlen, 2)) {
            size_t want = ((size_t)rlen[0] << 8) | rlen[1];
            if (want >= 12 && want <= 65535) {
                out.resize(want);
                if (recvAll(s, out.data(), want) && dnsGetId(out.data(), out.size()) == wantId) ok = true;
            }
        }
    }
    closesocket(s);
    return ok;
}

static std::vector<uint8_t> buildQuery(const std::string &name, uint16_t qtype, uint16_t id) {
    std::vector<uint8_t> q(12, 0);
    put16(q, 0, id);
    q[2] = 0x01;  // RD
    q[3] = 0x00;
    put16(q, 4, 1);
    std::string label;
    for (size_t i = 0; i <= name.size(); i++) {
        if (i == name.size() || name[i] == '.') {
            if (label.empty()) { label.clear(); continue; }
            if (label.size() > 63) return {};
            q.push_back((uint8_t)label.size());
            q.insert(q.end(), label.begin(), label.end());
            label.clear();
        } else {
            label += name[i];
        }
    }
    q.push_back(0);
    q.push_back((uint8_t)(qtype >> 8));
    q.push_back((uint8_t)qtype);
    q.push_back(0);
    q.push_back(1);
    return q;
}

static std::vector<uint8_t> buildError(const std::vector<uint8_t> &query, uint16_t rcode) {
    std::vector<uint8_t> e(12, 0);
    if (query.size() >= 12) {
        e = query;
        size_t off = 12;
        uint16_t qd = (uint16_t)((query[4] << 8) | query[5]);
        for (uint16_t i = 0; i < qd; i++) {
            bool done = false;
            while (off < query.size()) {
                uint8_t len = query[off];
                if (len == 0) { off++; done = true; break; }
                if ((len & 0xC0) == 0xC0) { off += 2; done = true; break; }
                if (len & 0xC0) { done = false; break; }
                off += 1 + len;
                if (off > query.size()) { done = false; break; }
            }
            if (!done || off + 4 > query.size()) { e.resize(12); break; }
            off += 4;
        }
        if (e.size() > 12 && off <= e.size()) e.resize(off);
        e[2] = (uint8_t)((e[2] & 0x01) | 0x80);
        e[3] = (uint8_t)((e[3] & 0xF0) | (rcode & 0x0F));
        put16(e, 6, 0);
        put16(e, 8, 0);
        put16(e, 10, 0);
        if (e.size() == 12) put16(e, 4, 0);
    } else {
        e[2] = 0x80;
        e[3] = (uint8_t)(rcode & 0x0F);
    }
    return e;
}

static std::string shortError(int code) {
    switch (code) {
        case 10048: return "端口已被占用";
        case 10013: return "端口被独占访问或权限不足";
        case 10049: return "监听地址不可用";
        case 10038: return "套接字已关闭";
        case 10061: return "目标拒绝连接";
        default: return "系统错误码 " + std::to_string(code);
    }
}

Engine::Engine(ConfigStore &cfg, Stats &stats, ProcTracker &proc, DnsCache &cache)
    : cfg_(cfg), stats_(stats), proc_(proc), cache_(cache) {}

Engine::~Engine() { stop(); }

bool Engine::start(std::string &err) {
    if (running_) return true;
    ResolverCfg rc = cfg_.resolver();
    port_ = (uint16_t)rc.port;
    listen_ = rc.listen;
    cache_.configure(rc.cacheEnabled, rc.minTtl, rc.maxTtl);

    sockaddr_storage ss{};
    socklen_t slen = 0;
    if (!util::makeSockaddr(listen_, port_, ss, slen)) {
        err = "监听地址无效：" + listen_;
        return false;
    }
    udp4_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (udp4_ == INVALID_SOCKET) {
        err = "创建 UDP 套接字失败：" + shortError(WSAGetLastError());
        return false;
    }
    // 提权重启时会短暂与新实例争用端口，重试 5 秒
    uint64_t bindDeadline = nowMs() + 5000;
    for (;;) {
        if (bind(udp4_, (sockaddr *)&ss, slen) == 0) break;
        int code = WSAGetLastError();
        closesocket(udp4_);
        udp4_ = INVALID_SOCKET;
        if (code == 10048 && nowMs() < bindDeadline) {
            Sleep(200);
            udp4_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (udp4_ == INVALID_SOCKET) break;
            continue;
        }
        err = "UDP 端口 " + std::to_string(port_) + " 绑定失败：" + shortError(code);
        if (code == 10048) err += "（请关闭占用该端口的程序，或使用 --port 指定其他端口）";
        return false;
    }
    if (udp4_ == INVALID_SOCKET) {
        err = "创建 UDP 套接字失败";
        return false;
    }
    tcp4_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (tcp4_ == INVALID_SOCKET || bind(tcp4_, (sockaddr *)&ss, slen) != 0 ||
        listen(tcp4_, SOMAXCONN) != 0) {
        int code = WSAGetLastError();
        err = "TCP 端口 " + std::to_string(port_) + " 监听失败：" + shortError(code);
        if (tcp4_ != INVALID_SOCKET) closesocket(tcp4_);
        tcp4_ = INVALID_SOCKET;
        closesocket(udp4_);
        udp4_ = INVALID_SOCKET;
        return false;
    }

    // IPv6 环回尽力监听，失败不影响主链路
    sockaddr_in6 v6{};
    v6.sin6_family = AF_INET6;
    v6.sin6_port = htons(port_);
    InetPtonA(AF_INET6, "::1", &v6.sin6_addr);
    udp6_ = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
    if (udp6_ != INVALID_SOCKET) {
        int only = 1;
        setsockopt(udp6_, IPPROTO_IPV6, IPV6_V6ONLY, (const char *)&only, sizeof(only));
        if (bind(udp6_, (sockaddr *)&v6, sizeof(v6)) != 0) {
            closesocket(udp6_);
            udp6_ = INVALID_SOCKET;
        }
    }
    if (udp6_ != INVALID_SOCKET) {
        tcp6_ = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
        if (tcp6_ != INVALID_SOCKET) {
            int only = 1;
            setsockopt(tcp6_, IPPROTO_IPV6, IPV6_V6ONLY, (const char *)&only, sizeof(only));
            if (bind(tcp6_, (sockaddr *)&v6, sizeof(v6)) != 0 || listen(tcp6_, SOMAXCONN) != 0) {
                closesocket(tcp6_);
                tcp6_ = INVALID_SOCKET;
            }
        }
    }

    running_ = true;
    pool_.start(16);
    threads_.emplace_back([this] { udpLoop(udp4_, AF_INET); });
    threads_.emplace_back([this] { tcpLoop(tcp4_, AF_INET); });
    if (udp6_ != INVALID_SOCKET) threads_.emplace_back([this] { udpLoop(udp6_, AF_INET6); });
    if (tcp6_ != INVALID_SOCKET) threads_.emplace_back([this] { tcpLoop(tcp6_, AF_INET6); });

    util::logInfo("解析器已监听 " + listen_ + ":" + std::to_string(port_) + " (UDP/TCP" +
                  (udp6_ != INVALID_SOCKET ? "，含 ::1" : "") + ")");
    return true;
}

void Engine::stop() {
    if (!running_.exchange(false)) return;
    if (udp4_ != INVALID_SOCKET) { closesocket(udp4_); udp4_ = INVALID_SOCKET; }
    if (udp6_ != INVALID_SOCKET) { closesocket(udp6_); udp6_ = INVALID_SOCKET; }
    if (tcp4_ != INVALID_SOCKET) { closesocket(tcp4_); tcp4_ = INVALID_SOCKET; }
    if (tcp6_ != INVALID_SOCKET) { closesocket(tcp6_); tcp6_ = INVALID_SOCKET; }
    for (auto &t : threads_)
        if (t.joinable()) t.join();
    threads_.clear();
    pool_.stop();
    util::logInfo("解析器已停止");
}

bool Engine::ready() const { return running_; }

void Engine::flushCache() { cache_.clear(); }

void Engine::applyRuntimeSettings() {
    ResolverCfg rc = cfg_.resolver();
    cache_.configure(rc.cacheEnabled, rc.minTtl, rc.maxTtl);
}

void Engine::udpLoop(SOCKET s, int family) {
    std::vector<uint8_t> buf(65535);
    while (running_) {
        sockaddr_storage from{};
        int fl = sizeof(from);
        int n = recvfrom(s, (char *)buf.data(), (int)buf.size(), 0, (sockaddr *)&from, &fl);
        if (n <= 0) {
            if (!running_) break;
            continue;
        }
        std::vector<uint8_t> query(buf.begin(), buf.begin() + n);
        uint16_t srcPort = util::sockaddrPort((sockaddr *)&from);
        pool_.post([this, s, query, from, fl, srcPort, family]() {
            ProcInfo pi = proc_.udpOwner(srcPort, family);
            auto resp = handleQuery(query, pi.exe, pi.pid, "dns");
            if (!resp.empty()) sendto(s, (const char *)resp.data(), (int)resp.size(), 0, (const sockaddr *)&from, fl);
        });
    }
}

void Engine::tcpLoop(SOCKET s, int family) {
    (void)family;
    while (running_) {
        sockaddr_storage from{};
        int fl = sizeof(from);
        SOCKET c = accept(s, (sockaddr *)&from, &fl);
        if (c == INVALID_SOCKET) {
            if (!running_) break;
            continue;
        }
        uint16_t peerPort = util::sockaddrPort((sockaddr *)&from);
        int fam = ((sockaddr *)&from)->sa_family;
        pool_.post([this, c, fam, peerPort]() { handleTcpClient(c, fam); (void)peerPort; });
    }
}

void Engine::handleTcpClient(SOCKET client, int family) {
    DWORD tv = 5000;
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
    setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, (const char *)&tv, sizeof(tv));
    uint8_t lenbuf[2];
    std::vector<uint8_t> query;
    if (recvAll(client, lenbuf, 2)) {
        size_t want = ((size_t)lenbuf[0] << 8) | lenbuf[1];
        if (want >= 12 && want <= 65535) {
            query.resize(want);
            if (!recvAll(client, query.data(), want)) query.clear();
        }
    }
    if (!query.empty()) {
        sockaddr_storage peer{};
        int pl = sizeof(peer);
        uint16_t peerPort = 0;
        int fam = family;
        if (getpeername(client, (sockaddr *)&peer, &pl) == 0) {
            peerPort = util::sockaddrPort((sockaddr *)&peer);
            fam = ((sockaddr *)&peer)->sa_family;
        }
        ProcInfo pi = proc_.tcpOwner(peerPort, fam);
        auto resp = handleQuery(query, pi.exe, pi.pid, "dns");
        if (!resp.empty()) {
            uint8_t l[2] = {(uint8_t)(resp.size() >> 8), (uint8_t)(resp.size() & 0xFF)};
            if (sendAll(client, l, 2)) sendAll(client, resp.data(), resp.size());
        }
    }
    shutdown(client, SD_BOTH);
    closesocket(client);
}

std::string Engine::profileNameOf(const std::string &id) const {
    Profile p;
    return cfg_.profileById(id, p) ? p.name : id;
}

void Engine::markServer(const std::string &server, bool ok) {
    std::lock_guard<std::mutex> g(healthMu_);
    if (ok) {
        health_.erase(server);
        return;
    }
    auto &h = health_[server];
    h.fails++;
    uint64_t penalty = (uint64_t)std::min(30000, 5000 * h.fails);
    h.retryAt = util::nowMs() + penalty;
}

std::vector<std::string> Engine::orderedServers(const std::vector<std::string> &servers) {
    std::vector<std::string> out = servers;
    std::lock_guard<std::mutex> g(healthMu_);
    uint64_t now = util::nowMs();
    std::stable_partition(out.begin(), out.end(), [&](const std::string &s) {
        auto it = health_.find(s);
        return it == health_.end() || it->second.retryAt <= now;
    });
    return out;
}

void Engine::emitDnsEvent(const DnsQuestion &q, const ProfileChoice &choice, const ResolveResult &r,
                          const std::string &exe, uint32_t pid) {
    Event e;
    e.t = nowMs();
    e.source = "dns";
    e.client = exe.empty() ? "未知" : exe;
    e.pid = pid;
    e.domain = q.name;
    e.qtype = q.qtype;
    e.profileId = r.profileId.empty() ? choice.profileId : r.profileId;
    e.profileName = r.profileName.empty() ? profileNameOf(e.profileId) : r.profileName;
    e.upstream = r.upstream;
    e.ruleType = choice.ruleType;
    e.ruleMatch = choice.ruleMatch;
    e.status = r.status;
    e.latencyMs = r.latencyMs;
    e.cached = r.cached;
    stats_.add(e);
    util::logDebug(std::string("DNS ") + e.client + " " + q.name + " " + dnsTypeName(q.qtype) + " -> " +
                   (r.cached ? "cache" : r.upstream) + " " + r.status + " " +
                   std::to_string((int)r.latencyMs) + "ms");
}

std::vector<uint8_t> Engine::handleQuery(const std::vector<uint8_t> &query, const std::string &clientExe,
                                         uint32_t pid, const char *source) {
    (void)source;
    if (query.size() < 12) {
        if (query.size() < 2) return {};
        // 报文不完整但能取到 ID：回 FORMERR，便于客户端快速失败
        std::vector<uint8_t> e(12, 0);
        e[0] = query[0];
        e[1] = query[1];
        e[2] = 0x80;
        e[3] = 0x01;
        return e;
    }
    if (query[2] & 0x80) return {};  // 响应包，忽略以防环
    DnsQuestion q;
    if (!dnsParseQuestion(query.data(), query.size(), q)) return buildError(query, 1);

    ProfileChoice choice = cfg_.chooseProfile(q.name, clientExe);
    ResolveResult r = lookupPacket(query, choice.profileId, true);
    if (r.pkt.empty()) {
        r.profileId = choice.profileId;
        r.profileName = profileNameOf(choice.profileId);
        r.status = r.status.empty() ? "servfail" : r.status;
        emitDnsEvent(q, choice, r, clientExe, pid);
        return buildError(query, 2);
    }
    auto resp = r.pkt;
    dnsSetId(resp.data(), resp.size(), dnsGetId(query.data(), query.size()));
    emitDnsEvent(q, choice, r, clientExe, pid);
    return resp;
}

UpstreamResult Engine::forward(const std::vector<uint8_t> &query, const Profile &p, int timeoutMs) {
    UpstreamResult out;
    std::vector<std::string> servers = p.servers;
    if (p.strategy == "only" && servers.size() > 1) servers.resize(1);
    if (servers.empty()) { out.status = "error"; return out; }

    std::vector<uint8_t> lastPkt;
    std::string lastServer, lastStatus = "timeout";
    for (auto &s : orderedServers(servers)) {
        std::string host;
        uint16_t port = 53;
        if (!util::parseServer(s, host, port)) continue;
        if (util::isLoopbackHost(host) && port == port_) continue;
        sockaddr_storage ss{};
        socklen_t slen = 0;
        if (!util::makeSockaddr(host, port, ss, slen)) continue;

        std::vector<uint8_t> q2 = query;
        uint16_t id = util::randU16();
        dnsSetId(q2.data(), q2.size(), id);
        std::vector<uint8_t> resp;
        bool got = udpQuery(ss, slen, q2, id, timeoutMs, resp);
        DnsMeta meta{};
        bool valid = got && dnsAnalyze(resp.data(), resp.size(), meta) && meta.isResponse;
        if (valid && meta.truncated) {
            std::vector<uint8_t> resp2;
            if (tcpQuery(ss, slen, q2, id, timeoutMs, resp2)) {
                DnsMeta meta2{};
                if (dnsAnalyze(resp2.data(), resp2.size(), meta2) && meta2.isResponse && !meta2.truncated) {
                    resp = std::move(resp2);
                    meta = meta2;
                }
            }
        }
        if (valid) {
            if (meta.rcode == 0 || meta.rcode == 3) {
                markServer(s, true);
                out.ok = true;
                out.pkt = std::move(resp);
                out.server = s;
                out.status = rcodeStatus(meta.rcode);
                out.rcode = meta.rcode;
                return out;
            }
            markServer(s, false);
            lastPkt = std::move(resp);
            lastServer = s;
            lastStatus = rcodeStatus(meta.rcode);
            continue;
        }
        markServer(s, false);
        lastServer = s;
        lastStatus = "timeout";
    }
    out.ok = false;
    out.server = lastServer;
    out.status = lastStatus;
    out.pkt = std::move(lastPkt);
    return out;
}

ResolveResult Engine::lookupPacket(const std::vector<uint8_t> &query, const std::string &profileId,
                                   bool useCache) {
    ResolveResult out;
    DnsQuestion q;
    if (!dnsParseQuestion(query.data(), query.size(), q)) { out.status = "error"; return out; }
    out.qtype = q.qtype;

    Profile p;
    if (!cfg_.profileById(profileId, p)) {
        Profile p2;
        std::string def = cfg_.defaultProfile();
        if (!cfg_.profileById(def, p2)) { out.status = "error"; return out; }
        p = p2;
    }
    out.profileId = p.id;
    out.profileName = p.name;

    bool cacheOn = cfg_.cacheEnabled();
    std::string key = util::lowerAscii(q.name) + "|" + std::to_string(q.qtype) + "|" +
                      std::to_string(q.qclass) + "|" + p.id;
    if (useCache && cacheOn) {
        std::vector<uint8_t> cached;
        uint32_t age = 0;
        if (cache_.get(key, cached, age)) {
            out.ok = true;
            out.cached = true;
            out.pkt = std::move(cached);
            out.upstream = "cache";
            DnsMeta meta{};
            if (dnsAnalyze(out.pkt.data(), out.pkt.size(), meta)) {
                out.rcode = meta.rcode;
                out.status = rcodeStatus(meta.rcode);
            } else out.status = "error";
            dnsParseAnswerIps(out.pkt.data(), out.pkt.size(), out.ips);
            return out;
        }
    }

    uint64_t t0 = nowMs();
    UpstreamResult up = forward(query, p, cfg_.resolver().queryTimeoutMs);
    out.latencyMs = (double)(nowMs() - t0);
    out.upstream = up.server;
    if (!up.pkt.empty()) {
        out.ok = true;
        out.pkt = std::move(up.pkt);
        DnsMeta meta{};
        if (dnsAnalyze(out.pkt.data(), out.pkt.size(), meta)) {
            out.rcode = meta.rcode;
            out.status = rcodeStatus(meta.rcode);
            if ((meta.rcode == 0 || meta.rcode == 3) && !meta.truncated && useCache && cacheOn)
                cache_.put(key, out.pkt, meta.minTtl);
        } else {
            out.status = "error";
        }
        dnsParseAnswerIps(out.pkt.data(), out.pkt.size(), out.ips);
    } else {
        out.status = up.status.empty() ? "timeout" : up.status;
    }
    return out;
}

ResolveResult Engine::lookupName(const std::string &domain, uint16_t qtype, const std::string &profileId,
                                 bool useCache) {
    auto q = buildQuery(domain, qtype, util::randU16());
    if (q.empty()) {
        ResolveResult r;
        r.status = "error";
        return r;
    }
    auto out = lookupPacket(q, profileId, useCache);
    out.qtype = qtype;
    return out;
}

ResolveResult Engine::resolveTest(const std::string &domain, const std::string &profileId) {
    std::string pid = profileId;
    Profile tmp;
    if (pid.empty() || !cfg_.profileById(pid, tmp)) pid = cfg_.defaultProfile();
    return lookupName(domain, 1, pid, false);
}

ResolveResult Engine::resolveForHost(const std::string &host, const std::string &exe, uint32_t pid,
                                     uint16_t qtype) {
    ProfileChoice choice = cfg_.chooseProfile(host, exe);
    ResolveResult out;
    sockaddr_storage ss{};
    socklen_t slen = 0;
    if (util::makeSockaddr(host, 0, ss, slen)) {
        out.ok = true;
        out.status = "ok";
        out.ips.push_back(host);
        out.profileId = choice.profileId;
        out.profileName = profileNameOf(choice.profileId);
        out.ruleType = choice.ruleType;
        out.ruleMatch = choice.ruleMatch;
        return out;
    }
    out = lookupName(host, qtype, choice.profileId, true);
    // 只有 AAAA 记录的站点：A 查询为空时自动补一次 AAAA
    if (out.status == "ok" && out.ips.empty() && qtype == 1) {
        auto out6 = lookupName(host, 28, choice.profileId, true);
        if (out6.status == "ok" && !out6.ips.empty()) {
            out6.ruleType = choice.ruleType;
            out6.ruleMatch = choice.ruleMatch;
            out = out6;
        }
    }
    out.ruleType = choice.ruleType;
    out.ruleMatch = choice.ruleMatch;
    if (out.profileName.empty()) out.profileName = profileNameOf(choice.profileId);

    Event e;
    e.t = nowMs();
    e.source = "proxy";
    e.client = exe.empty() ? "未知" : exe;
    e.pid = pid;
    e.domain = host;
    e.qtype = qtype;
    e.profileId = out.profileId;
    e.profileName = out.profileName;
    e.upstream = out.upstream;
    e.ruleType = choice.ruleType;
    e.ruleMatch = choice.ruleMatch;
    e.status = out.status;
    e.latencyMs = out.latencyMs;
    e.cached = out.cached;
    stats_.add(e);
    return out;
}

bool Engine::selfTest(std::string &err) {
    auto q = buildQuery("example.com", 1, util::randU16());
    if (q.empty()) { err = "无法构造探测报文"; return false; }
    sockaddr_storage ss{};
    socklen_t slen = 0;
    if (!util::makeSockaddr(listen_, port_, ss, slen)) { err = "监听地址无效"; return false; }
    std::vector<uint8_t> resp;
    if (!udpQuery(ss, slen, q, dnsGetId(q.data(), q.size()), 4000, resp)) {
        err = "本机解析器无响应（端口 " + std::to_string(port_) + "）";
        return false;
    }
    DnsMeta meta{};
    if (!dnsAnalyze(resp.data(), resp.size(), meta) || !meta.isResponse) {
        err = "本机解析器返回了无效响应";
        return false;
    }
    if (meta.rcode != 0 && meta.rcode != 3) {
        err = "上游解析失败（rcode=" + std::to_string(meta.rcode) + "），请检查 DNS 配置或网络";
        return false;
    }
    return true;
}
