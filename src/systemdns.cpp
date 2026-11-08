#include "systemdns.h"

#include <algorithm>
#include <cstring>

#include "util.h"

static std::wstring netshPath() {
    wchar_t sys[MAX_PATH];
    UINT n = GetSystemDirectoryW(sys, MAX_PATH);
    std::wstring p(sys, n);
    return p + L"\\netsh.exe";
}

static bool runNetsh(const std::wstring &args, std::string &err) {
    DWORD code = 1;
    if (!util::runHidden(netshPath(), args, code)) {
        err = "无法启动 netsh";
        return false;
    }
    if (code != 0) {
        err = "netsh 返回错误码 " + std::to_string(code);
        return false;
    }
    return true;
}

static std::string regRead(const std::string &guid, bool v6, const wchar_t *name) {
    std::wstring sub = L"SYSTEM\\CurrentControlSet\\Services\\";
    sub += v6 ? L"Tcpip6" : L"Tcpip";
    sub += L"\\Parameters\\Interfaces\\";
    sub += util::w(guid);
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, sub.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS) return {};
    wchar_t buf[4096];
    DWORD cb = sizeof(buf);
    DWORD type = 0;
    std::string out;
    if (RegGetValueW(key, nullptr, name, RRF_RT_REG_SZ, &type, buf, &cb) == ERROR_SUCCESS) {
        size_t chars = cb / sizeof(wchar_t);
        while (chars > 0 && buf[chars - 1] == 0) chars--;
        out = util::u8(std::wstring(buf, chars));
    }
    RegCloseKey(key);
    return out;
}

static void readDnsState(AdapterInfo &a) {
    std::string ns4 = util::trim(regRead(a.guid, false, L"NameServer"));
    std::string ns6 = util::trim(regRead(a.guid, true, L"NameServer"));
    a.static4 = util::splitAny(ns4, ", ");
    a.static6 = util::splitAny(ns6, ", ");
    a.mode4 = a.static4.empty() ? "dhcp" : "static";
    a.mode6 = a.static6.empty() ? "dhcp" : "static";

    // 静态项为空时，注册表里的 DhcpNameServer 反映当前租约（仅展示用）
    if (a.static4.empty()) {
        std::string dhcp = regRead(a.guid, false, L"DhcpNameServer");
        a.dns4 = util::splitAny(dhcp, ", ");
    } else {
        a.dns4 = a.static4;
    }
    if (a.static6.empty()) {
        std::string dhcp = regRead(a.guid, true, L"DhcpNameServer");
        a.dns6 = util::splitAny(dhcp, ", ");
    } else {
        a.dns6 = a.static6;
    }
}

std::vector<AdapterInfo> SystemDns::adapters() {
    std::vector<AdapterInfo> out;
    ULONG size = 32 * 1024;
    std::vector<uint8_t> buf(size);
    ULONG flags = GAA_FLAG_INCLUDE_PREFIX | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST;
    ULONG rc = ERROR_BUFFER_OVERFLOW;
    for (int attempt = 0; attempt < 4 && rc == ERROR_BUFFER_OVERFLOW; attempt++) {
        rc = GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, (IP_ADAPTER_ADDRESSES *)buf.data(), &size);
        if (rc == ERROR_BUFFER_OVERFLOW) buf.resize(size);
    }
    if (rc != NO_ERROR) return out;

    for (auto *p = (IP_ADAPTER_ADDRESSES *)buf.data(); p; p = p->Next) {
        AdapterInfo a;
        a.alias = util::u8(p->FriendlyName ? p->FriendlyName : L"");
        a.guid = p->AdapterName ? p->AdapterName : "";
        a.up = p->OperStatus == IfOperStatusUp;
        // 24 = IF_TYPE_SOFTWARE_LOOPBACK，接管时排除环回伪网卡
        a.loopback = (p->IfType == 24);
        for (auto *u = p->FirstUnicastAddress; u; u = u->Next) {
            if (!u->Address.lpSockaddr) continue;
            if (u->Address.lpSockaddr->sa_family == AF_INET) a.hasIpv4 = true;
            else if (u->Address.lpSockaddr->sa_family == AF_INET6) a.hasIpv6 = true;
        }
        for (auto *d = p->FirstDnsServerAddress; d; d = d->Next) {
            std::string ip = util::sockaddrToIp(d->Address.lpSockaddr, (socklen_t)d->Address.iSockaddrLength);
            if (ip.empty()) continue;
            if (d->Address.lpSockaddr->sa_family == AF_INET) a.dns4.push_back(ip);
            else if (d->Address.lpSockaddr->sa_family == AF_INET6) a.dns6.push_back(ip);
        }
        readDnsState(a);
        if (!a.alias.empty() && !a.loopback) out.push_back(a);
    }
    return out;
}

