#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "engine.h"
#include "httpd.h"
#include "ui_native.h"
#include "autostart.h"
#include "model.h"
#include "proc.h"
#include "proxy.h"
#include "stats.h"
#include "systemdns.h"
#include "util.h"

static const char *kVersion = "1.1.4";

struct Options {
    std::string configPath;
    std::string exportPath;
    std::string importPath;
    int port = 0, httpPort = 0, proxyPort = 0;
    bool noBrowser = false, noProxy = false, takeover = false, restore = false;
    bool elevate = false, status = false, help = false, verbose = false, takeoverPlan = false;
    bool noGui = false;
    bool autostart = false;  // 由计划任务传入：最小化窗口、不打开浏览器面板
};

static bool g_hasConsole = false;

// Windows 子系统启动时没有控制台；从 cmd 启动则附着父控制台，保留 CLI 输出
static void setupConsole() {
    if (GetConsoleWindow()) {
        g_hasConsole = true;
        SetConsoleOutputCP(CP_UTF8);
        SetConsoleTitleW(L"DNS Pilot");
        return;
    }
    // 输出已重定向到文件/管道时保留原句柄，否则 freopen(CONOUT$) 会吞掉重定向结果
    DWORD outType = GetFileType(GetStdHandle(STD_OUTPUT_HANDLE));
    DWORD errType = GetFileType(GetStdHandle(STD_ERROR_HANDLE));
    bool outRedirected = outType == FILE_TYPE_DISK || outType == FILE_TYPE_PIPE;
    bool errRedirected = errType == FILE_TYPE_DISK || errType == FILE_TYPE_PIPE;
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        g_hasConsole = true;
        if (!outRedirected) freopen("CONOUT$", "w", stdout);
        if (!errRedirected) freopen("CONOUT$", "w", stderr);
        freopen("CONIN$", "r", stdin);
        SetConsoleOutputCP(CP_UTF8);
        SetConsoleTitleW(L"DNS Pilot");
    }
}

static void printHelp() {
    printf(
        "DNS Pilot %s — 让指定应用或网站优先使用指定 DNS\n\n"
        "用法: dnspilot.exe [选项]\n\n"
        "  --port <n>         解析器端口（默认 53；非管理员测试可用 5451）\n"
        "  --http-port <n>    可视化面板端口（默认 8765）\n"
        "  --proxy-port <n>   应用代理端口（默认 8899）\n"
        "  --config <path>    指定配置文件路径\n"
        "  --export-config <path>  导出 DNS 配置与绑定规则（JSON）后退出\n"
        "  --import-config <path>  导入 DNS 配置与绑定规则（合并，跳过重复）后退出\n"
        "  --no-proxy         不启动应用代理\n"
        "  --no-browser       不自动打开浏览器面板\n"
        "  --no-gui           不创建桌面窗口（仅运行 Web 面板与 API）\n"
        "  --autostart        开机自启动模式：最小化窗口、不打开浏览器面板\n"
        "  --takeover         启动后接管系统 DNS（需管理员）\n"
        "  --takeover-plan    只打印接管计划（网卡与 netsh 命令），不执行\n"
        "  --restore          恢复系统 DNS 后退出（需管理员）\n"
        "  --elevate          以管理员身份重新启动\n"
        "  --status           打印当前配置与网卡状态后退出\n"
        "  --verbose          输出调试日志\n"
        "  --help             显示帮助\n",
        kVersion);
}

static std::string joinArgs(int argc, char **argv, int from) {
    std::string out;
    for (int i = from; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--elevate") continue;
        if (!out.empty()) out += " ";
        out += util::u8(util::quoteArg(a));
    }
    return out;
}

static std::vector<std::string> collectUpstreams(const Config &cfg) {
    std::vector<std::string> out;
    if (cfg.takeover.active) {
        for (auto &b : cfg.takeover.backup)
            for (auto &s : b.servers)
                if (!util::isLoopbackHost(s) && std::find(out.begin(), out.end(), s) == out.end())
                    out.push_back(s);
    }
    if (out.empty()) out = SystemDns::detectUpstreams();
    return out;
}

