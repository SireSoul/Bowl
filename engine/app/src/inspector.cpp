#include "ph/app/inspector.hpp"

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
    if (!ImGui::CollapsingHeader(header.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) return false;
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
    for (ecs::ComponentId c : ids) changed |= component(world, e, c, assets, select);
    return changed;
}

} // namespace ph::inspector

#endif
