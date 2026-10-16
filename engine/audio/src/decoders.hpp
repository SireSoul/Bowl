#pragma once

// Compressed audio decoders behind one interface, so music streams don't care what codec a track uses.

#include "ph/core/types.hpp"

#include <memory>
#include <optional>
#include <vector>

namespace ph::audio::detail {

// Decodes a file held in memory, a little at a time: interleaved float frames at the file's own rate.
class Decoder {
public:
    virtual ~Decoder() = default;
    // Up to `frames` frames into out; 0 at the end.
    virtual u32 read(f32* out, u32 frames) = 0;
    virtual void rewind() = 0;
};

struct StreamInfo {
    u32 sample_rate, channels;
    f32 seconds;
};

// Ogg Vorbis (stb_vorbis).
std::optional<StreamInfo> probe_vorbis(const std::vector<u8>& data);
std::unique_ptr<Decoder> open_vorbis(const std::vector<u8>& data);

// Ogg Opus (opusfile). Always 48 kHz stereo; mono and surround files are mixed to stereo. Without PH_OPUS these
// find nothing.
bool opus_built();
bool looks_like_opus(const std::vector<u8>& data);
std::optional<StreamInfo> probe_opus(const std::vector<u8>& data);
std::unique_ptr<Decoder> open_opus(const std::vector<u8>& data);

} // namespace ph::audio::detail
