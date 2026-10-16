#pragma once

#include "bowl/app/engine.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace bowl::editor {

// Components in the file whose types the editor doesn't know (the game's code library wasn't loaded). They're kept
// as JSON, shown in the inspector as text, and written back when saving, so opening and saving never loses them.
struct RawComponents {
    json::Object components;
};

using Selection = std::vector<ecs::Entity>;

// The level being edited and the file it belongs to: open, save, undo, redo and autosave. The engine's world holds
// the entities; this keeps their order (the order in the file) and the edit history.
class Document {
public:
    explicit Document(Engine& engine) : engine_(engine) {}

    void clear();
    bool open(const std::filesystem::path& file);
    bool save();
    bool save_as(const std::filesystem::path& file);
    const std::filesystem::path& file() const { return file_; }
    std::string title() const; // the file name, or "untitled"
    bool modified() const;

    // Entities in file order. New entities go through add() so they're saved in a predictable place.
    std::vector<ecs::Entity> entities() const;
    void add(ecs::Entity e) { order_.push_back(e); }

    // Edit history. track() records the current state if it differs from the last one; call it whenever the user
    // isn't in the middle of an edit (no drag in progress, no field being typed in). undo and redo return the
    // selection to restore.
    void track(const Selection& selection);
    Selection undo();
    Selection redo();
    bool can_undo() const { return cursor_ > 0; }
    bool can_redo() const { return cursor_ + 1 < history_.size(); }

    // Unsaved work, kept next to the file (or in the user folder for an untitled level) when the editor closes.
    std::filesystem::path autosave_path() const;
    void write_autosave() const;
    // An autosave newer than the file, if there is one: offered back when the file is opened.
    std::optional<std::string> pending_autosave() const;
    void recover(const std::string& text); // loads the autosaved state as an unsaved edit
    void discard_autosave() const;

    // The document as text, and back.
    std::string snapshot() const;
    void restore(std::string_view text);

private:
    struct Entry {
        std::string text;
        std::vector<i64> selected; // file indices
    };
    json::Value to_json() const;
    void from_json(const json::Value& doc);
    void destroy_all();
    void reset_history();
    std::vector<i64> indices_of(const Selection& selection) const;
    Selection entities_at(const std::vector<i64>& indices) const;

    Engine& engine_;
    std::filesystem::path file_;
    bool binary_ = false; // the file was binary, so it's saved that way again
    std::vector<ecs::Entity> order_;
    std::vector<Entry> history_;
    usize cursor_ = 0;
    std::string saved_;
};

} // namespace bowl::editor
