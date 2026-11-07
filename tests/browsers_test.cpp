// browsers.cpp 自包含控制台测试：Chrome/Edge/Firefox 配置文件 DoH 绑定
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

#include "../src/browsers.h"

namespace {

int gPass = 0;
int gFail = 0;
int gSkip = 0;

void check(bool ok, const std::string &name, const std::string &detail = std::string()) {
    if (ok) {
        ++gPass;
        std::printf("[PASS] %s\n", name.c_str());
    } else {
        ++gFail;
        if (detail.empty())
            std::printf("[FAIL] %s\n", name.c_str());
        else
            std::printf("[FAIL] %s -- %s\n", name.c_str(), detail.c_str());
    }
}

void skip(const std::string &name) {
    ++gSkip;
    std::printf("[SKIP] %s\n", name.c_str());
}

std::string toUtf8(const std::wstring &ws) {
    if (ws.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), (int)ws.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), (int)ws.size(), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring toWide(const std::string &s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    if (n <= 0) return {};
    std::wstring out((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), out.data(), n);
    return out;
}

std::string trimAscii(const std::string &s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

bool isDir(const std::wstring &p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool isFile(const std::wstring &p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool ensureDirs(const std::wstring &path) {
    std::wstring cur;
    for (size_t i = 0; i < path.size(); ++i) {
        wchar_t c = path[i];
        cur += c;
        if (c == L'\\' || c == L'/') {
            if (cur.size() > 1 && !(cur.size() == 3 && cur[1] == L':'))
                CreateDirectoryW(cur.c_str(), nullptr);
        }
    }
    if (!path.empty()) CreateDirectoryW(path.c_str(), nullptr);
    return isDir(path);
}

bool writeText(const std::wstring &path, const std::string &data) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    size_t off = 0;
    bool ok = true;
    while (off < data.size()) {
        DWORD want = (DWORD)std::min<size_t>(data.size() - off, 1u << 20);
        DWORD wrote = 0;
        if (!WriteFile(h, data.data() + off, want, &wrote, nullptr) || wrote != want) {
            ok = false;
            break;
        }
        off += wrote;
    }
    CloseHandle(h);
    return ok;
}

bool readText(const std::wstring &path, std::string &out) {
    out.clear();
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h, &size) || size.QuadPart < 0) {
        CloseHandle(h);
        return false;
    }
    out.resize((size_t)size.QuadPart);
    size_t total = 0;
    while (total < out.size()) {
        DWORD want = (DWORD)std::min<size_t>(out.size() - total, 1u << 20);
        DWORD got = 0;
        if (!ReadFile(h, out.data() + total, want, &got, nullptr)) {
            CloseHandle(h);
            out.clear();
            return false;
        }
        if (got == 0) break;
        total += got;
    }
    CloseHandle(h);
    out.resize(total);
    return true;
}

void deleteTree(const std::wstring &p) {
    DWORD a = GetFileAttributesW(p.c_str());
    if (a == INVALID_FILE_ATTRIBUTES) return;
    if (!(a & FILE_ATTRIBUTE_DIRECTORY)) {
        DeleteFileW(p.c_str());
        return;
    }
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((p + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
            deleteTree(p + L"\\" + fd.cFileName);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(p.c_str());
}

bool processRunning(const wchar_t *exe) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    bool found = false;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, exe) == 0) {
                found = true;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return found;
}

std::vector<BrowserProfileInfo> filterBrowser(const std::vector<BrowserProfileInfo> &all,
                                              const std::string &browser) {
    std::vector<BrowserProfileInfo> out;
    for (const auto &p : all)
        if (p.browser == browser) out.push_back(p);
    return out;
}

const BrowserProfileInfo *findProfile(const std::vector<BrowserProfileInfo> &v,
                                      const std::string &folder) {
    for (const auto &p : v)
        if (p.folder == folder) return &p;
    return nullptr;
}

void printProfiles(const std::vector<BrowserProfileInfo> &v) {
    if (v.empty()) {
        std::printf("  (无配置文件)\n");
        return;
    }
    for (const auto &p : v) {
        std::printf("  %s/%s | name=%s | email=%s | running=%s | mode=%s | doh=%s | error=%s\n",
                    p.browser.c_str(), p.folder.c_str(), p.name.c_str(), p.email.c_str(),
                    p.running ? "true" : "false", p.mode.c_str(), p.dohUrl.c_str(),
                    p.error.c_str());
    }
}

size_t countOccurrences(const std::string &text, const std::string &needle) {
    size_t count = 0, pos = 0;
    while ((pos = text.find(needle, pos)) != std::string::npos) {
        ++count;
        pos += needle.size();
    }
    return count;
}

// ---------------------------------------------------------------------------
// 测试侧独立的最小 JSON 值区间扫描器（用于验证“区间外字节不变”）
// ---------------------------------------------------------------------------

size_t skipWsT(const std::string &s, size_t i) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) ++i;
    return i;
}

