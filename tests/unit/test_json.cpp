// tests/test_json.cpp — the asset pipeline's JSON parser (used by the Tiled importer). Covers
// nested objects/arrays, numbers/strings/bools/null, string escapes, and malformed-input
// rejection. Host-only tool header pulled into the unit binary, like test_lz/test_png.
#include "phx_test.h"
#include "json.h"

using namespace phxtool;

PHX_TEST(json_parses_nested_structure) {
    const std::string text =
        "{ \"name\": \"level1\", \"w\": 3, \"h\": 2, \"ok\": true, \"nope\": false, \"nil\": null,"
        "  \"data\": [1, 2, 3, 40], \"meta\": { \"x\": -5, \"f\": 1.5 } }";
    JsonValue root;
    CHECK(JsonParser::parse(text, root));
    CHECK(root.is_obj());
    CHECK(root.str_at("name") == "level1");
    CHECK_EQ(root.int_at("w"), 3);
    CHECK_EQ(root.int_at("h"), 2);

    const JsonValue* ok = root.find("ok");
    CHECK(ok && ok->type == JsonValue::Bool && ok->boolean == true);
    const JsonValue* nil = root.find("nil");
    CHECK(nil && nil->type == JsonValue::Null);

    const JsonValue* data = root.find("data");
    CHECK(data && data->is_arr());
    CHECK(data && data->arr.size() == 4);
    CHECK(data && data->arr.size() == 4 && data->arr[3].as_int() == 40);

    const JsonValue* meta = root.find("meta");
    CHECK(meta && meta->is_obj());
    CHECK(meta && meta->int_at("x") == -5);
    CHECK(meta && meta->find("f") && meta->find("f")->as_num() > 1.4 && meta->find("f")->as_num() < 1.6);
}

PHX_TEST(json_string_escapes) {
    JsonValue v;
    CHECK(JsonParser::parse("\"a\\tb\\n\\u0041\"", v));     // tab, newline, A == 'A'
    CHECK(v.type == JsonValue::Str);
    CHECK(v.str == "a\tb\nA");
}

PHX_TEST(json_empty_containers) {
    JsonValue a, o;
    CHECK(JsonParser::parse("[]", a));
    CHECK(a.is_arr() && a.arr.empty());
    CHECK(JsonParser::parse("{}", o));
    CHECK(o.is_obj() && o.members.empty());
}

PHX_TEST(json_rejects_malformed) {
    JsonValue v;
    CHECK(!JsonParser::parse("{ \"a\": }", v));            // missing value
    CHECK(!JsonParser::parse("{ \"a\" 1 }", v));           // missing colon
    CHECK(!JsonParser::parse("[1, 2", v));                 // unterminated array
    CHECK(!JsonParser::parse("\"unterminated", v));        // unterminated string
    CHECK(!JsonParser::parse("{ } trailing", v));          // trailing garbage
    CHECK(!JsonParser::parse("", v));                      // empty
}
