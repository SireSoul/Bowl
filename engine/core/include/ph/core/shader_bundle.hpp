#pragma once

#include "ph/core/types.hpp"

#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace ph {

enum class ShaderStage : u32 { Vertex = 0, Fragment = 1, Compute = 2 };
// Hlsl is source (Shader Model 5.1, SDL's register layout); on Windows it's compiled to Dxbc when the D3D12 backend
// runs, since the HLSL compiler ships with Windows but not with other build hosts.
enum class ShaderBlobFormat : u32 { SpirV = 1, Msl = 2, Dxil = 3, Dxbc = 4, Hlsl = 5 };

// One shader stage compiled for every GPU backend, plus the resource counts SDL_GPU needs to create it.
// Written by ph-shaderc (build time, or at runtime in dev builds); read by the renderer. Format: little-endian u32s.
struct ShaderBundle {
    static constexpr u32 magic = 0x48534850; // "PHSH"
    static constexpr u32 format_version = 2; // 2 added the compute fields; version 1 bundles still load

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
        if (!ok) return std::nullopt;
        return b;
    }
};

} // namespace ph
