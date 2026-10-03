// phx/resource/table.h — read a baked data table (a phxbin Blob asset) by COLUMN NAME. phxbin
// appends a schema trailer to every table (phx/resource/bundle.h: TableFieldDef per column), so
// engine code — the level loader's prefab columns, a game's own tuning columns — can read a row
// without the game's generated header, and a table can gain columns without breaking readers:
//
//     TableView t;
//     if (TableView::load(res, "prefabs"_hash, t)) {
//         const int32_t row = t.find("type"_hash, "coin"_hash);
//         const int32_t value = t.get_int(row, "value"_hash, 1);     // 1 when the column is absent
//     }
//
// Zero-copy over the mounted blob (the same lifetime rule as every view), header-only, no
// allocation. Lookups scan the schema linearly: meant for load time, not per-entity per-frame.
// A table baked before the trailer existed still reads (count/stride/record); its columns don't.
#ifndef PHX_RESOURCE_TABLE_H
#define PHX_RESOURCE_TABLE_H

#include "phx/resource/bundle.h"
#include "phx/resource/cache.h"

#include <cstring>

namespace phx {

class TableView {
public:
    // Parse a table blob. False (and an empty view) when it isn't one.
    bool parse(const BlobView& b) {
        *this = TableView{};
        if (!b.data || b.size < 8) return false;
        const uint8_t* p = static_cast<const uint8_t*>(b.data);
        uint32_t count = 0, stride = 0;
        std::memcpy(&count, p, 4);
        std::memcpy(&stride, p + 4, 4);
        const uint64_t end = 8u + uint64_t(count) * stride;
        if ((count && !stride) || end > b.size) return false;
        count_ = count; stride_ = stride; recs_ = p + 8;
        const uint64_t t = (end + 3u) & ~uint64_t(3);
        uint32_t magic = 0, n = 0;
        if (t + 8u <= b.size) {
            std::memcpy(&magic, p + t, 4);
            std::memcpy(&n, p + t + 4, 4);
            if (magic == kTableSchemaMagic && t + 8u + uint64_t(n) * sizeof(TableFieldDef) <= b.size) {
                fields_ = p + t + 8; nfields_ = n;
            }
        }
        return true;
    }
    // The table asset `name` from a mounted bundle.
    static bool load(ResourceCache& res, NameHash name, TableView& out) {
        auto b = res.blob(name);
        return b.ok() && out.parse(b.unwrap());
    }

    uint32_t count()  const { return count_; }
    uint32_t stride() const { return stride_; }
    bool has_schema() const { return fields_ != nullptr; }
    const uint8_t* record(uint32_t row) const { return row < count_ ? recs_ + size_t(row) * stride_ : nullptr; }

    // The column called `name` (false when absent, or the table has no schema).
    bool field(NameHash name, TableFieldDef& out) const {
        for (uint32_t i = 0; i < nfields_; ++i) {
            std::memcpy(&out, fields_ + size_t(i) * sizeof(TableFieldDef), sizeof(TableFieldDef));
            if (out.name == name && out.offset + out.size <= stride_) return true;
        }
        return false;
    }
    bool has(NameHash name) const { TableFieldDef f; return field(name, f); }

    // An integer column (any int type; f32 truncates) of `row`, else `def` (no row, no column,
    // or a string column).
    int32_t get_int(int32_t row, NameHash name, int32_t def = 0) const {
        TableFieldDef f;
        const uint8_t* r = row >= 0 ? record(uint32_t(row)) : nullptr;
        if (!r || !field(name, f)) return def;
        const uint8_t* v = r + f.offset;
        switch (f.type) {
            case kFieldU8:  return int32_t(v[0]);
            case kFieldI8:  return int32_t(int8_t(v[0]));
            case kFieldU16: { uint16_t x; std::memcpy(&x, v, 2); return int32_t(x); }
            case kFieldI16: { int16_t x;  std::memcpy(&x, v, 2); return int32_t(x); }
            case kFieldU32: { uint32_t x; std::memcpy(&x, v, 4); return int32_t(x); }
            case kFieldI32: { int32_t x;  std::memcpy(&x, v, 4); return x; }
            case kFieldF32: { float x;    std::memcpy(&x, v, 4); return int32_t(x); }
            default:        return def;
        }
    }
    // A numeric column of `row` as Q16.16 (an f32 keeps its fraction; ints are whole), for scalar
    // values that must come out identical on both tiers. False when absent or a string column.
    bool get_q16(int32_t row, NameHash name, int32_t& out) const {
        TableFieldDef f;
        const uint8_t* r = row >= 0 ? record(uint32_t(row)) : nullptr;
        if (!r || !field(name, f) || f.type == kFieldStr) return false;
        if (f.type == kFieldF32) {
            float x; std::memcpy(&x, r + f.offset, 4);
            const float q = x * 65536.0f;
            out = q >= 2147483520.0f ? INT32_MAX : q <= -2147483648.0f ? INT32_MIN : int32_t(q);
            return true;
        }
        const int64_t v = int64_t(get_int(row, name)) * 65536;   // clamped to the Q16 range
        out = int32_t(v > INT32_MAX ? INT32_MAX : v < INT32_MIN ? INT32_MIN : v);
        return true;
    }
    // A string column of `row` (NUL-terminated, in the bundle), else nullptr.
    const char* get_str(int32_t row, NameHash name) const {
        TableFieldDef f;
        const uint8_t* r = row >= 0 ? record(uint32_t(row)) : nullptr;
        if (!r || !field(name, f) || f.type != kFieldStr) return nullptr;
        return reinterpret_cast<const char*>(r + f.offset);
    }
    // The FNV-1a hash of a string column ("coin" -> "coin"_hash), 0 when absent or empty.
    NameHash get_hash(int32_t row, NameHash name) const {
        const char* s = get_str(row, name);
        if (!s || !*s) return 0;
        NameHash h = kFnvOffset;
        for (; *s; ++s) h = (h ^ NameHash(uint8_t(*s))) * kFnvPrime;
        return h;
    }
    // The first row whose string column `name` hashes to `value`, else -1.
    int32_t find(NameHash name, NameHash value) const {
        for (uint32_t r = 0; r < count_; ++r)
            if (get_hash(int32_t(r), name) == value) return int32_t(r);
        return -1;
    }

private:
    const uint8_t* recs_    = nullptr;
    const uint8_t* fields_  = nullptr;
    uint32_t       count_   = 0;
    uint32_t       stride_  = 0;
    uint32_t       nfields_ = 0;
};

} // namespace phx
#endif // PHX_RESOURCE_TABLE_H
