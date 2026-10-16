#include "ph/app/inspector.hpp"

#include "ph/ecs/hierarchy.hpp"

#include <algorithm>
#include <string>

#if PH_DEV_TOOLS

#include "ph/core/reflect.hpp"

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <format>
#include <string>

namespace ph::inspector {

namespace {

// Edits reflected fields with ImGui widgets and remembers whether anything changed.
class FieldEditor final : public reflect::FieldVisitor {
public:
    FieldEditor(AssetManager& assets, ecs::Entity* select) : assets_(assets), select_(select) {}
    bool changed() const { return changed_; }

    void boolean(std::string_view name, bool& v) override { changed_ |= ImGui::Checkbox(label(name), &v); }
    void integer(std::string_view name, i64& v, i64 min, i64 max) override {
        if (ImGui::DragScalar(label(name), ImGuiDataType_S64, &v, 0.2f, &min, &max)) {
            v = std::clamp(v, min, max);
            changed_ = true;
        }
    }
    void real(std::string_view name, f64& v) override {
        changed_ |= ImGui::DragScalar(label(name), ImGuiDataType_Double, &v, 0.05f);
    }
    void text(std::string_view name, std::string& v) override {
        char buffer[512];
        std::snprintf(buffer, sizeof buffer, "%s", v.c_str());
        if (ImGui::InputText(label(name), buffer, sizeof buffer)) {
            v = buffer;
            changed_ = true;
        }
    }
    void vector(std::string_view name, f32* values, int count) override {
        changed_ |= ImGui::DragScalarN(label(name), ImGuiDataType_Float, values, count, 0.25f);
    }
    void color(std::string_view name, Color& c) override {
        changed_ |= ImGui::ColorEdit4(label(name), &c.r, ImGuiColorEditFlags_Float);
    }
    void entity(std::string_view name, u64 bits) override {
        ecs::Entity e = ecs::Entity::from_bits(bits);
        if (!e) {
            ImGui::TextDisabled("%.*s: none", static_cast<int>(name.size()), name.data());
            return;
        }
        std::string text = std::format("{}: entity {}:{}##{}", name, e.index, e.generation, name);
        if (ImGui::SmallButton(text.c_str()) && select_) *select_ = e; // jump to it
    }
    bool begin_group(std::string_view name) override {
        return ImGui::TreeNodeEx(label(name), ImGuiTreeNodeFlags_DefaultOpen);
    }
    void end_group() override { ImGui::TreePop(); }
    void unsupported(std::string_view name, std::string_view type) override {
        ImGui::TextDisabled("%.*s: %.*s", static_cast<int>(name.size()), name.data(), static_cast<int>(type.size()),
                            type.data());
    }
    // A path field: type a path and press Enter, or drop an asset on it from the editor's asset browser.
    bool asset(std::string_view name, std::string& path, std::string_view) override {
        char buffer[512];
        std::snprintf(buffer, sizeof buffer, "%s", path.c_str());
        bool commit = ImGui::InputText(label(name), buffer, sizeof buffer, ImGuiInputTextFlags_EnterReturnsTrue);
        if (commit) path = buffer;
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(asset_payload)) {
                path.assign(static_cast<const char*>(p->Data), static_cast<usize>(p->DataSize));
                commit = true;
            }
            ImGui::EndDragDropTarget();
        }
        changed_ |= commit;
        return commit;
    }
    AssetManager* assets() override { return &assets_; }
    // A list: its items in a tree, with + and - to grow or shrink vectors at the end.
    bool begin_list(std::string_view name, usize& size, bool resizable) override {
        std::string text = std::format("{} ({})###{}", name, size, name);
        bool open = ImGui::TreeNodeEx(text.c_str(), size <= 8 ? ImGuiTreeNodeFlags_DefaultOpen : 0);
        if (resizable) {
            ImGui::SameLine();
            ImGui::PushID(label(name));
            if (ImGui::SmallButton("+")) {
                ++size;
                changed_ = true;
            }
            ImGui::SameLine();
            ImGui::BeginDisabled(size == 0);
            if (ImGui::SmallButton("-") && size > 0) {
                --size;
                changed_ = true;
            }
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        return open;
    }
    void end_list() override { ImGui::TreePop(); }

private:
    const char* label(std::string_view name) {
        label_.assign(name);
        return label_.c_str();
    }
    AssetManager& assets_;
    ecs::Entity* select_;
    std::string label_;
    bool changed_ = false;
};

} // namespace

