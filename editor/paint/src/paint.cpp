#include "bowl/paint/paint.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>

namespace bowl::paint {

namespace {

constexpr std::array<std::array<i32, 4>, 4> bayer = {{{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}}};

u8 to8(f32 v) { return static_cast<u8>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); }
f32 lum(Pixel p) { return 0.299f * red(p) + 0.587f * green(p) + 0.114f * blue(p); }

// Each channel within `tol` of the other's.
bool close(Pixel a, Pixel b, i32 tol) {
    for (int s = 0; s < 32; s += 8)
        if (std::abs(static_cast<i32>((a >> s) & 255) - static_cast<i32>((b >> s) & 255)) > tol) return false;
    return true;
}

bool in_area(const Mask& m, i32 x, i32 y) { return !m.any() || m.test(x, y); }

Pixel mix(Pixel a, Pixel b, f32 t) {
    auto ch = [&](int s) {
        f32 va = static_cast<f32>((a >> s) & 255), vb = static_cast<f32>((b >> s) & 255);
        return static_cast<Pixel>(std::lround(va + (vb - va) * t)) << s;
    };
    return ch(0) | ch(8) | ch(16) | ch(24);
}

// Moves hue `h` toward `target` by up to `step` degrees, the short way round.
f32 lean(f32 h, f32 target, f32 step) {
    f32 d = std::fmod(target - h + 540.0f, 360.0f) - 180.0f;
    return std::fmod(h + std::clamp(d, -step, step) + 360.0f, 360.0f);
}

} // namespace

Pixel blend(Pixel under, Pixel over, f32 opacity) {
    f32 oa = alpha(over) / 255.0f * opacity, ua = alpha(under) / 255.0f;
    f32 out_a = oa + ua * (1 - oa);
    if (out_a <= 0) return clear;
    auto ch = [&](u8 o, u8 u) { return to8((o / 255.0f * oa + u / 255.0f * ua * (1 - oa)) / out_a); };
    return rgba(ch(red(over), red(under)), ch(green(over), green(under)), ch(blue(over), blue(under)), to8(out_a));
}

Hsv to_hsv(Pixel p) {
    f32 r = red(p) / 255.0f, g = green(p) / 255.0f, b = blue(p) / 255.0f;
    f32 mx = std::max({r, g, b}), mn = std::min({r, g, b}), d = mx - mn;
    Hsv out{0, mx > 0 ? d / mx : 0, mx};
    if (d > 0) {
        if (mx == r) out.h = 60 * std::fmod((g - b) / d + 6, 6.0f);
        else if (mx == g) out.h = 60 * ((b - r) / d + 2);
        else out.h = 60 * ((r - g) / d + 4);
    }
    return out;
}

Pixel from_hsv(Hsv c, u8 a) {
    f32 h = std::fmod(std::fmod(c.h, 360.0f) + 360.0f, 360.0f) / 60.0f, s = std::clamp(c.s, 0.0f, 1.0f), v = std::clamp(c.v, 0.0f, 1.0f);
    f32 k = v * s, x = k * (1 - std::abs(std::fmod(h, 2.0f) - 1)), m = v - k;
    f32 r = 0, g = 0, b = 0;
    switch (static_cast<int>(h)) {
    case 0: r = k, g = x; break;
    case 1: r = x, g = k; break;
    case 2: g = k, b = x; break;
    case 3: g = x, b = k; break;
    case 4: r = x, b = k; break;
    default: r = k, b = x; break;
    }
    return rgba(to8(r + m), to8(g + m), to8(b + m), a);
}

// --- Image and Mask ------------------------------------------------------------------------------------------------

Image Image::cut(Box b) const {
    Image out(std::max(0, b.w), std::max(0, b.h));
    for (i32 y = 0; y < b.h; ++y)
        for (i32 x = 0; x < b.w; ++x) out.set(x, y, get(b.x + x, b.y + y));
    return out;
}

