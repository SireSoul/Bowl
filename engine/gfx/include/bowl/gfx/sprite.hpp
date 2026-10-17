#pragma once

#include "bowl/core/assets.hpp"
#include "bowl/ecs/hierarchy.hpp"
#include "bowl/gfx/sprites.hpp"

#include <functional>
#include <optional>

namespace bowl::gfx {

// An entity that looks like something: a texture (or a plain rectangle) drawn at the entity's GlobalTransform2D.
// This is what the editor places and shows. Games are free to draw entities their own way instead.
//
//     world.create(ecs::Transform2D{{120, 80}}, gfx::Sprite{.texture = assets.load<gfx::Texture>("props/barrel.png")});
//     gfx::draw_sprites(world, batch);   // after the transform systems have run
struct Sprite {
    Asset<Texture> texture; // none: a solid rectangle in `color`
    Rect region;            // pixels of the texture to show; empty means all of it
    Vec2 size;              // world units before the transform's scale; zero means the region's pixel size
    Vec2 origin{0.5f, 0.5f};
    Color color = Color::white();
    f32 depth = 0;      // added to the draw depth
    bool y_sort = true; // draw depth is the entity's world y plus `depth`, so lower things draw in front
    bool visible = true;
};
BOWL_COMPONENT(Sprite, texture, region, size, origin, color, depth, y_sort, visible)

// The sprite's size in world units before scaling: `size`, or the region's or texture's pixel size, or 32 x 32 for a
// rectangle with no size.
Vec2 sprite_size(const Sprite& sprite);

// Draws every visible Sprite that has a GlobalTransform2D. `skip` leaves entities out (Bowl Editor's hidden ones).
void draw_sprites(ecs::World& world, SpriteBatch& batch, const std::function<bool(ecs::Entity)>& skip = {});

// The axis-aligned world rectangle a sprite covers (enclosing it when rotated), for picking and culling. Empty if the
// entity has no Sprite or no GlobalTransform2D.
std::optional<Rect> sprite_bounds(const ecs::World& world, ecs::Entity e);

} // namespace bowl::gfx
