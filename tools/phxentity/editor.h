// tools/phxentity/editor.h — the entity/prefab editor's DOCUMENT model, separated from the
// GUI so it is unit-testable headlessly. Loads/saves the phxbin author JSON (docs/08 §1:
// editors output author formats the converters bake): a typed record table
//   { "struct":"Name", "fields":[{"name","type"}...], "records":[{k:v}...] }
// with integer field types (u8/u16/u32/i8/i16/i32) plus string fields (str8/str16/str32 —
// phxbin bakes them as inline NUL-terminated char[N]). A string column NAMES records, which
// is what makes a table a PREFAB SCHEMA: the game hashes the name to match baked spawn
// types, and `phxtmap --prefabs` reads the same table as its placeable-entity vocabulary
// (name_column()). Host-only (STL fine).
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
    std::vector<std::vector<int64_t>> records;        // records[r][f] (integer cells)
    std::vector<std::vector<std::string>> str_cells;  // records[r][f] (string cells; parallel)
    bool dirty = false;

    // The integer field types the GUI can step with ±1/±10 (phxbin also bakes f32, but a
    // cell cursor stepping has no sensible float story — author those in JSON directly).
    static bool valid_type(const std::string& t) {
        return t == "u8" || t == "i8" || t == "u16" || t == "i16" || t == "u32" || t == "i32";
    }
    // String field types (baked by phxbin as NUL-terminated char[N]). Shown read-only in the
    // GUI — a cell cursor has no text entry; author the strings in the JSON directly.
    static bool str_type(const std::string& t) {
        return t == "str8" || t == "str16" || t == "str32";
    }
    // Every type a schema may declare (valid for --new/--fields and add_field).
    static bool schema_type(const std::string& t) { return valid_type(t) || str_type(t); }
    bool field_is_str(size_t f) const { return f < fields.size() && str_type(fields[f].type); }

    // A fresh table from a schema (the `--new NAME --fields a:t,b:t` CLI path — bootstrap a
    // record table without hand-writing JSON). Field syntax "name:type"; returns false and
    // sets `err` on a bad spec.
    static bool blank(const std::string& sname, const std::vector<std::string>& field_specs,
                      BinDoc& out, std::string* err = nullptr) {
        auto fail = [&](const std::string& why) { if (err) *err = why; return false; };
        if (sname.empty()) return fail("struct name is empty");
        if (field_specs.empty()) return fail("a table needs at least one field");
        out.struct_name = sname;
        out.fields.clear(); out.records.clear(); out.str_cells.clear();
        for (const std::string& fs : field_specs) {
            const size_t c = fs.find(':');
            if (c == std::string::npos || c == 0)
                return fail("bad field spec '" + fs + "' (want name:type, e.g. hp:u16)");
            Field f{ fs.substr(0, c), fs.substr(c + 1) };
            if (!schema_type(f.type))
                return fail("bad field type '" + f.type + "' in '" + fs +
                            "' (want u8/i8/u16/i16/u32/i32 or str8/str16/str32)");
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
        out.records.clear(); out.str_cells.clear();
        if (const JsonValue* rs = root.find("records"); rs && rs->is_arr())
            for (const JsonValue& r : rs->arr) {
                std::vector<int64_t> rec(out.fields.size(), 0);
                std::vector<std::string> srec(out.fields.size());
                for (size_t i = 0; i < out.fields.size(); ++i)
                    if (const JsonValue* v = r.find(out.fields[i].name.c_str())) {
                        if (str_type(out.fields[i].type)) srec[i] = v->as_str();
                        else                              rec[i] = int64_t(v->as_num());
                    }
                out.records.push_back(std::move(rec));
                out.str_cells.push_back(std::move(srec));
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
        if (str_type(fields[field].type)) return;    // strings don't step; edit in the JSON
        int64_t lo, hi; type_range(fields[field].type, lo, hi);
        int64_t v = records[rec][field] + delta;
        records[rec][field] = v < lo ? lo : (v > hi ? hi : v);
        dirty = true;
    }
    // The string cell (empty for integer fields / out of range).
    const std::string& str_cell(size_t rec, size_t field) const {
        static const std::string e;
        return rec < str_cells.size() && field < str_cells[rec].size() ? str_cells[rec][field] : e;
    }
    void set_str(size_t rec, size_t field, const std::string& v) {
        if (rec >= records.size() || field >= fields.size() || !str_type(fields[field].type)) return;
        str_cells[rec][field] = v;
        dirty = true;
    }
    // New record: a clone of `from` (or zeros/empties when none exist / out of range).
    void add_record(size_t from) {
        const bool clone = from < records.size();
        records.push_back(clone ? records[from] : std::vector<int64_t>(fields.size(), 0));
        str_cells.push_back(clone ? str_cells[from] : std::vector<std::string>(fields.size()));
        dirty = true;
    }
    void remove_record(size_t rec) {
        if (rec >= records.size()) return;
        records.erase(records.begin() + long(rec));
        str_cells.erase(str_cells.begin() + long(rec));
        dirty = true;
    }
    // Schema edits: grow/shrink every record with the field list (values default to 0/"").
    bool add_field(const std::string& name, const std::string& type) {
        if (name.empty() || !schema_type(type)) return false;
        for (const Field& f : fields) if (f.name == name) return false;   // duplicate
        fields.push_back(Field{ name, type });
        for (auto& r : records) r.push_back(0);
        for (auto& s : str_cells) s.emplace_back();
        dirty = true;
        return true;
    }
    bool remove_field(size_t field) {
        if (field >= fields.size() || fields.size() == 1) return false;   // keep >= 1 field
        fields.erase(fields.begin() + long(field));
        for (auto& r : records) r.erase(r.begin() + long(field));
        for (auto& s : str_cells) s.erase(s.begin() + long(field));
        dirty = true;
        return true;
    }

    // ---- the prefab-schema seam (docs/08 §8) ----
    // The values of the table's NAME column — the field called "type" (or "name"), else the
    // first string-typed field — skipping empty cells. This is the vocabulary `phxtmap
    // --prefabs` places in entity mode, and the string a game hashes to match baked spawn
    // types. Empty when the table has no string column (a plain stats table).
    std::vector<std::string> name_column() const {
        size_t col = fields.size();
        for (size_t f = 0; f < fields.size(); ++f)
            if (str_type(fields[f].type)) {
                if (fields[f].name == "type" || fields[f].name == "name") { col = f; break; }
                if (col == fields.size()) col = f;                        // first str fallback
            }
        std::vector<std::string> out;
        if (col == fields.size()) return out;
        for (size_t r = 0; r < str_cells.size(); ++r)
            if (!str_cells[r][col].empty()) out.push_back(str_cells[r][col]);
        return out;
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
                j += "\"" + fields[f].name + "\":";
                if (str_type(fields[f].type)) j += "\"" + jesc(str_cells[r][f]) + "\"";
                else                          j += std::to_string(records[r][f]);
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

private:
    // Minimal JSON string escape (quotes/backslashes; control chars won't survive a char[N]
    // bake anyway, so authors have no reason to put them in a name).
    static std::string jesc(const std::string& s) {
        std::string o;
        for (char c : s) { if (c == '"' || c == '\\') o += '\\'; o += c; }
        return o;
    }
};

} // namespace phxtool
#endif // PHX_TOOLS_PHXENTITY_EDITOR_H
