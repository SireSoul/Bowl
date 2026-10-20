#include "bowl/gfx/material.hpp"

#include "bowl/gfx/renderer.hpp"

#include <algorithm>
#include <charconv>
#include <format>

namespace bowl::gfx {

namespace {

// "#rrggbb", "#rrggbbaa", or [r, g, b(, a)] in 0..1.
std::optional<Color> color_of(const json::Value& v) {
    if (v.is_array() && v.size() >= 3)
        return Color{v[0].as_float(), v[1].as_float(), v[2].as_float(), v.size() > 3 ? v[3].as_float(1) : 1.0f};
    std::string_view s = v.as_string();
    if (!s.starts_with('#') || (s.size() != 7 && s.size() != 9)) return std::nullopt;
    u32 n = 0;
    auto [end, ec] = std::from_chars(s.data() + 1, s.data() + s.size(), n, 16);
    if (ec != std::errc{} || end != s.data() + s.size()) return std::nullopt;
    if (s.size() == 7) return Color::hex(n);
    return Color::hex(n >> 8, static_cast<f32>(n & 0xff) / 255.0f);
}

} // namespace

std::string_view blend_name(BlendMode mode) {
    switch (mode) {
    case BlendMode::Alpha: return "alpha";
    case BlendMode::Additive: return "additive";
    case BlendMode::Multiply: return "multiply";
    case BlendMode::Opaque: return "opaque";
    }
    return "alpha";
}

std::optional<BlendMode> blend_from_name(std::string_view name) {
    for (BlendMode m : {BlendMode::Alpha, BlendMode::Additive, BlendMode::Multiply, BlendMode::Opaque})
        if (blend_name(m) == name) return m;
    return std::nullopt;
}

std::string_view shadow_name(Shadow shadow) {
    switch (shadow) {
    case Shadow::None: return "none";
    case Shadow::Shape: return "shape";
    case Shadow::Faint: return "faint";
    }
    return "none";
}

void SpriteMaterial::set_param(std::string_view name, std::span<const f32> value) {
    auto it = params.find(name);
    if (it == params.end()) it = params.emplace(std::string(name), std::vector<f32>{}).first;
    it->second.assign(value.begin(), value.end());
    stale_ = true;
}

void SpriteMaterial::build() const {
    const Shader* s = shader.get();
    look_.fragment = shader;
    look_.blend = blend;
    look_.textures = {};
    // Hot reload replaces textures, so their pointers are looked up for every frame's draw.
    if (s)
        for (usize b = 1; b < s->textures().size() && b <= 3; ++b)
            if (auto it = textures.find(s->textures()[b].name); it != textures.end()) look_.textures[b - 1] = it->second.get();
    if (!stale_ && s == built_shader_ && shader.version() == built_version_) return;
    stale_ = false;
    built_shader_ = s;
    built_version_ = shader.version();
    look_.uniforms.clear();
    if (!s || s->block_size() == 0) return;
    look_.uniforms = s->defaults();
    for (const auto& [name, value] : params)
        if (const ShaderBundle::Uniform* u = s->uniform(name)) ShaderBundle::write(look_.uniforms, *u, value);
}

const Material* SpriteMaterial::look() const {
    if (!shader && !blend) return nullptr;
    build();
    return &look_;
}

const Material* SpriteMaterial::glow_look() const {
    if (!glows()) return nullptr;
    glow_look_.fragment = glow_shader_;
    return &glow_look_;
}

std::unique_ptr<SpriteMaterial> parse_material(const json::Value& doc, AssetManager& assets,
                                               std::vector<std::string>* warnings) {
    auto warn = [&](std::string message) {
        if (warnings) warnings->push_back(std::move(message));
    };
    auto m = std::make_unique<SpriteMaterial>();
    m->glow_shader_ = assets.load<Shader>("engine/shaders/emissive.frag");
    if (!doc.is_object()) {
        warn("a material is a JSON object");
        return m;
    }
    if (std::string_view path = doc["shader"].as_string(); !path.empty()) {
        m->shader = assets.load<Shader>(path);
        if (!m->shader) warn(std::format("the shader '{}' didn't load", path));
    }
    for (const auto& [name, value] : doc["params"].object()) {
        std::vector<f32> numbers;
        if (value.is_number()) numbers.push_back(value.as_float());
        else if (value.is_bool()) numbers.push_back(value.as_bool() ? 1.0f : 0.0f);
        else if (auto c = color_of(value); c && value.is_string()) numbers = {c->r, c->g, c->b, c->a};
        else if (value.is_array())
            for (const json::Value& n : value.array()) numbers.push_back(n.as_float());
        else {
            warn(std::format("the parameter '{}' isn't a number, a list of them or a color", name));
            continue;
        }
        if (m->shader && !m->shader->uniform(name)) warn(std::format("the shader has no parameter '{}'", name));
        m->params[name] = std::move(numbers);
    }
    for (const auto& [name, value] : doc["textures"].object()) {
        std::string_view path = value.as_string();
        if (path.empty()) continue;
        m->textures[name] = assets.load<Texture>(path);
        if (m->shader) {
            const auto& slots = m->shader->textures();
            bool known = std::any_of(slots.begin() + std::min<usize>(1, slots.size()), slots.end(),
                                     [&](const ShaderBundle::TextureSlot& t) { return t.name == name; });
            if (!known) warn(std::format("the shader has no texture '{}' (its extra textures are at bindings 1 to 3)", name));
        }
    }
    if (const json::Value* b = doc.find("blend")) {
        m->blend = blend_from_name(b->as_string());
        if (!m->blend) warn(std::format("'{}' isn't a blend mode: alpha, additive, multiply or opaque", b->as_string()));
    }
    const json::Value& emissive = doc["emissive"];
    if (emissive.is_object()) {
        if (std::string_view map = emissive["map"].as_string(); !map.empty()) m->emissive = assets.load<Texture>(map);
        if (const json::Value* c = emissive.find("color")) {
            if (auto color = color_of(*c)) m->emissive_color = *color;
            else warn("the emissive color is \"#rrggbb\" or [r, g, b]");
        }
        m->emissive_strength = std::max(0.0f, emissive["strength"].as_float(1));
    } else if (std::string_view map = emissive.as_string(); !map.empty()) { // just the map
        m->emissive = assets.load<Texture>(map);
        m->emissive_strength = 1;
    }
    if (std::string_view normal = doc["normal"].as_string(); !normal.empty()) m->normal = assets.load<Texture>(normal);
    if (const json::Value* s = doc.find("shadow")) {
        std::string_view name = s->as_string();
        if (name == "shape") m->shadow = Shadow::Shape;
        else if (name == "faint") m->shadow = Shadow::Faint;
        else if (name != "none") warn(std::format("'{}' isn't a shadow: none, shape or faint", name));
    }
    m->preview = std::string(doc["preview"].as_string());
    return m;
}

// SpriteLayers

void SpriteLayers::begin(const Camera2D& camera, SpriteSort sort, BlendMode blend) {
    scene.begin(camera, sort, blend);
    emissive.begin(camera, sort);
    normals.begin(camera, sort);
    occluders.begin(camera, SpriteSort::None);
}

void SpriteLayers::draw(const Texture* texture, const SpriteInstance& sprite, const SpriteMaterial* material) {
    scene.draw(texture, sprite, material ? material->look() : nullptr);
    if (!material) return;
    if (material->glows()) {
        SpriteInstance g = sprite;
        Color c = material->glow();
        g.color = {c.r, c.g, c.b, sprite.color.a};
        emissive.draw(material->emissive ? material->emissive.get() : texture, g, material->glow_look());
    }
    if (material->normal) {
        SpriteInstance n = sprite;
        n.color = Color::white().with_alpha(sprite.color.a); // a tint mustn't bend the normals
        normals.draw(material->normal.get(), n);
    }
    if (material->shadow != Shadow::None) {
        SpriteInstance o = sprite;
        o.color = Color::white().with_alpha(material->shadow == Shadow::Shape ? 1.0f : 0.25f);
        occluders.draw(texture, o);
    }
}

void SpriteLayers::submit(Renderer& renderer) const {
    if (scene.size()) renderer.submit(scene);
    if (emissive.size()) renderer.submit_emissive(emissive);
    if (normals.size()) renderer.submit_normals(normals);
    if (occluders.size()) renderer.submit_occluders(occluders);
}

} // namespace bowl::gfx
