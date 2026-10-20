#include "code_editor.hpp"

#include "ui.hpp"

#include "bowl/core/types.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <format>
#include <string_view>
#include <unordered_set>

namespace bowl::editor {

namespace {

namespace col {
constexpr ImU32 plain = IM_COL32(214, 218, 226, 255);
constexpr ImU32 keyword = IM_COL32(242, 160, 92, 255);
constexpr ImU32 type = IM_COL32(110, 205, 196, 255);
constexpr ImU32 builtin = IM_COL32(130, 175, 255, 255);
constexpr ImU32 number = IM_COL32(190, 220, 130, 255);
constexpr ImU32 comment = IM_COL32(120, 130, 142, 255);
constexpr ImU32 preprocessor = IM_COL32(200, 140, 230, 255);
constexpr ImU32 reserved = IM_COL32(242, 196, 92, 255); // bowl_time and friends
} // namespace col

const std::unordered_set<std::string_view>& keywords() {
    static const std::unordered_set<std::string_view> k = {
        "if", "else", "for", "while", "do", "return", "break", "continue", "discard", "switch", "case", "default",
        "const", "in", "out", "inout", "uniform", "buffer", "layout", "readonly", "writeonly", "flat", "smooth",
        "struct", "true", "false", "precision", "highp", "mediump", "lowp", "std140", "std430", "set", "binding",
        "location", "push_constant", "local_size_x", "local_size_y", "local_size_z", "shared"};
    return k;
}

const std::unordered_set<std::string_view>& types() {
    static const std::unordered_set<std::string_view> t = {
        "void", "bool", "int", "uint", "float", "double", "vec2", "vec3", "vec4", "ivec2", "ivec3", "ivec4", "uvec2",
        "uvec3", "uvec4", "bvec2", "bvec3", "bvec4", "mat2", "mat3", "mat4", "sampler2D", "texture2D", "sampler",
        "image2D"};
    return t;
}

const std::unordered_set<std::string_view>& builtins() {
    static const std::unordered_set<std::string_view> b = {
        "texture", "textureSize", "texelFetch", "textureLod", "mix", "step", "smoothstep", "clamp", "min", "max",
        "abs", "sign", "floor", "ceil", "fract", "mod", "pow", "exp", "exp2", "log", "log2", "sqrt", "inversesqrt",
        "sin", "cos", "tan", "asin", "acos", "atan", "radians", "degrees", "length", "distance", "dot", "cross",
        "normalize", "reflect", "refract", "dFdx", "dFdy", "fwidth", "round", "trunc", "any", "all", "not",
        "gl_FragCoord", "gl_VertexIndex", "gl_Position", "gl_GlobalInvocationID", "gl_LocalInvocationID",
        "premultiply", "rotate", "v_uv", "v_color", "out_color", "u_texture"};
    return b;
}

bool ident_start(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_'; }
bool ident(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

struct Run {
    usize from, to;
    ImU32 color;
};

// One line's colors. `in_comment` carries a /* */ comment from line to line.
void tokenize(std::string_view line, bool& in_comment, std::vector<Run>& runs) {
    runs.clear();
    usize i = 0, n = line.size();
    if (!in_comment) {
        usize first = line.find_first_not_of(' ');
        if (first != std::string_view::npos && line[first] == '#') {
            usize slash = line.find("//", first);
            runs.push_back({0, slash == std::string_view::npos ? n : slash, col::preprocessor});
            if (slash != std::string_view::npos) runs.push_back({slash, n, col::comment});
            return;
        }
    }
    while (i < n) {
        if (in_comment) {
            usize end = line.find("*/", i);
            usize to = end == std::string_view::npos ? n : end + 2;
            runs.push_back({i, to, col::comment});
            if (end != std::string_view::npos) in_comment = false;
            i = to;
            continue;
        }
        char c = line[i];
        if (c == '/' && i + 1 < n && line[i + 1] == '/') {
            runs.push_back({i, n, col::comment});
            return;
        }
        if (c == '/' && i + 1 < n && line[i + 1] == '*') {
            in_comment = true;
            runs.push_back({i, i + 2, col::comment});
            i += 2;
            continue;
        }
        if (std::isdigit(static_cast<unsigned char>(c)) || (c == '.' && i + 1 < n && std::isdigit(static_cast<unsigned char>(line[i + 1])))) {
            usize j = i;
            while (j < n && (std::isalnum(static_cast<unsigned char>(line[j])) || line[j] == '.')) ++j;
            runs.push_back({i, j, col::number});
            i = j;
            continue;
        }
        if (ident_start(c)) {
            usize j = i;
            while (j < n && ident(line[j])) ++j;
            std::string_view word = line.substr(i, j - i);
            ImU32 color = col::plain;
            if (keywords().contains(word)) color = col::keyword;
            else if (types().contains(word)) color = col::type;
            else if (builtins().contains(word)) color = col::builtin;
            else if (word.starts_with("bowl_")) color = col::reserved;
            runs.push_back({i, j, color});
            i = j;
            continue;
        }
        usize j = i + 1;
        while (j < n && !ident_start(line[j]) && !std::isdigit(static_cast<unsigned char>(line[j])) && line[j] != '/') ++j;
        runs.push_back({i, j, col::plain});
        i = j;
    }
}

} // namespace

void CodeEditor::set_text(std::string text) {
    // Tabs become four spaces: the colors are drawn over the text, and a tab's width is the font's guess.
    for (usize at; (at = text.find('\t')) != std::string::npos;) text.replace(at, 1, "    ");
    std::erase(text, '\r');
    text_ = std::move(text);
    known_length_ = -1;
}

void CodeEditor::go_to(int line) {
    goto_line_ = std::max(1, line);
    focus_ = true;
}

int CodeEditor::callback(ImGuiInputTextCallbackData* data) {
    auto* self = static_cast<CodeEditor*>(data->UserData);
    if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
        self->text_.resize(static_cast<usize>(data->BufTextLen));
        data->Buf = self->text_.data();
        return 0;
    }
    if (data->EventFlag != ImGuiInputTextFlags_CallbackAlways) return 0;
    // Tabs (typed or pasted) as spaces.
    for (int i = 0; i < data->BufTextLen; ++i)
        if (data->Buf[i] == '\t') {
            data->DeleteChars(i, 1);
            data->InsertChars(i, "    ");
            i += 3;
        }
    // A line break just typed: indent the new line like the one above, a step more after an opening brace.
    if (self->known_length_ >= 0 && data->BufTextLen == self->known_length_ + 1 && data->CursorPos > 0 &&
        data->Buf[data->CursorPos - 1] == '\n' && !data->HasSelection()) {
        int end = data->CursorPos - 1, start = end;
        while (start > 0 && data->Buf[start - 1] != '\n') --start;
        int spaces = 0;
        while (start + spaces < end && data->Buf[start + spaces] == ' ') ++spaces;
        int last = end - 1;
        while (last >= start && data->Buf[last] == ' ') --last;
        if (last >= start && data->Buf[last] == '{') spaces += 4;
        if (spaces > 0) data->InsertChars(data->CursorPos, std::string(static_cast<usize>(spaces), ' ').c_str());
    }
    self->known_length_ = data->BufTextLen;
    if (self->goto_line_ > 0) {
        int line = 1, pos = 0;
        while (pos < data->BufTextLen && line < self->goto_line_) {
            if (data->Buf[pos] == '\n') ++line;
            ++pos;
        }
        data->CursorPos = data->SelectionStart = data->SelectionEnd = pos;
        self->goto_line_ = 0;
        self->follow_ = true;
    }
    if (data->CursorPos != self->last_cursor_) {
        self->last_cursor_ = data->CursorPos;
        int line = 0, col = 0;
        for (int i = 0; i < data->CursorPos && i < data->BufTextLen; ++i) {
            if (data->Buf[i] == '\n') ++line, col = 0;
            else ++col;
        }
        self->cursor_line_ = line;
        self->cursor_column_ = col;
        self->follow_ = true;
    }
    return 0;
}

bool CodeEditor::draw(const char* id, ImVec2 size, ImFont* font) {
    ImGui::PushID(id);
    if (font) ImGui::PushFont(font, 0);
    // The lines, and the widest.
    std::vector<std::pair<usize, usize>> lines;
    {
        usize start = 0;
        for (usize i = 0; i <= text_.size(); ++i)
            if (i == text_.size() || text_[i] == '\n') {
                lines.emplace_back(start, i);
                start = i + 1;
            }
    }
    const ImGuiStyle& style = ImGui::GetStyle();
    f32 line_h = ImGui::GetTextLineHeight();
    f32 char_w = ImGui::CalcTextSize("M").x;
    f32 widest = 0;
    for (auto [a, b] : lines) widest = std::max(widest, static_cast<f32>(b - a) * char_w);
    int digits = std::max(3, static_cast<int>(std::to_string(lines.size()).size()));
    f32 gutter = static_cast<f32>(digits) * char_w + 22;

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ui::color::field);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
    ImGui::BeginChild("##code", size, ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoMove);
    ImGui::PopStyleVar();
    ImVec2 origin = ImGui::GetCursorScreenPos();
    ImVec2 visible = ImGui::GetContentRegionAvail();
    f32 text_w = std::max(widest + char_w * 8 + style.FramePadding.x * 2, visible.x - gutter);
    f32 text_h = std::max(static_cast<f32>(lines.size() + 2) * line_h + style.FramePadding.y * 2, visible.y);

