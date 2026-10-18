// The Content workspace: a game's content edited as forms. Schemas (*.schema.json in the assets) say which files hold
// which records and what each field is; every edit goes through json::TextDocument, so the files keep their comments
// and layout, and a running game reloads each one when it's saved.

#include "content.hpp"

#include "ui.hpp"

#include "bowl/audio/audio.hpp"
#include "bowl/audio/sound_event.hpp"
#include "bowl/core/file.hpp"
#include "bowl/gfx/tilemap.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <format>

namespace bowl::editor {

namespace fs = std::filesystem;
using json::Value;

namespace {

ImTextureID texture_id(const gfx::Texture& t) { return static_cast<ImTextureID>(reinterpret_cast<intptr_t>(t.handle())); }

int resize_string(ImGuiInputTextCallbackData* data) {
    if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
        auto* s = static_cast<std::string*>(data->UserData);
        s->resize(static_cast<usize>(data->BufTextLen));
        data->Buf = s->data();
    }
    return 0;
}

bool input_text(const char* id, std::string& s, bool multiline = false, const char* hint = nullptr) {
    ImGuiInputTextFlags flags = ImGuiInputTextFlags_CallbackResize;
    if (multiline) {
        float lines = std::clamp(static_cast<float>(std::count(s.begin(), s.end(), '\n') + 1), 2.0f, 8.0f);
        f32 h = ImGui::GetTextLineHeight() * (lines + 0.6f) + ImGui::GetStyle().FramePadding.y * 2;
        return ImGui::InputTextMultiline(id, s.data(), s.size() + 1, {-1, h}, flags | ImGuiInputTextFlags_WordWrap,
                                         resize_string, &s);
    }
    if (hint) return ImGui::InputTextWithHint(id, hint, s.data(), s.size() + 1, flags, resize_string, &s);
    return ImGui::InputText(id, s.data(), s.size() + 1, flags, resize_string, &s);
}

std::string readable(std::string_view key) {
    std::string s(key);
    std::replace(s.begin(), s.end(), '_', ' ');
    if (!s.empty()) s[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(s[0])));
    return s;
}

// 390 as "6:30 am"; past midnight wraps.
std::string clock_text(i64 minutes) {
    i64 m = ((minutes % 1440) + 1440) % 1440, h = m / 60;
    return std::format("{}:{:02} {}", h % 12 == 0 ? 12 : h % 12, m % 60, h < 12 ? "am" : "pm");
}

std::string short_text(std::string_view s, usize most = 48) {
    std::string out(s.substr(0, std::min(s.size(), most)));
    std::replace(out.begin(), out.end(), '\n', ' ');
    return s.size() > most ? out + "..." : out;
}

std::string virtual_path(const fs::path& file, const fs::path& root) {
    std::error_code ec;
    return fs::relative(file, root, ec).generic_string();
}

constexpr float label_w = 150;
const ImVec4 muted = ImGui::ColorConvertU32ToFloat4(ui::color::muted);
const ImVec4 danger = ImGui::ColorConvertU32ToFloat4(ui::color::danger);
const ImVec4 accent = ImGui::ColorConvertU32ToFloat4(ui::color::accent);

// Sound events: a collection every game has, with its schema built in.
constexpr const char* builtin_schema = R"({
  "collections": [
    { "id": "bowl.sounds", "title": "Sounds", "folder": "", "extension": ".sound", "type": "bowl.sound_event",
      "help": "Sound events (.sound files): the clips a sound picks from, and how loud and high it plays. The game plays them by path." }
  ],
  "types": {
    "bowl.sound_event": {
      "fields": [
        { "key": "clips", "type": "list", "of": { "type": "audio" }, "help": "One is picked each time it plays." },
        { "key": "gain", "type": "range", "min": 0, "max": 4, "step": 0.01, "default": 1, "label": "Loudness", "help": "Picked between the two each time." },
        { "key": "pitch", "type": "range", "min": 0.25, "max": 4, "step": 0.01, "default": 1, "help": "1 is as recorded; picked between the two each time." },
        { "key": "delay", "type": "range", "min": 0, "max": 10, "step": 0.01, "default": 0, "help": "Seconds before it starts." },
        { "key": "bus", "type": "choice", "options": ["effects", "ui", "voice", "ambience", "music", "master"], "default": "effects", "help": "Which volume slider it follows." },
        { "key": "no_repeat", "type": "bool", "default": true, "label": "Never twice running", "help": "Never the same clip twice in a row." },
        { "key": "loop", "type": "bool", "default": false },
        { "key": "max_instances", "type": "int", "min": 0, "max": 64, "default": 0, "label": "At most at once", "help": "0: any number." },
        { "key": "cooldown", "type": "number", "min": 0, "max": 60, "step": 0.01, "default": 0, "help": "Seconds after it plays before it can play again." }
      ]
    }
  }
})";

bool is_picture(std::string_view ext) { return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp" || ext == ".tga" || ext == ".bwtx"; }
bool is_audio(std::string_view ext) { return ext == ".wav" || ext == ".ogg" || ext == ".oga" || ext == ".opus"; }

} // namespace

// --- Schema ----------------------------------------------------------------------------------------------------------

namespace schema {

std::string Field::title() const { return label.empty() ? readable(key) : label; }

const Variant* Type::variant(std::string_view n) const {
    for (const Variant& v : variants)
        if (v.name == n) return &v;
    return nullptr;
}

const Type* Schema::type(std::string_view name) const {
    auto it = types.find(name);
    return it == types.end() ? nullptr : &it->second;
}

const Collection* Schema::collection(std::string_view id) const {
    for (const Collection& c : collections)
        if (c.id == id) return &c;
    return nullptr;
}

Field Schema::resolve(const Field& f) const {
    const Type* t = type(f.type);
    if (!t || !t->base) return f;
    Field out = resolve(*t->base); // an alias of an alias
    out.key = f.key;
    if (!f.label.empty()) out.label = f.label;
    if (!f.help.empty()) out.help = f.help;
    if (f.has_default) {
        out.fallback = f.fallback;
        out.has_default = true;
    }
    if (f.optional) out.optional = true;
    return out;
}

namespace {

std::vector<std::string> strings(const Value& v) {
    std::vector<std::string> out;
    if (v.is_array())
        for (const Value& s : v.array()) out.emplace_back(s.as_string());
    return out;
}

Field parse_field(const Value& o) {
    Field f;
    f.key = o["key"].as_string();
    f.type = o["type"].as_string("text");
    f.label = o["label"].as_string();
    f.help = o["help"].as_string();
    if (const Value* d = o.find("default")) {
        f.fallback = *d;
        f.has_default = true;
    }
    if (const Value* v = o.find("min"); v && v->is_number()) f.min = v->as_number();
    if (const Value* v = o.find("max"); v && v->is_number()) f.max = v->as_number();
    f.step = o["step"].as_number(0);
    f.options = strings(o["options"]);
    f.free = o["free"].as_bool();
    f.multiline = o["multiline"].as_bool();
    f.optional = o["optional"].as_bool();
    f.to = o["to"].as_string();
    f.area = o["area"].as_string();
    f.suggest = o["suggest"].as_string();
    f.folder = o["folder"].as_string("maps");
    f.summary = strings(o["summary"]);
    if (const Value* of = o.find("of"); of && of->is_object()) f.of = std::make_shared<Field>(parse_field(*of));
    return f;
}

std::vector<Field> parse_fields(const Value& list) {
    std::vector<Field> out;
    if (list.is_array())
        for (const Value& o : list.array())
            if (o.is_object()) out.push_back(parse_field(o));
    return out;
}

} // namespace

void Schema::add(const Value& doc, std::string_view source) {
    if (!doc.is_object()) {
        problems.push_back(std::format("{}: not a schema", source));
        return;
    }
    if (const Value* list = doc.find("collections"); list && list->is_array())
        for (const Value& o : list->array()) {
            Collection c;
            c.id = o["id"].as_string();
            c.title = o["title"].as_string(c.id);
            c.help = o["help"].as_string();
            c.file = o["file"].as_string();
            c.list = o["list"].as_string();
            c.folder = o["folder"].as_string();
            c.extension = o["extension"].as_string();
            c.type = o["type"].as_string();
            c.label = o["label"].as_string();
            c.picture = o["picture"].as_string();
            c.id_key = o["id_key"].as_string("id");
            if (c.id.empty() || c.type.empty() || (c.file.empty() && c.extension.empty())) {
                problems.push_back(std::format("{}: a collection needs an id, a type, and a file or an extension", source));
                continue;
            }
            if (collection(c.id)) {
                problems.push_back(std::format("{}: there's already a collection \"{}\"", source, c.id));
                continue;
            }
            collections.push_back(std::move(c));
        }
    if (const Value* map = doc.find("types"); map && map->is_object())
        for (const auto& [name, o] : map->object()) {
            Type t;
            t.name = name;
            if (o.contains("base")) {
                Value base = o;
                base.set("type", o["base"]);
                t.base = parse_field(base);
            }
            t.fields = parse_fields(o["fields"]);
            t.tag = o["tag"].as_string();
            t.summary = strings(o["summary"]);
            if (const Value* vs = o.find("variants"); vs && vs->is_object())
                for (const auto& [vname, v] : vs->object())
                    t.variants.push_back({vname, std::string(v["label"].as_string(vname)), parse_fields(v["fields"])});
            types[name] = std::move(t);
        }
}

Value default_value(const Schema& s, const Field& field) {
    Field f = s.resolve(field);
    if (f.has_default) return f.fallback;
    if (f.type == "int" || f.type == "number" || f.type == "time" || f.type == "range") return f.min ? *f.min : 0.0;
    if (f.type == "bool") return false;
    if (f.type == "list" || f.type == "choices") {
        Value a;
        a.make_array();
        return a;
    }
    if (f.type == "cell") return json::Array{Value(0.5), Value(0.5)};
    if (f.type == "choice" && !f.options.empty()) return f.options[0];
    if (f.type == "color") return "#ffffff";
    if (const Type* t = s.type(f.type); t && !t->base) return default_record(s, *t);
    return "";
}

Value default_record(const Schema& s, const Type& t) {
    Value o;
    o.make_object();
    for (const Field& f : t.fields) {
        if (f.type == "variant") {
            if (t.variants.empty()) continue;
            o.set(t.tag, t.variants[0].name);
            for (const Field& vf : t.variants[0].fields) o.set(vf.key, default_value(s, vf));
            continue;
        }
        // Optional references and empty lists stay out: the files stay short.
        Field r = s.resolve(f);
        if ((r.type == "ref" && r.optional && !r.has_default)) continue;
        o.set(f.key, default_value(s, f));
    }
    if (!t.tag.empty() && !o.contains(t.tag) && !t.variants.empty()) {
        o.set(t.tag, t.variants[0].name);
        for (const Field& vf : t.variants[0].fields) o.set(vf.key, default_value(s, vf));
    }
    return o;
}

} // namespace schema

