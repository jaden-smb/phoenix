// tools/phxpack/json.h — a small, dependency-free JSON parser for the asset pipeline (host-only,
// STL allowed). Recursive-descent over a std::string into a simple DOM; enough to read Tiled
// `.tmj` maps (objects, arrays, strings, numbers, bools, null). Returns false on malformed input
// rather than throwing; the optional `err` out-param receives a "line L, col C: reason" message
// pointing at the deepest failure, so authors can fix their file instead of guessing. Not a
// general-purpose library — just what the converters need.
#ifndef PHX_TOOLS_JSON_H
#define PHX_TOOLS_JSON_H

#include <cstdint>
#include <cstdio>
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
    bool         as_bool(bool d = false) const { return type == Bool ? boolean : d; }
    const std::string& as_str() const { static const std::string empty; return type == Str ? str : empty; }
    // Convenience: child value by key with a typed default.
    int          int_at(const char* k, int d = 0) const { const JsonValue* v = find(k); return v ? v->as_int(d) : d; }
    const std::string& str_at(const char* k) const { const JsonValue* v = find(k); static const std::string e; return v ? v->as_str() : e; }
};

class JsonParser {
public:
    static bool parse(const std::string& text, JsonValue& out, std::string* err = nullptr) {
        JsonParser p(text);
        p.ws();
        bool ok = p.value(out);
        if (ok) {
            p.ws();
            if (p.i_ != p.s_.size()) ok = p.fail("trailing characters after the JSON value");
        }
        if (!ok && err) *err = p.describe();
        return ok;
    }

private:
    explicit JsonParser(const std::string& s) : s_(s) {}
    const std::string& s_;
    size_t i_ = 0;
    size_t err_pos_ = 0;            // position of the DEEPEST failure (set once, kept on unwind)
    const char* err_msg_ = nullptr;

    // Record the failure where it was first detected (the deepest parse point); the message is
    // not overwritten as the recursion unwinds. Always returns false so call sites can
    // `return fail("...")`.
    bool fail(const char* msg) {
        if (!err_msg_) { err_msg_ = msg; err_pos_ = i_; }
        return false;
    }
    std::string describe() const {
        int line = 1, col = 1;
        for (size_t k = 0; k < err_pos_ && k < s_.size(); ++k) {
            if (s_[k] == '\n') { ++line; col = 1; } else ++col;
        }
        char head[48];
        std::snprintf(head, sizeof(head), "line %d, col %d: ", line, col);
        return std::string(head) + (err_msg_ ? err_msg_ : "malformed JSON");
    }

    void ws() { while (i_ < s_.size() && (s_[i_]==' '||s_[i_]=='\t'||s_[i_]=='\n'||s_[i_]=='\r')) ++i_; }
    bool eof() const { return i_ >= s_.size(); }

    bool value(JsonValue& v) {
        if (eof()) return fail("unexpected end of input (expected a value)");
        const char c = s_[i_];
        if (c == '{') return object(v);
        if (c == '[') return array(v);
        if (c == '"') { v.type = JsonValue::Str; return string(v.str); }
        if (c == 't' || c == 'f') return boolean(v);
        if (c == 'n') return null(v);
        if (c == '-' || (c >= '0' && c <= '9')) return number(v);
        return fail("unexpected character (expected a value)");
    }

    bool object(JsonValue& v) {
        v.type = JsonValue::Obj; ++i_; ws();
        if (!eof() && s_[i_] == '}') { ++i_; return true; }
        for (;;) {
            ws();
            if (eof()) return fail("unterminated object (expected '\"' or '}')");
            if (s_[i_] != '"') return fail("expected '\"' to start an object key");
            std::string key; if (!string(key)) return false;
            ws();
            if (eof() || s_[i_] != ':') return fail("expected ':' after object key");
            ++i_; ws();
            JsonValue child; if (!value(child)) return false;
            v.members.emplace_back(std::move(key), std::move(child));
            ws();
            if (eof()) return fail("unterminated object (expected ',' or '}')");
            if (s_[i_] == ',') { ++i_; continue; }
            if (s_[i_] == '}') { ++i_; return true; }
            return fail("expected ',' or '}' in object");
        }
    }

    bool array(JsonValue& v) {
        v.type = JsonValue::Arr; ++i_; ws();
        if (!eof() && s_[i_] == ']') { ++i_; return true; }
        for (;;) {
            ws();
            JsonValue child; if (!value(child)) return false;
            v.arr.push_back(std::move(child));
            ws();
            if (eof()) return fail("unterminated array (expected ',' or ']')");
            if (s_[i_] == ',') { ++i_; continue; }
            if (s_[i_] == ']') { ++i_; return true; }
            return fail("expected ',' or ']' in array");
        }
    }

    bool string(std::string& out) {
        if (eof() || s_[i_] != '"') return fail("expected '\"' to start a string");
        ++i_;
        while (!eof()) {
            char c = s_[i_++];
            if (c == '"') return true;
            if (c == '\\') {
                if (eof()) return fail("unterminated string escape");
                char e = s_[i_++];
                switch (e) {
                    case '"': out += '"'; break;   case '\\': out += '\\'; break;
                    case '/': out += '/'; break;   case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;  case 'n': out += '\n'; break;
                    case 'r': out += '\r'; break;  case 't': out += '\t'; break;
                    case 'u': {
                        if (i_ + 4 > s_.size()) return fail("bad \\uXXXX escape (needs 4 hex digits)");
                        unsigned cp = 0;
                        for (int k = 0; k < 4; ++k) {
                            char h = s_[i_++]; cp <<= 4;
                            if (h >= '0' && h <= '9') cp |= unsigned(h - '0');
                            else if (h >= 'a' && h <= 'f') cp |= unsigned(h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') cp |= unsigned(h - 'A' + 10);
                            else return fail("bad \\uXXXX escape (needs 4 hex digits)");
                        }
                        if (cp < 0x80) out += char(cp);                         // UTF-8 encode
                        else if (cp < 0x800) { out += char(0xC0|(cp>>6)); out += char(0x80|(cp&0x3F)); }
                        else { out += char(0xE0|(cp>>12)); out += char(0x80|((cp>>6)&0x3F)); out += char(0x80|(cp&0x3F)); }
                        break;
                    }
                    default: return fail("unknown string escape");
                }
            } else {
                out += c;
            }
        }
        return fail("unterminated string");
    }

    bool number(JsonValue& v) {
        size_t start = i_;
        if (!eof() && s_[i_] == '-') ++i_;
        while (!eof() && ((s_[i_] >= '0' && s_[i_] <= '9') || s_[i_]=='.' || s_[i_]=='e' || s_[i_]=='E' || s_[i_]=='+' || s_[i_]=='-')) ++i_;
        if (i_ == start) return fail("expected a number");
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
            if (!literal("true")) return fail("expected 'true'");
            v.type = JsonValue::Bool; v.boolean = true; return true;
        }
        if (!literal("false")) return fail("expected 'false'");
        v.type = JsonValue::Bool; v.boolean = false; return true;
    }
    bool null(JsonValue& v) {
        if (!literal("null")) return fail("expected 'null'");
        v.type = JsonValue::Null; return true;
    }
};

} // namespace phxtool
#endif // PHX_TOOLS_JSON_H