    // The text field, see-through: the colored text is drawn over it.
    ImGui::SetCursorScreenPos({origin.x + gutter, origin.y});
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_InputTextCursor, ui::color::accent);
    ImGui::PushStyleColor(ImGuiCol_TextSelectedBg, IM_COL32(242, 196, 92, 70));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0);
    if (focus_) {
        ImGui::SetKeyboardFocusHere();
        focus_ = false;
    }
    if (known_length_ < 0) known_length_ = static_cast<int>(text_.size());
    ImGuiInputTextFlags flags = ImGuiInputTextFlags_AllowTabInput | ImGuiInputTextFlags_CallbackResize |
                                ImGuiInputTextFlags_CallbackAlways | ImGuiInputTextFlags_NoHorizontalScroll;
    bool changed = ImGui::InputTextMultiline("##text", text_.data(), text_.capacity() + 1, {text_w, text_h}, flags,
                                             &CodeEditor::callback, this);
    bool active = ImGui::IsItemActive();
    if (!active) known_length_ = static_cast<int>(text_.size());
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(6);
    ImVec2 text_min = ImGui::GetItemRectMin();
    text_min.x += style.FramePadding.x;
    text_min.y += style.FramePadding.y;

    // Keep the cursor in view.
    f32 scroll_y = ImGui::GetScrollY(), scroll_x = ImGui::GetScrollX(), view_h = ImGui::GetWindowHeight();
    f32 view_w = ImGui::GetWindowWidth();
    if (follow_) {
        follow_ = false;
        f32 y = style.FramePadding.y + static_cast<f32>(cursor_line_) * line_h;
        if (y < scroll_y) ImGui::SetScrollY(y);
        else if (y + line_h * 2 > scroll_y + view_h) ImGui::SetScrollY(y + line_h * 2 - view_h);
        f32 x = gutter + style.FramePadding.x + static_cast<f32>(cursor_column_) * char_w;
        if (x < scroll_x + gutter) ImGui::SetScrollX(std::max(0.0f, x - gutter - char_w * 4));
        else if (x + char_w * 2 > scroll_x + view_w) ImGui::SetScrollX(x + char_w * 6 - view_w);
    }

    // Colors, marks, line numbers: only the lines in view.
    ImDrawList* dl = ImGui::GetWindowDrawList();
    usize first = static_cast<usize>(std::max(0.0f, (scroll_y - style.FramePadding.y) / line_h));
    usize last = std::min(lines.size(), first + static_cast<usize>(view_h / line_h) + 2);
    bool in_comment = false;
    std::vector<Run> runs;
    for (usize i = 0; i < first && i < lines.size(); ++i) // carry block comments into view
        tokenize(std::string_view(text_).substr(lines[i].first, lines[i].second - lines[i].first), in_comment, runs);
    f32 gutter_x = origin.x + scroll_x; // the gutter stays put when scrolling sideways
    for (usize i = first; i < last; ++i) {
        f32 y = text_min.y + static_cast<f32>(i) * line_h;
        const Mark* mark = nullptr;
        for (const Mark& m : marks_)
            if (m.line == static_cast<int>(i) + 1 && (!mark || mark->warning)) mark = &m;
        if (mark)
            dl->AddRectFilled({origin.x + gutter, y}, {origin.x + gutter + text_w, y + line_h},
                              mark->warning ? IM_COL32(242, 196, 92, 34) : IM_COL32(224, 108, 100, 48));
        else if (active && static_cast<int>(i) == cursor_line_)
            dl->AddRectFilled({origin.x + gutter, y}, {origin.x + gutter + text_w, y + line_h}, IM_COL32(255, 255, 255, 10));
        std::string_view line = std::string_view(text_).substr(lines[i].first, lines[i].second - lines[i].first);
        tokenize(line, in_comment, runs);
        f32 x = text_min.x;
        for (const Run& r : runs) {
            const char* a = line.data() + r.from;
            const char* b = line.data() + r.to;
            dl->AddText({x, y}, r.color, a, b);
            x += ImGui::CalcTextSize(a, b).x;
        }
    }
    // The gutter over the text that scrolls under it.
    dl->AddRectFilled({gutter_x, origin.y + scroll_y}, {gutter_x + gutter - 6, origin.y + scroll_y + view_h}, ui::color::bar);
    for (usize i = first; i < last; ++i) {
        f32 y = text_min.y + static_cast<f32>(i) * line_h;
        const Mark* mark = nullptr;
        for (const Mark& m : marks_)
            if (m.line == static_cast<int>(i) + 1 && (!mark || mark->warning)) mark = &m;
        std::string number = std::to_string(i + 1);
        bool here = static_cast<int>(i) == cursor_line_;
        ImU32 c = mark ? (mark->warning ? ui::color::accent : ui::color::danger) : here ? ui::color::text : ui::color::muted;
        dl->AddText({gutter_x + gutter - 14 - ImGui::CalcTextSize(number.c_str()).x, y}, c, number.c_str());
        if (mark) {
            dl->AddRectFilled({gutter_x, y}, {gutter_x + 3, y + line_h}, c);
            ImVec2 m = ImGui::GetMousePos();
            if (ImGui::IsWindowHovered() && m.x >= gutter_x && m.x < gutter_x + gutter && m.y >= y && m.y < y + line_h) {
                ImGui::BeginTooltip();
                for (const Mark& k : marks_)
                    if (k.line == mark->line)
                        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(k.warning ? ui::color::accent : ui::color::danger), "%s",
                                           k.message.c_str());
                ImGui::EndTooltip();
            }
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
    if (font) ImGui::PopFont();
    ImGui::PopID();
    return changed;
}

} // namespace bowl::editor