// --- Files -----------------------------------------------------------------------------------------------------------

void ContentWorkspace::init(Engine& engine, fs::path assets_dir, std::function<void(std::string)> say) {
    assets_dir_ = std::move(assets_dir);
    assets_ = &engine.assets();
    say_ = std::move(say);
    scan();
}

void ContentWorkspace::scan() {
    schema_ = {};
    auto builtin = json::parse(builtin_schema);
    schema_.add(builtin.value, "Bowl's sound events");
    pictures_.clear();
    sounds_.clear();
    audio_.clear();
    map_names_.clear();
    files_by_ext_.clear();
    maps_.clear();
    std::error_code ec;
    std::vector<fs::path> schemas;
    if (!assets_dir_.empty() && fs::exists(assets_dir_, ec))
        for (auto it = fs::recursive_directory_iterator(assets_dir_, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec || !it->is_regular_file()) continue;
            std::string name = it->path().filename().string(), ext = it->path().extension().string();
            std::string rel = virtual_path(it->path(), assets_dir_);
            if (name.ends_with(".schema.json")) schemas.push_back(it->path());
            files_by_ext_[ext].push_back(rel);
            if (is_picture(ext)) pictures_.push_back(rel);
            if (ext == ".sound") sounds_.push_back(rel);
            if (is_audio(ext)) audio_.push_back(rel);
        }
    std::sort(schemas.begin(), schemas.end());
    for (const fs::path& p : schemas) {
        auto text = read_file_text(p);
        auto doc = text ? json::parse(*text) : json::ParseResult{};
        std::string source = virtual_path(p, assets_dir_);
        if (!text || !doc) {
            schema_.problems.push_back(std::format("{}: {}", source, doc.error ? doc.error->message : "can't read it"));
            continue;
        }
        schema_.add(doc.value, source);
    }
    // Game collections first, the built-in sounds last.
    std::stable_partition(schema_.collections.begin(), schema_.collections.end(),
                          [](const schema::Collection& c) { return !c.id.starts_with("bowl."); });
    for (auto* list : {&pictures_, &sounds_, &audio_}) std::sort(list->begin(), list->end());
    for (auto& [ext, list] : files_by_ext_) std::sort(list.begin(), list.end());
    if (collection_ >= schema_.collections.size()) collection_ = 0;
    // Files on disk may have changed: drop the ones without unsaved changes, so they're read again.
    std::erase_if(docs_, [](const auto& entry) { return !entry.second.modified(); });
}

ContentWorkspace::Doc* ContentWorkspace::doc(const std::string& virtual_path) {
    if (virtual_path.empty()) return nullptr;
    auto it = docs_.find(virtual_path);
    if (it != docs_.end()) return &it->second;
    Doc d;
    d.file = assets_dir_ / virtual_path;
    auto text = read_file_text(d.file);
    d.exists = text.has_value();
    d.text.load(text.value_or(""));
    d.saved = d.text.text();
    d.history = {d.saved};
    return &docs_.emplace(virtual_path, std::move(d)).first->second;
}

bool ContentWorkspace::unsaved() const {
    return std::any_of(docs_.begin(), docs_.end(), [](const auto& e) { return e.second.modified(); });
}

bool ContentWorkspace::save_all() {
    bool ok = true;
    usize saved = 0;
    for (auto& [path, d] : docs_) {
        if (!d.modified()) continue;
        std::error_code ec;
        fs::create_directories(d.file.parent_path(), ec);
        if (!write_file_bytes(d.file, d.text.text().data(), d.text.text().size())) {
            say_("Couldn't save " + path);
            ok = false;
            continue;
        }
        d.saved = d.text.text();
        d.exists = true;
        ++saved;
    }
    if (saved) say_(saved == 1 ? std::string("Saved") : std::format("Saved {} files", saved));
    return ok;
}

void ContentWorkspace::apply_edits() {
    for (const Edit& e : edits_) {
        Doc* d = doc(e.doc);
        if (!d) continue;
        bool ok = e.kind == Edit::Set      ? d->text.set(e.path, e.value)
                  : e.kind == Edit::Insert ? d->text.insert(e.path, e.value, e.key)
                                           : d->text.erase(e.path);
        if (!ok) say_("That change didn't fit " + e.doc);
    }
    edits_.clear();
}

void ContentWorkspace::track_history() {
    if (ImGui::IsAnyItemActive()) return;
    for (auto& [path, d] : docs_) {
        if (d.text.text() == d.history[d.cursor]) continue;
        d.history.resize(d.cursor + 1);
        d.history.push_back(d.text.text());
        d.cursor = d.history.size() - 1;
    }
}

void ContentWorkspace::undo(int direction) {
    auto it = docs_.find(history_doc_);
    if (it == docs_.end()) return;
    Doc& d = it->second;
    if (direction < 0 && d.cursor > 0) --d.cursor;
    else if (direction > 0 && d.cursor + 1 < d.history.size()) ++d.cursor;
    else return;
    d.text.load(d.history[d.cursor]);
}

void ContentWorkspace::shortcuts(Engine& engine) {
    Input& in = engine.input();
    bool cmd = in.down(Key::LeftCtrl) || in.down(Key::RightCtrl) || in.down(Key::LeftSuper) || in.down(Key::RightSuper);
    bool shift = in.down(Key::LeftShift) || in.down(Key::RightShift);
    if (in.pressed(Key::F5)) scan();
    if (!cmd) return;
    if (in.pressed(Key::S)) save_all();
    if (!ImGui::GetIO().WantTextInput) {
        if (in.pressed(Key::Z)) undo(shift ? 1 : -1);
        else if (in.pressed(Key::Y)) undo(1);
    }
}

// --- Records ---------------------------------------------------------------------------------------------------------

namespace {
std::vector<std::string> files_in(const std::map<std::string, std::vector<std::string>>& by_ext, const schema::Collection& c) {
    std::vector<std::string> out;
    auto it = by_ext.find(c.extension);
    if (it == by_ext.end()) return out;
    for (const std::string& f : it->second)
        if (c.folder.empty() || f.starts_with(c.folder + "/")) out.push_back(f);
    return out;
}
} // namespace

usize ContentWorkspace::record_count(const schema::Collection& c) {
    if (c.files()) return files_in(files_by_ext_, c).size();
    Doc* d = doc(c.file);
    if (!d || !d->exists || !d->text.valid()) return 0;
    if (c.list.empty()) return 1;
    const Value& l = d->text.value()[c.list];
    return l.is_array() ? l.size() : 0;
}

const Value* ContentWorkspace::record(const schema::Collection& c, usize index, std::string* doc_path, Path* path) {
    std::string file;
    Path p;
    if (c.files()) {
        auto files = files_in(files_by_ext_, c);
        if (index >= files.size()) return nullptr;
        file = files[index];
    } else {
        file = c.file;
        if (!c.list.empty()) p = {c.list, index};
    }
    Doc* d = doc(file);
    if (!d || !d->text.valid()) return nullptr;
    const Value* v = &d->text.value();
    if (!c.list.empty() && !c.files()) {
        const Value& l = (*v)[c.list];
        if (!l.is_array() || index >= l.size()) return nullptr;
        v = &l[index];
    }
    if (doc_path) *doc_path = file;
    if (path) *path = p;
    return v;
}

std::string ContentWorkspace::record_id(const schema::Collection& c, usize index) {
    if (c.files()) {
        auto files = files_in(files_by_ext_, c);
        return index < files.size() ? files[index] : std::string();
    }
    const Value* r = record(c, index, nullptr, nullptr);
    return r ? std::string((*r)[c.id_key].as_string()) : std::string();
}

std::string ContentWorkspace::record_label(const schema::Collection& c, usize index) {
    if (c.files()) return fs::path(record_id(c, index)).stem().string();
    if (c.list.empty()) return c.title;
    const Value* r = record(c, index, nullptr, nullptr);
    if (!r) return "?";
    std::string id(r->find(c.id_key) ? (*r)[c.id_key].as_string() : "");
    std::string label(c.label.empty() ? "" : (*r)[c.label].as_string());
    // Events and other records without a name: what sets them off says the most.
    if (label.empty())
        if (const schema::Type* t = schema_.type(c.type); t && !t->tag.empty())
            if (const schema::Variant* v = t->variant((*r)[t->tag].as_string())) label = v->label;
    if (label.empty()) return id.empty() ? std::format("#{}", index + 1) : id;
    return id.empty() || label == id ? label : std::format("{}  ({})", label, id);
}