void Image::paste(const Image& src, i32 x, i32 y, bool over) {
    for (i32 sy = 0; sy < src.height(); ++sy)
        for (i32 sx = 0; sx < src.width(); ++sx) {
            i32 dx = x + sx, dy = y + sy;
            if (!inside(dx, dy)) continue;
            Pixel p = src.get(sx, sy);
            set(dx, dy, over ? blend(get(dx, dy), p) : p);
        }
}

bool Mask::any() const { return std::any_of(on_.begin(), on_.end(), [](u8 v) { return v != 0; }); }

usize Mask::count() const { return static_cast<usize>(std::count_if(on_.begin(), on_.end(), [](u8 v) { return v != 0; })); }

Box Mask::bounds() const {
    i32 x0 = w_, y0 = h_, x1 = -1, y1 = -1;
    for (i32 y = 0; y < h_; ++y)
        for (i32 x = 0; x < w_; ++x)
            if (on_[index(x, y)]) {
                x0 = std::min(x0, x), y0 = std::min(y0, y), x1 = std::max(x1, x), y1 = std::max(y1, y);
            }
    return x1 < 0 ? Box{} : Box{x0, y0, x1 - x0 + 1, y1 - y0 + 1};
}

void Mask::invert() {
    for (u8& v : on_) v = v ? 0 : 1;
}

void Mask::combine(const Mask& m, Mode mode) {
    if (m.w_ != w_ || m.h_ != h_) {
        if (mode == Mode::Replace) *this = m;
        return;
    }
    for (usize i = 0; i < on_.size(); ++i) {
        if (mode == Mode::Replace) on_[i] = m.on_[i];
        else if (mode == Mode::Add) on_[i] = on_[i] || m.on_[i];
        else on_[i] = on_[i] && !m.on_[i];
    }
}

// --- Shapes --------------------------------------------------------------------------------------------------------

void line(Point a, Point b, const Put& put) {
    // The isometric 2:1 slope as even stairs, two pixels a step (Bresenham would start with a single one).
    i32 adx = std::abs(b.x - a.x), ady = std::abs(b.y - a.y), sx0 = b.x < a.x ? -1 : 1, sy0 = b.y < a.y ? -1 : 1;
    if (adx && ady && (adx == 2 * ady || ady == 2 * adx)) {
        bool across = adx > ady;
        for (i32 i = 0; i <= std::max(adx, ady); ++i)
            put(a.x + sx0 * (across ? i : i / 2), a.y + sy0 * (across ? i / 2 : i));
        return;
    }
    i32 dx = std::abs(b.x - a.x), dy = -std::abs(b.y - a.y), sx = a.x < b.x ? 1 : -1, sy = a.y < b.y ? 1 : -1, err = dx + dy;
    for (;;) {
        put(a.x, a.y);
        if (a == b) break;
        i32 e2 = 2 * err;
        if (e2 >= dy) err += dy, a.x += sx;
        if (e2 <= dx) err += dx, a.y += sy;
    }
}

Point snap_line(Point a, Point b) {
    i32 dx = b.x - a.x, dy = b.y - a.y;
    if (!dx && !dy) return b;
    // Directions: flat, upright, 45 degrees, and the 2:1 isometric slope (both ways round).
    static constexpr Point dirs[] = {{1, 0}, {0, 1}, {1, 1}, {2, 1}, {1, 2}};
    f32 len = std::sqrt(static_cast<f32>(dx * dx + dy * dy));
    Point best = b;
    f32 best_cos = -2;
    for (Point d : dirs)
        for (i32 sx : {-1, 1})
            for (i32 sy : {-1, 1}) {
                Point v{d.x * sx, d.y * sy};
                f32 vl = std::sqrt(static_cast<f32>(v.x * v.x + v.y * v.y));
                f32 c = (static_cast<f32>(dx * v.x + dy * v.y)) / (len * vl);
                if (c <= best_cos) continue;
                best_cos = c;
                i32 k = static_cast<i32>(std::lround(len * c / vl)); // whole steps of the direction
                best = {a.x + v.x * k, a.y + v.y * k};
            }
    return best;
}

