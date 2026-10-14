#pragma once

#include "ph/core/function_ref.hpp"
#include "ph/core/math.hpp"

#include <cmath>
#include <unordered_map>
#include <vector>

namespace ph::physics {

// Collision for games that move things around a world: overlap tests that say how to separate, swept boxes that
// never tunnel, raycasts, a spatial hash broadphase, and move_and_slide for characters. Any consistent unit works:
// pixels, tiles, or the cell space of an isometric map.

struct Circle {
    Vec2 center;
    f32 radius = 0;
};

// Minimum translation: move the first shape by normal * depth to stop overlapping.
struct Contact {
    bool hit = false;
    Vec2 normal; // points from the second shape toward the first
    f32 depth = 0;
};

bool overlaps(const Rect& a, const Rect& b);
bool overlaps(const Circle& a, const Circle& b);
bool overlaps(const Circle& c, const Rect& r);
bool contains(const Circle& c, Vec2 p);

Contact collide(const Rect& a, const Rect& b);
Contact collide(const Circle& a, const Circle& b);
Contact collide(const Circle& c, const Rect& r);

// A box moving by `delta` against a static box. time is the fraction of delta travelled before touching (0..1).
struct SweepHit {
    bool hit = false;
    f32 time = 1;
    Vec2 normal;
};
SweepHit sweep(const Rect& moving, Vec2 delta, const Rect& target);

struct RayHit {
    bool hit = false;
    f32 distance = 0;
    Vec2 point, normal;
};
// Finds where origin + dir * t first enters the box, for t in [0, max_t]. `distance` is that t (in units of dir's
// length; pass a normalized dir to get world distance). Rays starting inside the box don't hit it.
RayHit raycast(Vec2 origin, Vec2 dir, f32 max_t, const Rect& box);

// Supplies the solid boxes near an area: call `emit` for each.
using SolidQuery = FunctionRef<void(const Rect& area, FunctionRef<void(const Rect&)> emit)>;

// Solid cells of a uniform grid (tilemaps): solid(x, y) decides per cell; cells are cell_size wide, starting at origin.
struct GridSolids {
    Vec2 cell_size{1, 1};
    Vec2 origin;
    FunctionRef<bool(i32 x, i32 y)> solid;

    void operator()(const Rect& area, FunctionRef<void(const Rect&)> emit) const;
};

struct MoveResult {
    Vec2 moved;          // how far the box actually went
    bool collided = false;
    Vec2 last_normal;    // normal of the last surface touched (e.g. normal.y < 0 means standing on a floor)
};

// Moves `box` by `delta`, sliding along whatever `solids` reports instead of stopping dead, without tunneling.
// Updates box in place.
MoveResult move_and_slide(Rect& box, Vec2 delta, SolidQuery solids, int max_iterations = 4);

// Uniform-grid spatial hash for broadphase over many moving things. Rebuild it every tick (clear + insert): that is
// cheaper than updating and stays correct. T is usually an ecs::Entity.
template <class T> class SpatialHash {
public:
    explicit SpatialHash(f32 cell_size = 64) : cell_(cell_size), inv_(1.0f / cell_size) {}

    void clear() {
        for (auto& [key, bucket] : cells_) bucket.clear();
        items_.clear();
    }
    void insert(const T& value, const Rect& bounds) {
        u32 index = static_cast<u32>(items_.size());
        items_.push_back({value, bounds, 0});
        for_cells(bounds, [&](u64 key) { cells_[key].push_back(index); });
    }
    // Calls f(value, bounds) once for each item whose bounds overlap `area`.
    template <class F> void query(const Rect& area, F&& f) {
        ++stamp_;
        for_cells(area, [&](u64 key) {
            auto it = cells_.find(key);
            if (it == cells_.end()) return;
            for (u32 index : it->second) {
                Item& item = items_[index];
                if (item.stamp == stamp_) continue; // already reported via another cell
                item.stamp = stamp_;
                if (overlaps(item.bounds, area)) f(item.value, item.bounds);
            }
        });
    }
    usize size() const { return items_.size(); }

private:
    struct Item {
        T value;
        Rect bounds;
        u32 stamp;
    };
    template <class F> void for_cells(const Rect& r, F&& f) const {
        i32 x0 = static_cast<i32>(std::floor(r.x * inv_)), y0 = static_cast<i32>(std::floor(r.y * inv_));
        i32 x1 = static_cast<i32>(std::floor(r.right() * inv_)), y1 = static_cast<i32>(std::floor(r.bottom() * inv_));
        for (i32 y = y0; y <= y1; ++y)
            for (i32 x = x0; x <= x1; ++x) f((static_cast<u64>(static_cast<u32>(x)) << 32) | static_cast<u32>(y));
    }

    f32 cell_, inv_;
    std::unordered_map<u64, std::vector<u32>> cells_;
    std::vector<Item> items_;
    u32 stamp_ = 0;
};

} // namespace ph::physics
