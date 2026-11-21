// rawdanger-recomp: rain on the lens while playing (keys.ini rain_on_lens = on / off, on by default; F1 -> Settings -> Picture, and
// the game's own Options list). How hard is not a setting: it follows the map's rain (light, steady, heavy), eased in and out over
// a few seconds, and gusts: a slow random swell that makes the drops come in bursts and the mist come and go.
// Drops land at random places, cling a moment, then run down, leaving a thin wet trail that dries, like rain on a camera that is
// out in it; heavy rain also mists the lens at its edges. They are drawn over the picture by blit.wgsl (drops_over). Written for
// this project; the idea is the one of rain-drop mods for PC games, no code is taken from them.
//
// What the game says decides when (both set by RawDangerPC.exe, both optional):
//   PS2_LENS_WHEN="ADDRESS=VALUE"  a 32-bit word that holds VALUE while the player is playing (Raw Danger's scene 0x48F488=21).
//   PS2_LENS_WEATHER="ADDRESS"     the map's weather, a 32-bit word: 0 dry (indoors, the underground, a clear night), 1 to 3 rain
//                                  (light, steady, heavy), 4 to 6 snow. Raw Danger keeps it at 0x48F310 (gp - 0x4B60), set with
//                                  every map by the function at 0x100BF0, which prints "MAP Wether = %d"; the code that reads
//                                  it (0x14A784) treats 1-3 and 4-6 as two families of three strengths. Rain lands drops in
//                                  proportion; snow and dry maps land none.
// Off, or when the game stops playing (a menu, a film) or the map is dry, the drops on the lens run off within a second or two.
#include "lens_rain.hpp"

#include "ps2/memory.hpp"
#include "ps2/video.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

