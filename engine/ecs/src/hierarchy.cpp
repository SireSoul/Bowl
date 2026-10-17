#include "bowl/ecs/hierarchy.hpp"

#include <algorithm>

namespace bowl::ecs {

namespace {

void unlink(World& world, Entity child) {
    Parent* p = world.try_get<Parent>(child);
    if (!p) return;
    if (Children* siblings = world.try_get<Children>(p->entity)) std::erase(siblings->list, child);
    world.remove<Parent>(child);
}

GlobalTransform2D compose(const GlobalTransform2D& parent, const Transform2D& local) {
    return {parent.apply(local.position), parent.rotation + local.rotation, parent.scale * local.scale};
}

constexpr int max_depth = 1024; // deeper than this is corrupted data with a cycle

void propagate(World& world, Entity e, const GlobalTransform2D& parent_global, int depth) {
    if (depth > max_depth) return; // stop instead of overflowing the stack
    const Transform2D* local = world.try_get<Transform2D>(e);
    GlobalTransform2D* global = world.try_get<GlobalTransform2D>(e);
    if (!local || !global) return;
    *global = compose(parent_global, *local);
    if (const Children* kids = world.try_get<Children>(e))
        for (Entity child : kids->list) propagate(world, child, *global, depth + 1);
}

} // namespace

bool set_parent(World& world, Entity child, Entity parent) {
    if (!world.alive(child) || child == parent) return false;
    if (parent) {
        if (!world.alive(parent)) return false;
        // Walking up from the new parent must not reach the child (nor go round a cycle already there).
        int depth = 0;
        for (Entity up = parent; up; ++depth) {
            if (up == child || depth > max_depth) return false;
            const Parent* p = world.try_get<Parent>(up);
            up = p ? p->entity : null_entity;
        }
    }
    unlink(world, child);
    if (!parent) return true;
    world.add<Parent>(child, parent);
    if (Children* kids = world.try_get<Children>(parent)) kids->list.push_back(child);
    else world.add<Children>(parent, std::vector<Entity>{child});
    return true;
}

namespace {
void destroy_subtree(World& world, Entity root, int depth) {
    if (!world.alive(root)) return;
    if (const Children* kids = world.try_get<Children>(root); kids && depth < max_depth) {
        std::vector<Entity> copy = kids->list;
        world.remove<Children>(root); // a cycle back here finds nothing more to follow
        for (Entity c : copy) destroy_subtree(world, c, depth + 1);
    }
    unlink(world, root);
    world.destroy(root);
}
} // namespace

void destroy_recursive(World& world, Entity root) { destroy_subtree(world, root, 0); }

void propagate_transforms(World& world) {
    // Make sure everything with a Transform2D has somewhere to write its global transform.
    std::vector<Entity> missing;
    world.query<const Transform2D>(without<GlobalTransform2D>()).each([&](Entity e, const Transform2D&) { missing.push_back(e); });
    for (Entity e : missing) world.add<GlobalTransform2D>(e);

    // Roots: no parent, or one that's gone (destroyed without destroy_recursive): those count from the origin now.
    std::vector<Entity> roots;
    world.query<const Transform2D>(without<Parent>()).each([&](Entity e, const Transform2D&) { roots.push_back(e); });
    world.query<const Transform2D, const Parent>().each([&](Entity e, const Transform2D&, const Parent& p) {
        if (!world.alive(p.entity)) roots.push_back(e);
    });
    for (Entity root : roots) propagate(world, root, GlobalTransform2D{}, 0);
}

void add_transform_systems(Schedule& schedule) {
    schedule.add("propagate transforms", [](SystemContext& ctx) { propagate_transforms(ctx.world); });
}

} // namespace bowl::ecs
