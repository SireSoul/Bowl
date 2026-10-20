#pragma once

#include <imgui.h>

#include <string>
#include <vector>

// A code editor for GLSL in an ImGui window: line numbers, syntax colors, problems marked on their lines (the message
// shows over the line number), auto-indent, and tabs typed as spaces. It's ImGui's own text input underneath, so typing,
// selecting, copy and paste and undo work as in every other field, with the colors drawn over it.
namespace bowl::editor {

class CodeEditor {
public:
    struct Mark {
        int line = 0; // from 1
        std::string message;
        bool warning = false;
    };

    void set_text(std::string text);
    const std::string& text() const { return text_; }
    void set_marks(std::vector<Mark> marks) { marks_ = std::move(marks); }
    const std::vector<Mark>& marks() const { return marks_; }
    // Puts the cursor at the start of a line (from 1) and scrolls to it.
    void go_to(int line);
    int cursor_line() const { return cursor_line_ + 1; }
    int cursor_column() const { return cursor_column_ + 1; }
    // Draws it filling `size`; `font` is a monospaced one (null: the current font). True when the text changed.
    bool draw(const char* id, ImVec2 size, ImFont* font);

private:
    static int callback(ImGuiInputTextCallbackData* data);

    std::string text_;
    std::vector<Mark> marks_;
    int goto_line_ = 0;      // waiting to move the cursor there
    bool focus_ = false;
    int cursor_line_ = 0, cursor_column_ = 0;
    int last_cursor_ = -1;
    bool follow_ = false;    // scroll to the cursor after this frame
    int known_length_ = -1;  // the text's length last frame, to see a line break just typed
};

} // namespace bowl::editor
