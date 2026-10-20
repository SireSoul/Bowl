#pragma once

#include "bowl/core/types.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bowl {

enum class ShaderStage : u32 { Vertex = 0, Fragment = 1, Compute = 2 };
// Hlsl is source (Shader Model 5.1, SDL's register layout); on Windows it's compiled to Dxbc when the D3D12 backend
// runs, since the HLSL compiler ships with Windows but not with other build hosts.
enum class ShaderBlobFormat : u32 { SpirV = 1, Msl = 2, Dxil = 3, Dxbc = 4, Hlsl = 5 };

// One shader stage compiled for every GPU backend, plus the resource counts SDL_GPU needs to create it.
// Written by bowl-shaderc (build time, or at runtime in dev builds); read by the renderer. Format: little-endian u32s.
struct ShaderBundle {
    static constexpr u32 magic = 0x48535742; // "BWSH"
    // 2 added the compute fields, 3 the uniform block's layout and texture names; older bundles still load.
    static constexpr u32 format_version = 3;

    struct Blob {
        ShaderBlobFormat format;
        std::string entry_point;
        std::vector<u8> code;
    };

    ShaderStage stage = ShaderStage::Vertex;
    u32 samplers = 0;
    u32 storage_textures = 0;
    u32 storage_buffers = 0;
    u32 uniform_buffers = 0;
    // Compute shaders: storage_textures/storage_buffers above count the read-only ones; these the read-write ones.
    u32 readwrite_storage_textures = 0;
    u32 readwrite_storage_buffers = 0;
    u32 threads[3] = {1, 1, 1}; // local_size_x/y/z
    std::vector<Blob> blobs;

    // What a material needs to fill the uniform block at binding 0 and the textures by name, read from the shader:
    // each member's place in the block, and what a comment after its declaration says about it:
    //
    //     vec4 glow;    // color = 1, 0.6, 0.2, 1   The glow's color
    //     float speed;  // = 2 [0, 10]              How fast it moves
    //
    // `color` shows it as a color, `= ...` is its value when a material doesn't set it (one number fills every
    // component; a color's missing alpha is 1), `[lo, hi]` its range, and the rest says what it's for.
    enum class ValueType : u32 { Float, Vec2, Vec3, Vec4, Int, IVec2, IVec3, IVec4, UInt, Mat4, Other };
    struct Uniform {
        std::string name;
        ValueType type = ValueType::Float;
        u32 offset = 0, size = 0; // bytes within the block (size covers every element of an array)
        u32 count = 1;            // array elements
        bool has_default = false;
        bool color = false;
        f32 value[4] = {};        // the default
        f32 min = 0, max = 0;     // a range when max > min
        std::string help;
    };
    struct TextureSlot {
        std::string name; // as the shader calls it ("u_noise")
        std::string help;
    };
    u32 block_size = 0;                // bytes in the uniform block at binding 0 (0: none)
    std::vector<Uniform> uniforms;     // its members, in order
    std::vector<TextureSlot> textures; // sampled textures, by binding

    const Uniform* uniform(std::string_view name) const {
        for (const Uniform& u : uniforms)
            if (u.name == name) return &u;
        return nullptr;
    }

    // Numbers in a value of this type (16 for a mat4; 0 for types a material can't set).
    static u32 components(ValueType t) {
        switch (t) {
        case ValueType::Float: case ValueType::Int: case ValueType::UInt: return 1;
        case ValueType::Vec2: case ValueType::IVec2: return 2;
        case ValueType::Vec3: case ValueType::IVec3: return 3;
        case ValueType::Vec4: case ValueType::IVec4: return 4;
        case ValueType::Mat4: return 16;
        case ValueType::Other: return 0;
        }
        return 0;
    }
    // Writes a value (as many numbers as its type has; missing ones are 0) into a uniform block at its offset.
    static void write(std::span<u8> block, const Uniform& u, std::span<const f32> values) {
        u32 n = components(u.type);
        if (u.offset + n * 4 > block.size()) return;
        bool integer = u.type == ValueType::Int || u.type == ValueType::IVec2 || u.type == ValueType::IVec3 ||
                       u.type == ValueType::IVec4 || u.type == ValueType::UInt;
        for (u32 i = 0; i < n; ++i) {
            f32 v = i < values.size() ? values[i] : 0.0f;
            u32 bits = !integer ? std::bit_cast<u32>(v)
                       : u.type == ValueType::UInt ? static_cast<u32>(std::max(v, 0.0f) + 0.5f)
                                                   : std::bit_cast<u32>(static_cast<i32>(v < 0 ? v - 0.5f : v + 0.5f));
            std::memcpy(block.data() + u.offset + i * 4, &bits, 4);
        }
    }

