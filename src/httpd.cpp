#include "httpd.h"

#include <algorithm>
#include <cstring>
#include <vector>

#include "web_asset.h"

static bool hSendAll(SOCKET s, const void *data, size_t len) {
    const char *p = (const char *)data;
    size_t sent = 0;
    while (sent < len) {
        int n = send(s, p + sent, (int)(len - sent), 0);
        if (n <= 0) return false;
        sent += (size_t)n;
    }
    return true;
}

static std::string lower(std::string s) {
    for (auto &c : s)
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return s;
}

static std::string urlDecode(const std::string &s) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '+') out += ' ';
        else if (s[i] == '%' && i + 2 < s.size()) {
            auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            int hi = hex(s[i + 1]), lo = hex(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out += (char)((hi << 4) | lo);
                i += 2;
            } else out += s[i];
        } else out += s[i];
    }
    return out;
}

static void parseQuery(const std::string &qs, std::map<std::string, std::string> &out) {
    size_t pos = 0;
    while (pos < qs.size()) {
        size_t amp = qs.find('&', pos);
        std::string pair = qs.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
        size_t eq = pair.find('=');
        if (eq != std::string::npos)
            out[urlDecode(pair.substr(0, eq))] = urlDecode(pair.substr(eq + 1));
        else if (!pair.empty())
            out[urlDecode(pair)] = "";
        if (amp == std::string::npos) break;
        pos = amp + 1;
    }
}

static const char *reasonPhrase(int status) {
    switch (status) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 413: return "Payload Too Large";
        case 500: return "Internal Server Error";
        default: return "OK";
    }
}

static void sendResponse(SOCKET c, int status, const std::string &ctype, const std::string &body) {
    std::string h = "HTTP/1.1 " + std::to_string(status) + " " + reasonPhrase(status) + "\r\n";
    h += "Content-Type: " + ctype + "\r\n";
    h += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    h += "Cache-Control: no-store\r\n";
    h += "Connection: close\r\n\r\n";
    if (hSendAll(c, h.data(), h.size())) hSendAll(c, body.data(), body.size());
}

static js::Value errJson(const std::string &msg) {
    auto v = js::Value::objV();
    v["ok"] = js::Value::boolV(false);
    v["error"] = js::Value::strV(msg);
    return v;
}

static js::Value okJson() {
    auto v = js::Value::objV();
    v["ok"] = js::Value::boolV(true);
    return v;
}

static bool profileExists(Config &c, const std::string &id) {
    for (auto &p : c.profiles)
        if (p.id == id) return true;
    return false;
}

static bool ruleFromJson(const js::Value &o, Rule &r, Config &c, std::string &err) {
    r.match = util::trim(o.s("match"));
    r.profile = util::trim(o.s("profile"));
    r.note = o.s("note");
    r.enabled = o.bv("enabled", true);
    if (r.match.empty()) { err = "匹配内容不能为空"; return false; }
    if (!profileExists(c, r.profile)) { err = "所选的 DNS 配置不存在"; return false; }
    return true;
}

static bool applyRuleOp(Config &c, std::vector<Rule> &rules, const js::Value &body, std::string &err) {
    std::string op = body.s("op");
    int idx = (int)body.n("index", -1);
    const js::Value *rv = body.get("rule");
    if (op == "add") {
        if (!rv || !rv->isObj()) { err = "缺少规则内容"; return false; }
        Rule r;
        if (!ruleFromJson(*rv, r, c, err)) return false;
        rules.push_back(r);
        return true;
    }
    if (idx < 0 || idx >= (int)rules.size()) { err = "规则索引无效"; return false; }
    if (op == "delete") { rules.erase(rules.begin() + idx); return true; }
    if (op == "toggle") { rules[idx].enabled = !rules[idx].enabled; return true; }
    if (op == "update") {
        if (!rv || !rv->isObj()) { err = "缺少规则内容"; return false; }
        Rule r;
        if (!ruleFromJson(*rv, r, c, err)) return false;
        rules[idx] = r;
        return true;
    }
    err = "未知操作";
    return false;
}

