// Polygons, capsules and the generic Shape tests.
//
// Every shape is handled as a convex core (one point, a segment, or a polygon) grown by a radius: a circle is a point
// with a radius, a capsule a segment with one, rectangles and polygons have none. Two shapes touch when their cores
// overlap, or when the cores are closer than the two radii added up. When the cores overlap, the separating axis test
// over the cores' edge normals (with each projection widened by its radius) finds the shortest way out; otherwise the
// closest points between the cores give it.

#include "ph/physics/collision.hpp"

#include <algorithm>
#include <limits>

namespace ph::physics {

namespace {

constexpr f32 inf = std::numeric_limits<f32>::infinity();

struct Hull {
    std::vector<Vec2> core; // 1 point, 2 (a segment), or a convex polygon
    f32 radius = 0;
};

Hull hull_of(const Shape& s) {
    return std::visit(
        [](const auto& v) -> Hull {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, Rect>) return {Polygon::box(v).points, 0};
            else if constexpr (std::is_same_v<T, Circle>) return {{v.center}, v.radius};
            else if constexpr (std::is_same_v<T, Capsule>) return {{v.a, v.b}, v.radius};
            else return {v.points, 0};
        },
        s);
}

Vec2 centroid(const std::vector<Vec2>& pts) {
    Vec2 sum;
    for (Vec2 p : pts) sum += p;
    return pts.empty() ? Vec2{} : sum / static_cast<f32>(pts.size());
}

// Directions worth testing as separating axes: edge normals, plus a segment's own direction (two segments on one
// line can only be told apart along it).
void add_axes(const Hull& h, std::vector<Vec2>& axes) {
    usize n = h.core.size();
    if (n == 2) {
        Vec2 d = h.core[1] - h.core[0];
        if (d.length_squared() > 1e-12f) {
            d = d.normalized();
            axes.push_back(d.perpendicular());
            axes.push_back(d);
        }
    } else if (n >= 3) {
        for (usize i = 0; i < n; ++i) {
            Vec2 e = h.core[(i + 1) % n] - h.core[i];
            if (e.length_squared() > 1e-12f) axes.push_back(e.normalized().perpendicular());
        }
    }
}

void project(const Hull& h, Vec2 axis, f32 radius, f32& lo, f32& hi) {
    lo = inf;
    hi = -inf;
    for (Vec2 p : h.core) {
        f32 d = p.dot(axis);
        lo = std::min(lo, d);
        hi = std::max(hi, d);
    }
    lo -= radius;
    hi += radius;
}

bool cores_overlap(const Hull& a, const Hull& b) {
    std::vector<Vec2> axes;
    add_axes(a, axes);
    add_axes(b, axes);
    if (axes.empty()) return (a.core[0] - b.core[0]).length_squared() < 1e-12f; // two points
    for (Vec2 axis : axes) {
        f32 alo, ahi, blo, bhi;
        project(a, axis, 0, alo, ahi);
        project(b, axis, 0, blo, bhi);
        if (ahi < blo || bhi < alo) return false;
    }
    return true;
}

Vec2 closest_on_segment(Vec2 p, Vec2 a, Vec2 b) {
    Vec2 ab = b - a;
    f32 len2 = ab.length_squared();
    if (len2 < 1e-12f) return a;
    f32 t = std::clamp((p - a).dot(ab) / len2, 0.0f, 1.0f);
    return a + ab * t;
}

// The closest points between two cores that don't overlap. For segments that don't cross, the closest pair always
// includes an endpoint, so testing every endpoint against every edge is enough.
void closest(const Hull& a, const Hull& b, Vec2& pa, Vec2& pb) {
    f32 best = inf;
    auto edges = [](const Hull& h, auto&& f) {
        usize n = h.core.size();
        if (n == 1) f(h.core[0], h.core[0]);
        else if (n == 2) f(h.core[0], h.core[1]);
        else
            for (usize i = 0; i < n; ++i) f(h.core[i], h.core[(i + 1) % n]);
    };
    for (Vec2 p : a.core)
        edges(b, [&](Vec2 s0, Vec2 s1) {
            Vec2 q = closest_on_segment(p, s0, s1);
            if (f32 d = (p - q).length_squared(); d < best) {
                best = d;
                pa = p;
                pb = q;
            }
        });
    for (Vec2 p : b.core)
        edges(a, [&](Vec2 s0, Vec2 s1) {
            Vec2 q = closest_on_segment(p, s0, s1);
            if (f32 d = (p - q).length_squared(); d < best) {
                best = d;
                pa = q;
                pb = p;
            }
        });
}

RayHit ray_circle(Vec2 o, Vec2 d, f32 max_t, Circle c) {
    Vec2 m = o - c.center;
    f32 a = d.length_squared(), b = m.dot(d), k = m.length_squared() - c.radius * c.radius;
    if (a < 1e-12f || k <= 0) return {}; // no direction, or starting inside
    f32 disc = b * b - a * k;
    if (disc < 0) return {};
    f32 t = (-b - std::sqrt(disc)) / a;
    if (t < 0 || t > max_t) return {};
    Vec2 p = o + d * t;
    return {true, t, p, (p - c.center).normalized()};
}

RayHit ray_polygon(Vec2 o, Vec2 d, f32 max_t, const std::vector<Vec2>& pts) {
    usize n = pts.size();
    if (n < 3) return {};
    Vec2 c = centroid(pts);
    f32 t_enter = -inf, t_exit = inf;
    Vec2 normal;
    for (usize i = 0; i < n; ++i) {
        Vec2 p0 = pts[i], p1 = pts[(i + 1) % n];
        Vec2 nrm = (p1 - p0).perpendicular().normalized();
        if (nrm.dot(p0 - c) < 0) nrm = -nrm; // outward, whichever way the points go around
        f32 denom = nrm.dot(d), dist = nrm.dot(p0 - o);
        if (std::abs(denom) < 1e-12f) {
            if (dist < 0) return {}; // parallel to this edge and outside it
            continue;
        }
        f32 t = dist / denom;
        if (denom < 0) { // entering through this edge
            if (t > t_enter) {
                t_enter = t;
                normal = nrm;
            }
        } else {
            t_exit = std::min(t_exit, t);
        }
        if (t_enter > t_exit) return {};
    }
    if (t_enter < 0 || t_enter > max_t) return {}; // starting inside isn't a hit
    return {true, t_enter, o + d * t_enter, normal};
}

} // namespace

