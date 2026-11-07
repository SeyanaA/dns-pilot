#include "model.h"

#include <algorithm>
#include <cstdlib>
#include <map>

using util::trim;

static Profile mkProfile(const std::string &id, const std::string &name, const std::string &provider,
                         std::vector<std::string> servers) {
    Profile p;
    p.id = id;
    p.name = name;
    p.provider = provider;
    p.strategy = "failover";
    p.servers = std::move(servers);
    return p;
}

Config makeDefaultConfig() {
    Config c;
    c.profiles.push_back(mkProfile("system", "系统默认", "system", {}));
    c.profiles.push_back(mkProfile("cloudflare", "Cloudflare", "cloudflare", {"1.1.1.1", "1.0.0.1"}));
    c.profiles.push_back(mkProfile("aliyun", "阿里云 DNS", "aliyun", {"223.5.5.5", "223.6.6.6"}));
    c.profiles.push_back(mkProfile("google", "Google DNS", "google", {"8.8.8.8", "8.8.4.4"}));
    c.profiles.push_back(mkProfile("dnspod", "腾讯 DNSPod", "dnspod", {"119.29.29.29", "182.254.116.116"}));
    c.profiles.push_back(mkProfile("quad9", "Quad9", "quad9", {"9.9.9.9", "149.112.112.112"}));
    return c;
}

std::string defaultConfigPath() {
    std::string dir = util::exeDir();
    std::string p = dir + "\\dnspilot.json";
    if (util::fileExists(p)) return p;
    // 探测可写性
    std::string probe = dir + "\\.dnspilot-write-test";
    HANDLE h = CreateFileW(util::w(probe).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        CloseHandle(h);
        return p;
    }
    const char *appdata = getenv("APPDATA");
    std::string base = appdata ? appdata : "";
    if (base.empty()) base = dir;
    std::string d = base + "\\DNSPilot";
    util::ensureDir(d);
    return d + "\\dnspilot.json";
}

js::Value configToJson(const Config &c) {
    js::Value v = js::Value::objV();
    v["version"] = js::Value::numV(c.version);

    js::Value r = js::Value::objV();
    r["listen"] = js::Value::strV(c.resolver.listen);
    r["port"] = js::Value::numV(c.resolver.port);
    r["httpPort"] = js::Value::numV(c.resolver.httpPort);
    r["cacheEnabled"] = js::Value::boolV(c.resolver.cacheEnabled);
    r["minTtl"] = js::Value::numV(c.resolver.minTtl);
    r["maxTtl"] = js::Value::numV(c.resolver.maxTtl);
    r["queryTimeoutMs"] = js::Value::numV(c.resolver.queryTimeoutMs);
    v["resolver"] = r;

    js::Value px = js::Value::objV();
    px["enabled"] = js::Value::boolV(c.proxy.enabled);
    px["port"] = js::Value::numV(c.proxy.port);
    v["proxy"] = px;

    js::Value ps = js::Value::arrV();
    for (auto &p : c.profiles) {
        js::Value o = js::Value::objV();
        o["id"] = js::Value::strV(p.id);
        o["name"] = js::Value::strV(p.name);
        o["provider"] = js::Value::strV(p.provider);
        o["strategy"] = js::Value::strV(p.strategy);
        o["note"] = js::Value::strV(p.note);
        js::Value arr = js::Value::arrV();
        for (auto &s : p.servers) arr.arr.push_back(js::Value::strV(s));
        o["servers"] = arr;
        ps.arr.push_back(o);
    }
    v["profiles"] = ps;
    v["defaultProfile"] = js::Value::strV(c.defaultProfile);

    auto rulesJson = [](const std::vector<Rule> &rules) {
        js::Value a = js::Value::arrV();
        for (auto &r : rules) {
            js::Value o = js::Value::objV();
            o["match"] = js::Value::strV(r.match);
            o["profile"] = js::Value::strV(r.profile);
            o["note"] = js::Value::strV(r.note);
            o["enabled"] = js::Value::boolV(r.enabled);
            a.arr.push_back(o);
        }
        return a;
    };
    v["appRules"] = rulesJson(c.appRules);
    v["domainRules"] = rulesJson(c.domainRules);

    js::Value tk = js::Value::objV();
    tk["active"] = js::Value::boolV(c.takeover.active);
    js::Value bk = js::Value::arrV();
    for (auto &b : c.takeover.backup) {
        js::Value o = js::Value::objV();
        o["alias"] = js::Value::strV(b.alias);
        o["family"] = js::Value::strV(b.family);
        o["mode"] = js::Value::strV(b.mode);
        js::Value arr = js::Value::arrV();
        for (auto &s : b.servers) arr.arr.push_back(js::Value::strV(s));
        o["servers"] = arr;
        bk.arr.push_back(o);
    }
    tk["backup"] = bk;
    v["takeover"] = tk;

    js::Value ui = js::Value::objV();
    ui["openBrowser"] = js::Value::boolV(c.ui.openBrowser);
    ui["restoreOnExit"] = js::Value::boolV(c.ui.restoreOnExit);
    ui["nativeWindow"] = js::Value::boolV(c.ui.nativeWindow);
    ui["theme"] = js::Value::strV(c.ui.theme);
    v["ui"] = ui;
    return v;
}

