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

struct JointRecord {
    b2JointId id;
    std::string key; // the Joint's JSON when it was made
};

struct Box2DState {
    b2WorldId world = b2_nullWorldId;
    std::unordered_map<u64, Body> bodies;
    std::unordered_map<u64, JointRecord> joints;
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
    def.enableContactEvents = !c.trigger;
    def.enableHitEvents = !c.trigger;
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

std::string joint_key(const Joint& j) {
    serial::Context ctx;
    std::unordered_map<u64, u64> ids; // entity references written as their own bits, so the key is stable
    ids[j.a.bits()] = j.a.bits();
    ids[j.b.bits()] = j.b.bits();
    ctx.entity_map = &ids;
    return json::stringify(serial::to_json(j, ctx), -1);
}

b2JointId make_joint(b2WorldId world, b2BodyId a, b2BodyId b, const Joint& j, f32 ppm) {
    auto m = [&](Vec2 v) { return b2Vec2{v.x / ppm, v.y / ppm}; };
    b2Vec2 axis = b2Normalize({j.axis.x, j.axis.y});
    switch (j.kind) {
    case Joint::Kind::Distance: {
        b2DistanceJointDef d = b2DefaultDistanceJointDef();
        d.bodyIdA = a;
        d.bodyIdB = b;
        d.localAnchorA = m(j.anchor_a);
        d.localAnchorB = m(j.anchor_b);
        if (j.length > 0) {
            d.length = j.length / ppm;
        } else { // the distance between the anchors right now
            b2Vec2 pa = b2Body_GetWorldPoint(a, d.localAnchorA), pb = b2Body_GetWorldPoint(b, d.localAnchorB);
            d.length = std::max(b2Distance(pa, pb), 0.01f);
        }
        // A rope is a spring with no stiffness held between min and max: it only pulls once taut.
        d.enableSpring = j.spring_hertz > 0 || j.max_length > 0;
        d.hertz = j.spring_hertz;
        d.dampingRatio = j.spring_damping;
        d.enableLimit = j.max_length > 0;
        d.minLength = j.min_length / ppm;
        if (j.max_length > 0) d.maxLength = j.max_length / ppm; // otherwise the default: no limit
        d.enableMotor = j.motor;
        d.motorSpeed = j.motor_speed / ppm;
        d.maxMotorForce = j.max_motor_force;
        d.collideConnected = j.collide;
        return b2CreateDistanceJoint(world, &d);
    }
    case Joint::Kind::Prismatic: {
        b2PrismaticJointDef d = b2DefaultPrismaticJointDef();
        d.bodyIdA = a;
        d.bodyIdB = b;
        d.localAnchorA = m(j.anchor_a);
        d.localAnchorB = m(j.anchor_b);
        d.localAxisA = axis;
        d.referenceAngle = b2RelativeAngle(b2Body_GetRotation(b), b2Body_GetRotation(a));
        d.enableSpring = j.spring_hertz > 0;
        d.hertz = j.spring_hertz;
        d.dampingRatio = j.spring_damping;
        d.enableLimit = j.limit;
        d.lowerTranslation = j.lower / ppm;
        d.upperTranslation = j.upper / ppm;
        d.enableMotor = j.motor;
        d.motorSpeed = j.motor_speed / ppm;
        d.maxMotorForce = j.max_motor_force;
        d.collideConnected = j.collide;
        return b2CreatePrismaticJoint(world, &d);
    }
    case Joint::Kind::Weld: {
        b2WeldJointDef d = b2DefaultWeldJointDef();
        d.bodyIdA = a;
        d.bodyIdB = b;
        d.localAnchorA = m(j.anchor_a);
        d.localAnchorB = m(j.anchor_b);
        d.referenceAngle = b2RelativeAngle(b2Body_GetRotation(b), b2Body_GetRotation(a));
        d.linearHertz = d.angularHertz = j.spring_hertz;
        d.linearDampingRatio = d.angularDampingRatio = j.spring_damping;
        d.collideConnected = j.collide;
        return b2CreateWeldJoint(world, &d);
    }
    case Joint::Kind::Wheel: {
        b2WheelJointDef d = b2DefaultWheelJointDef();
        d.bodyIdA = a;
        d.bodyIdB = b;
        d.localAnchorA = m(j.anchor_a);
        d.localAnchorB = m(j.anchor_b);
        d.localAxisA = axis;
        d.enableSpring = j.spring_hertz > 0;
        d.hertz = j.spring_hertz;
        d.dampingRatio = j.spring_damping;
        d.enableLimit = j.limit;
        d.lowerTranslation = j.lower / ppm;
        d.upperTranslation = j.upper / ppm;
        d.enableMotor = j.motor;
        d.motorSpeed = j.motor_speed;
        d.maxMotorTorque = j.max_motor_force;
        d.collideConnected = j.collide;
        return b2CreateWheelJoint(world, &d);
    }
    case Joint::Kind::Revolute:
    default: {
        b2RevoluteJointDef d = b2DefaultRevoluteJointDef();
        d.bodyIdA = a;
        d.bodyIdB = b;
        d.localAnchorA = m(j.anchor_a);
        d.localAnchorB = m(j.anchor_b);
        d.referenceAngle = b2RelativeAngle(b2Body_GetRotation(b), b2Body_GetRotation(a));
        d.enableSpring = j.spring_hertz > 0;
        d.hertz = j.spring_hertz;
        d.dampingRatio = j.spring_damping;
        d.enableLimit = j.limit;
        d.lowerAngle = j.lower;
        d.upperAngle = j.upper;
        d.enableMotor = j.motor;
        d.motorSpeed = j.motor_speed;
        d.maxMotorTorque = j.max_motor_force;
        d.collideConnected = j.collide;
        return b2CreateRevoluteJoint(world, &d);
    }
    }
}

ecs::Entity entity_of(b2ShapeId shape) {
    if (!b2Shape_IsValid(shape)) return ecs::null_entity;
    return ecs::Entity::from_bits(static_cast<u64>(reinterpret_cast<uintptr_t>(b2Body_GetUserData(b2Shape_GetBody(shape)))));
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

    // Joints: gone with their entity, their component or either body (Box2D drops a body's joints with it, including
    // when a changed body is rebuilt above); rebuilt when changed; made once both bodies exist.
    for (auto it = state->joints.begin(); it != state->joints.end();) {
        ecs::Entity e = ecs::Entity::from_bits(it->first);
        const Joint* j = world.alive(e) ? world.try_get<const Joint>(e) : nullptr;
        bool keep = j && b2Joint_IsValid(it->second.id) && joint_key(*j) == it->second.key;
        if (!keep) {
            if (b2Joint_IsValid(it->second.id)) b2DestroyJoint(it->second.id);
            it = state->joints.erase(it);
        } else {
            ++it;
        }
    }
    world.each<const Joint>([&](ecs::Entity e, const Joint& j) {
        if (state->joints.contains(e.bits())) return;
        auto a = state->bodies.find(j.a.bits()), b = state->bodies.find(j.b.bits());
        if (a == state->bodies.end() || b == state->bodies.end() || j.a == j.b) return;
        state->joints[e.bits()] = {make_joint(state->world, a->second.id, b->second.id, j, ppm), joint_key(j)};
    });

    b2World_SetHitEventThreshold(state->world, settings->hit_speed / ppm);
    if (dt > 0) b2World_Step(state->world, dt, std::max(1, settings->sub_steps));

    // Contacts, in a stable order.
    ContactEvents events;
    b2ContactEvents contacts = b2World_GetContactEvents(state->world);
    for (int i = 0; i < contacts.beginCount; ++i) {
        const b2ContactBeginTouchEvent& c = contacts.beginEvents[i];
        Vec2 point = c.manifold.pointCount > 0 ? Vec2{c.manifold.points[0].point.x, c.manifold.points[0].point.y} * ppm : Vec2{};
        events.began.push_back({entity_of(c.shapeIdA), entity_of(c.shapeIdB), point, {c.manifold.normal.x, c.manifold.normal.y}});
    }
    for (int i = 0; i < contacts.endCount; ++i)
        events.ended.push_back({entity_of(contacts.endEvents[i].shapeIdA), entity_of(contacts.endEvents[i].shapeIdB), {}, {}});
    for (int i = 0; i < contacts.hitCount; ++i) {
        const b2ContactHitEvent& h = contacts.hitEvents[i];
        events.hits.push_back({entity_of(h.shapeIdA), entity_of(h.shapeIdB), Vec2{h.point.x, h.point.y} * ppm,
                               {h.normal.x, h.normal.y}, h.approachSpeed * ppm});
    }
    auto order = [](const auto& x, const auto& y) {
        return x.a.bits() != y.a.bits() ? x.a.bits() < y.a.bits() : x.b.bits() < y.b.bits();
    };
    std::sort(events.began.begin(), events.began.end(), order);
    std::sort(events.ended.begin(), events.ended.end(), order);
    std::sort(events.hits.begin(), events.hits.end(), order);
    world.set_resource<ContactEvents>(std::move(events));

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