    const Blob* find(ShaderBlobFormat f) const {
        for (const Blob& b : blobs)
            if (b.format == f) return &b;
        return nullptr;
    }

    std::vector<u8> serialize() const {
        std::vector<u8> out;
        auto put = [&](u32 v) { for (int i = 0; i < 4; ++i) out.push_back(static_cast<u8>(v >> (8 * i))); };
        auto put_bytes = [&](const void* p, usize n) {
            put(static_cast<u32>(n));
            out.insert(out.end(), static_cast<const u8*>(p), static_cast<const u8*>(p) + n);
        };
        put(magic);
        put(format_version);
        put(static_cast<u32>(stage));
        put(samplers);
        put(storage_textures);
        put(storage_buffers);
        put(uniform_buffers);
        put(readwrite_storage_textures);
        put(readwrite_storage_buffers);
        for (u32 t : threads) put(t);
        put(static_cast<u32>(blobs.size()));
        for (const Blob& b : blobs) {
            put(static_cast<u32>(b.format));
            put_bytes(b.entry_point.data(), b.entry_point.size());
            put_bytes(b.code.data(), b.code.size());
        }
        put(block_size);
        put(static_cast<u32>(uniforms.size()));
        for (const Uniform& u : uniforms) {
            put_bytes(u.name.data(), u.name.size());
            put(static_cast<u32>(u.type));
            put(u.offset);
            put(u.size);
            put(u.count);
            put((u.has_default ? 1u : 0u) | (u.color ? 2u : 0u));
            for (f32 v : u.value) put(std::bit_cast<u32>(v));
            put(std::bit_cast<u32>(u.min));
            put(std::bit_cast<u32>(u.max));
            put_bytes(u.help.data(), u.help.size());
        }
        put(static_cast<u32>(textures.size()));
        for (const TextureSlot& t : textures) {
            put_bytes(t.name.data(), t.name.size());
            put_bytes(t.help.data(), t.help.size());
        }
        return out;
    }

    static std::optional<ShaderBundle> parse(std::span<const u8> data) {
        usize at = 0;
        bool ok = true;
        auto get = [&]() -> u32 {
            if (at + 4 > data.size()) { ok = false; return 0; }
            u32 v = u32(data[at]) | u32(data[at + 1]) << 8 | u32(data[at + 2]) << 16 | u32(data[at + 3]) << 24;
            at += 4;
            return v;
        };
        auto get_bytes = [&](u32 n) -> std::span<const u8> {
            if (!ok || at + n > data.size()) { ok = false; return {}; }
            auto s = data.subspan(at, n);
            at += n;
            return s;
        };
        if (get() != magic) return std::nullopt;
        u32 version = get();
        if (version < 1 || version > format_version) return std::nullopt;
        ShaderBundle b;
        b.stage = static_cast<ShaderStage>(get());
        b.samplers = get();
        b.storage_textures = get();
        b.storage_buffers = get();
        b.uniform_buffers = get();
        if (version >= 2) {
            b.readwrite_storage_textures = get();
            b.readwrite_storage_buffers = get();
            for (u32& t : b.threads) t = get();
        }
        u32 count = get();
        for (u32 i = 0; ok && i < count; ++i) {
            Blob blob;
            blob.format = static_cast<ShaderBlobFormat>(get());
            auto entry = get_bytes(get());
            blob.entry_point.assign(entry.begin(), entry.end());
            auto code = get_bytes(get());
            blob.code.assign(code.begin(), code.end());
            b.blobs.push_back(std::move(blob));
        }
        auto get_string = [&] {
            auto bytes = get_bytes(get());
            return std::string(bytes.begin(), bytes.end());
        };
        if (version >= 3) {
            b.block_size = get();
            u32 n = get();
            for (u32 i = 0; ok && i < n; ++i) {
                Uniform u;
                u.name = get_string();
                u.type = static_cast<ValueType>(std::min(get(), static_cast<u32>(ValueType::Other)));
                u.offset = get();
                u.size = get();
                u.count = get();
                u32 flags = get();
                u.has_default = flags & 1;
                u.color = flags & 2;
                for (f32& v : u.value) v = std::bit_cast<f32>(get());
                u.min = std::bit_cast<f32>(get());
                u.max = std::bit_cast<f32>(get());
                u.help = get_string();
                b.uniforms.push_back(std::move(u));
            }
            n = get();
            for (u32 i = 0; ok && i < n; ++i) {
                TextureSlot t;
                t.name = get_string();
                t.help = get_string();
                b.textures.push_back(std::move(t));
            }
        }
        if (!ok) return std::nullopt;
        return b;
    }
};

} // namespace bowl
