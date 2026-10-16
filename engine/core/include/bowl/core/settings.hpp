#pragma once

#include "bowl/core/json.hpp"
#include "bowl/core/serialize.hpp"

#include <filesystem>
#include <string_view>

namespace bowl {

// Player preferences that outlive the game: volume, language, window mode, controls. Stored as JSON in the OS's
// per-user app data folder (engine.settings() is loaded at startup and saved at exit).
//
//     f32 music = engine.settings().get("audio.music", 0.8f);
//     engine.settings().set("audio.music", music);
//
// Dotted keys address nested objects. Values use the serializer, so reflected structs work too.
class Settings {
public:
    bool load(const std::filesystem::path& file);
    bool save(); // writes atomically (temp file + rename), and only if something changed
    const std::filesystem::path& file() const { return file_; }

    template <class T> T get(std::string_view key, T fallback) const {
        const json::Value* v = find(key);
        if (!v) return fallback;
        serial::Context ctx;
        T out = fallback;
        if (!serial::from_json(*v, out, ctx)) return fallback;
        return out;
    }
    template <class T> void set(std::string_view key, const T& value) {
        serial::Context ctx;
        slot(key) = serial::to_json(value, ctx);
        dirty_ = true;
    }
    bool has(std::string_view key) const { return find(key) != nullptr; }
    const json::Value* find(std::string_view key) const;
    json::Value& slot(std::string_view key); // for writing: creates objects along the way, marks settings changed
    const json::Value& data() const { return data_; }
    bool dirty() const { return dirty_; }

private:
    json::Value data_;
    std::filesystem::path file_;
    bool dirty_ = false;
};

} // namespace bowl
