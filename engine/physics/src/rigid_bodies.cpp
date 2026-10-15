#include "ph/physics/rigid_bodies.hpp"

#include "ph/core/log.hpp"

#include <box2d/box2d.h>

#include <algorithm>
#include <unordered_map>

namespace ph::physics {

namespace {

// What step_physics keeps between steps: the Box2D world, and for each body what was last written to the entity, so
// changes the game made since are noticed and pushed back in.
struct Body {
    b2BodyId id;
    Vec2 position, velocity;
    f32 rotation = 0, angular_velocity = 0;
    std::string collider; // the collider's JSON when the shapes were made
    RigidBody::Type type;
    RigidBody settings;   // as last applied
};

struct Box2DState {
    b2WorldId world = b2_nullWorldId;
    std::unordered_map<u64, Body> bodies;
    bool warned_parent = false;

    Box2DState() {
        b2WorldDef def = b2DefaultWorldDef();
        world = b2CreateWorld(&def);
    }
    ~Box2DState() {
        if (b2World_IsValid(world)) b2DestroyWorld(world);
    }
    Box2DState(const Box2DState&) = delete;
    Box2DState& operator=(const Box2DState&) = delete;
};

b2BodyType type_of(RigidBody::Type t) {
    switch (t) {
    case RigidBody::Type::Static: return b2_staticBody;
    case RigidBody::Type::Kinematic: return b2_kinematicBody;
    default: return b2_dynamicBody;
    }
}

std::string collider_key(const Collider& c, Vec2 scale) {
    serial::Context ctx;
    json::Value j = serial::to_json(c, ctx);
    j.set("scale", serial::to_json(scale, ctx));
    return json::stringify(j, -1);
}

void make_shapes(b2BodyId body, const Collider& c, const RigidBody& rb, Vec2 scale, f32 ppm) {
    b2ShapeDef def = b2DefaultShapeDef();
    def.density = rb.density;
    def.material.friction = rb.friction;
    def.material.restitution = rb.restitution;
    def.filter.categoryBits = c.layer;
    def.filter.maskBits = c.mask;
    def.isSensor = c.trigger;
    auto m = [&](Vec2 v) { return b2Vec2{v.x / ppm, v.y / ppm}; };
    Vec2 offset = c.offset * scale;
    f32 s = std::max(std::abs(scale.x), std::abs(scale.y));
    switch (c.kind) {
    case Collider::Kind::Circle: {
        b2Circle circle{m(offset), c.radius * s / ppm};
        b2CreateCircleShape(body, &def, &circle);
        break;
    }
    case Collider::Kind::Capsule: {
        Vec2 half{0, c.length * 0.5f * scale.y};
        b2Capsule capsule{m(offset - half), m(offset + half), c.radius * std::abs(scale.x) / ppm};
        b2CreateCapsuleShape(body, &def, &capsule);
        break;
    }
    case Collider::Kind::Polygon: {
        std::vector<b2Vec2> points;
        for (Vec2 p : c.points) points.push_back(m(offset + p * scale));
        if (points.size() > B2_MAX_POLYGON_VERTICES) points.resize(B2_MAX_POLYGON_VERTICES);
        b2Hull hull = b2ComputeHull(points.data(), static_cast<int>(points.size()));
        if (hull.count < 3) {
            warn("physics: a polygon collider isn't convex or has too few points; skipped");
            break;
        }
        b2Polygon polygon = b2MakePolygon(&hull, 0);
        b2CreatePolygonShape(body, &def, &polygon);
        break;
    }
    case Collider::Kind::Box:
    default: {
        b2Polygon box = b2MakeOffsetBox(std::abs(c.size.x * scale.x) * 0.5f / ppm, std::abs(c.size.y * scale.y) * 0.5f / ppm,
                                        m(offset), b2Rot_identity);
        b2CreatePolygonShape(body, &def, &box);
        break;
    }
    }
}

} // namespace

void step_physics(ecs::World& world, f32 dt) {
    PhysicsSettings* settings = world.try_resource<PhysicsSettings>();
    if (!settings) settings = &world.set_resource<PhysicsSettings>();
    Box2DState* state = world.try_resource<Box2DState>();
    if (!state) state = &world.set_resource<Box2DState>();
    f32 ppm = settings->pixels_per_meter;
    b2World_SetGravity(state->world, {settings->gravity.x / ppm, settings->gravity.y / ppm});

    // Bodies whose entity died or lost its RigidBody or Collider go.
    for (auto it = state->bodies.begin(); it != state->bodies.end();) {
        ecs::Entity e = ecs::Entity::from_bits(it->first);
        if (!world.alive(e) || !world.has<RigidBody>(e) || !world.has<Collider>(e)) {
            b2DestroyBody(it->second.id);
            it = state->bodies.erase(it);
        } else {
            ++it;
        }
    }

    // New bodies, and changes the game made since the last step.
    world.each<const RigidBody, const Collider, const ecs::Transform2D>(
        [&](ecs::Entity e, const RigidBody& rb, const Collider& c, const ecs::Transform2D& t) {
            if (world.has<ecs::Parent>(e)) {
                if (!state->warned_parent) warn("physics: rigid bodies must be top-level entities; skipping children");
                state->warned_parent = true;
                return;
            }
            std::string key = collider_key(c, t.scale);
            auto found = state->bodies.find(e.bits());
            if (found != state->bodies.end() && (found->second.collider != key || found->second.type != rb.type)) {
                b2DestroyBody(found->second.id); // a different shape or kind: build it again
                state->bodies.erase(found);
                found = state->bodies.end();
            }
            if (found == state->bodies.end()) {
                b2BodyDef def = b2DefaultBodyDef();
                def.type = type_of(rb.type);
                def.position = {t.position.x / ppm, t.position.y / ppm};
                def.rotation = b2MakeRot(t.rotation);
                def.linearVelocity = {rb.velocity.x / ppm, rb.velocity.y / ppm};
                def.angularVelocity = rb.angular_velocity;
                def.linearDamping = rb.linear_damping;
                def.angularDamping = rb.angular_damping;
                def.gravityScale = rb.gravity_scale;
                def.fixedRotation = rb.fixed_rotation;
                def.isBullet = rb.bullet;
                def.userData = reinterpret_cast<void*>(static_cast<uintptr_t>(e.bits()));
                b2BodyId id = b2CreateBody(state->world, &def);
                make_shapes(id, c, rb, t.scale, ppm);
                state->bodies[e.bits()] = {id, t.position, rb.velocity, t.rotation, rb.angular_velocity, std::move(key), rb.type, rb};
                return;
            }
            Body& b = found->second;
            if (t.position != b.position || t.rotation != b.rotation) // moved by the game: put it there
                b2Body_SetTransform(b.id, {t.position.x / ppm, t.position.y / ppm}, b2MakeRot(t.rotation));
            if (rb.velocity != b.velocity) b2Body_SetLinearVelocity(b.id, {rb.velocity.x / ppm, rb.velocity.y / ppm});
            if (rb.angular_velocity != b.angular_velocity) b2Body_SetAngularVelocity(b.id, rb.angular_velocity);
            // Settings only when they change: some, like fixed rotation, make Box2D recompute the mass.
            if (rb.linear_damping != b.settings.linear_damping) b2Body_SetLinearDamping(b.id, rb.linear_damping);
            if (rb.angular_damping != b.settings.angular_damping) b2Body_SetAngularDamping(b.id, rb.angular_damping);
            if (rb.gravity_scale != b.settings.gravity_scale) b2Body_SetGravityScale(b.id, rb.gravity_scale);
            if (rb.fixed_rotation != b.settings.fixed_rotation) b2Body_SetFixedRotation(b.id, rb.fixed_rotation);
            if (rb.bullet != b.settings.bullet) b2Body_SetBullet(b.id, rb.bullet);
            b.settings = rb;
        });

    if (dt > 0) b2World_Step(state->world, dt, std::max(1, settings->sub_steps));

    // Results back into the entities.
    for (auto& [bits, b] : state->bodies) {
        ecs::Entity e = ecs::Entity::from_bits(bits);
        auto* t = world.try_get<ecs::Transform2D>(e);
        auto* rb = world.try_get<RigidBody>(e);
        if (!t || !rb) continue;
        b2Vec2 p = b2Body_GetPosition(b.id);
        b2Vec2 v = b2Body_GetLinearVelocity(b.id);
        t->position = b.position = {p.x * ppm, p.y * ppm};
        t->rotation = b.rotation = b2Rot_GetAngle(b2Body_GetRotation(b.id));
        rb->velocity = b.velocity = {v.x * ppm, v.y * ppm};
        rb->angular_velocity = b.angular_velocity = b2Body_GetAngularVelocity(b.id);
    }
}

void apply_impulse(ecs::World& world, ecs::Entity e, Vec2 impulse) {
    Box2DState* state = world.try_resource<Box2DState>();
    PhysicsSettings* settings = world.try_resource<PhysicsSettings>();
    if (!state || !settings) return;
    auto it = state->bodies.find(e.bits());
    if (it == state->bodies.end()) return;
    f32 ppm = settings->pixels_per_meter;
    b2Body_ApplyLinearImpulseToCenter(it->second.id, {impulse.x / ppm, impulse.y / ppm}, true);
}

} // namespace ph::physics
