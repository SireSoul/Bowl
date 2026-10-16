#include "bowl/physics/collision.hpp"

#include <algorithm>
#include <limits>

namespace bowl::physics {

bool overlaps(const Rect& a, const Rect& b) { return a.overlaps(b); }

bool overlaps(const Circle& a, const Circle& b) {
    f32 r = a.radius + b.radius;
    return (a.center - b.center).length_squared() < r * r;
}

static Vec2 closest_point(const Rect& r, Vec2 p) {
    return {std::clamp(p.x, r.x, r.right()), std::clamp(p.y, r.y, r.bottom())};
}

bool overlaps(const Circle& c, const Rect& r) {
    return (closest_point(r, c.center) - c.center).length_squared() < c.radius * c.radius;
}

bool contains(const Circle& c, Vec2 p) { return (p - c.center).length_squared() <= c.radius * c.radius; }

Contact collide(const Rect& a, const Rect& b) {
    f32 dx1 = b.right() - a.x, dx2 = a.right() - b.x; // overlap if pushed left/right
    f32 dy1 = b.bottom() - a.y, dy2 = a.bottom() - b.y;
    if (dx1 <= 0 || dx2 <= 0 || dy1 <= 0 || dy2 <= 0) return {};
    // Push out along the axis of least overlap.
    f32 px = std::min(dx1, dx2), py = std::min(dy1, dy2);
    if (px < py) return {true, {dx1 < dx2 ? 1.0f : -1.0f, 0}, px};
    return {true, {0, dy1 < dy2 ? 1.0f : -1.0f}, py};
}

Contact collide(const Circle& a, const Circle& b) {
    Vec2 d = a.center - b.center;
    f32 r = a.radius + b.radius;
    f32 dist2 = d.length_squared();
    if (dist2 >= r * r) return {};
    f32 dist = std::sqrt(dist2);
    Vec2 n = dist > 1e-6f ? d / dist : Vec2{0, -1};
    return {true, n, r - dist};
}

Contact collide(const Circle& c, const Rect& r) {
    Vec2 p = closest_point(r, c.center);
    Vec2 d = c.center - p;
    f32 dist2 = d.length_squared();
    if (dist2 > 1e-12f) {
        if (dist2 >= c.radius * c.radius) return {};
        f32 dist = std::sqrt(dist2);
        return {true, d / dist, c.radius - dist};
    }
    // Center inside the box: push out through the nearest side.
    f32 left = c.center.x - r.x, right = r.right() - c.center.x, top = c.center.y - r.y, bottom = r.bottom() - c.center.y;
    f32 m = std::min({left, right, top, bottom});
    if (m == left) return {true, {-1, 0}, left + c.radius};
    if (m == right) return {true, {1, 0}, right + c.radius};
    if (m == top) return {true, {0, -1}, top + c.radius};
    return {true, {0, 1}, bottom + c.radius};
}

SweepHit sweep(const Rect& moving, Vec2 delta, const Rect& target) {
    // Minkowski sum: a ray from the moving box's corner against the target grown by the box's size.
    Rect grown{target.x - moving.w, target.y - moving.h, target.w + moving.w, target.h + moving.h};
    RayHit ray = raycast(moving.position(), delta, 1.0f, grown);
    if (!ray.hit) return {};
    return {true, ray.distance, ray.normal};
}

RayHit raycast(Vec2 origin, Vec2 dir, f32 max_t, const Rect& box) {
    constexpr f32 inf = std::numeric_limits<f32>::infinity();
    f32 t_near = -inf, t_far = inf;
    Vec2 normal;
    for (int axis = 0; axis < 2; ++axis) {
        f32 o = axis == 0 ? origin.x : origin.y;
        f32 d = axis == 0 ? dir.x : dir.y;
        f32 lo = axis == 0 ? box.x : box.y;
        f32 hi = axis == 0 ? box.right() : box.bottom();
        if (std::abs(d) < 1e-12f) {
            if (o <= lo || o >= hi) return {}; // parallel and outside (touching counts as outside)
            continue;
        }
        f32 t1 = (lo - o) / d, t2 = (hi - o) / d;
        Vec2 n = axis == 0 ? Vec2{-1, 0} : Vec2{0, -1};
        if (t1 > t2) {
            std::swap(t1, t2);
            n = -n;
        }
        if (t1 > t_near) {
            t_near = t1;
            normal = n;
        }
        t_far = std::min(t_far, t2);
        if (t_near >= t_far) return {};
    }
    // Starting inside (t_near < 0 < t_far) isn't a hit: overlap resolution is collide()'s job.
    if (t_near < 0 || t_near > max_t) return {};
    return {true, t_near, origin + dir * t_near, normal};
}

void GridSolids::operator()(const Rect& area, FunctionRef<void(const Rect&)> emit) const {
    i32 x0 = static_cast<i32>(std::floor((area.x - origin.x) / cell_size.x));
    i32 y0 = static_cast<i32>(std::floor((area.y - origin.y) / cell_size.y));
    i32 x1 = static_cast<i32>(std::floor((area.right() - origin.x) / cell_size.x));
    i32 y1 = static_cast<i32>(std::floor((area.bottom() - origin.y) / cell_size.y));
    for (i32 y = y0; y <= y1; ++y)
        for (i32 x = x0; x <= x1; ++x)
            if (solid(x, y))
                emit({origin.x + static_cast<f32>(x) * cell_size.x, origin.y + static_cast<f32>(y) * cell_size.y,
                      cell_size.x, cell_size.y});
}

MoveResult move_and_slide(Rect& box, Vec2 delta, SolidQuery solids, int max_iterations) {
    MoveResult result;
    Vec2 start = box.position();
    constexpr f32 skin = 1e-4f; // stop just short of surfaces so the next query doesn't start inside them
    for (int i = 0; i < max_iterations && delta.length_squared() > 1e-12f; ++i) {
        // Everything the box could touch on the way.
        Rect area{std::min(box.x, box.x + delta.x), std::min(box.y, box.y + delta.y), box.w + std::abs(delta.x),
                  box.h + std::abs(delta.y)};
        SweepHit first;
        solids(area, [&](const Rect& solid) {
            SweepHit h = sweep(box, delta, solid);
            if (h.hit && h.time < first.time) first = h;
        });
        if (!first.hit) {
            box.x += delta.x;
            box.y += delta.y;
            break;
        }
        result.collided = true;
        result.last_normal = first.normal;
        f32 t = std::max(0.0f, first.time - skin / std::max(delta.length(), 1e-6f));
        box.x += delta.x * t;
        box.y += delta.y * t;
        // Slide: keep only the motion along the surface.
        Vec2 remaining = delta * (1.0f - t);
        delta = remaining - first.normal * remaining.dot(first.normal);
    }
    result.moved = box.position() - start;
    return result;
}

} // namespace bowl::physics
