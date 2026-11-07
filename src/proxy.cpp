#include "proxy.h"

#include <algorithm>
#include <cstring>
#include <vector>

static bool pSendAll(SOCKET s, const void *data, size_t len) {
    const char *p = (const char *)data;
    size_t sent = 0;
    while (sent < len) {
        int n = send(s, p + sent, (int)(len - sent), 0);
        if (n <= 0) return false;
        sent += (size_t)n;
    }
    return true;
}

static bool pRecvAll(SOCKET s, void *data, size_t len) {
    char *p = (char *)data;
    size_t got = 0;
    while (got < len) {
        int n = recv(s, p + got, (int)(len - got), 0);
        if (n <= 0) return false;
        got += (size_t)n;
    }
    return true;
}

static bool pump(SOCKET from, SOCKET to) {
    std::vector<char> buf(32768);
    for (;;) {
        int n = recv(from, buf.data(), (int)buf.size(), 0);
        if (n <= 0) break;
        if (!pSendAll(to, buf.data(), (size_t)n)) break;
    }
    shutdown(to, SD_SEND);
    return true;
}

static void relay(SOCKET a, SOCKET b) {
    std::thread t([a, b] { pump(a, b); });
    pump(b, a);
    t.join();
}

static SOCKET connectWithTimeout(const std::string &ip, uint16_t port, int timeoutMs) {
    sockaddr_storage ss{};
    socklen_t slen = 0;
    if (!util::makeSockaddr(ip, port, ss, slen)) return INVALID_SOCKET;
    SOCKET s = socket(((sockaddr *)&ss)->sa_family, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return INVALID_SOCKET;

    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
    bool ok = false;
    int rc = connect(s, (sockaddr *)&ss, slen);
    if (rc == 0) {
        ok = true;
    } else if (WSAGetLastError() == WSAEWOULDBLOCK) {
        fd_set wfds, efds;
        FD_ZERO(&wfds);
        FD_ZERO(&efds);
        FD_SET(s, &wfds);
        FD_SET(s, &efds);
        timeval tv{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
        if (select(0, nullptr, &wfds, &efds, &tv) > 0) {
            int err = 0, len = sizeof(err);
            getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&err, &len);
            ok = (err == 0);
        }
    }
    if (!ok) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    nb = 0;
    ioctlsocket(s, FIONBIO, &nb);
    int nodelay = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&nodelay, sizeof(nodelay));
    return s;
}

static void socks5Reply(SOCKET c, uint8_t rep, SOCKET out) {
    uint8_t r[22] = {5, rep, 0, 1, 0, 0, 0, 0, 0, 0};
    size_t len = 10;
    if (out != INVALID_SOCKET) {
        sockaddr_storage local{};
        int ll = sizeof(local);
        if (getsockname(out, (sockaddr *)&local, &ll) == 0) {
            if (local.ss_family == AF_INET) {
                r[3] = 1;
                memcpy(r + 4, &((sockaddr_in *)&local)->sin_addr, 4);
                memcpy(r + 8, &((sockaddr_in *)&local)->sin_port, 2);
            } else if (local.ss_family == AF_INET6) {
                r[3] = 4;
                memcpy(r + 4, &((sockaddr_in6 *)&local)->sin6_addr, 16);
                memcpy(r + 20, &((sockaddr_in6 *)&local)->sin6_port, 2);
                len = 22;
            }
        }
    }
    pSendAll(c, r, len);
}

Proxy::Proxy(Engine &engine, ConfigStore &cfg, ProcTracker &proc)
    : engine_(engine), cfg_(cfg), proc_(proc) {}

bool Proxy::start(std::string &err) {
    if (running_) return true;
    ProxyCfg pc = cfg_.proxy();
    port_ = (uint16_t)pc.port;

    sockaddr_in a4{};
    a4.sin_family = AF_INET;
    a4.sin_port = htons(port_);
    InetPtonA(AF_INET, "127.0.0.1", &a4.sin_addr);

    uint64_t deadline = util::nowMs() + 5000;
    for (;;) {
        listen4_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listen4_ == INVALID_SOCKET) {
            err = "创建代理套接字失败";
            return false;
        }
        if (bind(listen4_, (sockaddr *)&a4, sizeof(a4)) == 0 && listen(listen4_, SOMAXCONN) == 0) break;
        int code = WSAGetLastError();
        closesocket(listen4_);
        listen4_ = INVALID_SOCKET;
        if (code == 10048 && util::nowMs() < deadline) {
            Sleep(200);
            continue;
        }
        err = "代理端口 " + std::to_string(port_) + " 绑定失败（错误码 " + std::to_string(code) + "）";
        return false;
    }

    sockaddr_in6 a6{};
    a6.sin6_family = AF_INET6;
    a6.sin6_port = htons(port_);
    InetPtonA(AF_INET6, "::1", &a6.sin6_addr);
    listen6_ = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
    if (listen6_ != INVALID_SOCKET) {
        int only = 1;
        setsockopt(listen6_, IPPROTO_IPV6, IPV6_V6ONLY, (const char *)&only, sizeof(only));
        if (bind(listen6_, (sockaddr *)&a6, sizeof(a6)) != 0 || listen(listen6_, SOMAXCONN) != 0) {
            closesocket(listen6_);
            listen6_ = INVALID_SOCKET;
        }
    }

    running_ = true;
    thread4_ = std::thread([this] { acceptLoop(listen4_); });
    if (listen6_ != INVALID_SOCKET) thread6_ = std::thread([this] { acceptLoop(listen6_); });
    util::logInfo("应用代理已监听 127.0.0.1:" + std::to_string(port_) + " (SOCKS5 / HTTP CONNECT)");
    return true;
}

