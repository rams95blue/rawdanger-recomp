#include "quickmenu.hpp"

#include <algorithm>
#include <iterator>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace quickmenu {
namespace {

// The font: 5 x 8 pixels per letter, printable ASCII (32 to 126), drawn for this project (no font file, no licence to carry). One letter a
// line, its 8 rows from the top separated by spaces, '#' a lit pixel. Capitals and digits use rows 0 to 6; row 7 is for the tails of
// g, j, p, q, y and the comma.
constexpr const char *kFont[95] = {
    "..... ..... ..... ..... ..... ..... ..... .....",  // space
    "..#.. ..#.. ..#.. ..#.. ..#.. ..... ..#.. .....",  // !
    ".#.#. .#.#. ..... ..... ..... ..... ..... .....",  // "
    ".#.#. .#.#. ##### .#.#. ##### .#.#. .#.#. .....",  // #
    "..#.. .#### #.#.. .###. ..#.# ####. ..#.. .....",  // $
    "##... ##..# ...#. ..#.. .#... #..## ...## .....",  // %
    ".##.. #..#. #.#.. .#... #.#.# #..#. .##.# .....",  // &
    "..#.. ..#.. ..... ..... ..... ..... ..... .....",  // '
    "...#. ..#.. .#... .#... .#... ..#.. ...#. .....",  // (
    ".#... ..#.. ...#. ...#. ...#. ..#.. .#... .....",  // )
    "..... ..#.. #.#.# .###. #.#.# ..#.. ..... .....",  // *
    "..... ..#.. ..#.. ##### ..#.. ..#.. ..... .....",  // +
    "..... ..... ..... ..... ..... .##.. ..#.. .#...",  // ,
    "..... ..... ..... ##### ..... ..... ..... .....",  // -
    "..... ..... ..... ..... ..... .##.. .##.. .....",  // .
    "..... ....# ...#. ..#.. .#... #.... ..... .....",  // /
    ".###. #...# #..## #.#.# ##..# #...# .###. .....",  // 0
    "..#.. .##.. ..#.. ..#.. ..#.. ..#.. .###. .....",  // 1
    ".###. #...# ....# ...#. ..#.. .#... ##### .....",  // 2
    "##### ...#. ..#.. ...#. ....# #...# .###. .....",  // 3
    "...#. ..##. .#.#. #..#. ##### ...#. ...#. .....",  // 4
    "##### #.... ####. ....# ....# #...# .###. .....",  // 5
    "..##. .#... #.... ####. #...# #...# .###. .....",  // 6
    "##### ....# ...#. ..#.. .#... .#... .#... .....",  // 7
    ".###. #...# #...# .###. #...# #...# .###. .....",  // 8
    ".###. #...# #...# .#### ....# ...#. .##.. .....",  // 9
    "..... .##.. .##.. ..... .##.. .##.. ..... .....",  // :
    "..... .##.. .##.. ..... .##.. ..#.. .#... .....",  // ;
    "...#. ..#.. .#... #.... .#... ..#.. ...#. .....",  // <
    "..... ..... ##### ..... ##### ..... ..... .....",  // =
    ".#... ..#.. ...#. ....# ...#. ..#.. .#... .....",  // >
    ".###. #...# ....# ...#. ..#.. ..... ..#.. .....",  // ?
    ".###. #...# ....# .##.# #.#.# #.#.# .###. .....",  // @
    ".###. #...# #...# ##### #...# #...# #...# .....",  // A
    "####. #...# #...# ####. #...# #...# ####. .....",  // B
    ".###. #...# #.... #.... #.... #...# .###. .....",  // C
    "####. #...# #...# #...# #...# #...# ####. .....",  // D
    "##### #.... #.... ####. #.... #.... ##### .....",  // E
    "##### #.... #.... ####. #.... #.... #.... .....",  // F
    ".###. #...# #.... #.### #...# #...# .#### .....",  // G
    "#...# #...# #...# ##### #...# #...# #...# .....",  // H
    ".###. ..#.. ..#.. ..#.. ..#.. ..#.. .###. .....",  // I
    "..### ...#. ...#. ...#. ...#. #..#. .##.. .....",  // J
    "#...# #..#. #.#.. ##... #.#.. #..#. #...# .....",  // K
    "#.... #.... #.... #.... #.... #.... ##### .....",  // L
    "#...# ##.## #.#.# #.#.# #...# #...# #...# .....",  // M
    "#...# #...# ##..# #.#.# #..## #...# #...# .....",  // N
    ".###. #...# #...# #...# #...# #...# .###. .....",  // O
    "####. #...# #...# ####. #.... #.... #.... .....",  // P
    ".###. #...# #...# #...# #.#.# #..#. .##.# .....",  // Q
    "####. #...# #...# ####. #.#.. #..#. #...# .....",  // R
    ".#### #.... #.... .###. ....# ....# ####. .....",  // S
    "##### ..#.. ..#.. ..#.. ..#.. ..#.. ..#.. .....",  // T
    "#...# #...# #...# #...# #...# #...# .###. .....",  // U
    "#...# #...# #...# #...# #...# .#.#. ..#.. .....",  // V
    "#...# #...# #...# #.#.# #.#.# #.#.# .#.#. .....",  // W
    "#...# #...# .#.#. ..#.. .#.#. #...# #...# .....",  // X
    "#...# #...# .#.#. ..#.. ..#.. ..#.. ..#.. .....",  // Y
    "##### ....# ...#. ..#.. .#... #.... ##### .....",  // Z
    ".###. .#... .#... .#... .#... .#... .###. .....",  // [
    "..... #.... .#... ..#.. ...#. ....# ..... .....",  // backslash
    ".###. ...#. ...#. ...#. ...#. ...#. .###. .....",  // ]
    "..#.. .#.#. #...# ..... ..... ..... ..... .....",  // ^
    "..... ..... ..... ..... ..... ..... ##### .....",  // _
    ".#... ..#.. ..... ..... ..... ..... ..... .....",  // `
    "..... ..... .###. ....# .#### #...# .#### .....",  // a
    "#.... #.... #.##. ##..# #...# #...# ####. .....",  // b
    "..... ..... .###. #.... #.... #...# .###. .....",  // c
    "....# ....# .##.# #..## #...# #...# .#### .....",  // d
    "..... ..... .###. #...# ##### #.... .###. .....",  // e
    "..##. .#..# .#... ###.. .#... .#... .#... .....",  // f
    "..... ..... .#### #...# #...# .#### ....# .###.",  // g
    "#.... #.... #.##. ##..# #...# #...# #...# .....",  // h
    "..#.. ..... .##.. ..#.. ..#.. ..#.. .###. .....",  // i
    "...#. ..... ..##. ...#. ...#. ...#. #..#. .##..",  // j
    "#.... #.... #..#. #.#.. ##... #.#.. #..#. .....",  // k
    ".##.. ..#.. ..#.. ..#.. ..#.. ..#.. .###. .....",  // l
    "..... ..... ##.#. #.#.# #.#.# #.#.# #.#.# .....",  // m
    "..... ..... #.##. ##..# #...# #...# #...# .....",  // n
    "..... ..... .###. #...# #...# #...# .###. .....",  // o
    "..... ..... ####. #...# #...# ####. #.... #....",  // p
    "..... ..... .#### #...# #...# .#### ....# ....#",  // q
    "..... ..... #.##. ##..# #.... #.... #.... .....",  // r
    "..... ..... .###. #.... .###. ....# ####. .....",  // s
    ".#... .#... ###.. .#... .#... .#..# ..##. .....",  // t
    "..... ..... #...# #...# #...# #..## .##.# .....",  // u
    "..... ..... #...# #...# #...# .#.#. ..#.. .....",  // v
    "..... ..... #...# #...# #.#.# #.#.# .#.#. .....",  // w
    "..... ..... #...# .#.#. ..#.. .#.#. #...# .....",  // x
    "..... ..... #...# #...# #...# .#### ....# .###.",  // y
    "..... ..... ##### ...#. ..#.. .#... ##### .....",  // z
    "...#. ..#.. ..#.. .#... ..#.. ..#.. ...#. .....",  // {
    "..#.. ..#.. ..#.. ..#.. ..#.. ..#.. ..#.. .....",  // |
    ".#... ..#.. ..#.. ...#. ..#.. ..#.. .#... .....",  // }
    "..... ..... .#... #.#.# ...#. ..... ..... .....",  // ~
};

// rawdanger-recomp: the menu laid out for Raw Danger - the game's own options first, then the picture, then controls.
constexpr int kEntries = 7;
constexpr Command kCommands[kEntries] = {Command::resume, Command::save, Command::load, Command::graphics, Command::achievements, Command::language, Command::quit};
// the program's key for the same thing, shown dim at the right end of the entry
constexpr const char *kKeys[kEntries] = {"F1", "F5", "F8", "F3", "F4", "", ""};
// Settings: one page, five tabs (the Game tab lists the game's own options, filled by the window)
constexpr const char *kTabNames[Menu::kTabs] = {"DISPLAY", "PICTURE", "SOUND", "CONTROLS", "GAME"};
constexpr int kAchievementRows = 7;   // rawdanger-recomp: achievements lines shown at a time (bigger panel)
constexpr Command kDisplay[] = {Command::scale, Command::window_size, Command::fullscreen, Command::widescreen, Command::integer_scale, Command::menu_game_runs, Command::pause_unfocused, Command::menu_skin, Command::popup_size, Command::back};
constexpr const char *kDisplayKeys[] = {"", "", "F11", "", "", "", "", "", "", "Esc"};
constexpr Command kPicture[] = {Command::look, Command::antialias, Command::sharpen, Command::sharp_2d, Command::game_blur, Command::filter_pass, Command::ambient_occlusion, Command::black_bars, Command::lens_rain, Command::back};
constexpr const char *kPictureKeys[] = {"F2", "", "", "", "", "", "", "", "", "Esc"};
constexpr Command kSound[] = {Command::master_volume, Command::reverb, Command::back};
constexpr const char *kSoundKeys[] = {"", "", "Esc"};
constexpr Command kControls[] = {Command::change_keys, Command::keys_on_screen, Command::mouse_look, Command::mouse_speed, Command::mouse_invert_x, Command::mouse_invert, Command::key_prompts, Command::back};
constexpr const char *kControlsKeys[] = {"", "F10", "", "", "", "", "", "Esc"};
struct TabList { const Command *commands; const char *const *keys; int n; };
static_assert(std::size(kDisplay) == std::size(kDisplayKeys) && std::size(kPicture) == std::size(kPictureKeys) && std::size(kSound) == std::size(kSoundKeys) &&
              std::size(kControls) == std::size(kControlsKeys), "one key text per line");
constexpr TabList kTabLists[Menu::kTabs - 1] = {{kDisplay, kDisplayKeys, static_cast<int>(std::size(kDisplay))}, {kPicture, kPictureKeys, static_cast<int>(std::size(kPicture))},
                                                {kSound, kSoundKeys, static_cast<int>(std::size(kSound))}, {kControls, kControlsKeys, static_cast<int>(std::size(kControls))}};
constexpr int kVisibleRows = 11;  // lines of the keys page shown at once; the page scrolls
constexpr int kKeysPageChars = 58;

constexpr std::uint32_t rgba(int r, int g, int b, int a) {
    return static_cast<std::uint32_t>(r) | static_cast<std::uint32_t>(g) << 8 | static_cast<std::uint32_t>(b) << 16 | static_cast<std::uint32_t>(a) << 24;
}
// Raw Danger's hazard look (the launcher's): black panel, warning yellow, a strip of hazard tape over the top
// (set_skin switches these between the two looks)
std::uint32_t kPanel = rgba(14, 13, 10, 226), kFrame = rgba(120, 104, 40, 255), kTitle = rgba(255, 206, 0, 255),
              kText = rgba(236, 232, 220, 255), kBar = rgba(255, 206, 0, 250), kBarText = rgba(16, 14, 8, 255),
              kDim = rgba(150, 144, 128, 255), kBarDim = rgba(92, 70, 0, 255), kTape1 = rgba(255, 206, 0, 255),
              kTape2 = rgba(18, 16, 10, 255), kAlarm = rgba(214, 40, 30, 255);
int g_skin = 0;

// the game's font (skin 2): coverage of the 384 x 384 font picture; ASCII 0x20-0x7E in cells of 24 x 24 from row 2, 16 a row
struct GameFont {
    int w = 0, h = 0;
    std::vector<std::uint8_t> a;
    int x0[95] = {}, x1[95] = {};
    bool ok = false;
} g_font;

// one font pixel `s` of the menu is about two pixels of the game's font: its capitals (15 pixels) are 7 font pixels tall, as the
// menu's own letters are
inline float game_font_k(int s) { return s * 0.47f; }

int game_text_width(int s, const std::string &line) {
    const float k = game_font_k(s);
    float x = 0;
    for (unsigned char ch : line) {
        const int c = (ch >= 32 && ch <= 126 ? ch : '?') - 32;
        x += (g_font.x1[c] - g_font.x0[c] + 1) * k + s * 0.96f;
    }
    return static_cast<int>(x + 0.5f);
}

void game_text(Picture &p, int x, int y, int s, const std::string &line, std::uint32_t color) {
    const float k = game_font_k(s), inv = 1.0f / k;
    float pen = static_cast<float>(x);
    const int top = 4;   // the font's capitals start 4 pixels into their cell
    for (unsigned char ch : line) {
        const int c = (ch >= 32 && ch <= 126 ? ch : '?') - 32;
        const int cx = (c % 16) * 24 + g_font.x0[c], cy = (2 + c / 16) * 24 + top, gw = g_font.x1[c] - g_font.x0[c] + 1, gh = 19;
        const int tw = static_cast<int>(gw * k + 1.0f), th = static_cast<int>(gh * k + 1.0f), tx0 = static_cast<int>(pen);
        for (int ty = 0; ty < th; ++ty)
            for (int tx = 0; tx < tw; ++tx) {
                // the source pixels under this one, averaged (the font is made smaller)
                const float sx0 = tx * inv, sy0 = ty * inv, sx1 = std::min<float>(static_cast<float>(gw), (tx + 1) * inv),
                            sy1 = std::min<float>(static_cast<float>(gh), (ty + 1) * inv);
                float sum = 0, area = 0;
                for (int sy = static_cast<int>(sy0); sy < static_cast<int>(std::ceil(sy1)); ++sy)
                    for (int sx = static_cast<int>(sx0); sx < static_cast<int>(std::ceil(sx1)); ++sx) {
                        const float ox = std::min<float>(sx + 1.0f, sx1) - std::max<float>(static_cast<float>(sx), sx0),
                                    oy = std::min<float>(sy + 1.0f, sy1) - std::max<float>(static_cast<float>(sy), sy0);
                        if (ox <= 0 || oy <= 0) continue;
                        sum += g_font.a[static_cast<std::size_t>(cy + sy) * g_font.w + cx + sx] * ox * oy;
                        area += ox * oy;
                    }
                if (area <= 0) continue;
                const float cov = std::min(1.0f, sum / area / 255.0f * 1.15f);
                if (cov < 0.02f) continue;
                const int px = tx0 + tx, py = y + ty;
                if (px < 0 || py < 0 || px >= p.width || py >= p.height) continue;
                std::uint32_t &d = p.rgba[static_cast<std::size_t>(py) * p.width + px];
                const auto mix = [&](int sh) {
                    const float a = static_cast<float>((color >> sh) & 255u), b = static_cast<float>((d >> sh) & 255u);
                    return static_cast<std::uint32_t>(a * cov + b * (1 - cov) + 0.5f) << sh;
                };
                d = mix(0) | mix(8) | mix(16) | (std::max(d >> 24, static_cast<std::uint32_t>(cov * static_cast<float>(color >> 24))) << 24);
            }
        pen += gw * k + s * 0.96f;
    }
}

int shown_text_width(int s, const std::string &line) { return (g_skin == 2 && g_font.ok) ? game_text_width(s, line) : static_cast<int>(line.size()) * 6 * s; }

struct Canvas {
    Picture &p;
    // hazard tape: diagonal yellow and black bands, `band` pixels wide
    void tape(int x, int y, int w, int h, int band) {
        if (g_skin == 2) {
            // the game's header strip: dark to steel blue from left to right, with a faint grid
            for (int j = std::max(0, y); j < std::min(p.height, y + h); ++j)
                for (int i = std::max(0, x); i < std::min(p.width, x + w); ++i) {
                    const float f = w > 1 ? static_cast<float>(i - x) / (w - 1) : 0.f;
                    const bool grid = ((i - x) % std::max(3, band * 2) == 0) || ((j - y) % std::max(3, band) == 0);
                    const auto lerp = [f](int a, int b) { return static_cast<int>(a + (b - a) * f); };
                    const int g = grid ? 18 : 0;
                    p.rgba[static_cast<std::size_t>(j) * p.width + i] = rgba(lerp(14, 60) + g, lerp(30, 96) + g, lerp(48, 140) + g, 255);
                }
            return;
        }
        for (int j = std::max(0, y); j < std::min(p.height, y + h); ++j)
            for (int i = std::max(0, x); i < std::min(p.width, x + w); ++i)
                p.rgba[static_cast<std::size_t>(j) * p.width + i] = ((i + j) / std::max(1, band)) % 2 ? kTape2 : kTape1;
    }
    void fill(int x, int y, int w, int h, std::uint32_t color) {
        const int x0 = std::max(0, x), y0 = std::max(0, y), x1 = std::min(p.width, x + w), y1 = std::min(p.height, y + h);
        for (int j = y0; j < y1; ++j) std::fill(p.rgba.begin() + j * p.width + x0, p.rgba.begin() + j * p.width + x1, color);
    }
    // one letter is 5 x 8 pixels of the font, each `s` x `s` screen pixels; a letter is 6 font pixels wide with its gap
    void box(int x, int y, int w, int h, int t, std::uint32_t color) {
        fill(x, y, w, t, color);
        fill(x, y + h - t, w, t, color);
        fill(x, y, t, h, color);
        fill(x + w - t, y, t, h, color);
    }
    // rawdanger-recomp: a key cap (a box with a lighter top edge and the key's name), `s` font pixels; returns its width
    int keycap(int x, int y, int s, const std::string &name, std::uint32_t face, std::uint32_t edge, std::uint32_t ink) {
        // as wide as its name in the letters in use (the game's own font is narrower or wider than the menu's)
        const int text_w = (g_skin == 2 && g_font.ok) ? game_text_width(s, name) : static_cast<int>(name.size()) * 6 * s;
        const int w = text_w + 5 * s, h = 11 * s, t = std::max(1, s / 2);
        fill(x, y, w, h, face);
        box(x, y, w, h, t, edge);
        fill(x + t, y + h - 2 * t, w - 2 * t, t, edge);   // its lower lip, like a real key
        text(x + 3 * s, y + 2 * s, s, name, ink);
        return w;
    }
    // rawdanger-recomp: the PlayStation's face buttons (cross, circle, square, triangle) as drawn symbols, d x d window pixels
    void symbol(int x, int y, int d, int which, std::uint32_t color) {
        const float r = d * 0.5f, cx = x + r, cy = y + r, line = std::max(1.0f, d / 9.0f);
        for (int j = 0; j < d; ++j)
            for (int i = 0; i < d; ++i) {
                const float px = x + i + 0.5f - cx, py = y + j + 0.5f - cy;
                bool on = false;
                if (which == 0) on = (std::fabs(px - py) < line || std::fabs(px + py) < line) && std::fabs(px) < r * 0.72f && std::fabs(py) < r * 0.72f;  // cross
                else if (which == 1) on = std::fabs(std::sqrt(px * px + py * py) - r * 0.7f) < line;                                                   // circle
                else if (which == 2) on = std::max(std::fabs(px), std::fabs(py)) < r * 0.68f && std::max(std::fabs(px), std::fabs(py)) > r * 0.68f - line * 1.4f;  // square
                else {
                    // triangle: three edges of an upright triangle
                    const float top = -r * 0.62f, bottom = r * 0.5f, half = r * 0.7f;
                    if (py >= top - line && py <= bottom + line) {
                        const float k = (py - top) / (bottom - top), edge_x = k * half;
                        on = (std::fabs(std::fabs(px) - edge_x) < line * 1.3f && py <= bottom) || (std::fabs(py - bottom) < line && std::fabs(px) <= half);
                    }
                }
                if (on && x + i >= 0 && y + j >= 0 && x + i < p.width && y + j < p.height) p.rgba[static_cast<std::size_t>(y + j) * p.width + x + i] = color;
            }
    }
    void text(int x, int y, int s, const std::string &line, std::uint32_t color) {
        if (g_skin == 2 && g_font.ok) {
            game_text(p, x, y, s, line, color);
            return;
        }
        for (unsigned char c : line) {
            const char *glyph = kFont[(c >= 32 && c <= 126 ? c : '?') - 32];
            for (int row = 0; row < 8; ++row)
                for (int col = 0; col < 5; ++col)
                    if (glyph[row * 6 + col] == '#') fill(x + col * s, y + row * s, s, s, color);
            x += 6 * s;
        }
    }
};

}  // namespace

