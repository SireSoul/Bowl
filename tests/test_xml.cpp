#include "bowl/core/xml.hpp"

#include <doctest/doctest.h>

using namespace bowl;

TEST_CASE("xml: elements, attributes, text and entities") {
    auto doc = xml::parse(R"(<?xml version="1.0"?>
<!-- a comment -->
<!DOCTYPE map>
<map name="a &amp; b" quote='it&apos;s'>
  <layer id="1"/>
  <note>x &lt; y &#65;&#x42; <![CDATA[<raw> & stuff]]></note>
  <layer id="2"></layer>
</map>
)");
    REQUIRE_FALSE(doc.error.has_value());
    CHECK(doc.root.name == "map");
    CHECK(doc.root.attribute("name") == "a & b");
    CHECK(doc.root.attribute("quote") == "it's");
    CHECK(doc.root.attribute("missing", "fallback") == "fallback");
    CHECK(doc.root.has("name"));
    REQUIRE(doc.root.children.size() == 3);
    CHECK(doc.root.child("layer")->attribute("id") == "1");
    CHECK(doc.root.child("note")->text == "x < y AB <raw> & stuff");
    CHECK(doc.root.child("nothing") == nullptr);
}

TEST_CASE("xml: errors say where") {
    auto bad = xml::parse("<a>\n  <b></c>\n</a>");
    REQUIRE_FALSE(bad);
    CHECK(bad.error->line == 2);
    CHECK(bad.error->message.find("</c>") != std::string::npos);
    CHECK_FALSE(xml::parse("<a x=1/>"));
    CHECK_FALSE(xml::parse("<a>&bogus;</a>"));
    CHECK_FALSE(xml::parse("<a></a><b/>"));
    CHECK_FALSE(xml::parse(""));
}
