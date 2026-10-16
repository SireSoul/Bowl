#pragma once

#include "bowl/core/assets.hpp"
#include "bowl/ecs/world.hpp"

// Dear ImGui widgets for looking at and editing entities, shared by the F1 overlay and the editor. Dev builds only
// (BOWL_DEV_TOOLS); call them between ImGui::Begin and ImGui::End.
namespace bowl::inspector {

// Drag-and-drop payload type for asset paths (a UTF-8 string, no terminator). Asset fields accept it.
inline constexpr const char* asset_payload = "BOWL_ASSET_PATH";

// One component of an entity under a collapsing header: editable fields for reflected components (lists included), a
// note for the rest, and a close button that removes the component. Clicking an entity field stores that entity in
// *select. Returns true if anything changed.
bool component(ecs::World& world, ecs::Entity e, ecs::ComponentId c, AssetManager& assets, ecs::Entity* select);

// Every component of the entity, in order, then add_component_menu.
bool components(ecs::World& world, ecs::Entity e, AssetManager& assets, ecs::Entity* select);

// An "Add component" button: a searchable list of the savable component types the entity doesn't have yet.
bool add_component_menu(ecs::World& world, ecs::Entity e);

// Whether an entity matches a filter (these two work in every build): every word must appear, ignoring case, in one
// of its component names or its Name, or be "#index". "player health" finds entities with both; "#42" finds entity 42.
bool matches(const ecs::World& world, ecs::Entity e, std::string_view filter);
bool text_matches(std::string_view text, std::string_view filter);

// Just the editable fields of one component, without a header (for tools that draw their own). Returns true if a
// value changed; false (and nothing drawn) for components that aren't reflected.
bool fields(ecs::World& world, ecs::Entity e, ecs::ComponentId c, AssetManager& assets, ecs::Entity* select);

} // namespace bowl::inspector