std::vector<std::string> SystemDns::detectUpstreams() {
    std::vector<std::string> out;
    auto list = adapters();
    for (auto &a : list) {
        if (!a.up) continue;
        for (auto &ip : a.dns4) {
            if (util::isLoopbackHost(ip)) continue;
            if (std::find(out.begin(), out.end(), ip) == out.end()) out.push_back(ip);
        }
    }
    for (auto &a : list) {
        if (!a.up) continue;
        for (auto &ip : a.dns6) {
            if (util::isLoopbackHost(ip)) continue;
            if (std::find(out.begin(), out.end(), ip) == out.end()) out.push_back(ip);
        }
    }
    if (out.size() > 6) out.resize(6);
    if (out.empty()) out.push_back("223.5.5.5");
    return out;
}

bool SystemDns::apply(bool takeIpv6, std::vector<TakeoverBackup> &backup, TakeoverResult &res) {
    backup.clear();
    res = TakeoverResult();
    if (!util::isAdmin()) {
        res.message = "需要管理员权限才能接管系统 DNS";
        return false;
    }
    auto list = adapters();

    // 已指向本机说明系统 DNS 可能已被接管（或手工改成了 127.0.0.1）。
    // 此时再接管会把环回地址当成“原配置”写入备份，必须拒绝并提示先恢复。
    // IPv6 的 ::1 允许存在（可能是上次接管残留）：它不会影响 IPv4 备份，
    // 且下面的备份过滤会把静态 ::1 记为 DHCP，恢复时改为自动获取。
    for (auto &a : list) {
        if (!a.up) continue;
        bool loop4 = false;
        for (auto &ip : a.dns4) if (util::isLoopbackHost(ip)) loop4 = true;
        if (loop4) {
            res.message = "检测到网卡“" + a.alias +
                          "”的 IPv4 DNS 已指向本机（127.0.0.1）。系统 DNS 可能已被接管；请先点「恢复原 DNS」，"
                          "避免把环回地址写入备份。";
            return false;
        }
    }

    int applied = 0;
    for (auto &a : list) {
        if (!a.up || !a.hasIpv4 || a.alias.empty()) continue;

        std::string err;
        std::wstring alias = util::w(a.alias);
        std::wstring cmd4 = L"interface ipv4 set dnsservers name=\"" + alias + L"\" static 127.0.0.1 primary validate=no";
        if (runNetsh(cmd4, err)) {
            TakeoverBackup b;
            b.alias = a.alias;
            b.family = "ipv4";
            b.mode = a.mode4;
            for (auto &s : a.static4)
                if (!util::isLoopbackHost(s)) b.servers.push_back(s);
            if (b.mode == "static" && b.servers.empty()) b.mode = "dhcp";
            backup.push_back(b);
            util::logInfo("接管备份 IPv4 " + a.alias + "：" + b.mode +
                          (b.servers.empty() ? "" : " " + b.servers.front()));
            applied++;
        } else {
            res.errors.push_back(a.alias + " IPv4: " + err);
        }

        if (takeIpv6 && (a.hasIpv6 || !a.dns6.empty())) {
            std::wstring cmd6 =
                L"interface ipv6 set dnsservers name=\"" + alias + L"\" static ::1 primary validate=no";
            if (runNetsh(cmd6, err)) {
                TakeoverBackup b;
                b.alias = a.alias;
                b.family = "ipv6";
                b.mode = a.mode6;
                for (auto &s : a.static6)
                    if (!util::isLoopbackHost(s)) b.servers.push_back(s);
                if (b.mode == "static" && b.servers.empty()) b.mode = "dhcp";
                backup.push_back(b);
                util::logInfo("接管备份 IPv6 " + a.alias + "：" + b.mode);
                applied++;
            } else {
                res.errors.push_back(a.alias + " IPv6: " + err);
            }
        }
    }
    flushDns();
    if (applied == 0) {
        res.message = "没有可接管的网卡";
        for (auto &e : res.errors) res.message += "；" + e;
        return false;
    }
    res.ok = true;
    res.message = "已接管 " + std::to_string(applied) + " 项网卡 DNS 设置";
    if (!res.errors.empty()) res.message += "（部分失败）";
    return true;
}

