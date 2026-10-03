// tools/phxstudio/syntax.h — line-at-a-time syntax highlighting for the Studio's code editor:
// C/C++ (the engine and games), JSON (phxbin tables, sprite sidecars, .tmj), Makefile / shell,
// Python (depcheck & friends), CMake, Markdown (docs + instructions.md) and the .sprdef format.
//
// highlight_line() takes the lexer state at the start of a line (0 = normal; non-zero = inside a
// multi-line construct such as a C block comment or a Python docstring) and returns the state at
// its end, so the view caches one int per line and re-lexes only from the first edited line.
// Pure and headless (unit-tested in the editors suite). Host-only.
#ifndef PHX_TOOLS_PHXSTUDIO_SYNTAX_H
#define PHX_TOOLS_PHXSTUDIO_SYNTAX_H

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace phxstudio {

enum class Lang : uint8_t { Plain, Cpp, Json, Make, Shell, Python, CMake, Markdown, Sprdef };

enum class Tok : uint8_t {
    Text, Keyword, Type, Number, String, Char, Comment, Preproc, Punct, Func, Constant, Key,
    Heading, Emphasis, Variable, Doc, Error,
};

struct Span { int start = 0, len = 0; Tok tok = Tok::Text; };

inline const char* lang_name(Lang l) {
    switch (l) {
    case Lang::Cpp: return "C++";      case Lang::Json: return "JSON";   case Lang::Make: return "Makefile";
    case Lang::Shell: return "Shell";  case Lang::Python: return "Python"; case Lang::CMake: return "CMake";
    case Lang::Markdown: return "Markdown"; case Lang::Sprdef: return "sprdef"; default: return "Text";
    }
}

inline bool ends_with_ci(const std::string& s, const char* suf) {
    const size_t n = std::strlen(suf);
    if (s.size() < n) return false;
    for (size_t i = 0; i < n; ++i) {
        char a = s[s.size() - n + i], b = suf[i];
        if (a >= 'A' && a <= 'Z') a = char(a - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}

inline Lang lang_for_path(const std::string& path) {
    const size_t sl = path.find_last_of("/\\");
    const std::string base = sl == std::string::npos ? path : path.substr(sl + 1);
    if (base == "Makefile" || base == "makefile" || base == "GNUmakefile" || ends_with_ci(base, ".mk")) return Lang::Make;
    if (base == "CMakeLists.txt" || ends_with_ci(base, ".cmake")) return Lang::CMake;
    for (const char* e : { ".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inl", ".glsl" })
        if (ends_with_ci(base, e)) return Lang::Cpp;
    for (const char* e : { ".json", ".tmj", ".phxproj" }) if (ends_with_ci(base, e)) return Lang::Json;
    for (const char* e : { ".sh", ".bash", ".yml", ".yaml", ".toml", ".ini", ".cfg" }) if (ends_with_ci(base, e)) return Lang::Shell;
    if (ends_with_ci(base, ".py")) return Lang::Python;
    if (ends_with_ci(base, ".md")) return Lang::Markdown;
    if (ends_with_ci(base, ".sprdef")) return Lang::Sprdef;
    return Lang::Plain;
}

// The line-comment prefix Ctrl+/ toggles ("" = none).
inline const char* comment_prefix(Lang l) {
    switch (l) {
    case Lang::Cpp: return "//";
    case Lang::Make: case Lang::Shell: case Lang::Python: case Lang::CMake: case Lang::Sprdef: return "#";
    default: return "";
    }
}

namespace syn {

inline bool is_ident0(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
inline bool is_ident(char c) { return is_ident0(c) || (c >= '0' && c <= '9'); }
inline bool is_digit(char c) { return c >= '0' && c <= '9'; }

inline bool in_list(const char* const* list, const std::string& w) {
    for (const char* const* p = list; *p; ++p) if (w == *p) return true;
    return false;
}

inline const char* const* cpp_keywords() {
    static const char* const k[] = {
        "alignas", "alignof", "and", "asm", "auto", "break", "case", "catch", "class", "const",
        "constexpr", "consteval", "constinit", "const_cast", "continue", "co_await", "co_return",
        "decltype", "default", "delete", "do", "dynamic_cast", "else", "enum", "explicit", "export",
        "extern", "false", "final", "for", "friend", "goto", "if", "inline", "mutable", "namespace",
        "new", "noexcept", "not", "nullptr", "operator", "or", "override", "private", "protected",
        "public", "register", "reinterpret_cast", "return", "sizeof", "static", "static_assert",
        "static_cast", "struct", "switch", "template", "this", "thread_local", "throw", "true",
        "try", "typedef", "typeid", "typename", "union", "using", "virtual", "volatile", "while",
        "NULL", nullptr };
    return k;
}
inline const char* const* cpp_types() {
    static const char* const k[] = {
        "void", "bool", "char", "short", "int", "long", "float", "double", "signed", "unsigned",
        "wchar_t", "char16_t", "char32_t", "size_t", "ssize_t", "ptrdiff_t", "intptr_t", "uintptr_t",
        "int8_t", "int16_t", "int32_t", "int64_t", "uint8_t", "uint16_t", "uint32_t", "uint64_t",
        "std", "string", "vector", "map", "unique_ptr", "shared_ptr", "array", "pair",
        "scalar", "fixed16", "vec2", "Rgba", "Entity", "World", "App", "Game", "Renderer", "Status",
        "TextureId", "TilemapId", "NameHash", nullptr };
    return k;
}
inline const char* const* py_keywords() {
    static const char* const k[] = {
        "and", "as", "assert", "async", "await", "break", "class", "continue", "def", "del", "elif",
        "else", "except", "False", "finally", "for", "from", "global", "if", "import", "in", "is",
        "lambda", "None", "nonlocal", "not", "or", "pass", "raise", "return", "True", "try", "while",
        "with", "yield", "self", nullptr };
    return k;
}
inline const char* const* sh_keywords() {
    static const char* const k[] = {
        "if", "then", "else", "elif", "fi", "for", "in", "do", "done", "while", "until", "case",
        "esac", "function", "return", "exit", "export", "local", "set", "echo", "test", "true",
        "false", "ifeq", "ifneq", "ifdef", "ifndef", "endif", "include", "define", "endef",
        "override", "command", "mkdir", "cd", "rm", "cp", nullptr };
    return k;
}
inline const char* const* cmake_keywords() {
    static const char* const k[] = {
        "if", "elseif", "else", "endif", "foreach", "endforeach", "while", "endwhile", "function",
        "endfunction", "macro", "endmacro", "set", "unset", "option", "project", "add_executable",
        "add_library", "add_subdirectory", "target_link_libraries", "target_include_directories",
        "target_compile_definitions", "target_sources", "include", "find_package", "message",
        "list", "string", "file", "add_test", "install", "return", nullptr };
    return k;
}

// Upper-case-with-underscores identifiers (PHX_ASSERT, kMaxUndo is not) read as constants/macros.
inline bool looks_constant(const std::string& w) {
    if (w.size() < 2) return false;
    bool upper = false;
    for (char c : w) { if (c >= 'a' && c <= 'z') return false; if (c >= 'A' && c <= 'Z') upper = true; }
    return upper;
}

// Scan a number literal starting at i (C-ish: 0x.., 1.5f, 1e-3, 1'000, suffixes). Returns end.
inline size_t scan_number(const std::string& s, size_t i) {
    const size_t n = s.size();
    if (i + 1 < n && s[i] == '0' && (s[i + 1] == 'x' || s[i + 1] == 'X' || s[i + 1] == 'b' || s[i + 1] == 'B')) {
        i += 2;
        while (i < n && (is_ident(s[i]) || s[i] == '\'')) ++i;
        return i;
    }
    while (i < n && (is_digit(s[i]) || s[i] == '.' || s[i] == '\'')) ++i;
    if (i < n && (s[i] == 'e' || s[i] == 'E')) {
        ++i;
        if (i < n && (s[i] == '+' || s[i] == '-')) ++i;
        while (i < n && is_digit(s[i])) ++i;
    }
    while (i < n && is_ident(s[i])) ++i;   // suffixes: u, ll, f, ...
    return i;
}

// Scan a quoted string/char literal starting at the quote; stops at the matching quote or EOL.
inline size_t scan_quoted(const std::string& s, size_t i) {
    const char q = s[i++];
    while (i < s.size()) {
        if (s[i] == '\\') { i += 2; continue; }
        if (s[i] == q) return i + 1;
        ++i;
    }
    return s.size();
}

struct Out {
    std::vector<Span>& v;
    void add(size_t a, size_t b, Tok t) {
        if (b <= a) return;
        if (!v.empty() && v.back().tok == t && v.back().start + v.back().len == int(a)) { v.back().len += int(b - a); return; }
        v.push_back(Span{ int(a), int(b - a), t });
    }
};

// ---- C / C++ ----
inline int lex_cpp(const std::string& s, int state, Out& o) {
    const size_t n = s.size();
    size_t i = 0;
    if (state == 1) {                                   // inside /* ... */
        const size_t e = s.find("*/");
        if (e == std::string::npos) { o.add(0, n, Tok::Comment); return 1; }
        o.add(0, e + 2, Tok::Comment);
        i = e + 2;
    }
    // Preprocessor lines: '#' first non-blank. The directive is Preproc; <header> / "header" is a String.
    size_t fb = s.find_first_not_of(" \t");
    const bool pp = fb != std::string::npos && s[fb] == '#' && i <= fb;
    while (i < n) {
        const char c = s[i];
        if (c == '/' && i + 1 < n && s[i + 1] == '/') {
            const bool doc = i + 2 < n && (s[i + 2] == '/' || s[i + 2] == '!');
            o.add(i, n, doc ? Tok::Doc : Tok::Comment); i = n; break;
        }
        if (c == '/' && i + 1 < n && s[i + 1] == '*') {
            const size_t e = s.find("*/", i + 2);
            if (e == std::string::npos) { o.add(i, n, Tok::Comment); return 1; }
            o.add(i, e + 2, Tok::Comment); i = e + 2; continue;
        }
        if (pp && i == fb) {
            size_t e = i + 1;
            while (e < n && (s[e] == ' ' || s[e] == '\t')) ++e;
            while (e < n && is_ident(s[e])) ++e;
            o.add(i, e, Tok::Preproc);
            i = e;
            // #include <...>
            size_t k = i;
            while (k < n && (s[k] == ' ' || s[k] == '\t')) ++k;
            if (k < n && s[k] == '<') {
                const size_t e2 = s.find('>', k);
                const size_t end = e2 == std::string::npos ? n : e2 + 1;
                o.add(i, k, Tok::Text);
                o.add(k, end, Tok::String);
                i = end;
            }
            continue;
        }
        if (c == '"' || c == '\'') {
            // raw string R"( ... )" on one line
            const size_t e = scan_quoted(s, i);
            o.add(i, e, c == '"' ? Tok::String : Tok::Char);
            i = e; continue;
        }
        if (is_digit(c) || (c == '.' && i + 1 < n && is_digit(s[i + 1]))) {
            const size_t e = scan_number(s, i);
            o.add(i, e, Tok::Number); i = e; continue;
        }
        if (is_ident0(c)) {
            size_t e = i;
            while (e < n && is_ident(s[e])) ++e;
            const std::string w = s.substr(i, e - i);
            Tok t = Tok::Text;
            if (in_list(cpp_keywords(), w)) t = Tok::Keyword;
            else if (in_list(cpp_types(), w)) t = Tok::Type;
            else if (looks_constant(w)) t = Tok::Constant;
            else {
                size_t k = e;
                while (k < n && s[k] == ' ') ++k;
                if (k < n && s[k] == '(') t = Tok::Func;
                else if (w.size() > 2 && w[0] == 'k' && w[1] >= 'A' && w[1] <= 'Z') t = Tok::Constant;   // kFoo
            }
            if (pp && t == Tok::Text) t = Tok::Preproc;
            o.add(i, e, t); i = e; continue;
        }
        if (c == '\\' && i + 1 == n) { o.add(i, n, Tok::Punct); ++i; continue; }
        if (std::strchr("{}()[];,.<>=+-*/%&|^!~?:", c)) { o.add(i, i + 1, Tok::Punct); ++i; continue; }
        o.add(i, i + 1, Tok::Text); ++i;
    }
    return 0;
}

// ---- JSON ----
inline int lex_json(const std::string& s, Out& o) {
    const size_t n = s.size();
    size_t i = 0;
    while (i < n) {
        const char c = s[i];
        if (c == '"') {
            const size_t e = scan_quoted(s, i);
            size_t k = e;
            while (k < n && (s[k] == ' ' || s[k] == '\t')) ++k;
            o.add(i, e, k < n && s[k] == ':' ? Tok::Key : Tok::String);
            i = e; continue;
        }
        if (is_digit(c) || c == '-') {
            size_t e = i + 1;
            while (e < n && (is_digit(s[e]) || s[e] == '.' || s[e] == 'e' || s[e] == 'E' || s[e] == '+' || s[e] == '-')) ++e;
            o.add(i, e, Tok::Number); i = e; continue;
        }
        if (is_ident0(c)) {
            size_t e = i;
            while (e < n && is_ident(s[e])) ++e;
            const std::string w = s.substr(i, e - i);
            o.add(i, e, (w == "true" || w == "false" || w == "null") ? Tok::Keyword : Tok::Error);
            i = e; continue;
        }
        if (std::strchr("{}[]:,", c)) { o.add(i, i + 1, Tok::Punct); ++i; continue; }
        o.add(i, i + 1, Tok::Text); ++i;
    }
    return 0;
}

// ---- hash-comment languages: Makefile, shell, CMake, sprdef ----
inline int lex_hash(const std::string& s, Lang lang, Out& o) {
    const size_t n = s.size();
    size_t i = 0;
    const size_t fb = s.find_first_not_of(" \t");
    // Makefile rule target "name: deps" — the target is a Func
    if (lang == Lang::Make && fb == 0 && n && s[0] != '\t') {
        const size_t colon = s.find(':');
        const size_t eq = s.find('=');
        if (colon != std::string::npos && (eq == std::string::npos || colon < eq) &&
            (colon + 1 >= n || s[colon + 1] != '=') && s[0] != '#') {
            o.add(0, colon, Tok::Func);
            i = colon;
        }
    }
    if (lang == Lang::Sprdef && fb != std::string::npos && s[fb] != '#') {
        size_t e = fb;
        while (e < n && is_ident(s[e])) ++e;
        o.add(fb, e, Tok::Keyword);                     // sheet / clip
        i = e;
    }
    while (i < n) {
        const char c = s[i];
        if (c == '#') { o.add(i, n, Tok::Comment); break; }
        if (c == '"' || c == '\'') { const size_t e = scan_quoted(s, i); o.add(i, e, Tok::String); i = e; continue; }
        if (c == '$' && i + 1 < n && (s[i + 1] == '(' || s[i + 1] == '{')) {
            const char close = s[i + 1] == '(' ? ')' : '}';
            int depth = 0;
            size_t e = i + 1;
            for (; e < n; ++e) { if (s[e] == s[i + 1]) ++depth; else if (s[e] == close && --depth == 0) { ++e; break; } }
            o.add(i, e, Tok::Variable); i = e; continue;
        }
        if (c == '$' && i + 1 < n) {                    // $x, $@, $<, $1 ...
            size_t k = i + 1;
            if (is_ident0(s[k])) while (k < n && is_ident(s[k])) ++k;
            else ++k;
            o.add(i, k, Tok::Variable); i = k; continue;
        }
        if (is_digit(c)) { size_t e = i; while (e < n && (is_ident(s[e]) || s[e] == '.')) ++e; o.add(i, e, Tok::Number); i = e; continue; }
        if (is_ident0(c)) {
            size_t e = i;
            while (e < n && (is_ident(s[e]) || s[e] == '-')) ++e;
            const std::string w = s.substr(i, e - i);
            Tok t = Tok::Text;
            if (lang == Lang::CMake) {
                std::string lw = w;
                for (char& ch : lw) if (ch >= 'A' && ch <= 'Z') ch = char(ch - 'A' + 'a');
                if (in_list(cmake_keywords(), lw)) t = Tok::Keyword;
                else if (looks_constant(w)) t = Tok::Constant;
            } else if (lang != Lang::Sprdef) {
                if (in_list(sh_keywords(), w)) t = Tok::Keyword;
                else if (looks_constant(w) && lang == Lang::Make) t = Tok::Constant;
            }
            o.add(i, e, t); i = e; continue;
        }
        if (std::strchr("=:;|&<>(){}[]@%+?!", c)) { o.add(i, i + 1, Tok::Punct); ++i; continue; }
        o.add(i, i + 1, Tok::Text); ++i;
    }
    return 0;
}

// ---- Python ----
inline int lex_python(const std::string& s, int state, Out& o) {
    const size_t n = s.size();
    size_t i = 0;
    if (state == 2 || state == 3) {                     // inside """ or '''
        const char* q = state == 2 ? "\"\"\"" : "'''";
        const size_t e = s.find(q);
        if (e == std::string::npos) { o.add(0, n, Tok::String); return state; }
        o.add(0, e + 3, Tok::String);
        i = e + 3;
    }
    while (i < n) {
        const char c = s[i];
        if (c == '#') { o.add(i, n, Tok::Comment); break; }
        if ((c == '"' || c == '\'') && i + 2 < n && s[i + 1] == c && s[i + 2] == c) {
            const char q[4] = { c, c, c, 0 };
            const size_t e = s.find(q, i + 3);
            if (e == std::string::npos) { o.add(i, n, Tok::String); return c == '"' ? 2 : 3; }
            o.add(i, e + 3, Tok::String); i = e + 3; continue;
        }
        if (c == '"' || c == '\'') { const size_t e = scan_quoted(s, i); o.add(i, e, Tok::String); i = e; continue; }
        if (is_digit(c)) { const size_t e = scan_number(s, i); o.add(i, e, Tok::Number); i = e; continue; }
        if (is_ident0(c)) {
            size_t e = i;
            while (e < n && is_ident(s[e])) ++e;
            const std::string w = s.substr(i, e - i);
            Tok t = Tok::Text;
            if (in_list(py_keywords(), w)) t = Tok::Keyword;
            else if (looks_constant(w)) t = Tok::Constant;
            else if (e < n && s[e] == '(') t = Tok::Func;
            o.add(i, e, t); i = e; continue;
        }
        if (c == '@') { size_t e = i + 1; while (e < n && (is_ident(s[e]) || s[e] == '.')) ++e; o.add(i, e, Tok::Preproc); i = e; continue; }
        if (std::strchr("{}()[]:,.=+-*/%<>!&|^~", c)) { o.add(i, i + 1, Tok::Punct); ++i; continue; }
        o.add(i, i + 1, Tok::Text); ++i;
    }
    return 0;
}

// ---- Markdown ----
inline int lex_markdown(const std::string& s, int state, Out& o) {
    const size_t n = s.size();
    const size_t fb = s.find_first_not_of(" \t");
    const bool fence = fb != std::string::npos && s.compare(fb, 3, "```") == 0;
    if (state == 4) {                                   // inside a ``` fence
        o.add(0, n, fence ? Tok::Preproc : Tok::String);
        return fence ? 0 : 4;
    }
    if (fence) { o.add(0, n, Tok::Preproc); return 4; }
    if (fb != std::string::npos && s[fb] == '#') { o.add(0, n, Tok::Heading); return 0; }
    if (fb != std::string::npos && s[fb] == '>') { o.add(0, n, Tok::Comment); return 0; }
    size_t i = 0;
    if (fb != std::string::npos && (s[fb] == '-' || s[fb] == '*' || s[fb] == '|') ) { o.add(fb, fb + 1, Tok::Punct); o.add(0, fb, Tok::Text); i = fb + 1; }
    while (i < n) {
        const char c = s[i];
        if (c == '`') {
            const size_t e = s.find('`', i + 1);
            const size_t end = e == std::string::npos ? n : e + 1;
            o.add(i, end, Tok::String); i = end; continue;
        }
        if (c == '*' && i + 1 < n && s[i + 1] == '*') {
            const size_t e = s.find("**", i + 2);
            const size_t end = e == std::string::npos ? n : e + 2;
            o.add(i, end, Tok::Emphasis); i = end; continue;
        }
        if (c == '[') {
            const size_t e = s.find(']', i);
            if (e != std::string::npos && e + 1 < n && s[e + 1] == '(') {
                const size_t p = s.find(')', e);
                const size_t end = p == std::string::npos ? n : p + 1;
                o.add(i, e + 1, Tok::Func);
                o.add(e + 1, end, Tok::Comment);
                i = end; continue;
            }
        }
        if (c == '|') { o.add(i, i + 1, Tok::Punct); ++i; continue; }
        o.add(i, i + 1, Tok::Text); ++i;
    }
    return 0;
}

} // namespace syn

// Lex one line. `state` = the state at the line's start; returns the state at its end.
inline int highlight_line(Lang lang, const std::string& line, int state, std::vector<Span>& out) {
    out.clear();
    syn::Out o{ out };
    switch (lang) {
    case Lang::Cpp:      return syn::lex_cpp(line, state, o);
    case Lang::Json:     return syn::lex_json(line, o);
    case Lang::Make: case Lang::Shell: case Lang::CMake: case Lang::Sprdef:
                         return syn::lex_hash(line, lang, o);
    case Lang::Python:   return syn::lex_python(line, state, o);
    case Lang::Markdown: return syn::lex_markdown(line, state, o);
    default:             o.add(0, line.size(), Tok::Text); return 0;
    }
}

// Bracket matching for the caret: given lines and a position ON a bracket, find its partner.
// Ignores brackets inside quotes on the same line (good enough for an editor highlight).
inline bool match_bracket(const std::vector<std::string>& lines, int l, int c, int& ml, int& mc) {
    if (l < 0 || l >= int(lines.size()) || c < 0 || c >= int(lines[size_t(l)].size())) return false;
    const char ch = lines[size_t(l)][size_t(c)];
    const char* open = "([{", *close = ")]}";
    const char* po = std::strchr(open, ch);
    const char* pc = std::strchr(close, ch);
    if (!ch || (!po && !pc)) return false;
    const bool fwd = po != nullptr;
    const char me = ch, other = fwd ? close[po - open] : open[pc - close];
    int depth = 0;
    int li = l, ci = c;
    for (int guard = 0; guard < 200000; ++guard) {
        const std::string& s = lines[size_t(li)];
        if (ci >= 0 && ci < int(s.size())) {
            if (s[size_t(ci)] == me) ++depth;
            else if (s[size_t(ci)] == other && --depth == 0) { ml = li; mc = ci; return true; }
        }
        if (fwd) {
            if (++ci >= int(s.size())) { if (++li >= int(lines.size())) return false; ci = -1; }
        } else {
            if (--ci < 0) { if (--li < 0) return false; ci = int(lines[size_t(li)].size()); }
        }
    }
    return false;
}

} // namespace phxstudio
#endif // PHX_TOOLS_PHXSTUDIO_SYNTAX_H
