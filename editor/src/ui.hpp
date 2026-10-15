#pragma once

#include <imgui.h>

#include <string>
#include <string_view>

// The editor's look and its few custom controls, on top of Dear ImGui: a dark blue-gray theme with a warm accent,
// icons drawn as shapes (no icon font to ship), tool buttons, cards and tooltips that say what a control does and its
// key.
namespace ph::editor::ui {

namespace color {
inline constexpr ImU32 panel = IM_COL32(24, 27, 34, 245);
inline constexpr ImU32 bar = IM_COL32(18, 20, 26, 255);
inline constexpr ImU32 line = IM_COL32(70, 78, 92, 255);
inline constexpr ImU32 text = IM_COL32(222, 226, 232, 255);
inline constexpr ImU32 muted = IM_COL32(140, 148, 160, 255);
inline constexpr ImU32 accent = IM_COL32(242, 196, 92, 255);
inline constexpr ImU32 button = IM_COL32(44, 50, 62, 255);
inline constexpr ImU32 hover = IM_COL32(62, 70, 86, 255);
inline constexpr ImU32 on = IM_COL32(96, 82, 44, 255);
inline constexpr ImU32 field = IM_COL32(16, 18, 24, 255);
inline constexpr ImU32 danger = IM_COL32(224, 108, 100, 255);
} // namespace color

enum class Icon {
    Select, Place, Pan, Grid, Snap, Undo, Redo, Save, Open, New, Help, Plus, Trash, Copy, Group, Reload, Eye, Cross,
    AlignLeft, AlignCenterX, AlignRight, AlignTop, AlignCenterY, AlignBottom, Folder, Entity, Image, Brush, Region
};

void apply_theme();
void draw_icon(ImDrawList* list, Icon icon, ImVec2 center, float size, ImU32 color);

// A square button with an icon. `on` paints it as the active choice. The tooltip names it and its key.
bool tool_button(const char* id, Icon icon, bool on, const char* tip, float size = 30, bool enabled = true);
// Shows `text` (and a dimmer `keys` line) when the last item is hovered.
void tip(std::string_view text, std::string_view keys = {});
// A card: a titled block with an optional icon button on the right of its title (a remove "x", for example).
// Returns true while open; call end_card() after its contents if so. *pressed is set when the title button was hit.
bool begin_card(const char* title, const char* id, Icon button = Icon::Cross, bool* pressed = nullptr,
                const char* button_tip = nullptr);
void end_card();
// A dim one-line hint.
void hint(std::string_view text);
// A thin line across the current window.
void rule();

} // namespace ph::editor::ui
