// Aseprite sprite sheets (JSON, array or hash layout), and animating with what was read.

#include "fuzz.hpp"

#include "bowl/gfx/sprite_sheet.hpp"

using namespace bowl;
using namespace bowl::gfx;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    auto doc = json::parse(std::string_view(reinterpret_cast<const char*>(data), size));
    if (!doc) return 0;
    auto slot = std::make_shared<AssetSlot<SpriteSheet>>();
    slot->value = std::make_unique<SpriteSheet>();
    SpriteSheet& sheet = *slot->value;
    std::string image, error;
    if (!SpriteSheet::parse_aseprite(doc.value, sheet, image, error)) return 0;

    for (const std::string& name : sheet.frame_names) sheet.frame_index(name);
    SpriteAnimator animator{.sheet = Asset<SpriteSheet>(slot)};
    for (const SpriteAnimation& animation : sheet.animations) {
        animator.play(animation.name, true);
        for (int i = 0; i < 16; ++i) {
            animator.update(0.07f);
            for (const FrameEvent& e : animation.events) animator.fired(e.name);
        }
    }
    SpriteBatch batch;
    batch.begin(Camera2D::screen({320, 180}));
    for (u32 frame = 0; frame < sheet.frames.size() + 1 && frame < 64; ++frame) {
        draw_frame(batch, sheet, frame, {10, 10});
        for (const SpriteSlice& slice : sheet.slices) {
            slice_rect(sheet, slice.name, frame, {10, 10}, {-2, 1});
            draw_nine_slice(batch, sheet, slice.name, frame, {0, 0, 100, 40});
        }
    }
    return 0;
}
