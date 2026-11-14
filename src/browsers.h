#pragma once
#include <string>
#include <vector>

struct BrowserProfileInfo {
    std::string browser;      // "chrome" | "edge" | "firefox"
    std::string browserName;  // "Chrome" | "Edge" | "Firefox"
    std::string folder;       // profile folder name, e.g. "Default"
    std::string path;         // absolute profile directory
    std::string name;         // display name from Preferences/profile.ini, fallback folder
    std::string email;        // may be empty
    bool running = false;     // browser process running (chrome.exe / msedge.exe / firefox.exe)
    bool supported = true;
    std::string mode;         // "off" | "secure" | "automatic" | "unknown"
    std::string dohUrl;       // current template/uri if any
    std::string error;        // non-empty if profile unreadable
};

namespace browsers {
std::vector<BrowserProfileInfo> scan();
// 正在运行且已开启安全 DNS（DoH）的浏览器配置：Chrome/Edge mode=secure/automatic，
// Firefox network.trr.mode=2/3。结果缓存 10 秒，只读配置文件。
std::vector<BrowserProfileInfo> secureDnsRunning();
void invalidateSecureDnsCache();
// off=true -> disable secure DNS; otherwise set secure DNS to dohUrl (must start with https://)
// Returns false with msg on failure. backupPath is set when a backup was created.
bool apply(const std::string& browser, const std::string& folder, const std::string& dohUrl,
           bool off, std::string& msg, std::string& backupPath);
}
