#pragma once

#include "bowl/core/types.hpp"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The API reference (docs/reference/api) from the engine's public headers: their comments and declarations, without
// function bodies or private members. A comment directly above a declaration documents it; one followed by a blank
// line is a note of its own; the first one in a file introduces the header. bowl-apidoc is the command-line tool around
// it, and the bowl_api_docs target runs it.
namespace bowl::apidoc {

struct Item {
    enum class Kind : u8 { Note, Decl, Class };
    Kind kind = Kind::Decl;
    std::vector<std::string> comment; // its lines, without the "// "
    std::string code;                 // Decl: the declaration; Class: its head ("class Host : NonCopyable")
    std::string trailing;             // a comment at the end of the declaration's line
    usize trailing_pad = 1;           // the spaces before it, to keep comments lined up
    std::string name;                 // Class
    bool gap_before = false;          // a blank line separates it from what came before
    std::vector<Item> members;        // Class: the public ones
};

struct Header {
    std::string path; // "bowl/net/host.hpp"
    std::vector<std::string> namespaces;
    std::vector<std::string> intro;
    std::vector<Item> items;
};

Header parse(std::string_view path, std::string_view source);

// A page for one module's headers.
std::string render_module(std::string_view module, const std::vector<Header>& headers);
// The index of every module and header.
std::string render_index(const std::vector<std::pair<std::string, std::vector<Header>>>& modules);
// GitHub's anchor for a heading.
std::string anchor(std::string_view heading);

} // namespace bowl::apidoc
