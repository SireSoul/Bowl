// Everything the editor draws with ImGui: the top bar, the art shelf and outline on the left, the inspector on the
// right, the status line, and the dialogs.

#include "editor.hpp"

#include "bowl/app/inspector.hpp"
#include "bowl/core/time.hpp"
#include "bowl/gfx/sprite.hpp"

#include <SDL3/SDL_misc.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>
#include <map>

namespace bowl::editor {

namespace fs = std::filesystem;

namespace {

constexpr float top_h = 48, status_h = 28, left_w = 300, right_w = 340;
constexpr const char* entity_payload = "BOWL_ENTITY";
constexpr ImGuiWindowFlags fixed = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoBringToFrontOnFocus;

std::string display_name(ecs::World& world, ecs::Entity e) {
    if (const ecs::Name* n = world.try_get<const ecs::Name>(e); n && !n->value.empty()) return n->value;
    return std::format("Thing {}", e.index);
}

ImVec2 screen_center() {
    const ImGuiIO& io = ImGui::GetIO();
    return {io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f};
}

ImTextureID texture_id(const gfx::Texture& t) { return static_cast<ImTextureID>(reinterpret_cast<intptr_t>(t.handle())); }

// Components shown by their own cards, or kept out of sight because the editor manages them.
bool has_own_card(ecs::ComponentId c) {
    return c == ecs::component_id<ecs::Name>() || c == ecs::component_id<ecs::Transform2D>() ||
           c == ecs::component_id<ecs::GlobalTransform2D>() || c == ecs::component_id<ecs::Parent>() ||
           c == ecs::component_id<ecs::Children>() || c == ecs::component_id<gfx::Sprite>() ||
           c == ecs::component_id<RawComponents>();
}

void separator_v() {
    ImGui::SameLine(0, 10);
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddLine({p.x, p.y + 4}, {p.x, p.y + 26}, ui::color::line);
    ImGui::Dummy({1, 30});
    ImGui::SameLine(0, 10);
}

// A row of buttons where one is chosen.
bool segment(const char* label, bool on, const char* tip_text, float width = 0) {
    ImGui::PushStyleColor(ImGuiCol_Button, on ? ui::color::on : ui::color::button);
    ImGui::PushStyleColor(ImGuiCol_Text, on ? ui::color::accent : ui::color::text);
    bool pressed = ImGui::Button(label, {width, 0});
    ImGui::PopStyleColor(2);
    if (tip_text) { // "what it does|its key"
        std::string_view t = tip_text;
        usize bar = t.find('|');
        ui::tip(t.substr(0, bar), bar == std::string_view::npos ? std::string_view{} : t.substr(bar + 1));
    }
    return pressed;
}

} // namespace

void Editor::dev_ui(Engine& engine) {
    game_update(engine);
    content_.update(engine);
    const ImGuiIO& io = ImGui::GetIO();
    free_area_ = {left_w, top_h, std::max(1.0f, io.DisplaySize.x - left_w - right_w),
                  std::max(1.0f, io.DisplaySize.y - top_h - status_h)};
    top_bar(engine);
    if (workspace_ == Workspace::Level) {
        left_panel(engine);
        right_panel(engine);
        art_drop(engine);
    } else if (workspace_ == Workspace::Content) {
        content_.draw(engine, {0, top_h, io.DisplaySize.x, std::max(1.0f, io.DisplaySize.y - top_h - status_h)});
    } else if (workspace_ == Workspace::Data) {
        data_workspace(engine);
    } else if (workspace_ == Workspace::Game) {
        game_workspace(engine);
    } else {
        paint_.draw(engine, {0, top_h, io.DisplaySize.x, std::max(1.0f, io.DisplaySize.y - top_h - status_h)});
    }
    status_bar(engine);
    modals(engine);
}

// Top bar

void Editor::top_bar(Engine& engine) {
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos({0, 0});
    ImGui::SetNextWindowSize({io.DisplaySize.x, top_h});
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ui::color::bar);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {12, 9});
    ImGui::Begin("##top", nullptr, fixed | ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar();

    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(ui::color::accent), "Bowl");
    ImGui::SameLine(0, 12);
    // The three workspaces.
    if (segment("Level", workspace_ == Workspace::Level, "Lay out the level|Ctrl+1")) workspace_ = Workspace::Level;
    ImGui::SameLine(0, 2);
    if (segment("Content", workspace_ == Workspace::Content,
                "The game's items, people, shops, events and sounds, as forms|Ctrl+5"))
        workspace_ = Workspace::Content;
    ImGui::SameLine(0, 2);
    if (segment("Data", workspace_ == Workspace::Data, "The game's data files, edited in place|Ctrl+2")) workspace_ = Workspace::Data;
    ImGui::SameLine(0, 2);
    if (segment("Paint", workspace_ == Workspace::Paint, "Paint pixel art|Ctrl+3")) workspace_ = Workspace::Paint;
    ImGui::SameLine(0, 2);
    if (segment("Game", workspace_ == Workspace::Game, "Attach to a running game: its profiler, systems and entities|Ctrl+4"))
        workspace_ = Workspace::Game;
    if (workspace_ != Workspace::Level) {
        ImGui::SameLine(io.DisplaySize.x - 44);
        if (ui::tool_button("help", ui::Icon::Help, show_help_, "Every key and mouse action|F1")) show_help_ = !show_help_;
        ImGui::End();
        ImGui::PopStyleColor();
        return;
    }
    separator_v();
    ImGui::TextUnformatted(doc_->title().c_str());
    if (doc_->modified()) {
        ImGui::SameLine(0, 4);
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(ui::color::accent), "*");
        ui::tip("Unsaved changes", "Ctrl+S saves");
    }

    separator_v();
    if (ui::tool_button("new", ui::Icon::New, false, "New level|Ctrl+N")) new_level(engine);
    ImGui::SameLine(0, 4);
    if (ui::tool_button("open", ui::Icon::Open, false, "Open a level|Ctrl+O")) open_dialog(engine);
    ImGui::SameLine(0, 4);
    if (ui::tool_button("save", ui::Icon::Save, false, "Save|Ctrl+S (Ctrl+Shift+S: save as)")) save(engine, false);

    separator_v();
    if (ui::tool_button("select", ui::Icon::Select, tool_ == Tool::Select,
                        "Select and move: click a thing, drag to move it, drag on empty ground to select an area|V"))
        tool_ = Tool::Select;
    ImGui::SameLine(0, 4);
    if (ui::tool_button("place", ui::Icon::Place, tool_ == Tool::Place,
                        "Place: every click puts down the art picked on the left|P (Esc: back to select)"))
        tool_ = Tool::Place;
    ImGui::SameLine(0, 4);
    if (ui::tool_button("pan", ui::Icon::Pan, tool_ == Tool::Pan,
                        "Move the view: drag to look around (the right or middle button always does)|H, or hold Space"))
        tool_ = Tool::Pan;
    ImGui::SameLine(0, 4);
    if (ui::tool_button("brush", ui::Icon::Brush, tool_ == Tool::Brush,
                        "Brush: paint on a picture where it stands in the level (Paint workspace for tools and colors)|B"))
        tool_ = Tool::Brush;
    ImGui::SameLine(0, 4);
    if (ui::tool_button("region", ui::Icon::Region, tool_ == Tool::Region,
                        "Region: drag out an area the game can find by name (a trigger collider)|R"))
        tool_ = Tool::Region;

    separator_v();
    if (ui::tool_button("undo", ui::Icon::Undo, false, "Undo|Ctrl+Z", 30, doc_->can_undo())) undo(engine);
    ImGui::SameLine(0, 4);
    if (ui::tool_button("redo", ui::Icon::Redo, false, "Redo|Ctrl+Shift+Z or Ctrl+Y", 30, doc_->can_redo())) redo(engine);

    separator_v();
    if (ui::tool_button("grid", ui::Icon::Grid, show_grid_, "Show the grid|G")) show_grid_ = !show_grid_;
    ImGui::SameLine(0, 4);
    if (ui::tool_button("snap", ui::Icon::Snap, snap_,
                        "Snap to the grid when placing and moving (hold Alt for a moment of freedom)|Shift+G"))
        snap_ = !snap_;
    ImGui::SameLine(0, 6);
    ImGui::SetNextItemWidth(64);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 2);
    if (ImGui::DragFloat("##gridsize", &grid_, 0.5f, 1, 512, "%.0f px")) grid_ = std::max(grid_, 1.0f);
    ui::tip("Grid size in world units. Drag or double-click to type.");
    ImGui::SameLine(0, 6);
    if (ui::tool_button("colliders", ui::Icon::Eye, show_colliders_, "Show collision shapes and regions|C"))
        show_colliders_ = !show_colliders_;

    // Right end: help.
    ImGui::SameLine(io.DisplaySize.x - 44);
    if (ui::tool_button("help", ui::Icon::Help, show_help_, "Every key and mouse action|F1")) show_help_ = !show_help_;
    ImGui::End();
    ImGui::PopStyleColor();
}