void box(Point a, Point b, bool filled, bool diamond, const Put& put) {
    i32 x0 = std::min(a.x, b.x), x1 = std::max(a.x, b.x), y0 = std::min(a.y, b.y), y1 = std::max(a.y, b.y);
    if (!diamond) {
        for (i32 y = y0; y <= y1; ++y)
            for (i32 x = x0; x <= x1; ++x)
                if (filled || x == x0 || x == x1 || y == y0 || y == y1) put(x, y);
        return;
    }
    // The diamond whose corners are the middles of the box's sides, its edges drawn as stairs. Filled: every row
    // between its edges.
    i32 mx = (x0 + x1) / 2, my = (y0 + y1) / 2;
    std::map<i32, std::pair<i32, i32>> rows;
    auto edge = [&](i32 x, i32 y) {
        auto [it, fresh] = rows.try_emplace(y, x, x);
        if (!fresh) it->second = {std::min(it->second.first, x), std::max(it->second.second, x)};
        if (!filled) put(x, y);
    };
    line({mx, y0}, {x1, my}, edge);
    line({x1, my}, {mx, y1}, edge);
    line({mx, y1}, {x0, my}, edge);
    line({x0, my}, {mx, y0}, edge);
    if (filled)
        for (const auto& [y, span] : rows)
            for (i32 x = span.first; x <= span.second; ++x) put(x, y);
}

void ellipse(Point a, Point b, bool filled, const Put& put) {
    // Alois Zingl's ellipse in a box: exact for even and odd sizes.
    i64 x0 = std::min(a.x, b.x), x1 = std::max(a.x, b.x), y0 = std::min(a.y, b.y), y1 = std::max(a.y, b.y);
    i64 aa = x1 - x0, bb = y1 - y0, b1 = bb & 1;
    f64 dx = 4 * (1.0 - static_cast<f64>(aa)) * static_cast<f64>(bb * bb), dy = 4 * static_cast<f64>((b1 + 1) * aa * aa);
    f64 err = dx + dy + static_cast<f64>(b1 * aa * aa), e2;
    y0 += (bb + 1) / 2;
    y1 = y0 - b1;
    i64 a8 = 8 * aa * aa, b8 = 8 * bb * bb;
    std::map<i64, std::pair<i64, i64>> rows;
    auto plot = [&](i64 x, i64 y) {
        auto [it, fresh] = rows.try_emplace(y, x, x);
        if (!fresh) it->second = {std::min(it->second.first, x), std::max(it->second.second, x)};
        if (!filled) put(static_cast<i32>(x), static_cast<i32>(y));
    };
    do {
        plot(x1, y0), plot(x0, y0), plot(x0, y1), plot(x1, y1);
        e2 = 2 * err;
        if (e2 <= dy) {
            ++y0, --y1;
            err += dy += static_cast<f64>(a8);
        }
        if (e2 >= dx || 2 * err > dy) {
            ++x0, --x1;
            err += dx += static_cast<f64>(b8);
        }
    } while (x0 <= x1);
    while (y0 - y1 <= bb) { // flat ellipses: finish the tips
        plot(x0 - 1, y0), plot(x1 + 1, y0++);
        plot(x0 - 1, y1), plot(x1 + 1, y1--);
    }
    if (filled)
        for (const auto& [y, span] : rows)
            for (i64 x = span.first; x <= span.second; ++x) put(static_cast<i32>(x), static_cast<i32>(y));
}

std::vector<Point> pixel_perfect(const std::vector<Point>& points) {
    std::vector<Point> out;
    for (Point c : points) {
        if (!out.empty() && out.back() == c) continue;
        if (out.size() >= 2) {
            Point a = out[out.size() - 2], b = out.back();
            bool l_shape = (a.x == b.x || a.y == b.y) && (c.x == b.x || c.y == b.y) && a.x != c.x && a.y != c.y;
            if (l_shape) out.pop_back();
        }
        out.push_back(c);
    }
    return out;
}