namespace lens_rain {
namespace {

constexpr std::size_t kMost = 64;

struct Drop {
    float x, y, r;      // centre (0 ... 1 of the picture), radius (a share of its height)
    float age, life;    // seconds
    float hold;         // seconds it clings before it starts to run
    float speed;        // picture heights per second once it runs
    float alpha;
    float trail;        // length of the wet trail above it (picture heights)
    float stretch;      // 1 round, more: pulled long while it runs
    float slant = 0.0f; // its path across while it runs (picture widths per height): the trail lies that way
    float seed;         // per-drop variety for the shader (0 ... 1)
    float wobble;       // the sideways drift it has picked
};

int g_level = 0;
std::vector<Drop> g_drops;
std::mt19937 g_random{20061108u};
bool g_when_set = false, g_weather_set = false;
std::uint32_t g_when_address = 0, g_when_value = 0, g_weather_address = 0;
bool g_parsed = false;
float g_mist = 0.0f;                  // how misted the lens is (0 ... 1), follows the rain slowly
float g_clock = 0.0f;
std::uint32_t g_last_weather = 0xFFFFFFFFu;

std::uint32_t word(std::uint32_t address) {
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(ps2::ld8(address + static_cast<std::uint32_t>(i))) << (8 * i);
    return v;
}

void parse_env() {
    g_parsed = true;
    if (const char *w = std::getenv("PS2_LENS_WHEN"); w && *w) {
        const std::string s(w);
        const std::size_t eq = s.find('=');
        if (eq != std::string::npos) {
            g_when_address = static_cast<std::uint32_t>(std::strtoul(s.substr(0, eq).c_str(), nullptr, 0)) & 0x01FFFFFCu;
            g_when_value = static_cast<std::uint32_t>(std::strtoul(s.substr(eq + 1).c_str(), nullptr, 0));
            g_when_set = true;
        }
    }
    if (const char *w = std::getenv("PS2_LENS_WEATHER"); w && *w) {
        g_weather_address = static_cast<std::uint32_t>(std::strtoul(w, nullptr, 0)) & 0x01FFFFFCu;
        g_weather_set = g_weather_address != 0;
    }
}

bool playing() {
    if (!g_parsed) parse_env();
    return !g_when_set || word(g_when_address) == g_when_value;
}

// how hard it rains on the lens, 0 ... 1 (1 without a weather address: the old "always" behaviour)
float rain() {
    if (!g_weather_set) return 1.0f;
    const std::uint32_t w = word(g_weather_address);
    if (w != g_last_weather) {
        g_last_weather = w;
        std::fprintf(stderr, "[lens] map weather %u: %s\n", w, w == 0 ? "dry" : w <= 3 ? "rain" : w <= 6 ? "snow" : "unknown");
    }
    switch (w) {
    case 1: return 0.4f;
    case 2: return 0.7f;
    case 3: return 1.0f;
    default: return 0.0f;   // dry, snow, or a value this does not know
    }
}

float uniform(float a, float b) { return std::uniform_real_distribution<float>(a, b)(g_random); }

void spawn(float strength) {
    Drop d;
    d.x = uniform(0.02f, 0.98f);
    d.y = uniform(-0.02f, 0.88f);
    const float size = uniform(0.0f, 1.0f);
    d.r = 0.004f + size * size * size * 0.022f * (0.7f + 0.3f * strength);   // mostly small beads, now and then a bigger one
    d.age = 0.0f;
    d.life = uniform(3.0f, 8.0f);
    d.hold = uniform(0.4f, 3.0f) * (1.2f - 0.4f * strength);                 // in heavy rain they let go sooner
    d.speed = 0.04f + d.r * 7.0f;                                            // big drops run faster
    d.alpha = 0.0f;
    d.trail = 0.0f;
    d.stretch = 1.0f;
    d.seed = uniform(0.0f, 1.0f);
    d.wobble = uniform(-0.012f, 0.012f);
    g_drops.push_back(d);
}

}  // namespace

void set_level(int level) {
    g_level = level ? 1 : 0;
    ps2::set_ui_boxes(g_level > 0);   // the drops stay under the game's HUD and texts
}
int level() { return g_level; }
bool knows_when() {
    if (!g_parsed) parse_env();
    return g_when_set;
}

// rawdanger-recomp: where the drops run. Water on a lens runs down, and sideways when the camera turns (the drops lag behind the
// turn) or the wind of the run pushes them: the camera's angle (PS2_LENS_YAW, radians, Raw Danger 0x5FDBAC) is read each frame
// and its turning, smoothed, slants the running drops and their trails; a fast turn also shakes the still ones loose.
std::uint32_t g_yaw_address = 0;
bool g_yaw_set = false, g_yaw_have = false;
float g_yaw_prev = 0.0f, g_drift = 0.0f;

// rawdanger-recomp: the player's own movement, seen from the camera: walking or running to the left pushes the water on the lens to
// the right (the air goes past the other way), and running at the camera's view drives the drops down faster.
// PS2_LENS_MOTION="POINTER,PLAYER,VIEW": a word holding the camera block's address, then the offsets in it of the player's position
// (x y z floats) and of the view's direction (Raw Danger: 0x48F7D8, 0x310, 0x1B0). Screen-right is (view z, 0, -view x).
std::uint32_t g_motion_pointer = 0, g_motion_player = 0, g_motion_view = 0;
bool g_motion_set = false, g_motion_have = false, g_motion_absolute = false;
float g_motion_prev[3] = {}, g_forward = 0.0f, g_view_yaw = 0.0f, g_view_turn = 0.0f, g_look_up = 0.0f;
bool g_view_have = false;

float read_float(std::uint32_t address) {
    const std::uint32_t w = ps2::ld32(address);
    float f;
    std::memcpy(&f, &w, 4);
    return f;
}

float motion_drift(float seconds) {
    static bool parsed = false;
    if (!parsed) {
        parsed = true;
        if (const char *m = std::getenv("PS2_LENS_MOTION"); m && *m) {
            char *end = nullptr;
            g_motion_absolute = *m == '@';   // "@ADDRESS": the camera block itself, not a word pointing at it
            g_motion_pointer = static_cast<std::uint32_t>(std::strtoul(m + (g_motion_absolute ? 1 : 0), &end, 16));
            if (end && *end == ',') g_motion_player = static_cast<std::uint32_t>(std::strtoul(end + 1, &end, 16));
            if (end && *end == ',') g_motion_view = static_cast<std::uint32_t>(std::strtoul(end + 1, &end, 16));
            g_motion_set = g_motion_pointer && g_motion_player && g_motion_view;
        }
    }
    float side = 0.0f, forward = 0.0f;
    const std::uint32_t block = g_motion_set && playing() && seconds > 0.0f ? (g_motion_absolute ? g_motion_pointer : ps2::ld32(g_motion_pointer)) : 0u;
    if (block >= 0x100000u && block < 0x2000000u) {
        float p[3], v[3];
        for (int i = 0; i < 3; ++i) p[i] = read_float(block + g_motion_player + 4u * i), v[i] = read_float(block + g_motion_view + 4u * i);
        const bool fine = std::isfinite(p[0]) && std::isfinite(p[2]) && std::isfinite(v[0]) && std::isfinite(v[2]);
        if (fine && g_motion_have) {
            const float dx = p[0] - g_motion_prev[0], dz = p[2] - g_motion_prev[2];
            const float len = std::max(1e-4f, std::sqrt(v[0] * v[0] + v[2] * v[2]));
            if (dx * dx + dz * dz < 4.0f) {   // not a jump (a door, a loaded save)
                side = (dx * v[2] - dz * v[0]) / len / seconds;
                forward = (dx * v[0] + dz * v[2]) / len / seconds;
            }
        }
        for (int i = 0; i < 3; ++i) g_motion_prev[i] = p[i];
        g_motion_have = fine;
        // the camera's turn from its view (every camera has one; the angle word of PS2_LENS_YAW is only the follow camera's)
        if (fine) {
            // looking up into the rain: the game's y points down (a level camera reads about +0.07), so up is -y
            // (the follow camera tilts up about 25 degrees at most, -y 0.4: that is the full effect)
            const float up = std::clamp(2.5f * -v[1] / std::max(1e-3f, std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2])), 0.0f, 1.0f);
            g_look_up += (up - g_look_up) * std::min(1.0f, seconds * 3.0f);
            const float yaw = std::atan2(v[0], v[2]);
            if (g_view_have) {
                float turn = yaw - g_view_yaw;
                if (turn > 3.14159265f) turn -= 6.2831853f;
                if (turn < -3.14159265f) turn += 6.2831853f;
                g_view_turn = turn / seconds;
            }
            g_view_yaw = yaw, g_view_have = true;
        }
    } else {
        g_motion_have = false, g_view_have = false, g_view_turn = 0.0f;
    }
    g_forward += (std::max(0.0f, forward) - g_forward) * std::min(1.0f, seconds * 3.0f);
    return std::clamp(-side * 0.14f, -0.8f, 0.8f);   // running sideways (about 4.5 a second) slants them about 3 in 5
}

