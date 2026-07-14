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

    // The integer field types the GUI can edit (phxbin also bakes f32, but a cell cursor
    // stepping by ±1/±10 has no sensible float story — author those in JSON directly).
    static bool valid_type(const std::string& t) {
        return t == "u8" || t == "i8" || t == "u16" || t == "i16" || t == "u32" || t == "i32";
    }

    // A fresh table from a schema (the `--new NAME --fields a:t,b:t` CLI path — bootstrap a
    // record table without hand-writing JSON). Field syntax "name:type"; returns false and
    // sets `err` on a bad spec.
    static bool blank(const std::string& sname, const std::vector<std::string>& field_specs,
                      BinDoc& out, std::string* err = nullptr) {
        auto fail = [&](const std::string& why) { if (err) *err = why; return false; };
        if (sname.empty()) return fail("struct name is empty");
        if (field_specs.empty()) return fail("a table needs at least one field");
        out.struct_name = sname;
        out.fields.clear(); out.records.clear();
        for (const std::string& fs : field_specs) {
            const size_t c = fs.find(':');
            if (c == std::string::npos || c == 0)
                return fail("bad field spec '" + fs + "' (want name:type, e.g. hp:u16)");
            Field f{ fs.substr(0, c), fs.substr(c + 1) };
            if (!valid_type(f.type))
                return fail("bad field type '" + f.type + "' in '" + fs +
                            "' (want u8/i8/u16/i16/u32/i32)");
            out.fields.push_back(std::move(f));
        }
        out.dirty = true;                        // a new table is unsaved by definition
        return true;
    }

    static bool load(const std::string& json_text, BinDoc& out, std::string* err = nullptr) {
        auto fail = [&](const std::string& why) { if (err) *err = why; return false; };
        JsonValue root;
        std::string jerr;
        if (!JsonParser::parse(json_text, root, &jerr)) return fail("invalid JSON: " + jerr);
        if (!root.is_obj()) return fail("top level is not a JSON object");
        out.struct_name = root.str_at("struct");
        if (out.struct_name.empty()) out.struct_name = "Record";
        const JsonValue* fs = root.find("fields");
        if (!fs || !fs->is_arr() || fs->arr.empty())
            return fail("needs a non-empty \"fields\" array of {\"name\",\"type\"}");
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
    // Schema edits: grow/shrink every record with the field list (values default to 0).
    bool add_field(const std::string& name, const std::string& type) {
        if (name.empty() || !valid_type(type)) return false;
        for (const Field& f : fields) if (f.name == name) return false;   // duplicate
        fields.push_back(Field{ name, type });
        for (auto& r : records) r.push_back(0);
        dirty = true;
        return true;
    }
    bool remove_field(size_t field) {
        if (field >= fields.size() || fields.size() == 1) return false;   // keep >= 1 field
        fields.erase(fields.begin() + long(field));
        for (auto& r : records) r.erase(r.begin() + long(field));
        dirty = true;
        return true;
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
