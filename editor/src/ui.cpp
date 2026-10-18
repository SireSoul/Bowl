#include "ui.hpp"

#include <cctype>
#include <cmath>
#include <string>

namespace bowl::editor::ui {

namespace {
ImVec4 v(ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); }
} // namespace

void apply_theme() {
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 0;
    s.ChildRounding = 6;
    s.FrameRounding = 5;
    s.PopupRounding = 6;
    s.GrabRounding = 4;
    s.TabRounding = 5;
    s.WindowBorderSize = 0;
    s.FrameBorderSize = 0;
    s.WindowPadding = {12, 10};
    s.FramePadding = {8, 5};
    s.ItemSpacing = {8, 7};
    s.IndentSpacing = 14;
    s.ScrollbarSize = 10;

    ImVec4* c = s.Colors;
    c[ImGuiCol_Text] = v(color::text);
    c[ImGuiCol_TextDisabled] = v(color::muted);
    c[ImGuiCol_WindowBg] = v(color::panel);
    c[ImGuiCol_ChildBg] = {0, 0, 0, 0};
    c[ImGuiCol_PopupBg] = v(IM_COL32(28, 32, 40, 252));
    c[ImGuiCol_Border] = v(color::line);
    c[ImGuiCol_FrameBg] = v(color::field);
    c[ImGuiCol_FrameBgHovered] = v(IM_COL32(26, 30, 38, 255));
    c[ImGuiCol_FrameBgActive] = v(IM_COL32(30, 34, 44, 255));
    c[ImGuiCol_Button] = v(color::button);
    c[ImGuiCol_ButtonHovered] = v(color::hover);
    c[ImGuiCol_ButtonActive] = v(color::on);
    c[ImGuiCol_Header] = v(color::button);
    c[ImGuiCol_HeaderHovered] = v(color::hover);
    c[ImGuiCol_HeaderActive] = v(color::on);
    c[ImGuiCol_CheckMark] = v(color::accent);
    c[ImGuiCol_SliderGrab] = v(color::accent);
    c[ImGuiCol_SliderGrabActive] = v(color::accent);
    c[ImGuiCol_Separator] = v(color::line);
    c[ImGuiCol_Tab] = v(color::button);
    c[ImGuiCol_TabHovered] = v(color::hover);
    c[ImGuiCol_TabSelected] = v(color::on);
    c[ImGuiCol_TitleBg] = v(color::bar);
    c[ImGuiCol_TitleBgActive] = v(color::bar);
    c[ImGuiCol_MenuBarBg] = v(color::bar);
    c[ImGuiCol_ScrollbarBg] = {0, 0, 0, 0};
    c[ImGuiCol_ScrollbarGrab] = v(color::button);
    c[ImGuiCol_TextSelectedBg] = v(IM_COL32(242, 196, 92, 90));
    c[ImGuiCol_DragDropTarget] = v(color::accent);
    c[ImGuiCol_NavCursor] = v(color::accent);
    c[ImGuiCol_ModalWindowDimBg] = {0, 0, 0, 0.55f};
}