void read_drift(float seconds) {
    static bool parsed = false;
    if (!parsed) {
        parsed = true;
        if (const char *y = std::getenv("PS2_LENS_YAW"); y && *y) {
            g_yaw_address = static_cast<std::uint32_t>(std::strtoul(y, nullptr, 16));
            g_yaw_set = true;
        }
    }
    float goal = 0.0f;
    if (g_yaw_set && seconds > 0.0f && playing()) {
        const std::uint32_t w = ps2::ld32(g_yaw_address);
        float yaw;
        std::memcpy(&yaw, &w, 4);
        if (std::isfinite(yaw)) {
            if (g_yaw_have) {
                float turn = yaw - g_yaw_prev;
                if (turn > 3.14159265f) turn -= 6.2831853f;
                if (turn < -3.14159265f) turn += 6.2831853f;
                goal = std::clamp(turn / seconds * 0.22f, -0.9f, 0.9f);   // a turn of 1 rad/s slants them about 1 in 5
            }
            g_yaw_prev = yaw;
            g_yaw_have = true;
        }
    } else {
        g_yaw_have = false;
    }
    // the camera turning (from its view: the angle word above misses some cameras): a turn to the right pushes the water left
    const float side = motion_drift(seconds);
    const float view_goal = std::clamp(-g_view_turn * 0.3f, -0.9f, 0.9f);
    if (std::fabs(view_goal) > std::fabs(goal)) goal = view_goal;
    goal += side;
    g_drift += (std::clamp(goal, -1.1f, 1.1f) - g_drift) * std::min(1.0f, seconds * 4.0f);
}

// the gust: a slow random swell around 1 (0.35 ... 1.6), a new goal every few seconds, followed smoothly
float g_gust = 1.0f, g_gust_goal = 1.0f, g_gust_next = 0.0f;
float g_strength = 0.0f;   // the map's rain, eased (0 ... 1)