bool dithered(i32 x, i32 y, i32 density) { return bayer[static_cast<usize>(y & 3)][static_cast<usize>(x & 3)] < density; }

void dab(const Brush& b, i32 x, i32 y, i32 w, i32 h, const Put& put) {
    i32 n = std::clamp(b.size, 1, 64), start = -(n - 1) / 2;
    f32 c = static_cast<f32>(start) + static_cast<f32>(n - 1) * 0.5f, r = static_cast<f32>(n) * 0.5f;
    f32 limit = r * r - r * 0.5f + 0.25f; // round brushes: 3 is a plus, 5 a circle
    auto paint = [&](i32 px, i32 py) {
        if (!dithered(px, py, b.density)) return;
        put(px, py);
        if (b.mirror_x) put(w - 1 - px, py);
        if (b.mirror_y) put(px, h - 1 - py);
        if (b.mirror_x && b.mirror_y) put(w - 1 - px, h - 1 - py);
    };
    for (i32 oy = start; oy < start + n; ++oy)
        for (i32 ox = start; ox < start + n; ++ox) {
            f32 ddx = static_cast<f32>(ox) - c, ddy = static_cast<f32>(oy) - c;
            if (b.square || n <= 2 || ddx * ddx + ddy * ddy <= limit) paint(x + ox, y + oy);
        }
}

Mask region(const Image& img, i32 x, i32 y, i32 tolerance, bool contiguous) {
    Mask m(img.width(), img.height());
    if (!img.inside(x, y)) return m;
    Pixel from = img.get(x, y);
    if (!contiguous) {
        for (i32 py = 0; py < img.height(); ++py)
            for (i32 px = 0; px < img.width(); ++px)
                if (close(img.get(px, py), from, tolerance)) m.set(px, py, true);
        return m;
    }
    std::vector<Point> todo{{x, y}};
    while (!todo.empty()) {
        Point p = todo.back();
        todo.pop_back();
        if (!img.inside(p.x, p.y) || m.test(p.x, p.y) || !close(img.get(p.x, p.y), from, tolerance)) continue;
        m.set(p.x, p.y, true);
        todo.insert(todo.end(), {{p.x + 1, p.y}, {p.x - 1, p.y}, {p.x, p.y + 1}, {p.x, p.y - 1}});
    }
    return m;
}

// --- Changing pixels -----------------------------------------------------------------------------------------------

void gradient(Image& img, const Mask& area, Point from, Point to, Pixel a, Pixel b, bool dither) {
    f32 vx = static_cast<f32>(to.x - from.x), vy = static_cast<f32>(to.y - from.y), len2 = vx * vx + vy * vy;
    for (i32 y = 0; y < img.height(); ++y)
        for (i32 x = 0; x < img.width(); ++x) {
            if (!in_area(area, x, y)) continue;
            f32 t = len2 > 0 ? std::clamp(((static_cast<f32>(x - from.x)) * vx + static_cast<f32>(y - from.y) * vy) / len2, 0.0f, 1.0f) : 0;
            if (dither) img.set(x, y, bayer[static_cast<usize>(y & 3)][static_cast<usize>(x & 3)] < std::lround(t * 16) ? b : a);
            else img.set(x, y, mix(a, b, t));
        }
}