bool skipStringT(const std::string &s, size_t &i) {
    if (i >= s.size() || s[i] != '"') return false;
    ++i;
    while (i < s.size()) {
        if (s[i] == '\\') {
            i += 2;
            continue;
        }
        if (s[i] == '"') {
            ++i;
            return true;
        }
        ++i;
    }
    return false;
}

bool skipValueT(const std::string &s, size_t &i, int depth) {
    if (depth > 128) return false;
    i = skipWsT(s, i);
    if (i >= s.size()) return false;
    char c = s[i];
    if (c == '"') return skipStringT(s, i);
    if (c == '{' || c == '[') {
        char close = (c == '{') ? '}' : ']';
        ++i;
        i = skipWsT(s, i);
        if (i < s.size() && s[i] == close) {
            ++i;
            return true;
        }
        for (;;) {
            i = skipWsT(s, i);
            if (c == '{') {
                if (!skipStringT(s, i)) return false;
                i = skipWsT(s, i);
                if (i >= s.size() || s[i] != ':') return false;
                ++i;
            }
            if (!skipValueT(s, i, depth + 1)) return false;
            i = skipWsT(s, i);
            if (i >= s.size()) return false;
            if (s[i] == ',') {
                ++i;
                continue;
            }
            if (s[i] == close) {
                ++i;
                return true;
            }
            return false;
        }
    }
    if (c == 't' && s.compare(i, 4, "true") == 0) {
        i += 4;
        return true;
    }
    if (c == 'f' && s.compare(i, 5, "false") == 0) {
        i += 5;
        return true;
    }
    if (c == 'n' && s.compare(i, 4, "null") == 0) {
        i += 4;
        return true;
    }
    if (c == '-' || (c >= '0' && c <= '9')) {
        size_t start = i;
        while (i < s.size()) {
            char d = s[i];
            if ((d >= '0' && d <= '9') || d == '-' || d == '+' || d == '.' || d == 'e' ||
                d == 'E')
                ++i;
            else
                break;
        }
        return i > start;
    }
    return false;
}

struct SpanT {
    bool found = false;
    size_t start = 0;
    size_t end = 0;
};

SpanT findValueT(const std::string &text, const std::string &key) {
    SpanT out;
    std::string needle = "\"" + key + "\"";
    size_t k = text.find(needle);
    if (k == std::string::npos) return out;
    size_t colon = text.find(':', k + needle.size());
    if (colon == std::string::npos) return out;
    size_t i = skipWsT(text, colon + 1);
    size_t start = i;
    if (!skipValueT(text, i, 0)) return out;
    out.found = true;
    out.start = start;
    out.end = i;
    return out;
}

size_t rootCloseT(const std::string &text) {
    size_t i = 0;
    if (text.size() >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB &&
        (unsigned char)text[2] == 0xBF)
        i = 3;
    i = skipWsT(text, i);
    if (i >= text.size() || text[i] != '{') return std::string::npos;
    int depth = 0;
    while (i < text.size()) {
        char c = text[i];
        if (c == '"') {
            if (!skipStringT(text, i)) return std::string::npos;
            continue;
        }
        if (c == '{') {
            ++depth;
        } else if (c == '}') {
            --depth;
            if (depth == 0) return i;
        }
        ++i;
    }
    return std::string::npos;
}

bool outsideSame(const std::string &before, const std::string &after, const std::string &key) {
    SpanT a = findValueT(before, key);
    SpanT b = findValueT(after, key);
    if (!a.found || !b.found) return false;
    return before.substr(0, a.start) == after.substr(0, b.start) &&
           before.substr(a.end) == after.substr(b.end);
}

std::string expectedInsertion(const std::string &original, const std::string &value) {
    size_t close = rootCloseT(original);
    if (close == std::string::npos) return {};
    return original.substr(0, close) + ",\"dns_over_https\":" + value + original.substr(close);
}

