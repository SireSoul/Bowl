#include "ph/core/settings.hpp"

#include "ph/core/file.hpp"
#include "ph/core/log.hpp"

namespace ph {

namespace fs = std::filesystem;

bool Settings::load(const fs::path& file) {
    file_ = file;
    data_ = {};
    data_.make_object();
    dirty_ = false;
    auto text = read_file_text(file);
    if (!text) return false; // first run: nothing saved yet
    auto parsed = json::parse(*text);
    if (!parsed || !parsed.value.is_object()) {
        warn("settings: {} is unreadable; starting from defaults", file.string());
        return false;
    }
    data_ = std::move(parsed.value);
    return true;
}

bool Settings::save() {
    if (!dirty_ || file_.empty()) return true;
    std::string text = json::stringify(data_);
    fs::path temp = file_;
    temp += ".tmp";
    if (!write_file_bytes(temp, text.data(), text.size())) {
        error("settings: can't write {}", temp.string());
        return false;
    }
    std::error_code ec;
    fs::rename(temp, file_, ec); // atomic on the same volume: a crash never leaves half a file
    if (ec) {
        error("settings: can't replace {}: {}", file_.string(), ec.message());
        return false;
    }
    dirty_ = false;
    return true;
}

const json::Value* Settings::find(std::string_view key) const {
    const json::Value* v = &data_;
    while (!key.empty()) {
        usize dot = key.find('.');
        v = v->find(key.substr(0, dot));
        if (!v) return nullptr;
        key = dot == std::string_view::npos ? std::string_view{} : key.substr(dot + 1);
    }
    return v;
}

json::Value& Settings::slot(std::string_view key) {
    dirty_ = true; // the caller is about to write through the reference
    json::Value* v = &data_;
    while (true) {
        usize dot = key.find('.');
        std::string_view part = key.substr(0, dot);
        json::Value* child = const_cast<json::Value*>(v->find(part));
        if (!child) child = &v->set(part, json::Value{});
        if (dot == std::string_view::npos) return *child;
        if (!child->is_object()) child->make_object();
        v = child;
        key = key.substr(dot + 1);
    }
}

} // namespace ph
