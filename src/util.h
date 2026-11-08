// 平台与通用工具（Win32 包装，无第三方依赖）
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <shellapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <functional>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace util {

inline std::atomic<bool> &quitFlag() {
    static std::atomic<bool> f{false};
    return f;
}

inline std::mutex &logMutex() {
    static std::mutex m;
    return m;
}

inline FILE *&logFile() {
    static FILE *f = nullptr;
    return f;
}

inline bool &verboseFlag() {
    static bool v = false;
    return v;
}

inline uint64_t nowMs() {
    using namespace std::chrono;
    return (uint64_t)duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

inline std::string timeHms(uint64_t ms) {
    (void)ms;
    SYSTEMTIME st;
    GetLocalTime(&st);
    char buf[16];
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d", st.wHour, st.wMinute, st.wSecond);
    return buf;
}

inline std::string u8(const std::wstring &w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

inline std::wstring w(const std::string &s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring out((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), out.data(), n);
    return out;
}

inline std::string exePath() {
    wchar_t buf[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(nullptr, buf, (DWORD)(sizeof(buf) / sizeof(buf[0])));
    return u8(std::wstring(buf, n));
}

inline std::string exeDir() {
    std::string p = exePath();
    size_t pos = p.find_last_of("\\/");
    return pos == std::string::npos ? "." : p.substr(0, pos);
}

inline void ensureDir(const std::string &path) {
    std::wstring wp = w(path);
    CreateDirectoryW(wp.c_str(), nullptr);
}

inline bool fileExists(const std::string &path) {
    DWORD a = GetFileAttributesW(w(path).c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

inline bool fileRead(const std::string &path, std::string &out) {
    HANDLE h = CreateFileW(w(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h, &size) || size.QuadPart > 16 * 1024 * 1024) { CloseHandle(h); return false; }
    out.resize((size_t)size.QuadPart);
    DWORD read = 0, total = 0;
    while (total < (DWORD)out.size() &&
           ReadFile(h, out.data() + total, (DWORD)out.size() - total, &read, nullptr) && read > 0)
        total += read;
    out.resize(total);
    CloseHandle(h);
    return true;
}

inline bool fileWrite(const std::string &path, const std::string &data) {
    HANDLE h = CreateFileW(w(path).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool ok = WriteFile(h, data.data(), (DWORD)data.size(), &written, nullptr) && written == data.size();
    CloseHandle(h);
    return ok;
}

inline void logLine(const char *level, const std::string &msg) {
    std::lock_guard<std::mutex> g(logMutex());
    std::string line = "[" + timeHms(nowMs()) + "] " + level + " " + msg + "\n";
    fputs(line.c_str(), stdout);
    fflush(stdout);
    if (!logFile()) {
        std::string p = exeDir() + "\\dnspilot.log";
        std::string old;
        if (fileRead(p, old) && old.size() > 4 * 1024 * 1024) old.clear();
        logFile() = _wfopen(w(p).c_str(), L"ab");
    }
    if (logFile()) {
        fputs(line.c_str(), logFile());
        fflush(logFile());
    }
}

inline void logInfo(const std::string &m) { logLine("INFO ", m); }
inline void logWarn(const std::string &m) { logLine("WARN ", m); }
inline void logError(const std::string &m) { logLine("ERROR", m); }
inline void logDebug(const std::string &m) {
    if (verboseFlag()) logLine("DEBUG", m);
}

inline std::string trim(const std::string &s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

inline std::string lowerAscii(const std::string &s) {
    std::string r = s;
    for (auto &c : r) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return r;
}

inline bool startsWith(const std::string &s, const std::string &p) {
    return s.size() >= p.size() && memcmp(s.data(), p.data(), p.size()) == 0;
}

inline bool endsWith(const std::string &s, const std::string &p) {
    return s.size() >= p.size() && memcmp(s.data() + s.size() - p.size(), p.data(), p.size()) == 0;
}

// 大小写不敏感的 glob（* 与 ?），用于 exe 规则
inline bool globMatch(const std::string &pattern, const std::string &text) {
    std::string p = lowerAscii(pattern), t = lowerAscii(text);
    size_t pi = 0, ti = 0, star = std::string::npos, mark = 0;
    while (ti < t.size()) {
        if (pi < p.size() && (p[pi] == '?' || p[pi] == t[ti])) { pi++; ti++; }
        else if (pi < p.size() && p[pi] == '*') { star = pi++; mark = ti; }
        else if (star != std::string::npos) { pi = star + 1; ti = ++mark; }
        else return false;
    }
    while (pi < p.size() && p[pi] == '*') pi++;
    return pi == p.size();
}

// 域名输入归一：允许直接粘贴网址（https://host:port/path?x#y），统一成小写主机名；
// 保留 *. 通配前缀；去掉末尾点。空结果表示输入不可用。
inline std::string normalizeDomainRule(const std::string &in) {
    std::string s = trim(in);
    size_t scheme = s.find("://");
    if (scheme != std::string::npos) s = s.substr(scheme + 3);
    size_t cut = s.find_first_of("/?#");
    if (cut != std::string::npos) s = s.substr(0, cut);
    size_t at = s.rfind('@');
    if (at != std::string::npos) s = s.substr(at + 1);
    if (!s.empty() && s[0] == '[') {
        size_t close = s.find(']');
        if (close != std::string::npos) s = s.substr(1, close - 1);
    } else {
        size_t colon = s.rfind(':');
        if (colon != std::string::npos && s.find(':') == colon) s = s.substr(0, colon);
    }
    while (!s.empty() && s.back() == '.') s.pop_back();
    return lowerAscii(trim(s));
}

// 域名后缀匹配：netflix.com 命中 netflix.com / www.netflix.com；支持 *. 前缀与通配 *
// 两侧都会先归一化，因此旧配置里存成完整 URL 的规则也能命中。
inline bool domainMatch(const std::string &ruleIn, const std::string &nameIn) {
    std::string rule = normalizeDomainRule(ruleIn);
    std::string name = normalizeDomainRule(nameIn);
    if (rule.empty() || name.empty()) return false;
    if (rule == "*") return true;
    if (startsWith(rule, "*.")) rule = rule.substr(2);
    while (!rule.empty() && rule.front() == '.') rule.erase(rule.begin());
    if (!rule.empty() && rule.back() == '.') rule.pop_back();
    if (rule.empty()) return false;
    if (name == rule) return true;
    return name.size() > rule.size() && endsWith(name, "." + rule);
}

inline std::vector<std::string> splitAny(const std::string &s, const char *seps) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (strchr(seps, c)) {
            if (!trim(cur).empty()) out.push_back(trim(cur));
            cur.clear();
        } else cur += c;
    }
    if (!trim(cur).empty()) out.push_back(trim(cur));
    return out;
}

inline std::string randHex(int bytes) {
    static thread_local std::mt19937_64 rng((uint64_t)nowMs() ^ ((uint64_t)GetCurrentThreadId() << 32) ^
                                           (uint64_t)(uintptr_t)&rng);
    static const char *hex = "0123456789abcdef";
    std::string s;
    for (int i = 0; i < bytes; i++) {
        unsigned v = (unsigned)(rng() & 0xFF);
        s += hex[v >> 4];
        s += hex[v & 0xF];
    }
    return s;
}

inline uint16_t randU16() {
    static thread_local std::mt19937 rng((uint32_t)nowMs() ^ (uint32_t)GetCurrentThreadId());
    return (uint16_t)rng();
}

inline bool isAdmin() {
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return false;
    TOKEN_ELEVATION elev{};
    DWORD len = 0;
    bool ok = GetTokenInformation(tok, TokenElevation, &elev, sizeof(elev), &len) && elev.TokenIsElevated;
    CloseHandle(tok);
    return ok;
}

// 程序图标（资源 ID 1，见 src/app.rc）；资源缺失时回退系统默认图标
inline HICON appIcon() {
    static HICON cached = nullptr;
    if (!cached) {
        cached = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1));
        if (!cached) cached = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
    }
    return cached;
}

// 以隐藏窗口执行命令（直接 CreateProcessW，避免 shell 拼接）
inline bool runHidden(const std::wstring &exe, const std::wstring &args, DWORD &exitCode) {
    std::wstring cmd = exe + L" " + args;
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    if (!CreateProcessW(exe.c_str(), buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
                        &si, &pi)) {
        exitCode = GetLastError();
        return false;
    }
    WaitForSingleObject(pi.hProcess, 15000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    exitCode = code;
    return true;
}

inline bool openUrl(const std::string &url) {
    HINSTANCE r = ShellExecuteW(nullptr, L"open", w(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    return (INT_PTR)r > 32;
}

inline bool selfElevate(const std::string &args) {
    HINSTANCE r = ShellExecuteW(nullptr, L"runas", w(exePath()).c_str(), w(args).c_str(), w(exeDir()).c_str(),
                                SW_SHOWNORMAL);
    return (INT_PTR)r > 32;
}

// 宽字符命令行参数转义（只用于重启自身）
inline std::wstring quoteArg(const std::string &arg) {
    std::wstring a = w(arg);
    if (a.find_first_of(L" \t\"") == std::wstring::npos) return a;
    std::wstring out = L"\"";
    for (wchar_t c : a) {
        if (c == L'"') out += L'\\';
        out += c;
    }
    out += L"\"";
    return out;
}

// 解析 "1.1.1.1"、"1.1.1.1:5353"、"[::1]:53"、"::1"
inline bool parseServer(const std::string &text, std::string &host, uint16_t &port) {
    std::string s = trim(text);
    port = 53;
    if (s.empty()) return false;
    if (s.front() == '[') {
        size_t close = s.find(']');
        if (close == std::string::npos) return false;
        host = s.substr(1, close - 1);
        if (close + 1 < s.size() && s[close + 1] == ':') {
            int p = atoi(s.c_str() + close + 2);
            if (p <= 0 || p > 65535) return false;
            port = (uint16_t)p;
        }
    } else {
        size_t colons = (size_t)std::count(s.begin(), s.end(), ':');
        if (colons == 1) {
            size_t pos = s.find(':');
            host = s.substr(0, pos);
            int p = atoi(s.c_str() + pos + 1);
            if (p <= 0 || p > 65535) return false;
            port = (uint16_t)p;
        } else {
            host = s;  // 裸 IPv6 不带端口
        }
    }
    sockaddr_storage ss{};
    sockaddr *sa = (sockaddr *)&ss;
    return InetPtonA(AF_INET, host.c_str(), &((sockaddr_in *)sa)->sin_addr) == 1 ||
           InetPtonA(AF_INET6, host.c_str(), &((sockaddr_in6 *)sa)->sin6_addr) == 1;
}

inline bool isLoopbackHost(const std::string &host) {
    std::string h = lowerAscii(host);
    if (h == "localhost" || h == "::1") return true;
    return startsWith(h, "127.");
}

inline std::string sockaddrToIp(const sockaddr *sa, socklen_t len) {
    char buf[INET6_ADDRSTRLEN] = {0};
    if (sa->sa_family == AF_INET)
        InetNtopA(AF_INET, &((const sockaddr_in *)sa)->sin_addr, buf, sizeof(buf));
    else if (sa->sa_family == AF_INET6)
        InetNtopA(AF_INET6, &((const sockaddr_in6 *)sa)->sin6_addr, buf, sizeof(buf));
    (void)len;
    return buf;
}

inline uint16_t sockaddrPort(const sockaddr *sa) {
    if (sa->sa_family == AF_INET) return ntohs(((const sockaddr_in *)sa)->sin_port);
    if (sa->sa_family == AF_INET6) return ntohs(((const sockaddr_in6 *)sa)->sin6_port);
    return 0;
}

inline bool makeSockaddr(const std::string &host, uint16_t port, sockaddr_storage &ss, socklen_t &len) {
    memset(&ss, 0, sizeof(ss));
    sockaddr_in *v4 = (sockaddr_in *)&ss;
    if (InetPtonA(AF_INET, host.c_str(), &v4->sin_addr) == 1) {
        v4->sin_family = AF_INET;
        v4->sin_port = htons(port);
        len = sizeof(sockaddr_in);
        return true;
    }
    sockaddr_in6 *v6 = (sockaddr_in6 *)&ss;
    if (InetPtonA(AF_INET6, host.c_str(), &v6->sin6_addr) == 1) {
        v6->sin6_family = AF_INET6;
        v6->sin6_port = htons(port);
        len = sizeof(sockaddr_in6);
        return true;
    }
    return false;
}

class WorkerPool {
  public:
    void start(int n) {
        std::lock_guard<std::mutex> g(mu_);
        if (!threads_.empty()) return;
        stop_ = false;
        for (int i = 0; i < n; i++) threads_.emplace_back([this] { loop(); });
    }
    void stop() {
        {
            std::lock_guard<std::mutex> g(mu_);
            stop_ = true;
        }
        cv_.notify_all();
        for (auto &t : threads_)
            if (t.joinable()) t.join();
        threads_.clear();
        std::lock_guard<std::mutex> g(mu_);
        q_.clear();
    }
    void post(std::function<void()> f) {
        {
            std::lock_guard<std::mutex> g(mu_);
            if (stop_) return;
            q_.push_back(std::move(f));
        }
        cv_.notify_one();
    }

  private:
    void loop() {
        for (;;) {
            std::function<void()> job;
            {
                std::unique_lock<std::mutex> lk(mu_);
                cv_.wait(lk, [this] { return stop_ || !q_.empty(); });
                if (stop_ && q_.empty()) return;
                job = std::move(q_.front());
                q_.pop_front();
            }
            try { job(); } catch (...) {}
        }
    }
    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> q_;
    std::vector<std::thread> threads_;
    bool stop_ = false;
};

}  // namespace util