// Left: art shelf and outline

void Editor::left_panel(Engine& engine) {
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos({0, top_h});
    ImGui::SetNextWindowSize({left_w, io.DisplaySize.y - top_h - status_h});
    ImGui::Begin("##left", nullptr, fixed);
    float half = (ImGui::GetContentRegionAvail().x - 8) * 0.5f;
    if (segment("Art", left_tab_ == 0, "Pictures in the assets folder: pick one, then click the level", half)) left_tab_ = 0;
    ImGui::SameLine(0, 8);
    if (segment("Outline", left_tab_ == 1, "Everything in the level, with what's inside what", half)) left_tab_ = 1;
    ImGui::Dummy({0, 2});
    if (left_tab_ == 0) art_shelf(engine);
    else outline(engine);
    ImGui::End();
}

bool Editor::thumbnail(Art& art, float size, bool chosen) {
    ImGui::PushID(art.path.c_str());
    ImVec2 pos = ImGui::GetCursorScreenPos();
    bool pressed = ImGui::InvisibleButton("##t", {size, size + 18});
    bool hovered = ImGui::IsItemHovered();
    if (ImGui::BeginDragDropSource()) {
        ImGui::SetDragDropPayload(inspector::asset_payload, art.path.data(), art.path.size());
        ImGui::Text("Place %s", art.name.c_str());
        ImGui::EndDragDropSource();
    }
    if (hovered) {
        std::string what = art.texture ? std::format("{}\n{} x {} px", art.path, art.texture->width(), art.texture->height())
                                       : art.path;
        const char* how = art.kind == Art::Kind::Map ? "A Tiled map. Click, then click the level where its corner goes."
                          : art.kind == Art::Kind::Prefab ? "A prefab: click, then click the level to place a copy."
                                                          : "Click, then click the level. Or drag it in.";
        ui::tip(what, how);
    }
    ImDrawList* l = ImGui::GetWindowDrawList();
    ImU32 bg = chosen ? ui::color::on : hovered ? ui::color::hover : ui::color::field;
    l->AddRectFilled(pos, {pos.x + size, pos.y + size}, bg, 6);
    if (chosen) l->AddRect(pos, {pos.x + size, pos.y + size}, ui::color::accent, 6, 0, 1.5f);
    if (art.kind != Art::Kind::Picture) { // no picture to show: say what it is
        const char* kind = art.kind == Art::Kind::Map ? "MAP" : "PREFAB";
        ImVec2 ts = ImGui::CalcTextSize(kind);
        l->AddText({pos.x + (size - ts.x) * 0.5f, pos.y + (size - ts.y) * 0.5f}, ui::color::accent, kind);
    } else if (ImGui::IsItemVisible() && !art.texture) {
        art.texture = assets_->load<gfx::Texture>(art.path);
    }
    if (art.kind == Art::Kind::Picture && art.texture) {
        // Fit inside the square with a margin, keeping its shape.
        Vec2 s = art.texture->size();
        float fit = (size - 10) / std::max(s.x, s.y);
        fit = std::min(fit, 4.0f); // tiny pixel art shouldn't balloon
        ImVec2 half{s.x * fit * 0.5f, s.y * fit * 0.5f}, c{pos.x + size * 0.5f, pos.y + size * 0.5f};
        l->AddImage(texture_id(*art.texture), {c.x - half.x, c.y - half.y}, {c.x + half.x, c.y + half.y});
    }
    std::string label = art.name;
    if (ImGui::CalcTextSize(label.c_str()).x > size) {
        while (label.size() > 1 && ImGui::CalcTextSize((label + "...").c_str()).x > size) label.pop_back();
        label += "...";
    }
    float tw = ImGui::CalcTextSize(label.c_str()).x;
    l->AddText({pos.x + (size - tw) * 0.5f, pos.y + size + 2}, chosen ? ui::color::accent : ui::color::muted, label.c_str());
    ImGui::PopID();
    return pressed;
}