void draw_icon(ImDrawList* l, Icon icon, ImVec2 c, float size, ImU32 col) {
    float h = size * 0.5f, t = std::max(1.5f, size * 0.09f);
    auto P = [&](float x, float y) { return ImVec2{c.x + x * h, c.y + y * h}; };
    auto line = [&](float x0, float y0, float x1, float y1) { l->AddLine(P(x0, y0), P(x1, y1), col, t); };
    auto box = [&](float x0, float y0, float x1, float y1) { l->AddRect(P(x0, y0), P(x1, y1), col, 2, 0, t); };
    switch (icon) {
    case Icon::Select: { // an arrow pointer
        ImVec2 pts[] = {P(-0.55f, -0.8f), P(0.6f, 0.15f), P(0.05f, 0.2f), P(0.35f, 0.8f), P(0.1f, 0.9f),
                        P(-0.18f, 0.3f), P(-0.55f, 0.6f)};
        l->AddConvexPolyFilled(pts, 3, col);
        l->AddPolyline(pts, 7, col, ImDrawFlags_Closed, t);
        break;
    }
    case Icon::Place: // a stamp
        box(-0.6f, 0.2f, 0.6f, 0.55f);
        line(-0.15f, 0.2f, -0.15f, -0.25f);
        line(0.15f, 0.2f, 0.15f, -0.25f);
        l->AddCircle(P(0, -0.5f), h * 0.3f, col, 12, t);
        line(-0.7f, 0.85f, 0.7f, 0.85f);
        break;
    case Icon::Pan: // a four-way arrow
        line(-0.8f, 0, 0.8f, 0);
        line(0, -0.8f, 0, 0.8f);
        for (float s : {-1.0f, 1.0f}) {
            line(0.8f * s, 0, 0.5f * s, -0.3f);
            line(0.8f * s, 0, 0.5f * s, 0.3f);
            line(0, 0.8f * s, -0.3f, 0.5f * s);
            line(0, 0.8f * s, 0.3f, 0.5f * s);
        }
        break;
    case Icon::Grid:
        box(-0.75f, -0.75f, 0.75f, 0.75f);
        line(-0.25f, -0.75f, -0.25f, 0.75f);
        line(0.25f, -0.75f, 0.25f, 0.75f);
        line(-0.75f, -0.25f, 0.75f, -0.25f);
        line(-0.75f, 0.25f, 0.75f, 0.25f);
        break;
    case Icon::Snap: // a magnet
        l->PathArcTo(P(0, 0), h * 0.55f, 3.14159f, 6.28318f, 12);
        l->PathStroke(col, 0, t * 1.6f);
        line(-0.55f, 0, -0.55f, 0.75f);
        line(0.55f, 0, 0.55f, 0.75f);
        break;
    case Icon::Undo:
    case Icon::Redo: {
        float s = icon == Icon::Undo ? 1.0f : -1.0f;
        l->PathArcTo(P(0.05f * s, 0.15f), h * 0.55f, s > 0 ? 3.6f : -0.46f, s > 0 ? 6.9f : 2.8f, 12);
        l->PathStroke(col, 0, t);
        line(-0.65f * s, -0.15f, -0.45f * s, -0.6f);
        line(-0.65f * s, -0.15f, -0.2f * s, -0.1f);
        break;
    }
    case Icon::Save:
        box(-0.7f, -0.7f, 0.7f, 0.7f);
        box(-0.35f, -0.7f, 0.35f, -0.2f);
        box(-0.4f, 0.15f, 0.4f, 0.7f);
        break;
    case Icon::Open:
    case Icon::Folder:
        l->AddRect(P(-0.75f, -0.4f), P(0.75f, 0.65f), col, 2, 0, t);
        line(-0.75f, -0.4f, -0.6f, -0.65f);
        line(-0.6f, -0.65f, -0.1f, -0.65f);
        line(-0.1f, -0.65f, 0.05f, -0.4f);
        break;
    case Icon::New:
        box(-0.55f, -0.75f, 0.55f, 0.75f);
        line(0, -0.25f, 0, 0.35f);
        line(-0.3f, 0.05f, 0.3f, 0.05f);
        break;
    case Icon::Help:
        l->AddCircle(c, h * 0.8f, col, 20, t);
        l->PathArcTo(P(0, -0.2f), h * 0.28f, 3.4f, 6.9f, 10);
        l->PathStroke(col, 0, t);
        line(0, 0.1f, 0, 0.25f);
        l->AddCircleFilled(P(0, 0.48f), t * 0.8f, col);
        break;
    case Icon::Plus:
        line(-0.65f, 0, 0.65f, 0);
        line(0, -0.65f, 0, 0.65f);
        break;
    case Icon::Cross:
        line(-0.5f, -0.5f, 0.5f, 0.5f);
        line(-0.5f, 0.5f, 0.5f, -0.5f);
        break;
    case Icon::Trash:
        line(-0.7f, -0.5f, 0.7f, -0.5f);
        line(-0.2f, -0.75f, 0.2f, -0.75f);
        l->AddRect(P(-0.5f, -0.5f), P(0.5f, 0.8f), col, 2, 0, t);
        line(-0.15f, -0.2f, -0.15f, 0.55f);
        line(0.15f, -0.2f, 0.15f, 0.55f);
        break;
    case Icon::Copy:
        box(-0.7f, -0.35f, 0.3f, 0.75f);
        line(-0.35f, -0.35f, -0.35f, -0.75f);
        line(-0.35f, -0.75f, 0.7f, -0.75f);
        line(0.7f, -0.75f, 0.7f, 0.35f);
        line(0.7f, 0.35f, 0.3f, 0.35f);
        break;
    case Icon::Group:
        l->AddRect(P(-0.8f, -0.8f), P(0.8f, 0.8f), col, 3, 0, t * 0.7f);
        box(-0.5f, -0.5f, 0.05f, 0.05f);
        box(-0.05f, -0.05f, 0.5f, 0.5f);
        break;
    case Icon::Reload:
        l->PathArcTo(c, h * 0.62f, 0.6f, 5.6f, 16);
        l->PathStroke(col, 0, t);
        line(0.62f, -0.55f, 0.62f, -0.05f);
        line(0.62f, -0.05f, 0.15f, -0.15f);
        break;
    case Icon::Eye:
        l->PathArcTo(P(0, 0.75f), h * 1.05f, 4.1f, 5.33f, 12);
        l->PathStroke(col, 0, t);
        l->PathArcTo(P(0, -0.75f), h * 1.05f, 0.96f, 2.18f, 12);
        l->PathStroke(col, 0, t);
        l->AddCircleFilled(c, h * 0.22f, col);
        break;
    case Icon::AlignLeft:
    case Icon::AlignCenterX:
    case Icon::AlignRight: {
        float x = icon == Icon::AlignLeft ? -0.75f : icon == Icon::AlignRight ? 0.75f : 0;
        line(x, -0.85f, x, 0.85f);
        float a = icon == Icon::AlignLeft ? -0.75f : icon == Icon::AlignRight ? -0.35f : -0.55f;
        l->AddRectFilled(P(a, -0.55f), P(a + 1.1f, -0.15f), col, 1);
        float b = icon == Icon::AlignLeft ? -0.75f : icon == Icon::AlignRight ? 0.05f : -0.35f;
        l->AddRectFilled(P(b, 0.15f), P(b + 0.7f, 0.55f), col, 1);
        break;
    }
    case Icon::AlignTop:
    case Icon::AlignCenterY:
    case Icon::AlignBottom: {
        float y = icon == Icon::AlignTop ? -0.75f : icon == Icon::AlignBottom ? 0.75f : 0;
        line(-0.85f, y, 0.85f, y);
        float a = icon == Icon::AlignTop ? -0.75f : icon == Icon::AlignBottom ? -0.35f : -0.55f;
        l->AddRectFilled(P(-0.55f, a), P(-0.15f, a + 1.1f), col, 1);
        float b = icon == Icon::AlignTop ? -0.75f : icon == Icon::AlignBottom ? 0.05f : -0.35f;
        l->AddRectFilled(P(0.15f, b), P(0.55f, b + 0.7f), col, 1);
        break;
    }
    case Icon::Entity: // a crosshair: something with a position
        l->AddCircle(c, h * 0.45f, col, 16, t);
        line(-0.85f, 0, -0.5f, 0);
        line(0.5f, 0, 0.85f, 0);
        line(0, -0.85f, 0, -0.5f);
        line(0, 0.5f, 0, 0.85f);
        break;
    case Icon::Image:
        box(-0.75f, -0.6f, 0.75f, 0.6f);
        l->AddTriangleFilled(P(-0.55f, 0.45f), P(-0.1f, -0.1f), P(0.3f, 0.45f), col);
        l->AddCircleFilled(P(0.4f, -0.2f), h * 0.14f, col);
        break;
    case Icon::Brush: // a brush: a handle and its tip
        line(0.75f, -0.75f, -0.1f, 0.1f);
        l->AddQuadFilled(P(-0.1f, 0.1f), P(0.1f, 0.3f), P(-0.4f, 0.8f), P(-0.6f, 0.6f), col);
        break;
    case Icon::Lock: // a padlock: the shackle over a body
        l->PathArcTo(P(0, -0.2f), h * 0.42f, 3.1416f, 6.2832f, 12);
        l->PathStroke(col, 0, t);
        line(-0.42f, -0.2f, -0.42f, 0.05f);
        line(0.42f, -0.2f, 0.42f, 0.05f);
        l->AddRectFilled(P(-0.62f, 0.05f), P(0.62f, 0.8f), col, 2);
        break;
    case Icon::Person: // a head and shoulders
        l->AddCircle(P(0, -0.4f), h * 0.32f, col, 16, t);
        l->PathArcTo(P(0, 0.75f), h * 0.65f, 3.1416f, 6.2832f, 16);
        l->PathStroke(col, 0, t);
        break;
    case Icon::Bag: // a satchel: a body, a flap and a strap
        l->AddRect(P(-0.75f, -0.3f), P(0.75f, 0.75f), col, h * 0.2f, 0, t);
        line(-0.75f, 0.05f, 0.75f, 0.05f);
        line(-0.15f, 0.05f, -0.15f, 0.3f);
        line(0.15f, 0.05f, 0.15f, 0.3f);
        l->PathArcTo(P(0, -0.3f), h * 0.4f, 3.1416f, 6.2832f, 10);
        l->PathStroke(col, 0, t);
        break;
    case Icon::Shop: // an awning over a counter
        line(-0.8f, -0.35f, 0.8f, -0.35f);
        line(-0.65f, -0.75f, 0.65f, -0.75f);
        line(-0.8f, -0.35f, -0.65f, -0.75f);
        line(0.8f, -0.35f, 0.65f, -0.75f);
        box(-0.65f, -0.35f, 0.65f, 0.75f);
        box(-0.2f, 0.2f, 0.2f, 0.75f);
        break;
    case Icon::Bolt: { // something happening
        ImVec2 pts[] = {P(0.2f, -0.85f), P(-0.45f, 0.1f), P(0.0f, 0.1f), P(-0.2f, 0.85f), P(0.45f, -0.15f), P(0.0f, -0.15f)};
        l->AddPolyline(pts, 6, col, ImDrawFlags_Closed, t);
        break;
    }
    case Icon::Map: // a folded map
        line(-0.75f, -0.55f, -0.25f, -0.75f);
        line(-0.25f, -0.75f, 0.25f, -0.55f);
        line(0.25f, -0.55f, 0.75f, -0.75f);
        line(-0.75f, 0.75f, -0.25f, 0.55f);
        line(-0.25f, 0.55f, 0.25f, 0.75f);
        line(0.25f, 0.75f, 0.75f, 0.55f);
        line(-0.75f, -0.55f, -0.75f, 0.75f);
        line(-0.25f, -0.75f, -0.25f, 0.55f);
        line(0.25f, -0.55f, 0.25f, 0.75f);
        line(0.75f, -0.75f, 0.75f, 0.55f);
        break;
    case Icon::Note: // a musical note
        l->AddCircleFilled(P(-0.3f, 0.5f), h * 0.25f, col);
        line(-0.07f, 0.5f, -0.07f, -0.75f);
        line(-0.07f, -0.75f, 0.55f, -0.45f);
        break;
    case Icon::Gear:
        l->AddCircle(c, h * 0.42f, col, 16, t);
        for (int k = 0; k < 8; ++k) {
            float a = k * 0.7854f, ca = std::cos(a), sa = std::sin(a);
            line(ca * 0.55f, sa * 0.55f, ca * 0.85f, sa * 0.85f);
        }
        break;
    case Icon::Search:
        l->AddCircle(P(-0.15f, -0.15f), h * 0.5f, col, 16, t);
        line(0.22f, 0.22f, 0.75f, 0.75f);
        break;
    case Icon::ArrowUp:
    case Icon::ArrowDown: {
        float s = icon == Icon::ArrowUp ? 1.0f : -1.0f;
        line(0, -0.7f * s, 0, 0.7f * s);
        line(0, -0.7f * s, -0.45f, -0.25f * s);
        line(0, -0.7f * s, 0.45f, -0.25f * s);
        break;
    }
    case Icon::Play: {
        ImVec2 pts[] = {P(-0.45f, -0.7f), P(0.7f, 0), P(-0.45f, 0.7f)};
        l->AddConvexPolyFilled(pts, 3, col);
        break;
    }
    case Icon::Link: // two links of a chain
        l->AddRect(P(-0.85f, -0.3f), P(0.15f, 0.3f), col, h * 0.3f, 0, t);
        l->AddRect(P(-0.15f, -0.3f), P(0.85f, 0.3f), col, h * 0.3f, 0, t);
        break;
    case Icon::Wand: // a stick with a sparkle
        line(-0.7f, 0.7f, 0.25f, -0.25f);
        line(0.5f, -0.85f, 0.5f, -0.35f);
        line(0.25f, -0.6f, 0.75f, -0.6f);
        line(0.75f, -0.1f, 0.75f, 0.15f);
        line(0.62f, 0.02f, 0.88f, 0.02f);
        break;
    case Icon::Bucket: { // a tipped pail with a drop
        ImVec2 pts[] = {P(-0.6f, -0.3f), P(0.2f, -0.75f), P(0.65f, 0.05f), P(-0.15f, 0.5f)};
        l->AddPolyline(pts, 4, col, ImDrawFlags_Closed, t);
        l->AddCircleFilled(P(0.6f, 0.65f), h * 0.18f, col);
        break;
    }
    case Icon::Pipette:
        line(-0.65f, 0.65f, 0.2f, -0.2f);
        line(-0.65f, 0.65f, -0.75f, 0.75f);
        l->AddCircleFilled(P(0.45f, -0.45f), h * 0.28f, col);
        line(0.0f, -0.4f, 0.4f, 0.0f);
        break;
    case Icon::Line:
        line(-0.7f, 0.7f, 0.7f, -0.7f);
        l->AddCircleFilled(P(-0.7f, 0.7f), t * 1.2f, col);
        l->AddCircleFilled(P(0.7f, -0.7f), t * 1.2f, col);
        break;
    case Icon::Rect: box(-0.7f, -0.55f, 0.7f, 0.55f); break;
    case Icon::Circle: l->AddCircle(c, h * 0.72f, col, 20, t); break;
    case Icon::Gradient:
        box(-0.7f, -0.7f, 0.7f, 0.7f);
        for (int k = 0; k < 4; ++k) l->AddRectFilled(P(-0.7f + k * 0.35f, -0.7f), P(-0.7f + (k + 1) * 0.35f, 0.7f), (col & 0x00ffffff) | (static_cast<ImU32>(40 + k * 60) << 24));
        break;
    case Icon::Shade: // half dark, half light
        l->AddCircle(c, h * 0.72f, col, 20, t);
        l->PathArcTo(c, h * 0.72f, 1.5708f, 4.7124f, 16);
        l->PathFillConvex(col);
        break;
    case Icon::Lasso: { // a loop with a tail
        l->AddEllipse(P(0.05f, -0.25f), ImVec2{h * 0.75f, h * 0.5f}, col, 0, 20, t);
        l->AddBezierCubic(P(-0.45f, 0.1f), P(-0.6f, 0.45f), P(-0.2f, 0.55f), P(-0.45f, 0.9f), col, t, 10);
        break;
    }
    case Icon::Recolor: { // a drop over two swatches, one taking the other's color
        box(-0.8f, 0.2f, -0.1f, 0.85f);
        l->AddRectFilled(P(0.1f, 0.2f), P(0.8f, 0.85f), col, 2);
        ImVec2 drop[] = {P(0, -0.9f), P(0.3f, -0.4f), P(0, -0.15f), P(-0.3f, -0.4f)};
        l->AddConvexPolyFilled(drop, 4, col);
        break;
    }
    case Icon::Dither: // a checkerboard
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 4; ++x)
                if ((x + y) % 2 == 0) l->AddRectFilled(P(-0.8f + x * 0.4f, -0.8f + y * 0.4f), P(-0.4f + x * 0.4f, -0.4f + y * 0.4f), col);
        break;
    case Icon::Spray: { // a can and its mist
        box(-0.75f, -0.15f, -0.15f, 0.85f);
        line(-0.55f, -0.15f, -0.55f, -0.4f);
        line(-0.55f, -0.4f, -0.3f, -0.4f);
        for (ImVec2 d : {ImVec2{0.2f, -0.55f}, ImVec2{0.5f, -0.75f}, ImVec2{0.55f, -0.35f}, ImVec2{0.8f, -0.55f}, ImVec2{0.35f, -0.1f},
                         ImVec2{0.75f, -0.15f}})
            l->AddCircleFilled(P(d.x, d.y), h * 0.09f, col);
        break;
    }
    case Icon::Blur: // a drop with soft rings
        l->AddCircleFilled(c, h * 0.3f, col);
        l->AddCircle(c, h * 0.55f, (col & 0x00ffffff) | 0xa0000000, 20, t);
        l->AddCircle(c, h * 0.82f, (col & 0x00ffffff) | 0x50000000, 24, t);
        break;
    case Icon::Polygon: { // a five-sided shape
        ImVec2 pts[] = {P(0, -0.85f), P(0.8f, -0.25f), P(0.5f, 0.75f), P(-0.5f, 0.75f), P(-0.8f, -0.25f)};
        l->AddPolyline(pts, 5, col, ImDrawFlags_Closed, t);
        break;
    }
    case Icon::Curve:
        l->AddBezierCubic(P(-0.8f, 0.7f), P(-0.6f, -0.9f), P(0.6f, 0.9f), P(0.8f, -0.7f), col, t, 16);
        l->AddCircleFilled(P(-0.8f, 0.7f), h * 0.14f, col);
        l->AddCircleFilled(P(0.8f, -0.7f), h * 0.14f, col);
        break;
    case Icon::Text: // a serif T
        line(-0.7f, -0.75f, 0.7f, -0.75f);
        line(0, -0.75f, 0, 0.8f);
        line(-0.3f, 0.8f, 0.3f, 0.8f);
        line(-0.7f, -0.75f, -0.7f, -0.5f);
        line(0.7f, -0.75f, 0.7f, -0.5f);
        break;
    case Icon::Onion: // frames before and after, faint, behind this one
        l->AddRect(P(-0.85f, -0.55f), P(0.15f, 0.45f), (col & 0x00ffffff) | 0x60000000, 2, 0, t);
        l->AddRect(P(-0.15f, -0.45f), P(0.85f, 0.55f), (col & 0x00ffffff) | 0x60000000, 2, 0, t);
        l->AddRectFilled(P(-0.5f, -0.5f), P(0.5f, 0.5f), col, 2);
        break;
    case Icon::Tiles: // the picture repeated
        box(-0.85f, -0.85f, -0.05f, -0.05f);
        box(0.05f, -0.85f, 0.85f, -0.05f);
        box(-0.85f, 0.05f, -0.05f, 0.85f);
        box(0.05f, 0.05f, 0.85f, 0.85f);
        break;
    case Icon::Frames: // a strip of film
        box(-0.85f, -0.55f, 0.85f, 0.55f);
        line(-0.28f, -0.55f, -0.28f, 0.55f);
        line(0.28f, -0.55f, 0.28f, 0.55f);
        break;
    case Icon::Eraser: {
        ImVec2 pts[] = {P(-0.75f, 0.25f), P(0.05f, -0.55f), P(0.65f, 0.05f), P(-0.15f, 0.85f)};
        l->AddPolyline(pts, 4, col, ImDrawFlags_Closed, t);
        line(-0.35f, -0.15f, 0.25f, 0.45f);
        break;
    }
    case Icon::Region: // a dashed area
        for (float s = -0.75f; s < 0.7f; s += 0.5f) {
            line(s, -0.75f, s + 0.25f, -0.75f);
            line(s, 0.75f, s + 0.25f, 0.75f);
            line(-0.75f, s, -0.75f, s + 0.25f);
            line(0.75f, s, 0.75f, s + 0.25f);
        }
        l->AddRectFilled(P(-0.35f, -0.35f), P(0.35f, 0.35f), (col & 0x00FFFFFF) | 0x50000000, 1);
        break;
    }
}