bool load_game_font(const std::string &path) {
    std::FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    char magic[4] = {};
    std::uint32_t w = 0, h = 0;
    bool good = std::fread(magic, 1, 4, f) == 4 && std::memcmp(magic, "RDF1", 4) == 0 && std::fread(&w, 4, 1, f) == 1 && std::fread(&h, 4, 1, f) == 1 && w >= 384 &&
                h >= 288 && w <= 4096 && h <= 4096;
    if (good) {
        g_font.a.resize(static_cast<std::size_t>(w) * h);
        good = std::fread(g_font.a.data(), 1, g_font.a.size(), f) == g_font.a.size();
    }
    std::fclose(f);
    if (!good) return false;
    g_font.w = static_cast<int>(w);
    g_font.h = static_cast<int>(h);
    for (int c = 0; c < 95; ++c) {
        const int cx = (c % 16) * 24, cy = (2 + c / 16) * 24;
        int lo = 24, hi = -1;
        for (int y = 0; y < 24; ++y)
            for (int x = 0; x < 24; ++x)
                if (g_font.a[static_cast<std::size_t>(cy + y) * w + cx + x] > 40) lo = std::min(lo, x), hi = std::max(hi, x);
        if (hi < lo) lo = 0, hi = 5;   // the space
        g_font.x0[c] = lo;
        g_font.x1[c] = hi;
    }
    g_font.ok = true;
    return true;
}