static std::vector<std::string> readStringArray(const js::Value *arr) {
    std::vector<std::string> out;
    if (!arr || !arr->isArr()) return out;
    for (auto &x : arr->arr)
        if (x.isStr()) out.push_back(trim(x.str));
    return out;
}

bool configFromJson(const js::Value &v, Config &c, std::string &err) {
    if (!v.isObj()) { err = "配置根节点必须是对象"; return false; }
    Config d = makeDefaultConfig();
    c = d;
    c.version = (int)v.n("version", 1);

    if (const js::Value *r = v.get("resolver")) {
        c.resolver.listen = r->s("listen", c.resolver.listen);
        c.resolver.port = (int)r->n("port", c.resolver.port);
        c.resolver.httpPort = (int)r->n("httpPort", c.resolver.httpPort);
        c.resolver.cacheEnabled = r->bv("cacheEnabled", c.resolver.cacheEnabled);
        c.resolver.minTtl = (int)r->n("minTtl", c.resolver.minTtl);
        c.resolver.maxTtl = (int)r->n("maxTtl", c.resolver.maxTtl);
        c.resolver.queryTimeoutMs = (int)r->n("queryTimeoutMs", c.resolver.queryTimeoutMs);
    }
    if (c.resolver.port < 1 || c.resolver.port > 65535) c.resolver.port = 53;
    if (c.resolver.httpPort < 1 || c.resolver.httpPort > 65535) c.resolver.httpPort = 8765;
    if (c.resolver.queryTimeoutMs < 200) c.resolver.queryTimeoutMs = 200;
    if (c.resolver.queryTimeoutMs > 10000) c.resolver.queryTimeoutMs = 10000;
    if (c.resolver.minTtl < 1) c.resolver.minTtl = 1;
    if (c.resolver.maxTtl < c.resolver.minTtl) c.resolver.maxTtl = c.resolver.minTtl;
    if (c.resolver.maxTtl > 86400) c.resolver.maxTtl = 86400;

    if (const js::Value *p = v.get("proxy")) {
        c.proxy.enabled = p->bv("enabled", c.proxy.enabled);
        c.proxy.port = (int)p->n("port", c.proxy.port);
    }
    if (c.proxy.port < 1 || c.proxy.port > 65535) c.proxy.port = 8899;

    c.profiles.clear();
    if (const js::Value *ps = v.get("profiles")) {
        if (ps->isArr()) {
            for (auto &x : ps->arr) {
                if (!x.isObj()) continue;
                Profile p;
                p.id = trim(x.s("id"));
                p.name = trim(x.s("name"));
                p.provider = x.s("provider", "custom");
                p.strategy = x.s("strategy", "failover");
                p.note = x.s("note");
                if (p.id.empty() || p.name.empty()) continue;
                bool dup = false;
                for (auto &q : c.profiles) if (q.id == p.id) dup = true;
                if (dup) continue;
                for (auto &s : readStringArray(x.get("servers"))) {
                    std::string host;
                    uint16_t port = 0;
                    if (util::parseServer(s, host, port)) p.servers.push_back(s);
                }
                if (p.strategy != "only") p.strategy = "failover";
                c.profiles.push_back(p);
            }
        }
    }
    if (c.profiles.empty()) c.profiles = d.profiles;

    bool hasSystem = false;
    for (auto &p : c.profiles) if (p.id == "system") hasSystem = true;
    if (!hasSystem) {
        Profile sys;
        sys.id = "system";
        sys.name = "系统默认";
        sys.provider = "system";
        sys.strategy = "failover";
        c.profiles.insert(c.profiles.begin(), sys);
    }

    c.defaultProfile = trim(v.s("defaultProfile", "system"));
    bool defOk = false;
    for (auto &p : c.profiles) if (p.id == c.defaultProfile) defOk = true;
    if (!defOk) c.defaultProfile = "system";

    auto readRules = [&](const js::Value *arr, std::vector<Rule> &out) {
        out.clear();
        if (!arr || !arr->isArr()) return;
        for (auto &x : arr->arr) {
            if (!x.isObj()) continue;
            Rule r;
            r.match = trim(x.s("match"));
            r.profile = trim(x.s("profile"));
            r.note = x.s("note");
            r.enabled = x.bv("enabled", true);
            if (r.match.empty()) continue;
            bool found = false;
            for (auto &p : c.profiles) if (p.id == r.profile) found = true;
            if (!found) continue;
            out.push_back(r);
        }
    };
    readRules(v.get("appRules"), c.appRules);
    readRules(v.get("domainRules"), c.domainRules);

    if (const js::Value *t = v.get("takeover")) {
        c.takeover.active = t->bv("active", false);
        c.takeover.backup.clear();
        if (const js::Value *bk = t->get("backup")) {
            if (bk->isArr()) {
                for (auto &x : bk->arr) {
                    if (!x.isObj()) continue;
                    TakeoverBackup b;
                    b.alias = x.s("alias");
                    b.family = x.s("family", "ipv4");
                    b.mode = x.s("mode", "dhcp");
                    b.servers = readStringArray(x.get("servers"));
                    if (b.alias.empty()) continue;
                    c.takeover.backup.push_back(b);
                }
            }
        }
    }
    if (const js::Value *ui = v.get("ui")) {
        c.ui.openBrowser = ui->bv("openBrowser", c.ui.openBrowser);
        c.ui.restoreOnExit = ui->bv("restoreOnExit", c.ui.restoreOnExit);
        c.ui.nativeWindow = ui->bv("nativeWindow", c.ui.nativeWindow);
        // 只接受 light/dark；缺失或非法（含旧配置没有该字段）一律按浅色处理
        std::string theme = util::trim(ui->s("theme", c.ui.theme));
        c.ui.theme = (theme == "dark") ? "dark" : "light";
    }
    return true;
}

