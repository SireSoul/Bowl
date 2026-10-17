// The Game workspace: Bowl Editor attached to a game running on this computer, through the game's dev link
// (bowl/app/dev_link.hpp). Its profiler, its schedules (which system reads and writes what, and why each batch starts)
// and its entities, whose components can be changed while it runs.

#include "editor.hpp"

#include <algorithm>
#include <chrono>
#include <format>
#include <map>

namespace bowl::editor {

namespace {

constexpr float top_h = 48, status_h = 28, left_w = 300;
constexpr ImGuiWindowFlags fixed = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoBringToFrontOnFocus;
constexpr f64 scan_every = 1.0;      // seconds between looks for running games
constexpr usize profile_frames = 10; // Frames averaged in the zone table (a second of them)
constexpr u32 page_size = 200;

ImVec4 accent() { return ImGui::ColorConvertU32ToFloat4(ui::color::accent); }
ImVec4 muted() { return ImGui::ColorConvertU32ToFloat4(ui::color::muted); }

std::string joined(const std::vector<std::string>& names) {
    std::string out;
    for (const auto& n : names) out += (out.empty() ? "" : ", ") + n;
    return out.empty() ? "-" : out;
}

bool has(const std::vector<std::string>& names, std::string_view name) {
    return std::find(names.begin(), names.end(), name) != names.end();
}

std::string today() {
    auto now = std::chrono::system_clock::now();
    return std::format("{:%Y-%m-%d %H:%M}", std::chrono::floor<std::chrono::minutes>(now));
}

} // namespace

void Editor::game_update(Engine& engine) {
    link_.update();
    if (ImGui::GetTime() - link_scanned_ >= scan_every) {
        link_scanned_ = ImGui::GetTime();
        link_games_ = devlink::running_games();
        if (attach_first_ && !link_games_.empty() && !link_.connected() && !link_.connecting()) {
            const auto& g = link_games_.front();
            if (link_.connect(g)) link_game_ = g.game + "#" + std::to_string(g.pid);
            attach_first_ = false;
        }
    }
    // Keep the newest schedules the game sent, so the Systems pane and the Level tab have them with no game running.
    if (link_.schedules() && link_.hello()) {
        serial::Context ctx;
        json::Value now = serial::to_json(*link_.schedules(), ctx);
        if (!known_schedules_ || json::stringify(serial::to_json(*known_schedules_, ctx), 0) != json::stringify(now, 0)) {
            known_schedules_ = *link_.schedules();
            known_schedules_when_ = today();
            Settings& settings = engine.settings();
            settings.slot("game_link.game") = link_.hello()->game;
            settings.slot("game_link.when") = known_schedules_when_;
            settings.slot("game_link.schedules") = std::move(now);
        }
    } else if (!known_schedules_) { // first frame: what an earlier session saw
        if (const json::Value* saved = engine.settings().find("game_link.schedules")) {
            devlink::Schedules s;
            serial::Context ctx;
            if (serial::from_json(*saved, s, ctx)) {
                known_schedules_ = std::move(s);
                if (const json::Value* when = engine.settings().find("game_link.when"))
                    known_schedules_when_ = std::string(when->as_string());
                if (const json::Value* game = engine.settings().find("game_link.game"))
                    link_game_ = std::string(game->as_string());
            }
        }
        if (!known_schedules_) known_schedules_ = devlink::Schedules{}; // looked: nothing saved
    }
}

void Editor::game_workspace(Engine&) {
    const ImGuiIO& io = ImGui::GetIO();
    float h = io.DisplaySize.y - top_h - status_h;

    // Left: the games running on this computer, and the panes.
    ImGui::SetNextWindowPos({0, top_h});
    ImGui::SetNextWindowSize({left_w, h});
    ImGui::Begin("##games", nullptr, fixed);
    ImGui::TextUnformatted("Running games");
    ui::rule();
    if (link_games_.empty())
        ui::hint("None found. Start a game from a dev build: it lets the editor attach on this computer (nothing on the "
                 "network can).");
    for (const auto& g : link_games_) {
        ImGui::PushID(static_cast<int>(g.pid));
        bool attached = (link_.connected() || link_.connecting()) && link_game_ == g.game + "#" + std::to_string(g.pid);
        std::string label = std::format("{}  (pid {})", g.game, g.pid);
        if (ImGui::Selectable(label.c_str(), attached)) {
            if (attached) {
                link_.disconnect();
            } else if (link_.connect(g)) {
                link_game_ = g.game + "#" + std::to_string(g.pid);
                link_watching_.reset();
                link_docs_.clear();
                link_from_ = 0;
            }
        }
        ui::tip(attached ? "Attached: click to let go" : "Attach to this game");
        ImGui::PopID();
    }
    ui::rule();
    if (link_.connected()) ImGui::TextColored(accent(), "Attached to %s", link_.hello() ? link_.hello()->game.c_str() : "a game");
    else if (link_.connecting()) ImGui::TextUnformatted("Attaching...");
    else ImGui::TextColored(muted(), "Not attached");
    ui::rule();
    const char* panes[] = {"Profiler", "Systems", "Entities"};
    for (int i = 0; i < 3; ++i)
        if (ImGui::Selectable(panes[i], game_pane_ == i)) game_pane_ = i;
    ImGui::End();

    ImGui::SetNextWindowPos({left_w, top_h});
    ImGui::SetNextWindowSize({io.DisplaySize.x - left_w, h});
    ImGui::Begin("##gamepane", nullptr, fixed);
    if (game_pane_ == 0) game_profiler();
    else if (game_pane_ == 1) game_systems();
    else game_entities();
    ImGui::End();
}

void Editor::game_profiler() {
    const auto& frames = link_.frames();
    if (!link_.connected() || frames.empty()) {
        ui::hint("Attach to a running game on the left to see its frame times and where they go.");
        return;
    }
    const devlink::Frame& last = frames.back();
    // Time: pause, step and slow down the game from here.
    bool paused = last.paused;
    if (ImGui::Checkbox("Paused", &paused)) link_.time({paused, false, last.time_scale});
    ImGui::SameLine();
    ImGui::BeginDisabled(!last.paused);
    if (ImGui::Button("Step")) link_.time({true, true, last.time_scale});
    ImGui::EndDisabled();
    ui::tip("One fixed tick while paused");
    ImGui::SameLine();
    f32 scale = last.time_scale;
    ImGui::SetNextItemWidth(180);
    if (ImGui::SliderFloat("speed", &scale, 0.1f, 4.0f, "%.2fx", ImGuiSliderFlags_Logarithmic))
        link_.time({last.paused, false, scale});
    ui::rule();

    std::vector<f32> ms;
    for (const auto& f : frames) ms.push_back(f.ms);
    f32 avg = 0;
    for (f32 m : ms) avg += m;
    avg /= static_cast<f32>(ms.size());
    ImGui::Text("%.1f fps  %.2f ms of CPU a frame (over the last %.0f seconds)", avg > 0 ? 1000.0f / avg : 0.0f, avg,
                static_cast<f64>(ms.size()) * 0.1);
    ImGui::PlotLines("##frames", ms.data(), static_cast<int>(ms.size()), 0, nullptr, 0, 33.3f, {-1, 80});
    ImGui::Text("%u entities, %u archetypes | %u sprites, %u draws | %u voices", last.entities, last.archetypes,
                last.sprites, last.draw_calls, last.voices);
    ui::rule();

    // Zones, averaged over the last second so rows hold still.
    std::map<std::string, std::pair<f64, f64>> totals; // name: ms, calls
    usize n = std::min(profile_frames, frames.size());
    for (usize i = frames.size() - n; i < frames.size(); ++i)
        for (const auto& z : frames[i].zones) {
            auto& [t, c] = totals[z.name];
            t += z.ms;
            c += z.calls;
        }
    std::vector<std::pair<std::string, std::pair<f64, f64>>> rows(totals.begin(), totals.end());
    std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.second.first > b.second.first; });
    if (ImGui::BeginTable("zones", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY)) {
        ImGui::TableSetupColumn("zone");
        ImGui::TableSetupColumn("ms / frame", ImGuiTableColumnFlags_WidthFixed, 110);
        ImGui::TableSetupColumn("calls / frame", ImGuiTableColumnFlags_WidthFixed, 110);
        ImGui::TableHeadersRow();
        for (const auto& [name, v] : rows) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(name.c_str());
            ImGui::TableNextColumn();
            ImGui::Text("%.3f", v.first / static_cast<f64>(n));
            ImGui::TableNextColumn();
            ImGui::Text("%.1f", v.second / static_cast<f64>(n));
        }
        ImGui::EndTable();
    }
}

