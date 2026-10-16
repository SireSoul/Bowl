// The Data workspace: every JSON file in the assets folder, edited in place. Values change where they stand in the
// text, so the file keeps its comments, spacing and order, and the game (which hot-reloads assets) picks up each save.

#include "editor.hpp"

#include "bowl/core/file.hpp"

#include <algorithm>
#include <cmath>
#include <format>

namespace bowl::editor {

namespace fs = std::filesystem;

namespace {

constexpr float top_h = 48, status_h = 28, left_w = 300;
constexpr ImGuiWindowFlags fixed = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoBringToFrontOnFocus;

// InputTextMultiline on a std::string that grows as you type.
int resize_string(ImGuiInputTextCallbackData* data) {
    if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
        auto* s = static_cast<std::string*>(data->UserData);
        s->resize(static_cast<usize>(data->BufTextLen));
        data->Buf = s->data();
    }
    return 0;
}

// A new item for a list: a copy of its last item, or nothing.
json::Value new_item_for(const json::Value& list) { return list.size() ? list[list.size() - 1] : json::Value(); }

} // namespace

void Editor::scan_data() {
    data_files_.clear();
    std::error_code ec;
    if (assets_dir_.empty() || !fs::exists(assets_dir_, ec)) return;
    for (auto it = fs::recursive_directory_iterator(assets_dir_, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec || !it->is_regular_file()) continue;
        std::string ext = it->path().extension().string();
        if (ext != ".json" && ext != ".prefab") continue;
        std::string rel = fs::relative(it->path(), assets_dir_, ec).generic_string();
        data_files_.push_back({rel, it->path()});
    }
    std::sort(data_files_.begin(), data_files_.end(), [](const DataFile& a, const DataFile& b) { return a.path < b.path; });
}

void Editor::data_open(usize index) {
    if (index >= data_files_.size()) return;
    auto text = read_file_text(data_files_[index].file);
    if (!text) {
        say("Can't read " + data_files_[index].path);
        return;
    }
    data_index_ = index;
    data_doc_.load(*text);
    data_saved_ = *text;
    data_history_ = {*text};
    data_cursor_ = 0;
}

bool Editor::data_save() {
    if (!data_index_) return false;
    const std::string& text = data_doc_.text();
    if (!write_file_bytes(data_files_[*data_index_].file, text.data(), text.size())) {
        say("Couldn't save " + data_files_[*data_index_].path);
        return false;
    }
    data_saved_ = text;
    say("Saved " + data_files_[*data_index_].path);
    return true;
}

void Editor::data_undo(int direction) {
    if (data_history_.empty()) return;
    if (direction < 0 && data_cursor_ > 0) --data_cursor_;
    else if (direction > 0 && data_cursor_ + 1 < data_history_.size()) ++data_cursor_;
    else return;
    data_doc_.load(data_history_[data_cursor_]);
}

void Editor::data_shortcuts(Engine& engine) {
    Input& in = engine.input();
    bool cmd = in.down(Key::LeftCtrl) || in.down(Key::RightCtrl) || in.down(Key::LeftSuper) || in.down(Key::RightSuper);
    bool shift = in.down(Key::LeftShift) || in.down(Key::RightShift);
    if (!cmd) return;
    if (in.pressed(Key::S)) data_save();
    // Undo works on the whole file unless a text field is being typed in (which has its own).
    if (!ImGui::GetIO().WantTextInput) {
        if (in.pressed(Key::Z)) data_undo(shift ? 1 : -1);
        else if (in.pressed(Key::Y)) data_undo(1);
    }
}

