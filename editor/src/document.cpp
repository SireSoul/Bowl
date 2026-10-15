#include "document.hpp"

#include "ph/core/file.hpp"

#include <algorithm>

namespace ph::editor {

namespace fs = std::filesystem;

std::vector<ecs::Entity> Document::entities() const {
    std::vector<ecs::Entity> alive;
    for (ecs::Entity e : order_)
        if (engine_.world().alive(e)) alive.push_back(e);
    return alive;
}

std::vector<i64> Document::indices_of(const Selection& selection) const {
    std::vector<ecs::Entity> list = entities();
    std::vector<i64> out;
    for (ecs::Entity e : selection) {
        auto it = std::find(list.begin(), list.end(), e);
        if (it != list.end()) out.push_back(static_cast<i64>(it - list.begin()));
    }
    return out;
}

Selection Document::entities_at(const std::vector<i64>& indices) const {
    std::vector<ecs::Entity> list = entities();
    Selection out;
    for (i64 i : indices)
        if (i >= 0 && i < static_cast<i64>(list.size())) out.push_back(list[static_cast<usize>(i)]);
    return out;
}

json::Value Document::to_json() const {
    serial::Context ctx{&engine_.assets()};
    std::vector<ecs::Entity> list = entities();
    json::Value doc = ecs::save_entities(engine_.world(), list, ctx);
    // Put the components the editor couldn't read back where they came from.
    json::Array entries = doc["entities"].array();
    bool any = false;
    for (usize i = 0; i < list.size() && i < entries.size(); ++i) {
        const RawComponents* raw = engine_.world().try_get<const RawComponents>(list[i]);
        if (!raw) continue;
        json::Value components = entries[i]["components"];
        for (const auto& [name, value] : raw->components) components.set(name, value);
        entries[i].set("components", std::move(components));
        any = true;
    }
    if (any) doc.set("entities", std::move(entries));
    return doc;
}

void Document::from_json(const json::Value& doc) {
    serial::Context ctx{&engine_.assets()};
    std::vector<ecs::Entity> spawned = ecs::load_entities(engine_.world(), doc, ctx);
    const json::Array& entries = doc["entities"].array();
    for (usize i = 0; i < spawned.size() && i < entries.size(); ++i) {
        RawComponents raw;
        for (const auto& [name, value] : entries[i]["components"].object()) {
            auto id = ecs::find_component(name);
            if (!id || !ecs::component_info(*id).savable()) raw.components.emplace_back(name, value);
        }
        if (!raw.components.empty()) engine_.world().add<RawComponents>(spawned[i], std::move(raw));
    }
    order_ = std::move(spawned);
    ecs::propagate_transforms(engine_.world());
}

void Document::destroy_all() {
    ecs::World& world = engine_.world();
    std::vector<ecs::Entity> all;
    for (const auto& a : world.archetypes())
        for (ecs::Entity e : a->entities()) all.push_back(e);
    for (ecs::Entity e : all) world.destroy(e);
    order_.clear();
}

std::string Document::snapshot() const { return json::stringify(to_json(), -1); }

void Document::restore(std::string_view text) {
    destroy_all();
    auto parsed = json::parse(text);
    if (parsed) from_json(parsed.value);
}

void Document::reset_history() {
    history_ = {{snapshot(), {}}};
    cursor_ = 0;
    saved_ = history_[0].text;
}

void Document::clear() {
    destroy_all();
    file_.clear();
    binary_ = false;
    reset_history();
}

std::string Document::title() const { return file_.empty() ? "untitled" : file_.filename().string(); }

bool Document::open(const fs::path& file) {
    auto bytes = read_file_bytes(file);
    if (!bytes) {
        error("editor: can't read {}", file.string());
        return false;
    }
    auto parsed = json::parse_any(*bytes);
    if (!parsed) {
        error("editor: {}:{}:{}: {}", file.string(), parsed.error->line, parsed.error->column, parsed.error->message);
        return false;
    }
    destroy_all();
    from_json(parsed.value);
    file_ = file;
    binary_ = json::is_binary(*bytes);
    reset_history();
    info("editor: opened {} ({} entities)", file.string(), order_.size());
    return true;
}

bool Document::save() { return file_.empty() ? false : save_as(file_); }

bool Document::save_as(const fs::path& file) {
    bool written = false;
    if (binary_ && file == file_) {
        std::vector<u8> bytes = json::encode_binary(to_json());
        written = write_file_bytes(file, bytes.data(), bytes.size());
    } else {
        std::string text = json::stringify(to_json());
        written = write_file_bytes(file, text.data(), text.size());
        binary_ = false;
    }
    if (!written) {
        error("editor: can't write {}", file.string());
        return false;
    }
    discard_autosave();
    file_ = file;
    saved_ = snapshot();
    if (history_.empty() || history_[cursor_].text != saved_) track({});
    info("editor: saved {}", file.string());
    return true;
}

bool Document::modified() const { return history_.empty() || history_[cursor_].text != saved_; }

void Document::track(const Selection& selection) {
    std::string now = snapshot();
    if (!history_.empty() && history_[cursor_].text == now) {
        history_[cursor_].selected = indices_of(selection);
        return;
    }
    history_.resize(cursor_ + 1); // a new edit drops whatever could have been redone
    history_.push_back({std::move(now), indices_of(selection)});
    if (history_.size() > 300) history_.erase(history_.begin());
    cursor_ = history_.size() - 1;
}

Selection Document::undo() {
    if (!can_undo()) return {};
    std::vector<i64> selected = history_[cursor_].selected; // what was being edited when this step was recorded
    --cursor_;
    restore(history_[cursor_].text);
    return entities_at(selected);
}

Selection Document::redo() {
    if (!can_redo()) return {};
    ++cursor_;
    restore(history_[cursor_].text);
    return entities_at(history_[cursor_].selected);
}

fs::path Document::autosave_path() const {
    if (file_.empty()) return engine_.user_data_path() / "untitled.world.autosave";
    fs::path p = file_;
    p += ".autosave";
    return p;
}

void Document::write_autosave() const {
    if (!modified()) return;
    std::string text = json::stringify(to_json());
    if (write_file_bytes(autosave_path(), text.data(), text.size()))
        info("editor: kept unsaved work in {}", autosave_path().string());
}

std::optional<std::string> Document::pending_autosave() const {
    std::error_code ec;
    fs::path p = autosave_path();
    if (!fs::exists(p, ec)) return std::nullopt;
    if (!file_.empty() && fs::exists(file_, ec) && fs::last_write_time(p, ec) < fs::last_write_time(file_, ec))
        return std::nullopt; // the file was saved after it: nothing to recover
    return read_file_text(p);
}

void Document::recover(const std::string& text) {
    restore(text);
    track({});
}

void Document::discard_autosave() const {
    std::error_code ec;
    fs::remove(autosave_path(), ec);
}

} // namespace ph::editor