Polygon Polygon::box(Vec2 center, Vec2 size, f32 rotation) {
    Vec2 h = size * 0.5f;
    Polygon p;
    for (Vec2 corner : {Vec2{-h.x, -h.y}, Vec2{h.x, -h.y}, Vec2{h.x, h.y}, Vec2{-h.x, h.y}})
        p.points.push_back(center + corner.rotated(rotation));
    return p;
}

Rect Polygon::bounds() const {
    if (points.empty()) return {};
    Vec2 lo = points[0], hi = points[0];
    for (Vec2 p : points) {
        lo = {std::min(lo.x, p.x), std::min(lo.y, p.y)};
        hi = {std::max(hi.x, p.x), std::max(hi.y, p.y)};
    }
    return {lo.x, lo.y, hi.x - lo.x, hi.y - lo.y};
}

Rect Capsule::bounds() const {
    Vec2 lo{std::min(a.x, b.x) - radius, std::min(a.y, b.y) - radius};
    Vec2 hi{std::max(a.x, b.x) + radius, std::max(a.y, b.y) + radius};
    return {lo.x, lo.y, hi.x - lo.x, hi.y - lo.y};
}

Rect bounds(const Shape& s) {
    return std::visit(
        [](const auto& v) -> Rect {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, Rect>) return v;
            else if constexpr (std::is_same_v<T, Circle>) return Rect::from_center(v.center, Vec2{v.radius * 2});
            else return v.bounds();
        },
        s);
}

Contact collide(const Shape& sa, const Shape& sb) {
    Hull a = hull_of(sa), b = hull_of(sb);
    if (a.core.empty() || b.core.empty()) return {};
    f32 r = a.radius + b.radius;
    if (!cores_overlap(a, b)) {
        if (r <= 0) return {};
        Vec2 pa, pb;
        closest(a, b, pa, pb);
        Vec2 d = pa - pb;
        f32 dist2 = d.length_squared();
        if (dist2 >= r * r) return {};
        f32 dist = std::sqrt(dist2);
        Vec2 n = dist > 1e-6f ? d / dist : (centroid(a.core) - centroid(b.core)).normalized();
        if (n.length_squared() < 0.5f) n = {0, -1};
        return {true, n, r - dist};
    }
    std::vector<Vec2> axes;
    add_axes(a, axes);
    add_axes(b, axes);
    if (axes.empty()) return {true, {0, -1}, r}; // two points in the same place
    f32 best = inf;
    Vec2 best_axis;
    for (Vec2 axis : axes) {
        f32 alo, ahi, blo, bhi;
        project(a, axis, a.radius, alo, ahi);
        project(b, axis, b.radius, blo, bhi);
        f32 overlap = std::min(ahi, bhi) - std::max(alo, blo);
        if (overlap < best) {
            best = overlap;
            best_axis = axis;
        }
    }
    if (best <= 0) return {};
    if ((centroid(a.core) - centroid(b.core)).dot(best_axis) < 0) best_axis = -best_axis; // from b toward a
    return {true, best_axis, best};
}

bool overlaps(const Shape& a, const Shape& b) { return collide(a, b).hit; }

bool contains(const Shape& s, Vec2 p) { return collide(Circle{p, 0}, s).hit; }

RayHit raycast(Vec2 origin, Vec2 dir, f32 max_t, const Shape& s) {
    return std::visit(
        [&](const auto& v) -> RayHit {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, Rect>) return raycast(origin, dir, max_t, v);
            else if constexpr (std::is_same_v<T, Circle>) return ray_circle(origin, dir, max_t, v);
            else if constexpr (std::is_same_v<T, Polygon>) return ray_polygon(origin, dir, max_t, v.points);
            else {
                // The two end circles and the body between them; the nearest hit wins.
                RayHit best = ray_circle(origin, dir, max_t, {v.a, v.radius});
                RayHit h = ray_circle(origin, dir, max_t, {v.b, v.radius});
                if (h.hit && (!best.hit || h.distance < best.distance)) best = h;
                Vec2 axis = v.b - v.a;
                if (axis.length_squared() > 1e-12f) {
                    Vec2 side = axis.normalized().perpendicular() * v.radius;
                    h = ray_polygon(origin, dir, max_t, {v.a + side, v.b + side, v.b - side, v.a - side});
                    if (h.hit && (!best.hit || h.distance < best.distance)) best = h;
                }
                return best;
            }
        },
        s);
}

} // namespace ph::physics