static bool restoreOne(const TakeoverBackup &b, const std::vector<std::string> &fallback4,
                       bool &usedFallback, bool &usedDhcp, std::string &err) {
    usedFallback = false;
    usedDhcp = false;
    std::wstring family = util::w(b.family == "ipv6" ? "ipv6" : "ipv4");
    std::wstring alias = util::w(b.alias);
    std::wstring base = L"interface " + family + L" ";

    // 绝不把 127.0.0.1 / ::1 写回系统 DNS：备份里出现环回地址说明备份被污染或已接管
    std::vector<std::string> servers;
    for (auto &s : b.servers)
        if (!util::isLoopbackHost(s)) servers.push_back(s);

    bool staticMode = (b.mode == "static");
    if (staticMode && servers.empty()) {
        if (b.family != "ipv6" && !fallback4.empty()) {
            servers = fallback4;
            usedFallback = true;
            util::logWarn("备份 " + b.alias + " " + b.family +
                          " 无效（仅含环回/为空），改用当前系统 DNS 恢复");
        } else {
            // IPv6 没有可用的回退地址时改为自动获取，避免回写 ::1
            usedDhcp = true;
            staticMode = false;
            util::logWarn("备份 " + b.alias + " " + b.family +
                          " 无效（仅含环回/为空），改为自动获取（DHCP）");
        }
    }

    if (staticMode) {
        std::wstring cmd = base + L"set dnsservers name=\"" + alias + L"\" static " + util::w(servers[0]) +
                           L" primary validate=no";
        if (!runNetsh(cmd, err)) return false;
        for (size_t i = 1; i < servers.size(); i++) {
            std::wstring add = base + L"add dnsservers name=\"" + alias + L"\" " + util::w(servers[i]) +
                               L" index=" + std::to_wstring(i + 1) + L" validate=no";
            if (!runNetsh(add, err)) return false;
        }
        util::logInfo("恢复 " + b.alias + " " + b.family + "：静态 " + servers[0]);
        return true;
    }
    std::wstring cmd = base + L"set dnsservers name=\"" + alias + L"\" dhcp";
    if (!runNetsh(cmd, err)) return false;
    util::logInfo("恢复 " + b.alias + " " + b.family + "：自动获取（DHCP）");
    return true;
}

bool SystemDns::restore(const std::vector<TakeoverBackup> &backup, TakeoverResult &res,
                        const std::vector<std::string> &fallback4) {
    res = TakeoverResult();
    if (!util::isAdmin()) {
        res.message = "需要管理员权限才能恢复系统 DNS";
        return false;
    }
    if (backup.empty()) {
        res.message = "没有可恢复的备份";
        return false;
    }
    int okCount = 0, fallbackCount = 0, dhcpCount = 0;
    for (auto &b : backup) {
        std::string err;
        bool usedFallback = false, usedDhcp = false;
        if (restoreOne(b, fallback4, usedFallback, usedDhcp, err)) {
            okCount++;
            if (usedFallback) fallbackCount++;
            if (usedDhcp) dhcpCount++;
        } else {
            res.errors.push_back(b.alias + " " + b.family + ": " + err);
        }
    }
    flushDns();
    // 只有全部恢复成功才算成功；部分失败时保留备份，便于重试
    res.ok = okCount == (int)backup.size();
    res.message = "已恢复 " + std::to_string(okCount) + "/" + std::to_string(backup.size()) + " 项网卡 DNS 设置";
    if (fallbackCount > 0)
        res.message += "（其中 " + std::to_string(fallbackCount) + " 项备份无效，已用当前系统 DNS 恢复）";
    if (dhcpCount > 0)
        res.message += "（其中 " + std::to_string(dhcpCount) + " 项备份无效，已改为自动获取）";
    if (!res.errors.empty()) res.message += "（部分失败）";
    return res.ok;
}

void SystemDns::flushDns() {
    wchar_t sys[MAX_PATH];
    UINT n = GetSystemDirectoryW(sys, MAX_PATH);
    std::wstring exe = std::wstring(sys, n) + L"\\ipconfig.exe";
    DWORD code = 0;
    util::runHidden(exe, L"/flushdns", code);
}