static bool readServers(const js::Value *arr, std::vector<std::string> &out, std::string &err) {
    out.clear();
    if (!arr || !arr->isArr()) { err = "服务器列表无效"; return false; }
    for (auto &x : arr->arr) {
        if (!x.isStr()) continue;
        std::string s = util::trim(x.str);
        if (s.empty()) continue;
        std::string host;
        uint16_t port = 0;
        if (!util::parseServer(s, host, port)) {
            err = "DNS 服务器地址无效：" + s;
            return false;
        }
        if (out.size() < 8) out.push_back(s);
    }
    if (out.empty()) { err = "至少需要一个 DNS 服务器"; return false; }
    return true;
}

static bool applyProfileOp(Config &c, const js::Value &body, std::string &err) {
    std::string op = body.s("op");
    if (op == "add") {
        const js::Value *pv = body.get("profile");
        if (!pv || !pv->isObj()) { err = "缺少配置内容"; return false; }
        Profile p;
        p.id = util::trim(pv->s("id"));
        if (p.id.empty()) p.id = "custom-" + util::randHex(3);
        for (auto &e : c.profiles)
            if (e.id == p.id) { err = "配置 ID 已存在"; return false; }
        p.name = util::trim(pv->s("name"));
        if (p.name.empty()) { err = "配置名称不能为空"; return false; }
        p.provider = "custom";
        p.strategy = pv->s("strategy", "failover") == "only" ? "only" : "failover";
        p.note = pv->s("note");
        if (!readServers(pv->get("servers"), p.servers, err)) return false;
        c.profiles.push_back(p);
        return true;
    }
    std::string id = util::trim(body.s("id"));
    if (op == "delete") {
        if (id == "system") { err = "系统默认配置不能删除"; return false; }
        size_t before = c.profiles.size();
        c.profiles.erase(std::remove_if(c.profiles.begin(), c.profiles.end(),
                                        [&](const Profile &p) { return p.id == id; }),
                         c.profiles.end());
        if (c.profiles.size() == before) { err = "配置不存在"; return false; }
        if (c.defaultProfile == id) c.defaultProfile = "system";
        for (auto &r : c.appRules)
            if (r.profile == id) r.profile = c.defaultProfile;
        for (auto &r : c.domainRules)
            if (r.profile == id) r.profile = c.defaultProfile;
        return true;
    }
    if (op == "update") {
        const js::Value *pv = body.get("profile");
        if (!pv || !pv->isObj()) { err = "缺少配置内容"; return false; }
        if (id == "system") { err = "系统默认配置由程序自动维护"; return false; }
        for (auto &p : c.profiles) {
            if (p.id != id) continue;
            std::string name = util::trim(pv->s("name"));
            if (!name.empty()) p.name = name;
            p.strategy = pv->s("strategy", p.strategy) == "only" ? "only" : "failover";
            p.note = pv->s("note", p.note);
            std::vector<std::string> servers;
            std::string serr;
            if (readServers(pv->get("servers"), servers, serr)) p.servers = servers;
            else if (pv->get("servers")) { err = serr; return false; }
            return true;
        }
        err = "配置不存在";
        return false;
    }
    err = "未知操作";
    return false;
}

Httpd::Httpd(Engine &engine, ConfigStore &cfg, Stats &stats, Proxy &proxy, std::string relaunchArgs,
             std::function<void()> requestQuit)
    : engine_(engine),
      cfg_(cfg),
      stats_(stats),
      proxy_(proxy),
      relaunchArgs_(std::move(relaunchArgs)),
      requestQuit_(std::move(requestQuit)) {}

bool Httpd::start(std::string &err) {
    if (running_) return true;
    ResolverCfg rc = cfg_.resolver();
    port_ = (uint16_t)rc.httpPort;
    startMs_ = util::nowMs();

    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port_);
    InetPtonA(AF_INET, "127.0.0.1", &a.sin_addr);

    uint64_t deadline = util::nowMs() + 5000;
    for (;;) {
        listen_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listen_ == INVALID_SOCKET) { err = "创建面板套接字失败"; return false; }
        if (bind(listen_, (sockaddr *)&a, sizeof(a)) == 0 && listen(listen_, SOMAXCONN) == 0) break;
        int code = WSAGetLastError();
        closesocket(listen_);
        listen_ = INVALID_SOCKET;
        if (code == 10048 && util::nowMs() < deadline) {
            Sleep(200);
            continue;
        }
        err = "面板端口 " + std::to_string(port_) + " 绑定失败（错误码 " + std::to_string(code) + "）";
        return false;
    }
    running_ = true;
    thread_ = std::thread([this] { acceptLoop(listen_); });
    util::logInfo("可视化面板: http://127.0.0.1:" + std::to_string(port_) + "/");
    return true;
}

