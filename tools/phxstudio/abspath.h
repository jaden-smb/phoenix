// tools/phxstudio/abspath.h — is a path absolute? The Studio keeps every path as a '/'-separated
// string (generic_string), so on Windows an absolute path is `C:/...` (or `//server/...`), which a
// bare `p[0] == '/'` test misses.
#pragma once
#include <string>

inline bool is_abs_path(const std::string& p) {
    if (p.empty()) return false;
    const char sep = p[0];
    if (sep == '/' || sep == '\\') return true;
    const bool drive = (p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z');
    return drive && p.size() >= 3 && p[1] == ':' && (p[2] == '/' || p[2] == '\\');
}