bool component(ecs::World& world, ecs::Entity e, ecs::ComponentId c, AssetManager& assets, ecs::Entity* select) {
    const ecs::ComponentInfo& info = ecs::component_info(c);
    std::string header(info.name);
    bool keep = true;
    bool open = ImGui::CollapsingHeader(header.c_str(), &keep, ImGuiTreeNodeFlags_DefaultOpen);
    if (!keep) { // its close button: remove the component
        world.remove_component(e, c);
        return true;
    }
    if (!open) return false;
    ImGui::PushID(c);
    bool changed = false;
    if (info.visit) {
        FieldEditor editor(assets, select);
        info.visit(world.component_ptr(e, c), editor);
        changed = editor.changed();
    } else if (info.size == 0) {
        ImGui::TextDisabled("tag");
    } else {
        ImGui::TextDisabled("not reflected: add PH_COMPONENT(%.*s, fields...) to edit it here",
                            static_cast<int>(info.name.size()), info.name.data());
    }
    ImGui::PopID();
    return changed;
}

bool fields(ecs::World& world, ecs::Entity e, ecs::ComponentId c, AssetManager& assets, ecs::Entity* select) {
    const ecs::ComponentInfo& info = ecs::component_info(c);
    if (!info.visit || !world.alive(e)) return false;
    FieldEditor editor(assets, select);
    info.visit(world.component_ptr(e, c), editor);
    return editor.changed();
}

bool components(ecs::World& world, ecs::Entity e, AssetManager& assets, ecs::Entity* select) {
    if (!world.alive(e)) return false;
    // Copied: editing may not change the set, but a hook could.
    std::vector<ecs::ComponentId> ids(world.components_of(e).begin(), world.components_of(e).end());
    bool changed = false;
    for (ecs::ComponentId c : ids) {
        if (!world.alive(e)) break;
        changed |= component(world, e, c, assets, select);
    }
    if (world.alive(e)) changed |= add_component_menu(world, e);
    return changed;
}

bool add_component_menu(ecs::World& world, ecs::Entity e) {
    bool added = false;
    if (ImGui::Button("Add component")) ImGui::OpenPopup("add component");
    if (ImGui::BeginPopup("add component")) {
        static char search[64] = "";
        if (ImGui::IsWindowAppearing()) {
            search[0] = 0;
            ImGui::SetKeyboardFocusHere();
        }
        ImGui::InputTextWithHint("##search", "search", search, sizeof search);
        // Savable types can be made from nothing (default-constructed); others need code to create them.
        std::vector<std::pair<std::string_view, ecs::ComponentId>> choices;
        for (ecs::ComponentId c = 0; c < ecs::component_type_count(); ++c) {
            const ecs::ComponentInfo& info = ecs::component_info(c);
            if (!info.savable() || world.component_ptr(e, c) || !text_matches(info.name, search)) continue;
            choices.emplace_back(info.name, c);
        }
        std::sort(choices.begin(), choices.end());
        for (const auto& [name, c] : choices) {
            std::string item(name);
            if (ImGui::Selectable(item.c_str())) {
                added = world.add_component(e, c, [](void*) {});
                ImGui::CloseCurrentPopup();
            }
        }
        if (choices.empty()) ImGui::TextDisabled("nothing to add");
        ImGui::EndPopup();
    }
    return added;
}

} // namespace ph::inspector

#endif

namespace ph::inspector {

bool text_matches(std::string_view text, std::string_view filter) {
    auto lower = [](char c) { return static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c); };
    if (filter.empty()) return true;
    auto it = std::search(text.begin(), text.end(), filter.begin(), filter.end(),
                          [&](char a, char b) { return lower(a) == lower(b); });
    return it != text.end();
}

bool matches(const ecs::World& world, ecs::Entity e, std::string_view filter) {
    // Every word must match something: a component's name, the entity's Name, or "#index".
    usize at = 0;
    while (at < filter.size()) {
        usize end = filter.find(' ', at);
        std::string_view word = filter.substr(at, end == std::string_view::npos ? std::string_view::npos : end - at);
        at = end == std::string_view::npos ? filter.size() : end + 1;
        if (word.empty()) continue;
        bool found = false;
        if (word.starts_with('#')) found = std::to_string(e.index) == word.substr(1);
        if (const auto* name = world.try_get<ecs::Name>(e); !found && name) found = text_matches(name->value, word);
        for (ecs::ComponentId c : world.components_of(e))
            found = found || text_matches(ecs::component_info(c).name, word);
        if (!found) return false;
    }
    return true;
}

} // namespace ph::inspector
