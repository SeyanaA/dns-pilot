// 轻量 JSON：解析 + 序列化（无第三方依赖）
#pragma once
#include <string>
#include <vector>
#include <utility>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>

namespace js {

struct Value {
    enum Type { NUL, BOOL, NUM, STR, ARR, OBJ } type = NUL;
    bool b = false;
    double num = 0;
    std::string str;
    std::vector<Value> arr;
    std::vector<std::pair<std::string, Value>> obj;

    static Value objV() { Value v; v.type = OBJ; return v; }
    static Value arrV() { Value v; v.type = ARR; return v; }
    static Value strV(std::string s) { Value v; v.type = STR; v.str = std::move(s); return v; }
    static Value numV(double n) { Value v; v.type = NUM; v.num = n; return v; }
    static Value boolV(bool x) { Value v; v.type = BOOL; v.b = x; return v; }

    bool isObj() const { return type == OBJ; }
    bool isArr() const { return type == ARR; }
    bool isStr() const { return type == STR; }
    bool isNum() const { return type == NUM; }

    const Value *get(const std::string &k) const {
        if (type != OBJ) return nullptr;
        for (auto &kv : obj) if (kv.first == k) return &kv.second;
        return nullptr;
    }
    Value *get(const std::string &k) {
        if (type != OBJ) return nullptr;
        for (auto &kv : obj) if (kv.first == k) return &kv.second;
        return nullptr;
    }
    void set(const std::string &k, Value v) {
        for (auto &kv : obj) if (kv.first == k) { kv.second = std::move(v); return; }
        obj.emplace_back(k, std::move(v));
    }
    void erase(const std::string &k) {
        for (size_t i = 0; i < obj.size(); i++) if (obj[i].first == k) { obj.erase(obj.begin() + i); return; }
    }
    Value &operator[](const std::string &k) {
        if (type != OBJ) { type = OBJ; }
        for (auto &kv : obj) if (kv.first == k) return kv.second;
        obj.emplace_back(k, Value());
        return obj.back().second;
    }
    std::string s(const std::string &k, const std::string &def = "") const {
        const Value *v = get(k);
        return (v && v->type == STR) ? v->str : def;
    }
    double n(const std::string &k, double def = 0) const {
        const Value *v = get(k);
        if (!v) return def;
        if (v->type == NUM) return v->num;
        if (v->type == BOOL) return v->b ? 1 : 0;
        return def;
    }
    bool bv(const std::string &k, bool def = false) const {
        const Value *v = get(k);
        if (!v) return def;
        if (v->type == BOOL) return v->b;
        if (v->type == NUM) return v->num != 0;
        return def;
    }

    static void encodeStr(const std::string &in, std::string &out) {
        out += '"';
        for (unsigned char c : in) {
            switch (c) {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                case '\b': out += "\\b"; break;
                case '\f': out += "\\f"; break;
                default:
                    if (c < 0x20) {
                        char buf[8];
                        snprintf(buf, sizeof(buf), "\\u%04x", (unsigned)c);
                        out += buf;
                    } else out += (char)c;
            }
        }
        out += '"';
    }

    std::string dump(int indent = 0) const { std::string o; dumpTo(o, indent, 0); return o; }

    void dumpTo(std::string &o, int indent, int depth) const {
        auto nl = [&](int d) {
            if (indent > 0) { o += '\n'; o.append((size_t)(indent * d), ' '); }
        };
        switch (type) {
            case NUL: o += "null"; break;
            case BOOL: o += b ? "true" : "false"; break;
            case NUM: {
                double v = num;
                if (!std::isfinite(v)) { o += "null"; break; }
                if (v == (double)(long long)v && std::fabs(v) < 1e15) {
                    char buf[32]; snprintf(buf, sizeof(buf), "%lld", (long long)v); o += buf;
                } else {
                    char buf[40]; snprintf(buf, sizeof(buf), "%.10g", v); o += buf;
                }
                break;
            }
            case STR: encodeStr(str, o); break;
            case ARR: {
                o += '[';
                for (size_t i = 0; i < arr.size(); i++) {
                    if (i) o += ',';
                    nl(depth + 1);
                    arr[i].dumpTo(o, indent, depth + 1);
                }
                if (!arr.empty()) nl(depth);
                o += ']';
                break;
            }
            case OBJ: {
                o += '{';
                for (size_t i = 0; i < obj.size(); i++) {
                    if (i) o += ',';
                    nl(depth + 1);
                    encodeStr(obj[i].first, o);
                    o += ':';
                    if (indent > 0) o += ' ';
                    obj[i].second.dumpTo(o, indent, depth + 1);
                }
                if (!obj.empty()) nl(depth);
                o += '}';
                break;
            }
        }
    }

    static bool parse(const std::string &text, Value &out, std::string &err) {
        Parser p{text.c_str(), text.c_str() + text.size(), std::string()};
        if (!p.value(out, 0)) { err = p.err.empty() ? "JSON 解析失败" : p.err; return false; }
        p.ws();
        if (p.cur != p.end) { err = "JSON 尾部存在多余字符"; return false; }
        return true;
    }

  private:
    struct Parser {
        const char *cur, *end;
        std::string err;