void Proxy::stop() {
    if (!running_.exchange(false)) return;
    if (listen4_ != INVALID_SOCKET) { closesocket(listen4_); listen4_ = INVALID_SOCKET; }
    if (listen6_ != INVALID_SOCKET) { closesocket(listen6_); listen6_ = INVALID_SOCKET; }
    if (thread4_.joinable()) thread4_.join();
    if (thread6_.joinable()) thread6_.join();
    util::logInfo("应用代理已停止");
}

void Proxy::acceptLoop(SOCKET listenSock) {
    while (running_) {
        sockaddr_storage from{};
        int fl = sizeof(from);
        SOCKET c = accept(listenSock, (sockaddr *)&from, &fl);
        if (c == INVALID_SOCKET) {
            if (!running_) break;
            continue;
        }
        uint16_t peerPort = util::sockaddrPort((sockaddr *)&from);
        int fam = ((sockaddr *)&from)->sa_family;
        std::thread([this, c, fam, peerPort] { handleClient(c, fam, peerPort); }).detach();
    }
}

void Proxy::handleClient(SOCKET client, int family, uint16_t peerPort) {
    DWORD tv = 10000;
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
    setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, (const char *)&tv, sizeof(tv));

    ProcInfo pi = proc_.tcpOwner(peerPort, family);
    uint8_t peek[3] = {0, 0, 0};
    int n = recv(client, (char *)peek, 3, MSG_PEEK);
    bool handled = false;
    if (n >= 1 && peek[0] == 0x05) {
        handled = doSocks5(client, pi.exe, pi.pid);
    } else if (n >= 3 && memcmp(peek, "CON", 3) == 0) {
        handled = doHttpConnect(client, pi.exe, pi.pid);
    } else if (n >= 1) {
        const char *msg = "HTTP/1.1 405 Method Not Allowed\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        pSendAll(client, msg, strlen(msg));
    }
    (void)handled;
    shutdown(client, SD_BOTH);
    closesocket(client);
}

bool Proxy::doSocks5(SOCKET client, const std::string &exe, uint32_t pid) {
    uint8_t hdr[2];
    if (!pRecvAll(client, hdr, 2) || hdr[0] != 5) return false;
    std::vector<uint8_t> methods(hdr[1]);
    if (!hdr[1] || !pRecvAll(client, methods.data(), methods.size())) return false;
    uint8_t sel[2] = {5, 0};  // 无认证
    if (!pSendAll(client, sel, 2)) return false;

    uint8_t req[4];
    if (!pRecvAll(client, req, 4) || req[0] != 5) return false;
    if (req[1] != 1) {  // 只支持 CONNECT
        socks5Reply(client, 7, INVALID_SOCKET);
        return false;
    }
    std::string host;
    if (req[3] == 1) {
        uint8_t ip[4];
        if (!pRecvAll(client, ip, 4)) return false;
        sockaddr_in sa{};
        sa.sin_family = AF_INET;
        memcpy(&sa.sin_addr, ip, 4);
        char buf[INET_ADDRSTRLEN];
        InetNtopA(AF_INET, &sa.sin_addr, buf, sizeof(buf));
        host = buf;
    } else if (req[3] == 3) {
        uint8_t len = 0;
        if (!pRecvAll(client, &len, 1) || len == 0) return false;
        std::vector<char> name(len);
        if (!pRecvAll(client, name.data(), len)) return false;
        host.assign(name.begin(), name.end());
    } else if (req[3] == 4) {
        uint8_t ip[16];
        if (!pRecvAll(client, ip, 16)) return false;
        sockaddr_in6 sa{};
        sa.sin6_family = AF_INET6;
        memcpy(&sa.sin6_addr, ip, 16);
        char buf[INET6_ADDRSTRLEN];
        InetNtopA(AF_INET6, &sa.sin6_addr, buf, sizeof(buf));
        host = buf;
    } else {
        socks5Reply(client, 8, INVALID_SOCKET);
        return false;
    }
    uint8_t pbuf[2];
    if (!pRecvAll(client, pbuf, 2)) return false;
    uint16_t port = (uint16_t)((pbuf[0] << 8) | pbuf[1]);
    if (port == 0) {
        socks5Reply(client, 1, INVALID_SOCKET);
        return false;
    }

    std::vector<std::string> ips;
    sockaddr_storage tmp{};
    socklen_t tmplen = 0;
    if (util::makeSockaddr(host, 0, tmp, tmplen)) {
        ips.push_back(host);
    } else {
        auto r = engine_.resolveForHost(host, exe, pid, 1);
        if (r.status != "ok" || r.ips.empty()) {
            util::logInfo("代理解析失败 " + exe + " -> " + host + " (" + r.status + ")");
            socks5Reply(client, 4, INVALID_SOCKET);
            return false;
        }
        ips = r.ips;
    }

    SOCKET remote = INVALID_SOCKET;
    for (auto &ip : ips) {
        remote = connectWithTimeout(ip, port, 5000);
        if (remote != INVALID_SOCKET) break;
    }
    if (remote == INVALID_SOCKET) {
        socks5Reply(client, 5, INVALID_SOCKET);
        return false;
    }
    socks5Reply(client, 0, remote);
    util::logDebug("SOCKS5 " + exe + " -> " + host + ":" + std::to_string(port));
    relay(client, remote);
    closesocket(remote);
    return true;
}

