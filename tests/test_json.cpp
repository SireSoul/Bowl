#include "bowl/core/json.hpp"
#include "bowl/core/json_text.hpp"

#include <doctest/doctest.h>

using namespace bowl;

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

TEST_CASE("json text: edits keep comments, spacing and order") {
    std::string original = R"({
  // Enemies of the first area.
  "slime": {
    "hp": 30, // tune later
    "speed": 1.5,
    "drops": ["gel"],
  },
  /* bats come at night */
  "bat": { "hp": 8, "tags": [] }
}
)";
    json::TextDocument doc;
    REQUIRE(doc.load(original));
    REQUIRE(doc.set({"slime", "hp"}, 40));
    CHECK(doc.text().find("\"hp\": 40, // tune later") != std::string::npos);
    CHECK(doc.text().find("// Enemies of the first area.") != std::string::npos);
    CHECK(doc.text().find("/* bats come at night */") != std::string::npos);
    CHECK(doc.value()["slime"]["hp"].as_int() == 40);

    REQUIRE(doc.insert({"slime", "drops"}, "slime ball"));
    CHECK(doc.text().find(R"(["gel", "slime ball"])") != std::string::npos);
    REQUIRE(doc.insert({"bat", "tags"}, "flying"));
    CHECK(doc.text().find(R"("tags": ["flying"])") != std::string::npos);
    REQUIRE(doc.insert({"slime"}, 2, "armor"));
    CHECK(doc.value()["slime"]["armor"].as_int() == 2);
    CHECK(doc.text().find("\n    \"armor\": 2") != std::string::npos); // lined up with its neighbours
    REQUIRE(doc.insert({"slime"}, 3, "armor")); // an existing key is replaced
    CHECK(doc.value()["slime"]["armor"].as_int() == 3);

    REQUIRE(doc.erase({"slime", "speed"}));
    CHECK_FALSE(doc.value()["slime"].contains("speed"));
    CHECK(doc.text().find("\"hp\": 40, // tune later") != std::string::npos);
    REQUIRE(doc.erase({"slime", "drops", usize{0}}));
    CHECK(doc.value()["slime"]["drops"].size() == 1);
    REQUIRE(doc.erase({"bat", "tags", usize{0}}));
    CHECK(doc.text().find(R"("tags": [])") != std::string::npos);

    // Replacing a value with a container indents it to fit.
    json::Value boss;
    boss.set("hp", 500);
    REQUIRE(doc.set({"bat"}, boss));
    CHECK(doc.value()["bat"]["hp"].as_int() == 500);
    CHECK(doc.valid());

    CHECK_FALSE(doc.set({"nope"}, 1));
    CHECK_FALSE(doc.erase({"slime", "drops", usize{9}}));
    CHECK_FALSE(doc.load("{ broken"));
    CHECK(doc.error());
    CHECK(doc.text() == "{ broken"); // kept, so it can be fixed
}