bool ConfigStore::load(std::string &err) {
    std::lock_guard<std::mutex> g(mu_);
    std::string text;
    if (!util::fileRead(path_, text)) {
        cfg_ = makeDefaultConfig();
        return true;
    }
    js::Value v;
    if (!js::Value::parse(text, v, err)) {
        err = "配置文件解析失败：" + err + "（已回退默认配置）";
        cfg_ = makeDefaultConfig();
        return false;
    }
    Config parsed;
    std::string perr;
    if (!configFromJson(v, parsed, perr)) {
        err = "配置内容无效：" + perr;
        cfg_ = makeDefaultConfig();
        return false;
    }
    cfg_ = std::move(parsed);
    return true;
}

bool ConfigStore::save(std::string &err) const {
    std::lock_guard<std::mutex> g(mu_);
    if (path_.empty()) { err = "配置路径为空"; return false; }
    std::string tmp = path_ + ".tmp";
    std::string data = configToJson(cfg_).dump(2) + "\n";
    if (!util::fileWrite(tmp, data)) { err = "无法写入配置文件：" + tmp; return false; }
    if (!MoveFileExW(util::w(tmp).c_str(), util::w(path_).c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(util::w(tmp).c_str());
        err = "替换配置文件失败：" + path_;
        return false;
    }
    return true;
}

Config ConfigStore::snapshot() const {
    std::lock_guard<std::mutex> g(mu_);
    return cfg_;
}

std::string ConfigStore::path() const {
    std::lock_guard<std::mutex> g(mu_);
    return path_;
}

void ConfigStore::setPath(std::string p) {
    std::lock_guard<std::mutex> g(mu_);
    path_ = std::move(p);
}

bool ConfigStore::update(const std::function<bool(Config &)> &fn, std::string &err) {
    {
        std::lock_guard<std::mutex> g(mu_);
        if (!fn(cfg_)) { err = "配置更新被拒绝"; return false; }
    }
    return save(err);
}

void ConfigStore::mutate(const std::function<void(Config &)> &fn) {
    std::lock_guard<std::mutex> g(mu_);
    fn(cfg_);
}

ProfileChoice ConfigStore::chooseProfile(const std::string &domain, const std::string &exe) const {
    std::lock_guard<std::mutex> g(mu_);
    auto exists = [&](const std::string &id) {
        for (auto &p : cfg_.profiles) if (p.id == id) return true;
        return false;
    };
    ProfileChoice out;
    out.profileId = cfg_.defaultProfile;
    out.ruleType = "none";
    if (!exists(out.profileId) && !cfg_.profiles.empty()) out.profileId = cfg_.profiles.front().id;

    size_t bestLen = 0;
    for (auto &r : cfg_.domainRules) {
        if (!r.enabled) continue;
        if (!exists(r.profile)) continue;
        if (!util::domainMatch(r.match, domain)) continue;
        size_t len = util::lowerAscii(util::trim(r.match)).size();
        if (len >= bestLen) {
            bestLen = len;
            out.profileId = r.profile;
            out.ruleType = "domain";
            out.ruleMatch = r.match;
        }
    }
    if (out.ruleType == "domain") return out;

    std::string exeLower = util::lowerAscii(util::trim(exe));
    if (!exeLower.empty()) {
        for (auto &r : cfg_.appRules) {
            if (!r.enabled) continue;
            if (!exists(r.profile)) continue;
            if (util::globMatch(r.match, exeLower)) {
                out.profileId = r.profile;
                out.ruleType = "app";
                out.ruleMatch = r.match;
                break;
            }
        }
    }
    return out;
}

bool ConfigStore::profileById(const std::string &id, Profile &out) const {
    std::lock_guard<std::mutex> g(mu_);
    for (auto &p : cfg_.profiles)
        if (p.id == id) { out = p; return true; }
    return false;
}

std::vector<Profile> ConfigStore::profiles() const {
    std::lock_guard<std::mutex> g(mu_);
    return cfg_.profiles;
}

void ConfigStore::setSystemServers(const std::vector<std::string> &servers) {
    {
        std::lock_guard<std::mutex> g(mu_);
        for (auto &p : cfg_.profiles) {
            if (p.id == "system") { p.servers = servers; break; }
        }
    }
    std::string err;
    save(err);
}

void ConfigStore::setTakeover(bool active, const std::vector<TakeoverBackup> &backup) {
    {
        std::lock_guard<std::mutex> g(mu_);
        cfg_.takeover.active = active;
        cfg_.takeover.backup = backup;
    }
    std::string err;
    save(err);
}

TakeoverCfg ConfigStore::takeover() const {
    std::lock_guard<std::mutex> g(mu_);
    return cfg_.takeover;
}

ResolverCfg ConfigStore::resolver() const {
    std::lock_guard<std::mutex> g(mu_);
    return cfg_.resolver;
}

ProxyCfg ConfigStore::proxy() const {
    std::lock_guard<std::mutex> g(mu_);
    return cfg_.proxy;
}

std::string ConfigStore::defaultProfile() const {
    std::lock_guard<std::mutex> g(mu_);
    return cfg_.defaultProfile;
}

bool ConfigStore::restoreOnExit() const {
    std::lock_guard<std::mutex> g(mu_);
    return cfg_.ui.restoreOnExit;
}

bool ConfigStore::cacheEnabled() const {
    std::lock_guard<std::mutex> g(mu_);
    return cfg_.resolver.cacheEnabled;
}