std::vector<std::pair<std::string, std::string>> ContentWorkspace::ids_of(std::string_view collection) {
    std::vector<std::pair<std::string, std::string>> out;
    const schema::Collection* c = schema_.collection(collection);
    if (!c) return out;
    usize n = record_count(*c);
    for (usize i = 0; i < n; ++i) {
        std::string id = record_id(*c, i);
        if (id.empty()) continue;
        const Value* r = c->files() ? nullptr : record(*c, i, nullptr, nullptr);
        std::string label = r && !c->label.empty() ? std::string((*r)[c->label].as_string()) : std::string();
        out.emplace_back(id, label);
    }
    return out;
}

void ContentWorkspace::new_record(const schema::Collection& c) {
    const schema::Type* t = schema_.type(c.type);
    if (!t) return;
    if (c.files()) {
        new_file_name_.clear();
        new_file_popup_ = true;
        return;
    }
    Doc* d = doc(c.file);
    if (!d) return;
    Value r = schema::default_record(schema_, *t);
    // A new id no other record has: "item_1", "item_2", ...
    std::string base = c.id.ends_with('s') ? c.id.substr(0, c.id.size() - 1) : c.id;
    std::set<std::string> taken;
    for (const auto& [id, label] : ids_of(c.id)) taken.insert(id);
    std::string id;
    for (int n = 1; id.empty() || taken.contains(id); ++n) id = std::format("{}_{}", base, n);
    if (r.contains(c.id_key)) r.set(c.id_key, id);
    if (!d->exists || !d->text.valid() || !d->text.value()[c.list].is_array()) {
        Value file;
        file.make_object();
        file.set(c.list, json::Array{r});
        d->text.load(json::stringify(file) + "\n");
        d->exists = true;
        record_ = 0;
        return;
    }
    edits_.push_back({Edit::Insert, c.file, {c.list}, r, {}});
    record_ = record_count(c);
}

void ContentWorkspace::duplicate_record(const schema::Collection& c, usize index) {
    std::string file;
    const Value* r = record(c, index, &file, nullptr);
    if (!r || c.files() || c.list.empty()) return;
    Value copy = *r;
    if (copy.contains(c.id_key)) copy.set(c.id_key, std::string(copy[c.id_key].as_string()) + "_copy");
    edits_.push_back({Edit::Insert, file, {c.list}, copy, {}});
    record_ = record_count(c);
}

void ContentWorkspace::delete_record(const schema::Collection& c, usize index) {
    if (c.files()) {
        std::string path = record_id(c, index);
        auto it = docs_.find(path);
        if (it != docs_.end()) docs_.erase(it);
        std::error_code ec;
        fs::remove(assets_dir_ / path, ec);
        say_(ec ? "Couldn't delete " + path : "Deleted " + path);
        scan();
    } else if (!c.list.empty()) {
        edits_.push_back({Edit::Erase, c.file, {c.list, index}, {}, {}});
    }
    if (record_ > 0) --record_;
}

// --- The workspace ---------------------------------------------------------------------------------------------------

void ContentWorkspace::update(Engine& engine) {
    std::optional<fs::path> picked;
    {
        std::lock_guard lock(pending_->mutex);
        if (pending_->done) {
            picked.swap(pending_->picked);
            pending_->done = false;
        }
    }
    if (!picked || !after_import_) return;
    // Copy the file into sounds/clips/, under a name nothing has yet.
    fs::path dir = assets_dir_ / "sounds" / "clips";
    std::error_code ec;
    fs::create_directories(dir, ec);
    fs::path to = dir / picked->filename();
    for (int n = 2; fs::exists(to, ec); ++n) to = dir / std::format("{}_{}{}", picked->stem().string(), n, picked->extension().string());
    if (!fs::copy_file(*picked, to, ec)) {
        say_("Couldn't copy " + picked->filename().string());
        return;
    }
    std::string rel = virtual_path(to, assets_dir_);
    say_("Added " + rel);
    auto then = std::move(after_import_);
    after_import_ = {};
    scan();
    then(rel);
    (void)engine;
}

void ContentWorkspace::import_audio(Engine& engine, std::function<void(const std::string&)> then) {
    after_import_ = std::move(then);
    static const SDL_DialogFileFilter filter{"Sounds (WAV, Ogg Vorbis, Opus)", "wav;ogg;oga;opus"};
    SDL_ShowOpenFileDialog(
        [](void* user, const char* const* files, int) {
            auto* p = static_cast<Pending*>(user);
            std::lock_guard lock(p->mutex);
            p->done = true;
            if (files && files[0]) p->picked = fs::path(files[0]);
        },
        pending_.get(), engine.window().handle(), &filter, 1, nullptr, false);
}

void ContentWorkspace::play(Engine& engine, const std::string& path, bool audio) {
    if (path.empty()) return;
    if (audio) engine.audio().play(assets_->load<audio::Sound>(path));
    else engine.audio().play_event(assets_->load<audio::SoundEvent>(path));
}

void ContentWorkspace::draw(Engine& engine, Rect area) {
    if (!jump_to_.empty()) { // a reference's go-to button
        usize nl = jump_to_.find('\n');
        std::string coll = jump_to_.substr(0, nl), id = jump_to_.substr(nl + 1);
        jump_to_.clear();
        for (usize i = 0; i < schema_.collections.size(); ++i)
            if (schema_.collections[i].id == coll) {
                collection_ = i;
                usize n = record_count(schema_.collections[i]);
                for (usize r = 0; r < n; ++r)
                    if (record_id(schema_.collections[i], r) == id) record_ = r;
                filter_[0] = 0;
            }
    }
    constexpr float left = 300;
    collections_panel(engine, {area.x, area.y, left, area.h});
    form_panel(engine, {area.x + left, area.y, area.w - left, area.h});
    map_spot_window(engine);
    apply_edits();
    track_history();
    shortcuts(engine);
}