// One value as a row: a widget for numbers, text and switches; a tree for lists and objects, with + to add and x to
// remove. Edits are queued and applied after drawing.
void Editor::data_tree(const json::Value& value, json::TextDocument::Path& path, std::string_view label) {
    ImGui::PushID(static_cast<int>(path.size()));
    std::string id = std::string(label);
    ImGui::PushID(id.c_str());
    bool removable = !path.empty();
    auto remove_button = [&] {
        if (!removable) return;
        ImGui::SameLine(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - 22);
        if (ui::tool_button("remove", ui::Icon::Cross, false, "Remove", 22)) data_edits_.push_back({DataEdit::Erase, path, {}, {}});
    };
    if (value.is_object() || value.is_array()) {
        bool object = value.is_object();
        std::string title = std::format("{}  {}", label, object ? std::format("{{{}}}", value.size()) : std::format("[{}]", value.size()));
        bool open = ImGui::TreeNodeEx("##node", path.size() < 2 ? ImGuiTreeNodeFlags_DefaultOpen : 0, "%s", title.c_str());
        remove_button();
        if (open) {
            if (object) {
                for (const auto& [key, child] : value.object()) {
                    path.emplace_back(key);
                    data_tree(child, path, key);
                    path.pop_back();
                }
                // Add a key: type its name, then Enter.
                // While a field is being typed in, ImGui keeps its text; this buffer only receives it on Enter, so each
                // object's box shows its own typing.
                char new_key[64] = "";
                ImGui::SetNextItemWidth(160);
                if (ImGui::InputTextWithHint("##newkey", "+ new key", new_key, sizeof new_key, ImGuiInputTextFlags_EnterReturnsTrue) &&
                    new_key[0])
                    data_edits_.push_back({DataEdit::Insert, path, json::Value(), new_key});
                ui::tip("Adds a key to this object (its value starts empty: set it here or in the text)");
            } else {
                for (usize i = 0; i < value.size(); ++i) {
                    path.emplace_back(i);
                    data_tree(value[i], path, std::format("[{}]", i));
                    path.pop_back();
                }
                if (ui::tool_button("add", ui::Icon::Plus, false, "Add an item (a copy of the last one)", 22))
                    data_edits_.push_back({DataEdit::Insert, path, new_item_for(value), {}});
            }
            ImGui::TreePop();
        }
    } else {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(id.c_str());
        ImGui::SameLine(160);
        ImGui::SetNextItemWidth(std::max(80.0f, ImGui::GetContentRegionAvail().x - (removable ? 30.0f : 0.0f)));
        if (value.is_bool()) {
            bool b = value.as_bool();
            if (ImGui::Checkbox("##v", &b)) data_edits_.push_back({DataEdit::Set, path, b, {}});
        } else if (value.is_number()) {
            f64 n = value.as_number();
            bool whole = std::trunc(n) == n;
            if (ImGui::DragScalar("##v", ImGuiDataType_Double, &n, whole ? 1.0f : 0.01f, nullptr, nullptr, whole ? "%.0f" : "%.4g"))
                data_edits_.push_back({DataEdit::Set, path, n, {}});
        } else if (value.is_string()) {
            std::string s(value.as_string());
            if (ImGui::InputText("##v", &s[0], s.size() + 1, ImGuiInputTextFlags_CallbackResize, resize_string, &s))
                data_edits_.push_back({DataEdit::Set, path, s, {}});
        } else {
            // null: offer a type.
            if (ImGui::Button("set...")) ImGui::OpenPopup("type");
            if (ImGui::BeginPopup("type")) {
                json::Value obj, arr;
                obj.make_object();
                arr.make_array();
                if (ImGui::Selectable("number")) data_edits_.push_back({DataEdit::Set, path, 0, {}});
                if (ImGui::Selectable("text")) data_edits_.push_back({DataEdit::Set, path, "", {}});
                if (ImGui::Selectable("on/off")) data_edits_.push_back({DataEdit::Set, path, false, {}});
                if (ImGui::Selectable("list")) data_edits_.push_back({DataEdit::Set, path, arr, {}});
                if (ImGui::Selectable("object")) data_edits_.push_back({DataEdit::Set, path, obj, {}});
                ImGui::EndPopup();
            }
        }
        remove_button();
    }
    ImGui::PopID();
    ImGui::PopID();
}