bool game_font_loaded() { return g_font.ok; }

void set_skin(int skin) {
    g_skin = skin == 1 || skin == 2 ? skin : 0;
    if (g_skin == 2) {
        // Raw Danger's Options screen: navy, thin pale-blue lines, white words, a steel-blue bar on the chosen line
        kPanel = rgba(10, 22, 44, 240);    kFrame = rgba(150, 178, 214, 255); kTitle = rgba(236, 242, 252, 255);
        kText = rgba(214, 226, 244, 255);  kBar = rgba(52, 104, 162, 245);   kBarText = rgba(255, 255, 255, 255);
        kDim = rgba(112, 134, 168, 255);   kBarDim = rgba(190, 212, 240, 255); kTape1 = rgba(56, 100, 150, 255);
        kTape2 = rgba(16, 34, 60, 255);    kAlarm = rgba(110, 190, 240, 255);
    } else if (g_skin == 1) {
        // the launcher's Japanese skin (Theme in launcher\Launcher.cs): white-blue paper, navy text, blue bars and tape
        kPanel = rgba(246, 250, 254, 248); kFrame = rgba(150, 184, 226, 255); kTitle = rgba(18, 86, 196, 255);
        kText = rgba(14, 30, 64, 255);     kBar = rgba(18, 86, 196, 250);    kBarText = rgba(255, 255, 255, 255);
        kDim = rgba(84, 108, 146, 255);    kBarDim = rgba(196, 216, 244, 255); kTape1 = rgba(40, 110, 214, 255);
        kTape2 = rgba(236, 244, 253, 255); kAlarm = rgba(0, 160, 230, 255);
    } else {
        kPanel = rgba(14, 13, 10, 226);   kFrame = rgba(120, 104, 40, 255); kTitle = rgba(255, 206, 0, 255);
        kText = rgba(236, 232, 220, 255); kBar = rgba(255, 206, 0, 250);    kBarText = rgba(16, 14, 8, 255);
        kDim = rgba(150, 144, 128, 255);  kBarDim = rgba(92, 70, 0, 255);   kTape1 = rgba(255, 206, 0, 255);
        kTape2 = rgba(18, 16, 10, 255);   kAlarm = rgba(214, 40, 30, 255);
    }
}

int skin() { return g_skin; }

namespace {
int g_popup = 1;
// a pop-up's size: its own size for the window (s, font pixels), times 3/4, 1, 3/2 or 2
int popup_scale(int s) {
    static const int kTimes4[] = {3, 4, 6, 8};
    return std::max(1, (s * kTimes4[std::clamp(g_popup, 0, 3)] + 2) / 4);
}
}  // namespace
void set_popup_size(int size) { g_popup = std::clamp(size, 0, 3); }
int popup_size() { return g_popup; }