void Editor::art_shelf(Engine&) {
    float w = ImGui::GetContentRegionAvail().x;
    ImGui::SetNextItemWidth(w - 72);
    ImGui::InputTextWithHint("##find", "Find art", art_filter_, sizeof art_filter_);
    ImGui::SameLine(0, 6);
    if (ui::tool_button("reload", ui::Icon::Reload, false, "Look for new or changed pictures|F5", 28)) {
        scan_art();
        say("Looked for new art");
    }
    ImGui::SameLine(0, 4);
    if (ui::tool_button("folder", ui::Icon::Folder, false, "Open the assets folder", 28))
        SDL_OpenURL(("file://" + assets_dir_.generic_string()).c_str());

    // An empty thing first: a position with nothing drawn (spawn points, triggers, groups).
    ImGui::Dummy({0, 2});
    bool empty_on = tool_ == Tool::Place && place_art_.empty();
    if (segment("+ Empty thing", empty_on,
                "Something with a position and nothing drawn: spawn points, markers, groups. Click, then click the "
                "level.",
                w)) {
        tool_ = Tool::Place;
        place_art_.clear();
    }
    ImGui::Dummy({0, 4});

    ImGui::BeginChild("art", {0, 0});
    if (art_.empty()) {
        ui::hint(std::format("No pictures in {} yet. Put PNGs there (subfolders are fine), then press F5.",
                             assets_dir_.filename().string()));
    }
    std::string_view filter = art_filter_;
    const float cell = 82;
    int columns = std::max(1, static_cast<int>((ImGui::GetContentRegionAvail().x + 8) / (cell + 8)));
    std::string folder = "\x01"; // differs from every real folder
    bool folder_open = true;
    int column = 0;
    for (Art& art : art_) {
        if (!filter.empty() && art.path.find(filter) == std::string::npos) continue;
        if (art.folder != folder) {
            folder = art.folder;
            column = 0;
            if (!folder.empty())
                folder_open = ImGui::CollapsingHeader(folder.c_str(), ImGuiTreeNodeFlags_DefaultOpen);
            else folder_open = true;
        }
        if (!folder_open) continue;
        if (column > 0) ImGui::SameLine(0, 8);
        bool chosen = tool_ == Tool::Place && place_art_ == art.path;
        if (thumbnail(art, cell, chosen)) {
            tool_ = Tool::Place;
            place_art_ = art.path;
        }
        column = (column + 1) % columns;
    }
    ImGui::EndChild();
}

void Editor::outline(Engine& engine) {
    ecs::World& world = engine.world();
    ImGui::BeginChild("outline");
    if (doc_->entities().empty()) ui::hint("The level is empty. Pick art on the Art tab and click the level to place it.");
    for (ecs::Entity e : doc_->entities()) {
        const ecs::Parent* parent = world.try_get<const ecs::Parent>(e);
        if (!parent || !world.alive(parent->entity)) outline_node(engine, e);
    }
    // Dropping something on the empty space below takes it out of whatever it's in.
    ImVec2 rest = ImGui::GetContentRegionAvail();
    ImGui::InvisibleButton("##out", {std::max(rest.x, 1.0f), std::max(rest.y, 40.0f)});
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(entity_payload))
            reparent(engine, ecs::Entity::from_bits(*static_cast<const u64*>(p->Data)), ecs::null_entity);
        ImGui::EndDragDropTarget();
    }
    if (ImGui::IsItemClicked()) selection_.clear();
    ImGui::EndChild();
}

