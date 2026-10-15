#include "ph/core/json.hpp"

#include <doctest/doctest.h>

using namespace ph;

TEST_CASE("json: parse values and navigate forgivingly") {
    auto r = json::parse(R"({
        "name": "hero", "frames": [1, 2.5, -3e2], "loop": true, "parent": null,
        "nested": {"size": {"w": 32, "h": 48}}
    })");
    REQUIRE(r);
    const json::Value& v = r.value;
    CHECK(v["name"].as_string() == "hero");
    CHECK(v["frames"].size() == 3);
    CHECK(v["frames"][1].as_number() == 2.5);
    CHECK(v["frames"][2].as_int() == -300);
    CHECK(v["loop"].as_bool());
    CHECK(v["parent"].is_null());
    CHECK(v["nested"]["size"]["h"].as_int() == 48);
    // Missing keys and wrong types fall back instead of throwing.
    CHECK(v["missing"]["deeper"].as_int(7) == 7);
    CHECK(v["name"].as_number(-1) == -1);
    CHECK(v["frames"][99].is_null());
}

TEST_CASE("json: strings with escapes and unicode") {
    auto r = json::parse(R"(["a\"b\\c\n", "é€", "😀", "raw é"])");
    REQUIRE(r);
    CHECK(r.value[0].as_string() == "a\"b\\c\n");
    CHECK(r.value[1].as_string() == "\xC3\xA9\xE2\x82\xAC");
    CHECK(r.value[2].as_string() == "\xF0\x9F\x98\x80");
    CHECK(r.value[3].as_string() == "raw \xC3\xA9");
}

TEST_CASE("json: comments and trailing commas are accepted") {
    auto r = json::parse("// config\n{ /* speed */ \"speed\": 4, \"tags\": [\"a\", \"b\",], }");
    REQUIRE(r);
    CHECK(r.value["speed"].as_int() == 4);
    CHECK(r.value["tags"].size() == 2);
}

TEST_CASE("json: errors report line and column") {
    auto r = json::parse("{\n  \"a\": 1,\n  \"b\" 2\n}");
    REQUIRE_FALSE(r);
    CHECK(r.error->line == 3);
    CHECK(r.error->message.find("':'") != std::string::npos);
    CHECK_FALSE(json::parse("[1, 2"));
    CHECK_FALSE(json::parse("\"unterminated"));
    CHECK_FALSE(json::parse("{} extra"));
    CHECK_FALSE(json::parse("[01x]"));
}

TEST_CASE("json: build, stringify and round-trip") {
    json::Value doc;
    doc.set("title", "Ad astra");
    doc.set("version", 2);
    doc.set("ratio", 0.5);
    json::Value list;
    list.push(1);
    list.push("two");
    list.push(nullptr);
    doc.set("list", list);
    doc.set("version", 3); // replaces, keeps position

    std::string compact = json::stringify(doc, -1);
    CHECK(compact == R"({"title":"Ad astra","version":3,"ratio":0.5,"list":[1,"two",null]})");
    auto back = json::parse(json::stringify(doc));
    REQUIRE(back);
    CHECK(back.value == doc);
}
