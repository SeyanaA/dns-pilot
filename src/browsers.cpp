// 浏览器配置文件级别的安全 DNS（DoH）绑定
// 只改写浏览器自身配置文件中的 dns_over_https / network.trr.* 片段，
// 其余字节保持原样；写入前备份，写入使用临时文件 + MoveFileEx 原子替换。
#include "browsers.h"

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
#include <utility>
#include <vector>

namespace {

constexpr size_t kMaxFileBytes = 64ull * 1024ull * 1024ull;

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

std::string lowerAscii(const std::string &s) {
    std::string r = s;
    for (auto &c : r)
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return r;
}

std::wstring lowerWide(const std::wstring &s) {
    std::wstring r = s;
    for (auto &c : r)
        if (c >= L'A' && c <= L'Z') c = (wchar_t)(c - L'A' + 32);
    return r;
}

std::string trimAscii(const std::string &s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::wstring joinPath(const std::wstring &a, const std::wstring &b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    if (a.back() == L'\\' || a.back() == L'/') return a + b;
    return a + L"\\" + b;
}

std::wstring envValue(const wchar_t *name) {
    DWORD n = GetEnvironmentVariableW(name, nullptr, 0);
    if (n == 0) return {};
    std::wstring v((size_t)n, L'\0');
    DWORD got = GetEnvironmentVariableW(name, v.data(), n);
    if (got == 0 || got >= n) return {};
    v.resize((size_t)got);
    return v;
}

std::wstring normalizePath(const std::wstring &p) {
    if (p.empty()) return {};
    DWORD n = GetFullPathNameW(p.c_str(), 0, nullptr, nullptr);
    if (n == 0) return p;
    std::wstring buf((size_t)n, L'\0');
    DWORD m = GetFullPathNameW(p.c_str(), n, buf.data(), nullptr);
    if (m == 0) return p;
    buf.resize((size_t)m);
    return buf;
}

std::wstring parentDir(const std::wstring &p) {
    std::wstring s = p;
    while (s.size() > 3 && (s.back() == L'\\' || s.back() == L'/')) s.pop_back();
    size_t pos = s.find_last_of(L"\\/");
    if (pos == std::wstring::npos) return L".";
    if (pos == 2 && s.size() > 1 && s[1] == L':') return s.substr(0, 3);
    return s.substr(0, pos);
}

bool isFile(const std::wstring &p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool isDir(const std::wstring &p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool withinRoot(const std::wstring &root, const std::wstring &path) {
    std::wstring r = normalizePath(root);
    std::wstring p = normalizePath(path);
    while (!r.empty() && (r.back() == L'\\' || r.back() == L'/')) r.pop_back();
    if (r.empty()) return false;
    std::wstring prefix = r + L"\\";
    return p.size() > prefix.size() && _wcsnicmp(p.c_str(), prefix.c_str(), prefix.size()) == 0;
}

bool safeFolderName(const std::string &folder) {
    if (folder.empty() || folder == "." || folder == "..") return false;
    for (unsigned char c : folder) {
        if (c < 0x20 || c == 0x7f) return false;
        if (std::strchr("\\/:*?\"<>|", (int)c) != nullptr) return false;
    }
    if (folder.back() == ' ' || folder.back() == '.') return false;
    return true;
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

bool readFileBytes(const std::wstring &path, std::string &out, DWORD &err) {
    out.clear();
    err = 0;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        err = GetLastError();
        return false;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h, &size)) {
        err = GetLastError();
        CloseHandle(h);
        return false;
    }
    if (size.QuadPart < 0 || (uint64_t)size.QuadPart > kMaxFileBytes) {
        err = ERROR_FILE_TOO_LARGE;
        CloseHandle(h);
        return false;
    }
    out.resize((size_t)size.QuadPart);
    size_t total = 0;
    while (total < out.size()) {
        DWORD want = (DWORD)std::min<size_t>(out.size() - total, 1u << 20);
        DWORD got = 0;
        if (!ReadFile(h, out.data() + total, want, &got, nullptr)) {
            err = GetLastError();
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

// 先写 <path>.tmp，再以 MoveFileExW 原子替换目标文件。
bool atomicWrite(const std::wstring &path, const std::string &data, DWORD &err) {
    err = 0;
    std::wstring tmp = path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        err = GetLastError();
        return false;
    }
    size_t off = 0;
    while (off < data.size()) {
        DWORD chunk = (DWORD)std::min<size_t>(data.size() - off, 1u << 20);
        DWORD wrote = 0;
        if (!WriteFile(h, data.data() + off, chunk, &wrote, nullptr) || wrote != chunk) {
            err = GetLastError();
            CloseHandle(h);
            DeleteFileW(tmp.c_str());
            return false;
        }
        off += wrote;
    }
    if (!FlushFileBuffers(h)) {
        err = GetLastError();
        CloseHandle(h);
        DeleteFileW(tmp.c_str());
        return false;
    }
    CloseHandle(h);
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        err = GetLastError();
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

bool ensureBackup(const std::wstring &src, const std::wstring &dst, bool &created, DWORD &err) {
    created = false;
    err = 0;
    if (GetFileAttributesW(dst.c_str()) != INVALID_FILE_ATTRIBUTES) return true;  // 已有备份不覆盖
    if (!CopyFileW(src.c_str(), dst.c_str(), TRUE)) {
        err = GetLastError();
        return false;
    }
    created = true;
    return true;
}

void listDirs(const std::wstring &root, std::vector<std::wstring> &out) {
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((root + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        out.push_back(fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

std::wstring chromiumRoot(const std::string &browser) {
    std::wstring r = envValue(browser == "chrome" ? L"DNSPILOT_CHROME_ROOT" : L"DNSPILOT_EDGE_ROOT");
    if (r.empty()) {
        std::wstring local = envValue(L"LOCALAPPDATA");
        if (local.empty()) return {};
        r = joinPath(local, browser == "chrome" ? L"Google\\Chrome\\User Data"
                                                : L"Microsoft\\Edge\\User Data");
    }
    return normalizePath(r);
}

std::wstring firefoxRoot() {
    std::wstring r = envValue(L"DNSPILOT_FIREFOX_ROOT");
    if (r.empty()) {
        std::wstring app = envValue(L"APPDATA");
        if (app.empty()) return {};
        r = joinPath(app, L"Mozilla\\Firefox\\Profiles");
    }
    return normalizePath(r);
}

std::string fileErr(const char *what, DWORD err) {
    return std::string(what) + "（错误码 " + std::to_string((unsigned long)err) + "）";
}

// ---------------------------------------------------------------------------
// 容错 JSON 扫描：只定位文本片段，不反序列化、不重排。
// ---------------------------------------------------------------------------

size_t skipWs(const std::string &s, size_t i, size_t limit) {
    while (i < limit) {
        char c = s[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            ++i;
        } else {
            break;
        }
    }
    return i;
}

int hexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

void appendUtf8(std::string &out, unsigned cp) {
    if (cp <= 0x7F) {
        out += (char)cp;
    } else if (cp <= 0x7FF) {
        out += (char)(0xC0 | (cp >> 6));
        out += (char)(0x80 | (cp & 0x3F));
    } else if (cp <= 0xFFFF) {
        out += (char)(0xE0 | (cp >> 12));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    } else {
        out += (char)(0xF0 | (cp >> 18));
        out += (char)(0x80 | ((cp >> 12) & 0x3F));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    }
}

struct StrLit {
    std::string text;
    size_t next = 0;
    bool ok = false;
};

StrLit parseJsonString(const std::string &s, size_t i, size_t limit) {
    StrLit r;
    if (i >= limit || s[i] != '"') return r;
    ++i;
    std::string out;
    while (i < limit) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"') {
            r.text = std::move(out);
            r.next = i + 1;
            r.ok = true;
            return r;
        }
        if (c == '\\') {
            ++i;
            if (i >= limit) return r;
            char e = s[i++];
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    if (i + 4 > limit) return r;
                    unsigned cp = 0;
                    for (int k = 0; k < 4; ++k) {
                        int hv = hexVal(s[i + (size_t)k]);
                        if (hv < 0) return r;
                        cp = (cp << 4) | (unsigned)hv;
                    }
                    i += 4;
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        unsigned low = 0;
                        bool paired = false;
                        if (i + 6 <= limit && s[i] == '\\' && s[i + 1] == 'u') {
                            bool okLow = true;
                            for (int k = 0; k < 4; ++k) {
                                int hv = hexVal(s[i + 2 + (size_t)k]);
                                if (hv < 0) {
                                    okLow = false;
                                    break;
                                }
                                low = (low << 4) | (unsigned)hv;
                            }
                            if (okLow && low >= 0xDC00 && low <= 0xDFFF) {
                                cp = 0x10000u + ((cp - 0xD800u) << 10) + (low - 0xDC00u);
                                i += 6;
                                paired = true;
                            }
                        }
                        if (!paired) cp = 0xFFFD;
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        cp = 0xFFFD;
                    }
                    appendUtf8(out, cp);
                    break;
                }
                default: out += e; break;  // 容错：未知转义按字面字符保留
            }
            continue;
        }
        out += (char)c;
        ++i;
    }
    return r;
}

bool skipJsonString(const std::string &s, size_t &i, size_t limit) {
    if (i >= limit || s[i] != '"') return false;
    ++i;
    while (i < limit) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"') {
            ++i;
            return true;
        }
        if (c == '\\') {
            i += 2;
            continue;
        }
        ++i;
    }
    return false;
}

bool skipJsonValue(const std::string &s, size_t &i, size_t limit, int depth) {
    if (depth > 128) return false;
    i = skipWs(s, i, limit);
    if (i >= limit) return false;
    char c = s[i];
    if (c == '"') return skipJsonString(s, i, limit);

    if (c == '{' || c == '[') {
        char close = (c == '{') ? '}' : ']';
        ++i;
        i = skipWs(s, i, limit);
        if (i < limit && s[i] == close) {
            ++i;
            return true;
        }
        for (;;) {
            i = skipWs(s, i, limit);
            if (i >= limit) return false;
            if (c == '{') {
                if (!skipJsonString(s, i, limit)) return false;
                i = skipWs(s, i, limit);
                if (i >= limit || s[i] != ':') return false;
                ++i;
            }
            if (!skipJsonValue(s, i, limit, depth + 1)) return false;
            i = skipWs(s, i, limit);
            if (i >= limit) return false;
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

    if (c == 't') {
        if (limit - i >= 4 && std::memcmp(s.data() + i, "true", 4) == 0) {
            i += 4;
            return true;
        }
        return false;
    }
    if (c == 'f') {
        if (limit - i >= 5 && std::memcmp(s.data() + i, "false", 5) == 0) {
            i += 5;
            return true;
        }
        return false;
    }
    if (c == 'n') {
        if (limit - i >= 4 && std::memcmp(s.data() + i, "null", 4) == 0) {
            i += 4;
            return true;
        }
        return false;
    }
    if (c == '-' || (c >= '0' && c <= '9')) {
        size_t start = i;
        while (i < limit) {
            char d = s[i];
            if ((d >= '0' && d <= '9') || d == '-' || d == '+' || d == '.' || d == 'e' || d == 'E') {
                ++i;
            } else {
                break;
            }
        }
        return i > start;
    }
    return false;
}

struct RootScan {
    bool isObject = false;
    bool valid = false;
    bool hasMembers = false;
    bool found = false;
    size_t open = std::string::npos;
    size_t close = std::string::npos;
    size_t valueStart = std::string::npos;
    size_t valueEnd = std::string::npos;
};

// 扫描根对象成员，并记录根级 key 的值区间（字符串/转义/括号感知）。
RootScan scanRoot(const std::string &s, const std::string &key) {
    RootScan r;
    size_t n = s.size();
    size_t i = 0;
    if (n >= 3 && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB &&
        (unsigned char)s[2] == 0xBF)
        i = 3;
    i = skipWs(s, i, n);
    if (i >= n || s[i] != '{') return r;
    r.isObject = true;
    r.open = i;
    i = skipWs(s, i + 1, n);
    if (i < n && s[i] == '}') {
        r.close = i;
        r.valid = true;
        return r;
    }
    for (;;) {
        StrLit k = parseJsonString(s, i, n);
        if (!k.ok) return r;
        i = skipWs(s, k.next, n);
        if (i >= n || s[i] != ':') return r;
        ++i;
        i = skipWs(s, i, n);
        size_t vs = i;
        if (!skipJsonValue(s, i, n, 0)) return r;
        size_t ve = i;
        r.hasMembers = true;
        if (k.text == key) {
            r.found = true;
            r.valueStart = vs;
            r.valueEnd = ve;
        }
        i = skipWs(s, i, n);
        if (i >= n) return r;
        if (s[i] == ',') {
            i = skipWs(s, i + 1, n);
            continue;
        }
        if (s[i] == '}') {
            r.close = i;
            r.valid = true;
            return r;
        }
        return r;
    }
}

// 在 [open, limit) 的对象里查找成员，返回其值区间。
bool findMember(const std::string &s, size_t open, size_t limit, const std::string &key,
                size_t &valueStart, size_t &valueEnd, bool &found) {
    found = false;
    if (open >= limit || open >= s.size() || s[open] != '{') return false;
    size_t i = skipWs(s, open + 1, limit);
    if (i >= limit) return false;
    if (s[i] == '}') return true;
    for (;;) {
        StrLit k = parseJsonString(s, i, limit);
        if (!k.ok) return false;
        i = skipWs(s, k.next, limit);
        if (i >= limit || s[i] != ':') return false;
        ++i;
        i = skipWs(s, i, limit);
        size_t vs = i;
        if (!skipJsonValue(s, i, limit, 0)) return false;
        size_t ve = i;
        if (k.text == key) {
            valueStart = vs;
            valueEnd = ve;
            found = true;
        }
        i = skipWs(s, i, limit);
        if (i >= limit) return false;
        if (s[i] == ',') {
            i = skipWs(s, i + 1, limit);
            continue;
        }
        if (s[i] == '}') return true;
        return false;
    }
}

bool firstArrayObject(const std::string &s, size_t vs, size_t ve, size_t &objStart,
                      size_t &objEnd) {
    if (vs >= s.size() || s[vs] != '[') return false;
    size_t i = skipWs(s, vs + 1, ve);
    if (i >= ve || s[i] == ']') return false;
    if (s[i] != '{') return false;
    size_t e = i;
    if (!skipJsonValue(s, e, ve, 0)) return false;
    objStart = i;
    objEnd = e;
    return true;
}

bool jsonStringMember(const std::string &s, size_t open, size_t limit, const std::string &key,
                      std::string &out) {
    size_t vs = 0, ve = 0;
    bool found = false;
    if (!findMember(s, open, limit, key, vs, ve, found) || !found) return false;
    if (vs >= ve || vs >= s.size() || s[vs] != '"') return false;
    StrLit v = parseJsonString(s, vs, ve);
    if (!v.ok) return false;
    out = std::move(v.text);
    return true;
}

void readChromiumPreferences(const std::string &text, std::string &name, std::string &email,
                             std::string &mode, std::string &dohUrl) {
    name.clear();
    email.clear();
    mode = "unknown";
    dohUrl.clear();

    RootScan root = scanRoot(text, "dns_over_https");
    if (!root.isObject || !root.valid || root.close == std::string::npos) return;

    size_t vs = 0, ve = 0, objStart = 0, objEnd = 0;
    bool found = false;

    if (findMember(text, root.open, root.close + 1, "profile", vs, ve, found) && found &&
        vs < ve && text[vs] == '{') {
        jsonStringMember(text, vs, ve, "name", name);
    }
    if (findMember(text, root.open, root.close + 1, "account_info", vs, ve, found) && found &&
        vs < ve && text[vs] == '[') {
        if (firstArrayObject(text, vs, ve, objStart, objEnd)) {
            jsonStringMember(text, objStart, objEnd, "email", email);
        }
    }
    if (root.found && root.valueStart < root.valueEnd && text[root.valueStart] == '{') {
        std::string m;
        if (jsonStringMember(text, root.valueStart, root.valueEnd, "mode", m)) {
            std::string ml = lowerAscii(m);
            if (ml == "off" || ml == "secure" || ml == "automatic")
                mode = ml;
            else
                mode = "unknown";
        }
        std::string t;
        if (jsonStringMember(text, root.valueStart, root.valueEnd, "templates", t)) dohUrl = t;
    }
}

std::string jsonEscape(const std::string &in) {
    std::string out;
    out.reserve(in.size() + 8);
    for (unsigned char c : in) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", (unsigned)c);
                    out += buf;
                } else {
                    out += (char)c;
                }
                break;
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Firefox prefs.js
// ---------------------------------------------------------------------------

struct PrefEntry {
    std::string key;
    std::string value;
    bool isString = false;
};

bool parsePrefLine(const std::string &line, PrefEntry &out) {
    size_t i = 0, n = line.size();
    i = skipWs(line, i, n);
    if (i >= n || line.compare(i, 9, "user_pref") != 0) return false;
    i += 9;
    i = skipWs(line, i, n);
    if (i >= n || line[i] != '(') return false;
    ++i;
    i = skipWs(line, i, n);
    if (i >= n || line[i] != '"') return false;
    StrLit k = parseJsonString(line, i, n);
    if (!k.ok) return false;
    i = skipWs(line, k.next, n);
    if (i >= n || line[i] != ',') return false;
    ++i;
    i = skipWs(line, i, n);
    if (i >= n) return false;
    out.key = std::move(k.text);
    out.value.clear();
    out.isString = false;
    if (line[i] == '"') {
        StrLit v = parseJsonString(line, i, n);
        if (!v.ok) return false;
        out.value = std::move(v.text);
        out.isString = true;
    } else {
        size_t start = i;
        while (i < n && line[i] != ',' && line[i] != ')') ++i;
        out.value = trimAscii(line.substr(start, i - start));
    }
    return true;
}

void readFirefoxPrefs(const std::string &text, std::string &mode, std::string &dohUrl) {
    mode = "unknown";
    dohUrl.clear();
    bool hasMode = false;
    std::string uri, custom;
    size_t pos = 0;
    bool first = true;
    while (pos <= text.size()) {
        size_t end = text.find_first_of("\r\n", pos);
        std::string line =
            text.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        size_t off = 0;
        if (first && line.size() >= 3 && (unsigned char)line[0] == 0xEF &&
            (unsigned char)line[1] == 0xBB && (unsigned char)line[2] == 0xBF)
            off = 3;
        first = false;
        PrefEntry pe;
        if (parsePrefLine(line.substr(off), pe)) {
            if (pe.key == "network.trr.mode") {
                hasMode = true;
                std::string v = trimAscii(pe.value);
                if (v == "3")
                    mode = "secure";
                else if (v == "2")
                    mode = "automatic";
                else if (v == "5" || v == "0")
                    mode = "off";
                else
                    mode = "unknown";
            } else if (pe.key == "network.trr.uri") {
                uri = pe.value;
            } else if (pe.key == "network.trr.custom_uri") {
                custom = pe.value;
            }
        }
        if (end == std::string::npos) break;
        pos = end + 1;
        if (text[end] == '\r' && pos < text.size() && text[pos] == '\n') ++pos;
    }
    if (!hasMode) mode = "unknown";
    dohUrl = !uri.empty() ? uri : custom;
}

std::string prefStringLine(const std::string &key, const std::string &value) {
    return "user_pref(\"" + key + "\", \"" + jsonEscape(value) + "\");";
}

std::string rewriteFirefoxPrefs(const std::string &text, bool off, const std::string &url) {
    std::string newline = "\r\n";
    if (text.find("\r\n") != std::string::npos)
        newline = "\r\n";
    else if (text.find('\n') != std::string::npos)
        newline = "\n";
    else if (text.find('\r') != std::string::npos)
        newline = "\r";

    std::string out;
    out.reserve(text.size() + 256);
    size_t i = 0;
    bool first = true;
    bool sawMode = false, sawUri = false, sawCustom = false;

    while (i < text.size()) {
        size_t start = i;
        size_t eol = i;
        while (eol < text.size() && text[eol] != '\n' && text[eol] != '\r') ++eol;
        std::string line = text.substr(start, eol - start);
        std::string ending;
        size_t next = eol;
        if (next < text.size()) {
            if (text[next] == '\r') {
                if (next + 1 < text.size() && text[next + 1] == '\n') {
                    ending = "\r\n";
                    next += 2;
                } else {
                    ending = "\r";
                    ++next;
                }
            } else {
                ending = "\n";
                ++next;
            }
        }

        std::string bom;
        size_t bodyStart = 0;
        if (first && line.size() >= 3 && (unsigned char)line[0] == 0xEF &&
            (unsigned char)line[1] == 0xBB && (unsigned char)line[2] == 0xBF) {
            bom = line.substr(0, 3);
            bodyStart = 3;
        }
        first = false;
        std::string body = line.substr(bodyStart);
        PrefEntry pe;
        bool matches = parsePrefLine(body, pe);
        bool replace = false;
        std::string replacement;

        if (matches && pe.key == "network.trr.mode") {
            sawMode = true;
            replace = true;
            replacement = std::string("user_pref(\"network.trr.mode\", ") + (off ? "5" : "3") + ");";
        } else if (matches && !off && pe.key == "network.trr.uri") {
            sawUri = true;
            replace = true;
            replacement = prefStringLine("network.trr.uri", url);
        } else if (matches && !off && pe.key == "network.trr.custom_uri") {
            sawCustom = true;
            replace = true;
            replacement = prefStringLine("network.trr.custom_uri", url);
        }

        if (replace) {
            size_t indentEnd = 0;
            while (indentEnd < body.size() && (body[indentEnd] == ' ' || body[indentEnd] == '\t'))
                ++indentEnd;
            out += bom;
            out += body.substr(0, indentEnd);
            out += replacement;
            out += ending;
        } else {
            out += line;
            out += ending;
        }
        i = next;
    }

    std::string block;
    if (!sawMode) block += std::string("user_pref(\"network.trr.mode\", ") + (off ? "5" : "3") + ");" + newline;
    if (!off) {
        if (!sawUri) block += prefStringLine("network.trr.uri", url) + newline;
        if (!sawCustom) block += prefStringLine("network.trr.custom_uri", url) + newline;
    }
    if (!block.empty()) {
        if (!out.empty() && out.back() != '\n' && out.back() != '\r') out += newline;
        out += block;
    }
    return out;
}

struct IniEntry {
    std::string name;
    std::string path;
    bool isRelative = true;
};

std::wstring profilesIniPath(const std::wstring &root) {
    std::wstring a = joinPath(parentDir(root), L"profiles.ini");
    if (isFile(a)) return a;
    std::wstring b = joinPath(root, L"profiles.ini");
    if (isFile(b)) return b;
    return {};
}

std::vector<IniEntry> loadProfilesIni(const std::wstring &path) {
    std::vector<IniEntry> out;
    std::string text;
    DWORD err = 0;
    if (!readFileBytes(path, text, err)) return out;

    IniEntry cur;
    bool inProfile = false;
    auto flush = [&]() {
        if (inProfile && !cur.path.empty()) out.push_back(cur);
        cur = IniEntry{};
    };

    size_t pos = 0;
    bool first = true;
    while (pos <= text.size()) {
        size_t end = text.find_first_of("\r\n", pos);
        std::string line =
            text.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        if (first && line.size() >= 3 && (unsigned char)line[0] == 0xEF &&
            (unsigned char)line[1] == 0xBB && (unsigned char)line[2] == 0xBF)
            line = line.substr(3);
        first = false;
        std::string t = trimAscii(line);
        if (!t.empty() && t[0] != ';' && t[0] != '#') {
            if (t.front() == '[') {
                flush();
                std::string lower = lowerAscii(t);
                inProfile = lower.size() >= 8 && lower.compare(0, 8, "[profile") == 0;
            } else if (inProfile) {
                size_t eq = t.find('=');
                if (eq != std::string::npos) {
                    std::string key = lowerAscii(trimAscii(t.substr(0, eq)));
                    std::string val = trimAscii(t.substr(eq + 1));
                    if (key == "name")
                        cur.name = val;
                    else if (key == "path")
                        cur.path = val;
                    else if (key == "isrelative")
                        cur.isRelative = (val != "0");
                }
            }
        }
        if (end == std::string::npos) break;
        pos = end + 1;
        if (text[end] == '\r' && pos < text.size() && text[pos] == '\n') ++pos;
    }
    flush();
    return out;
}

std::string firefoxDisplayName(const std::vector<IniEntry> &ini, const std::string &folder,
                               const std::wstring &fullPath) {
    std::string folderLower = lowerAscii(folder);
    std::wstring fullNorm = lowerWide(normalizePath(fullPath));
    for (const auto &e : ini) {
        std::string p = e.path;
        for (auto &c : p)
            if (c == '/') c = '\\';
        std::string pl = lowerAscii(p);
        bool match = false;
        if (e.isRelative) {
            if (pl == folderLower) {
                match = true;
            } else if (pl.size() > folderLower.size()) {
                std::string suffix = "\\" + folderLower;
                if (pl.size() >= suffix.size() &&
                    pl.compare(pl.size() - suffix.size(), suffix.size(), suffix) == 0)
                    match = true;
            }
        } else {
            match = (lowerWide(normalizePath(toWide(p))) == fullNorm);
        }
        if (match && !e.name.empty()) return e.name;
    }
    return folder;
}

bool firefoxLocked(const std::wstring &dir) {
    return isFile(joinPath(dir, L"parent.lock")) || isFile(joinPath(dir, L".parentlock"));
}

// ---------------------------------------------------------------------------
// 扫描
// ---------------------------------------------------------------------------

void scanChromium(const std::string &browser, std::vector<BrowserProfileInfo> &out) {
    std::wstring root = chromiumRoot(browser);
    if (root.empty() || !isDir(root)) return;
    bool running = processRunning(browser == "chrome" ? L"chrome.exe" : L"msedge.exe");

    std::vector<std::wstring> dirs;
    listDirs(root, dirs);
    std::sort(dirs.begin(), dirs.end(), [](const std::wstring &a, const std::wstring &b) {
        return _wcsicmp(a.c_str(), b.c_str()) < 0;
    });

    for (const auto &nameW : dirs) {
        std::wstring dir = joinPath(root, nameW);
        std::wstring prefs = joinPath(dir, L"Preferences");
        if (!isFile(prefs)) continue;

        BrowserProfileInfo info;
        info.browser = browser;
        info.browserName = (browser == "chrome") ? "Chrome" : "Edge";
        info.folder = toUtf8(nameW);
        info.path = toUtf8(normalizePath(dir));
        info.name = info.folder;
        info.running = running;
        info.supported = true;
        info.mode = "unknown";

        std::string text;
        DWORD err = 0;
        if (!readFileBytes(prefs, text, err)) {
            info.error = fileErr("无法读取 Preferences", err);
        } else {
            readChromiumPreferences(text, info.name, info.email, info.mode, info.dohUrl);
            if (info.name.empty()) info.name = info.folder;
        }
        out.push_back(std::move(info));
    }
}

void scanFirefox(std::vector<BrowserProfileInfo> &out) {
    std::wstring root = firefoxRoot();
    if (root.empty() || !isDir(root)) return;
    bool running = processRunning(L"firefox.exe");

    std::vector<IniEntry> ini;
    std::wstring iniPath = profilesIniPath(root);
    if (!iniPath.empty()) ini = loadProfilesIni(iniPath);

    std::vector<std::wstring> dirs;
    listDirs(root, dirs);
    std::sort(dirs.begin(), dirs.end(), [](const std::wstring &a, const std::wstring &b) {
        return _wcsicmp(a.c_str(), b.c_str()) < 0;
    });

    for (const auto &nameW : dirs) {
        std::wstring dir = joinPath(root, nameW);
        std::wstring prefs = joinPath(dir, L"prefs.js");
        if (!isFile(prefs)) continue;

        BrowserProfileInfo info;
        info.browser = "firefox";
        info.browserName = "Firefox";
        info.folder = toUtf8(nameW);
        info.path = toUtf8(normalizePath(dir));
        info.name = firefoxDisplayName(ini, info.folder, dir);
        info.running = running || firefoxLocked(dir);
        info.supported = true;
        info.mode = "unknown";

        std::string text;
        DWORD err = 0;
        if (!readFileBytes(prefs, text, err)) {
            info.error = fileErr("无法读取 prefs.js", err);
        } else {
            readFirefoxPrefs(text, info.mode, info.dohUrl);
        }
        out.push_back(std::move(info));
    }
}

// ---------------------------------------------------------------------------
// 写入
// ---------------------------------------------------------------------------

bool applyChromium(const std::string &browser, const std::string &folder, const std::string &dohUrl,
                   bool off, std::string &msg, std::string &backupPath) {
    const std::string displayName = (browser == "chrome") ? "Chrome" : "Edge";

    if (!safeFolderName(folder)) {
        msg = "配置文件目录名无效或包含路径穿越字符";
        return false;
    }
    std::wstring root = chromiumRoot(browser);
    if (root.empty()) {
        msg = "无法确定浏览器用户数据目录";
        return false;
    }
    const wchar_t *exe = (browser == "chrome") ? L"chrome.exe" : L"msedge.exe";
    if (processRunning(exe)) {
        msg = "浏览器正在运行，请完全退出后重试";
        return false;
    }
    std::wstring dir = joinPath(root, toWide(folder));
    if (!withinRoot(root, dir)) {
        msg = "配置文件目录不在浏览器用户数据目录内";
        return false;
    }
    if (!isDir(dir)) {
        msg = "未找到浏览器配置文件目录：" + toUtf8(dir);
        return false;
    }
    std::wstring prefs = joinPath(dir, L"Preferences");
    if (!isFile(prefs)) {
        msg = "未找到 Preferences 文件：" + toUtf8(prefs);
        return false;
    }
    if (!off) {
        if (dohUrl.compare(0, 8, "https://") != 0) {
            msg = "DoH 地址必须以 https:// 开头";
            return false;
        }
        if (dohUrl.size() >= 2048) {
            msg = "DoH 地址过长（上限 2047 字节）";
            return false;
        }
    }

    std::string text;
    DWORD err = 0;
    if (!readFileBytes(prefs, text, err)) {
        msg = fileErr("无法读取 Preferences", err);
        return false;
    }

    RootScan rootScan = scanRoot(text, "dns_over_https");
    if (!rootScan.isObject || !rootScan.valid || rootScan.close == std::string::npos) {
        msg = "Preferences 内容不是有效的 JSON 对象，已取消修改";
        return false;
    }

    std::string value = off ? std::string("{\"mode\":\"off\"}")
                            : std::string("{\"mode\":\"secure\",\"templates\":\"") +
                                  jsonEscape(dohUrl) + "\"}";

    std::string updated;
    if (rootScan.found && rootScan.valueStart <= rootScan.valueEnd) {
        updated = text.substr(0, rootScan.valueStart) + value + text.substr(rootScan.valueEnd);
    } else {
        std::string insert =
            (rootScan.hasMembers ? "," : "") + std::string("\"dns_over_https\":") + value;
        updated = text.substr(0, rootScan.close) + insert + text.substr(rootScan.close);
    }

    std::wstring backup = prefs + L".dnspilot.bak";
    bool created = false;
    if (!ensureBackup(prefs, backup, created, err)) {
        msg = fileErr("备份 Preferences 失败", err);
        return false;
    }
    if (!atomicWrite(prefs, updated, err)) {
        msg = fileErr("写入 Preferences 失败", err);
        return false;
    }
    if (created) backupPath = toUtf8(backup);
    msg = off ? ("已关闭 " + displayName + " 的安全 DNS")
              : ("已为 " + displayName + " 设置安全 DNS（DoH）");
    if (created) msg += "，原配置已备份";
    return true;
}

bool applyFirefox(const std::string &folder, const std::string &dohUrl, bool off,
                  std::string &msg, std::string &backupPath) {
    if (!safeFolderName(folder)) {
        msg = "配置文件目录名无效或包含路径穿越字符";
        return false;
    }
    std::wstring root = firefoxRoot();
    if (root.empty()) {
        msg = "无法确定 Firefox 配置文件目录";
        return false;
    }
    if (processRunning(L"firefox.exe")) {
        msg = "浏览器正在运行，请完全退出后重试";
        return false;
    }
    std::wstring dir = joinPath(root, toWide(folder));
    if (!withinRoot(root, dir)) {
        msg = "配置文件目录不在 Firefox 配置目录内";
        return false;
    }
    if (!isDir(dir)) {
        msg = "未找到 Firefox 配置文件目录：" + toUtf8(dir);
        return false;
    }
    if (firefoxLocked(dir)) {
        msg = "浏览器正在运行或配置文件已锁定，请完全退出后重试";
        return false;
    }
    std::wstring prefs = joinPath(dir, L"prefs.js");
    if (!isFile(prefs)) {
        msg = "未找到 prefs.js：" + toUtf8(prefs);
        return false;
    }
    if (!off) {
        if (dohUrl.compare(0, 8, "https://") != 0) {
            msg = "DoH 地址必须以 https:// 开头";
            return false;
        }
        if (dohUrl.size() >= 2048) {
            msg = "DoH 地址过长（上限 2047 字节）";
            return false;
        }
    }

    std::string text;
    DWORD err = 0;
    if (!readFileBytes(prefs, text, err)) {
        msg = fileErr("无法读取 prefs.js", err);
        return false;
    }

    std::string updated = rewriteFirefoxPrefs(text, off, dohUrl);

    std::wstring backup = prefs + L".dnspilot.bak";
    bool created = false;
    if (!ensureBackup(prefs, backup, created, err)) {
        msg = fileErr("备份 prefs.js 失败", err);
        return false;
    }
    if (!atomicWrite(prefs, updated, err)) {
        msg = fileErr("写入 prefs.js 失败", err);
        return false;
    }
    if (created) backupPath = toUtf8(backup);
    msg = off ? "已关闭 Firefox 的安全 DNS（DoH）" : "已为 Firefox 设置安全 DNS（DoH）";
    if (created) msg += "，原配置已备份";
    return true;
}

}  // namespace

std::vector<BrowserProfileInfo> browsers::scan() {
    std::vector<BrowserProfileInfo> out;
    scanChromium("chrome", out);
    scanChromium("edge", out);
    scanFirefox(out);
    return out;
}

bool browsers::apply(const std::string &browser, const std::string &folder,
                     const std::string &dohUrl, bool off, std::string &msg,
                     std::string &backupPath) {
    msg.clear();
    backupPath.clear();
    std::string b = lowerAscii(browser);
    if (b == "chrome" || b == "edge") return applyChromium(b, folder, dohUrl, off, msg, backupPath);
    if (b == "firefox") return applyFirefox(folder, dohUrl, off, msg, backupPath);
    msg = "不支持的浏览器：" + browser;
    return false;
}
