// tools/phxentity/editor.h — the entity/prefab editor's DOCUMENT model, separated from the
// GUI so it is unit-testable headlessly. Loads/saves the phxbin author JSON (docs/08 §1:
// editors output author formats the converters bake): a typed record table
//   { "struct":"Name", "fields":[{"name","type"}...], "records":[{k:v}...] }
// with integer field types (u8/u16/u32/i8/i16/i32), f32, and string fields (str8/str16/str32 —
// phxbin bakes them as inline NUL-terminated char[N], so a strN cell holds at most N-1 chars).
// A string column NAMES records, which is what makes a table a PREFAB SCHEMA: the game hashes
// the name to match baked spawn types, and the map editor reads the same table as its placeable-
// entity vocabulary (name_column()). Every value the model accepts is one the baked struct can
// hold: integers clamp to their type's range, strings clip to their capacity, identifiers are
// validated. Bounded snapshot undo. Host-only (STL fine).
#ifndef PHX_TOOLS_PHXENTITY_EDITOR_H
#define PHX_TOOLS_PHXENTITY_EDITOR_H

#include "json.h"   // tools/phxpack — the one JSON parser

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
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
    std::vector<std::vector<double>> flt_cells;       // records[r][f] (f32 cells; parallel)
    bool dirty = false;

    // The integer field types (stepped with ±1/±10 and clamped to the type's range).
    static bool valid_type(const std::string& t) {
        return t == "u8" || t == "i8" || t == "u16" || t == "i16" || t == "u32" || t == "i32";
    }
    // String field types (baked by phxbin as NUL-terminated char[N]).
    static bool str_type(const std::string& t) {
        return t == "str8" || t == "str16" || t == "str32";
    }
    static bool flt_type(const std::string& t) { return t == "f32"; }
    // Every type a schema may declare (valid for --new/--fields and add_field).
    static bool schema_type(const std::string& t) { return valid_type(t) || str_type(t) || flt_type(t); }
    static const std::vector<std::string>& all_types() {
        static const std::vector<std::string> k = { "u8", "i8", "u16", "i16", "u32", "i32", "f32", "str8", "str16", "str32" };
        return k;
    }
    // Max characters a strN cell can hold (N-1: the NUL terminator takes one byte).
    static size_t str_capacity(const std::string& t) {
        return t == "str8" ? 7 : t == "str16" ? 15 : t == "str32" ? 31 : 0;
    }
    bool field_is_str(size_t f) const { return f < fields.size() && str_type(fields[f].type); }
    bool field_is_flt(size_t f) const { return f < fields.size() && flt_type(fields[f].type); }
    // C identifier (the field becomes a struct member in the generated header).
    static bool valid_ident(const std::string& s) {
        if (s.empty() || s.size() > 48) return false;
        if (!((s[0] >= 'a' && s[0] <= 'z') || (s[0] >= 'A' && s[0] <= 'Z') || s[0] == '_')) return false;
        for (char c : s) if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) return false;
        return true;
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
        out.fields.clear(); out.records.clear(); out.str_cells.clear(); out.flt_cells.clear();
        for (const std::string& fs : field_specs) {
            const size_t c = fs.find(':');
            if (c == std::string::npos || c == 0)
                return fail("bad field spec '" + fs + "' (want name:type, e.g. hp:u16)");
            Field f{ fs.substr(0, c), fs.substr(c + 1) };
            if (!schema_type(f.type))
                return fail("bad field type '" + f.type + "' in '" + fs +
                            "' (want u8/i8/u16/i16/u32/i32/f32 or str8/str16/str32)");
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
        out.records.clear(); out.str_cells.clear(); out.flt_cells.clear();
        if (const JsonValue* rs = root.find("records"); rs && rs->is_arr())
            for (const JsonValue& r : rs->arr) {
                std::vector<int64_t> rec(out.fields.size(), 0);
                std::vector<std::string> srec(out.fields.size());
                std::vector<double> frec(out.fields.size(), 0.0);
                for (size_t i = 0; i < out.fields.size(); ++i)
                    if (const JsonValue* v = r.find(out.fields[i].name.c_str())) {
                        if (str_type(out.fields[i].type))      srec[i] = v->as_str();
                        else if (flt_type(out.fields[i].type)) frec[i] = v->as_num();
                        else                                   rec[i] = int64_t(v->as_num());
                    }
                out.records.push_back(std::move(rec));
                out.str_cells.push_back(std::move(srec));
                out.flt_cells.push_back(std::move(frec));
            }
        out.undo_.clear(); out.redo_.clear();
        return true;
    }

    // ---- cells ----
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
        if (str_type(fields[field].type)) return;    // strings don't step
        if (flt_type(fields[field].type)) {
            flt_cells[rec][field] = double(float(flt_cells[rec][field] + double(delta)));
            dirty = true;
            return;
        }
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
    double flt_cell(size_t rec, size_t field) const {
        return rec < flt_cells.size() && field < flt_cells[rec].size() ? flt_cells[rec][field] : 0.0;
    }
    void set_str(size_t rec, size_t field, const std::string& v) {
        if (rec >= records.size() || field >= fields.size() || !str_type(fields[field].type)) return;
        str_cells[rec][field] = v.substr(0, str_capacity(fields[field].type));
        dirty = true;
    }
    // Display text of any cell.
    std::string cell_text(size_t rec, size_t field) const {
        if (rec >= records.size() || field >= fields.size()) return "";
        if (str_type(fields[field].type)) return str_cells[rec][field];
        if (flt_type(fields[field].type)) return fmt_float(flt_cells[rec][field]);
        return std::to_string(records[rec][field]);
    }
    // Set any cell from typed text. Integers parse (dec/hex/0b-less) and CLAMP to the type;
    // floats round to f32; strings clip to capacity. Returns false when the text does not parse.
    bool set_cell_text(size_t rec, size_t field, const std::string& text) {
        if (rec >= records.size() || field >= fields.size()) return false;
        const std::string& t = fields[field].type;
        if (str_type(t)) { set_str(rec, field, text); return true; }
        const char* s = text.c_str();
        while (*s == ' ') ++s;
        char* end = nullptr;
        if (flt_type(t)) {
            const double v = std::strtod(s, &end);
            if (end == s) return false;
            flt_cells[rec][field] = double(float(v));
        } else {
            const long long v = std::strtoll(s, &end, 0);
            if (end == s) return false;
            int64_t lo, hi; type_range(t, lo, hi);
            records[rec][field] = std::max<int64_t>(lo, std::min<int64_t>(hi, v));
        }
        dirty = true;
        return true;
    }

    // ---- records ----
    // New record: a clone of `from` (or zeros/empties when none exist / out of range).
    void add_record(size_t from) {
        const bool clone = from < records.size();
        records.push_back(clone ? records[from] : std::vector<int64_t>(fields.size(), 0));
        str_cells.push_back(clone ? str_cells[from] : std::vector<std::string>(fields.size()));
        flt_cells.push_back(clone ? flt_cells[from] : std::vector<double>(fields.size(), 0.0));
        dirty = true;
    }
    // Insert a zeroed (or cloned) record AT index `at` (the rows below shift down).
    void insert_record(size_t at, bool clone_prev) {
        at = std::min(at, records.size());
        const bool clone = clone_prev && at > 0;
        std::vector<int64_t> r = clone ? records[at - 1] : std::vector<int64_t>(fields.size(), 0);
        std::vector<std::string> s = clone ? str_cells[at - 1] : std::vector<std::string>(fields.size());
        std::vector<double> f = clone ? flt_cells[at - 1] : std::vector<double>(fields.size(), 0.0);
        records.insert(records.begin() + long(at), r);
        str_cells.insert(str_cells.begin() + long(at), s);
        flt_cells.insert(flt_cells.begin() + long(at), f);
        dirty = true;
    }
    void remove_record(size_t rec) {
        if (rec >= records.size()) return;
        records.erase(records.begin() + long(rec));
        str_cells.erase(str_cells.begin() + long(rec));
        flt_cells.erase(flt_cells.begin() + long(rec));
        dirty = true;
    }
    bool move_record(size_t rec, int dir) {
        const long to = long(rec) + dir;
        if (rec >= records.size() || to < 0 || to >= long(records.size())) return false;
        std::swap(records[rec], records[size_t(to)]);
        std::swap(str_cells[rec], str_cells[size_t(to)]);
        std::swap(flt_cells[rec], flt_cells[size_t(to)]);
        dirty = true;
        return true;
    }

    // ---- schema edits: grow/shrink every record with the field list (values default 0/"") ----
    bool add_field(const std::string& name, const std::string& type) {
        if (name.empty() || !schema_type(type)) return false;
        for (const Field& f : fields) if (f.name == name) return false;   // duplicate
        fields.push_back(Field{ name, type });
        for (auto& r : records) r.push_back(0);
        for (auto& s : str_cells) s.emplace_back();
        for (auto& f : flt_cells) f.push_back(0.0);
        dirty = true;
        return true;
    }
    bool remove_field(size_t field) {
        if (field >= fields.size() || fields.size() == 1) return false;   // keep >= 1 field
        fields.erase(fields.begin() + long(field));
        for (auto& r : records) r.erase(r.begin() + long(field));
        for (auto& s : str_cells) s.erase(s.begin() + long(field));
        for (auto& f : flt_cells) f.erase(f.begin() + long(field));
        dirty = true;
        return true;
    }
    bool rename_field(size_t field, const std::string& name) {
        if (field >= fields.size() || !valid_ident(name)) return false;
        for (size_t i = 0; i < fields.size(); ++i) if (i != field && fields[i].name == name) return false;
        if (fields[field].name == name) return true;
        fields[field].name = name;
        dirty = true;
        return true;
    }
    bool move_field(size_t field, int dir) {
        const long to = long(field) + dir;
        if (field >= fields.size() || to < 0 || to >= long(fields.size())) return false;
        std::swap(fields[field], fields[size_t(to)]);
        for (auto& r : records) std::swap(r[field], r[size_t(to)]);
        for (auto& s : str_cells) std::swap(s[field], s[size_t(to)]);
        for (auto& f : flt_cells) std::swap(f[field], f[size_t(to)]);
        dirty = true;
        return true;
    }
    // Change a column's type, converting every value (int<->int clamps, int<->float converts,
    // anything->string prints, string->number parses (0 when it doesn't)).
    bool set_field_type(size_t field, const std::string& type) {
        if (field >= fields.size() || !schema_type(type)) return false;
        if (fields[field].type == type) return true;
        std::vector<std::string> texts;
        for (size_t r = 0; r < records.size(); ++r) texts.push_back(cell_text(r, field));
        fields[field].type = type;
        for (size_t r = 0; r < records.size(); ++r) {
            records[r][field] = 0; str_cells[r][field].clear(); flt_cells[r][field] = 0.0;
            if (!set_cell_text(r, field, texts[r]) && !str_type(type)) {
                // e.g. "boss" -> u8: leave 0
            }
        }
        dirty = true;
        return true;
    }
    bool set_struct_name(const std::string& n) {
        if (!valid_ident(n)) return false;
        if (n != struct_name) { struct_name = n; dirty = true; }
        return true;
    }
    // A field name not in use yet ("field", "field2", ...).
    std::string fresh_field_name(const std::string& base) const {
        auto used = [&](const std::string& n) { for (const Field& f : fields) if (f.name == n) return true; return false; };
        if (!used(base)) return base;
        for (int k = 2;; ++k) { const std::string n = base + std::to_string(k); if (!used(n)) return n; }
    }

    // ---- the prefab-schema seam (docs/08 §8) ----
    // The index of the table's NAME column — the field called "type" (or "name"), else the first
    // string-typed field — or fields.size() when the table has no string column.
    size_t name_field() const {
        size_t col = fields.size();
        for (size_t f = 0; f < fields.size(); ++f)
            if (str_type(fields[f].type)) {
                if (fields[f].name == "type" || fields[f].name == "name") { col = f; break; }
                if (col == fields.size()) col = f;                        // first str fallback
            }
        return col;
    }
    // The values of the NAME column, skipping empty cells. This is the vocabulary the map editor
    // places in entity mode, and the string a game hashes to match baked spawn types. Empty when
    // the table has no string column (a plain stats table).
    std::vector<std::string> name_column() const {
        std::vector<std::string> out;
        const size_t col = name_field();
        if (col == fields.size()) return out;
        for (size_t r = 0; r < str_cells.size(); ++r)
            if (!str_cells[r][col].empty()) out.push_back(str_cells[r][col]);
        return out;
    }
    // Names that appear more than once in the NAME column (a prefab table must be unambiguous).
    std::vector<std::string> duplicate_names() const {
        std::vector<std::string> names = name_column(), dup;
        std::sort(names.begin(), names.end());
        for (size_t i = 1; i < names.size(); ++i)
            if (names[i] == names[i - 1] && (dup.empty() || dup.back() != names[i])) dup.push_back(names[i]);
        return dup;
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
                if (str_type(fields[f].type))      j += "\"" + jesc(str_cells[r][f]) + "\"";
                else if (flt_type(fields[f].type)) j += fmt_float(flt_cells[r][f]);
                else                               j += std::to_string(records[r][f]);
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

    // ---- undo/redo (bounded snapshots; the GUI pushes one per edit) ----
    void push_undo() {
        undo_.push_back(snap());
        if (undo_.size() > kMaxUndo) undo_.erase(undo_.begin());
        redo_.clear();
    }
    void drop_undo() { if (!undo_.empty()) undo_.pop_back(); }
    bool undo() {
        if (undo_.empty()) return false;
        redo_.push_back(snap());
        restore(undo_.back());
        undo_.pop_back();
        dirty = true;
        return true;
    }
    bool redo() {
        if (redo_.empty()) return false;
        undo_.push_back(snap());
        restore(redo_.back());
        redo_.pop_back();
        dirty = true;
        return true;
    }
    size_t undo_depth() const { return undo_.size(); }
    size_t redo_depth() const { return redo_.size(); }

    // Shortest decimal that round-trips through f32 (JSON has no float precision notion).
    static std::string fmt_float(double v) {
        char b[48];
        for (int p = 1; p <= 9; ++p) {
            std::snprintf(b, sizeof(b), "%.*g", p, v);
            if (float(std::strtod(b, nullptr)) == float(v)) break;
        }
        std::string s = b;
        if (s.find_first_of(".eEn") == std::string::npos) s += ".0";   // keep it visibly a float
        return s;
    }

private:
    struct Snapshot {
        std::string struct_name;
        std::vector<Field> fields;
        std::vector<std::vector<int64_t>> records;
        std::vector<std::vector<std::string>> str_cells;
        std::vector<std::vector<double>> flt_cells;
    };
    static constexpr size_t kMaxUndo = 128;
    Snapshot snap() const { return Snapshot{ struct_name, fields, records, str_cells, flt_cells }; }
    void restore(const Snapshot& s) {
        struct_name = s.struct_name; fields = s.fields; records = s.records;
        str_cells = s.str_cells; flt_cells = s.flt_cells;
    }
    std::vector<Snapshot> undo_, redo_;

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