void Editor::outline_node(Engine& engine, ecs::Entity e) {
    ecs::World& world = engine.world();
    std::vector<ecs::Entity> children;
    if (const ecs::Children* c = world.try_get<const ecs::Children>(e)) children = c->list;
    std::erase_if(children, [&](ecs::Entity c) { return !world.alive(c); });

    ImGui::PushID(static_cast<int>(e.index)); // live entities never share an index
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth |
                               ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_FramePadding;
    if (selected(e)) flags |= ImGuiTreeNodeFlags_Selected;
    if (children.empty()) flags |= ImGuiTreeNodeFlags_Leaf;
    ImVec2 row = ImGui::GetCursorScreenPos();
    bool open = ImGui::TreeNodeEx("##node", flags, "      %s", display_name(world, e).c_str());
    float indent = ImGui::GetTreeNodeToLabelSpacing();
    ui::draw_icon(ImGui::GetWindowDrawList(),
                  world.has<gfx::Sprite>(e) || world.has<gfx::Map>(e) ? ui::Icon::Image : ui::Icon::Entity,
                  {row.x + indent + 8, row.y + ImGui::GetFrameHeight() * 0.5f}, 13, ui::color::muted);
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
        if (ImGui::GetIO().KeyShift || ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeySuper) toggle_selected(e);
        else select_only(e);
    }
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        select_only(e);
        frame(engine, true);
    }
    if (ImGui::BeginDragDropSource()) {
        u64 bits = e.bits();
        ImGui::SetDragDropPayload(entity_payload, &bits, sizeof bits);
        ImGui::Text("Put %s inside...", display_name(world, e).c_str());
        ImGui::EndDragDropSource();
    }
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(entity_payload))
            reparent(engine, ecs::Entity::from_bits(*static_cast<const u64*>(p->Data)), e);
        ImGui::EndDragDropTarget();
    }
    if (ImGui::BeginPopupContextItem()) {
        if (!selected(e)) select_only(e);
        if (ImGui::MenuItem("Duplicate", "Ctrl+D")) duplicate(engine);
        if (ImGui::MenuItem("Group", "Ctrl+G")) group_selection(engine);
        if (!children.empty() && ImGui::MenuItem("Ungroup", "Ctrl+Shift+G")) ungroup_selection(engine);
        if (ImGui::MenuItem("Show in view", "F")) frame(engine, true);
        ImGui::Separator();
        const EditorFlags* f = world.try_get<const EditorFlags>(e);
        bool is_hidden = f && f->hidden, is_locked = f && f->locked;
        if (ImGui::MenuItem(is_hidden ? "Show in the editor" : "Hide in the editor")) set_hidden(engine, !is_hidden);
        if (ImGui::MenuItem(is_locked ? "Unlock" : "Lock")) set_locked(engine, !is_locked);
        ImGui::Separator();
        if (ImGui::MenuItem("Delete", "Del")) remove_selection(engine);
        ImGui::EndPopup();
    }
    // Hide (in the editor only: the game still shows it) and lock (can't be clicked in the scene).
    {
        EditorFlags state;
        if (const EditorFlags* f = world.try_get<const EditorFlags>(e)) state = *f;
        auto toggle = [&](bool EditorFlags::*field, bool on) {
            EditorFlags* f = world.try_get<EditorFlags>(e);
            if (!f) f = &world.add<EditorFlags>(e);
            f->*field = on;
            if (field == &EditorFlags::hidden && on) std::erase(selection_, e);
        };
        ImDrawList* l = ImGui::GetWindowDrawList();
        ImGui::SameLine(ImGui::GetWindowWidth() - 60);
        if (ui::tool_button("lock", ui::Icon::Lock, state.locked,
                            state.locked ? "Locked: clicks in the scene go past it. Click to unlock"
                                         : "Lock, so clicks in the scene go past it",
                            20))
            toggle(&EditorFlags::locked, !state.locked);
        ImGui::SameLine(0, 4);
        if (ui::tool_button("vis", ui::Icon::Eye, false,
                            state.hidden ? "Hidden in the editor (the game still shows it). Click to show"
                                         : "Hide in the editor (the game still shows it)",
                            20))
            toggle(&EditorFlags::hidden, !state.hidden);
        if (state.hidden) {
            ImVec2 p = ImGui::GetItemRectMin(), q = ImGui::GetItemRectMax();
            l->AddLine({p.x + 3, q.y - 3}, {q.x - 3, p.y + 3}, ui::color::danger, 1.5f);
        }
    }
    if (open) {
        for (ecs::Entity c : children) outline_node(engine, c);
        ImGui::TreePop();
    }
    ImGui::PopID();
}

// Right: the inspector

void Editor::right_panel(Engine& engine) {
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos({io.DisplaySize.x - right_w, top_h});
    ImGui::SetNextWindowSize({right_w, io.DisplaySize.y - top_h - status_h});
    ImGui::Begin("##right", nullptr, fixed);
    if (selection_.empty()) inspect_level(engine);
    else if (selection_.size() == 1) inspect_one(engine, selection_[0]);
    else inspect_many(engine);
    ImGui::End();
}

void Editor::inspect_level(Engine& engine) {
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(ui::color::accent), "Level");
    ImGui::TextUnformatted(doc_->file().empty() ? "Not saved yet" : doc_->file().filename().string().c_str());
    ui::hint(std::format("{} things. Art from {}.", doc_->entities().size(), assets_dir_.string()));
    ui::rule();
    ui::hint("Pick art on the left, then click the level to place it. Drag things to move them; hold Alt to place "
             "freely, or Alt-drag to copy.");
    ui::hint("Drag on empty ground to select an area. The right or middle button moves the view, the wheel zooms.");
    ui::hint("F1 lists every key.");
    (void)engine;
}