void Editor::data_workspace(Engine& engine) {
    const ImGuiIO& io = ImGui::GetIO();
    float h = io.DisplaySize.y - top_h - status_h;

    // Left: the files.
    ImGui::SetNextWindowPos({0, top_h});
    ImGui::SetNextWindowSize({left_w, h});
    ImGui::Begin("##datafiles", nullptr, fixed);
    ImGui::SetNextItemWidth(-30);
    ImGui::InputTextWithHint("##filter", "filter", data_filter_, sizeof data_filter_);
    ImGui::SameLine();
    if (ui::tool_button("rescan", ui::Icon::Reload, false, "Look for new files|F5", 24)) scan_data();
    ui::rule();
    if (data_files_.empty()) ui::hint("No .json files in the assets folder.");
    std::string_view filter = data_filter_;
    for (usize i = 0; i < data_files_.size(); ++i) {
        const DataFile& f = data_files_[i];
        if (!filter.empty() && f.path.find(filter) == std::string::npos) continue;
        bool open = data_index_ == i;
        std::string label = open && data_doc_.text() != data_saved_ ? f.path + " *" : f.path;
        if (ImGui::Selectable(label.c_str(), open)) {
            if (data_index_ && data_doc_.text() != data_saved_) say("Unsaved changes in the file you left (Ctrl+Z there still works)");
            data_open(i);
        }
    }
    ImGui::End();

    // Right of it: the open file, as a tree and as text.
    float main_w = io.DisplaySize.x - left_w, tree_w = std::floor(main_w * 0.55f);
    ImGui::SetNextWindowPos({left_w, top_h});
    ImGui::SetNextWindowSize({tree_w, h});
    ImGui::Begin("##datatree", nullptr, fixed);
    if (!data_index_) {
        ui::hint("Pick a file on the left. Changes keep the file's comments and layout; Ctrl+S saves, and a running game "
                 "picks the change up.");
    } else {
        const DataFile& f = data_files_[*data_index_];
        ImGui::TextUnformatted(f.path.c_str());
        ImGui::SameLine();
        bool unsaved = data_doc_.text() != data_saved_;
        ImGui::BeginDisabled(!unsaved);
        if (ImGui::SmallButton("Save")) data_save();
        ImGui::SameLine();
        if (ImGui::SmallButton("Revert")) data_open(*data_index_);
        ImGui::EndDisabled();
        ui::rule();
        if (!data_doc_.valid()) {
            const auto& e = *data_doc_.error();
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(ui::color::danger), "Line %u, column %u: %s", e.line, e.column,
                               e.message.c_str());
            ui::hint("Fix the text on the right to edit it here again.");
        } else {
            json::TextDocument::Path path;
            data_tree(data_doc_.value(), path, "(file)");
        }
    }
    ImGui::End();

    ImGui::SetNextWindowPos({left_w + tree_w, top_h});
    ImGui::SetNextWindowSize({main_w - tree_w, h});
    ImGui::Begin("##datatext", nullptr, fixed);
    if (data_index_) {
        ui::hint("The file as text: edit here too.");
        std::string text = data_doc_.text();
        if (ImGui::InputTextMultiline("##text", &text[0], text.size() + 1, ImGui::GetContentRegionAvail(),
                                      ImGuiInputTextFlags_CallbackResize | ImGuiInputTextFlags_AllowTabInput,
                                      resize_string, &text))
            data_doc_.load(text);
    }
    ImGui::End();

    // Apply what the tree asked for, then remember the result as an undo step once nothing is being dragged or typed.
    for (const DataEdit& e : data_edits_) {
        bool ok = e.kind == DataEdit::Set      ? data_doc_.set(e.path, e.value)
                  : e.kind == DataEdit::Insert ? data_doc_.insert(e.path, e.value, e.key)
                                               : data_doc_.erase(e.path);
        if (!ok) say("That change didn't fit the file");
    }
    data_edits_.clear();
    if (data_index_ && !ImGui::IsAnyItemActive() && data_doc_.text() != data_history_[data_cursor_]) {
        data_history_.resize(data_cursor_ + 1);
        data_history_.push_back(data_doc_.text());
        data_cursor_ = data_history_.size() - 1;
    }
    data_shortcuts(engine);
}

} // namespace bowl::editor