void ContentWorkspace::collections_panel(Engine& engine, Rect area) {
    constexpr ImGuiWindowFlags fixed = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                       ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                                       ImGuiWindowFlags_NoBringToFrontOnFocus;
    ImGui::SetNextWindowPos({area.x, area.y});
    ImGui::SetNextWindowSize({area.w, area.h});
    ImGui::Begin("##content_left", nullptr, fixed);
    if (schema_.collections.size() <= 1)
        ui::hint("No schema in the assets folder yet: add a file ending in .schema.json to describe the game's content "
                 "(see the editor guide). Sound events work without one.");
    for (const std::string& p : schema_.problems) ImGui::TextColored(danger, "%s", p.c_str());

    // The collections.
    for (usize i = 0; i < schema_.collections.size(); ++i) {
        const schema::Collection& c = schema_.collections[i];
        usize n = record_count(c), bad = problems(c).size();
        bool dirty = c.files() ? false : doc(c.file) && doc(c.file)->modified();
        std::string label = c.list.empty() && !c.files() ? c.title : std::format("{}  {}", c.title, n);
        if (dirty) label += " *";
        ImGui::PushID(static_cast<int>(i));
        if (ImGui::Selectable(label.c_str(), collection_ == i)) {
            if (collection_ != i) record_ = 0;
            collection_ = i;
        }
        if (!c.help.empty()) ui::tip(c.help);
        if (bad) {
            ImGui::SameLine(area.w - 50);
            ImGui::TextColored(danger, "%zu !", bad);
            ui::tip(std::format("{} problem{}: see the top of the form", bad, bad == 1 ? "" : "s"));
        }
        ImGui::PopID();
    }
    ui::rule();
    if (collection_ >= schema_.collections.size()) {
        ImGui::End();
        return;
    }
    const schema::Collection& c = schema_.collections[collection_];
    bool single = !c.files() && c.list.empty();
    if (!single) {
        // The records, with a filter and the buttons to add, copy and remove.
        if (ui::tool_button("new", ui::Icon::Plus, false, c.files() ? "A new file" : "A new one, at the end")) new_record(c);
        ImGui::SameLine();
        if (ui::tool_button("dup", ui::Icon::Copy, false, "A copy of the one picked", 30, !c.files()))
            duplicate_record(c, record_);
        ImGui::SameLine();
        if (ui::tool_button("del", ui::Icon::Trash, false, "Remove the one picked")) confirm_delete_ = record_;
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##filter", "filter", filter_, sizeof filter_);
        ImGui::BeginChild("records");
        usize n = record_count(c);
        std::string_view filter = filter_;
        for (usize i = 0; i < n; ++i) {
            std::string label = record_label(c, i);
            if (!filter.empty()) {
                std::string a = label, b(filter);
                std::transform(a.begin(), a.end(), a.begin(), ::tolower);
                std::transform(b.begin(), b.end(), b.begin(), ::tolower);
                if (a.find(b) == std::string::npos) continue;
            }
            ImGui::PushID(static_cast<int>(i));
            // A thumbnail when the records have a picture.
            if (!c.picture.empty())
                if (const Value* r = record(c, i, nullptr, nullptr)) {
                    auto tex = assets_->load<gfx::Texture>((*r)[c.picture].as_string());
                    if (tex) {
                        f32 s = 22, k = s / std::max<f32>(1, static_cast<f32>(std::max(tex->width(), tex->height())));
                        ImVec2 size{tex->width() * k, tex->height() * k};
                        ImGui::Image(texture_id(*tex), size);
                    } else {
                        ImGui::Dummy({22, 22});
                    }
                    ImGui::SameLine();
                }
            if (ImGui::Selectable(label.c_str(), record_ == i)) record_ = i;
            ImGui::PopID();
        }
        if (n == 0) ui::hint(c.files() ? "None yet: + makes one." : "None yet: + adds one.");
        ImGui::EndChild();
    }
    ImGui::End();

    // Asking first: removing can't be undone for files, and a click on the bin is easy to make by mistake.
    if (confirm_delete_) ImGui::OpenPopup("Remove?");
    if (ImGui::BeginPopupModal("Remove?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Remove %s?", record_label(c, *confirm_delete_).c_str());
        if (c.files()) ImGui::TextColored(danger, "This deletes the file %s.", record_id(c, *confirm_delete_).c_str());
        else ui::hint("Ctrl+Z brings it back.");
        if (ImGui::Button("Remove")) {
            delete_record(c, *confirm_delete_);
            confirm_delete_.reset();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Keep it")) {
            confirm_delete_.reset();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    // Naming a new file (sound events).
    if (new_file_popup_) {
        ImGui::OpenPopup("New file");
        new_file_popup_ = false;
    }
    if (ImGui::BeginPopupModal("New file", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        std::string folder = c.folder.empty() ? "sounds" : c.folder;
        ImGui::Text("A new %s file in %s/", c.extension.c_str(), folder.c_str());
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        bool enter = input_text("##name", new_file_name_, false, "name");
        fs::path target = assets_dir_ / folder / (new_file_name_ + c.extension);
        std::error_code ec;
        bool taken = fs::exists(target, ec), ok = !new_file_name_.empty() && !taken &&
                                                   new_file_name_.find_first_of("/\\:") == std::string::npos;
        if (taken) ImGui::TextColored(danger, "There's already one with that name.");
        ImGui::BeginDisabled(!ok);
        if (ImGui::Button("Make it") || (enter && ok && ImGui::IsKeyPressed(ImGuiKey_Enter))) {
            const schema::Type* t = schema_.type(c.type);
            Value r = t ? schema::default_record(schema_, *t) : Value();
            fs::create_directories(target.parent_path(), ec);
            std::string text = json::stringify(r) + "\n";
            if (write_file_bytes(target, text.data(), text.size())) {
                std::string rel = virtual_path(target, assets_dir_);
                scan();
                auto files = files_in(files_by_ext_, c);
                record_ = static_cast<usize>(std::find(files.begin(), files.end(), rel) - files.begin());
                say_("Made " + rel);
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    (void)engine;
}

void ContentWorkspace::problems_list(const std::vector<Problem>& list) {
    if (list.empty()) return;
    ImGui::PushStyleColor(ImGuiCol_Text, danger);
    bool open = ImGui::TreeNodeEx("##problems", 0, "%zu problem%s in this collection", list.size(), list.size() == 1 ? "" : "s");
    ImGui::PopStyleColor();
    if (!open) return;
    for (usize i = 0; i < list.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        if (ImGui::Selectable(list[i].text.c_str())) record_ = list[i].record;
        ImGui::PopID();
    }
    ImGui::TreePop();
}

void ContentWorkspace::form_panel(Engine& engine, Rect area) {
    constexpr ImGuiWindowFlags fixed = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                       ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                                       ImGuiWindowFlags_NoBringToFrontOnFocus;
    ImGui::SetNextWindowPos({area.x, area.y});
    ImGui::SetNextWindowSize({area.w, area.h});
    ImGui::Begin("##content_form", nullptr, fixed);
    if (collection_ >= schema_.collections.size()) {
        ImGui::End();
        return;
    }
    const schema::Collection& c = schema_.collections[collection_];
    const schema::Type* type = schema_.type(c.type);
    if (!type) {
        ImGui::TextColored(danger, "The schema has no type \"%s\" for %s.", c.type.c_str(), c.title.c_str());
        ImGui::End();
        return;
    }
    // The file doesn't exist yet: offer to make it.
    if (!c.files()) {
        Doc* d = doc(c.file);
        if (!d->exists && !d->modified()) {
            ImGui::Text("%s isn't there yet.", c.file.c_str());
            if (ImGui::Button("Make it")) {
                Value file;
                if (c.list.empty()) file = schema::default_record(schema_, *type);
                else {
                    file.make_object();
                    file.set(c.list, json::Array{});
                }
                d->text.load(json::stringify(file) + "\n");
                d->exists = true;
            }
            ImGui::End();
            return;
        }
        if (!d->text.valid()) {
            const auto& e = *d->text.error();
            ImGui::TextColored(danger, "%s line %u, column %u: %s", c.file.c_str(), e.line, e.column, e.message.c_str());
            ui::hint("Fix it in the Data workspace (Ctrl+2) to edit it here.");
            ImGui::End();
            return;
        }
    }
    usize n = record_count(c);
    if (record_ >= n && n > 0) record_ = n - 1;
    std::string file;
    Path path;
    const Value* r = n ? record(c, record_, &file, &path) : nullptr;

    // Header: what's open, and saving.
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.3f);
    ImGui::TextUnformatted(r ? record_label(c, record_).c_str() : c.title.c_str());
    ImGui::PopFont();
    ImGui::SameLine();
    bool dirty = unsaved();
    ImGui::BeginDisabled(!dirty);
    if (ImGui::SmallButton("Save")) save_all();
    ImGui::EndDisabled();
    ui::tip("Saves every changed content file; a running game picks the change up", "Ctrl+S");
    if (r) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", file.c_str());
    }
    if (!c.help.empty()) ui::hint(c.help);
    problems_list(problems(c));
    ui::rule();
    if (!r) {
        ui::hint(n ? "Pick one on the left." : "Nothing here yet.");
        ImGui::End();
        return;
    }
    history_doc_ = file;
    ImGui::BeginChild("form", {0, 0}, 0, ImGuiWindowFlags_AlwaysVerticalScrollbar);
    if (!r->is_object()) {
        Slot s{file, path, r, nullptr, fs::path(file).parent_path().generic_string()};
        raw_widget(s);
    } else {
        Slot s{file, path, r, nullptr, fs::path(file).parent_path().generic_string()};
        object_widget(engine, *type, s);
        // Keys the schema doesn't know stay as they are; they can be edited as JSON.
        std::vector<std::string> known;
        auto add = [&](const std::vector<schema::Field>& list) {
            for (const auto& f : list) known.push_back(f.key);
        };
        add(type->fields);
        if (!type->tag.empty()) {
            known.push_back(type->tag);
            for (const auto& v : type->variants) add(v.fields);
        }
        bool header = false;
        for (const auto& [key, value] : r->object()) {
            if (std::find(known.begin(), known.end(), key) != known.end()) continue;
            if (!header) {
                ui::rule();
                ui::hint("Other keys (not in the schema), kept as they are:");
                header = true;
            }
            Path p = path;
            p.emplace_back(key);
            ImGui::PushID(key.c_str());
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(key.c_str());
            ImGui::SameLine(label_w);
            raw_widget(Slot{file, p, &value, r, s.record_dir});
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
    ImGui::End();
}

// --- Forms -----------------------------------------------------------------------------------------------------------

void ContentWorkspace::set(const Slot& slot, Value v) {
    if (slot.value) {
        edits_.push_back({Edit::Set, slot.doc, slot.path, std::move(v), {}});
        return;
    }
    // A missing key: add it to the object it belongs in.
    Path parent = slot.path;
    std::string key = std::get<std::string>(parent.back());
    parent.pop_back();
    edits_.push_back({Edit::Insert, slot.doc, parent, std::move(v), key});
}

void ContentWorkspace::raw_widget(const Slot& slot) {
    std::string text = slot.value ? json::stringify(*slot.value, -1) : std::string();
    ImGui::SetNextItemWidth(-1);
    std::string edited = text;
    if (input_text("##raw", edited) && edited != text) {
        auto parsed = json::parse(edited);
        if (parsed) set(slot, parsed.value);
    }
    ui::tip("Not what the schema expects here, so it's shown as JSON. Edit it as JSON, or remove it in the Data workspace.");
}

void ContentWorkspace::fields(Engine& engine, const std::vector<schema::Field>& list, const schema::Type* owner, const Slot& object) {
    for (const schema::Field& f : list) {
        if (f.type == "variant") {
            if (!owner || owner->tag.empty()) continue;
            variant_menu(*owner, object);
            if (const schema::Variant* v = owner->variant((*object.value)[owner->tag].as_string()))
                fields(engine, v->fields, nullptr, object);
            continue;
        }
        Slot s{object.doc, object.path, object.value->find(f.key), object.value, object.record_dir};
        s.path.emplace_back(f.key);
        ImGui::PushID(f.key.c_str());
        field(engine, f, s);
        ImGui::PopID();
    }
}

void ContentWorkspace::variant_menu(const schema::Type& t, const Slot& object) {
    std::string current(object.value->find(t.tag) ? (*object.value)[t.tag].as_string() : "");
    const schema::Variant* now = t.variant(current);
    ImGui::AlignTextToFramePadding();
    std::string title = readable(t.tag);
    for (const schema::Field& f : t.fields)
        if (f.type == "variant") title = f.title();
    ImGui::TextUnformatted(title.c_str());
    ImGui::SameLine(label_w);
    ImGui::SetNextItemWidth(-1);
    std::string preview = now ? now->label : current.empty() ? "(pick one)" : current + " (unknown)";
    if (ImGui::BeginCombo("##variant", preview.c_str())) {
        for (const schema::Variant& v : t.variants) {
            if (!ImGui::Selectable(v.label.c_str(), now == &v) || now == &v) continue;
            // The new variant's fields at their defaults, keeping whatever values carry over (an item, an NPC).
            Value fresh;
            fresh.make_object();
            for (const auto& [key, value] : object.value->object()) {
                bool old_only = now && std::any_of(now->fields.begin(), now->fields.end(), [&](const schema::Field& f) { return f.key == key; }) &&
                                std::none_of(v.fields.begin(), v.fields.end(), [&](const schema::Field& f) { return f.key == key; });
                if (!old_only) fresh.set(key, key == t.tag ? Value(v.name) : value);
            }
            if (!fresh.contains(t.tag)) fresh.set(t.tag, v.name);
            for (const schema::Field& f : v.fields)
                if (!fresh.contains(f.key)) fresh.set(f.key, schema::default_value(schema_, f));
            edits_.push_back({Edit::Set, object.doc, object.path, fresh, {}});
        }
        ImGui::EndCombo();
    }
}

void ContentWorkspace::object_widget(Engine& engine, const schema::Type& t, const Slot& slot) {
    if (!slot.value || !slot.value->is_object()) {
        if (slot.value) {
            raw_widget(slot);
            return;
        }
        if (ImGui::SmallButton("Add")) set(slot, schema::default_record(schema_, t));
        return;
    }
    bool has_menu = std::any_of(t.fields.begin(), t.fields.end(), [](const schema::Field& f) { return f.type == "variant"; });
    if (!t.tag.empty() && !has_menu) {
        variant_menu(t, slot);
        if (const schema::Variant* v = t.variant((*slot.value)[t.tag].as_string())) fields(engine, v->fields, nullptr, slot);
    }
    fields(engine, t.fields, &t, slot);
}

void ContentWorkspace::field(Engine& engine, const schema::Field& raw, const Slot& slot) {
    schema::Field f = schema_.resolve(raw);
    const schema::Type* object_type = schema_.type(f.type);
    bool block = f.type == "list" || (object_type && !object_type->base);
    ImGui::AlignTextToFramePadding();
    if (!slot.value) ImGui::PushStyleColor(ImGuiCol_Text, muted); // not in the file: its default shows
    ImGui::TextUnformatted(f.title().c_str());
    if (!slot.value) ImGui::PopStyleColor();
    if (!f.help.empty()) ui::tip(f.help);
    if (block) {
        ImGui::Indent(14);
        if (f.type == "list") list_widget(engine, f, slot);
        else object_widget(engine, *object_type, slot);
        ImGui::Unindent(14);
        ImGui::Spacing();
        return;
    }
    ImGui::SameLine(label_w);
    value_widget(engine, f, slot);
}

std::string ContentWorkspace::item_title(const schema::Field& item, const Value& v, usize index) {
    schema::Field f = schema_.resolve(item);
    const schema::Type* t = schema_.type(f.type);
    auto show = [&](const schema::Field& field, const Value& value) -> std::string {
        schema::Field r = schema_.resolve(field);
        if (r.type == "time" && value.is_number()) return clock_text(value.as_int());
        if (value.is_string()) return short_text(value.as_string());
        if (value.is_number()) return std::format("{}", value.as_number());
        return {};
    };
    if (t && !t->base) {
        std::vector<std::string> parts;
        const schema::Variant* var = t->tag.empty() ? nullptr : t->variant(v[t->tag].as_string());
        if (var) parts.push_back(var->label);
        std::vector<std::string> keys = !f.summary.empty() ? f.summary : t->summary;
        auto all = t->fields;
        if (var) all.insert(all.end(), var->fields.begin(), var->fields.end());
        if (keys.empty()) // the first two fields with something to show
            for (const schema::Field& field : all)
                if (field.type != "variant" && v.contains(field.key) && !show(field, v[field.key]).empty() && keys.size() < 2)
                    keys.push_back(field.key);
        for (const std::string& k : keys)
            for (const schema::Field& field : all)
                if (field.key == k && v.contains(k))
                    if (std::string s = show(field, v[k]); !s.empty()) parts.push_back(s);
        std::string out;
        for (const std::string& p : parts) out += (out.empty() ? "" : "  ·  ") + p;
        if (!out.empty()) return out;
    }
    if (v.is_string()) return std::string(v.as_string());
    return std::format("#{}", index + 1);
}

void ContentWorkspace::list_widget(Engine& engine, const schema::Field& f, const Slot& slot) {
    if (slot.value && !slot.value->is_array()) {
        raw_widget(slot);
        return;
    }
    schema::Field item = f.of ? schema_.resolve(*f.of) : schema::Field{.type = "text"};
    const schema::Type* t = schema_.type(item.type);
    bool cards = t && !t->base;
    usize n = slot.value ? slot.value->size() : 0;
    std::optional<std::pair<usize, usize>> swap;
    for (usize i = 0; i < n; ++i) {
        const Value& v = (*slot.value)[i];
        Slot s{slot.doc, slot.path, &v, slot.value, slot.record_dir};
        s.path.emplace_back(i);
        ImGui::PushID(static_cast<int>(i));
        // Buttons on the right: up, down, copy, remove.
        auto buttons = [&] {
            float x = ImGui::GetContentRegionMax().x - 4 * 26;
            ImGui::SameLine(std::max(x, ImGui::GetCursorPosX()));
            if (ui::tool_button("up", ui::Icon::AlignTop, false, "Move up", 22, i > 0)) swap = {i - 1, i};
            ImGui::SameLine(0, 4);
            if (ui::tool_button("down", ui::Icon::AlignBottom, false, "Move down", 22, i + 1 < n)) swap = {i, i + 1};
            ImGui::SameLine(0, 4);
            if (ui::tool_button("copy", ui::Icon::Copy, false, "Add a copy below the last", 22))
                edits_.push_back({Edit::Insert, slot.doc, slot.path, v, {}});
            ImGui::SameLine(0, 4);
            if (ui::tool_button("remove", ui::Icon::Cross, false, "Remove", 22)) edits_.push_back({Edit::Erase, slot.doc, s.path, {}, {}});
        };
        if (cards) {
            bool open = ImGui::TreeNodeEx("##item", ImGuiTreeNodeFlags_AllowOverlap | ImGuiTreeNodeFlags_SpanAvailWidth, "%s",
                                          item_title(item, v, i).c_str());
            buttons();
            if (open) {
                object_widget(engine, *t, s);
                ImGui::TreePop();
            }
        } else {
            ImGui::SetCursorPosX(ImGui::GetCursorPosX());
            float w = ImGui::GetContentRegionAvail().x - 4 * 26 - 8;
            ImGui::BeginGroup();
            ImGui::PushItemWidth(w);
            value_widget(engine, item, s);
            ImGui::PopItemWidth();
            ImGui::EndGroup();
            buttons();
        }
        ImGui::PopID();
    }
    if (swap) {
        const Value& a = (*slot.value)[swap->first];
        const Value& b = (*slot.value)[swap->second];
        Path pa = slot.path, pb = slot.path;
        pa.emplace_back(swap->first);
        pb.emplace_back(swap->second);
        edits_.push_back({Edit::Set, slot.doc, pa, b, {}});
        edits_.push_back({Edit::Set, slot.doc, pb, a, {}});
    }
    // Adding: for items with variants, which kind.
    auto add = [&](Value v) {
        if (slot.value) edits_.push_back({Edit::Insert, slot.doc, slot.path, std::move(v), {}});
        else set(slot, json::Array{std::move(v)});
    };
    if (t && !t->base && !t->variants.empty()) {
        if (ImGui::SmallButton("+ Add")) ImGui::OpenPopup("kind");
        if (ImGui::BeginPopup("kind")) {
            for (const schema::Variant& v : t->variants)
                if (ImGui::Selectable(v.label.c_str())) {
                    Value r = schema::default_record(schema_, *t);
                    for (const schema::Field& vf : t->variants[0].fields) r.erase(vf.key);
                    r.set(t->tag, v.name);
                    for (const schema::Field& vf : v.fields) r.set(vf.key, schema::default_value(schema_, vf));
                    add(r);
                }
            ImGui::EndPopup();
        }
    } else if (ImGui::SmallButton("+ Add")) {
        add(schema::default_value(schema_, item));
    }
    if (n == 0) {
        ImGui::SameLine();
        ImGui::TextDisabled("none");
    }
}

bool ContentWorkspace::path_combo(const char* id, std::string& value, const std::vector<std::string>& options, bool allow_empty) {
    bool changed = false;
    if (ImGui::BeginCombo(id, value.empty() ? "(none)" : value.c_str(), ImGuiComboFlags_HeightLarge)) {
        static char find[64];
        if (ImGui::IsWindowAppearing()) {
            find[0] = 0;
            ImGui::SetKeyboardFocusHere();
        }
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##find", "search", find, sizeof find);
        if (allow_empty && ImGui::Selectable("(none)", value.empty())) {
            value.clear();
            changed = true;
        }
        for (const std::string& o : options) {
            if (find[0] && o.find(find) == std::string::npos) continue;
            if (ImGui::Selectable(o.c_str(), o == value)) {
                value = o;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    return changed;
}

bool ContentWorkspace::picture_picker(const char* id, std::string& path) {
    bool changed = false;
    ImGui::PushID(id);
    auto tex = path.empty() ? Asset<gfx::Texture>{} : assets_->load<gfx::Texture>(path);
    if (tex) {
        f32 s = 40, k = s / std::max<f32>(1, static_cast<f32>(std::max(tex->width(), tex->height())));
        ImGui::Image(texture_id(*tex), {tex->width() * k, tex->height() * k});
    } else {
        ImGui::Dummy({40, 40});
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##pic", path.empty() ? "(none)" : path.c_str(), ImGuiComboFlags_HeightLargest)) {
        static char find[64];
        if (ImGui::IsWindowAppearing()) {
            find[0] = 0;
            ImGui::SetKeyboardFocusHere();
        }
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##find", "search", find, sizeof find);
        for (const std::string& p : pictures_) {
            if (find[0] && p.find(find) == std::string::npos) continue;
            if (p.starts_with("engine/")) continue;
            ImGui::PushID(p.c_str());
            auto t = assets_->load<gfx::Texture>(p);
            if (t) {
                f32 k = 28 / std::max<f32>(1, static_cast<f32>(std::max(t->width(), t->height())));
                ImGui::Image(texture_id(*t), {t->width() * k, t->height() * k});
                ImGui::SameLine(40);
            }
            if (ImGui::Selectable(p.c_str(), p == path, 0, {0, 28})) {
                path = p;
                changed = true;
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    ImGui::PopID();
    return changed;
}

void ContentWorkspace::sound_buttons(Engine& engine, const std::string& path, bool audio) {
    ImGui::SameLine(0, 4);
    ImGui::BeginDisabled(path.empty());
    if (ImGui::ArrowButton("##play", ImGuiDir_Right)) play(engine, path, audio);
    ImGui::EndDisabled();
    ui::tip("Play it");
}

void ContentWorkspace::suggestions(std::string_view key, std::set<std::string>& out) {
    // Every value held by a field with this suggest key, anywhere in the content.
    std::function<void(const schema::Field&, const Value*)> walk = [&](const schema::Field& raw, const Value* v) {
        if (!v) return;
        schema::Field f = schema_.resolve(raw);
        if (f.suggest == key && v->is_string() && !v->as_string().empty()) out.emplace(v->as_string());
        if (f.type == "list" && f.of && v->is_array())
            for (const Value& item : v->array()) walk(*f.of, &item);
        if (const schema::Type* t = schema_.type(f.type); t && !t->base && v->is_object()) {
            for (const schema::Field& sub : t->fields) walk(sub, v->find(sub.key));
            if (!t->tag.empty())
                if (const schema::Variant* var = t->variant((*v)[t->tag].as_string()))
                    for (const schema::Field& sub : var->fields) walk(sub, v->find(sub.key));
        }
    };
    for (const schema::Collection& c : schema_.collections) {
        usize n = record_count(c);
        schema::Field root{.type = c.type};
        for (usize i = 0; i < n; ++i) walk(root, record(c, i, nullptr, nullptr));
    }
}

void ContentWorkspace::value_widget(Engine& engine, const schema::Field& f, const Slot& slot) {
    const Value* v = slot.value;
    const Value fallback = f.has_default ? f.fallback : schema::default_value(schema_, f);
    const Value& shown = v ? *v : fallback;
    const std::string& t = f.type;
    float width = ImGui::CalcItemWidth() > 0 && ImGui::GetContentRegionAvail().x > 0 ? ImGui::GetContentRegionAvail().x : 200;

    // What the file holds must be what the field shows, or it's shown as JSON.
    bool text_kind = t == "text" || t == "id" || t == "ref" || t == "picture" || t == "sound" || t == "audio" ||
                     t == "map" || t == "spawn" || t == "choice" || t == "color";
    bool fits = !v || (text_kind && v->is_string()) || ((t == "int" || t == "number" || t == "time") && v->is_number()) ||
                (t == "bool" && v->is_bool()) || (t == "choices" && v->is_array()) ||
                (t == "cell" && v->is_array() && v->size() == 2) || (t == "range" && (v->is_number() || v->is_array()));
    if (!fits) {
        raw_widget(slot);
        return;
    }

    if (t == "text" || t == "id") {
        std::string s(shown.as_string());
        ImGui::SetNextItemWidth(f.suggest.empty() ? -1 : width - 30);
        if (input_text("##v", s, f.multiline)) set(slot, s);
        if (!f.suggest.empty()) {
            ImGui::SameLine(0, 4);
            if (ImGui::ArrowButton("##suggest", ImGuiDir_Down)) ImGui::OpenPopup("suggest");
            ui::tip("Values used elsewhere in the content");
            if (ImGui::BeginPopup("suggest")) {
                std::set<std::string> values;
                suggestions(f.suggest, values);
                if (values.empty()) ImGui::TextDisabled("None yet");
                for (const std::string& s2 : values)
                    if (ImGui::Selectable(s2.c_str())) set(slot, s2);
                ImGui::EndPopup();
            }
        }
    } else if (t == "int") {
        i64 n = static_cast<i64>(std::llround(shown.as_number()));
        i64 lo = f.min ? static_cast<i64>(*f.min) : INT32_MIN, hi = f.max ? static_cast<i64>(*f.max) : INT32_MAX;
        ImGui::SetNextItemWidth(-1);
        if (ImGui::DragScalar("##v", ImGuiDataType_S64, &n, f.step > 0 ? static_cast<float>(f.step) : 0.25f, &lo, &hi, "%lld",
                              ImGuiSliderFlags_AlwaysClamp))
            set(slot, static_cast<f64>(n));
    } else if (t == "number") {
        f64 n = shown.as_number();
        f64 lo = f.min.value_or(-1e9), hi = f.max.value_or(1e9);
        ImGui::SetNextItemWidth(-1);
        if (ImGui::DragScalar("##v", ImGuiDataType_Double, &n, f.step > 0 ? static_cast<float>(f.step) : 0.01f, &lo, &hi, "%.3g",
                              ImGuiSliderFlags_AlwaysClamp))
            set(slot, std::round(n * 1000) / 1000);
    } else if (t == "time") {
        int m = static_cast<int>(shown.as_int());
        std::string label = clock_text(m);
        if (m >= 1440) label += " (next day)";
        ImGui::SetNextItemWidth(-1);
        if (ImGui::DragInt("##v", &m, 0.4f, 0, 48 * 60 - 1, label.c_str(), ImGuiSliderFlags_AlwaysClamp))
            set(slot, static_cast<f64>(m - m % 5)); // in steps of five minutes
        ui::tip("Drag to change it in five-minute steps; Ctrl+click to type minutes after midnight (6:30 am is 390).");
    } else if (t == "bool") {
        bool b = shown.as_bool();
        if (ImGui::Checkbox("##v", &b)) set(slot, b);
    } else if (t == "choice") {
        std::string s(shown.as_string());
        ImGui::SetNextItemWidth(-1);
        if (f.free) {
            ImGui::SetNextItemWidth(width - 30);
            if (input_text("##v", s)) set(slot, s);
            ImGui::SameLine(0, 4);
            if (ImGui::ArrowButton("##pick", ImGuiDir_Down)) ImGui::OpenPopup("pick");
            if (ImGui::BeginPopup("pick")) {
                for (const std::string& o : f.options)
                    if (ImGui::Selectable(o.c_str(), o == s)) set(slot, o);
                ImGui::EndPopup();
            }
        } else if (ImGui::BeginCombo("##v", s.c_str())) {
            for (const std::string& o : f.options)
                if (ImGui::Selectable(o.c_str(), o == s)) set(slot, o);
            ImGui::EndCombo();
        }
    } else if (t == "choices") {
        std::vector<std::string> on;
        if (shown.is_array())
            for (const Value& x : shown.array()) on.emplace_back(x.as_string());
        std::vector<std::string> all = f.options;
        for (const std::string& o : on)
            if (std::find(all.begin(), all.end(), o) == all.end()) all.push_back(o);
        bool changed = false;
        for (usize i = 0; i < all.size(); ++i) {
            if (i) ImGui::SameLine(0, 3);
            bool is_on = std::find(on.begin(), on.end(), all[i]) != on.end();
            if (is_on) ImGui::PushStyleColor(ImGuiCol_Button, ui::color::on);
            if (ImGui::SmallButton(all[i].c_str())) {
                if (is_on) std::erase(on, all[i]);
                else on.push_back(all[i]);
                changed = true;
            }
            if (is_on) ImGui::PopStyleColor();
        }
        if (f.free) {
            ImGui::SameLine(0, 6);
            static char extra[48];
            ImGui::SetNextItemWidth(90);
            if (ImGui::InputTextWithHint("##more", "+ other", extra, sizeof extra, ImGuiInputTextFlags_EnterReturnsTrue) && extra[0]) {
                on.emplace_back(extra);
                extra[0] = 0;
                changed = true;
            }
        }
        if (changed) {
            // Kept in the options' order.
            json::Array a;
            for (const std::string& o : all)
                if (std::find(on.begin(), on.end(), o) != on.end()) a.emplace_back(o);
            set(slot, Value(std::move(a)));
        }
    } else if (t == "ref") {
        std::string s(shown.as_string());
        auto ids = ids_of(f.to);
        std::string preview = s.empty() ? "(none)" : s;
        for (const auto& [id, label] : ids)
            if (id == s && !label.empty()) preview = std::format("{}  ({})", label, id);
        bool known = s.empty() || std::any_of(ids.begin(), ids.end(), [&](const auto& p) { return p.first == s; });
        ImGui::SetNextItemWidth(width - (known && !s.empty() ? 30 : 0));
        if (!known) ImGui::PushStyleColor(ImGuiCol_Text, f.free ? ImGui::GetStyleColorVec4(ImGuiCol_Text) : danger);
        if (ImGui::BeginCombo("##v", preview.c_str(), ImGuiComboFlags_HeightLarge)) {
            if (!known) ImGui::PopStyleColor();
            if (f.free) {
                std::string typed = known ? std::string() : s;
                ImGui::SetNextItemWidth(-1);
                if (input_text("##typed", typed, false, "or type a name")) set(slot, typed);
            }
            if ((f.optional || f.free) && ImGui::Selectable("(none)", s.empty())) set(slot, std::string());
            for (const auto& [id, label] : ids) {
                std::string row = label.empty() ? id : std::format("{}  ({})", label, id);
                if (ImGui::Selectable(row.c_str(), id == s)) set(slot, id);
            }
            ImGui::EndCombo();
        } else if (!known) {
            ImGui::PopStyleColor();
        }
        if (!known && !f.free) ui::tip(std::format("There's no \"{}\" in {}", s, f.to));
        if (known && !s.empty()) {
            ImGui::SameLine(0, 4);
            if (ImGui::ArrowButton("##go", ImGuiDir_Right)) jump_to_ = f.to + "\n" + s;
            ui::tip("Go to it");
        }
    } else if (t == "picture") {
        std::string s(shown.as_string());
        if (picture_picker("##v", s)) set(slot, s);
    } else if (t == "sound") {
        std::string s(shown.as_string());
        ImGui::SetNextItemWidth(width - 60);
        if (path_combo("##v", s, sounds_, true)) set(slot, s);
        sound_buttons(engine, s, false);
        ImGui::SameLine(0, 4);
        if (ImGui::Button("+", {24, 0})) {
            // A new sound event from an audio file: the clip goes in sounds/clips/, the event in sounds/.
            Slot target{slot.doc, slot.path, nullptr, nullptr, {}};
            bool exists = slot.value != nullptr;
            import_audio(engine, [this, target, exists](const std::string& clip) {
                fs::path clip_path(clip);
                std::string event = "sounds/" + clip_path.stem().string() + ".sound";
                std::error_code ec;
                for (int k = 2; fs::exists(assets_dir_ / event, ec); ++k)
                    event = std::format("sounds/{}_{}.sound", clip_path.stem().string(), k);
                Value e;
                e.make_object();
                e.set("clips", json::Array{Value(virtual_path(assets_dir_ / clip, assets_dir_ / "sounds"))});
                std::string text = json::stringify(e) + "\n";
                write_file_bytes(assets_dir_ / event, text.data(), text.size());
                scan();
                Slot at = target;
                Value dummy;
                if (exists) at.value = &dummy; // a Set rather than an Insert
                set(at, event);
                apply_edits();
                say_("Made " + event);
            });
        }
        ui::tip("A new sound from an audio file (WAV, Ogg): copies it into sounds/clips/ and makes a .sound for it");
    } else if (t == "audio") {
        // Paths in a .sound are from its own folder.
        std::string s(shown.as_string());
        std::string base = slot.record_dir.empty() ? std::string() : slot.record_dir + "/";
        std::vector<std::string> options;
        for (const std::string& a : audio_)
            options.push_back(!base.empty() && a.starts_with(base) ? a.substr(base.size()) : "/" + a);
        std::string full = s.starts_with('/') ? s.substr(1) : base + s;
        ImGui::SetNextItemWidth(std::max(80.0f, ImGui::CalcItemWidth() - 60));
        if (path_combo("##v", s, options, false)) set(slot, s);
        sound_buttons(engine, full, true);
        ImGui::SameLine(0, 4);
        if (ImGui::Button("+", {24, 0})) {
            Slot target{slot.doc, slot.path, nullptr, nullptr, slot.record_dir};
            bool exists = slot.value != nullptr;
            import_audio(engine, [this, target, exists, base](const std::string& clip) {
                Slot at = target;
                Value dummy;
                if (exists) at.value = &dummy;
                set(at, !base.empty() && clip.starts_with(base) ? clip.substr(base.size()) : "/" + clip);
                apply_edits();
            });
        }
        ui::tip("Bring in an audio file (WAV, Ogg): it's copied into sounds/clips/");
    } else if (t == "map") {
        std::string s(shown.as_string());
        std::vector<std::string> names;
        for (const char* ext : {".tmj", ".tmx"})
            if (auto it = files_by_ext_.find(ext); it != files_by_ext_.end())
                for (const std::string& p : it->second)
                    if (p.starts_with(f.folder + "/")) names.push_back(fs::path(p).stem().string());
        std::sort(names.begin(), names.end());
        ImGui::SetNextItemWidth(-1);
        if (path_combo("##v", s, names, f.optional)) set(slot, s);
    } else if (t == "spawn") {
        std::string s(shown.as_string());
        std::string map = slot.parent ? std::string((*slot.parent)[f.area].as_string()) : std::string();
        std::vector<std::string> names;
        if (MapInfo* m = map.empty() ? nullptr : map_info(map, "maps"))
            for (const auto& [name, cell] : m->spawns) names.push_back(name);
        ImGui::SetNextItemWidth(-1);
        if (names.empty()) {
            if (input_text("##v", s, false, map.empty() ? "pick the map first" : "no spawn points on that map")) set(slot, s);
        } else if (path_combo("##v", s, names, false)) {
            set(slot, s);
        }
    } else if (t == "cell") {
        f32 xy[2] = {shown.is_array() ? shown[0].as_float() : 0, shown.is_array() ? shown[1].as_float() : 0};
        ImGui::SetNextItemWidth(std::max(80.0f, width - 110));
        if (ImGui::DragFloat2("##v", xy, 0.05f, -1000, 1000, "%.2f")) set(slot, json::Array{Value(xy[0]), Value(xy[1])});
        ImGui::SameLine(0, 4);
        std::string map = slot.parent ? std::string((*slot.parent)[f.area].as_string()) : std::string();
        ImGui::BeginDisabled(map.empty());
        if (ImGui::Button("On the map...")) spot_ = SpotPick{slot.doc, slot.path, slot.value != nullptr, map, "maps", {xy[0], xy[1]}};
        ImGui::EndDisabled();
        ui::tip(map.empty() ? "Pick the map first" : "Click the spot on the map");
    } else if (t == "color") {
        std::string s(shown.as_string());
        u32 hex = 0xffffff;
        if (s.size() == 7 && s[0] == '#') hex = static_cast<u32>(std::strtoul(s.c_str() + 1, nullptr, 16));
        float c[3] = {((hex >> 16) & 255) / 255.0f, ((hex >> 8) & 255) / 255.0f, (hex & 255) / 255.0f};
        if (ImGui::ColorEdit3("##v", c))
            set(slot, std::format("#{:02x}{:02x}{:02x}", static_cast<int>(c[0] * 255 + 0.5f), static_cast<int>(c[1] * 255 + 0.5f),
                                  static_cast<int>(c[2] * 255 + 0.5f)));
    } else if (t == "range") {
        // One number, or [min, max] picked between each time.
        bool two = shown.is_array();
        float lo = two ? shown[0].as_float() : shown.as_float(), hi = two ? shown[1].as_float(1) : lo;
        float mn = static_cast<float>(f.min.value_or(0)), mx = static_cast<float>(f.max.value_or(100));
        float speed = f.step > 0 ? static_cast<float>(f.step) : 0.01f;
        ImGui::SetNextItemWidth(std::max(80.0f, width - 90));
        if (two) {
            if (ImGui::DragFloatRange2("##v", &lo, &hi, speed, mn, mx, "%.2f", "%.2f", ImGuiSliderFlags_AlwaysClamp))
                set(slot, json::Array{Value(std::round(lo * 100) / 100.0), Value(std::round(hi * 100) / 100.0)});
        } else if (ImGui::DragFloat("##v", &lo, speed, mn, mx, "%.2f", ImGuiSliderFlags_AlwaysClamp)) {
            set(slot, std::round(lo * 100) / 100.0);
        }
        ImGui::SameLine(0, 6);
        if (ImGui::Checkbox("between", &two))
            set(slot, two ? Value(json::Array{Value(lo), Value(lo)}) : Value(static_cast<f64>(lo)));
        ui::tip("Pick a different value each time, between two");
    } else {
        raw_widget(slot);
    }
}

// --- Maps ------------------------------------------------------------------------------------------------------------

std::string ContentWorkspace::map_path(const std::string& name, const std::string& folder) const {
    std::error_code ec;
    for (const char* ext : {".tmj", ".tmx"})
        if (fs::exists(assets_dir_ / folder / (name + ext), ec)) return folder + "/" + name + ext;
    return folder + "/" + name + ".tmj";
}

ContentWorkspace::MapInfo* ContentWorkspace::map_info(const std::string& name, const std::string& folder) {
    auto it = maps_.find(name);
    if (it == maps_.end()) {
        MapInfo info;
        info.map = assets_->load<gfx::Tilemap>(map_path(name, folder));
        it = maps_.emplace(name, std::move(info)).first;
    }
    MapInfo& m = it->second;
    m.spawns.clear();
    m.markers.clear();
    if (!m.map) return &m;
    // Object positions, in cells: isometric maps measure both axes in tile heights.
    const gfx::Tilemap& map = *m.map;
    f32 per_x = static_cast<f32>(map.orientation == gfx::Tilemap::Orientation::Isometric ? map.tile_height : map.tile_width);
    f32 per_y = static_cast<f32>(map.tile_height);
    for (const gfx::MapLayer& layer : map.layers)
        for (const gfx::MapObject& o : layer.objects) {
            Rect cells{o.bounds.x / per_x, o.bounds.y / per_y, o.bounds.w / per_x, o.bounds.h / per_y};
            if (o.type == "spawn") m.spawns.emplace_back(o.name, cells.position());
            else if (!o.name.empty()) m.markers.emplace_back(o.name, cells);
        }
    return &m;
}

void ContentWorkspace::map_spot_window(Engine& engine) {
    if (!spot_) return;
    bool open = true;
    ImGui::SetNextWindowSize({760, 600}, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, {0.5f, 0.5f});
    std::string title = std::format("{} — click a spot###spot", spot_->map);
    if (!ImGui::Begin(title.c_str(), &open, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar |
                                                ImGuiWindowFlags_NoScrollWithMouse)) {
        ImGui::End();
        if (!open) spot_.reset();
        return;
    }
    MapInfo* info = map_info(spot_->map, spot_->folder);
    if (!info || !info->map) {
        ImGui::TextColored(danger, "Can't open %s", map_path(spot_->map, spot_->folder).c_str());
        ImGui::End();
        if (!open) spot_.reset();
        return;
    }
    const gfx::Tilemap& m = *info->map;
    ui::hint("Click to put it in the middle of a cell, Alt-click for exactly there. The wheel zooms, right-drag moves.");
    ImVec2 origin = ImGui::GetCursorScreenPos(), avail = ImGui::GetContentRegionAvail();
    avail.y -= 30;
    ImGui::InvisibleButton("canvas", avail, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    bool hovered = ImGui::IsItemHovered();

    // The map's extent in map pixels: the cells' corners, with room above for tall tiles.
    f32 W = static_cast<f32>(m.width), H = static_cast<f32>(m.height);
    Vec2 corners[] = {m.cell_to_world(0, 0), m.cell_to_world(W, 0), m.cell_to_world(0, H), m.cell_to_world(W, H)};
    Vec2 lo = corners[0], hi = corners[0];
    for (Vec2 c : corners) {
        lo = {std::min(lo.x, c.x), std::min(lo.y, c.y)};
        hi = {std::max(hi.x, c.x), std::max(hi.y, c.y)};
    }
    if (spot_->zoom <= 0) {
        spot_->zoom = std::min(avail.x / (hi.x - lo.x + 64), avail.y / (hi.y - lo.y + 64));
        spot_->pan = (lo + hi) * 0.5f;
    }
    ImGuiIO& io = ImGui::GetIO();
    Vec2 center{origin.x + avail.x * 0.5f, origin.y + avail.y * 0.5f};
    auto to_screen = [&](Vec2 w) { Vec2 s = center + (w - spot_->pan) * spot_->zoom; return ImVec2{s.x, s.y}; };
    auto to_world = [&](ImVec2 s) { return spot_->pan + (Vec2{s.x, s.y} - center) / spot_->zoom; };
    if (hovered && io.MouseWheel != 0) {
        Vec2 before = to_world(io.MousePos);
        spot_->zoom = std::clamp(spot_->zoom * std::pow(1.15f, io.MouseWheel), 0.1f, 20.0f);
        Vec2 after = to_world(io.MousePos);
        spot_->pan += before - after;
    }
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Right))
        spot_->pan -= Vec2{io.MouseDelta.x, io.MouseDelta.y} / spot_->zoom;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(origin, {origin.x + avail.x, origin.y + avail.y}, true);
    dl->AddRectFilled(origin, {origin.x + avail.x, origin.y + avail.y}, IM_COL32(16, 18, 24, 255));
    bool iso = m.orientation == gfx::Tilemap::Orientation::Isometric;
    // Tile layers, in order, each tile with its bottom on its cell like Tiled draws them.
    for (const gfx::MapLayer& layer : m.layers) {
        if (layer.kind != gfx::MapLayer::Kind::Tiles || !layer.visible) continue;
        ImU32 tint = IM_COL32(255, 255, 255, static_cast<int>(layer.opacity * 255));
        for (i32 y = 0; y < static_cast<i32>(m.height); ++y)
            for (i32 x = 0; x < static_cast<i32>(m.width); ++x) {
                u32 gid = layer.at(x, y) & gfx::tile_gid_mask;
                if (!gid) continue;
                const gfx::Tileset* ts = m.tileset_for(gid);
                if (!ts || !ts->image) continue;
                Rect src = ts->source_rect(gid - ts->first_gid);
                Vec2 a = m.cell_to_world(static_cast<f32>(x), static_cast<f32>(y));
                f32 left = iso ? a.x - m.tile_width * 0.5f : a.x, bottom = a.y + m.tile_height;
                Vec2 p0{left + ts->tile_offset.x, bottom - src.h + ts->tile_offset.y};
                Vec2 p1 = p0 + Vec2{src.w, src.h};
                f32 iw = static_cast<f32>(ts->image->width()), ih = static_cast<f32>(ts->image->height());
                dl->AddImage(texture_id(*ts->image), to_screen(p0), to_screen(p1), {src.x / iw, src.y / ih},
                             {src.right() / iw, src.bottom() / ih}, tint);
            }
    }
    auto cell_outline = [&](f32 x, f32 y, f32 w, f32 h, ImU32 color, f32 thick) {
        ImVec2 pts[4] = {to_screen(m.cell_to_world(x, y)), to_screen(m.cell_to_world(x + w, y)),
                         to_screen(m.cell_to_world(x + w, y + h)), to_screen(m.cell_to_world(x, y + h))};
        dl->AddPolyline(pts, 4, color, ImDrawFlags_Closed, thick);
    };
    for (const auto& [name, r] : info->markers) {
        cell_outline(r.x, r.y, std::max(r.w, 0.2f), std::max(r.h, 0.2f), IM_COL32(90, 200, 200, 200), 1.5f);
        dl->AddText(to_screen(m.cell_to_world(r.x, r.y)), IM_COL32(150, 230, 230, 255), name.c_str());
    }
    for (const auto& [name, at] : info->spawns) {
        ImVec2 p = to_screen(m.cell_to_world(at.x, at.y));
        dl->AddCircleFilled(p, 4, IM_COL32(120, 200, 255, 255));
        dl->AddText({p.x + 6, p.y - 7}, IM_COL32(200, 230, 255, 255), name.c_str());
    }
    // The cell under the mouse, and the spot now.
    Vec2 under = m.world_to_cell(to_world(io.MousePos));
    if (hovered) {
        cell_outline(std::floor(under.x), std::floor(under.y), 1, 1, ui::color::accent, 2);
        ImGui::SetTooltip("%.2f, %.2f", under.x, under.y);
    }
    ImVec2 now = to_screen(m.cell_to_world(spot_->cell.x, spot_->cell.y));
    dl->AddCircleFilled(now, 6, ui::color::accent);
    dl->AddCircle(now, 6, IM_COL32(0, 0, 0, 255), 0, 2);
    dl->PopClipRect();

    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        Vec2 cell = io.KeyAlt ? Vec2{std::round(under.x * 100) / 100, std::round(under.y * 100) / 100}
                              : Vec2{std::floor(under.x) + 0.5f, std::floor(under.y) + 0.5f};
        spot_->cell = cell;
        Doc* d = doc(spot_->doc);
        if (d) {
            Value v = json::Array{Value(cell.x), Value(cell.y)};
            if (spot_->exists) {
                d->text.set(spot_->path, v);
            } else {
                Path parent = spot_->path;
                std::string key = std::get<std::string>(parent.back());
                parent.pop_back();
                d->text.insert(parent, v, key);
                spot_->exists = true;
            }
        }
    }
    ImGui::Text("Spot: %.2f, %.2f", spot_->cell.x, spot_->cell.y);
    ImGui::SameLine(ImGui::GetContentRegionMax().x - 60);
    if (ImGui::Button("Done")) open = false;
    ImGui::End();
    if (!open) spot_.reset();
    (void)engine;
}

// --- Problems --------------------------------------------------------------------------------------------------------

void ContentWorkspace::check(const schema::Field& raw, const Value* v, const std::string& where, const schema::Collection& c,
                             usize record, std::vector<Problem>& out) {
    schema::Field f = schema_.resolve(raw);
    std::string here = where.empty() ? f.title() : where + " · " + f.title();
    auto problem = [&](std::string text) { out.push_back({c.id, record, std::format("{}: {}", here, text)}); };
    std::error_code ec;
    if (!v || v->is_null()) {
        if (f.type == "ref" && !f.optional && !f.free && !f.has_default) problem("not set");
        if (f.type == "id") problem("no id");
        return;
    }
    if (f.type == "id" && v->as_string().empty()) problem("no id");
    if (f.type == "ref" && !f.free) {
        std::string s(v->as_string());
        if (s.empty() && !f.optional) problem("not set");
        else if (!s.empty()) {
            auto ids = ids_of(f.to);
            if (std::none_of(ids.begin(), ids.end(), [&](const auto& p) { return p.first == s; }))
                problem(std::format("there's no \"{}\" in {}", s, f.to));
        }
    }
    if ((f.type == "picture" || f.type == "sound") && !v->as_string().empty() && !fs::exists(assets_dir_ / v->as_string(), ec))
        problem(std::format("{} isn't there", v->as_string()));
    if (f.type == "map" && !v->as_string().empty() && !fs::exists(assets_dir_ / map_path(std::string(v->as_string()), f.folder), ec))
        problem(std::format("there's no map \"{}\"", v->as_string()));
    if (f.type == "list" && f.of && v->is_array())
        for (usize i = 0; i < v->size(); ++i) check(*f.of, &(*v)[i], std::format("{} {}", here, i + 1), c, record, out);
    if (const schema::Type* t = schema_.type(f.type); t && !t->base && v->is_object()) {
        for (const schema::Field& sub : t->fields)
            if (sub.type != "variant") check(sub, v->find(sub.key), where.empty() ? std::string() : here, c, record, out);
        if (!t->tag.empty()) {
            const schema::Variant* var = t->variant((*v)[t->tag].as_string());
            if (!var) out.push_back({c.id, record, std::format("{}: \"{}\" isn't one of the kinds", here, (*v)[t->tag].as_string())});
            else
                for (const schema::Field& sub : var->fields) check(sub, v->find(sub.key), here, c, record, out);
        }
    }
}

std::vector<ContentWorkspace::Problem> ContentWorkspace::problems(const schema::Collection& c) {
    std::vector<Problem> out;
    if (!schema_.type(c.type)) return out;
    usize n = record_count(c);
    std::map<std::string, usize> seen;
    for (usize i = 0; i < n; ++i) {
        const Value* r = record(c, i, nullptr, nullptr);
        if (!r) continue;
        std::string label = record_label(c, i);
        check(schema::Field{.type = c.type}, r, "", c, i, out);
        for (Problem& p : out)
            if (p.record == i && p.collection == c.id && !p.text.starts_with(label)) p.text = label + ": " + p.text;
        if (!c.files() && !c.list.empty()) {
            std::string id(r->find(c.id_key) ? (*r)[c.id_key].as_string() : "");
            if (!id.empty() && !seen.emplace(id, i).second) out.push_back({c.id, i, std::format("{}: another one has the id \"{}\"", label, id)});
        }
    }
    return out;
}

} // namespace bowl::editor