void Editor::inspect_many(Engine& engine) {
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(ui::color::accent), "%zu things", selection_.size());
    ui::rule();
    float w = (ImGui::GetContentRegionAvail().x - 16) / 3;
    if (ImGui::Button("Group", {w, 0})) group_selection(engine);
    ui::tip("Put them inside a new group, so they move together", "Ctrl+G");
    ImGui::SameLine(0, 8);
    if (ImGui::Button("Duplicate", {w, 0})) duplicate(engine);
    ui::tip("Copies, a grid step away", "Ctrl+D (or Alt-drag)");
    ImGui::SameLine(0, 8);
    ImGui::PushStyleColor(ImGuiCol_Text, ui::color::danger);
    if (ImGui::Button("Delete", {w, 0})) remove_selection(engine);
    ImGui::PopStyleColor();
    ui::tip("Remove them, and anything inside them", "Del");

    ImGui::Dummy({0, 6});
    ImGui::TextDisabled("Line up");
    const struct {
        ui::Icon icon;
        const char* tip;
    } aligns[] = {{ui::Icon::AlignLeft, "Left edges"},   {ui::Icon::AlignCenterX, "Centers, across"},
                  {ui::Icon::AlignRight, "Right edges"}, {ui::Icon::AlignTop, "Tops"},
                  {ui::Icon::AlignCenterY, "Centers, up and down"}, {ui::Icon::AlignBottom, "Bottoms (feet)"}};
    for (int i = 0; i < 6; ++i) {
        if (i) ImGui::SameLine(0, 4);
        if (ui::tool_button(aligns[i].tip, aligns[i].icon, false, aligns[i].tip, 32)) align(engine, i);
    }
    ImGui::Dummy({0, 6});
    ui::hint("Shift-click adds or removes things. Arrows nudge (Shift: a grid step), Q and E turn.");
}

void Editor::inspect_one(Engine& engine, ecs::Entity e) {
    ecs::World& world = engine.world();
    // The name, big, at the top.
    char name[256] = {};
    if (const ecs::Name* n = world.try_get<const ecs::Name>(e)) std::snprintf(name, sizeof name, "%s", n->value.c_str());
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputTextWithHint("##name", "Name", name, sizeof name)) world.add<ecs::Name>(e, std::string(name));
    ui::tip("What it's called here and in the outline. Games can look things up by name.");

    if (const ecs::Parent* p = world.try_get<const ecs::Parent>(e); p && world.alive(p->entity)) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("Inside");
        ImGui::SameLine();
        if (ImGui::SmallButton(display_name(world, p->entity).c_str())) select_only(p->entity);
        ui::tip("Select the group it's in");
        ImGui::SameLine();
        if (ImGui::SmallButton("Take out")) reparent(engine, e, ecs::null_entity);
        ui::tip("Move it out of the group, keeping where it is");
    }
    ImGui::Dummy({0, 2});

    if (world.has<ecs::Transform2D>(e)) transform_card(engine, e);
    if (world.has<gfx::Sprite>(e)) art_card(engine, e);

    std::vector<ecs::ComponentId> ids(world.components_of(e).begin(), world.components_of(e).end());
    for (ecs::ComponentId c : ids) {
        if (has_own_card(c)) continue;
        const ecs::ComponentInfo& info = ecs::component_info(c);
        std::string title = pretty_name(info.name);
        bool remove = false;
        std::string id = std::format("c{}", c);
        if (ui::begin_card(title.c_str(), id.c_str(), ui::Icon::Cross, &remove, "Remove")) {
            if (info.visit) inspector::fields(world, e, c, engine.assets(), nullptr);
            else if (info.size == 0) ui::hint("A tag: having it is all it says.");
            else ui::hint(std::format("{} has no BOWL_COMPONENT field list, so it can't be edited here.", info.name));
            ui::end_card();
        }
        if (ImGui::IsItemHovered()) ui::tip(info.name);
        if (remove) world.remove_component(e, c);
    }
    raw_cards(engine, e);
    add_component_button(engine, e);
    game_access_note(engine, e);
}

void Editor::transform_card(Engine& engine, ecs::Entity e) {
    ecs::Transform2D& t = engine.world().get<ecs::Transform2D>(e);
    if (!ui::begin_card("Place", "transform")) return;
    ImGui::SetNextItemWidth(-60);
    ImGui::DragFloat2("Position", &t.position.x, 1.0f, 0, 0, "%.1f");
    ui::tip("Where it stands (relative to its group, if it's in one). Drag, or double-click to type.");
    f32 degrees = bowl::degrees(t.rotation);
    ImGui::SetNextItemWidth(-60);
    if (ImGui::DragFloat("Turn", &degrees, 0.5f, 0, 0, "%.1f deg")) t.rotation = radians(degrees);
    ui::tip("Rotation in degrees", "Q / E: 15 degrees (Shift: 1)");
    ImGui::SetNextItemWidth(-60);
    ImGui::DragFloat2("Scale", &t.scale.x, 0.01f, 0, 0, "%.2f");
    ui::tip("1 is its natural size. A negative value mirrors it.");
    if (ImGui::SmallButton("Mirror")) t.scale.x = -t.scale.x;
    ui::tip("Flip it left to right");
    ImGui::SameLine();
    if (ImGui::SmallButton("Reset")) {
        t.rotation = 0;
        t.scale = {1, 1};
    }
    ui::tip("No rotation, natural size");
    ui::end_card();
}

