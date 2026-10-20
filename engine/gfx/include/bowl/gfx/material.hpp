#pragma once

#include "bowl/core/assets.hpp"
#include "bowl/core/json.hpp"
#include "bowl/gfx/sprites.hpp"

#include <array>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bowl::gfx {

class Renderer;

// How a material's sprites stand in the way of light_material lights.
enum class Shadow : u8 {
    None,  // light passes through
    Shape, // the sprite's shape blocks light
    Faint, // blocks nothing and is never shadowed itself: the upright part of something whose footprint casts the shadow
};

// A sprite's whole look, from a .material file (Bowl Editor's Materials workspace makes them):
//
//     {
//       "shader": "shaders/water.frag",                       // its own fragment shader; none: the sprite shader
//       "params": {"speed": 1.5, "tint": [0.4, 0.8, 1, 1]},   // the shader's uniforms, by name
//       "textures": {"u_noise": "textures/noise.png"},        // its extra textures, by name
//       "blend": "alpha",                                     // alpha, additive, multiply or opaque
//       "emissive": {"map": "props/lamp_emissive.png", "color": "#ffd890", "strength": 3},
//       "normal": "props/lamp_normal.png",
//       "shadow": "shape"                                     // none, shape or faint
//     }
//
// Every key is optional. Parameters the file doesn't set take the defaults the shader's comments give (see
// shaders.md). The emissive and normal maps line up with the sprite's own texture (the same layout, at any size), so
// sprite sheet frames and atlas regions find their parts. The emissive map glows over the lit scene where it's bright
// (black glows nowhere) in `color` times `strength`; with no map, a strength above 0 makes the sprite itself glow where
// it's bright. Load it as an asset; it hot-reloads, and so do the textures and shader it names:
//
//     Asset<gfx::SpriteMaterial> lamp = engine.assets().load<gfx::SpriteMaterial>("props/lamp.material");
//     world.create(ecs::Transform2D{{40, 80}}, gfx::Sprite{.texture = lamp_png, .material = lamp});
class SpriteMaterial {
public:
    Asset<Shader> shader;
    std::map<std::string, std::vector<f32>, std::less<>> params; // by uniform name; vectors and colors by component
    std::map<std::string, Asset<Texture>, std::less<>> textures;  // the shader's extra textures, by its names for them
    std::optional<BlendMode> blend;
    Asset<Texture> emissive;
    Color emissive_color = Color::white();
    f32 emissive_strength = 0;
    Asset<Texture> normal;
    Shadow shadow = Shadow::None;
    std::string preview; // the picture Bowl Editor shows it on; games ignore it

    // What its sprites draw with in the scene: null when it changes nothing there (the sprite shader, alpha blending).
    // Valid while the material lives.
    const Material* look() const;
    // What its glow draws with in the emissive layer (engine/shaders/emissive.frag); null when it doesn't glow.
    const Material* glow_look() const;
    bool glows() const { return emissive_strength > 0 && glow_shader_; }
    Color glow() const {
        return {emissive_color.r * emissive_strength, emissive_color.g * emissive_strength,
                emissive_color.b * emissive_strength, 1};
    }

    // Sets a parameter (one number, or a vector's or color's components) for the next draw.
    void set_param(std::string_view name, std::span<const f32> value);

private:
    friend std::unique_ptr<SpriteMaterial> parse_material(const json::Value&, AssetManager&, std::vector<std::string>*);
    void build() const;
    Asset<Shader> glow_shader_;
    mutable Material look_, glow_look_;
    mutable u32 built_version_ = ~0u;
    mutable const Shader* built_shader_ = nullptr;
    mutable bool stale_ = true;
};

// Reads a material from its JSON; the textures and shader it names load through `assets`. Problems (an unknown blend
// mode, a parameter the shader doesn't have) go in `warnings` when given; the material is made anyway.
std::unique_ptr<SpriteMaterial> parse_material(const json::Value& doc, AssetManager& assets,
                                               std::vector<std::string>* warnings = nullptr);

// The four layers a scene's sprites draw into, so their materials' glow, normal maps and shadows reach the renderer:
//
//     layers.begin(engine.camera());               // a member: it reuses its memory
//     gfx::draw_maps(world, layers.scene, time);
//     gfx::draw_sprites(world, layers);
//     layers.submit(engine.renderer());
//
// The glow needs nothing else; normal maps and shadows show under lights drawn with light_material.
struct SpriteLayers {
    SpriteBatch scene, emissive, normals, occluders;

    void begin(const Camera2D& camera, SpriteSort sort = SpriteSort::Depth, BlendMode blend = BlendMode::Alpha);
    // A sprite in its material's look (null: the plain sprite), with its glow, normal map and shadow.
    void draw(const Texture* texture, const SpriteInstance& sprite, const SpriteMaterial* material);
    // Submits each layer that has something in it.
    void submit(Renderer& renderer) const;
};

// The name of a blend mode in material files ("alpha", "additive", "multiply", "opaque"), and back.
std::string_view blend_name(BlendMode mode);
std::optional<BlendMode> blend_from_name(std::string_view name);
std::string_view shadow_name(Shadow shadow);

} // namespace bowl::gfx
