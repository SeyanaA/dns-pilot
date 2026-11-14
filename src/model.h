// 配置模型与唯一存储
#pragma once
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "json.h"
#include "util.h"

struct Profile {
    std::string id;
    std::string name;
    std::string provider;      // system / cloudflare / aliyun / google / dnspod / quad9 / custom
    std::string strategy;      // failover / only
    std::string note;
    std::vector<std::string> servers;  // 1.1.1.1 或 1.1.1.1:53 或 ::1
};

struct Rule {
    std::string match;
    std::string profile;
    std::string note;
    bool enabled = true;
};

struct ResolverCfg {
    std::string listen = "127.0.0.1";
    int port = 53;
    int httpPort = 8765;
    bool cacheEnabled = true;
    int minTtl = 30;
    int maxTtl = 3600;
    int queryTimeoutMs = 1200;
};

struct ProxyCfg {
    bool enabled = true;
    int port = 8899;
};

struct TakeoverBackup {
    std::string alias;
    std::string family;   // ipv4 / ipv6
    std::string mode;     // static / dhcp
    std::vector<std::string> servers;
};

struct TakeoverCfg {
    bool active = false;
    std::vector<TakeoverBackup> backup;
};

struct UiCfg {
    bool openBrowser = true;
    bool restoreOnExit = true;
    bool nativeWindow = true;
    bool autoStart = false;     // 登录时自动启动（计划任务，最高权限）
    bool autoTakeover = false;  // 启动后自动执行系统 DNS 接管
    std::string theme = "light";  // light / dark；新建配置默认浅色
};

struct Config {
    int version = 1;
    ResolverCfg resolver;
    ProxyCfg proxy;
    std::vector<Profile> profiles;
    std::string defaultProfile = "system";
    std::vector<Rule> appRules;
    std::vector<Rule> domainRules;
    TakeoverCfg takeover;
    UiCfg ui;
};

struct ProfileChoice {
    std::string profileId;
    std::string ruleType;   // domain / app / none
    std::string ruleMatch;
};

Config makeDefaultConfig();
std::string defaultConfigPath();
js::Value configToJson(const Config &c);
bool configFromJson(const js::Value &v, Config &c, std::string &err);

// 导出/导入：只包含 DNS 配置与规则绑定，不含端口、接管备份等本机状态
struct TransferStats {
    int profilesAdded = 0;
    int profilesReused = 0;
    int rulesAdded = 0;
    int rulesSkipped = 0;  // 重复或引用了无效配置
};

js::Value configExportBundle(const Config &c, const std::string &appVersion,
                             const std::string &onlyProfile = "");
bool configImportBundle(const js::Value &v, Config &c, TransferStats &st, std::string &err);

class ConfigStore {
  public:
    bool load(std::string &err);
    bool save(std::string &err) const;
    Config snapshot() const;
    std::string path() const;
    void setPath(std::string p);

    bool update(const std::function<bool(Config &)> &fn, std::string &err);
    void mutate(const std::function<void(Config &)> &fn);

    ProfileChoice chooseProfile(const std::string &domain, const std::string &exe) const;
    bool profileById(const std::string &id, Profile &out) const;
    std::vector<Profile> profiles() const;
    void setSystemServers(const std::vector<std::string> &servers);
    void setTakeover(bool active, const std::vector<TakeoverBackup> &backup);
    TakeoverCfg takeover() const;
    ResolverCfg resolver() const;
    ProxyCfg proxy() const;
    std::string defaultProfile() const;
    bool restoreOnExit() const;
    bool cacheEnabled() const;

  private:
    mutable std::mutex mu_;
    Config cfg_ = makeDefaultConfig();
    std::string path_;
};