namespace {

std::string entry_text(Command c, const Values &v, bool quit_armed) {
    switch (c) {
    case Command::resume: return "Resume";
    case Command::save: return "Quick save";
    case Command::load: return "Quick load";
    case Command::recomp: return "Game options";
    case Command::graphics: return "Settings";
    case Command::window_size: return "Window size:  < " + (v.window_size.empty() ? std::string("?") : v.window_size) + " >";
    case Command::game_blur:
        if (v.game_blur < 0) return "Original PS2 Post Processing Filter: as built";
        return std::string("Original PS2 Post Processing Filter:  < ") + (v.game_blur ? "on" : "off") + " >";
    case Command::mouse_look:
        if (v.mouse_look < 0) return "Mouse look: not in this game";
        return std::string("Mouse look (camera):  < ") + (v.mouse_look ? "on" : "off") + " >";
    case Command::mouse_speed: {
        char text[64];
        std::snprintf(text, sizeof text, "Mouse speed:  < %.1f >", v.mouse_speed);
        return text;
    }
    case Command::mouse_invert_x: return std::string("Mouse left and right:  < ") + (v.mouse_invert_x ? "inverted" : "normal") + " >";
    case Command::mouse_invert: return std::string("Mouse up and down:  < ") + (v.mouse_invert_y ? "inverted" : "normal") + " >";
    case Command::key_prompts:
        if (v.key_prompts < 0) return "Buttons in the game's texts: PlayStation (no key pictures here)";
        return std::string("Buttons in the game's texts:  < ") + (v.key_prompts ? "your keys" : "PlayStation symbols") + " >";
    case Command::sharpen: {
        static const char *const kLevels[] = {"off", "low", "medium", "high"};
        return std::string("Sharpen the picture:  < ") + kLevels[std::clamp(v.sharpen, 0, 3)] + " >";
    }
    case Command::black_bars: return "Cutscene black bars:  < " + (v.black_bars <= 0 ? std::string("removed") : std::to_string(v.black_bars) + " % opaque") + " >";
    case Command::reverb: return std::string("Reverb (rooms, tunnels):  < ") + (v.reverb ? "on" : "off") + " >";
    case Command::widescreen: return v.widescreen < 0 ? "Widescreen: not in this game" : std::string("Widescreen 16:9:  < ") + (v.widescreen ? "on" : "off") + " >";
    case Command::ambient_occlusion: {
        static const char *const kLevels[] = {"off", "light", "strong"};
        return std::string("Ambient occlusion (shading in corners):  < ") + kLevels[std::clamp(v.ambient_occlusion, 0, 2)] + " >";
    }
    case Command::filter_pass: return std::string("  drawn:  < ") + (v.filter_pass ? "one GPU pass (fast)" : "the game's own draws (slow, to compare)") + " >";
    case Command::popup_size: {
        static const char *const kSizes[] = {"small", "normal", "large", "huge"};
        return std::string("Pop-up size (achievements, notes, keys):  < ") + kSizes[std::clamp(v.popup_size, 0, 3)] + " >";
    }
    case Command::keys_on_screen: return std::string("Keys on screen while playing:  < ") + (v.keys_on_screen ? "on" : "off") + " >";
    case Command::menu_game_runs: return std::string("Game runs behind this menu:  < ") + (v.menu_game_runs ? "on" : "off") + " >";
    case Command::pause_unfocused: return std::string("Pause in the background:  < ") + (v.pause_unfocused ? "on" : "off") + " >";
    case Command::master_volume: {
        const int pct = std::clamp(v.master_volume, 0, 100);
        return "Master volume:  < " + (pct == 0 ? std::string("muted") : std::to_string(pct) + "%") + " >  " + std::string(static_cast<std::size_t>(pct / 10), '|') +
               std::string(static_cast<std::size_t>(10 - pct / 10), '.');
    }
    case Command::back: return "Back";
    case Command::achievements: return "Achievements";
    case Command::look: return "Upscale filter:  < " + (v.filter.empty() ? v.look : v.filter) + " >";
    case Command::antialias: {
        static const char *const kAa[] = {"off", "smooth edges", "SMAA low", "SMAA medium", "SMAA high", "SMAA ultra", "FXAA"};
        return std::string("Anti-aliasing:  < ") + kAa[std::clamp(v.antialias, 0, 6)] + " >";
    }
    case Command::integer_scale: return std::string("Whole-number scaling:  < ") + (v.integer_scale ? "on" : "off") + " >";
    case Command::menu_skin: return std::string("Menu look:  < ") + (v.menu_skin == 2 ? "the game's Options screen" : v.menu_skin == 1 ? "Japanese (blue on white)" : "hazard (yellow on black)") + " >";
    case Command::lens_rain:
        return std::string("Rain on the lens:  < ") + (v.lens_rain ? "on" : "off") + " >" + (v.lens_rain && !v.lens_rain_known ? "  (also on menus)" : "");
    case Command::scale: {
        // the inner resolution: the console's 640 x 448 times 1 to 8, or auto (the one at least as tall as the screen)
        const auto name = [](int n) {
            if (n == 9) return std::string("auto");
            return std::to_string(640 * n) + " x " + std::to_string(448 * n) + " (" + std::to_string(n) + "x)";
        };
        if (v.scale_locked) return "Internal resolution: " + name(v.scale_now) + " (set by PS2_SCALE)";
        return "Internal resolution:  < " + name(v.scale_next) + " >";
    }
    case Command::sharp_2d: {
        static const char *const kNames[] = {"soft", "sharp", "sharpest"};
        const auto name = [](int n) { return kNames[n >= 0 && n <= 2 ? n : 1]; };
        if (v.sharp_locked) return std::string("Text and menus: ") + name(v.sharp_now) + " (set by PS2_SHARP_2D)";
        return std::string("Text and menus:  < ") + name(v.sharp_next) + " >";
    }
    case Command::fullscreen: return std::string("Full screen:  < ") + (v.fullscreen ? "on" : "off") + " >";
    case Command::keys: return "Key card (all keys at a glance)";
    case Command::change_keys: return "Change keys (keyboard, mouse)";
    case Command::quit: return quit_armed ? "Quit to desktop?  Enter / Cross again" : "Quit to desktop";
    case Command::language:
        if (v.language_other.empty())
            return "Language: " + (v.language_now.empty() ? std::string("this version only") : v.language_now) +
                   (v.language_now == "English" ? "  (Japanese: add the JP disc in the launcher)" : v.language_now == "Japanese" ? "  (English: add the US disc in the launcher)" : "");
        return quit_armed ? "Restart in " + v.language_other + "?  Save in the game first.  Enter / Cross again"
                          : "Language: " + v.language_now + "  > play in " + v.language_other + " (restarts, saves carried over)";
    default: return "";
    }
}

// Settings' tab row: each name in a box, the current one filled; xs gets each tab's left edge and, last, the right end of the row
void tabs_row(Canvas &c, int x, int y, int s, int current, int *xs) {
    for (int i = 0; i < Menu::kTabs; ++i) {
        const std::string name = kTabNames[i];
        const int w = (static_cast<int>(name.size()) + 2) * 6 * s;
        xs[i] = x;
        if (i == current) c.fill(x, y, w, 12 * s, kBar);
        else {
            c.fill(x, y + 11 * s, w, std::max(1, s / 2), kFrame);
        }
        c.text(x + 6 * s, y + 2 * s, s, name, i == current ? kBarText : kDim);
        x += w + 2 * s;
    }
    xs[Menu::kTabs] = x;
}

}  // namespace

bool Menu::click_tab(int x, int y) {
    if (page_ != Page::graphics || tab_h_ <= 0 || y < tab_y_ || y >= tab_y_ + tab_h_) return false;
    for (int i = 0; i < kTabs; ++i)
        if (x >= tab_x_[i] && x < tab_x_[i + 1]) {
            if (i != tab_) { tab_ = i; selected_ = 0; top_ = 0; }
            return true;
        }
    return false;
}

int Menu::count() const {
    if (page_ == Page::graphics) return tab_ == kGameTab ? recomp_rows_ + 1 : kTabLists[tab_].n;
    return page_ == Page::keys ? key_rows_ + 2 : page_ == Page::recomp || page_ == Page::achievements ? recomp_rows_ + 1 : kEntries;
}

void Menu::tab(int delta) {
    if (page_ != Page::graphics) return;
    tab_ = (tab_ + delta % kTabs + kTabs) % kTabs;
    selected_ = 0;
    top_ = 0;
    quit_armed_ = false;
}

void Menu::fit() {
    const int shown = std::min(key_rows_, kVisibleRows);
    if (selected_ < key_rows_) {
        if (selected_ < top_) top_ = selected_;
        if (selected_ >= top_ + shown) top_ = selected_ - shown + 1;
    }
    top_ = std::clamp(top_, 0, std::max(0, key_rows_ - shown));
}

bool Menu::back() {
    if (page_ == Page::main) return false;
    if (native_ && page_ == Page::graphics) return false;   // opened from the game's Options screen: back to the game
    if (page_ == Page::keys && keys_from_settings_) {
        // back to Settings > Controls, on Change keys
        keys_from_settings_ = false;
        page_ = Page::graphics;
        tab_ = 3;
        selected_ = 0;
        capturing_ = false;
        defaults_armed_ = false;
        return true;
    }
    page_ = Page::main;
    selected_ = main_selected_;
    capturing_ = false;
    defaults_armed_ = false;
    return true;
}

void Menu::open() {
    open_ = true;
    native_ = false;
    quit_armed_ = false;
    page_ = Page::main;
    capturing_ = false;
    defaults_armed_ = false;
    top_ = 0;
    selected_ = 0;  // always starts on Resume, so a second press of the key that opened it gives the game back
}