Pixel shade(Pixel p, i32 direction, const std::vector<Pixel>& ramp) {
    if (alpha(p) == 0) return p;
    // Along the ramp, darkest to lightest, when the pixel is one of its colors.
    std::vector<Pixel> sorted = ramp;
    std::sort(sorted.begin(), sorted.end(), [](Pixel a, Pixel b) { return lum(a) < lum(b); });
    for (usize i = 0; i < sorted.size(); ++i)
        if ((sorted[i] & 0xffffff) == (p & 0xffffff)) {
            i64 j = std::clamp(static_cast<i64>(i) + direction, i64{0}, static_cast<i64>(sorted.size()) - 1);
            return (sorted[static_cast<usize>(j)] & 0xffffff) | (p & 0xff000000);
        }
    // Otherwise: light leans warm, shadow leans violet.
    Hsv c = to_hsv(p);
    if (direction > 0) {
        c.v = std::min(1.0f, c.v + 0.12f);
        c.s = std::max(0.0f, c.s - 0.04f);
        if (c.s > 0.05f) c.h = lean(c.h, 50, 8);
    } else {
        c.v = std::max(0.0f, c.v - 0.12f);
        c.s = std::min(1.0f, c.s + 0.05f);
        if (c.s > 0.05f) c.h = lean(c.h, 265, 8);
    }
    return from_hsv(c, alpha(p));
}

void adjust(Image& img, const Mask& area, f32 hue, f32 saturation, f32 brightness) {
    for (i32 y = 0; y < img.height(); ++y)
        for (i32 x = 0; x < img.width(); ++x) {
            Pixel p = img.get(x, y);
            if (!alpha(p) || !in_area(area, x, y)) continue;
            Hsv c = to_hsv(p);
            c.h += hue;
            c.s = std::clamp(c.s + saturation, 0.0f, 1.0f);
            c.v = std::clamp(c.v + brightness, 0.0f, 1.0f);
            img.set(x, y, from_hsv(c, alpha(p)));
        }
}

void replace(Image& img, const Mask& area, Pixel from, Pixel to, i32 tolerance) {
    for (i32 y = 0; y < img.height(); ++y)
        for (i32 x = 0; x < img.width(); ++x)
            if (in_area(area, x, y) && close(img.get(x, y), from, tolerance)) img.set(x, y, to);
}

void outline(Image& img, const Mask& area, Pixel color, bool corners) {
    Image before = img;
    for (i32 y = 0; y < img.height(); ++y)
        for (i32 x = 0; x < img.width(); ++x) {
            if (alpha(before.get(x, y)) || !in_area(area, x, y)) continue;
            bool next = false;
            for (i32 dy = -1; dy <= 1 && !next; ++dy)
                for (i32 dx = -1; dx <= 1 && !next; ++dx)
                    if ((dx || dy) && (corners || !dx || !dy) && alpha(before.get(x + dx, y + dy)) && before.get(x + dx, y + dy) != color)
                        next = true;
            if (next) img.set(x, y, color);
        }
}

void opaque(Image& img, const Mask& area) {
    for (i32 y = 0; y < img.height(); ++y)
        for (i32 x = 0; x < img.width(); ++x) {
            Pixel p = img.get(x, y);
            if (alpha(p) && in_area(area, x, y)) img.set(x, y, p | 0xff000000);
        }
}

Image flipped(const Image& img, bool across) {
    Image out(img.width(), img.height());
    for (i32 y = 0; y < img.height(); ++y)
        for (i32 x = 0; x < img.width(); ++x)
            out.set(across ? img.width() - 1 - x : x, across ? y : img.height() - 1 - y, img.get(x, y));
    return out;
}

Image turned(const Image& img, bool clockwise) {
    Image out(img.height(), img.width());
    for (i32 y = 0; y < img.height(); ++y)
        for (i32 x = 0; x < img.width(); ++x)
            if (clockwise) out.set(img.height() - 1 - y, x, img.get(x, y));
            else out.set(y, img.width() - 1 - x, img.get(x, y));
    return out;
}

Box painted_bounds(const Image& img) {
    i32 x0 = img.width(), y0 = img.height(), x1 = -1, y1 = -1;
    for (i32 y = 0; y < img.height(); ++y)
        for (i32 x = 0; x < img.width(); ++x)
            if (alpha(img.get(x, y))) x0 = std::min(x0, x), y0 = std::min(y0, y), x1 = std::max(x1, x), y1 = std::max(y1, y);
    return x1 < 0 ? Box{} : Box{x0, y0, x1 - x0 + 1, y1 - y0 + 1};
}