bool editableTrrLine(const std::string &line) {
    std::string t = trimAscii(line);
    static const char *prefixes[] = {"user_pref(\"network.trr.mode\"",
                                     "user_pref(\"network.trr.uri\"",
                                     "user_pref(\"network.trr.custom_uri\""};
    for (const char *prefix : prefixes) {
        std::string p(prefix);
        if (t.size() >= p.size() && t.compare(0, p.size(), p) == 0) return true;
    }
    return false;
}

// 去掉本工具会改写的三行后逐行比较，验证其他行完全不变。
std::vector<std::string> otherLines(const std::string &text) {
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t end = text.find_first_of("\r\n", pos);
        std::string line =
            text.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        if (!editableTrrLine(line)) out.push_back(line);
        if (end == std::string::npos) break;
        pos = end + 1;
        if (text[end] == '\r' && pos < text.size() && text[pos] == '\n') ++pos;
    }
    return out;
}

bool setEnv(const wchar_t *name, const std::wstring &value) {
    return SetEnvironmentVariableW(name, value.c_str()) != 0;
}

// ---------------------------------------------------------------------------
// 测试数据
// ---------------------------------------------------------------------------

const char *kDohUrl = "https://dns.alidns.com/dns-query";
const std::string kSecureValue =
    "{\"mode\":\"secure\",\"templates\":\"https://dns.alidns.com/dns-query\"}";
const std::string kOffValue = "{\"mode\":\"off\"}";

const std::string kChromePrefs = R"JSON({
  "profile": {
    "name": "\u6d4b\u8bd5\u7528\u6237 \"Alpha\"",
    "content_settings": {"pattern": "{\"x\":1}"}
  },
  "account_info": [
    {"email": "alpha@example.com", "account_id": "123"},
    {"email": "other@example.com"}
  ],
  "dns_over_https": {
    "mode": "automatic",
    "templates": "https://old.example/dns-query"
  },
  "big_number": 9007199254740993,
  "huge_number": 123456789012345678901234567890,
  "escaped": "line1\nline2 \"quote\" brace } { [ ]",
  "arr": [1, 2.5, -3e10, {"k": "v"}],
  "flag": true,
  "nothing": null
}
)JSON";

const std::string kProfile1Prefs = R"JSON({
  "profile": {"name": "工作配置"},
  "account_info": [{"email": "work@example.com"}],
  "nested": [1, {"brace": "}"}]
}
)JSON";

const std::string kFirefoxPrefs =
    "// Mozilla User Preferences\r\n"
    "\r\n"
    "user_pref(\"browser.startup.homepage\", \"https://example.com/\\\"quoted\\\"\");\r\n"
    "user_pref(\"some.json\", \"{\\\"brace\\\":\\\"}\\\"}\");\r\n"
    "user_pref(\"network.trr.mode\", 2);\r\n"
    "user_pref(\"network.trr.confirmationNS\", \"skip\");\r\n"
    "user_pref(\"network.trr.uri\", \"https://old.example/dns-query\");\r\n"
    "user_pref(\"network.trr.custom_uri\", \"https://custom.example/dns-query\");\r\n"
    "user_pref(\"network.trr.bootstrapAddress\", \"1.1.1.1\");\r\n";

const std::string kProfilesIni =
    "[General]\r\n"
    "StartWithLastProfile=1\r\n"
    "\r\n"
    "[Profile0]\r\n"
    "Name=测试火狐\r\n"
    "IsRelative=1\r\n"
    "Path=Profiles/abc123.default-release\r\n"
    "Default=1\r\n";

}  // namespace