void Menu::move(int delta) {
    quit_armed_ = false;
    defaults_armed_ = false;
    const int n = count();
    selected_ = (selected_ + delta % n + n) % n;
    if (page_ == Page::keys) fit();
}

Command Menu::choose() {
    if (page_ == Page::recomp) {
        if (selected_ < recomp_rows_) return Command::recomp_toggle;
        back();
        return Command::none;
    }
    if (page_ == Page::achievements) {
        if (selected_ < recomp_rows_) return Command::achievement_toggle;
        back();
        return Command::none;
    }
    if (page_ == Page::keys) {
        if (selected_ < key_rows_) {
            capturing_ = true;
            return Command::key_edit;
        }
        if (selected_ == key_rows_) {
            if (!defaults_armed_) {
                defaults_armed_ = true;  // asks once
                return Command::none;
            }
            defaults_armed_ = false;
            return Command::key_defaults;
        }
        back();
        return Command::none;
    }
    if (page_ == Page::graphics) {
        if (tab_ == kGameTab) {
            if (selected_ < recomp_rows_) return Command::recomp_toggle;
            back();
            return Command::none;
        }
        const Command g = kTabLists[tab_].commands[selected_];
        if (g == Command::back) { if (native_ && page_ == Page::graphics) return Command::resume; back(); return Command::none; }
        if (g == Command::change_keys) {
            keys_from_settings_ = true;
            page_ = Page::keys;
            selected_ = 0;
            top_ = 0;
            capturing_ = false;
            defaults_armed_ = false;
        }
        return g;
    }
    const Command c = kCommands[selected_];
    if (c == Command::graphics) {
        main_selected_ = selected_;
        page_ = Page::graphics;
        selected_ = 0;
        top_ = 0;
        return c;
    }
    if (c == Command::recomp) {
        main_selected_ = selected_;
        page_ = Page::recomp;
        selected_ = 0;
        return c;
    }
    if (c == Command::achievements) {
        main_selected_ = selected_;
        page_ = Page::achievements;
        selected_ = 0;
        top_ = 0;
        return c;
    }
    if (c == Command::change_keys) {
        main_selected_ = selected_;
        page_ = Page::keys;
        selected_ = 0;
        top_ = 0;
        capturing_ = false;
        defaults_armed_ = false;
        return c;
    }
    if (c != Command::quit && c != Command::language) return c;
    if (quit_armed_) return c;
    quit_armed_ = true;  // asks once
    return Command::none;
}

Command Menu::side() const {
    // rawdanger-recomp: the game's options (Settings > Game, the F3 page) change with left / right too
    if ((page_ == Page::recomp || (page_ == Page::graphics && tab_ == kGameTab)) && selected_ < recomp_rows_) return Command::recomp_toggle;
    if (page_ != Page::graphics || tab_ == kGameTab) return Command::none;
    const Command c = kTabLists[tab_].commands[selected_];
    return c == Command::look || c == Command::scale || c == Command::sharp_2d || c == Command::fullscreen || c == Command::widescreen || c == Command::pause_unfocused || c == Command::menu_game_runs ||
                   c == Command::master_volume || c == Command::integer_scale || c == Command::lens_rain || c == Command::antialias || c == Command::menu_skin ||
                   c == Command::window_size || c == Command::game_blur || c == Command::reverb || c == Command::key_prompts || c == Command::mouse_look || c == Command::black_bars || c == Command::sharpen ||
                   c == Command::mouse_speed || c == Command::mouse_invert || c == Command::mouse_invert_x || c == Command::popup_size || c == Command::keys_on_screen || c == Command::filter_pass || c == Command::ambient_occlusion
               ? c : Command::none;
}

int Menu::click_step(int x) const {
    const int slot = selected_ - arrow_top_;
    if (slot < 0 || slot >= static_cast<int>(arrow_mid_.size()) || arrow_mid_[static_cast<std::size_t>(slot)] < 0) return 0;
    if (x < arrow_lo_[static_cast<std::size_t>(slot)] - entry_h_) return 0;   // on the name: the line is only chosen
    return x < arrow_mid_[static_cast<std::size_t>(slot)] ? -1 : 1;
}

bool Menu::select_at(int x, int y) {
    if (page_ == Page::graphics && tab_h_ > 0 && y >= tab_y_ && y < tab_y_ + tab_h_) {
        // a tab: hovering does nothing, the click (choose) is handled by click_tab
        return false;
    }
    if (page_ == Page::recomp || page_ == Page::achievements || (page_ == Page::graphics && tab_ == kGameTab)) {
        const int shown = std::min(recomp_rows_, page_ == Page::achievements ? kAchievementRows : 14);
        if (entry_h_ <= 0 || x < entry_x_ || x >= entry_x_ + entry_w_ || y < rows_y_ || y >= rows_y_ + (shown + 1) * entry_h_) return false;
        const int slot = (y - rows_y_) / entry_h_;
        selected_ = slot < shown ? top_ + slot : recomp_rows_;
        return true;
    }
    if (page_ == Page::keys) {
        if (capturing_ || entry_h_ <= 0 || x < entry_x_ || x >= entry_x_ + entry_w_) return false;
        int now = -1;
        if (y >= rows_y_ && y < rows_y_ + keys_shown_ * entry_h_) now = top_ + (y - rows_y_) / entry_h_;
        else if (y >= footer_y_ && y < footer_y_ + 2 * entry_h_) now = key_rows_ + (y - footer_y_) / entry_h_;
        if (now < 0) return false;
        if (now != selected_) defaults_armed_ = false;
        selected_ = now;
        return true;
    }
    if (entry_h_ <= 0 || x < entry_x_ || x >= entry_x_ + entry_w_ || y < entry_y_ || y >= entry_y_ + count() * entry_h_) return false;
    const int now = (y - entry_y_) / entry_h_;
    if (now != selected_) quit_armed_ = false;
    selected_ = now;
    return true;
}

void Menu::draw_keys(int window_width, int window_height, const Values &values, Picture &out) {
    static const std::string kTitleText = "Change keys - the game waits", kHint = "Enter / click: change a key     Esc / Circle: back",
                             kFixed = "Not usable: F1 to F9, F11 (the program's)", kCapture = "Press a key or mouse button... (Esc: cancel)";
    key_rows_ = static_cast<int>(values.key_rows.size());
    fit();
    const int n = key_rows_, shown = std::min(n, kVisibleRows);
    keys_shown_ = shown;
    const int pad = 8, bar_h = 12, bar_pad = 5, label_chars = 18, keys_chars = 30;
    const int units_w = 2 * pad + 2 * bar_pad + kKeysPageChars * 6;
    const int units_h = pad + 8 + 12 + shown * bar_h + 3 + 2 * bar_h + 11 + 12 + 12 + 8 + pad;
    const int s = std::clamp(std::min(window_height * 4 / 5 / units_h, window_width * 23 / 25 / units_w), 1, 6);
    out.width = units_w * s;
    out.height = units_h * s;
    out.x = std::max(0, (window_width - out.width) / 2);
    out.y = std::max(0, (window_height - out.height) / 2);
    out.rgba.assign(static_cast<std::size_t>(out.width) * out.height, kPanel);
    Canvas c{out};
    const int frame = std::max(1, s / 2);
    c.fill(0, 0, out.width, frame, kFrame);
    c.fill(0, out.height - frame, out.width, frame, kFrame);
    c.fill(0, 0, frame, out.height, kFrame);
    c.fill(out.width - frame, 0, frame, out.height, kFrame);
    const auto fit_text = [](const std::string &t, std::size_t max) { return t.size() <= max ? t : t.substr(0, max - 3) + "..."; };
    const int text_x = (pad + bar_pad) * s, inner_w = out.width - 2 * pad * s;
    int y = pad * s;
    c.text(text_x, y, s, kTitleText, kTitle);
    if (n > shown) {
        const std::string where = std::to_string(top_ + 1) + "-" + std::to_string(top_ + shown) + " of " + std::to_string(n);
        c.text(out.width - text_x - static_cast<int>(where.size()) * 6 * s + s, y, s, where, kDim);
    }
    y += 8 * s + 6 * s;
    c.fill(pad * s, y, inner_w, std::max(1, s / 2), kFrame);
    y += 6 * s;
    entry_x_ = out.x + pad * s;
    entry_w_ = inner_w;
    entry_h_ = bar_h * s;
    rows_y_ = out.y + y;
    const auto line = [&](int index, const std::string &left, const std::string &right) {
        const bool lit = index == selected_;
        if (lit) c.fill(pad * s, y, inner_w, bar_h * s, kBar);
        const std::uint32_t color = lit ? kBarText : kText;
        // rawdanger-recomp: the PlayStation symbol before the face buttons, in their colours
        int lx = text_x;
        static const char *const kFaces[] = {"Cross", "Circle", "Square", "Triangle"};
        static const std::uint32_t kFaceColors[] = {rgba(124, 178, 232, 255), rgba(255, 102, 102, 255), rgba(255, 105, 248, 255), rgba(64, 226, 160, 255)};
        for (int f = 0; f < 4; ++f)
            if (left == kFaces[f]) {
                c.symbol(text_x - 2 * s, y + s, 10 * s, f, lit ? kBarText : kFaceColors[f]);
                lx = text_x + 11 * s;
            }
        c.text(lx, y + 2 * s, s, left, color);
        if (!right.empty()) {
            // the keys as key caps ("E, Mouse 1" -> [E] [Mouse 1]); what does not fit ends in "..."
            int kx = text_x + (label_chars + 1) * 6 * s;
            const int end = pad * s + inner_w - 2 * s;
            std::size_t from = 0;
            while (from <= right.size()) {
                std::size_t comma = right.find(", ", from);
                const std::string name = right.substr(from, comma == std::string::npos ? std::string::npos : comma - from);
                const int w = (static_cast<int>(name.size()) * 6 + 5) * s;
                if (kx + w > end) { c.text(kx, y + 2 * s, s, "...", color); break; }
                if (name == "none") c.text(kx, y + 2 * s, s, "(none)", lit ? kBarDim : kDim);
                else kx += c.keycap(kx, y + s / 2, s, name, lit ? kBarText : kPanel | 0xFF000000u, lit ? kBarDim : kFrame, lit ? kBar : kText) + 3 * s;
                if (comma == std::string::npos) break;
                from = comma + 2;
            }
        }
        y += bar_h * s;
    };
    for (int r = 0; r < shown; ++r) {
        const int index = top_ + r;
        const Values::KeyRow &row = values.key_rows[index];
        if (index == selected_ && capturing_) line(index, kCapture, "");
        else line(index, row.label, row.keys);
    }
    y += 3 * s;
    footer_y_ = out.y + y;
    line(n, defaults_armed_ ? "Really put back the default keys?  Enter / Cross again" : "Put back the default keys", "");
    line(n + 1, "Back", "");
    y += 5 * s;
    c.fill(pad * s, y, inner_w, std::max(1, s / 2), kFrame);
    y += 6 * s;
    c.text(text_x, y, s, fit_text(values.key_note, kKeysPageChars), kTitle);
    y += 12 * s;
    c.text(text_x, y, s, kHint, kDim);
    y += 12 * s;
    c.text(text_x, y, s, kFixed, kDim);
}