void Httpd::stop() {
    if (!running_.exchange(false)) return;
    if (listen_ != INVALID_SOCKET) { closesocket(listen_); listen_ = INVALID_SOCKET; }
    if (thread_.joinable()) thread_.join();
}

void Httpd::acceptLoop(SOCKET listenSock) {
    while (running_) {
        sockaddr_storage from{};
        int fl = sizeof(from);
        SOCKET c = accept(listenSock, (sockaddr *)&from, &fl);
        if (c == INVALID_SOCKET) {
            if (!running_) break;
            continue;
        }
        std::thread([this, c] { handleConn(c); }).detach();
    }
}

void Httpd::handleConn(SOCKET client) {
    DWORD tv = 10000;
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
    setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, (const char *)&tv, sizeof(tv));

    std::string buf;
    char tmp[4096];
    size_t headerEnd = std::string::npos;
    while (buf.size() < 65536) {
        headerEnd = buf.find("\r\n\r\n");
        if (headerEnd != std::string::npos) break;
        int n = recv(client, tmp, sizeof(tmp), 0);
        if (n <= 0) break;
        buf.append(tmp, (size_t)n);
    }
    if (headerEnd == std::string::npos) {
        sendResponse(client, 400, "application/json; charset=utf-8", errJson("请求格式错误").dump());
        closesocket(client);
        return;
    }

    std::string head = buf.substr(0, headerEnd);
    std::string rest = buf.substr(headerEnd + 4);
    size_t lineEnd = head.find("\r\n");
    std::string firstLine = head.substr(0, lineEnd);
    std::map<std::string, std::string> headers;
    size_t pos = lineEnd == std::string::npos ? head.size() : lineEnd + 2;
    while (pos < head.size()) {
        size_t e = head.find("\r\n", pos);
        std::string line = head.substr(pos, e == std::string::npos ? std::string::npos : e - pos);
        size_t colon = line.find(':');
        if (colon != std::string::npos)
            headers[lower(util::trim(line.substr(0, colon)))] = util::trim(line.substr(colon + 1));
        if (e == std::string::npos) break;
        pos = e + 2;
    }

    std::string method, target, version;
    size_t s1 = firstLine.find(' '), s2 = firstLine.find(' ', s1 == std::string::npos ? 0 : s1 + 1);
    if (s1 != std::string::npos) {
        method = firstLine.substr(0, s1);
        target = firstLine.substr(s1 + 1, s2 == std::string::npos ? std::string::npos : s2 - s1 - 1);
        if (s2 != std::string::npos) version = firstLine.substr(s2 + 1);
    }
    std::string path = target, qs;
    size_t qpos = target.find('?');
    if (qpos != std::string::npos) {
        path = target.substr(0, qpos);
        qs = target.substr(qpos + 1);
    }
    std::map<std::string, std::string> query;
    parseQuery(qs, query);

    size_t contentLen = 0;
    auto cl = headers.find("content-length");
    if (cl != headers.end()) contentLen = (size_t)strtoul(cl->second.c_str(), nullptr, 10);
    if (contentLen > 1024 * 1024) {
        sendResponse(client, 413, "application/json; charset=utf-8", errJson("请求体过大").dump());
        closesocket(client);
        return;
    }
    while (rest.size() < contentLen) {
        int n = recv(client, tmp, sizeof(tmp), 0);
        if (n <= 0) break;
        rest.append(tmp, (size_t)n);
    }
    std::string body = rest.substr(0, std::min(rest.size(), contentLen));

    int status = 200;
    if (method == "POST" && headers["x-dnspilot"] != "1") {
        sendResponse(client, 403, "application/json; charset=utf-8",
                     errJson("缺少 X-DNSPilot 请求头").dump());
        closesocket(client);
        return;
    }
    std::string resp = route(method, path, query, headers, body, status);
    bool html = (path == "/" || path == "/index.html");
    const char *ctype = html ? "text/html; charset=utf-8" : "application/json; charset=utf-8";
    sendResponse(client, status, ctype, resp);
    shutdown(client, SD_BOTH);
    closesocket(client);
}

