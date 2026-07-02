// tools/phxentity/editor.h — the entity/prefab editor's DOCUMENT model, separated from the
// GUI so it is unit-testable headlessly. Loads/saves the phxbin author JSON (docs/08 §1:
// editors output author formats the converters bake): a typed record table
//   { "struct":"Name", "fields":[{"name","type"}...], "records":[{k:v}...] }
// with integer field types (u8/u16/u32/i8/i16/i32). Host-only (STL fine).
#ifndef PHX_TOOLS_PHXENTITY_EDITOR_H
#define PHX_TOOLS_PHXENTITY_EDITOR_H

#include "json.h"   // tools/phxpack — the one JSON parser

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace phxtool {

class BinDoc {
public:
    struct Field { std::string name, type; };

    std::string struct_name = "Record";
    std::vector<Field> fields;
    std::vector<std::vector<int64_t>> records;   // records[r][f]
    bool dirty = false;

    static bool load(const std::string& json_text, BinDoc& out) {
        JsonValue root;
        if (!JsonParser::parse(json_text, root) || !root.is_obj()) return false;
        out.struct_name = root.str_at("struct");
        if (out.struct_name.empty()) out.struct_name = "Record";
        const JsonValue* fs = root.find("fields");
        if (!fs || !fs->is_arr() || fs->arr.empty()) return false;
        out.fields.clear();
        for (const JsonValue& f : fs->arr)
            out.fields.push_back(Field{ f.str_at("name"), f.str_at("type") });
        out.records.clear();
        if (const JsonValue* rs = root.find("records"); rs && rs->is_arr())
            for (const JsonValue& r : rs->arr) {
                std::vector<int64_t> rec(out.fields.size(), 0);
                for (size_t i = 0; i < out.fields.size(); ++i)
                    if (const JsonValue* v = r.find(out.fields[i].name.c_str()))
                        rec[i] = int64_t(v->as_num());
                out.records.push_back(std::move(rec));
            }
        return true;
    }

    // ---- edits ----
    static void type_range(const std::string& t, int64_t& lo, int64_t& hi) {
        if      (t == "u8")  { lo = 0;           hi = 0xFF; }
        else if (t == "u16") { lo = 0;           hi = 0xFFFF; }
        else if (t == "u32") { lo = 0;           hi = 0xFFFFFFFFll; }
        else if (t == "i8")  { lo = -128;        hi = 127; }
        else if (t == "i16") { lo = -32768;      hi = 32767; }
        else                 { lo = -0x80000000ll; hi = 0x7FFFFFFFll; }   // i32 / unknown
    }
    void step(size_t rec, size_t field, int64_t delta) {
        if (rec >= records.size() || field >= fields.size()) return;
        int64_t lo, hi; type_range(fields[field].type, lo, hi);
        int64_t v = records[rec][field] + delta;
        records[rec][field] = v < lo ? lo : (v > hi ? hi : v);
        dirty = true;
    }
    // New record: a clone of `from` (or zeros when none exist / out of range).
    void add_record(size_t from) {
        records.push_back(from < records.size() ? records[from]
                                                : std::vector<int64_t>(fields.size(), 0));
        dirty = true;
    }
    void remove_record(size_t rec) {
        if (rec >= records.size()) return;
        records.erase(records.begin() + long(rec));
        dirty = true;
    }

    // ---- save: the exact dialect phxbin's build_bin parses back ----
    std::string save_json() const {
        std::string j = "{ \"struct\":\"" + struct_name + "\",\n  \"fields\":[";
        for (size_t i = 0; i < fields.size(); ++i) {
            if (i) j += ", ";
            j += "{\"name\":\"" + fields[i].name + "\",\"type\":\"" + fields[i].type + "\"}";
        }
        j += "],\n  \"records\":[";
        for (size_t r = 0; r < records.size(); ++r) {
            if (r) j += ",";
            j += "\n    {";
            for (size_t f = 0; f < fields.size(); ++f) {
                if (f) j += ",";
                j += "\"" + fields[f].name + "\":" + std::to_string(records[r][f]);
            }
            j += "}";
        }
        j += "\n] }\n";
        return j;
    }

    bool save_file(const std::string& path) {
        const std::string j = save_json();
        FILE* f = std::fopen(path.c_str(), "wb");
        if (!f) return false;
        const bool ok = std::fwrite(j.data(), 1, j.size(), f) == j.size();
        std::fclose(f);
        if (ok) dirty = false;
        return ok;
    }
};

} // namespace phxtool
#endif // PHX_TOOLS_PHXENTITY_EDITOR_H