void Editor::art_card(Engine& engine, ecs::Entity e) {
    ecs::World& world = engine.world();
    bool remove = false;
    if (!ui::begin_card("Art", "art", ui::Icon::Cross, &remove, "Remove the art (the thing stays)")) {
        if (remove) world.remove<gfx::Sprite>(e);
        return;
    }
    gfx::Sprite& s = world.get<gfx::Sprite>(e);
    // A preview, with the picture's path and a button to change it.
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::Dummy({64, 64});
    ImDrawList* l = ImGui::GetWindowDrawList();
    l->AddRectFilled(pos, {pos.x + 64, pos.y + 64}, ui::color::field, 6);
    if (s.texture) {
        Vec2 size = s.texture->size();
        float fit = 56 / std::max(size.x, size.y);
        ImVec2 half{size.x * fit * 0.5f, size.y * fit * 0.5f}, c{pos.x + 32, pos.y + 32};
        l->AddImage(texture_id(*s.texture), {c.x - half.x, c.y - half.y}, {c.x + half.x, c.y + half.y});
    } else {
        ImU32 col = ImGui::ColorConvertFloat4ToU32({s.color.r, s.color.g, s.color.b, s.color.a});
        l->AddRectFilled({pos.x + 14, pos.y + 14}, {pos.x + 50, pos.y + 50}, col, 3);
    }
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::TextUnformatted(s.texture ? std::string(s.texture.path()).c_str() : "A plain rectangle");
    if (s.texture) ImGui::TextDisabled("%u x %u px", s.texture->width(), s.texture->height());
    if (ImGui::SmallButton("Change...")) ImGui::OpenPopup("pick art");
    ui::tip("Choose another picture (or drop one from the shelf onto the preview)");
    if (s.texture) {
        ImGui::SameLine();
        if (ImGui::SmallButton("None")) s.texture = {};
        ui::tip("Draw a plain rectangle in its color instead");
    }
    ImGui::EndGroup();
    // Art dropped from the shelf onto the card replaces the picture.
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(inspector::asset_payload))
            s.texture = engine.assets().load<gfx::Texture>(
                std::string(static_cast<const char*>(p->Data), static_cast<usize>(p->DataSize)));
        ImGui::EndDragDropTarget();
    }
    art_picker(engine, e);

    ImGui::ColorEdit4("Color", &s.color.r, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar);
    ui::tip("Tints the picture (white keeps its own colors). Values above 1 glow when the game uses bloom.");
    ImGui::SameLine();
    ImGui::Checkbox("Shown", &s.visible);
    ui::tip("Hidden things are still in the level, just not drawn");

    bool natural = s.size.x == 0 && s.size.y == 0;
    if (ImGui::Checkbox("Natural size", &natural)) s.size = natural ? Vec2{} : gfx::sprite_size(s);
    ui::tip("Draw the picture at its own pixel size (scale it with Place > Scale)");
    if (!natural) {
        ImGui::SetNextItemWidth(-60);
        ImGui::DragFloat2("Size", &s.size.x, 0.5f, 0, 0, "%.0f");
    }

    ImGui::TextDisabled("Stands on");
    ImGui::SameLine();
    struct Pivot {
        const char* label;
        Vec2 at;
        const char* tip;
    };
    const Pivot pivots[] = {{"Feet", {0.5f, 1}, "The middle of its bottom edge: right for anything standing up"},
                            {"Center", {0.5f, 0.5f}, "Its middle: for things seen from above, or floating"},
                            {"Top-left", {0, 0}, "Its top-left corner: for tiles and backgrounds"}};
    for (const Pivot& p : pivots) {
        if (segment(p.label, s.origin == p.at, p.tip)) s.origin = p.at;
        ImGui::SameLine(0, 4);
    }
    ImGui::NewLine();

    ImGui::Checkbox("Sorts by height", &s.y_sort);
    ui::tip("Things lower on screen draw in front: right for top-down and isometric levels. Off, only the draw "
            "order below decides.");
    ImGui::SetNextItemWidth(-60);
    ImGui::DragFloat("Order", &s.depth, 0.1f, 0, 0, "%.1f");
    ui::tip("Added to its draw order: higher draws in front. Give the ground a large negative number.");
    ui::end_card();
    if (remove) world.remove<gfx::Sprite>(e);
}

void Editor::art_picker(Engine& engine, ecs::Entity e) {
    ImGui::SetNextWindowSize({460, 420}, ImGuiCond_Appearing);
    if (!ImGui::BeginPopup("pick art")) return;
    static char filter[128] = {};
    ImGui::SetNextItemWidth(-1);
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::InputTextWithHint("##f", "Find art", filter, sizeof filter);
    ImGui::BeginChild("grid");
    int column = 0;
    int columns = std::max(1, static_cast<int>(ImGui::GetContentRegionAvail().x / 90));
    for (Art& art : art_) {
        if (filter[0] && art.path.find(filter) == std::string::npos) continue;
        if (column) ImGui::SameLine(0, 8);
        if (thumbnail(art, 82, false)) {
            engine.world().get<gfx::Sprite>(e).texture = engine.assets().load<gfx::Texture>(art.path);
            ImGui::CloseCurrentPopup();
        }
        column = (column + 1) % columns;
    }
    ImGui::EndChild();
    ImGui::EndPopup();
}

void Editor::raw_cards(Engine& engine, ecs::Entity e) {
    RawComponents* raw = engine.world().try_get<RawComponents>(e);
    if (!raw) return;
    static std::map<std::string, std::string> buffers; // text being edited, per entity and component
    for (usize i = 0; i < raw->components.size(); ++i) {
        auto& [name, value] = raw->components[i];
        std::string title = pretty_name(name) + "  (not loaded)";
        std::string id = std::format("raw{}", i);
        bool remove = false;
        if (!ui::begin_card(title.c_str(), id.c_str(), ui::Icon::Cross, &remove, "Remove")) {
            if (remove) raw->components.erase(raw->components.begin() + static_cast<std::ptrdiff_t>(i));
            break;
        }
        ui::hint(std::format("{} is the game's own type. Start the editor with --game to edit it with proper "
                             "fields; until then it's kept as JSON.",
                             name));
        std::string key = std::format("{}/{}", e.bits(), name);
        auto [it, fresh] = buffers.try_emplace(key, json::stringify(value));
        std::string& text = it->second;
        if (text.size() < 4096) text.resize(4096); // room to type
        ImGui::InputTextMultiline("##json", text.data(), text.size(), {-1, 120});
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            auto parsed = json::parse(text.c_str());
            if (parsed) value = std::move(parsed.value);
            else say(std::format("{} isn't valid JSON: {}", name, parsed.error->message));
        }
        if (!ImGui::IsItemActive()) it->second = json::stringify(value);
        ui::end_card();
        if (remove) {
            raw->components.erase(raw->components.begin() + static_cast<std::ptrdiff_t>(i));
            break;
        }
    }
}

