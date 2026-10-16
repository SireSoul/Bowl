// Regions and collision shapes: every physics::Collider drawn on the level (triggers filled, solid ones outlined),
// handles to resize the selected one, and the Region tool, which drags out a new trigger area.

#include "editor.hpp"

#include "bowl/physics/colliders.hpp"

#include <cmath>
#include <format>

namespace bowl::editor {

namespace {

const Color solid_color = Color::hex(0xff9a4d), trigger_color = Color::hex(0x5ad1c8);

void line(gfx::SpriteBatch& batch, Vec2 a, Vec2 b, f32 thickness, Color color) {
    Vec2 d = b - a;
    f32 length = d.length();
    if (length <= 0) return;
    gfx::SpriteInstance s;
    s.position = a;
    s.size = {length, thickness};
    s.origin = {0, 0.5f};
    s.rotation = std::atan2(d.y, d.x);
    s.color = color;
    batch.draw(nullptr, s);
}

void trace(gfx::SpriteBatch& batch, const std::vector<Vec2>& points, f32 thickness, Color color) {
    for (usize i = 0; i < points.size(); ++i) line(batch, points[i], points[(i + 1) % points.size()], thickness, color);
}

std::vector<Vec2> arc(Vec2 center, f32 radius, f32 from, f32 to, int steps) {
    std::vector<Vec2> out;
    for (int i = 0; i <= steps; ++i) out.push_back(center + Vec2::from_angle(from + (to - from) * f32(i) / f32(steps)) * radius);
    return out;
}

// The outline of a shape, as points around it.
std::vector<Vec2> outline_of(const physics::Shape& shape) {
    if (const auto* r = std::get_if<Rect>(&shape)) return {r->position(), {r->right(), r->y}, {r->right(), r->bottom()}, {r->x, r->bottom()}};
    if (const auto* c = std::get_if<physics::Circle>(&shape)) {
        auto pts = arc(c->center, c->radius, 0, tau, 40);
        pts.pop_back();
        return pts;
    }
    if (const auto* cap = std::get_if<physics::Capsule>(&shape)) {
        Vec2 d = cap->b - cap->a;
        f32 angle = std::atan2(d.y, d.x);
        auto pts = arc(cap->b, cap->radius, angle - pi / 2, angle + pi / 2, 16);
        auto back = arc(cap->a, cap->radius, angle + pi / 2, angle + pi * 1.5f, 16);
        pts.insert(pts.end(), back.begin(), back.end());
        return pts;
    }
    return std::get<physics::Polygon>(shape).points;
}

} // namespace

void Editor::draw_colliders(Engine& engine, f32 px) {
    if (!show_colliders_) return;
    ecs::World& world = engine.world();
    world.each<const physics::Collider, const ecs::GlobalTransform2D>(
        [&](ecs::Entity e, const physics::Collider& c, const ecs::GlobalTransform2D& g) {
            physics::Shape shape = physics::world_shape(c, g);
            std::vector<Vec2> points = outline_of(shape);
            Color color = c.trigger ? trigger_color : solid_color;
            bool chosen = selected(e);
            if (c.trigger) { // a region: tinted inside
                Rect b = physics::bounds(shape);
                if (std::holds_alternative<Rect>(shape)) gizmos_.draw_rect(b, color.with_alpha(chosen ? 0.18f : 0.1f));
            }
            trace(gizmos_, points, (chosen ? 2.5f : 1.5f) * px, color.with_alpha(chosen ? 1.0f : 0.75f));
            // The selected box or circle gets handles to resize it.
            if (chosen && selection_.size() == 1 && g.rotation == 0) {
                if (c.kind == physics::Collider::Kind::Box)
                    for (Vec2 corner : outline_of(shape)) gizmos_.draw_rect(Rect::from_center(corner, Vec2{7 * px}), color);
                else if (c.kind == physics::Collider::Kind::Circle)
                    gizmos_.draw_rect(Rect::from_center(std::get<physics::Circle>(shape).center +
                                                            Vec2{std::get<physics::Circle>(shape).radius, 0},
                                                        Vec2{7 * px}),
                                      color);
            }
        });
    // The Region tool's rectangle while dragging one out.
    if (tool_ == Tool::Region && region_handle_ == 99) {
        Vec2 a = region_anchor_, b = snapped(mouse_world(engine));
        Rect r{std::min(a.x, b.x), std::min(a.y, b.y), std::abs(b.x - a.x), std::abs(b.y - a.y)};
        gizmos_.draw_rect(r, trigger_color.with_alpha(0.15f));
        gizmos_.draw_outline(r, trigger_color, 2 * px);
    }
}

bool Editor::region_mouse(Engine& engine) {
    ecs::World& world = engine.world();
    Input& in = engine.input();
    bool over_ui = ImGui::GetIO().WantCaptureMouse;
    Vec2 m = mouse_world(engine);
    f32 px = 1.0f / engine.camera().zoom;

    // Dragging a handle or a new region.
    if (region_handle_ >= 0) {
        if (region_handle_ == 99) { // a new region
            if (!in.down(MouseButton::Left)) {
                region_handle_ = -1;
                Vec2 a = region_anchor_, b = snapped(m);
                Vec2 size{std::abs(b.x - a.x), std::abs(b.y - a.y)}, center = (a + b) * 0.5f;
                if (size.x < 2 || size.y < 2) { // a click: a region of four grid squares, centered there
                    size = {grid_ * 4, grid_ * 4};
                    center = a;
                }
                usize n = 1;
                world.each<const ecs::Name>([&](const ecs::Name& name) { n += name.value.starts_with("Region") ? 1 : 0; });
                ecs::Entity e = world.create(ecs::Name{std::format("Region {}", n)}, ecs::Transform2D{center},
                                             physics::Collider{.size = size, .trigger = true});
                doc_->add(e);
                select_only(e);
                ecs::propagate_transforms(world);
                say("Region made: name it, and the game finds it by that name (or add your own components)");
            }
            return true;
        }
        if (!in.down(MouseButton::Left) || selection_.size() != 1) {
            region_handle_ = -1;
            return true;
        }
        ecs::Entity e = selection_[0];
        auto* c = world.try_get<physics::Collider>(e);
        const auto* g = world.try_get<const ecs::GlobalTransform2D>(e);
        if (!c || !g) {
            region_handle_ = -1;
            return true;
        }
        Vec2 p = snapped(m);
        Vec2 scale{std::max(std::abs(g->scale.x), 1e-4f), std::max(std::abs(g->scale.y), 1e-4f)};
        if (c->kind == physics::Collider::Kind::Circle) {
            c->radius = std::max(1.0f, (p - (g->position + c->offset * scale)).length() / scale.x);
        } else {
            // The opposite corner stays put.
            Vec2 lo{std::min(region_anchor_.x, p.x), std::min(region_anchor_.y, p.y)};
            Vec2 hi{std::max(region_anchor_.x, p.x), std::max(region_anchor_.y, p.y)};
            c->size = {std::max(1.0f, (hi.x - lo.x) / scale.x), std::max(1.0f, (hi.y - lo.y) / scale.y)};
            c->offset = ((lo + hi) * 0.5f - g->position) / scale;
        }
        return true;
    }

    if (over_ui || !in.pressed(MouseButton::Left)) return false;

    // A handle on the selected collider?
    if (show_colliders_ && selection_.size() == 1) {
        ecs::Entity e = selection_[0];
        const auto* c = world.try_get<const physics::Collider>(e);
        const auto* g = world.try_get<const ecs::GlobalTransform2D>(e);
        if (c && g && g->rotation == 0) {
            physics::Shape shape = physics::world_shape(*c, *g);
            f32 reach = 7 * px;
            if (c->kind == physics::Collider::Kind::Box) {
                auto corners = outline_of(shape);
                for (int i = 0; i < 4; ++i)
                    if ((corners[i] - m).length() <= reach) {
                        region_handle_ = i;
                        region_anchor_ = corners[(i + 2) % 4];
                        return true;
                    }
            } else if (c->kind == physics::Collider::Kind::Circle) {
                const auto& circle = std::get<physics::Circle>(shape);
                if ((circle.center + Vec2{circle.radius, 0} - m).length() <= reach) {
                    region_handle_ = 0;
                    return true;
                }
            }
        }
    }
    if (tool_ == Tool::Region) {
        region_handle_ = 99;
        region_anchor_ = snapped(m);
        return true;
    }
    return false;
}

} // namespace bowl::editor