std::string Httpd::route(const std::string &method, const std::string &path,
                         const std::map<std::string, std::string> &query,
                         const std::map<std::string, std::string> &headers, const std::string &body,
                         int &status) {
    (void)headers;
    if (method == "GET" && (path == "/" || path == "/index.html")) {
        status = 200;
        return kIndexHtml;
    }

    if (method == "GET" && path == "/api/state") {
        status = 200;
        Config cfg = cfg_.snapshot();
        ResolverCfg rc = cfg.resolver;
        ProxyCfg pc = cfg.proxy;

        js::Value v = okJson();
        v["version"] = js::Value::strV("1.0.0");
        v["pid"] = js::Value::numV((double)GetCurrentProcessId());
        v["admin"] = js::Value::boolV(util::isAdmin());
        v["now"] = js::Value::numV((double)util::nowMs());
        v["uptimeSec"] = js::Value::numV((double)((util::nowMs() - startMs_) / 1000));

        js::Value r = js::Value::objV();
        r["listen"] = js::Value::strV(rc.listen);
        r["port"] = js::Value::numV(rc.port);
        r["httpPort"] = js::Value::numV(rc.httpPort);
        r["ready"] = js::Value::boolV(engine_.ready());
        r["ipv6"] = js::Value::boolV(engine_.ipv6Ready());
        r["cacheEnabled"] = js::Value::boolV(rc.cacheEnabled);
        r["cacheSize"] = js::Value::numV((double)engine_.cacheSize());
        v["resolver"] = r;

        js::Value p = js::Value::objV();
        p["enabled"] = js::Value::boolV(pc.enabled);
        p["port"] = js::Value::numV(pc.port);
        p["ready"] = js::Value::boolV(proxy_.ready());
        v["proxy"] = p;

        v["config"] = configToJson(cfg);

        TakeoverCfg tk = cfg.takeover;
        js::Value t = js::Value::objV();
        t["active"] = js::Value::boolV(tk.active);
        t["backupCount"] = js::Value::numV((double)tk.backup.size());
        js::Value ads = js::Value::arrV();
        for (auto &a : SystemDns::adapters()) {
            if (!a.up) continue;
            js::Value o = js::Value::objV();
            o["alias"] = js::Value::strV(a.alias);
            o["up"] = js::Value::boolV(a.up);
            o["hasIpv4"] = js::Value::boolV(a.hasIpv4);
            js::Value d4 = js::Value::arrV(), d6 = js::Value::arrV();
            for (auto &x : a.dns4) d4.arr.push_back(js::Value::strV(x));
            for (auto &x : a.dns6) d6.arr.push_back(js::Value::strV(x));
            o["dns4"] = d4;
            o["dns6"] = d6;
            o["mode4"] = js::Value::strV(a.mode4);
            o["mode6"] = js::Value::strV(a.mode6);
            ads.arr.push_back(o);
        }
        t["adapters"] = ads;
        v["takeover"] = t;

        js::Value st = stats_.snapshot(200);
        v["stats"] = st;
        return v.dump();
    }

    if (method == "GET" && path == "/api/test") {
        std::string domain = query.count("domain") ? query.at("domain") : "";
        std::string profile = query.count("profile") ? query.at("profile") : "";
        if (domain.empty() || domain.size() > 253) {
            status = 400;
            return errJson("域名无效").dump();
        }
        auto r = engine_.resolveTest(domain, profile);
        js::Value v = js::Value::objV();
        v["ok"] = js::Value::boolV(r.status == "ok");
        v["domain"] = js::Value::strV(domain);
        v["status"] = js::Value::strV(r.status);
        v["profileId"] = js::Value::strV(r.profileId);
        v["profileName"] = js::Value::strV(r.profileName);
        v["upstream"] = js::Value::strV(r.upstream);
        v["latencyMs"] = js::Value::numV(r.latencyMs);
        v["rcode"] = js::Value::numV(r.rcode);
        js::Value ips = js::Value::arrV();
        for (auto &ip : r.ips) ips.arr.push_back(js::Value::strV(ip));
        v["ips"] = ips;
        status = 200;
        return v.dump();
    }

    if (method == "POST" && (path == "/api/rules/app" || path == "/api/rules/domain")) {
        js::Value b;
        std::string perr;
        if (!js::Value::parse(body, b, perr)) {
            status = 400;
            return errJson("JSON 无效：" + perr).dump();
        }
        std::string err;
        bool ok = cfg_.update(
            [&](Config &c) {
                return applyRuleOp(c, path == "/api/rules/app" ? c.appRules : c.domainRules, b, err);
            },
            err);
        if (!ok) {
            status = 400;
            return errJson(err).dump();
        }
        status = 200;
        return okJson().dump();
    }

    if (method == "POST" && path == "/api/profiles") {
        js::Value b;
        std::string perr;
        if (!js::Value::parse(body, b, perr)) {
            status = 400;
            return errJson("JSON 无效：" + perr).dump();
        }
        std::string err;
        bool ok = cfg_.update([&](Config &c) { return applyProfileOp(c, b, err); }, err);
        if (!ok) {
            status = 400;
            return errJson(err).dump();
        }
        status = 200;
        return okJson().dump();
    }

    if (method == "POST" && path == "/api/settings") {
        js::Value b;
        std::string perr;
        if (!js::Value::parse(body, b, perr)) {
            status = 400;
            return errJson("JSON 无效：" + perr).dump();
        }
        Config before = cfg_.snapshot();
        bool proxyToggle = false, proxyEnable = false;
        std::string err;
        bool needsRestart = false;
        bool ok = cfg_.update(
            [&](Config &c) {
                if (b.get("defaultProfile")) {
                    std::string dp = util::trim(b.s("defaultProfile"));
                    if (!profileExists(c, dp)) { err = "默认 DNS 配置不存在"; return false; }
                    c.defaultProfile = dp;
                }
                if (b.get("cacheEnabled")) c.resolver.cacheEnabled = b.bv("cacheEnabled", true);
                if (b.get("minTtl")) {
                    int v = (int)b.n("minTtl", c.resolver.minTtl);
                    if (v < 1 || v > 86400) { err = "最小 TTL 超出范围"; return false; }
                    c.resolver.minTtl = v;
                }
                if (b.get("maxTtl")) {
                    int v = (int)b.n("maxTtl", c.resolver.maxTtl);
                    if (v < c.resolver.minTtl || v > 86400) { err = "最大 TTL 超出范围"; return false; }
                    c.resolver.maxTtl = v;
                }
                if (b.get("queryTimeoutMs")) {
                    int v = (int)b.n("queryTimeoutMs", c.resolver.queryTimeoutMs);
                    if (v < 200 || v > 10000) { err = "超时超出范围"; return false; }
                    c.resolver.queryTimeoutMs = v;
                }
                if (b.get("restoreOnExit")) c.ui.restoreOnExit = b.bv("restoreOnExit", true);
                if (b.get("openBrowser")) c.ui.openBrowser = b.bv("openBrowser", true);
                if (b.get("nativeWindow")) {
                    bool nw = b.bv("nativeWindow", true);
                    if (nw != c.ui.nativeWindow) { c.ui.nativeWindow = nw; needsRestart = true; }
                }
                if (b.get("port")) {
                    int v = (int)b.n("port", c.resolver.port);
                    if (v < 1 || v > 65535) { err = "解析端口无效"; return false; }
                    if (v != c.resolver.port) { c.resolver.port = v; needsRestart = true; }
                }
                if (b.get("httpPort")) {
                    int v = (int)b.n("httpPort", c.resolver.httpPort);
                    if (v < 1 || v > 65535) { err = "面板端口无效"; return false; }
                    if (v != c.resolver.httpPort) { c.resolver.httpPort = v; needsRestart = true; }
                }
                if (b.get("proxyPort")) {
                    int v = (int)b.n("proxyPort", c.proxy.port);
                    if (v < 1 || v > 65535) { err = "代理端口无效"; return false; }
                    if (v != c.proxy.port) { c.proxy.port = v; needsRestart = true; }
                }
                if (b.get("proxyEnabled")) {
                    bool en = b.bv("proxyEnabled", true);
                    if (en != c.proxy.enabled) { proxyToggle = true; proxyEnable = en; c.proxy.enabled = en; }
                }
                return true;
            },
            err);
        if (!ok) {
            status = 400;
            return errJson(err).dump();
        }
        engine_.applyRuntimeSettings();
        if (proxyToggle) {
            std::string perr2;
            if (proxyEnable) {
                if (!proxy_.start(perr2)) {
                    cfg_.update([&](Config &c) { c.proxy.enabled = false; return true; }, perr2);
                    status = 400;
                    return errJson("代理启动失败：" + perr2).dump();
                }
            } else {
                proxy_.stop();
            }
        }
        (void)before;
        js::Value v = okJson();
        v["needsRestart"] = js::Value::boolV(needsRestart);
        if (needsRestart) v["message"] = js::Value::strV("端口修改已保存，重启程序后生效");
        status = 200;
        return v.dump();
    }

    if (method == "POST" && path == "/api/takeover") {
        js::Value b;
        std::string perr;
        if (!js::Value::parse(body, b, perr)) {
            status = 400;
            return errJson("JSON 无效：" + perr).dump();
        }
        std::string action = b.s("action");
        if (action == "apply") {
            if (!util::isAdmin()) {
                status = 403;
                auto v = errJson("需要管理员权限");
                v["hint"] = js::Value::strV("请点击“提权重启”，在 UAC 确认后由管理员实例接管");
                return v.dump();
            }
            if (cfg_.resolver().port != 53) {
                status = 400;
                return errJson("系统接管要求解析端口为 53（当前为 " + std::to_string(cfg_.resolver().port) +
                               "），请修改端口后重启")
                    .dump();
            }
            std::string terr;
            if (!engine_.selfTest(terr)) {
                status = 400;
                return errJson("自检失败，已拒绝接管系统 DNS：" + terr).dump();
            }
            std::vector<TakeoverBackup> backup;
            TakeoverResult res;
            bool ok = SystemDns::apply(engine_.ipv6Ready(), backup, res);
            if (!ok) {
                status = 400;
                auto v = errJson(res.message.empty() ? "接管失败" : res.message);
                js::Value errs = js::Value::arrV();
                for (auto &e : res.errors) errs.arr.push_back(js::Value::strV(e));
                v["details"] = errs;
                return v.dump();
            }
            cfg_.setTakeover(true, backup);
            auto v = okJson();
            v["message"] = js::Value::strV(res.message);
            js::Value errs = js::Value::arrV();
            for (auto &e : res.errors) errs.arr.push_back(js::Value::strV(e));
            v["details"] = errs;
            status = 200;
            return v.dump();
        }
        if (action == "restore") {
            if (!util::isAdmin()) {
                status = 403;
                return errJson("需要管理员权限").dump();
            }
            TakeoverCfg tk = cfg_.takeover();
            if (tk.backup.empty()) {
                status = 400;
                return errJson("没有可恢复的 DNS 备份").dump();
            }
            TakeoverResult res;
            bool ok = SystemDns::restore(tk.backup, res);
            if (ok) cfg_.setTakeover(false, {});
            auto v = ok ? okJson() : errJson(res.message);
            v["message"] = js::Value::strV(res.message);
            js::Value errs = js::Value::arrV();
            for (auto &e : res.errors) errs.arr.push_back(js::Value::strV(e));
            v["details"] = errs;
            status = ok ? 200 : 400;
            return v.dump();
        }
        status = 400;
        return errJson("未知操作").dump();
    }

    if (method == "POST" && path == "/api/elevate") {
        if (util::isAdmin()) {
            status = 400;
            return errJson("当前已经是管理员权限").dump();
        }
        if (!util::selfElevate(relaunchArgs_)) {
            status = 500;
            return errJson("提权请求被取消或失败").dump();
        }
        if (requestQuit_) requestQuit_();
        status = 200;
        auto v = okJson();
        v["message"] = js::Value::strV("已请求提权，当前实例即将退出");
        return v.dump();
    }

    if (method == "POST" && path == "/api/quit") {
        if (requestQuit_) requestQuit_();
        status = 200;
        auto v = okJson();
        v["message"] = js::Value::strV("程序正在退出");
        return v.dump();
    }

    if (method == "POST" && path == "/api/cache/clear") {
        engine_.flushCache();
        status = 200;
        return okJson().dump();
    }

    status = 404;
    return errJson("接口不存在").dump();
}