// 备份失效（仅含环回）时用于恢复 IPv4 的兜底地址
static std::vector<std::string> systemFallback(const Config &cfg) {
    std::vector<std::string> out;
    for (auto &p : cfg.profiles) {
        if (p.id != "system") continue;
        for (auto &s : p.servers)
            if (!util::isLoopbackHost(s) && std::find(out.begin(), out.end(), s) == out.end())
                out.push_back(s);
    }
    return out;
}

int main(int argc, char **argv) {
    setupConsole();

    Options opt;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&](int &v) {
            if (i + 1 < argc) v = atoi(argv[++i]);
        };
        if (a == "--port") next(opt.port);
        else if (a == "--http-port") next(opt.httpPort);
        else if (a == "--proxy-port") next(opt.proxyPort);
        else if (a == "--config" && i + 1 < argc) opt.configPath = argv[++i];
        else if (a == "--export-config" && i + 1 < argc) opt.exportPath = argv[++i];
        else if (a == "--import-config" && i + 1 < argc) opt.importPath = argv[++i];
        else if (a == "--no-browser") opt.noBrowser = true;
        else if (a == "--no-gui") opt.noGui = true;
        else if (a == "--autostart") opt.autostart = true;
        else if (a == "--no-proxy") opt.noProxy = true;
        else if (a == "--takeover") opt.takeover = true;
        else if (a == "--takeover-plan") opt.takeoverPlan = true;
        else if (a == "--restore") opt.restore = true;
        else if (a == "--elevate") opt.elevate = true;
        else if (a == "--status") opt.status = true;
        else if (a == "--verbose") opt.verbose = true;
        else if (a == "--help" || a == "-h") opt.help = true;
    }
    if (opt.help) {
        printHelp();
        return 0;
    }
    util::verboseFlag() = opt.verbose;

    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        printf("WSAStartup 失败\n");
        return 1;
    }

    SetConsoleCtrlHandler(
        [](DWORD type) -> BOOL {
            if (type == CTRL_C_EVENT || type == CTRL_CLOSE_EVENT || type == CTRL_BREAK_EVENT) {
                util::quitFlag() = true;
                return TRUE;
            }
            return FALSE;
        },
        TRUE);

    std::string relaunchArgs = joinArgs(argc, argv, 1);
    if (opt.elevate) {
        if (util::isAdmin()) {
            printf("当前已经是管理员权限，无需提权\n");
        } else if (util::selfElevate(relaunchArgs)) {
            printf("已请求以管理员身份重新启动（请在 UAC 对话框中确认）\n");
            return 0;
        } else {
            printf("提权请求被取消或失败\n");
            return 1;
        }
    }

    ConfigStore cfg;
    cfg.setPath(opt.configPath.empty() ? defaultConfigPath() : opt.configPath);
    std::string err;
    if (!cfg.load(err) && !err.empty()) util::logWarn(err);

    // 配置导出 / 导入：只处理 DNS 配置与规则绑定，完成后直接退出（不启动服务）
    if (!opt.exportPath.empty()) {
        Config c = cfg.snapshot();
        js::Value bundle = configExportBundle(c, kVersion);
        std::string text = bundle.dump(2) + "\n";
        if (!util::fileWrite(opt.exportPath, text)) {
            printf("导出失败：无法写入 %s\n", opt.exportPath.c_str());
            return 1;
        }
        printf("已导出 %d 个 DNS 配置、%d 条应用规则、%d 条域名规则 -> %s\n",
               (int)c.profiles.size(), (int)c.appRules.size(), (int)c.domainRules.size(),
               opt.exportPath.c_str());
        return 0;
    }
    if (!opt.importPath.empty()) {
        std::string text;
        if (!util::fileRead(opt.importPath, text)) {
            printf("导入失败：无法读取 %s\n", opt.importPath.c_str());
            return 1;
        }
        js::Value bundle;
        std::string perr;
        if (!js::Value::parse(text, bundle, perr)) {
            printf("导入失败：JSON 解析错误：%s\n", perr.c_str());
            return 1;
        }
        TransferStats st;
        std::string ierr;
        bool accepted = false;
        bool saved = cfg.update(
            [&](Config &c) {
                accepted = configImportBundle(bundle, c, st, ierr);
                return accepted;
            },
            err);
        if (!accepted) {
            printf("导入失败：%s\n", ierr.c_str());
            return 1;
        }
        if (!saved) {
            printf("导入失败：%s\n", err.c_str());
            return 1;
        }
        printf("已导入：新增 %d 个 DNS 配置（复用 %d 个）、%d 条规则，跳过 %d 条；配置文件：%s\n",
               st.profilesAdded, st.profilesReused, st.rulesAdded, st.rulesSkipped,
               cfg.path().c_str());
        return 0;
    }

    cfg.mutate([&](Config &c) {
        if (opt.port > 0) c.resolver.port = opt.port;
        if (opt.httpPort > 0) c.resolver.httpPort = opt.httpPort;
        if (opt.proxyPort > 0) c.proxy.port = opt.proxyPort;
        if (opt.noProxy) c.proxy.enabled = false;
        if (opt.noBrowser) c.ui.openBrowser = false;
        if (opt.noGui) c.ui.nativeWindow = false;
    });
    cfg.update([&](Config &c) {
        std::vector<std::string> up = collectUpstreams(c);
        for (auto &p : c.profiles)
            if (p.id == "system") { p.servers = up; break; }
        return true;
    }, err);

    // 配置声称已接管，但网卡 DNS 已不含环回地址（被手工改回或系统重启释放）时修正状态
    {
        Config c = cfg.snapshot();
        if (c.takeover.active) {
            bool loop = false;
            for (auto &a : SystemDns::adapters()) {
                if (!a.up) continue;
                for (auto &ip : a.dns4) if (ip == "127.0.0.1") loop = true;
                for (auto &ip : a.dns6) if (ip == "::1") loop = true;
            }
            if (!loop) {
                std::string ierr;
                cfg.update([](Config &x) { x.takeover.active = false; return true; }, ierr);
                util::logWarn("配置记录为已接管，但网卡 DNS 未指向本机，已将接管状态标记为未接管");
            }
        }
    }

    if (opt.takeoverPlan) {
        printf("DNS Pilot %s · 接管计划（仅打印，不执行）\n", kVersion);
        int count = 0;
        for (auto &a : SystemDns::adapters()) {
            if (!a.up || !a.hasIpv4) continue;
            printf("\n[网卡] %s\n", a.alias.c_str());
            printf("  IPv4 原配置: %s", a.mode4 == "static" ? "静态" : "DHCP");
            for (auto &s : a.static4) printf(" %s", s.c_str());
            printf("\n  IPv6 原配置: %s", a.mode6 == "static" ? "静态" : "DHCP");
            for (auto &s : a.static6) printf(" %s", s.c_str());
            printf("\n  netsh interface ipv4 set dnsservers name=\"%s\" static 127.0.0.1 primary validate=no\n",
                   a.alias.c_str());
            if (a.hasIpv6 || !a.dns6.empty())
                printf("  netsh interface ipv6 set dnsservers name=\"%s\" static ::1 primary validate=no\n",
                       a.alias.c_str());
            count++;
        }
        printf("\n可接管网卡: %d 个。实际执行需要管理员权限。\n", count);
        return 0;
    }

    if (opt.status) {
        Config c = cfg.snapshot();
        printf("DNS Pilot %s\n", kVersion);
        printf("配置: %s\n", cfg.path().c_str());
        printf("权限: %s\n", util::isAdmin() ? "管理员" : "普通用户");
        printf("解析: %s:%d (TCP/UDP)\n", c.resolver.listen.c_str(), c.resolver.port);
        printf("面板: http://127.0.0.1:%d/\n", c.resolver.httpPort);
        printf("代理: %s 127.0.0.1:%d\n", c.proxy.enabled ? "启用" : "停用", c.proxy.port);
        printf("系统接管: %s（备份 %zu 项）\n", c.takeover.active ? "已接管" : "未接管",
               c.takeover.backup.size());
        printf("系统默认服务器:");
        for (auto &p : c.profiles)
            if (p.id == "system") {
                for (auto &s : p.servers) printf(" %s", s.c_str());
            }
        printf("\n");
        for (auto &a : SystemDns::adapters()) {
            if (!a.up) continue;
            printf("网卡 %s: IPv4 DNS", a.alias.c_str());
            for (auto &d : a.dns4) printf(" %s", d.c_str());
            printf(" | IPv6 DNS");
            for (auto &d : a.dns6) printf(" %s", d.c_str());
            printf("\n");
        }
        return 0;
    }

    if (opt.restore) {
        TakeoverCfg tk = cfg.takeover();
        if (tk.backup.empty()) {
            printf("没有可恢复的 DNS 备份\n");
            return 1;
        }
        TakeoverResult res;
        bool ok = SystemDns::restore(tk.backup, res, systemFallback(cfg.snapshot()));
        printf("%s\n", res.message.c_str());
        for (auto &e : res.errors) printf("  - %s\n", e.c_str());
        if (ok) cfg.setTakeover(false, {});
        return ok ? 0 : 1;
    }

    util::logInfo("DNS Pilot " + std::string(kVersion) + " 启动");
    util::logInfo("配置文件: " + cfg.path());
    util::logInfo(std::string("运行权限: ") + (util::isAdmin() ? "管理员" : "普通用户"));

    // 开机自启动：管理员启动时自动更新计划任务（自愈路径变化/被删）；普通权限只检查
    if (cfg.snapshot().ui.autoStart) {
        if (util::isAdmin()) {
            std::string amsg;
            if (autostart::enable(amsg)) util::logInfo("开机自启动任务已更新");
            else util::logWarn("更新开机自启动任务失败：" + amsg);
        } else if (!autostart::taskExists()) {
            util::logWarn("配置已开启开机自启动，但计划任务不存在；请以管理员身份重启后重新保存设置");
        }
    } else if (util::isAdmin() && autostart::taskExists()) {
        // 配置已关闭但任务残留（例如上次关闭时权限不足）：管理员启动时清理
        std::string amsg;
        if (autostart::disable(amsg)) util::logInfo("已清理残留的开机自启动任务");
        else util::logWarn("清理开机自启动任务失败：" + amsg);
    }

    Stats stats;
    ProcTracker proc;
    DnsCache cache;
    Engine engine(cfg, stats, proc, cache);
    if (!engine.start(err)) {
        util::logError(err);
        printf("\n启动失败：%s\n", err.c_str());
        return 1;
    }

    Proxy proxy(engine, cfg, proc);
    if (cfg.proxy().enabled) {
        std::string perr;
        if (!proxy.start(perr)) util::logWarn("应用代理启动失败：" + perr);
    }

    Httpd httpd(engine, cfg, stats, proxy, kVersion, relaunchArgs, [] { util::quitFlag() = true; });
    std::string herr;
    if (!httpd.start(herr)) {
        util::logError(herr);
        printf("启动失败：%s\n", herr.c_str());
        engine.stop();
        return 1;
    }

    // 启动时自动接管（ui.autoTakeover）与命令行 --takeover 共用同一套检查
    std::string autoTakeoverNotice;
    auto attemptTakeover = [&](bool retry, const char *source) {
        if (!util::isAdmin()) {
            util::logWarn(std::string(source) + "：需要管理员权限，已跳过（可用“以管理员身份重启”）");
            autoTakeoverNotice = "自动接管需要管理员权限，已跳过";
            return false;
        }
        if (cfg.takeover().active) {
            util::logInfo(std::string(source) + "：系统 DNS 已接管，跳过");
            return false;
        }
        if (cfg.resolver().port != 53) {
            util::logWarn(std::string(source) + "：接管要求解析端口为 53，当前为 " +
                          std::to_string(cfg.resolver().port));
            autoTakeoverNotice = "自动接管需要解析端口 53，已跳过";
            return false;
        }
        int attempts = retry ? 5 : 1;
        std::string terr;
        for (int i = 0; i < attempts; i++) {
            if (engine.selfTest(terr)) {
                std::vector<TakeoverBackup> backup;
                TakeoverResult res;
                if (SystemDns::apply(engine.ipv6Ready(), backup, res)) {
                    cfg.setTakeover(true, backup);
                    util::logInfo(res.message);
                    return true;
                }
                util::logError("接管失败：" + res.message);
                autoTakeoverNotice = "自动接管失败：" + res.message;
                return false;
            }
            if (i + 1 < attempts) {
                util::logWarn("自检失败，2 秒后重试：" + terr);
                Sleep(2000);
            }
        }
        util::logError("自检失败，未接管系统 DNS：" + terr);
        autoTakeoverNotice = "自动接管失败：解析自检未通过";
        return false;
    };

    if (opt.takeover) {
        attemptTakeover(false, "命令行 --takeover");
    } else if (cfg.snapshot().ui.autoTakeover) {
        util::logInfo("已开启启动时自动接管系统 DNS");
        attemptTakeover(true, "启动时自动接管");
    }

    printf("\n");
    printf("  DNS Pilot %s  ·  按应用 / 按域名 DNS 路由\n", kVersion);
    printf("  ------------------------------------------------------------\n");
    printf("  面板   http://127.0.0.1:%d/\n", cfg.resolver().httpPort);
    printf("  解析   %s:%d (UDP/TCP%s)\n", cfg.resolver().listen.c_str(), cfg.resolver().port,
           engine.ipv6Ready() ? "，含 ::1" : "");
    printf("  代理   %s\n", cfg.proxy().enabled ? ("127.0.0.1:" + std::to_string(cfg.proxy().port) +
                                                 " (SOCKS5 / HTTP CONNECT)").c_str()
                                               : "已停用");
    printf("  权限   %s\n", util::isAdmin() ? "管理员" : "普通用户");
    printf("  配置   %s\n", cfg.path().c_str());
    printf("  ------------------------------------------------------------\n");
    printf("  Ctrl+C 退出%s\n\n", cfg.takeover().active ? "（将自动恢复系统 DNS）" : "");
    fflush(stdout);

    bool guiShown = false;
    if (cfg.snapshot().ui.nativeWindow) {
        util::logInfo("打开桌面窗口");
        // 从终端启动时不要附着父控制台，否则终端关闭会连带关闭桌面窗口
        if (g_hasConsole) {
            FreeConsole();
            g_hasConsole = false;
        }
        ui::Context uctx;
        uctx.cfg = &cfg;
        uctx.stats = &stats;
        uctx.engine = &engine;
        uctx.proxy = &proxy;
        uctx.version = kVersion;
        uctx.relaunchArgs = relaunchArgs;
        uctx.startMinimized = opt.autostart;
        uctx.startupNotice = autoTakeoverNotice;
        guiShown = ui::Run(GetModuleHandleW(nullptr), uctx) == 0;
        if (!guiShown) util::logWarn("桌面窗口不可用，回退到浏览器面板");
    }
    if (!guiShown) {
        if (!opt.autostart && cfg.snapshot().ui.openBrowser)
            util::openUrl("http://127.0.0.1:" + std::to_string(cfg.resolver().httpPort) + "/");
        while (!util::quitFlag()) Sleep(200);
    }
    util::logInfo("正在退出…");
    if (cfg.takeover().active && cfg.restoreOnExit()) {
        TakeoverCfg tk = cfg.takeover();
        TakeoverResult res;
        if (SystemDns::restore(tk.backup, res, systemFallback(cfg.snapshot()))) {
            cfg.setTakeover(false, {});
            util::logInfo(res.message);
        } else {
            util::logWarn("恢复系统 DNS 失败：" + res.message +
                          "；请以管理员运行 dnspilot.exe --restore");
        }
    }
    httpd.stop();
    proxy.stop();
    engine.stop();
    WSACleanup();
    return 0;
}