std::vector<Pixel> colors_of(const Image& img, usize most) {
    std::map<Pixel, usize> counts;
    for (Pixel p : img.pixels())
        if (alpha(p)) ++counts[p];
    std::vector<std::pair<Pixel, usize>> list(counts.begin(), counts.end());
    std::stable_sort(list.begin(), list.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    std::vector<Pixel> out;
    for (usize i = 0; i < list.size() && i < most; ++i) out.push_back(list[i].first);
    return out;
}

// --- Document ------------------------------------------------------------------------------------------------------

Document::Document(Image picture) : w_(picture.width()), h_(picture.height()) {
    layers.push_back({"Layer 1", std::move(picture)});
    selection = Mask(w_, h_);
    history_ = {state("Opened")};
}

Document::Document(i32 w, i32 h) : Document(Image(w, h)) { history_[0].label = "New picture"; }

Image Document::composite() const {
    Image out(w_, h_);
    for (usize li = 0; li < layers.size(); ++li) {
        const Layer& l = layers[li];
        if (!l.visible) continue;
        Image src = l.image;
        if (li == active && floating) src.paste(floating->image, floating->x, floating->y, true);
        for (usize i = 0; i < out.pixels().size(); ++i) out.pixels()[i] = blend(out.pixels()[i], src.pixels()[i], l.opacity);
    }
    return out;
}

bool Document::editable(i32 x, i32 y) const {
    if (layers.empty() || layer().locked || x < 0 || y < 0 || x >= w_ || y >= h_) return false;
    return !selection_any_ || selection.test(x, y);
}

Document::State Document::state(std::string label) const { return {std::move(label), w_, h_, layers, active, selection}; }

void Document::restore(const State& s) {
    w_ = s.w, h_ = s.h;
    layers = s.layers;
    active = std::min(s.active, layers.empty() ? 0 : layers.size() - 1);
    selection = s.selection;
    floating.reset();
    selection_changed();
}

void Document::selection_changed() { selection_any_ = selection.any(); }

void Document::commit(std::string label) {
    selection_changed();
    history_.resize(cursor_ + 1);
    history_.push_back(state(std::move(label)));
    if (saved_ > cursor_) saved_ = static_cast<usize>(-1); // the saved state can't come back
    if (history_.size() > 200) { // pictures are small, but not that small
        history_.erase(history_.begin());
        if (saved_ != static_cast<usize>(-1)) saved_ = saved_ > 0 ? saved_ - 1 : static_cast<usize>(-1);
    }
    cursor_ = history_.size() - 1;
}

bool Document::undo() {
    if (floating) {
        put_down();
    }
    if (!can_undo()) return false;
    restore(history_[--cursor_]);
    return true;
}

bool Document::redo() {
    if (!can_redo()) return false;
    restore(history_[++cursor_]);
    return true;
}

std::vector<std::string> Document::history() const {
    std::vector<std::string> out;
    for (const State& s : history_) out.push_back(s.label);
    return out;
}

void Document::go_to(usize step) {
    if (step >= history_.size() || step == cursor_) return;
    floating.reset();
    cursor_ = step;
    restore(history_[cursor_]);
}

void Document::add_layer(std::string name) {
    layers.insert(layers.begin() + static_cast<std::ptrdiff_t>(layers.empty() ? 0 : active + 1), Layer{std::move(name), Image(w_, h_)});
    active = layers.size() == 1 ? 0 : active + 1;
    commit("New layer");
}

void Document::duplicate_layer() {
    Layer copy = layer();
    copy.name += " copy";
    layers.insert(layers.begin() + static_cast<std::ptrdiff_t>(active + 1), std::move(copy));
    ++active;
    commit("Duplicate layer");
}

void Document::remove_layer() {
    if (layers.size() <= 1) return;
    layers.erase(layers.begin() + static_cast<std::ptrdiff_t>(active));
    if (active > 0) --active;
    commit("Delete layer");
}

void Document::move_layer(i32 by) {
    i64 to = static_cast<i64>(active) + by;
    if (to < 0 || to >= static_cast<i64>(layers.size())) return;
    std::swap(layers[active], layers[static_cast<usize>(to)]);
    active = static_cast<usize>(to);
    commit(by > 0 ? "Move layer up" : "Move layer down");
}

void Document::merge_down() {
    if (active == 0) return;
    Layer& below = layers[active - 1];
    const Layer& top = layers[active];
    for (usize i = 0; i < below.image.pixels().size(); ++i)
        below.image.pixels()[i] = blend(below.image.pixels()[i], top.visible ? top.image.pixels()[i] : clear, top.opacity);
    layers.erase(layers.begin() + static_cast<std::ptrdiff_t>(active));
    --active;
    commit("Merge down");
}

void Document::flatten() {
    put_down();
    Image all = composite();
    layers = {Layer{"Layer 1", std::move(all)}};
    active = 0;
    commit("Flatten");
}

void Document::resize(i32 w, i32 h, f32 anchor_x, f32 anchor_y) {
    if (w <= 0 || h <= 0) return;
    put_down();
    i32 dx = static_cast<i32>(std::lround(static_cast<f32>(w - w_) * anchor_x));
    i32 dy = static_cast<i32>(std::lround(static_cast<f32>(h - h_) * anchor_y));
    for (Layer& l : layers) {
        Image n(w, h);
        n.paste(l.image, dx, dy);
        l.image = std::move(n);
    }
    w_ = w, h_ = h;
    selection = Mask(w, h);
    commit("Canvas size");
}

void Document::crop(Box b) {
    if (b.empty()) return;
    put_down();
    for (Layer& l : layers) l.image = l.image.cut(b);
    w_ = b.w, h_ = b.h;
    selection = Mask(w_, h_);
    commit("Crop");
}

Box Document::trim() {
    Box b = painted_bounds(composite());
    if (b.empty() || (b.w == w_ && b.h == h_)) return b;
    crop(b);
    history_[cursor_].label = "Trim";
    return b;
}

void Document::flip(bool across) {
    put_down();
    for (Layer& l : layers) l.image = flipped(l.image, across);
    commit(across ? "Flip across" : "Flip upside down");
}

void Document::select_all() {
    selection = Mask(w_, h_);
    selection.invert();
    commit("Select all");
}

void Document::select_none() {
    if (!selection.any()) return;
    selection = Mask(w_, h_);
    commit("Select none");
}

void Document::select_box(Box b, Mask::Mode mode) {
    Mask m(w_, h_);
    for (i32 y = b.y; y < b.y + b.h; ++y)
        for (i32 x = b.x; x < b.x + b.w; ++x) m.set(x, y, true);
    select_mask(m, mode);
}

void Document::select_mask(const Mask& m, Mask::Mode mode) {
    put_down();
    selection.combine(m, mode);
    commit("Select");
}

void Document::invert_selection() {
    selection.invert();
    commit("Invert selection");
}

Image Document::copy(Point* where) const {
    Box b = selection_any_ ? selection.bounds() : Box{0, 0, w_, h_};
    Image out = layer().image.cut(b);
    if (selection_any_)
        for (i32 y = 0; y < b.h; ++y)
            for (i32 x = 0; x < b.w; ++x)
                if (!selection.test(b.x + x, b.y + y)) out.set(x, y, clear);
    if (where) *where = {b.x, b.y};
    return out;
}

void Document::clear_selection_pixels() {
    if (layer().locked) return;
    for (i32 y = 0; y < h_; ++y)
        for (i32 x = 0; x < w_; ++x)
            if (!selection_any_ || selection.test(x, y)) layer().image.set(x, y, clear);
    commit("Delete");
}

void Document::lift() {
    if (floating || layer().locked) return;
    Point at;
    Image img = copy(&at);
    for (i32 y = 0; y < img.height(); ++y)
        for (i32 x = 0; x < img.width(); ++x)
            if (!selection_any_ || selection.test(at.x + x, at.y + y)) layer().image.set(at.x + x, at.y + y, clear);
    floating = Floating{std::move(img), at.x, at.y};
    floating_label_ = "Move";
}

void Document::put_down() {
    if (!floating) return;
    layer().image.paste(floating->image, floating->x, floating->y, true);
    // The selection follows the pixels.
    Mask m(w_, h_);
    for (i32 y = 0; y < floating->image.height(); ++y)
        for (i32 x = 0; x < floating->image.width(); ++x)
            if (alpha(floating->image.get(x, y))) m.set(floating->x + x, floating->y + y, true);
    floating.reset();
    selection = selection_any_ ? m : Mask(w_, h_);
    commit(floating_label_);
}

void Document::paste(const Image& img, Point at) {
    put_down();
    floating = Floating{img, at.x, at.y};
    floating_label_ = "Paste";
    selection_any_ = true; // what's pasted stays selected when it's put down
}

// --- Saving layers -------------------------------------------------------------------------------------------------

std::string base64(const std::vector<u8>& bytes) {
    static constexpr char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    for (usize i = 0; i < bytes.size(); i += 3) {
        u32 n = static_cast<u32>(bytes[i]) << 16 | (i + 1 < bytes.size() ? bytes[i + 1] << 8 : 0) | (i + 2 < bytes.size() ? bytes[i + 2] : 0);
        out += table[(n >> 18) & 63];
        out += table[(n >> 12) & 63];
        out += i + 1 < bytes.size() ? table[(n >> 6) & 63] : '=';
        out += i + 2 < bytes.size() ? table[n & 63] : '=';
    }
    return out;
}

std::vector<u8> unbase64(std::string_view text) {
    auto value = [](char c) -> i32 {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        return c == '+' ? 62 : c == '/' ? 63 : -1;
    };
    std::vector<u8> out;
    u32 n = 0;
    i32 bits = 0;
    for (char c : text) {
        i32 v = value(c);
        if (v < 0) continue;
        n = n << 6 | static_cast<u32>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<u8>(n >> bits));
        }
    }
    return out;
}