void Editor::game_systems() {
    const devlink::Schedules* s = link_.schedules() ? &*link_.schedules() : known_schedules_ ? &*known_schedules_ : nullptr;
    if (!s || (s->fixed.empty() && s->frame.empty())) {
        ui::hint("Attach to a running game to see its systems: what each reads and writes, which run side by side, and "
                 "why each batch starts. The editor remembers them for when no game is running.");
        return;
    }
    if (!link_.connected())
        ImGui::TextColored(muted(), "As of %s (%s); attach to a game to see it live and switch systems.",
                           known_schedules_when_.c_str(), link_game_.c_str());
    if (ImGui::BeginTabBar("schedules")) {
        if (ImGui::BeginTabItem("Fixed")) {
            game_schedule("fixed", s->fixed);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Frame")) {
            game_schedule("frame", s->frame);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

void Editor::game_schedule(const char* label, const std::vector<ecs::SystemInfo>& systems) {
    if (systems.empty()) {
        ui::hint("No systems in this schedule.");
        return;
    }
    // Live times from the profiler: each system runs inside a zone of its name.
    std::map<std::string, f64> live;
    if (link_.connected() && !link_.frames().empty())
        for (const auto& z : link_.frames().back().zones) live[z.name] = z.ms;
    ImGui::PushID(label);
    if (ImGui::BeginTable("systems", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
        ImGui::TableSetupColumn("on", ImGuiTableColumnFlags_WidthFixed, 30);
        ImGui::TableSetupColumn("batch", ImGuiTableColumnFlags_WidthFixed, 44);
        ImGui::TableSetupColumn("system");
        ImGui::TableSetupColumn("ms", ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupColumn("reads / writes");
        ImGui::TableHeadersRow();
        for (usize i = 0; i < systems.size(); ++i) {
            const auto& info = systems[i];
            ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            bool on = info.enabled;
            ImGui::BeginDisabled(!link_.connected());
            if (ImGui::Checkbox("##on", &on)) link_.set_system_enabled({label, info.name, on});
            ImGui::EndDisabled();
            ImGui::TableNextColumn();
            ImGui::Text("%u", info.batch);
            ImGui::TableNextColumn();
            if (info.why_new_batch.empty()) ImGui::TextUnformatted(info.name.c_str());
            else {
                ImGui::TextColored(accent(), "%s", info.name.c_str());
                ui::tip(std::format("Starts batch {}: {}", info.batch, info.why_new_batch));
            }
            if (info.exclusive) {
                ImGui::SameLine();
                ImGui::TextColored(muted(), "(exclusive)");
                ui::tip("Declares no reads or writes, so it runs alone: declare them and it can share a batch");
            }
            ImGui::TableNextColumn();
            auto t = live.find(info.name);
            ImGui::Text("%.3f", t != live.end() ? t->second : info.ms);
            ImGui::TableNextColumn();
            // Components, then resources (marked as such).
            auto both = [](const std::vector<std::string>& components, const std::vector<std::string>& resources) {
                std::vector<std::string> all = components;
                for (const auto& r : resources) all.push_back(r + " (resource)");
                return joined(all);
            };
            ImGui::TextWrapped("R %s\nW %s", both(info.reads, info.resource_reads).c_str(),
                               both(info.writes, info.resource_writes).c_str());
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    // The same as a table of components: what decides the batches.
    std::vector<std::string> components;
    for (const auto& info : systems)
        for (const auto* list : {&info.reads, &info.writes})
            for (const auto& c : *list)
                if (!has(components, c)) components.push_back(c);
    std::sort(components.begin(), components.end());
    if (!components.empty() && ImGui::CollapsingHeader("Access by component")) {
        if (ImGui::BeginTable("access", static_cast<int>(components.size()) + 1,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollX | ImGuiTableFlags_SizingFixedFit)) {
            ImGui::TableSetupColumn("system");
            for (const auto& c : components) ImGui::TableSetupColumn(c.c_str());
            ImGui::TableHeadersRow();
            for (const auto& info : systems) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(info.name.c_str());
                for (const auto& c : components) {
                    ImGui::TableNextColumn();
                    if (has(info.writes, c)) ImGui::TextColored(accent(), "W");
                    else if (has(info.reads, c)) ImGui::TextUnformatted("R");
                }
            }
            ImGui::EndTable();
        }
    }
    ImGui::PopID();
}

void Editor::game_entities() {
    if (!link_.connected()) {
        ui::hint("Attach to a running game to browse its entities and change their components while it runs.");
        return;
    }
    const ImGuiIO& io = ImGui::GetIO();
    float list_w = std::floor((io.DisplaySize.x - left_w) * 0.4f);
    bool refresh = !link_.page();
    ImGui::BeginChild("##entitylist", {list_w, 0}, ImGuiChildFlags_Borders);
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputTextWithHint("##filter", "name or component", link_filter_, sizeof link_filter_)) {
        link_from_ = 0;
        refresh = true;
    }
    if (const auto& page = link_.page()) {
        ImGui::BeginDisabled(link_from_ == 0);
        if (ImGui::SmallButton("< prev")) link_from_ -= std::min(link_from_, page_size), refresh = true;
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(link_from_ + page->rows.size() >= page->total);
        if (ImGui::SmallButton("next >")) link_from_ += static_cast<u32>(page->rows.size()), refresh = true;
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::SmallButton("refresh")) refresh = true;
        ImGui::SameLine();
        ImGui::TextColored(muted(), "%u-%zu of %u", page->total ? link_from_ + 1 : 0, link_from_ + page->rows.size(),
                           page->total);
        ui::rule();
        for (const auto& row : page->rows) {
            bool chosen = link_watching_ && link_watching_->first == row.index && link_watching_->second == row.generation;
            // Its name, or (most have none) what it's made of.
            std::string label = std::format("#{}  {}##{}.{}", row.index, row.name.empty() ? joined(row.components) : row.name,
                                            row.index, row.generation);
            if (ImGui::Selectable(label.c_str(), chosen)) {
                link_watching_ = {row.index, row.generation};
                link_docs_.clear();
                link_.watch(row.index, row.generation);
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", joined(row.components).c_str());
        }
    }
    ImGui::EndChild();
    if (refresh) link_.list_entities({link_from_, page_size, link_filter_});

    ImGui::SameLine();
    ImGui::BeginChild("##entitydetail", {0, 0}, ImGuiChildFlags_Borders);
    const auto& detail = link_.detail();
    if (!link_watching_) ui::hint("Pick an entity to see its components; values change in the game as you edit them.");
    else if (!detail) ImGui::TextUnformatted("...");
    else if (!detail->alive) ImGui::TextColored(muted(), "It's gone (destroyed in the game).");
    else {
        // While a value is being dragged or typed, the game's copy of it isn't taken back in, so it holds still.
        bool editing = ImGui::IsAnyItemActive();
        for (const auto& c : detail->components) {
            if (!ImGui::CollapsingHeader(c.name.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) continue;
            ImGui::PushID(c.name.c_str());
            if (!c.editable) {
                if (c.json.empty()) ImGui::TextColored(muted(), "(nothing to show: not savable, or too big)");
                else {
                    ImGui::TextColored(muted(), "Shown, not edited (a tag, or it points at other entities):");
                    ImGui::TextUnformatted(c.json.c_str());
                }
                ImGui::PopID();
                continue;
            }
            json::TextDocument& doc = link_docs_[c.name];
            if (!editing && doc.text() != c.json) doc.load(c.json);
            if (doc.valid()) {
                json::TextDocument::Path path;
                data_tree_fixed_ = true;
                data_tree(doc.value(), path, c.name);
                data_tree_fixed_ = false;
                bool changed = false;
                for (const DataEdit& e : data_edits_)
                    if (e.kind == DataEdit::Set) changed = doc.set(e.path, e.value) || changed;
                data_edits_.clear();
                if (changed) link_.set_component({detail->index, detail->generation, c.name, doc.text()});
            }
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
}

void Editor::game_access_note(Engine& engine, ecs::Entity e) {
    if (!known_schedules_ || (known_schedules_->fixed.empty() && known_schedules_->frame.empty())) return;
    std::vector<std::string> lines;
    for (ecs::ComponentId c : engine.world().components_of(e)) {
        std::string name(ecs::component_info(c).name);
        std::vector<std::string> readers, writers;
        for (const auto* list : {&known_schedules_->fixed, &known_schedules_->frame})
            for (const auto& info : *list) {
                if (has(info.writes, name)) writers.push_back(info.name);
                else if (has(info.reads, name)) readers.push_back(info.name);
            }
        if (readers.empty() && writers.empty()) continue;
        lines.push_back(std::format("{}: written by {}; read by {}", name, joined(writers), joined(readers)));
    }
    if (lines.empty()) return;
    if (ImGui::CollapsingHeader("In the game")) {
        ui::hint(std::format("The game's systems, as of {}:", known_schedules_when_));
        for (const auto& l : lines) ImGui::TextWrapped("%s", l.c_str());
    }
}

} // namespace bowl::editor
