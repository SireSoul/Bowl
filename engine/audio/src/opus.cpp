#include "decoders.hpp"

#include <algorithm>
#include <cstring>

#if PH_OPUS
#include <opusfile.h>
#endif

namespace ph::audio::detail {

bool looks_like_opus(const std::vector<u8>& data) {
    // The first Ogg page of an Opus file carries the "OpusHead" packet.
    if (data.size() < 64 || std::memcmp(data.data(), "OggS", 4) != 0) return false;
    for (usize i = 4; i + 8 <= std::min<usize>(data.size(), 128); ++i)
        if (std::memcmp(data.data() + i, "OpusHead", 8) == 0) return true;
    return false;
}

#if PH_OPUS

bool opus_built() { return true; }

namespace {

OggOpusFile* open(const std::vector<u8>& data) {
    int error = 0;
    return op_open_memory(data.data(), data.size(), &error);
}

class OpusDecoder final : public Decoder {
public:
    explicit OpusDecoder(OggOpusFile* file) : file_(file) {}
    ~OpusDecoder() override { op_free(file_); }

    u32 read(f32* out, u32 frames) override {
        u32 done = 0;
        while (done < frames) {
            int got = op_read_float_stereo(file_, out + done * 2, static_cast<int>((frames - done) * 2));
            if (got == OP_HOLE) continue; // a damaged page: skip it
            if (got <= 0) break;
            done += static_cast<u32>(got);
        }
        return done;
    }
    void rewind() override { op_pcm_seek(file_, 0); }

private:
    OggOpusFile* file_;
};

} // namespace

std::optional<StreamInfo> probe_opus(const std::vector<u8>& data) {
    OggOpusFile* file = open(data);
    if (!file) return std::nullopt;
    ogg_int64_t total = op_pcm_total(file, -1);
    op_free(file);
    return StreamInfo{48000, 2, total > 0 ? static_cast<f32>(total) / 48000.0f : 0.0f};
}

std::unique_ptr<Decoder> open_opus(const std::vector<u8>& data) {
    OggOpusFile* file = open(data);
    return file ? std::make_unique<OpusDecoder>(file) : nullptr;
}

#else

bool opus_built() { return false; }
std::optional<StreamInfo> probe_opus(const std::vector<u8>&) { return std::nullopt; }
std::unique_ptr<Decoder> open_opus(const std::vector<u8>&) { return nullptr; }

#endif

} // namespace ph::audio::detail