bool tool_button(const char* id, Icon icon, bool on, const char* tip_text, float size, bool enabled) {
    ImGui::PushID(id);
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::BeginDisabled(!enabled);
    bool pressed = ImGui::InvisibleButton("##b", {size, size});
    ImGui::EndDisabled();
    bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
    ImDrawList* l = ImGui::GetWindowDrawList();
    ImU32 bg = on ? color::on : hovered && enabled ? color::hover : 0;
    if (bg) l->AddRectFilled(pos, {pos.x + size, pos.y + size}, bg, 6);
    if (on) l->AddRect(pos, {pos.x + size, pos.y + size}, color::accent, 6, 0, 1.5f);
    ImU32 fg = !enabled ? IM_COL32(90, 96, 108, 255) : on ? color::accent : color::text;
    draw_icon(l, icon, {pos.x + size * 0.5f, pos.y + size * 0.5f}, size * 0.55f, fg);
    if (tip_text && hovered) {
        std::string_view all = tip_text;
        auto bar = all.find('|'); // "what it does|key"
        tip(all.substr(0, bar), bar == std::string_view::npos ? std::string_view{} : all.substr(bar + 1));
    }
    ImGui::PopID();
    return pressed && enabled;
}

void section(std::string_view title) {
    std::string upper(title);
    for (char& ch : upper) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    ImGui::PushStyleColor(ImGuiCol_Text, color::muted);
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 0.82f);
    ImGui::TextUnformatted(upper.c_str());
    ImGui::PopFont();
    ImGui::PopStyleColor();
}