// The Recomp options page: the game's patches, the live ones switchable here (and in RawDangerPC.exe, same file).
void Menu::draw_recomp(int window_width, int window_height, const Values &values, Picture &out) {
    const bool ach = page_ == Page::achievements, tabs = page_ == Page::graphics;
    const std::string kTitleText = ach ? "ACHIEVEMENTS - offline" : tabs ? "SETTINGS" : "GAME OPTIONS - switched while you play",
                      kHint = ach ? "Enter / Cross: switch on / off     Esc / Circle: back" : tabs ? "Enter: switch   Tab / Q / E / L1 / R1: tabs   Esc: back" : "Enter / Cross: switch     Esc / Circle: back",
                      kBuilt = ach ? "* unlocked. Kept next to the saves; nothing goes online" : "Also in RawDangerPC.exe (Recomp options)";
    recomp_rows_ = static_cast<int>(values.recomp_rows.size());
    const int all = recomp_rows_;
    // long lists scroll: the highlighted line stays in view
    // the achievements show fewer lines at a time, so the panel (and its pictures) can be drawn bigger
    const int shown_max = ach ? kAchievementRows : 14;
    if (selected_ < all) {
        if (selected_ < top_) top_ = selected_;
        if (selected_ >= top_ + shown_max) top_ = selected_ - shown_max + 1;
    }
    top_ = std::clamp(top_, 0, std::max(0, all - shown_max));
    const int n = std::min(all, shown_max);
    const int pad = 8, bar_h = ach ? 18 : 12, bar_pad = 5, chars = 62;
    const int units_w = 2 * pad + 2 * bar_pad + chars * 6;
    const int units_h = pad + 8 + 12 + (ach ? 8 : 0) + (tabs ? 16 : 0) + (n + 1) * bar_h + 3 + 11 + (ach ? 24 : 0) + 12 + 12 + 12 + 8 + pad;
    int s = std::clamp(std::min(window_height * 4 / 5 / units_h, window_width * 23 / 25 / units_w), 1, 6);
    out.width = units_w * s;
    out.height = units_h * s;
    // Settings > Game: the very panel of the other tabs (size, hazard tape, title), so switching tabs does not make it jump
    const int tape_h = tabs ? 6 : 0;
    if (tabs && settings_s_ > 0) s = settings_s_, out.width = settings_w_, out.height = settings_h_;
    else if (tabs) out.height += tape_h * s;
    out.x = std::max(0, (window_width - out.width) / 2);
    out.y = std::max(0, (window_height - out.height) / 2);
    out.rgba.assign(static_cast<std::size_t>(out.width) * out.height, kPanel);
    Canvas c{out};
    const int frame = std::max(1, s / 2);
    c.fill(0, 0, out.width, frame, kFrame);
    c.fill(0, out.height - frame, out.width, frame, kFrame);
    c.fill(0, 0, frame, out.height, kFrame);
    c.fill(out.width - frame, 0, frame, out.height, kFrame);
    const int text_x = (pad + bar_pad) * s, inner_w = out.width - 2 * pad * s;
    int y = (pad + tape_h) * s;
    if (tabs) {
        c.tape(0, 0, out.width, tape_h * s, 4 * s);
        c.fill(text_x, y, 2 * s, 8 * s, kAlarm);   // the red mark before the title, as on the other tabs
        c.text(text_x + 4 * s, y, s, kTitleText, kTitle);
    } else {
        c.text(text_x, y, s, kTitleText, kTitle);
    }
    y += 8 * s + 6 * s;
    if (tabs) {
        int xs[kTabs + 1];
        tabs_row(c, text_x - 2 * s, y, s, tab_, xs);
        for (int i = 0; i <= kTabs; ++i) tab_x_[i] = out.x + xs[i];
        tab_y_ = out.y + y;
        tab_h_ = 12 * s;
        y += 16 * s;
    } else {
        tab_h_ = 0;
    }
    if (ach) {
        // how far the player is: a bar of the points won, hazard tape for the part still to win
        const int bw = inner_w - 2 * bar_pad * s, bh = 4 * s;
        const float f = std::clamp(values.progress < 0 ? 0.f : values.progress, 0.f, 1.f);
        c.tape(text_x, y, bw, bh, std::max(2, 2 * s));
        c.fill(text_x, y, bw, bh, (kPanel & 0x00FFFFFFu) | 0xAA000000u);
        c.fill(text_x, y, static_cast<int>(bw * f), bh, kTitle);
        y += 8 * s;
    }
    c.fill(pad * s, y, inner_w, std::max(1, s / 2), kFrame);
    y += 6 * s;
    entry_x_ = out.x + pad * s;
    entry_w_ = inner_w;
    entry_h_ = bar_h * s;
    rows_y_ = out.y + y;
    arrow_lo_.assign(static_cast<std::size_t>(n + 1), -1), arrow_mid_.assign(static_cast<std::size_t>(n + 1), -1), arrow_top_ = top_;
    const auto line = [&](int index, const std::string &left, const std::string &right, bool dim, const Values::Icon *icon = nullptr) {
        const bool lit = index == selected_;
        if (const std::size_t lo = right.find("< "), hi = right.rfind(" >"); lo != std::string::npos && hi != std::string::npos && hi > lo) {
            const std::size_t slot = static_cast<std::size_t>(index >= all ? n : index - top_);
            const int rx = out.x + out.width - text_x - shown_text_width(s, right);
            if (slot < arrow_lo_.size()) {
                arrow_lo_[slot] = rx + shown_text_width(s, right.substr(0, lo));
                arrow_mid_[slot] = rx + (shown_text_width(s, right.substr(0, lo)) + shown_text_width(s, right.substr(0, hi + 2))) / 2;
            }
        }
        if (lit) c.fill(pad * s, y, inner_w, bar_h * s, kBar);
        const std::uint32_t color = lit ? kBarText : dim ? kDim : kText;
        int tx = text_x;
        if (ach && index < all) {
            // the badge, a square as tall as the line (nearest pixel), then the name beside it
            const int size = (bar_h - 2) * s, ix = text_x - 2 * s, iy = y + s;
            if (icon && icon->width > 0 && icon->height > 0) {
                for (int j = 0; j < size; ++j)
                    for (int i = 0; i < size; ++i) {
                        const std::uint32_t px = icon->rgba[static_cast<std::size_t>(j * icon->height / size) * icon->width + i * icon->width / size];
                        c.fill(ix + i, iy + j, 1, 1, px | 0xFF000000u);
                    }
            } else {
                c.fill(ix, iy, size, size, kFrame);
            }
            tx = ix + size + 4 * s;
        }
        const std::size_t room = static_cast<std::size_t>(ach ? 36 : 40);
        c.text(tx, y + (bar_h - 8) / 2 * s, s, left.size() > room ? left.substr(0, room - 3) + "..." : left, color);
        if (!right.empty()) c.text(out.width - text_x - shown_text_width(s, right), y + (bar_h - 8) / 2 * s, s, right, lit ? kBarDim : color);
        y += bar_h * s;
    };
    if (n == 0)
        c.text(text_x, y + 2 * s, s, ach ? "No achievements loaded" : tabs ? "Nothing else here: widescreen is in Display, the post processing in Picture" : "No options file (the game was not built by RawDangerPC.exe)", kDim);
    for (int r = 0; r < n; ++r) {
        const Values::RecompRow &row = values.recomp_rows[top_ + r];
        line(top_ + r, row.label, row.state, !row.live, row.icon);
    }
    if (n == 0) y += bar_h * s;
    line(all, "Back", "", false);
    y += 3 * s;
    c.fill(pad * s, y, inner_w, std::max(1, s / 2), kFrame);
    y += 6 * s;
    if (ach) {
        // the highlighted achievement: what it asks for (two lines of 62 letters, cut at a space)
        std::string d = selected_ < all ? values.recomp_rows[selected_].detail : std::string();
        for (int k = 0; k < 2; ++k) {
            std::string part = d;
            if (part.size() > static_cast<std::size_t>(chars)) {
                std::size_t cut = part.rfind(' ', chars);
                if (cut == std::string::npos || cut < 20) cut = chars;
                part = d.substr(0, cut);
                d = d.substr(std::min(d.size(), cut + 1));
            } else {
                d.clear();
            }
            if (k == 1 && !d.empty()) part = part.substr(0, std::max<std::size_t>(0, part.size() > 3 ? part.size() - 3 : 0)) + "...";
            c.text(text_x, y, s, part, kText);
            y += 12 * s;
        }
    }
    c.text(text_x, y, s, values.recomp_note.size() > 62 ? values.recomp_note.substr(0, 59) + "..." : values.recomp_note, kTitle);
    y += 12 * s;
    c.text(text_x, y, s, kHint, kDim);
    y += 12 * s;
    c.text(text_x, y, s, kBuilt, kDim);
}