        void ws() { while (cur < end && (*cur == ' ' || *cur == '\t' || *cur == '\n' || *cur == '\r')) cur++; }
        bool fail(const char *m) { if (err.empty()) err = m; return false; }

        bool value(Value &v, int depth) {
            if (depth > 64) return fail("JSON 嵌套过深");
            ws();
            if (cur >= end) return fail("JSON 意外结束");
            switch (*cur) {
                case '{': return object(v, depth);
                case '[': return array(v, depth);
                case '"': v.type = STR; return string(v.str);
                case 't': return lit("true", v, true);
                case 'f': return lit("false", v, false);
                case 'n': return null();
                default: return number(v);
            }
        }
        bool lit(const char *t, Value &v, bool b) {
            size_t n = strlen(t);
            if ((size_t)(end - cur) < n || memcmp(cur, t, n) != 0) return fail("JSON 字面量无效");
            cur += n; v.type = BOOL; v.b = b; return true;
        }
        bool null() {
            if ((size_t)(end - cur) < 4 || memcmp(cur, "null", 4) != 0) return fail("JSON 字面量无效");
            cur += 4; return true;
        }
        bool number(Value &v) {
            const char *s = cur;
            if (cur < end && *cur == '-') cur++;
            while (cur < end && ((*cur >= '0' && *cur <= '9') || *cur == '.' || *cur == 'e' || *cur == 'E' ||
                                 *cur == '+' || *cur == '-')) cur++;
            if (s == cur) return fail("JSON 数字无效");
            std::string tmp(s, (size_t)(cur - s));
            char *e = nullptr;
            double d = strtod(tmp.c_str(), &e);
            if (!e || *e) return fail("JSON 数字无效");
            v.type = NUM; v.num = d; return true;
        }
        bool string(std::string &out) {
            if (cur >= end || *cur != '"') return fail("JSON 字符串无效");
            cur++;
            out.clear();
            while (cur < end) {
                unsigned char c = (unsigned char)*cur++;
                if (c == '"') return true;
                if (c == '\\') {
                    if (cur >= end) return fail("JSON 转义不完整");
                    char e = *cur++;
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
                            unsigned cp = hex4();
                            if (cp == 0xFFFFFFFF) return fail("JSON \\u 转义无效");
                            if (cp >= 0xD800 && cp <= 0xDBFF) {
                                if (cur + 1 < end && cur[0] == '\\' && cur[1] == 'u') {
                                    cur += 2;
                                    unsigned lo = hex4();
                                    if (lo == 0xFFFFFFFF) return fail("JSON \\u 转义无效");
                                    if (lo >= 0xDC00 && lo <= 0xDFFF)
                                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                                    else cp = 0xFFFD;
                                } else cp = 0xFFFD;
                            }
                            appendUtf8(out, cp);
                            break;
                        }
                        default: return fail("JSON 转义字符无效");
                    }
                } else if (c < 0x20) {
                    return fail("JSON 字符串包含控制字符");
                } else out += (char)c;
            }
            return fail("JSON 字符串未闭合");
        }
        unsigned hex4() {
            if (end - cur < 4) return 0xFFFFFFFF;
            unsigned v = 0;
            for (int i = 0; i < 4; i++) {
                char c = *cur++;
                v <<= 4;
                if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
                else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
                else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
                else return 0xFFFFFFFF;
            }
            return v;
        }
        static void appendUtf8(std::string &o, unsigned cp) {
            if (cp <= 0x7F) o += (char)cp;
            else if (cp <= 0x7FF) { o += (char)(0xC0 | (cp >> 6)); o += (char)(0x80 | (cp & 0x3F)); }
            else if (cp <= 0xFFFF) {
                o += (char)(0xE0 | (cp >> 12)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F));
            } else {
                o += (char)(0xF0 | (cp >> 18)); o += (char)(0x80 | ((cp >> 12) & 0x3F));
                o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F));
            }
        }
        bool array(Value &v, int depth) {
            cur++; v.type = ARR;
            ws();
            if (cur < end && *cur == ']') { cur++; return true; }
            for (;;) {
                Value item;
                if (!value(item, depth + 1)) return false;
                v.arr.push_back(std::move(item));
                ws();
                if (cur >= end) return fail("JSON 数组未闭合");
                if (*cur == ',') { cur++; continue; }
                if (*cur == ']') { cur++; return true; }
                return fail("JSON 数组分隔符无效");
            }
        }
        bool object(Value &v, int depth) {
            cur++; v.type = OBJ;
            ws();
            if (cur < end && *cur == '}') { cur++; return true; }
            for (;;) {
                ws();
                std::string key;
                if (!string(key)) return false;
                ws();
                if (cur >= end || *cur != ':') return fail("JSON 缺少冒号");
                cur++;
                Value item;
                if (!value(item, depth + 1)) return false;
                v.obj.emplace_back(std::move(key), std::move(item));
                ws();
                if (cur >= end) return fail("JSON 对象未闭合");
                if (*cur == ',') { cur++; continue; }
                if (*cur == '}') { cur++; return true; }
                return fail("JSON 对象分隔符无效");
            }
        }
    };
};

}  // namespace js