int main() {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    wchar_t tmp[MAX_PATH + 2] = {0};
    DWORD tmpLen = GetTempPathW(MAX_PATH + 1, tmp);
    if (tmpLen == 0 || tmpLen > MAX_PATH) {
        std::printf("[FAIL] 无法获取临时目录\n");
        return 1;
    }
    std::string suffix;
    {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%08llx",
                      (unsigned long long)(GetTickCount64() ^ (unsigned long long)(uintptr_t)&suffix));
        suffix = buf;
    }
    std::wstring base = std::wstring(tmp) + L"dnspilot_browser_test\\" + toWide(suffix);
    if (!ensureDirs(base)) {
        std::printf("[FAIL] 无法创建测试临时目录：%s\n", toUtf8(base).c_str());
        return 1;
    }

    std::printf("=== DNS Pilot 浏览器配置测试 ===\n");
    std::printf("临时目录: %s\n\n", toUtf8(base).c_str());

    // 先隔离 Edge/Firefox，避免扫描本机真实配置
    setEnv(L"DNSPILOT_EDGE_ROOT", base + L"\\NoEdge");
    setEnv(L"DNSPILOT_FIREFOX_ROOT", base + L"\\NoFirefox");

    // ---------------- Chrome ----------------
    std::wstring chromeRoot = base + L"\\Chrome\\User Data";
    std::wstring chromeDefault = chromeRoot + L"\\Default";
    std::wstring chromeProfile1 = chromeRoot + L"\\Profile 1";
    std::wstring chromePrefsPath = chromeDefault + L"\\Preferences";
    std::wstring chromeP1PrefsPath = chromeProfile1 + L"\\Preferences";
    ensureDirs(chromeDefault);
    ensureDirs(chromeProfile1);
    check(writeText(chromePrefsPath, kChromePrefs), "创建 Chrome Default/Preferences");
    check(writeText(chromeP1PrefsPath, kProfile1Prefs), "创建 Chrome Profile 1/Preferences");
    setEnv(L"DNSPILOT_CHROME_ROOT", chromeRoot);

    std::printf("\n[Chrome scan]\n");
    std::vector<BrowserProfileInfo> all = browsers::scan();
    printProfiles(filterBrowser(all, "chrome"));

    std::vector<BrowserProfileInfo> chrome = filterBrowser(all, "chrome");
    check(chrome.size() == 2, "scan 发现 2 个 Chrome 配置文件",
          "实际数量=" + std::to_string(chrome.size()));
    const BrowserProfileInfo *def = findProfile(chrome, "Default");
    check(def != nullptr, "scan 找到 Default");
    if (def) {
        check(def->browser == "chrome" && def->browserName == "Chrome", "Default browser 字段正确");
        check(def->name == "测试用户 \"Alpha\"", "Default profile.name 解码正确", def->name);
        check(def->email == "alpha@example.com", "Default account_info[0].email 正确", def->email);
        check(def->mode == "automatic", "Default mode=automatic", def->mode);
        check(def->dohUrl == "https://old.example/dns-query", "Default templates 正确", def->dohUrl);
        check(def->supported && def->error.empty(), "Default supported 且无读取错误", def->error);
        check(def->running == processRunning(L"chrome.exe"), "Default running 与进程状态一致");
    }
    const BrowserProfileInfo *p1 = findProfile(chrome, "Profile 1");
    check(p1 != nullptr, "scan 找到 Profile 1");
    if (p1) {
        check(p1->name == "工作配置", "Profile 1 profile.name 正确", p1->name);
        check(p1->email == "work@example.com", "Profile 1 email 正确", p1->email);
        check(p1->mode == "unknown", "Profile 1 无 dns_over_https 时 mode=unknown", p1->mode);
        check(p1->dohUrl.empty(), "Profile 1 dohUrl 为空");
    }

    bool chromeRunning = processRunning(L"chrome.exe");
    if (chromeRunning) {
        skip("chrome.exe 正在运行，跳过 Chrome 写入测试（不结束真实浏览器进程）");
    } else {
        std::printf("\n[Chrome apply secure]\n");
        std::string original;
        check(readText(chromePrefsPath, original), "读取 Default 原始 Preferences");
        std::string msg, backupPath;
        bool ok = browsers::apply("chrome", "Default", kDohUrl, false, msg, backupPath);
        check(ok, "apply chrome/Default secure 成功", msg);
        check(!backupPath.empty() && isFile(toWide(backupPath)), "生成了 Preferences.dnspilot.bak",
              backupPath);

        std::string after;
        check(readText(chromePrefsPath, after), "读取写入后的 Preferences");
        SpanT span = findValueT(after, "dns_over_https");
        check(span.found && after.substr(span.start, span.end - span.start) == kSecureValue,
              "dns_over_https 值精确为 secure JSON",
              span.found ? after.substr(span.start, span.end - span.start) : std::string("未找到"));
        check(outsideSame(original, after, "dns_over_https"),
              "dns_over_https 区间外字节完全不变（含大数与转义字符串）");
        check(!isFile(chromePrefsPath + L".tmp"), "没有遗留 .tmp 临时文件");
        std::string backup;
        check(readText(toWide(backupPath), backup) && backup == original,
              "备份内容与原始文件完全一致");

        std::printf("\n[Chrome scan after secure]\n");
        std::vector<BrowserProfileInfo> chrome2 = filterBrowser(browsers::scan(), "chrome");
        printProfiles(chrome2);
        const BrowserProfileInfo *def2 = findProfile(chrome2, "Default");
        check(def2 && def2->mode == "secure", "重新 scan 后 mode=secure",
              def2 ? def2->mode : std::string("未找到"));
        check(def2 && def2->dohUrl == kDohUrl, "重新 scan 后 templates 为新 URL",
              def2 ? def2->dohUrl : std::string("未找到"));

        std::printf("\n[Chrome apply off]\n");
        std::string beforeOff = after;
        msg.clear();
        backupPath.clear();
        ok = browsers::apply("chrome", "Default", std::string(), true, msg, backupPath);
        check(ok, "apply chrome/Default off 成功", msg);
        std::string offText;
        check(readText(chromePrefsPath, offText), "读取关闭后的 Preferences");
        span = findValueT(offText, "dns_over_https");
        check(span.found && offText.substr(span.start, span.end - span.start) == kOffValue,
              "关闭后值精确为 {\"mode\":\"off\"}",
              span.found ? offText.substr(span.start, span.end - span.start) : std::string("未找到"));
        check(outsideSame(beforeOff, offText, "dns_over_https"),
              "关闭操作同样保持区间外字节不变");
        std::string backup2;
        check(backupPath.empty(), "第二次 apply 未新建备份时 backupPath 为空");
        check(readText(chromePrefsPath + L".dnspilot.bak", backup2) && backup2 == original,
              "已存在的备份未被覆盖（仍为原始内容）");
        std::vector<BrowserProfileInfo> chrome3 = filterBrowser(browsers::scan(), "chrome");
        const BrowserProfileInfo *def3 = findProfile(chrome3, "Default");
        check(def3 && def3->mode == "off", "重新 scan 后 mode=off",
              def3 ? def3->mode : std::string("未找到"));
        check(def3 && def3->dohUrl.empty(), "关闭后 templates 为空");

        std::printf("\n[Chrome insert missing key]\n");
        std::string p1Original;
        check(readText(chromeP1PrefsPath, p1Original), "读取 Profile 1 原始 Preferences");
        msg.clear();
        backupPath.clear();
        ok = browsers::apply("chrome", "Profile 1", kDohUrl, false, msg, backupPath);
        check(ok, "apply chrome/Profile 1（原文件无 dns_over_https）成功", msg);
        std::string p1After;
        check(readText(chromeP1PrefsPath, p1After), "读取 Profile 1 写入结果");
        std::string expected = expectedInsertion(p1Original, kSecureValue);
        check(!expected.empty() && p1After == expected,
              "缺失 key 时在根对象末尾精确插入 ,\"dns_over_https\":{...}");
        check(countOccurrences(p1After, "\"dns_over_https\"") == 1, "只插入一次 dns_over_https");
        check(isFile(chromeP1PrefsPath + L".dnspilot.bak"), "Profile 1 也生成了备份");
        std::vector<BrowserProfileInfo> chrome4 = filterBrowser(browsers::scan(), "chrome");
        const BrowserProfileInfo *p1AfterInfo = findProfile(chrome4, "Profile 1");
        check(p1AfterInfo && p1AfterInfo->mode == "secure", "Profile 1 重新 scan mode=secure",
              p1AfterInfo ? p1AfterInfo->mode : std::string("未找到"));

        std::printf("\n[Chrome validation]\n");
        std::string stable;
        check(readText(chromePrefsPath, stable), "读取当前 Default 用于校验失败不写入");
        msg.clear();
        backupPath.clear();
        ok = browsers::apply("chrome", "Default", "http://insecure.example/dns-query", false, msg,
                             backupPath);
        check(!ok && msg.find("https://") != std::string::npos, "拒绝非 https URL", msg);
        std::string tmpText;
        check(readText(chromePrefsPath, tmpText) && tmpText == stable, "拒绝后文件未被修改");

        std::string longUrl = "https://" + std::string(2100, 'a');
        msg.clear();
        ok = browsers::apply("chrome", "Default", longUrl, false, msg, backupPath);
        check(!ok && msg.find("过长") != std::string::npos, "拒绝 2048 字节及以上的 URL", msg);
        check(readText(chromePrefsPath, tmpText) && tmpText == stable, "长 URL 拒绝后文件未被修改");

        msg.clear();
        ok = browsers::apply("chrome", "..\\Evil", kDohUrl, false, msg, backupPath);
        check(!ok && msg.find("目录") != std::string::npos, "拒绝路径穿越目录名", msg);

        msg.clear();
        ok = browsers::apply("safari", "Default", kDohUrl, false, msg, backupPath);
        check(!ok && msg.find("不支持") != std::string::npos, "拒绝不支持的浏览器", msg);

        msg.clear();
        ok = browsers::apply("chrome", "NoSuchProfile", kDohUrl, false, msg, backupPath);
        check(!ok && msg.find("未找到") != std::string::npos, "拒绝不存在的配置文件目录", msg);
    }

    // ---------------- Firefox ----------------
    std::printf("\n[Firefox setup/scan]\n");
    std::wstring ffRoot = base + L"\\Firefox\\Profiles";
    std::wstring ffProfile = ffRoot + L"\\abc123.default-release";
    std::wstring ffPrefsPath = ffProfile + L"\\prefs.js";
    ensureDirs(ffProfile);
    check(writeText(ffPrefsPath, kFirefoxPrefs), "创建 Firefox prefs.js");
    check(writeText(base + L"\\Firefox\\profiles.ini", kProfilesIni), "创建 profiles.ini");
    setEnv(L"DNSPILOT_FIREFOX_ROOT", ffRoot);

    std::vector<BrowserProfileInfo> firefox =
        filterBrowser(browsers::scan(), "firefox");
    printProfiles(firefox);
    check(firefox.size() == 1, "scan 发现 1 个 Firefox 配置文件",
          "实际数量=" + std::to_string(firefox.size()));
    const BrowserProfileInfo *ff = findProfile(firefox, "abc123.default-release");
    check(ff != nullptr, "scan 找到 Firefox 配置文件");
    if (ff) {
        check(ff->browserName == "Firefox", "Firefox browserName 正确");
        check(ff->name == "测试火狐", "profiles.ini Name 正确", ff->name);
        check(ff->mode == "automatic", "Firefox mode=automatic", ff->mode);
        check(ff->dohUrl == "https://old.example/dns-query", "Firefox 读取 network.trr.uri",
              ff->dohUrl);
        check(ff->supported && ff->error.empty(), "Firefox supported 且无读取错误", ff->error);
        check(!ff->running, "Firefox 未运行且无锁");
    }

    std::printf("\n[Firefox apply secure]\n");
    std::string ffOriginal;
    check(readText(ffPrefsPath, ffOriginal), "读取 Firefox 原始 prefs.js");
    std::string ffMsg, ffBackup;
    bool ffOk = browsers::apply("firefox", "abc123.default-release", kDohUrl, false, ffMsg,
                                ffBackup);
    check(ffOk, "apply firefox secure 成功", ffMsg);
    check(!ffBackup.empty() && isFile(toWide(ffBackup)), "生成 prefs.js.dnspilot.bak", ffBackup);

    std::string ffAfter;
    check(readText(ffPrefsPath, ffAfter), "读取 Firefox 写入结果");
    check(ffAfter.find("user_pref(\"network.trr.mode\", 3);") != std::string::npos,
          "secure 写入 network.trr.mode = 3");
    check(ffAfter.find("user_pref(\"network.trr.uri\", \"" + std::string(kDohUrl) + "\");") !=
              std::string::npos,
          "secure 写入 network.trr.uri 新 URL");
    check(ffAfter.find("user_pref(\"network.trr.custom_uri\", \"" + std::string(kDohUrl) +
                       "\");") != std::string::npos,
          "secure 写入 network.trr.custom_uri 新 URL");
    check(countOccurrences(ffAfter, "user_pref(\"network.trr.mode\"") == 1,
          "mode 行未重复");
    check(otherLines(ffOriginal) == otherLines(ffAfter),
          "Firefox 其他行逐行保持不变（含注释、转义字符串和 trr 其他配置）");
    std::string ffBackupText;
    check(readText(toWide(ffBackup), ffBackupText) && ffBackupText == ffOriginal,
          "Firefox 备份内容与原始文件一致");

    std::vector<BrowserProfileInfo> ffScan2 = filterBrowser(browsers::scan(), "firefox");
    const BrowserProfileInfo *ff2 = findProfile(ffScan2, "abc123.default-release");
    check(ff2 && ff2->mode == "secure", "Firefox 重新 scan mode=secure",
          ff2 ? ff2->mode : std::string("未找到"));
    check(ff2 && ff2->dohUrl == kDohUrl, "Firefox 重新 scan URI 为新 URL",
          ff2 ? ff2->dohUrl : std::string("未找到"));

    std::printf("\n[Firefox apply off]\n");
    std::string ffBeforeOff = ffAfter;
    ffMsg.clear();
    ffBackup.clear();
    ffOk = browsers::apply("firefox", "abc123.default-release", std::string(), true, ffMsg,
                           ffBackup);
    check(ffOk, "apply firefox off 成功", ffMsg);
    std::string ffOff;
    check(readText(ffPrefsPath, ffOff), "读取 Firefox 关闭结果");
    check(ffOff.find("user_pref(\"network.trr.mode\", 5);") != std::string::npos,
          "off 写入 network.trr.mode = 5");
    check(ffOff.find("user_pref(\"network.trr.uri\", \"" + std::string(kDohUrl) + "\");") !=
              std::string::npos,
          "off 保留 network.trr.uri");
    check(ffOff.find("user_pref(\"network.trr.custom_uri\", \"" + std::string(kDohUrl) +
                     "\");") != std::string::npos,
          "off 保留 network.trr.custom_uri");
    check(otherLines(ffBeforeOff) == otherLines(ffOff), "off 操作其他行保持不变");
    std::string ffBackup2;
    check(ffBackup.empty(), "第二次 Firefox apply 未新建备份时 backupPath 为空");
    check(readText(ffPrefsPath + L".dnspilot.bak", ffBackup2) && ffBackup2 == ffOriginal,
          "Firefox 已存在备份未被覆盖");
    std::vector<BrowserProfileInfo> ffScan3 = filterBrowser(browsers::scan(), "firefox");
    const BrowserProfileInfo *ff3 = findProfile(ffScan3, "abc123.default-release");
    check(ff3 && ff3->mode == "off", "Firefox 重新 scan mode=off",
          ff3 ? ff3->mode : std::string("未找到"));
    check(ff3 && ff3->dohUrl == kDohUrl, "off 后 URI 仍被保留并展示",
          ff3 ? ff3->dohUrl : std::string("未找到"));

    std::printf("\n[Firefox lock simulation]\n");
    std::wstring lockPath = ffProfile + L"\\parent.lock";
    check(writeText(lockPath, std::string()), "创建模拟 parent.lock");
    std::vector<BrowserProfileInfo> ffLocked = filterBrowser(browsers::scan(), "firefox");
    const BrowserProfileInfo *ffL = findProfile(ffLocked, "abc123.default-release");
    check(ffL && ffL->running, "scan 通过 parent.lock 识别为运行中");
    std::string lockBefore;
    check(readText(ffPrefsPath, lockBefore), "读取锁定前 prefs.js");
    ffMsg.clear();
    ffOk = browsers::apply("firefox", "abc123.default-release", kDohUrl, false, ffMsg, ffBackup);
    check(!ffOk && (ffMsg.find("运行") != std::string::npos || ffMsg.find("锁定") != std::string::npos),
          "存在 parent.lock 时拒绝写入", ffMsg);
    std::string lockAfter;
    check(readText(ffPrefsPath, lockAfter) && lockAfter == lockBefore, "拒绝后 prefs.js 未被修改");
    DeleteFileW(lockPath.c_str());
    check(!isFile(lockPath), "清理 parent.lock");

    // ---------------- 清理 ----------------
    SetEnvironmentVariableW(L"DNSPILOT_CHROME_ROOT", nullptr);
    SetEnvironmentVariableW(L"DNSPILOT_EDGE_ROOT", nullptr);
    SetEnvironmentVariableW(L"DNSPILOT_FIREFOX_ROOT", nullptr);
    deleteTree(base);
    check(!isDir(base), "清理临时目录");
    // 父目录为空时一并删除（并发运行时失败无影响）
    size_t slash = base.find_last_of(L"\\/");
    if (slash != std::wstring::npos) RemoveDirectoryW(base.substr(0, slash).c_str());

    std::printf("\n汇总: PASS=%d FAIL=%d SKIP=%d\n", gPass, gFail, gSkip);
    return gFail == 0 ? 0 : 1;
}