json::Value Document::to_json() const {
    json::Value doc;
    doc.set("width", w_);
    doc.set("height", h_);
    doc.set("active", static_cast<f64>(active));
    json::Value list;
    list.make_array();
    for (const Layer& l : layers) {
        std::vector<u8> bytes(l.image.pixels().size() * 4);
        std::memcpy(bytes.data(), l.image.pixels().data(), bytes.size());
        json::Value o;
        o.set("name", l.name);
        o.set("visible", l.visible);
        o.set("locked", l.locked);
        o.set("opacity", l.opacity);
        o.set("rgba", base64(bytes));
        list.push(std::move(o));
    }
    doc.set("layers", std::move(list));
    return doc;
}

std::optional<Document> Document::from_json(const json::Value& v) {
    i32 w = static_cast<i32>(v["width"].as_int()), h = static_cast<i32>(v["height"].as_int());
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192 || !v["layers"].is_array() || v["layers"].size() == 0) return std::nullopt;
    Document d(w, h);
    d.layers.clear();
    for (const json::Value& o : v["layers"].array()) {
        std::vector<u8> bytes = unbase64(o["rgba"].as_string());
        if (bytes.size() != static_cast<usize>(w) * static_cast<usize>(h) * 4) return std::nullopt;
        Layer l{std::string(o["name"].as_string("Layer")), Image(w, h), o["visible"].as_bool(true), o["locked"].as_bool(false),
                o["opacity"].as_float(1)};
        std::memcpy(l.image.pixels().data(), bytes.data(), bytes.size());
        d.layers.push_back(std::move(l));
    }
    d.active = std::min<usize>(static_cast<usize>(v["active"].as_int()), d.layers.size() - 1);
    d.history_ = {d.state("Opened")};
    d.cursor_ = d.saved_ = 0;
    return d;
}

} // namespace bowl::paint
