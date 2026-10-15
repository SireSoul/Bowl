#pragma once

#include "ph/core/assets.hpp"
#include "ph/ecs/world.hpp"

// Dear ImGui widgets for looking at and editing entities, shared by the F1 overlay and the editor. Dev builds only
// (PH_DEV_TOOLS); call them between ImGui::Begin and ImGui::End.
namespace ph::inspector {

// Drag-and-drop payload type for asset paths (a UTF-8 string, no terminator). Asset fields accept it.
inline constexpr const char* asset_payload = "PH_ASSET_PATH";

// One component of an entity under a collapsing header: editable fields for reflected components, a note for the
// rest. Clicking an entity field stores that entity in *select. Returns true if a value changed.
bool component(ecs::World& world, ecs::Entity e, ecs::ComponentId c, AssetManager& assets, ecs::Entity* select);

// Every component of the entity, in order.
bool components(ecs::World& world, ecs::Entity e, AssetManager& assets, ecs::Entity* select);

} // namespace ph::inspector