void Menu::draw(int window_width, int window_height, const Values &values, Picture &out) {
    if (page_ == Page::recomp || page_ == Page::achievements || (page_ == Page::graphics && tab_ == kGameTab)) {
        draw_recomp(window_width, window_height, values, out);
        return;
    }
    if (page_ == Page::keys) {
        draw_keys(window_width, window_height, values, out);
        return;
    }
    draw_list(window_width, window_height, values, out);
}

// The main page and the Graphics page: a title under a strip of hazard tape, one bar per entry with its key at the right end.
void Menu::draw_list(int window_width, int window_height, const Values &values, Picture &out) {
    const bool gfx = page_ == Page::graphics;
    const int n = gfx ? kTabLists[tab_].n : kEntries;
    const Command *commands = gfx ? kTabLists[tab_].commands : kCommands;
    const char *const *keys = gfx ? kTabLists[tab_].keys : kKeys;
    std::string game = values.title.empty() ? (values.game_waits ? "PAUSED" : "MENU") : values.title;
    for (char &ch : game) ch = static_cast<char>(ch >= 'a' && ch <= 'z' ? ch - 32 : ch);
    const std::string kTitleText = gfx ? (native_ ? "PC SETTINGS" : "SETTINGS") : (values.game_waits || values.title.empty() ? game + (values.title.empty() ? "" : " - PAUSED") : game);
    const std::string kHint = gfx ? "Left / Right: change   Tab / Q / E / L1 / R1: tabs   Esc: back" : "Enter / Cross: OK     F1 / Esc / Circle: resume";
    const std::string kNote = gfx ? (values.scale_next != values.scale_now || values.sharp_next != values.sharp_now ? "Resolution and text filter: used at the next start" : "Changes apply at once and are kept")
                                  : "";
    std::vector<std::string> lines(n);
    std::size_t chars = std::max({kTitleText.size(), kHint.size(), kNote.size()});
    for (int i = 0; i < n; ++i) {
        lines[i] = entry_text(commands[i], values, quit_armed_ && i == selected_);
        chars = std::max(chars, lines[i].size() + 5);  // room for the dim key at the right end
    }
    // everything in font pixels first; one font pixel is `s` window pixels, as big as fits in about 80 % of the window
    const int pad = 8, bar_h = 14, bar_pad = 5, tape_h = 6;
    const int units_w = 2 * pad + 2 * bar_pad + static_cast<int>(chars) * 6;
    // Settings: every tab's panel as big as the biggest, so switching tabs does not make it jump
    int rows = n;
    if (gfx) {
        rows = 0;
        for (const TabList &t : kTabLists) rows = std::max(rows, t.n);
        for (const TabList &t : kTabLists)
            for (int i = 0; i < t.n; ++i) chars = std::max(chars, entry_text(t.commands[i], values, false).size() + 5);
        chars = std::max<std::size_t>(chars, 62);   // as wide as the Game tab (its list is 62 letters)
    }
    const int units_h = tape_h + pad + 8 + 12 + (gfx ? 16 : 0) + rows * bar_h + 11 + 8 + 4 + 8 + pad;
    const int s = std::clamp(std::min(window_height * 4 / 5 / units_h, window_width * 23 / 25 / units_w), 1, 6);
    out.width = units_w * s;
    out.height = units_h * s;
    out.x = std::max(0, (window_width - out.width) / 2);
    out.y = std::max(0, (window_height - out.height) / 2);
    out.rgba.assign(static_cast<std::size_t>(out.width) * out.height, kPanel);
    Canvas c{out};
    if (gfx) settings_s_ = s, settings_w_ = out.width, settings_h_ = out.height;
    c.tape(0, 0, out.width, tape_h * s, 4 * s);
    const int frame = std::max(1, s / 2);
    c.fill(0, out.height - frame, out.width, frame, kFrame);
    c.fill(0, 0, frame, out.height, kFrame);
    c.fill(out.width - frame, 0, frame, out.height, kFrame);
    int y = (tape_h + pad) * s;
    c.fill((pad + bar_pad) * s, y, 2 * s, 8 * s, kAlarm);  // a red mark before the title, like the warning signs
    c.text((pad + bar_pad + 4) * s, y, s, kTitleText, kTitle);
    y += 8 * s + 6 * s;
    if (gfx) {
        int xs[kTabs + 1];
        tabs_row(c, (pad + bar_pad - 2) * s, y, s, tab_, xs);
        for (int i = 0; i <= kTabs; ++i) tab_x_[i] = out.x + xs[i];
        tab_y_ = out.y + y;
        tab_h_ = 12 * s;
        y += 16 * s;
    } else {
        tab_h_ = 0;
    }
    c.fill(pad * s, y, out.width - 2 * pad * s, std::max(1, s / 2), kFrame);
    y += 6 * s;
    entry_x_ = out.x + pad * s;
    entry_y_ = out.y + y;
    entry_w_ = out.width - 2 * pad * s;
    entry_h_ = bar_h * s;
    arrow_lo_.assign(static_cast<std::size_t>(n), -1), arrow_mid_.assign(static_cast<std::size_t>(n), -1), arrow_top_ = 0;
    for (int i = 0; i < n; ++i) {
        if (const std::size_t lo = lines[i].find("< "), hi = lines[i].rfind(" >"); lo != std::string::npos && hi != std::string::npos && hi > lo) {
            const int x0 = out.x + (pad + bar_pad) * s;
            arrow_lo_[static_cast<std::size_t>(i)] = x0 + shown_text_width(s, lines[i].substr(0, lo));
            arrow_mid_[static_cast<std::size_t>(i)] = x0 + (shown_text_width(s, lines[i].substr(0, lo)) + shown_text_width(s, lines[i].substr(0, hi + 2))) / 2;
        }
        const bool lit = i == selected_;
        const bool quit = commands[i] == Command::quit;
        if (lit) c.fill(pad * s, y, out.width - 2 * pad * s, bar_h * s, quit ? kAlarm : kBar);
        const bool unusable = (commands[i] == Command::widescreen && values.widescreen < 0) || (commands[i] == Command::language && values.language_other.empty()) ||
                              (commands[i] == Command::game_blur && values.game_blur < 0) || (commands[i] == Command::key_prompts && values.key_prompts < 0) ||
                              (commands[i] == Command::mouse_look && values.mouse_look < 0);
        c.text((pad + bar_pad) * s, y + 3 * s, s, lines[i], lit ? (quit ? kText : kBarText) : unusable ? kDim : kText);
        const std::string key = keys[i];
        c.text(out.width - (pad + bar_pad) * s - static_cast<int>(key.size()) * 6 * s + s, y + 3 * s, s, key, lit ? (quit ? kText : kBarDim) : kDim);
        y += bar_h * s;
    }
    y += (rows - n) * bar_h * s;   // Settings: the room of a longer tab
    y += 5 * s;
    c.fill(pad * s, y, out.width - 2 * pad * s, std::max(1, s / 2), kFrame);
    y += 6 * s;
    c.text((pad + bar_pad) * s, y, s, kHint, kDim);
    y += 12 * s;
    c.text((pad + bar_pad) * s, y, s, kNote, gfx ? kTitle : kDim);
}