bool Proxy::doHttpConnect(SOCKET client, const std::string &exe, uint32_t pid) {
    std::string req;
    char buf[2048];
    while (req.find("\r\n\r\n") == std::string::npos && req.size() < 16384) {
        int n = recv(client, buf, sizeof(buf), 0);
        if (n <= 0) return false;
        req.append(buf, (size_t)n);
    }
    size_t lineEnd = req.find("\r\n");
    std::string first = lineEnd == std::string::npos ? req : req.substr(0, lineEnd);
    // CONNECT host:port HTTP/1.1
    if (first.size() < 12 || first.compare(0, 8, "CONNECT ") != 0) {
        const char *msg =
            "HTTP/1.1 501 Not Implemented\r\nContent-Type: text/plain; charset=utf-8\r\n"
            "Content-Length: 96\r\nConnection: close\r\n\r\n"
            "DNS Pilot: 仅支持 CONNECT 隧道；明文 HTTP 请使用 SOCKS5（--proxy socks5h://127.0.0.1:8899）\n";
        pSendAll(client, msg, strlen(msg));
        return false;
    }
    size_t hostStart = 8;
    size_t hostEnd = first.find(' ', hostStart);
    std::string target = first.substr(hostStart, hostEnd == std::string::npos ? std::string::npos
                                                                             : hostEnd - hostStart);
    std::string host;
    uint16_t port = 443;
    if (!target.empty() && target[0] == '[') {
        size_t close = target.find(']');
        if (close == std::string::npos) return false;
        host = target.substr(1, close - 1);
        if (close + 1 < target.size() && target[close + 1] == ':')
            port = (uint16_t)atoi(target.c_str() + close + 2);
    } else {
        size_t colon = target.find_last_of(':');
        if (colon != std::string::npos) {
            host = target.substr(0, colon);
            port = (uint16_t)atoi(target.c_str() + colon + 1);
        } else host = target;
    }
    if (host.empty() || port == 0) {
        const char *msg = "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        pSendAll(client, msg, strlen(msg));
        return false;
    }

    std::vector<std::string> ips;
    sockaddr_storage tmp{};
    socklen_t tmplen = 0;
    if (util::makeSockaddr(host, 0, tmp, tmplen)) {
        ips.push_back(host);
    } else {
        auto r = engine_.resolveForHost(host, exe, pid, 1);
        if (r.status != "ok" || r.ips.empty()) {
            const char *msg =
                "HTTP/1.1 502 Bad Gateway\r\nContent-Type: text/plain; charset=utf-8\r\n"
                "Content-Length: 24\r\nConnection: close\r\n\r\nDNS Pilot: 域名解析失败\n";
            pSendAll(client, msg, strlen(msg));
            return false;
        }
        ips = r.ips;
    }

    SOCKET remote = INVALID_SOCKET;
    for (auto &ip : ips) {
        remote = connectWithTimeout(ip, port, 5000);
        if (remote != INVALID_SOCKET) break;
    }
    if (remote == INVALID_SOCKET) {
        const char *msg = "HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        pSendAll(client, msg, strlen(msg));
        return false;
    }
    const char *ok = "HTTP/1.1 200 Connection Established\r\nProxy-Agent: DNS-Pilot\r\n\r\n";
    pSendAll(client, ok, strlen(ok));
    util::logDebug("HTTP " + exe + " -> " + host + ":" + std::to_string(port));
    relay(client, remote);
    closesocket(remote);
    return true;
}
