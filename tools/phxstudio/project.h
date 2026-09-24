// tools/phxstudio/project.h — the Studio workspace's headless helpers (unit-tested in the editors
// suite): what KIND of document a file is (which editor opens it), the repository file tree the
// Explorer shows, fuzzy matching for quick-open, parsing compiler diagnostics out of build output
// (so errors in the Run console jump to the code editor), and the starter content for new
// documents. Host-only (STL + <filesystem>).
#ifndef PHX_TOOLS_PHXSTUDIO_PROJECT_H
#define PHX_TOOLS_PHXSTUDIO_PROJECT_H

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace phxstudio {

namespace pfs = std::filesystem;

enum class FileKind : uint8_t { Dir, Code, Text, Image, Sprite, Map, Table, Sound, Bundle, Other };

inline const char* kind_name(FileKind k) {
    switch (k) {
    case FileKind::Dir: return "folder";   case FileKind::Code: return "code";    case FileKind::Text: return "text";
    case FileKind::Image: return "image";  case FileKind::Sprite: return "sprite"; case FileKind::Map: return "map";
    case FileKind::Table: return "table";  case FileKind::Sound: return "sound";   case FileKind::Bundle: return "bundle";
    default: return "file";
    }
}

inline std::string lower_ext(const std::string& path) {
    const size_t sl = path.find_last_of("/\\");
    const size_t d = path.find_last_of('.');
    if (d == std::string::npos || (sl != std::string::npos && d < sl)) return "";
    std::string e = path.substr(d);
    for (char& c : e) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return e;
}
inline std::string base_name(const std::string& path) {
    const size_t sl = path.find_last_of("/\\");
    return sl == std::string::npos ? path : path.substr(sl + 1);
}
inline std::string dir_name(const std::string& path) {
    const size_t sl = path.find_last_of("/\\");
    return sl == std::string::npos ? std::string() : path.substr(0, sl);
}
inline std::string stem_of(const std::string& path) {
    std::string b = base_name(path);
    const size_t d = b.find_last_of('.');
    return d == std::string::npos || d == 0 ? b : b.substr(0, d);
}
inline std::string join_path(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    if (b[0] == '/') return b;
    return a.back() == '/' ? a + b : a + "/" + b;
}

// Kind by extension; JSON needs a peek at its content (a phxbin table, a sprite sidecar, or
// plain JSON), which `json_head` provides (the first few KB of the file, or empty = unknown).
inline FileKind kind_for(const std::string& path, const std::string& json_head = "") {
    const std::string e = lower_ext(path);
    const std::string b = base_name(path);
    if (e == ".png" || e == ".ppm") return FileKind::Image;
    if (e == ".sprdef") return FileKind::Sprite;
    if (e == ".tmj") return FileKind::Map;
    if (e == ".wav") return FileKind::Sound;
    if (e == ".phxp") return FileKind::Bundle;
    if (e == ".json") {
        if (json_head.find("\"records\"") != std::string::npos && json_head.find("\"fields\"") != std::string::npos) return FileKind::Table;
        if (json_head.find("\"animations\"") != std::string::npos && json_head.find("\"image\"") != std::string::npos) return FileKind::Sprite;
        if (json_head.find("\"tilesets\"") != std::string::npos && json_head.find("\"layers\"") != std::string::npos) return FileKind::Map;
        return FileKind::Code;
    }
    for (const char* x : { ".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".inl", ".py", ".sh", ".cmake", ".mk", ".yml", ".yaml", ".s", ".ld" })
        if (e == x) return FileKind::Code;
    if (b == "Makefile" || b == "CMakeLists.txt" || b == "makefile") return FileKind::Code;
    for (const char* x : { ".md", ".txt", ".csv", ".tmcsv", ".cfg", ".ini", ".toml", ".gitignore", ".clang-format" })
        if (e == x || b == x) return FileKind::Text;
    if (b == "LICENSE" || b == "README" || b == ".gitignore") return FileKind::Text;
    return FileKind::Other;
}

// Read up to `n` bytes (JSON sniffing; also "is this binary?").
inline std::string read_head(const std::string& path, size_t n = 4096) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return "";
    std::string s(n, '\0');
    const size_t got = std::fread(&s[0], 1, n, f);
    std::fclose(f);
    s.resize(got);
    return s;
}
// The author files an asset named `stem` was baked from, among `files` (paths relative to
// `root`), best first. The bake names every asset by its file stem (bake_project.py and the
// converters' --name), so a source is a file with that stem that opens in the editor of kind
// `want` (Image for a texture, Sprite, Map for a tilemap or its spawns, Table for a blob, Sound).
// Copies under a build/ folder sort after the author's own files.
inline std::vector<std::string> asset_source_candidates(const std::vector<std::string>& files, const std::string& root,
                                                        const std::string& stem, FileKind want) {
    std::vector<std::string> out;
    for (const std::string& rel : files) {
        if (stem_of(rel) != stem) continue;
        const std::string e = lower_ext(rel);
        if (e == ".ppm") continue;                                  // a screenshot, never baked
        if (kind_for(rel, e == ".json" ? read_head(join_path(root, rel)) : std::string()) == want) out.push_back(rel);
    }
    std::stable_partition(out.begin(), out.end(), [](const std::string& r) {
        return r.compare(0, 6, "build/") != 0 && r.find("/build/") == std::string::npos;
    });
    return out;
}

inline bool looks_binary(const std::string& head) {
    for (char c : head) if (c == '\0') return true;
    return false;
}

// ---- the Explorer tree ----
struct FileNode {
    std::string name, path;      // path relative to the root ("" for the root itself)
    FileKind kind = FileKind::Other;
    bool dir = false, expanded = false, loaded = false;
    std::vector<FileNode> kids;
};

// Directories the Explorer never lists (VCS internals, per-config object dirs, caches).
inline bool hidden_entry(const std::string& name, bool dir) {
    if (name == ".git" || name == ".cache" || name == ".vscode" || name == ".idea" || name == "__pycache__") return true;
    if (dir && (name.rfind("obj-", 0) == 0 || name == "asan" || name == "cmake-check" || name == "cmake-verify" || name == "CMakeFiles")) return true;
    return false;
}

class FileTree {
public:
    std::string root;            // absolute
    FileNode top;

    void open(const std::string& root_abs) {
        root = root_abs;
        top = FileNode{};
        top.dir = true; top.expanded = true; top.kind = FileKind::Dir;
        load(top);
    }
    std::string abs(const std::string& rel) const { return rel.empty() ? root : join_path(root, rel); }

    // (Re)list one directory's entries: folders first, then files, both by name.
    void load(FileNode& n) {
        std::vector<FileNode> old;
        old.swap(n.kids);
        std::error_code ec;
        for (pfs::directory_iterator it(abs(n.path), ec), end; !ec && it != end; it.increment(ec)) {
            const std::string name = it->path().filename().string();
            const bool dir = it->is_directory(ec);
            if (hidden_entry(name, dir)) continue;
            FileNode k;
            k.name = name;
            k.path = n.path.empty() ? name : n.path + "/" + name;
            k.dir = dir;
            if (dir) k.kind = FileKind::Dir;
            else {
                const std::string e = lower_ext(name);
                k.kind = kind_for(name, e == ".json" ? read_head(abs(k.path), 2048) : "");
            }
            for (FileNode& o : old)               // keep expansion state across refreshes
                if (o.path == k.path && o.dir) { k.expanded = o.expanded; if (o.loaded && o.expanded) { k.kids.swap(o.kids); k.loaded = true; } }
            n.kids.push_back(std::move(k));
        }
        std::sort(n.kids.begin(), n.kids.end(), [](const FileNode& a, const FileNode& b) {
            if (a.dir != b.dir) return a.dir;
            return a.name < b.name;
        });
        n.loaded = true;
        for (FileNode& k : n.kids) if (k.dir && k.expanded) load(k);
    }
    void refresh() { load(top); }

    struct Row { FileNode* node; int depth; };
    // The visible rows (expanded folders' children), optionally filtered: with a filter every
    // matching FILE anywhere under a loaded folder shows, flattened with its relative path.
    std::vector<Row> rows() {
        std::vector<Row> out;
        walk(top, -1, out);
        return out;
    }
    FileNode* find(const std::string& rel) { return find_in(top, rel); }
    // Expand every folder on the way to `rel` (loading as needed) so it becomes visible.
    void reveal(const std::string& rel) {
        FileNode* n = &top;
        size_t p = 0;
        while (n && p < rel.size()) {
            const size_t e = rel.find('/', p);
            if (e == std::string::npos) break;
            const std::string want = rel.substr(0, e);
            FileNode* next = nullptr;
            if (!n->loaded) load(*n);
            for (FileNode& k : n->kids) if (k.path == want) next = &k;
            if (!next) break;
            next->expanded = true;
            if (!next->loaded) load(*next);
            n = next;
            p = e + 1;
        }
    }
    // Every file under the root (for quick-open), skipping hidden dirs; capped.
    std::vector<std::string> all_files(size_t cap = 20000) const {
        std::vector<std::string> out;
        std::error_code ec;
        pfs::recursive_directory_iterator it(root, pfs::directory_options::skip_permission_denied, ec), end;
        for (; !ec && it != end && out.size() < cap; it.increment(ec)) {
            const std::string name = it->path().filename().string();
            const bool dir = it->is_directory(ec);
            if (hidden_entry(name, dir)) { if (dir) it.disable_recursion_pending(); continue; }
            if (dir) continue;
            std::string rel = pfs::relative(it->path(), root, ec).generic_string();
            if (!ec) out.push_back(rel);
        }
        std::sort(out.begin(), out.end());
        return out;
    }

private:
    void walk(FileNode& n, int depth, std::vector<Row>& out) {
        if (depth >= 0) out.push_back(Row{ &n, depth });
        if (n.dir && n.expanded) {
            if (!n.loaded) load(n);
            for (FileNode& k : n.kids) walk(k, depth + 1, out);
        }
    }
    static FileNode* find_in(FileNode& n, const std::string& rel) {
        if (n.path == rel) return &n;
        for (FileNode& k : n.kids)
            if (rel == k.path || (k.dir && rel.compare(0, k.path.size() + 1, k.path + "/") == 0))
                if (FileNode* f = find_in(k, rel)) return f;
        return nullptr;
    }
};

// ---- fuzzy matching (quick open) ----
// Subsequence match of `q` in `s` (case-insensitive) scored by consecutive runs and word starts
// ('/', '_', '.', '-'), normalised by length. A match inside the FILE NAME scores double, so
// "rend" ranks renderer.cpp above docs/03-rendering-notes.md. < 0 = no match.
inline int fuzzy_run_score(const std::string& q, const std::string& s) {
    auto low = [](char c) { return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c; };
    int score = 0, run = 0;
    size_t qi = 0;
    for (size_t i = 0; i < s.size() && qi < q.size(); ++i) {
        if (low(s[i]) == low(q[qi])) {
            ++qi;
            ++run;
            score += 1 + run * 2;
            if (i == 0 || s[i - 1] == '/' || s[i - 1] == '_' || s[i - 1] == '.' || s[i - 1] == '-') score += 6;
        } else run = 0;
    }
    if (qi < q.size()) return -1;
    return score * 100 / int(10 + s.size());
}
inline int fuzzy_score(const std::string& q, const std::string& s) {
    if (q.empty()) return 0;
    const size_t sl = s.find_last_of('/');
    const std::string name = sl == std::string::npos ? s : s.substr(sl + 1);
    const int in_name = fuzzy_run_score(q, name);
    if (in_name >= 0) return 2 * in_name + 1;
    return fuzzy_run_score(q, s);
}

// ---- compiler diagnostics ----
struct Diag {
    std::string file;            // as printed (usually relative to the repo root)
    int line = 0, col = 0;
    int severity = 0;            // 2 error, 1 warning, 0 note
    std::string msg;
};
// Parse "path:line[:col]: (fatal )error|warning|note: message" (GCC/Clang style). Returns false
// for any other line.
inline bool parse_diag(const std::string& line, Diag& d) {
    size_t p = 0;
    // skip a leading "In file included from" etc. — those don't match the pattern anyway
    const size_t c1 = line.find(':', p);
    if (c1 == std::string::npos || c1 == 0) return false;
    // Windows drive letters are not a concern on the host tools' platforms (POSIX shell).
    const std::string file = line.substr(0, c1);
    if (file.find(' ') != std::string::npos) return false;
    size_t q = c1 + 1;
    int ln = 0, col = 0;
    size_t k = q;
    while (k < line.size() && line[k] >= '0' && line[k] <= '9') ln = ln * 10 + (line[k++] - '0');
    if (k == q || k >= line.size() || line[k] != ':') return false;
    q = k + 1;
    k = q;
    while (k < line.size() && line[k] >= '0' && line[k] <= '9') col = col * 10 + (line[k++] - '0');
    if (k > q && k < line.size() && line[k] == ':') q = k + 1; else col = 0;
    while (q < line.size() && line[q] == ' ') ++q;
    const std::string rest = line.substr(q);
    auto starts = [&](const char* s) { return rest.compare(0, std::char_traits<char>::length(s), s) == 0; };
    int sev = -1;
    size_t skip = 0;
    if (starts("fatal error:")) { sev = 2; skip = 12; }
    else if (starts("error:"))  { sev = 2; skip = 6; }
    else if (starts("warning:")) { sev = 1; skip = 8; }
    else if (starts("note:"))   { sev = 0; skip = 5; }
    if (sev < 0) return false;
    d.file = file; d.line = ln; d.col = col; d.severity = sev;
    d.msg = rest.substr(skip);
    while (!d.msg.empty() && d.msg[0] == ' ') d.msg.erase(0, 1);
    return true;
}

// ---- new-document starters ----
inline std::string header_guard_for(const std::string& rel) {
    std::string g;
    for (char c : rel) g += (c >= 'a' && c <= 'z') ? char(c - 'a' + 'A') : ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) ? c : '_';
    return g;
}
// Starter text for a new code file (by extension), following the repo's house style.
inline std::string code_template(const std::string& rel) {
    const std::string e = lower_ext(rel);
    const std::string b = base_name(rel);
    if (e == ".h" || e == ".hpp") {
        const std::string g = header_guard_for(rel);
        return "// " + rel + " — \n#ifndef " + g + "\n#define " + g + "\n\n\n\n#endif // " + g + "\n";
    }
    if (e == ".cpp" || e == ".cc") return "// " + rel + " — \n\n";
    if (e == ".md") return "# " + stem_of(rel) + "\n\n";
    if (e == ".py") return "#!/usr/bin/env python3\n\"\"\"" + b + " — \"\"\"\n\n";
    if (e == ".sh") return "#!/bin/sh\n# " + b + " — \n\n";
    return "";
}

} // namespace phxstudio
#endif // PHX_TOOLS_PHXSTUDIO_PROJECT_H