void Menu::draw_toast(int window_width, int window_height, const std::string &header, const std::string &title,
                      const std::vector<std::uint32_t> &icon, int icon_width, int icon_height, Picture &out) {
    // font pixels: tape strip, then the picture (24 x 24) at the left and the two lines at its right
    const int pad = 5, tape_h = 3, pic = 24, gap = 6;
    const bool has_icon = !icon.empty() && icon_width > 0 && icon_height > 0;
    const int text_chars = static_cast<int>(std::max(header.size(), title.size()));
    const int units_w = 2 * pad + (has_icon ? pic + gap : 0) + text_chars * 6, units_h = tape_h + 2 * pad + std::max(pic, 20);
    const int s = std::clamp(std::min(popup_scale(std::max(1, window_height / 200)), window_width * 9 / 10 / units_w), 1, 12);
    out.width = units_w * s;
    out.height = units_h * s;
    out.x = std::max(0, window_width - out.width - window_width / 40);   // bottom right, like a console's trophy pop-up
    out.y = std::max(0, window_height - out.height - window_height / 20);
    out.rgba.assign(static_cast<std::size_t>(out.width) * out.height, kPanel);
    Canvas c{out};
    c.tape(0, 0, out.width, tape_h * s, 3 * s);
    const int frame = std::max(1, s / 2);
    c.fill(0, out.height - frame, out.width, frame, kFrame);
    c.fill(0, 0, frame, out.height, kFrame);
    c.fill(out.width - frame, 0, frame, out.height, kFrame);
    int x = pad * s;
    const int top = (tape_h + pad) * s;
    if (has_icon) {
        // the picture, scaled to pic x pic font pixels (nearest), with a yellow border
        const int size = pic * s;
        c.fill(x - frame, top - frame, size + 2 * frame, size + 2 * frame, kTitle);
        for (int j = 0; j < size; ++j)
            for (int i = 0; i < size; ++i) {
                const std::uint32_t px = icon[static_cast<std::size_t>(j * icon_height / size) * icon_width + i * icon_width / size];
                const int a = static_cast<int>(px >> 24);
                if (a == 0) continue;
                out.rgba[static_cast<std::size_t>(top + j) * out.width + x + i] = px | 0xFF000000u;
            }
        x += (pic + gap) * s;
    }
    const int ty = top + std::max(0, (std::max(pic, 20) - 20) / 2) * s;
    c.text(x, ty, s, header, kTitle);
    c.text(x, ty + 11 * s, s, title, kText);
}

void Menu::draw_keys_on_screen(int window_width, int window_height, const std::vector<std::pair<std::string, std::string>> &rows,
                               const std::string &note, Picture &out) {
    // font pixels: a title, then one line per key (the name, then the keys), half see-through so the game shows through
    const int pad = 4, line_h = 10, name_chars = 13;
    std::size_t key_chars = 4;
    for (const auto &r : rows) key_chars = std::max(key_chars, r.second.size());
    key_chars = std::min<std::size_t>(key_chars, 28);
    const int title_h = 12, note_h = note.empty() ? 0 : 12;
    const int units_w = 2 * pad + (name_chars + 1 + static_cast<int>(key_chars)) * 6;
    const int units_h = 2 * pad + title_h + static_cast<int>(rows.size()) * line_h + note_h;
    const int s = std::clamp(std::min({popup_scale(std::max(1, window_height / 300)), window_height * 9 / 10 / units_h, window_width * 2 / 5 / units_w}), 1, 12);
    out.width = units_w * s;
    out.height = units_h * s;
    out.x = window_width / 60;
    out.y = std::max(0, (window_height - out.height) / 2);
    out.rgba.assign(static_cast<std::size_t>(out.width) * out.height, (kPanel & 0x00FFFFFFu) | 0xD2000000u);
    Canvas c{out};
    const int frame = std::max(1, s / 2);
    c.fill(0, 0, out.width, frame, kFrame);
    c.fill(0, out.height - frame, out.width, frame, kFrame);
    int y = pad * s;
    const int x = pad * s;
    c.text(x, y, s, "KEYS   (F10 hides)", kTitle);
    y += title_h * s;
    for (const auto &r : rows) {
        c.text(x, y, s, r.first.substr(0, name_chars), kDim);
        const std::string keys = r.second.size() > key_chars ? r.second.substr(0, key_chars - 3) + "..." : r.second;
        c.text(x + (name_chars + 1) * 6 * s, y, s, keys, kText);
        y += line_h * s;
    }
    if (!note.empty()) c.text(x, y + 2 * s, s, note.substr(0, static_cast<std::size_t>(name_chars + 1 + key_chars)), kDim);
}

void Menu::draw_note(int window_width, int window_height, const std::string &text, Picture &out) {
    const int pad = 5;
    const int units_w = 2 * pad + static_cast<int>(text.size()) * 6, units_h = 2 * pad + 8;
    const int s = std::clamp(std::min(popup_scale(std::max(1, window_height / 240)), window_width * 9 / 10 / units_w), 1, 12);
    out.width = units_w * s;
    out.height = units_h * s;
    out.x = std::max(0, (window_width - out.width) / 2);
    out.y = std::max(0, window_height / 20);
    out.rgba.assign(static_cast<std::size_t>(out.width) * out.height, kPanel);
    Canvas c{out};
    const int frame = std::max(1, s / 2);
    c.fill(0, 0, out.width, frame, kFrame);
    c.fill(0, out.height - frame, out.width, frame, kFrame);
    c.fill(0, 0, frame, out.height, kFrame);
    c.fill(out.width - frame, 0, frame, out.height, kFrame);
    c.text(pad * s, pad * s, s, text, kTitle);
}

void Menu::draw_native_label(int x, int y, int w, int h, bool selected, const std::string &text, Picture &out) {
    out.x = x;
    out.y = y;
    out.width = std::max(1, w);
    out.height = std::max(1, h);
    out.rgba.assign(static_cast<std::size_t>(out.width) * out.height, 0);
    // the line's own fill: a navy that darkens downwards, or the flat steel blue of the chosen line (measured on the screen)
    for (int j = 0; j < out.height; ++j) {
        const float f = out.height > 1 ? static_cast<float>(j) / (out.height - 1) : 0.f;
        const std::uint32_t c = selected ? rgba(54, 102, 134, 255) : rgba(static_cast<int>(33 - 18 * f), static_cast<int>(63 - 28 * f), static_cast<int>(83 - 35 * f), 255);
        for (int i = 0; i < out.width; ++i) out.rgba[static_cast<std::size_t>(j) * out.width + i] = c;
    }
    // the words in the game's font, the height of the game's own (its capitals about 55% of the line)
    const int s = std::max(1, static_cast<int>(std::lround(out.height * 0.55 / 7.0 / 0.47 * 0.47)));
    if (g_font.ok) game_text(out, 0, std::max(0, (out.height - static_cast<int>(19 * game_font_k(s))) / 2 + static_cast<int>(game_font_k(s))), s, text, rgba(236, 242, 250, 255));
}

// rawdanger-recomp: over the game's Options screen, at the bottom left: which button opens the PC settings, in the game's look
void Menu::draw_native_hint(int window_width, int window_height, const std::string &text, Picture &out) {
    const int was = g_skin;
    set_skin(2);
    const int pad = 4;
    const int s = std::clamp(window_height / 300, 1, 6);
    const int text_w = g_font.ok ? game_text_width(s, text) : static_cast<int>(text.size()) * 6 * s;
    out.width = text_w + 2 * pad * s + 12 * s;
    out.height = (2 * pad + 8) * s;
    out.x = std::max(0, window_width - out.width - window_width / 40);   // bottom right: the game's own hint line is in the middle
    out.y = std::max(0, window_height - out.height - window_height / 30);
    out.rgba.assign(static_cast<std::size_t>(out.width) * out.height, kPanel);
    Canvas c{out};
    const int line = std::max(1, s / 2);
    c.box(0, 0, out.width, out.height, line, kFrame);
    // the game's mark before a hint: a small light square
    c.fill(pad * s, (pad + 2) * s, 4 * s, 4 * s, kAlarm);
    c.text(pad * s + 8 * s, pad * s, s, text, kTitle);
    set_skin(was);
}

}  // namespace quickmenu