void tip(std::string_view text, std::string_view keys) {
    if (!ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled)) return;
    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(320);
    ImGui::TextUnformatted(text.data(), text.data() + text.size());
    if (!keys.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, color::accent);
        ImGui::TextUnformatted(keys.data(), keys.data() + keys.size());
        ImGui::PopStyleColor();
    }
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}

bool begin_card(const char* title, const char* id, Icon button, bool* pressed, const char* button_tip) {
    ImGui::PushID(id);
    ImGui::PushStyleColor(ImGuiCol_Header, IM_COL32(34, 39, 49, 255));
    float right = ImGui::GetContentRegionAvail().x;
    bool open = ImGui::CollapsingHeader(title, ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap);
    ImGui::PopStyleColor();
    if (pressed) {
        ImGui::SameLine(right - 18);
        *pressed = tool_button("card-button", button, false, button_tip, 22);
    }
    if (!open) ImGui::PopID();
    else ImGui::Indent(4);
    return open;
}

void end_card() {
    ImGui::Unindent(4);
    ImGui::Dummy({0, 4});
    ImGui::PopID();
}

void hint(std::string_view text) {
    ImGui::PushStyleColor(ImGuiCol_Text, color::muted);
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted(text.data(), text.data() + text.size());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}

void rule() {
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    ImGui::GetWindowDrawList()->AddLine({p.x, p.y + 2}, {p.x + w, p.y + 2}, color::line);
    ImGui::Dummy({w, 5});
}

} // namespace bowl::editor::ui