void Editor::add_component_button(Engine& engine, ecs::Entity e) {
    ecs::World& world = engine.world();
    ImGui::Dummy({0, 4});
    if (ImGui::Button("+ Add", {-1, 0})) {
        add_filter_[0] = 0;
        ImGui::OpenPopup("add component");
    }
    ui::tip("Give it more: art, particles, or any of the game's components");
    ImGui::SetNextWindowSize({300, 320}, ImGuiCond_Appearing);
    if (!ImGui::BeginPopup("add component")) return;
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##f", "Find", add_filter_, sizeof add_filter_);
    std::vector<std::pair<std::string, ecs::ComponentId>> choices;
    for (usize i = 0; i < ecs::component_type_count(); ++i) {
        auto c = static_cast<ecs::ComponentId>(i);
        const ecs::ComponentInfo& info = ecs::component_info(c);
        if (!info.savable() || !info.construct_default || world.components_of(e).end() !=
            std::find(world.components_of(e).begin(), world.components_of(e).end(), c))
            continue;
        if (c == ecs::component_id<ecs::GlobalTransform2D>() || c == ecs::component_id<ecs::Children>() ||
            c == ecs::component_id<ecs::Parent>())
            continue;
        std::string label = pretty_name(info.name);
        std::string lower = label + " " + std::string(info.name);
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        std::string want = add_filter_;
        std::transform(want.begin(), want.end(), want.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        if (!want.empty() && lower.find(want) == std::string::npos) continue;
        choices.emplace_back(label, c);
    }
    std::sort(choices.begin(), choices.end());
    for (const auto& [label, c] : choices) {
        if (ImGui::Selectable(label.c_str())) {
            world.add_component(e, c, [](void*) {});
            if (c == ecs::component_id<gfx::Sprite>()) world.get<gfx::Sprite>(e).origin = {0.5f, 1.0f};
            ImGui::CloseCurrentPopup();
        }
        ui::tip(ecs::component_info(c).name);
    }
    if (choices.empty()) ui::hint("Nothing to add. The game's components appear here when the editor starts with --game.");
    ImGui::EndPopup();
}

// Status line

void Editor::status_bar(Engine& engine) {
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos({0, io.DisplaySize.y - status_h});
    ImGui::SetNextWindowSize({io.DisplaySize.x, status_h});
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ui::color::bar);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {12, 5});
    ImGui::Begin("##status", nullptr, fixed | ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar();

    // What a click would do right now.
    ecs::World& world = engine.world();
    std::string what;
    if (workspace_ == Workspace::Content)
        what = "Pick a collection and one of its records on the left, then change it on the right. Ctrl+S saves, Ctrl+Z undoes.";
    else if (workspace_ == Workspace::Data)
        what = "Change values on the left or the text on the right; comments and layout stay. Ctrl+S saves, Ctrl+Z undoes.";
    else if (workspace_ == Workspace::Game)
        what = link_.connected() ? "Attached: changes here happen in the running game." : "Pick a running game on the left to attach.";
    else if (workspace_ == Workspace::Paint)
        what = "Left button: the first color, right: the second. Alt picks a color. Space-drag or the middle button moves, the wheel zooms. Ctrl+S saves.";
    else if (tool_ == Tool::Brush) what = "Paint on a picture where it stands. Colors and tools are in the Paint workspace.";
    else if (tool_ == Tool::Region) what = "Drag out a region (a click makes a square one). Esc: back to selecting.";
    else if (tool_ == Tool::Place)
        what = place_art_.empty() ? "Click to put down an empty thing. Alt: off the grid. Esc: done."
                                  : std::format("Click to place {}. Alt: off the grid. Esc: done.", fs::path(place_art_).stem().string());
    else if (tool_ == Tool::Pan) what = "Drag to look around. Esc: back to selecting.";
    else if (drag_ == Drag::Move) what = "Moving. Hold Alt for off-grid positions.";
    else if (drag_ == Drag::Box) what = "Selecting everything in the box.";
    else if (hovered_ && world.alive(hovered_))
        what = std::format("{}: click to select, drag to move, Alt-drag to copy, Shift-click to add.",
                           display_name(world, hovered_));
    else what = "Drag to select an area. Right or middle drag moves the view, the wheel zooms.";
    ImGui::TextDisabled("%s", what.c_str());

    Vec2 cursor = mouse_world(engine);
    std::string right = workspace_ == Workspace::Level
                            ? std::format("{:.0f}%   {:.0f}, {:.0f}", engine.camera().zoom * 100, cursor.x, cursor.y)
                            : std::string();
    f64 age = now_seconds() - message_time_;
    float right_w_text = ImGui::CalcTextSize(right.c_str()).x;
    if (age < 4 && !message_.empty()) {
        float mw = ImGui::CalcTextSize(message_.c_str()).x;
        ImGui::SameLine(io.DisplaySize.x - right_w_text - mw - 48);
        ImVec4 c = ImGui::ColorConvertU32ToFloat4(ui::color::accent);
        c.w = age < 3 ? 1.0f : static_cast<float>(4 - age);
        ImGui::TextColored(c, "%s", message_.c_str());
    }
    ImGui::SameLine(io.DisplaySize.x - right_w_text - 16);
    ImGui::TextDisabled("%s", right.c_str());
    ImGui::End();
    ImGui::PopStyleColor();
}

void Editor::art_drop(Engine& engine) {
    const ImGuiPayload* p = ImGui::GetDragDropPayload();
    if (!p || !p->IsDataType(inspector::asset_payload)) return;
    if (!ImGui::IsMouseReleased(ImGuiMouseButton_Left) || ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow)) return;
    std::string path(static_cast<const char*>(p->Data), static_cast<usize>(p->DataSize));
    bool alt = ImGui::GetIO().KeyAlt;
    create(engine, snapped(mouse_world(engine), alt), path);
}

