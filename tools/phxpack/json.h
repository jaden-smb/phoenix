// tools/phxpack/json.h — a small, dependency-free JSON parser for the asset pipeline (host-only,
// STL allowed). Recursive-descent over a std::string into a simple DOM; enough to read Tiled
// `.tmj` maps (objects, arrays, strings, numbers, bools, null). Returns false on malformed input
// rather than throwing. Not a general-purpose library — just what the converters need.
#ifndef PHX_TOOLS_JSON_H
#define PHX_TOOLS_JSON_H

#include <cstdint>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace phxtool {

struct JsonValue {
    enum Type { Null, Bool, Num, Str, Arr, Obj } type = Null;
    bool        boolean = false;
    double      number  = 0;
    std::string str;
    std::vector<JsonValue> arr;
    std::vector<std::pair<std::string, JsonValue>> members;

    bool is_obj() const { return type == Obj; }
    bool is_arr() const { return type == Arr; }

    const JsonValue* find(const char* key) const {
        if (type != Obj) return nullptr;
        for (const auto& kv : members) if (kv.first == key) return &kv.second;
        return nullptr;
    }
    int          as_int(int d = 0)    const { return type == Num ? int(number) : d; }
    double       as_num(double d = 0) const { return type == Num ? number : d; }
    const std::string& as_str() const { static const std::string empty; return type == Str ? str : empty; }
    // Convenience: child value by key with a typed default.
    int          int_at(const char* k, int d = 0) const { const JsonValue* v = find(k); return v ? v->as_int(d) : d; }
    const std::string& str_at(const char* k) const { const JsonValue* v = find(k); static const std::string e; return v ? v->as_str() : e; }
};

class JsonParser {
public:
    static bool parse(const std::string& text, JsonValue& out) {
        JsonParser p(text);
        p.ws();
        if (!p.value(out)) return false;
        p.ws();
        return p.i_ == p.s_.size();          // trailing garbage = malformed
    }

private:
    explicit JsonParser(const std::string& s) : s_(s) {}
    const std::string& s_;
    size_t i_ = 0;

    void ws() { while (i_ < s_.size() && (s_[i_]==' '||s_[i_]=='\t'||s_[i_]=='\n'||s_[i_]=='\r')) ++i_; }
    bool eof() const { return i_ >= s_.size(); }

    bool value(JsonValue& v) {
        if (eof()) return false;
        const char c = s_[i_];
        if (c == '{') return object(v);
        if (c == '[') return array(v);
        if (c == '"') { v.type = JsonValue::Str; return string(v.str); }
        if (c == 't' || c == 'f') return boolean(v);
        if (c == 'n') return null(v);
        if (c == '-' || (c >= '0' && c <= '9')) return number(v);
        return false;
    }

    bool object(JsonValue& v) {
        v.type = JsonValue::Obj; ++i_; ws();
        if (!eof() && s_[i_] == '}') { ++i_; return true; }
        for (;;) {
            ws();
            if (eof() || s_[i_] != '"') return false;
            std::string key; if (!string(key)) return false;
            ws(); if (eof() || s_[i_] != ':') return false; ++i_; ws();
            JsonValue child; if (!value(child)) return false;
            v.members.emplace_back(std::move(key), std::move(child));
            ws(); if (eof()) return false;
            if (s_[i_] == ',') { ++i_; continue; }
            if (s_[i_] == '}') { ++i_; return true; }
            return false;
        }
    }

    bool array(JsonValue& v) {
        v.type = JsonValue::Arr; ++i_; ws();
        if (!eof() && s_[i_] == ']') { ++i_; return true; }
        for (;;) {
            ws();
            JsonValue child; if (!value(child)) return false;
            v.arr.push_back(std::move(child));
            ws(); if (eof()) return false;
            if (s_[i_] == ',') { ++i_; continue; }
            if (s_[i_] == ']') { ++i_; return true; }
            return false;
        }
    }

    bool string(std::string& out) {
        if (eof() || s_[i_] != '"') return false;
        ++i_;
        while (!eof()) {
            char c = s_[i_++];
            if (c == '"') return true;
            if (c == '\\') {
                if (eof()) return false;
                char e = s_[i_++];
                switch (e) {
                    case '"': out += '"'; break;   case '\\': out += '\\'; break;
                    case '/': out += '/'; break;   case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;  case 'n': out += '\n'; break;
                    case 'r': out += '\r'; break;  case 't': out += '\t'; break;
                    case 'u': {
                        if (i_ + 4 > s_.size()) return false;
                        unsigned cp = 0;
                        for (int k = 0; k < 4; ++k) {
                            char h = s_[i_++]; cp <<= 4;
                            if (h >= '0' && h <= '9') cp |= unsigned(h - '0');
                            else if (h >= 'a' && h <= 'f') cp |= unsigned(h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') cp |= unsigned(h - 'A' + 10);
                            else return false;
                        }
                        if (cp < 0x80) out += char(cp);                         // UTF-8 encode
                        else if (cp < 0x800) { out += char(0xC0|(cp>>6)); out += char(0x80|(cp&0x3F)); }
                        else { out += char(0xE0|(cp>>12)); out += char(0x80|((cp>>6)&0x3F)); out += char(0x80|(cp&0x3F)); }
                        break;
                    }
                    default: return false;
                }
            } else {
                out += c;
            }
        }
        return false;                          // unterminated string
    }

    bool number(JsonValue& v) {
        size_t start = i_;
        if (!eof() && s_[i_] == '-') ++i_;
        while (!eof() && ((s_[i_] >= '0' && s_[i_] <= '9') || s_[i_]=='.' || s_[i_]=='e' || s_[i_]=='E' || s_[i_]=='+' || s_[i_]=='-')) ++i_;
        if (i_ == start) return false;
        v.type = JsonValue::Num;
        v.number = std::strtod(s_.c_str() + start, nullptr);
        return true;
    }

    bool literal(const char* lit) {
        size_t n = 0; while (lit[n]) ++n;
        if (i_ + n > s_.size()) return false;
        for (size_t k = 0; k < n; ++k) if (s_[i_ + k] != lit[k]) return false;
        i_ += n; return true;
    }
    bool boolean(JsonValue& v) {
        if (s_[i_] == 't') {
            if (!literal("true")) return false;
            v.type = JsonValue::Bool; v.boolean = true; return true;
        }
        if (!literal("false")) return false;
        v.type = JsonValue::Bool; v.boolean = false; return true;
    }
    bool null(JsonValue& v) { if (!literal("null")) return false; v.type = JsonValue::Null; return true; }
};

} // namespace phxtool
#endif // PHX_TOOLS_JSON_H
