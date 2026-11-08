// 系统 DNS 接管 / 恢复（netsh + 注册表备份）
#pragma once
#include <string>
#include <vector>

#include "model.h"

struct AdapterInfo {
    std::string alias;
    std::string guid;
    bool up = false;
    bool loopback = false;
    bool hasIpv4 = false;
    bool hasIpv6 = false;
    std::vector<std::string> dns4, dns6;   // 当前生效的 DNS
    std::vector<std::string> static4, static6;  // 注册表中的静态配置
    std::string mode4 = "dhcp", mode6 = "dhcp";
};

struct TakeoverResult {
    bool ok = false;
    std::string message;
    std::vector<std::string> errors;
};

class SystemDns {
  public:
    static std::vector<AdapterInfo> adapters();
    static std::vector<std::string> detectUpstreams();

    static bool apply(bool takeIpv6, std::vector<TakeoverBackup> &backup, TakeoverResult &res);
    // fallback4：备份无效（仅含环回/为空）时用于 IPv4 的恢复地址，通常取当前 system 配置探测到的 DNS
    static bool restore(const std::vector<TakeoverBackup> &backup, TakeoverResult &res,
                        const std::vector<std::string> &fallback4 = {});
    static void flushDns();
};