// Dialogs

void Editor::modals(Engine& engine) {
    if (save_prompt_open_) {
        ImGui::OpenPopup("Unsaved changes");
        save_prompt_open_ = false;
    }
    ImGui::SetNextWindowPos(screen_center(), ImGuiCond_Appearing, {0.5f, 0.5f});
    if (ImGui::BeginPopupModal("Unsaved changes", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Save the changes to %s first?", doc_->title().c_str());
        ImGui::Dummy({0, 6});
        if (ImGui::Button("Save", {110, 0})) {
            bool untitled = doc_->file().empty();
            save(engine, false);
            if (!untitled && after_save_prompt_) after_save_prompt_(engine);
            after_save_prompt_ = {};
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Don't save", {110, 0})) {
            doc_->discard_autosave();
            if (after_save_prompt_) after_save_prompt_(engine);
            after_save_prompt_ = {};
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", {110, 0}) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            after_save_prompt_ = {};
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (recovery_) ImGui::OpenPopup("Unsaved work");
    ImGui::SetNextWindowPos(screen_center(), ImGuiCond_Appearing, {0.5f, 0.5f});
    if (ImGui::BeginPopupModal("Unsaved work", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("%s has changes that weren't saved when the editor last closed.", doc_->title().c_str());
        ImGui::TextDisabled("Bring them back? They stay unsaved until you save.");
        ImGui::Dummy({0, 6});
        if (ImGui::Button("Bring them back", {150, 0})) {
            doc_->recover(*recovery_);
            selection_.clear();
            recovery_.reset();
            say("Brought back the unsaved work");
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Throw them away", {150, 0})) {
            doc_->discard_autosave();
            recovery_.reset();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (show_help_) help_window();
    if (show_welcome_) welcome_window(engine);
}

void Editor::help_window() {
    ImGui::SetNextWindowPos(screen_center(), ImGuiCond_Appearing, {0.5f, 0.5f});
    ImGui::SetNextWindowSize({640, 0}, ImGuiCond_Appearing);
    ImGui::Begin("Keys and mouse", &show_help_, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings);
    static const std::pair<const char*, const char*> rows[] = {
        {"Click", "Select (with Place: put down the picked art)"},
        {"Drag a thing", "Move it (and everything selected)"},
        {"Alt-drag", "Move copies, leaving the originals"},
        {"Drag on empty ground", "Select everything in the box"},
        {"Shift-click", "Add to or take from the selection"},
        {"Right or middle drag, Space-drag", "Move the view"},
        {"Wheel", "Zoom toward the mouse"},
        {"Hold Alt", "Ignore the grid for now"},
        {"V / P / H", "Select, Place, Move the view"},
        {"B / R", "Brush (paint on art in the level), Region (drag out an area)"},
        {"C", "Show or hide collision shapes and regions"},
        {"Ctrl+1 / 2 / 3 / 4 / 5", "Level, Data, Paint, Game and Content workspaces"},
        {"G / Shift+G", "Show the grid / snap to it"},
        {"Arrows (Shift)", "Nudge one unit (a grid step)"},
        {"Q / E (Shift)", "Turn 15 degrees (1 degree)"},
        {"Ctrl+D, Ctrl+C, Ctrl+X, Ctrl+V", "Duplicate, copy, cut, paste at the mouse"},
        {"Ctrl+G / Ctrl+Shift+G", "Group / ungroup"},
        {"Delete or Backspace", "Remove (with what's inside)"},
        {"Ctrl+A, Esc", "Select everything, select nothing (or stop placing)"},
        {"F / Home", "Show the selection / the whole level"},
        {"Ctrl+Z, Ctrl+Shift+Z or Ctrl+Y", "Undo, redo"},
        {"Ctrl+N, Ctrl+O, Ctrl+S, Ctrl+Shift+S", "New, open, save, save as"},
        {"F5", "Look for new or changed art"},
        {"F12", "The engine's overlay: profiler, systems, log"},
    };
    if (ImGui::BeginTable("keys", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        for (const auto& [keys, what] : rows) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(ui::color::accent), "%s", keys);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(what);
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

void Editor::welcome_window(Engine& engine) {
    ImGui::OpenPopup("Welcome to Bowl Editor");
    ImGui::SetNextWindowPos(screen_center(), ImGuiCond_Appearing, {0.5f, 0.5f});
    ImGui::SetNextWindowSize({520, 0}, ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Welcome to Bowl Editor", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    ImGui::PushTextWrapPos(500);
    ImGui::TextUnformatted("Levels are made of things: pictures, and markers with nothing drawn. The game loads the "
                           ".world files saved here.");
    ImGui::Dummy({0, 4});
    static const char* steps[] = {
        "Pick art on the left, then click the level to place it. Every click places another; Esc stops.",
        "Click a thing to select it, drag to move it. Its details are on the right.",
        "Drag on empty ground to select many. Group them (Ctrl+G) to move them together.",
        "Right-drag moves the view; the wheel zooms. Ctrl+S saves; a running game picks the level up by itself.",
    };
    for (int i = 0; i < 4; ++i) {
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(ui::color::accent), "%d", i + 1);
        ImGui::SameLine();
        ImGui::TextUnformatted(steps[i]);
    }
    ImGui::Dummy({0, 4});
    ui::hint("Rest the mouse on any button for what it does. F1 lists every key.");
    ImGui::PopTextWrapPos();
    ImGui::Dummy({0, 6});
    if (ImGui::Button("Start", {120, 0})) {
        show_welcome_ = false;
        engine.settings().set("editor.welcome_seen", true);
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

} // namespace bowl::editor