void frame(float seconds) {
    g_clock += seconds;
    read_drift(seconds);
    const float target = g_level > 0 && playing() ? rain() : 0.0f;
    // in over about 4 s when it starts raining (or the player walks out), out over about 1.5 s
    g_strength += (target - g_strength) * std::min(1.0f, seconds * (target > g_strength ? 0.35f : 1.2f));
    if (target == 0.0f && g_strength < 0.02f) g_strength = 0.0f;
    if (g_clock >= g_gust_next) {
        const float r = uniform(0.0f, 1.0f);
        g_gust_goal = r < 0.15f ? uniform(1.3f, 1.6f) : r < 0.35f ? uniform(0.35f, 0.6f) : uniform(0.75f, 1.15f);   // now and then a burst or a lull
        g_gust_next = g_clock + uniform(2.0f, 6.0f);
    }
    g_gust += (g_gust_goal - g_gust) * std::min(1.0f, seconds * 0.8f);
    const float strength = g_strength;
    const bool on = target > 0.0f && strength > 0.01f;
    // new drops: about 9 a second at full rain in an ordinary gust, more in a burst; the lens holds more the harder it rains
    if (on) {
        const float rate = 9.0f * strength * g_gust * (1.0f + std::min(g_forward, 5.0f) * 0.12f)   // running into the rain: more of it
                           * (1.0f + 3.0f * g_look_up * g_look_up);                                 // looking up into it: much more
        const std::size_t most = std::min<std::size_t>(kMost, static_cast<std::size_t>((16.0f + 48.0f * strength * std::min(g_gust, 1.0f)) * (1.0f + g_look_up)));
        float expect = rate * seconds;
        while (expect > 0.0f && g_drops.size() < most) {
            if (uniform(0.0f, 1.0f) < expect) spawn(std::min(1.0f, strength * g_gust));
            expect -= 1.0f;
        }
    }
    // the mist follows the rain: up in a few seconds of heavy rain, gone within a few seconds out of it
    const float mist_goal = on ? std::min(0.85f, strength * 0.6f * (0.6f + 0.4f * g_gust)) : 0.0f;
    g_mist += (mist_goal - g_mist) * std::min(1.0f, seconds * (mist_goal > g_mist ? 0.25f : 0.6f));
    for (Drop &d : g_drops) {
        d.age += seconds;
        if (!on) d.life = std::min(d.life, d.age + 1.2f);   // nothing more falls: the lens clears
        if (d.age > d.hold) {
            const float step = d.speed * seconds * (1.0f + std::min(g_forward, 5.0f) * 0.1f + std::fabs(g_drift) * 0.6f);
            d.y += step;
            d.x += d.wobble * seconds + uniform(-0.0015f, 0.0015f) + g_drift * step;   // a little crookedly, and sideways in a turn
            d.slant += (g_drift - d.slant) * std::min(1.0f, seconds * 3.0f);
            d.trail = std::min(std::min(0.12f, d.r * 7.0f), d.trail + step);   // v0.8.3: short wet marks, not lines down the picture
            d.r = std::max(0.005f, d.r - 0.003f * seconds);           // leaves water behind and gets smaller
            d.stretch = std::min(1.6f, d.stretch + seconds * 2.0f);
        } else {
            d.trail = std::max(0.0f, d.trail - seconds * 0.05f);
            if (std::fabs(g_drift) > 0.3f && d.r > 0.008f) d.hold = std::min(d.hold, d.age + 0.1f);   // a turn or a run shakes it loose
            d.x += g_drift * seconds * 0.05f;   // and a clinging one creeps the way the air pushes it
        }
        const float in = std::min(1.0f, d.age / 0.15f), out = std::clamp((d.life - d.age) / 1.0f, 0.0f, 1.0f);
        d.alpha = 0.9f * in * out;
    }
    // merging: a running drop swallows still drops it reaches (the bigger result runs on)
    for (Drop &d : g_drops) {
        if (d.age <= d.hold || d.alpha <= 0.0f) continue;
        for (Drop &o : g_drops) {
            if (&o == &d || o.alpha <= 0.0f || o.age > o.hold) continue;
            const float dx = (o.x - d.x) * 1.7f, dy = o.y - d.y;
            if (dx * dx + dy * dy < (d.r + o.r) * (d.r + o.r)) {
                d.r = std::min(0.045f, std::sqrt(d.r * d.r + o.r * o.r));
                d.speed = 0.04f + d.r * 7.0f;
                o.alpha = 0.0f;
                o.life = o.age;
            }
        }
    }
    g_drops.erase(std::remove_if(g_drops.begin(), g_drops.end(), [](const Drop &d) { return d.age >= d.life || d.y - d.r > 1.05f; }), g_drops.end());
    float data[kMost * 8];
    unsigned n = 0;
    for (const Drop &d : g_drops) {
        if (n == kMost) break;
        float *p = data + n * 8;
        p[0] = d.x;
        p[1] = d.y;
        p[2] = d.r;
        p[3] = d.alpha;
        p[4] = d.trail;
        p[5] = d.stretch;
        p[6] = d.seed;
        p[7] = d.slant;
        ++n;
    }
    ps2::set_lens_drops(data, n, g_mist, g_clock);
}

}  // namespace lens_rain
