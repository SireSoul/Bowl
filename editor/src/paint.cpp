// The Level workspace's Brush tool: painting on a picture where it stands in the level, through the Paint workspace
// (paint_studio.cpp), so the strokes are its strokes (its pencil, colors, layers and undo).

#include "editor.hpp"

#include "bowl/gfx/sprite.hpp"

#include <cmath>

namespace bowl::editor {

void Editor::brush_mouse(Engine& engine) {
    const ImGuiIO& io = ImGui::GetIO();
    ecs::World& world = engine.world();
    Vec2 p = mouse_world(engine);
    bool start = !io.WantCaptureMouse && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    if (!start && !painting_) return;
    if (start) {
        ecs::Entity e = pick(engine, p);
        const gfx::Sprite* s = e ? world.try_get<const gfx::Sprite>(e) : nullptr;
        if (!s || !s->texture || s->texture.path().empty()) {
            say("The brush paints on art: click a picture in the level");
            return;
        }
        if (!paint_.open(std::string(s->texture.path()))) return;
        brush_target_ = e;
    }
    const gfx::Sprite* s = world.alive(brush_target_) ? world.try_get<const gfx::Sprite>(brush_target_) : nullptr;
    const auto* g = world.try_get<const ecs::GlobalTransform2D>(brush_target_);
    if (!s || !g || !s->texture) {
        painting_ = false;
        return;
    }
    // World point -> the sprite's own space -> a pixel of its region.
    Vec2 size = gfx::sprite_size(*s) * g->scale;
    Vec2 local = (p - g->position).rotated(-g->rotation);
    Vec2 uv = Vec2{local.x / size.x, local.y / size.y} + s->origin;
    Rect region = s->region.w > 0 && s->region.h > 0 ? s->region
                                                      : Rect{0, 0, static_cast<f32>(s->texture->width()), static_cast<f32>(s->texture->height())};
    i32 x = static_cast<i32>(std::floor(region.x + uv.x * region.w));
    i32 y = static_cast<i32>(std::floor(region.y + uv.y * region.h));
    if (start) {
        painting_ = true;
        paint_.brush(x, y, true);
    } else if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        paint_.brush(x, y, false);
    } else {
        painting_ = false;
        paint_.brush_end();
    }
}

} // namespace bowl::editor
