#include "window.hpp"
#include "big_stack_thread.hpp"
#include "stick_map.hpp"

#include "recomp_options.hpp"
#include "achievements.hpp"
#include "perf_log.hpp"
#include "lens_rain.hpp"

#include "ps2/system.hpp"

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#include <shellapi.h>
#endif
#include <SDL.h>
#include <SDL_syswm.h>
#ifdef __APPLE__
#include <SDL_metal.h>
#include <mach/mach.h>
#endif
#ifndef _WIN32
#include <sys/resource.h>
#include <unistd.h>
#endif
#ifdef __linux__
#include <csignal>
#include <cerrno>
#include <cstring>
#include <sys/prctl.h>
#include <sys/syscall.h>
#ifdef __GLIBC__
#include <execinfo.h>
#endif
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <functional>
#include <array>
#include <map>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "keymap.hpp"
#include "quickmenu.hpp"
#include "ps2/crash.hpp"
#include "ps2/cpu.hpp"
#include "ps2/generated.hpp"
#include "ps2/memory.hpp"
#include "ps2/hitch.hpp"
#include "ps2/os.hpp"
#include "ps2/video.hpp"
#include "stretch.hpp"
#ifdef PS2_GPU
#include "../runtime/src/gs/gpu/overlay.hpp"  // the quick menu's panel in the WebGPU present pass
#endif

void fit_window_to_shape();  // rawdanger-recomp (defined with open_window)

namespace {

enum Button : std::uint16_t {
    kSelect = 1u << 0, kL3 = 1u << 1, kR3 = 1u << 2, kStart = 1u << 3,
    kUp = 1u << 4, kRight = 1u << 5, kDown = 1u << 6, kLeft = 1u << 7,
    kL2 = 1u << 8, kR2 = 1u << 9, kL1 = 1u << 10, kR1 = 1u << 11,
    kTriangle = 1u << 12, kCircle = 1u << 13, kCross = 1u << 14, kSquare = 1u << 15,
};

struct PadBinding {
    SDL_GameControllerButton button;
    std::uint16_t mask;
};

constexpr PadBinding kPadButtons[] = {
    {SDL_CONTROLLER_BUTTON_A, kCross},          {SDL_CONTROLLER_BUTTON_B, kCircle},
    {SDL_CONTROLLER_BUTTON_X, kSquare},         {SDL_CONTROLLER_BUTTON_Y, kTriangle},
    {SDL_CONTROLLER_BUTTON_LEFTSHOULDER, kL1},  {SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, kR1},
    {SDL_CONTROLLER_BUTTON_LEFTSTICK, kL3},     {SDL_CONTROLLER_BUTTON_RIGHTSTICK, kR3},
    {SDL_CONTROLLER_BUTTON_BACK, kSelect},      {SDL_CONTROLLER_BUTTON_START, kStart},
    {SDL_CONTROLLER_BUTTON_DPAD_UP, kUp},       {SDL_CONTROLLER_BUTTON_DPAD_DOWN, kDown},
    {SDL_CONTROLLER_BUTTON_DPAD_LEFT, kLeft},   {SDL_CONTROLLER_BUTTON_DPAD_RIGHT, kRight},
};

SDL_Window *g_window = nullptr;
#ifdef __APPLE__
SDL_MetalView g_metal_view = nullptr;
#endif
SDL_Renderer *g_renderer = nullptr;
SDL_Texture *g_texture = nullptr;
SDL_AudioDeviceID g_audio = 0;
ps2sound::SoundPipe *g_pipe = nullptr;  // the game's sound: pushed by the emulation thread, played (and stretched) by the device thread, host/stretch.hpp
// the keyboard drives the first controller port along with the first game controller, a second one is player 2
SDL_GameController *g_pads[2] = {};
// PS2_PAD_LOG=1 prints every change of what the two controller ports send to the game, and every rumble request
bool g_pad_log = false;
std::uint32_t g_tex_w = 0, g_tex_h = 0;
std::uint64_t g_last = 0;
std::vector<std::uint32_t> g_pixels;
bool g_gpu_present = false;

// Where the window thread is in its own loop ("asking the system for events", "drawing") and since when. Always on: two stores per
// step. The Linux watchdog prints it when no frame comes, so a window thread that is stuck in one call names the call (docs/linux.md,
// "The window freeze").
std::atomic<const char *> g_window_stage{nullptr};
std::atomic<std::int64_t> g_window_stage_since_ns{0};
void window_stage(const char *name) {
    g_window_stage_since_ns.store(ps2::hitch::now_ns(), std::memory_order_relaxed);
    g_window_stage.store(name, std::memory_order_release);
}

// --- keyboard and mouse (host/keymap.cpp, docs/keys.md) ---
//
// Which key works which button comes from keys.ini (written with the defaults on the first start, read again when the window
// gets the focus back after the file changed). The arrow keys are the D-pad only: they used to push the left stick as well, and
// a menu that reads both moved two entries for one press.
keymap::Config g_keys = keymap::defaults();
std::atomic<bool> g_fast_forward{false};   // rawdanger-recomp: F6 held
// rawdanger-recomp: the Japanese pictures inside maps and models (texture overrides under jp/) follow the Enhanced Edition's
// "Textures: Japanese faces" and "... signs" switches, at once
void sync_jp_textures() {
    bool on = false;
    for (const recomp::Section &sec : recomp::sections())
        if (sec.applied_on() && sec.name.rfind("Textures: Japanese", 0) == 0) on = true;
    if (const char *f = std::getenv("PS2_JP_TEXTURES"); f && *f) on = *f == '1';   // forced (the US build has no such switch)
    ps2::set_jp_textures(on);
}
// rawdanger-recomp: the game's own Options screen (PS2_NATIVE_MENU="ADDRESS=VALUE", Raw Danger's scene word 0x48F488 = 16):
// while it is on, a sign at the bottom left says Select opens the PC settings, drawn in the game's look
std::uint32_t g_native_address = 0, g_native_value = 0;
bool g_native_set = false, g_native_hint = false;
int g_skin_chosen = 0;
void open_menu(const char *why);
void menu_paint(bool show_again = true);   // the menu's look chosen in keys.ini (the native page borrows the game's look while it is open)
// rawdanger-recomp: the PC settings as a line of the game's own Options list. PS2_NATIVE_ROW="POINTER+OFFSET=ROW;X0,Y0,X1,Y1;
// HELP_ADDRESS": while the Options screen is on (PS2_NATIVE_MENU), the game's cursor is the word at [POINTER] + OFFSET; line ROW
// (Raw Danger: "Adjust screen position", which means nothing on a PC) gets the words "PC settings" over its own (X0..Y1: where its
// words are, in the console's 640 x 448 picture), the game's help line under the list says what it does (the game's own text,
// at HELP_ADDRESS, written in its place), and Cross on it opens the PC settings in the screen's look instead of the game's
// screen-position page. Up, Down and the rest stay the game's.
bool native_scene();
struct NativeRow {
    bool set = false;
    std::uint32_t pointer = 0, offset = 0, row = 0, help = 0, shown = 0;   // shown: [POINTER] + this is 1 while the list is up
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
} g_native_row;
bool g_native_label = false, g_native_label_selected = false;
const char kNativeHelpWas[] = "Adjust Screen Position";
const char kNativeHelp[] = "Picture, sound, keys";   // no longer than the game's own words

void parse_native_row() {
    static bool parsed = false;
    if (parsed) return;
    parsed = true;
    const char *v = std::getenv("PS2_NATIVE_ROW");
    if (!v || !*v) return;
    unsigned long p = 0, o = 0, r = 0, h = 0, sh = 0;
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    if (std::sscanf(v, "%lx+%lx=%lu;%d,%d,%d,%d;%lx;%lx", &p, &o, &r, &x0, &y0, &x1, &y1, &h, &sh) >= 7) {
        g_native_row = {true, static_cast<std::uint32_t>(p), static_cast<std::uint32_t>(o), static_cast<std::uint32_t>(r), static_cast<std::uint32_t>(h), static_cast<std::uint32_t>(sh), x0, y0, x1, y1};
        std::fprintf(stderr, "[menu] the game's Options list: line %lu is the PC settings\n", r);
    }
}

int native_cursor() {
    parse_native_row();
    if (!g_native_row.set || !native_scene()) return -1;   // the Options screen only
    const std::uint32_t base = ps2::ld32(g_native_row.pointer);
    if (base < 0x100000 || base >= 0x2000000) return -1;
    // the list's state sits at another place in the work memory from the title and from the in-game menu: it is found by the
    // pointer to the list's table (OFFSET is that table's address, 0x73C8A0 in OPTION.BIN), then the number of lines, then the
    // cursor; 24 bytes before the table pointer is 1 while the list (not one of its pages) is up
    for (std::uint32_t at = base; at < base + 0x1000; at += 4) {
        if (ps2::ld32(at) != g_native_row.offset || ps2::ld32(at + 4) != 7) continue;
        if (ps2::ld32(at - 24) != 1) return -1;
        return static_cast<int>(ps2::ld32(at + 8));
    }
    return -1;
}

// the game's help line for that line, in its own text (only where the game's words are still there, so nothing else is touched)
void native_help_text() {
    if (!g_native_row.help) return;
    bool same = true;
    for (std::size_t i = 0; i < sizeof kNativeHelpWas - 1 && same; ++i) same = ps2::ld8(g_native_row.help + static_cast<std::uint32_t>(i)) == static_cast<std::uint8_t>(kNativeHelpWas[i]);
    if (!same) return;
    for (std::size_t i = 0; i < sizeof kNativeHelpWas; ++i)
        ps2::st8(g_native_row.help + static_cast<std::uint32_t>(i), i < sizeof kNativeHelp - 1 && i < sizeof kNativeHelpWas - 1 ? static_cast<std::uint8_t>(kNativeHelp[i]) : 0);
}

bool native_scene() {
    static bool parsed = false;
    if (!parsed) {
        parsed = true;
        if (const char *w = std::getenv("PS2_NATIVE_MENU"); w && *w) {
            unsigned long a = 0, v = 0;
            if (std::sscanf(w, "%li=%li", reinterpret_cast<long *>(&a), reinterpret_cast<long *>(&v)) == 2) {
                g_native_address = static_cast<std::uint32_t>(a);
                g_native_value = static_cast<std::uint32_t>(v);
                g_native_set = true;
            }
        }
    }
    return g_native_set && ps2::ld32(g_native_address) == g_native_value;
}

// rawdanger-recomp: the game's pause box while playing (PS2_NATIVE_PAUSE="ADDRESS=VALUE": Raw Danger 0x48F4EC = 1): the same sign
// and Select open the PC settings from there too
bool native_pause() {
    static bool parsed = false, set = false;
    static std::uint32_t address = 0, value = 0;
    if (!parsed) {
        parsed = true;
        if (const char *w = std::getenv("PS2_NATIVE_PAUSE"); w && *w) {
            unsigned long a = 0, v = 0;
            if (std::sscanf(w, "%lx=%lu", &a, &v) == 2) address = static_cast<std::uint32_t>(a), value = static_cast<std::uint32_t>(v), set = true;
        }
    }
    return set && ps2::ld32(address) == value;
}
void send_key_prompts();  // rawdanger-recomp: the keys for the game's font (below)
// the picture options for the GPU: the look (bits 0-2) and whole-number scaling (bit 3, rawdanger-recomp)
std::uint32_t video_bits();
std::atomic<int> g_master_volume{100};  // rawdanger-recomp: keys.ini master_volume (%), read by the sound device's thread
std::vector<std::size_t> g_recomp_row_section;  // the Game options page's lines -> recomp::sections() (widescreen is on the Graphics page)
// rawdanger-recomp: the developers' menus left on the disc. PS2_DEBUG_SCENES="ADDRESS;DEBUG;MAPSEL": the game's next-scene word
// (Raw Danger: 0x48F48C, read by its scene switch at 0x16ED80; 0x48F488 is the scene it runs) and the numbers of its scenes
// DEBSEL (40, the debug menu) and MAP SEL (41); 0 leaves one out. On the retail program DEBSEL has three lines: MAP SELECT, SOUND
// TEST (its scene is -1: it does nothing) and EXIT (back to the logos); its text reads through PS2_SJIS_TEXT, and it takes Circle or
// Start (Cross too with the pnach section [Debug menu answers Cross]). The longer developers' menu (player move, flags, fog, info,
// item...) is only a table of values in the retail program (0x3BC810, 37 lines of 56 bytes): the code that drew it was taken out
// before release, and of its values only line 5 (R2 LAND PATH) is still read. The launcher offers both ("0x48F48C;40;41").
// Opened from Settings > Game (lines at the end), F7 (map select) or Shift+F7 (the debug menu).
// In the map select: up / down the line, left / right the value, Cross loads the map at that gate.
constexpr std::size_t kDebugRow = static_cast<std::size_t>(-2), kMapSelRow = static_cast<std::size_t>(-3), kInfoRow = static_cast<std::size_t>(-4),
                      kHealRow = static_cast<std::size_t>(-5), kKeepRow = static_cast<std::size_t>(-6),
                      // v0.9.5: the Debug tab's test switches
                      kDryRow = static_cast<std::size_t>(-7), kSoakRow = static_cast<std::size_t>(-8), kWetForceRow = static_cast<std::size_t>(-9),
                      kWetShowRow = static_cast<std::size_t>(-10), kHeadTestRow = static_cast<std::size_t>(-11),
                      kViewRow = static_cast<std::size_t>(-12), kStatsRow = static_cast<std::size_t>(-13), kAnimGainRow = static_cast<std::size_t>(-14),
                      kStepsRow = static_cast<std::size_t>(-15);
// rawdanger-recomp (v0.9.2): the hero's two bars. PS2_DEBUG_HERO="TABLE,PLAYER,COUNT,NOW1,MOST1,NOW2,MOST2" (hex: the table of the
// characters' addresses, the word with the player's number, how many the table has, and two pairs of offsets: a value and its
// most). Raw Danger: 0x5EAC80, 0x48F3E4, 235; health (+0x368 of +0x36C, 100) and body temperature (+0x370 of +0x374, 30000: it
// falls while wet and cold, the HUD's arrow turns blue). "Heal and warm" fills both once; "Keep healthy and warm" every frame.
struct DebugHero {
    bool set = false;
    std::uint32_t table = 0, player = 0, count = 0, now1 = 0, most1 = 0, now2 = 0, most2 = 0;
};
const DebugHero &debug_hero() {
    static const DebugHero d = [] {
        DebugHero r;
        const char *v = std::getenv("PS2_DEBUG_HERO");
        unsigned long t = 0, p = 0, n = 0, a = 0, b = 0, c = 0, e = 0;
        if (v && *v && std::sscanf(v, "%lx,%lx,%lx,%lx,%lx,%lx,%lx", &t, &p, &n, &a, &b, &c, &e) == 7)
            r = {true, static_cast<std::uint32_t>(t), static_cast<std::uint32_t>(p), static_cast<std::uint32_t>(n), static_cast<std::uint32_t>(a),
                 static_cast<std::uint32_t>(b), static_cast<std::uint32_t>(c), static_cast<std::uint32_t>(e)};
        return r;
    }();
    return d;
}
bool g_keep_hero = false;
// fills the player's two bars; false when there is no player
bool heal_hero() {
    const DebugHero &d = debug_hero();
    if (!d.set) return false;
    const std::uint32_t who = ps2::ld32(d.player);
    if (who >= d.count) return false;
    const std::uint32_t hero = ps2::ld32(d.table + 4u * who);
    if (hero < 0x100000u || hero >= 0x2000000u) return false;
    ps2::st32(hero + d.now1, ps2::ld32(hero + d.most1));
    ps2::st32(hero + d.now2, ps2::ld32(hero + d.most2));
    ps2::set_hero_wetness(0.0f);   // v0.9.5: and dry
    return true;
}
// rawdanger-recomp (v0.9.1): "Where am I" for reports: PS2_DEBUG_INFO="SCENE,CAMERA,MODE,PIVOT,PLAYER" (hex: the scene word, the
// word with the active camera block, the mode's and the follow point's offsets in it, the word with the player's character)
struct DebugInfo {
    bool set = false;
    std::uint32_t scene = 0, camera = 0, mode = 0, pivot = 0, player = 0;
};
const DebugInfo &debug_info() {
    static const DebugInfo d = [] {
        DebugInfo r;
        const char *v = std::getenv("PS2_DEBUG_INFO");
        unsigned long s = 0, c = 0, m = 0, p = 0, pl = 0;
        if (v && *v && std::sscanf(v, "%lx,%lx,%lx,%lx,%lx", &s, &c, &m, &p, &pl) == 5)
            r = {true, static_cast<std::uint32_t>(s), static_cast<std::uint32_t>(c), static_cast<std::uint32_t>(m), static_cast<std::uint32_t>(p), static_cast<std::uint32_t>(pl)};
        return r;
    }();
    return d;
}
std::string debug_info_text() {
    const DebugInfo &d = debug_info();
    const std::uint32_t cam = ps2::ld32(d.camera);
    char text[200];
    if (cam < 0x100000u || cam >= 0x2000000u) {
        std::snprintf(text, sizeof text, "Scene %u, no camera yet, frame %llu", ps2::ld32(d.scene), static_cast<unsigned long long>(ps2::frame_count()));
        return text;
    }
    float at[3];
    for (int k = 0; k < 3; ++k) { const std::uint32_t w = ps2::ld32(cam + d.pivot + 4u * k); std::memcpy(&at[k], &w, 4); }
    const float wet = ps2::hero_wetness();
    char wet_text[32] = "";
    if (wet >= 0.0f) std::snprintf(wet_text, sizeof wet_text, ", clothes %d %% wet", static_cast<int>(wet * 100.0f + 0.5f));
    std::snprintf(text, sizeof text, "Scene %u, camera mode %u, character %u, at %.1f %.1f %.1f%s, frame %llu", ps2::ld32(d.scene), ps2::ld32(cam + d.mode),
                  ps2::ld32(d.player), at[0], at[1], at[2], wet_text, static_cast<unsigned long long>(ps2::frame_count()));
    return text;
}
struct DebugScenes { std::uint32_t address = 0, debug = 0, mapsel = 0, from = 0; bool set = false; };
// the debug menu (DEBSEL) on screen, and the line its cursor is on: followed here from the pad (the game moves it one line per
// new press of up or down, from 0 to 2 without wrapping: 0x188088-0x1880C0), as its own cursor is in a block made at run time
std::atomic<bool> g_debug_menu_on{false};
std::atomic<int> g_debug_cursor{0};
const DebugScenes &debug_scenes() {
    static const DebugScenes d = [] {
        DebugScenes r;
        const char *v = std::getenv("PS2_DEBUG_SCENES");
        if (!v || !*v) return r;
        char *end = nullptr;
        r.address = static_cast<std::uint32_t>(std::strtoul(v, &end, 0));
        if (end && *end == ';') r.debug = static_cast<std::uint32_t>(std::strtoul(end + 1, &end, 0));
        if (end && *end == ';') r.mapsel = static_cast<std::uint32_t>(std::strtoul(end + 1, &end, 0));
        // a fourth number: the only scene they open from (Raw Danger: 21, the game itself; over the title the map select shows
        // but the title keeps the pad)
        if (end && *end == ';') r.from = static_cast<std::uint32_t>(std::strtoul(end + 1, &end, 0));
        r.set = r.address != 0 && (r.debug != 0 || r.mapsel != 0);
        return r;
    }();
    return d;
}
void open_debug_scene(bool map_select);  // below
bool in_skipped_scene();
void keep_scene_request();
void set_notice(const std::string &text);
std::string g_keys_path;
std::filesystem::file_time_type g_keys_time{};
int g_mouse_sum_x = 0, g_mouse_sum_y = 0;
// rawdanger-recomp: mouse look. The camera turns as far as the mouse moved: the right stick is pushed just enough, and the game's own
// camera angle is read back every frame so what it has turned already comes off what is still to turn (closed loop). The launcher
// names the words: PS2_MOUSE_LOOK_YAW="ADDRESS,RATE,DEADZONE" (an angle in radians that wraps; RATE: radians per game step for
// each unit of stick past DEADZONE, of 127), PS2_MOUSE_LOOK_PITCH="ADDRESS,RATE,DEADZONE,LOW,HIGH" (a value kept between LOW and
// HIGH), PS2_MOUSE_LOOK_WHEN="ADDRESS=VALUE" (only while that word holds VALUE: playing).
struct LookAxis {
    std::uint32_t address = 0;
    float rate = 0, dead = 0, lo = 0, hi = 0;
    bool angle = false, set = false, have_prev = false;
    float prev = 0;
    double pending = 0;   // still to turn, in the direction a push of the stick to 255 turns
    std::uint8_t held = 0x80;   // the push of the game's current step
    int sign = 1;         // +1: a push to 255 makes the word bigger; -1: smaller
};
LookAxis g_look_yaw, g_look_pitch;
// rawdanger-recomp: direct mouse look. PS2_MOUSE_LOOK_DIRECT="WORD,CHECK=VALUE": a code patch built into the game (the pnach section
// [Direct mouse look]) adds the float at WORD to the camera's turn of each step (radians, the stick's way round) and clears it;
// CHECK holding VALUE says the build has it. The mouse's turn is put there whenever the game has taken the last one, so the camera
// turns exactly as far as the mouse moved, at any speed. A camera that does not read the word (aiming, a scripted camera) leaves it
// set: after two game steps the mouse goes back to pushing the stick there, and a -0.0 (no turn) put in the word now and then says
// when the camera reads it again.
struct LookDirect {
    std::uint32_t word = 0, check = 0, value = 0;
    bool set = false, active = true;
    double pending = 0;
    int unread = 0, probe = 0;
    std::uint64_t since = 0;   // the console frame (vblank) when the word was last seen read; unread time counts frames, not
                               // passes of the window (which come 2 to 3 times a frame at 144 Hz, or many while the game hitches)
} g_look_direct;
// rawdanger-recomp (v0.9.1): the first-person view (Triangle) looks round with the LEFT stick, and its turn is not the follow
// camera's: PS2_MOUSE_LOOK_FIRST="WORD=VALUE" (Raw Danger: the active camera block 0x48F4E0 is the first-person one, 0x5FDC60)
// makes the mouse push the left stick while it holds, as far as the mouse moved, spread over a few frames
struct LookFirst {
    bool set = false;
    std::uint32_t word = 0, value = 0;
} g_look_first;
std::uint32_t g_look_when_address = 0, g_look_when_value = 0;
bool g_look_parsed = false, g_look_when_set = false;
double g_look_dx = 0, g_look_dy = 0;
int g_look_step = 3;   // frames of one game step (PS2_MOUSE_LOOK_STEP; Raw Danger: 3, its 20 steps a second)   // mouse counts since the last frame (relative mode)  // mouse counts since the last sample (only while the mouse pushes a stick)
Uint32 g_mouse_sample = 0;
double g_mouse_vx = 0.0, g_mouse_vy = 0.0;  // counts per millisecond, smoothed
Uint32 g_wheel_up_until = 0, g_wheel_down_until = 0;
int g_last_dx = 0, g_last_dy = -1;  // the last direction the walk keys gave, for a roll without one

// The picture look (F2): how the picture is enlarged and smoothed for the window (display only, the game and the saved pictures do not
// change). The choice sits in keys.ini ('look = ...'); PS2_LOOK=0..7 or a look word overrides it for a run and is not written.
int g_look = keymap::kDefaultLook;
bool g_blank_picture = false;   // rawdanger-recomp: a skipped scene (PS2_SKIP_SCENE): the picture is black
std::uint32_t video_bits() {
    return (g_blank_picture ? 128u : 0u) | static_cast<std::uint32_t>(g_look) | (g_keys.integer_scale ? 8u : 0u) | (g_keys.smaa ? 16u : 0u) | (static_cast<std::uint32_t>(std::clamp(g_keys.sharpen, 0, 3)) << 5);
}
bool g_look_forced = false;

// The title shows the speed; for a few seconds after F5, F8 or F9 a message is put in front of it.
std::string g_speed_title = "ps2-native-recomp";
// rawdanger-recomp (v0.9.6, Settings > Debug): the stats at the left of the picture, once a second
bool g_stats_on = false;
// rawdanger-recomp (v0.9.7, Settings > Debug): the game's steps a second forced (a test of the uncapped frame rate, docs/FRAMERATE.md
// in rawdanger-recomp): PS2_DEBUG_STEPS="POINTER,OFFSET" names the word of vblanks a step waits for (*POINTER + OFFSET)
int g_force_vblanks = 0;   // 0 the game's, else 3, 2 or 1 vblanks a step
struct DebugSteps { std::uint32_t pointer = 0, offset = 0; bool set = false; };
const DebugSteps &debug_steps() {
    static const DebugSteps d = [] {
        DebugSteps r;
        unsigned long p = 0, o = 0;
        if (const char *v = std::getenv("PS2_DEBUG_STEPS"); v && std::sscanf(v, "%lx,%lx", &p, &o) == 2) r = {static_cast<std::uint32_t>(p), static_cast<std::uint32_t>(o), true};
        return r;
    }();
    return d;
}
std::vector<std::pair<std::string, std::string>> g_stats_rows;
std::string g_game_title;  // optional PS2_WINDOW_TITLE, supplied by the launcher
std::string g_notice_text;
const char *const kStartNotice = "F1 = menu";  // the title for the first 30 s
Uint32 g_notice_until = 0;
// A short tone mixed into the game's sound says that a key worked, which the title cannot do in full screen. 0 none, else the kind of
// the message (1 saved, 2 loaded, 3 report written, 4 trouble). PS2_NO_BEEP=1 turns it off.
std::atomic<int> g_beep_request{0};
bool g_beep_on = true;

// The quick menu (F1, the extra menu key of keys.ini (none by default), hold Select then press Start or the Guide button on a controller;
// host/quickmenu.cpp). While it is open the
// emulation thread waits at the end of its frame (on_vblank), so the game stands still and makes no sound, and the window thread keeps
// drawing the last picture with the panel over it. The game's controller ports get nothing while it is open, and after it a port gets
// nothing until its buttons are all let go (the Enter or Cross that chose "Resume" must not reach the game).
quickmenu::Menu g_menu;
std::atomic<bool> g_menu_hold{false};
std::string g_key_note;  // the keys page's message line: what the last change did
std::string g_recomp_note;  // the Recomp options page's message line

// rawdanger-recomp: the game's switchable post processing (recomp.ini "Original PS2 Post Processing Filter", on = the filter; builds
// before v0.8.0: "No post-processing filter", on = no filter), shown as Settings > Picture > PS2 post-processing filter;
// -1 when the build has none
bool post_filter_on();  // below
int blur_section() {
    const std::vector<recomp::Section> &all = recomp::sections();
    for (std::size_t i = 0; i < all.size(); ++i)
        if (all[i].live && (all[i].name == "Original PS2 Post Processing Filter" || all[i].name == "No post-processing filter" ||
                            all[i].label.find("blur") != std::string::npos)) return static_cast<int>(i);
    return -1;
}
// the game's post processing is on (the old section's "on" was the filter removed)
bool post_filter_on() {
    const int blur = blur_section();
    if (blur < 0) return false;
    const recomp::Section &sec = recomp::sections()[static_cast<std::size_t>(blur)];
    return sec.name == "No post-processing filter" ? !sec.on : sec.on;
}
bool g_menu_redraw = false;  // show the picture again: the panel changed while the game sends none
quickmenu::Picture g_menu_picture;
SDL_Texture *g_menu_texture = nullptr;  // the panel for the SDL renderer (PS2_SDL_RENDERER=1)
int g_menu_texture_w = 0, g_menu_texture_h = 0;
Uint32 g_menu_opened_ticks = 0;
bool g_block_input[2] = {};
int g_stick_step[2] = {};  // the left stick moving the highlight: -1 up, 1 down, 0 near the middle
// The inner resolution (PS2_SCALE): the one of this run, and the one keys.ini holds for the next start (the menu changes that one).
int g_scale_now = 1, g_scale_next = 1;
bool g_scale_locked = false;  // PS2_SCALE was set before the start: keys.ini does not count
// The filter of upscaled 2D art (PS2_SHARP_2D: 0 soft, 1 sharp, 2 sharpest), the same way: this run's and keys.ini's for the next start.
int g_sharp_now = 1, g_sharp_next = 1;
bool g_sharp_locked = false;
// A short note drawn over the picture for a moment after the menu is closed ("Saved", "Loaded"): the menu asked for a quick save or load
bool g_note_wanted = false;  // a save or load was asked by the menu: the next message about it is shown over the picture
std::string g_note_text;  // not empty while the note is up
// rawdanger-recomp: the achievement pop-up shown with the note (its picture, straight RGBA); empty: a plain note
std::vector<std::uint32_t> g_toast_icon;

bool file_exists(const std::string &path) {
    if (path.empty()) return false;
    if (std::FILE *f = std::fopen(path.c_str(), "rb")) { std::fclose(f); return true; }
    return false;
}

// rawdanger-recomp: the achievements page's badges, read once (.bmp from RawDangerPC.exe) and kept; grey = the locked look made here
// when there is no _lock picture (the luminance, darkened a little)
const quickmenu::Values::Icon *menu_icon(const std::string &path, bool grey) {
    static std::map<std::string, quickmenu::Values::Icon> cache;
    if (path.empty()) return nullptr;
    const std::string key = path + (grey ? "#grey" : "");
    if (auto it = cache.find(key); it != cache.end()) return it->second.width > 0 ? &it->second : nullptr;
    quickmenu::Values::Icon &icon = cache[key];
    if (SDL_Surface *bmp = SDL_LoadBMP(path.c_str())) {
        if (SDL_Surface *rgba = SDL_ConvertSurfaceFormat(bmp, SDL_PIXELFORMAT_ABGR8888, 0)) {
            icon.width = rgba->w;
            icon.height = rgba->h;
            icon.rgba.resize(static_cast<std::size_t>(icon.width) * icon.height);
            for (int y = 0; y < icon.height; ++y)
                std::memcpy(icon.rgba.data() + static_cast<std::size_t>(y) * icon.width, static_cast<const std::uint8_t *>(rgba->pixels) + y * rgba->pitch,
                            static_cast<std::size_t>(icon.width) * 4);
            SDL_FreeSurface(rgba);
            if (grey)
                for (std::uint32_t &px : icon.rgba) {
                    const int r = px & 255, g = px >> 8 & 255, b = px >> 16 & 255;
                    const int l = (r * 77 + g * 150 + b * 29) >> 8;
                    const int d = l * 3 / 4;
                    px = static_cast<std::uint32_t>(d) | static_cast<std::uint32_t>(d) << 8 | static_cast<std::uint32_t>(d) << 16 | (px & 0xFF000000u);
                }
        }
        SDL_FreeSurface(bmp);
    }
    return icon.width > 0 ? &icon : nullptr;
}
int g_toast_w = 0, g_toast_h = 0;
bool g_toast = false;
std::string g_toast_title;
Uint32 g_note_until = 0;
bool g_overlay_on = false;  // g_menu_picture is laid over the game's picture (the menu, or the note)
std::uint64_t g_script_extra = 0;  // the pad script's clock goes on while the menu is open (the game's frames stand still)

void refresh_title() {
    std::string title = g_speed_title;
    if (!g_notice_text.empty()) title = g_notice_text + "   |   " + title;
    if (!g_game_title.empty()) title = g_game_title + "   |   " + title;
    SDL_SetWindowTitle(g_window, title.c_str());
}

bool create_renderer() {
    g_renderer = SDL_CreateRenderer(g_window, -1, SDL_RENDERER_ACCELERATED);
    if (!g_renderer) g_renderer = SDL_CreateRenderer(g_window, -1, SDL_RENDERER_SOFTWARE);
    if (!g_renderer) {
        std::fprintf(stderr, "SDL: %s\n", SDL_GetError());
        return false;
    }
    return true;
}

#ifdef __APPLE__
void destroy_metal_view() {
    if (!g_metal_view) return;
    SDL_Metal_DestroyView(g_metal_view);
    g_metal_view = nullptr;
}
#endif

bool attach_gpu() {
    if (std::getenv("PS2_SDL_RENDERER")) return false;
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(g_window, &info)) return false;
    ps2::WindowTarget target;
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(g_window, &w, &h);
    target.width = static_cast<std::uint32_t>(w);
    target.height = static_cast<std::uint32_t>(h);
    switch (info.subsystem) {
#if defined(__APPLE__) && defined(SDL_VIDEO_DRIVER_COCOA)
    case SDL_SYSWM_COCOA:
        g_metal_view = SDL_Metal_CreateView(g_window);
        if (!g_metal_view) return false;
        target.kind = ps2::WindowTarget::Metal;
        target.surface = SDL_Metal_GetLayer(g_metal_view);
        break;
#endif
#if defined(SDL_VIDEO_DRIVER_X11)
    case SDL_SYSWM_X11:
        target.kind = ps2::WindowTarget::X11;
        target.display = info.info.x11.display;
        target.window = static_cast<std::uint64_t>(info.info.x11.window);
        break;
#endif
#if defined(SDL_VIDEO_DRIVER_WAYLAND)
    case SDL_SYSWM_WAYLAND:
        target.kind = ps2::WindowTarget::Wayland;
        target.display = info.info.wl.display;
        target.surface = info.info.wl.surface;
        break;
#endif
#if defined(SDL_VIDEO_DRIVER_WINDOWS)
    case SDL_SYSWM_WINDOWS:
        target.kind = ps2::WindowTarget::Win32;
        target.display = info.info.win.hinstance;
        target.surface = info.info.win.window;
        break;
#endif
    default: return false;
    }
    const bool attached = ps2::attach_window(target);
#ifdef __APPLE__
    if (!attached) destroy_metal_view();
#endif
    return attached;
}

// A controller's stick, stretched from the pad's round gate to the console's square one (host/stick_map.hpp). An axis the keyboard
// or the mouse already pushed (not 0x80) is left alone; the keyboard's own diagonal is shorter on purpose (push_stick).
void stick(SDL_GameController *pad, SDL_GameControllerAxis ax, SDL_GameControllerAxis ay, std::uint8_t &x, std::uint8_t &y) {
    std::uint8_t px, py;
    stick_map::to_bytes(SDL_GameControllerGetAxis(pad, ax), SDL_GameControllerGetAxis(pad, ay), px, py);
    if (x == 0x80) x = px;
    if (y == 0x80) y = py;
}

void rumble(int port) {
    static double last_strong[2] = {};
    static bool last_weak[2] = {};
    static Uint32 sent[2] = {};
    SDL_GameController *pad = g_pads[port];
    if (!pad) return;
    const std::uint16_t motors = ps2::pad_rumble(port);
    const double strong = (motors >> 8) < 16 ? 0.0 : std::sqrt((motors >> 8) / 255.0);
    const bool weak = motors & 1u;
    const Uint32 now = SDL_GetTicks();
    if (std::fabs(strong - last_strong[port]) <= 0.08 && weak == last_weak[port] && (!(strong > 0.0 || weak) || now - sent[port] < 100)) return;
    if (g_pad_log && (strong != last_strong[port] || weak != last_weak[port]))
        std::fprintf(stderr, "[pad] frame %llu port %d rumble: large motor %u, small motor %s\n", static_cast<unsigned long long>(ps2::frames_run()), port + 1,
                     motors >> 8, weak ? "on" : "off");
    last_strong[port] = strong;
    last_weak[port] = weak;
    sent[port] = now;
    SDL_GameControllerRumble(pad, static_cast<Uint16>(strong * 65535.0), weak ? 0xFFFF : 0, strong > 0.0 || weak ? 200 : 0);
}

const char *pad_kind(SDL_GameController *pad) {
    switch (SDL_GameControllerGetType(pad)) {
    case SDL_CONTROLLER_TYPE_XBOX360: return "Xbox 360";
    case SDL_CONTROLLER_TYPE_XBOXONE: return "Xbox One/Series";
    case SDL_CONTROLLER_TYPE_PS3: return "DualShock 3";
    case SDL_CONTROLLER_TYPE_PS4: return "DualShock 4";
    case SDL_CONTROLLER_TYPE_PS5: return "DualSense";
    case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_PRO: return "Switch Pro";
    case SDL_CONTROLLER_TYPE_VIRTUAL: return "virtual";
    default: return "generic";
    }
}

const char *or_unknown(const char *name) { return name ? name : "a controller"; }

// a new controller takes the first free port, the second one plugs in player 2
void open_pad(int device) {
    const SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(device);
    for (SDL_GameController *pad : g_pads)
        if (pad && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pad)) == id) return;
    for (int port = 0; port < 2; ++port)
        if (!g_pads[port]) {
            g_pads[port] = SDL_GameControllerOpen(device);
            if (g_pads[port]) std::fprintf(stderr, "[pad] %s (%s) in port %d\n", or_unknown(SDL_GameControllerName(g_pads[port])), pad_kind(g_pads[port]), port + 1);
            else std::fprintf(stderr, "[pad] cannot open controller %d: %s\n", device, SDL_GetError());
            return;
        }
    std::fprintf(stderr, "[pad] %s is not used: both ports have a controller\n", or_unknown(SDL_GameControllerNameForIndex(device)));
}

void close_pad(SDL_JoystickID id) {
    for (int port = 0; port < 2; ++port) {
        SDL_GameController *&pad = g_pads[port];
        if (!pad || SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pad)) != id) continue;
        std::fprintf(stderr, "[pad] %s unplugged from port %d\n", or_unknown(SDL_GameControllerName(pad)), port + 1);
        SDL_GameControllerClose(pad);
        pad = nullptr;
    }
}

// --- PS2_PAD_SCRIPT: virtual controllers, to test without hardware ---
//
// PS2_PAD_SCRIPT=<file> plugs in virtual game controllers (SDL's virtual joysticks, which go through the same game controller
// code as a real pad) and plays a sequence on them, one step per line, at a video frame of the game:
//   <frame> plug <n>                          plugs in virtual controller n (0 to 3)
//   <frame> unplug <n>                        pulls it out again
//   <frame> button <n> <name> <frames>        holds a button: a b x y back guide start leftstick rightstick leftshoulder
//                                             rightshoulder dpup dpdown dpleft dpright
//   <frame> axis <n> <name> <value> <frames>  holds a stick or a trigger: leftx lefty rightx righty from -32768 to 32767 (0 is
//                                             the middle, negative is left or up), lefttrigger righttrigger from 0 to 32767
// Lines starting with # are comments. Controller input then counts while the window is in the background too, so a test
// does not depend on which window has the focus.
struct Step {
    enum Kind { kPlug, kUnplug, kButton, kAxis };
    std::uint64_t frame = 0, frames = 0;
    Kind kind = kPlug;
    int pad = 0, control = 0, value = 0;
};

struct Hold {
    std::uint64_t until = 0;
    int pad = 0, control = 0;
    bool axis = false;
};

struct VirtualPad {
    SDL_Joystick *joystick = nullptr;
    Uint16 low = 0, high = 0;
};

std::vector<Step> g_script;
std::size_t g_next_step = 0;
std::vector<Hold> g_holds;
VirtualPad g_virtual[4];

int SDLCALL virtual_rumble(void *user, Uint16 low, Uint16 high) {
    const int n = static_cast<int>(reinterpret_cast<std::intptr_t>(user));
    VirtualPad &v = g_virtual[n];
    if (low != v.low || high != v.high)
        std::fprintf(stderr, "[pad] frame %llu virtual controller %d rumbles: low %u high %u\n", static_cast<unsigned long long>(ps2::frames_run()), n, low, high);
    v.low = low;
    v.high = high;
    return 0;
}

Sint16 raw_axis(int control, int value) {
    // the game controller layer maps a trigger's whole joystick range onto 0..32767, so a trigger at rest is the joystick minimum
    if (control == SDL_CONTROLLER_AXIS_TRIGGERLEFT || control == SDL_CONTROLLER_AXIS_TRIGGERRIGHT)
        return static_cast<Sint16>(std::clamp(value * 2 - 32768, -32768, 32767));
    return static_cast<Sint16>(std::clamp(value, -32768, 32767));
}

void load_pad_script(const std::string &path) {
    std::FILE *f = ps2::os::open(path, "r");
    if (!f) {
        std::fprintf(stderr, "[pad] cannot read %s\n", path.c_str());
        return;
    }
    char line[256];
    int number = 0;
    while (std::fgets(line, sizeof line, f)) {
        ++number;
        char what[32] = {}, name[32] = {};
        unsigned long long frame = 0, frames = 0;
        int pad = 0, value = 0;
        if (line[0] == '#') continue;
        const int fields = std::sscanf(line, "%llu %31s %d %31s", &frame, what, &pad, name);
        if (fields <= 0) continue;
        Step step;
        step.frame = frame;
        step.pad = pad;
        const std::string w = what;
        bool ok = fields >= 3 && pad >= 0 && pad < 4;
        if (ok && (w == "plug" || w == "unplug")) step.kind = w == "plug" ? Step::kPlug : Step::kUnplug;
        else if (ok && w == "button" && std::sscanf(line, "%*u %*s %*d %*s %llu", &frames) == 1) {
            step.kind = Step::kButton;
            step.control = SDL_GameControllerGetButtonFromString(name);
            ok = step.control != SDL_CONTROLLER_BUTTON_INVALID;
        } else if (ok && w == "axis" && std::sscanf(line, "%*u %*s %*d %*s %d %llu", &value, &frames) == 2) {
            step.kind = Step::kAxis;
            step.control = SDL_GameControllerGetAxisFromString(name);
            step.value = value;
            ok = step.control != SDL_CONTROLLER_AXIS_INVALID;
        } else ok = false;
        if (!ok) {
            std::fprintf(stderr, "[pad] %s line %d is not a step: %s", path.c_str(), number, line);
            continue;
        }
        step.frames = frames;
        g_script.push_back(step);
    }
    std::fclose(f);
    std::stable_sort(g_script.begin(), g_script.end(), [](const Step &a, const Step &b) { return a.frame < b.frame; });
    std::fprintf(stderr, "[pad] playing %zu steps from %s on virtual controllers\n", g_script.size(), path.c_str());
}

void plug_virtual(int n, bool on) {
    VirtualPad &v = g_virtual[n];
    if (on == (v.joystick != nullptr)) return;
    if (!on) {
        const SDL_JoystickID id = SDL_JoystickInstanceID(v.joystick);
        SDL_JoystickClose(v.joystick);
        v.joystick = nullptr;
        for (int i = 0; i < SDL_NumJoysticks(); ++i)
            if (SDL_JoystickGetDeviceInstanceID(i) == id) {
                SDL_JoystickDetachVirtual(i);
                break;
            }
        return;
    }
    static const char *const kNames[4] = {"ps2run virtual controller 0", "ps2run virtual controller 1", "ps2run virtual controller 2",
                                          "ps2run virtual controller 3"};
    SDL_VirtualJoystickDesc desc;
    SDL_zero(desc);
    desc.version = SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
    desc.type = SDL_JOYSTICK_TYPE_GAMECONTROLLER;
    desc.naxes = SDL_CONTROLLER_AXIS_MAX;
    desc.nbuttons = SDL_CONTROLLER_BUTTON_DPAD_RIGHT + 1;
    desc.name = kNames[n];
    desc.userdata = reinterpret_cast<void *>(static_cast<std::intptr_t>(n));
    desc.Rumble = virtual_rumble;
    const int device = SDL_JoystickAttachVirtualEx(&desc);
    v.joystick = device >= 0 ? SDL_JoystickOpen(device) : nullptr;
    if (!v.joystick) {
        std::fprintf(stderr, "[pad] cannot attach virtual controller %d: %s\n", n, SDL_GetError());
        return;
    }
    SDL_JoystickSetVirtualAxis(v.joystick, SDL_CONTROLLER_AXIS_TRIGGERLEFT, SDL_JOYSTICK_AXIS_MIN);
    SDL_JoystickSetVirtualAxis(v.joystick, SDL_CONTROLLER_AXIS_TRIGGERRIGHT, SDL_JOYSTICK_AXIS_MIN);
}

// runs before SDL reads its events, which apply the values set here (and bring the plug and unplug events)
void play_pad_script() {
    static Uint32 last_ticks = SDL_GetTicks(), menu_ms = 0;
    const Uint32 ticks = SDL_GetTicks();
    if (g_menu.is_open()) {
        menu_ms += ticks - last_ticks;
        g_script_extra += menu_ms / 17;  // about one frame of the script per 1/60 s while the game waits
        menu_ms %= 17;
    }
    last_ticks = ticks;
    const std::uint64_t now = ps2::frames_run() + g_script_extra;
    for (auto h = g_holds.begin(); h != g_holds.end();) {
        if (now < h->until) {
            ++h;
            continue;
        }
        if (SDL_Joystick *j = g_virtual[h->pad].joystick) {
            if (h->axis) SDL_JoystickSetVirtualAxis(j, h->control, raw_axis(h->control, 0));
            else SDL_JoystickSetVirtualButton(j, h->control, SDL_RELEASED);
        }
        h = g_holds.erase(h);
    }
    for (; g_next_step < g_script.size() && g_script[g_next_step].frame <= now; ++g_next_step) {
        const Step &s = g_script[g_next_step];
        if (s.kind == Step::kPlug || s.kind == Step::kUnplug) {
            plug_virtual(s.pad, s.kind == Step::kPlug);
            continue;
        }
        SDL_Joystick *j = g_virtual[s.pad].joystick;
        if (!j) {
            std::fprintf(stderr, "[pad] frame %llu: virtual controller %d is not plugged in\n", static_cast<unsigned long long>(now), s.pad);
            continue;
        }
        if (s.kind == Step::kAxis) SDL_JoystickSetVirtualAxis(j, s.control, raw_axis(s.control, s.value));
        else SDL_JoystickSetVirtualButton(j, s.control, SDL_PRESSED);
        g_holds.push_back(Hold{now + s.frames, s.pad, s.control, s.kind == Step::kAxis});
    }
}

// --- the key file and the key card ---

bool mouse_look_on();
void apply_mouse_mode() {
    // a mouse that pushes a stick needs the pointer locked in the window, else it runs into the window's edge
    const bool want = g_window && (g_keys.mouse_stick != 0 || mouse_look_on()) && !g_menu.is_open() && (SDL_GetWindowFlags(g_window) & SDL_WINDOW_INPUT_FOCUS);
    if ((SDL_GetRelativeMouseMode() == SDL_TRUE) != want) SDL_SetRelativeMouseMode(want ? SDL_TRUE : SDL_FALSE);
    g_mouse_sum_x = g_mouse_sum_y = 0;
    g_mouse_vx = g_mouse_vy = 0.0;
}

void remember_keys_time() {
    std::error_code error;
    const auto t = std::filesystem::last_write_time(ps2::os::path(g_keys_path), error);
    if (!error) g_keys_time = t;
}

// Reads keys.ini; writes it with the defaults when it is missing. A line that cannot be read is logged and ignored.
void load_keys() {
    std::string text;
    bool have = false;
    if (std::FILE *f = ps2::os::open(g_keys_path, "rb")) {
        char buffer[4096];
        std::size_t n;
        while ((n = std::fread(buffer, 1, sizeof buffer, f)) > 0) text.append(buffer, n);
        std::fclose(f);
        have = true;
    } else if (std::FILE *out = ps2::os::open(g_keys_path, "w")) {
        const std::string defaults = keymap::default_file_text();
        std::fwrite(defaults.data(), 1, defaults.size(), out);
        std::fclose(out);
        std::fprintf(stderr, "[keys] wrote %s with the default keys\n", g_keys_path.c_str());
    } else {
        std::fprintf(stderr, "[keys] cannot write %s: the default keys are used and cannot be changed\n", g_keys_path.c_str());
    }
    keymap::Config config = keymap::defaults();
    std::vector<std::string> problems;
    if (have) keymap::parse(text, config, problems);
    for (const std::string &p : problems) std::fprintf(stderr, "[keys] %s, %s\n", g_keys_path.c_str(), p.c_str());
    g_keys = config;
    {
        static bool font_tried = false;
        if (!font_tried) {
            font_tried = true;
            if (const char *font = std::getenv("PS2_MENU_FONT"); font && *font)
                std::fprintf(stderr, "[menu] the game's font for the Options look: %s\n", quickmenu::load_game_font(font) ? font : "not readable, the menu's own letters");
        }
        // rawdanger-recomp: the menu's look: keys.ini, else the game's own Options screen (v0.8.0; needs the game's font), else the
        // launcher's skin (PS2_MENU_SKIN=japanese) or the game's language
        int skin = g_keys.menu_skin;
        if (skin == 2 && !quickmenu::game_font_loaded()) skin = -1;
        if (skin < 0 && quickmenu::game_font_loaded()) skin = 2;
        if (skin < 0) {
            const char *want = std::getenv("PS2_MENU_SKIN");
            const char *lang = std::getenv("PS2_LANGUAGE");
            skin = (want && (std::string(want) == "japanese" || std::string(want) == "1")) || (!want && lang && std::string(lang) == "Japanese") ? 1 : 0;
        }
        quickmenu::set_skin(skin);
        g_skin_chosen = skin;
        ps2::set_reverb(g_keys.reverb != 0);
        if (g_keys.smaa) ps2::set_smaa_quality(g_keys.smaa == 5 ? 8u : static_cast<unsigned>(g_keys.smaa));
        send_key_prompts();
    }
    ps2::set_bar_opacity(static_cast<unsigned>(std::clamp(g_keys.black_bars, 0, 100)));
    quickmenu::set_popup_size(g_keys.popup_size);
    if (g_keys.filter_pass >= 0) ps2::set_game_filter_pass(g_keys.filter_pass);
    ps2::set_ambient_occlusion(g_keys.ambient_occlusion);
    g_master_volume = std::clamp(g_keys.master_volume, 0, 100);
    lens_rain::set_level(g_keys.lens_rain);
    if (!g_look_forced && config.look >= 0) g_look = config.look;
    g_keys.look = g_look;
    ps2::set_video_options(video_bits());
    remember_keys_time();
    std::fprintf(stderr, "[keys] %s: %s%zu problem%s; mouse stick %s\n", g_keys_path.c_str(), have ? "read, " : "defaults, ", problems.size(),
                 problems.size() == 1 ? "" : "s", g_keys.mouse_stick == 0 ? "off" : g_keys.mouse_stick == 1 ? "left" : "right");
    apply_mouse_mode();
}

// when the window gets the focus back, and when the key card opens: pick up changes the player made to keys.ini
void reload_keys_if_changed() {
    if (g_keys_path.empty()) return;
    std::error_code error;
    const auto t = std::filesystem::last_write_time(ps2::os::path(g_keys_path), error);
    if (error || t != g_keys_time) load_keys();  // missing: written again with the defaults
}

#ifdef _WIN32
void edit_keys_file() {
    const std::wstring path = ps2::os::path(g_keys_path).wstring();
    if (reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) <= 32)
        ShellExecuteW(nullptr, L"open", L"notepad.exe", (L"\"" + path + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
}
#endif

// F1 (or the help key, H by default): the key card. A message box of the system. The buttons are the quick save and the quick load,
// for laptops where F5 and F8 need the Fn key.
//
// Windows and macOS: the box blocks the window thread while it is open (Windows keeps answering the window's messages inside the box;
// the game holds: the picture stands, the sound goes quiet, and it goes on where it was when the box closes).
//
// Everywhere else (Linux, the BSDs): the box runs on a thread of its own and the game keeps running behind it. SDL's box there is a
// separate program (zenity) or a window of the compositor, and the window thread must never wait for it: a box that is hidden by the
// compositor, opened on another workspace, or simply not noticed stopped the whole game for as long as it stood (the window thread
// answers the compositor, draws the pictures and takes the emulation's frames; GTA III froze 14 s that way on 2026-10-29,
// docs/linux.md "The window freeze"). The answer is taken in poll(), on the window thread.
#if !defined(_WIN32) && !defined(__APPLE__)
#define PS2_KEY_CARD_ON_A_THREAD 1
#endif

enum KeyCardChoice { kCardClose, kCardSave, kCardLoad, kCardEdit };

struct KeyCard {
    std::string text;
    std::atomic<int> state{0};  // 0 no box, 1 the box is open (its thread is in SDL_ShowMessageBox), 2 closed, the answer waits in poll()
    std::atomic<int> chosen{kCardClose};
    std::atomic<bool> failed{false};
    ps2::hitch::Clock::time_point opened{};
};
// never destroyed: a box that is still open when the program ends must not meet a destroyed object
KeyCard &g_card = *new KeyCard;
Uint32 g_focus_gained_ticks = 0;  // when the window last got the keyboard focus, for the log line when the card opens

void close_menu_if_open(const char *why);

// what happens after the box: the cursor back, the mouse mode again, the button that was chosen
void key_card_answer(int chosen) {
    SDL_ShowCursor(g_menu.is_open() ? SDL_ENABLE : SDL_DISABLE);
    apply_mouse_mode();
    if (chosen == kCardSave || chosen == kCardLoad) {
        close_menu_if_open("a button of the key card");  // the card was opened from the menu, which still holds the game
        g_note_wanted = true;
        ps2::quick_request(chosen == kCardSave ? ps2::QuickKey::save : ps2::QuickKey::load);
    }
#ifdef _WIN32
    else if (chosen == kCardEdit) edit_keys_file();
#endif
}

// the window thread, once per pass of poll(): the box on its thread has closed
void take_key_card_answer() {
#ifdef PS2_KEY_CARD_ON_A_THREAD
    if (g_card.state.load(std::memory_order_acquire) != 2) return;
    const double seconds = ps2::hitch::ms_since(g_card.opened) / 1000.0;
    if (!g_card.failed.load()) std::fprintf(stderr, "[keys] the key card was closed after %.1f s (the game ran on behind it)\n", seconds);
    const int chosen = g_card.chosen.load();
    g_card.state.store(0, std::memory_order_release);
    key_card_answer(chosen);
#endif
}

// why: the name of the key that opened it, for the log (how a card opens that nobody meant to open is the question this answers)
void show_key_card(const char *why) {
#ifdef PS2_KEY_CARD_ON_A_THREAD
    if (g_card.state.load(std::memory_order_acquire) != 0) return;  // one box at a time
#endif
    reload_keys_if_changed();
    g_card.text = keymap::card_text(g_keys);
    if (!g_keys_path.empty())
        g_card.text += "\nTo change keys: \"Change keys\" in the quick menu, or edit this file (the game reads it again when you come back):\n" + g_keys_path + "\n";
    if (g_notice_text == kStartNotice) {
        g_notice_text.clear();
        refresh_title();
    }
    const Uint32 since_focus = SDL_GetTicks() - g_focus_gained_ticks;
    std::fprintf(stderr, "[keys] the key card opens (%s; the window got the keyboard focus %s)\n", why,
                 g_focus_gained_ticks && since_focus < 3000 ? (std::to_string(since_focus) + " ms before").c_str() : "more than 3 s before or never");
    SDL_SetRelativeMouseMode(SDL_FALSE);  // the box needs the pointer
    SDL_ShowCursor(SDL_ENABLE);
    g_card.opened = ps2::hitch::Clock::now();
#ifdef PS2_KEY_CARD_ON_A_THREAD
    g_card.failed.store(false);
    g_card.chosen.store(kCardClose);
    g_card.state.store(1, std::memory_order_release);
    std::thread([] {
        SDL_MessageBoxButtonData buttons[] = {
            {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT | SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, kCardClose, "Close"},
            {0, kCardSave, "Quick save (F5)"},
            {0, kCardLoad, "Quick load (F8)"},
        };
        SDL_MessageBoxData box{};
        box.flags = SDL_MESSAGEBOX_INFORMATION;
        box.window = nullptr;  // the box is not tied to the game window, whose thread is not the one that waits for it
        box.title = "Keys (F1)";
        box.message = g_card.text.c_str();
        box.numbuttons = static_cast<int>(std::size(buttons));
        box.buttons = buttons;
        int chosen = kCardClose;
        if (SDL_ShowMessageBox(&box, &chosen) != 0) {
            std::fprintf(stderr, "[keys] cannot show the key card: %s\n", SDL_GetError());
            g_card.failed.store(true);
            chosen = kCardClose;
        }
        g_card.chosen.store(chosen);
        g_card.state.store(2, std::memory_order_release);
    }).detach();
#else
    SDL_MessageBoxButtonData buttons[] = {
        {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT | SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, kCardClose, "Close"},
        {0, kCardSave, "Quick save (F5)"},
        {0, kCardLoad, "Quick load (F8)"},
#ifdef _WIN32
        {0, kCardEdit, "Edit keys.ini"},
#endif
    };
    SDL_MessageBoxData box{};
    box.flags = SDL_MESSAGEBOX_INFORMATION;
    box.window = g_window;
    box.title = "Keys (F1)";
    box.message = g_card.text.c_str();
    box.numbuttons = static_cast<int>(std::size(buttons));
    box.buttons = buttons;
    int chosen = kCardClose;
    if (SDL_ShowMessageBox(&box, &chosen) != 0) {
        std::fprintf(stderr, "[keys] cannot show the key card: %s\n", SDL_GetError());
        chosen = kCardClose;
    }
    key_card_answer(chosen);
#endif
}

// A stick pushed by keys: all the way in a direction; two keys make a diagonal that is as far from the middle as a straight push
// (not 1.4 times as far, which the game would take as a push past the stick's circle).
// rawdanger-recomp: walk speed for the walk keys, set with the mouse wheel when the wheel is not given to a button (keys.ini)
// rawdanger-recomp: the walk keys' pace (the mouse wheel): the game has two, walking (the stick past its dead zone, however far)
// and running (Cross held with the stick); v0.8.0's five pushes of the stick all walked the same, so since v0.8.4 the wheel
// switches between the two: down walks, up runs (the walk keys then hold Cross, only while playing)
constexpr int kPushSteps = 1, kWalkSteps = 2;
int g_walk_step = kPushSteps;   // walking
double walk_push() { return 127.0; }
bool wheel_is_a_button() {
    for (const auto &list : g_keys.inputs)
        for (int input : list)
            if (input == keymap::kWheelUp || input == keymap::kWheelDown) return true;
    return false;
}

void push_stick(int dx, int dy, std::uint8_t &x, std::uint8_t &y) {
    const bool diagonal = dx != 0 && dy != 0;
    if (dx) x = diagonal ? static_cast<std::uint8_t>(0x80 + dx * 90) : (dx < 0 ? 0x00 : 0xFF);
    if (dy) y = diagonal ? static_cast<std::uint8_t>(0x80 + dy * 90) : (dy < 0 ? 0x00 : 0xFF);
}

// the mouse as a stick: how fast it moves, sampled every 16 ms (the window thread runs more often than the game's frames)
void parse_mouse_look() {
    if (g_look_parsed) return;
    g_look_parsed = true;
    if (const char *y = std::getenv("PS2_MOUSE_LOOK_YAW"); y && *y) {
        unsigned long a = 0;
        if (std::sscanf(y, "%lx,%f,%f", &a, &g_look_yaw.rate, &g_look_yaw.dead) == 3 && g_look_yaw.rate > 0) {
            g_look_yaw.address = static_cast<std::uint32_t>(a);
            g_look_yaw.angle = true;
            g_look_yaw.sign = -1;   // Raw Danger: right (255) makes the angle smaller
            g_look_yaw.set = true;
        }
    }
    if (const char *p = std::getenv("PS2_MOUSE_LOOK_PITCH"); p && *p) {
        unsigned long a = 0;
        if (std::sscanf(p, "%lx,%f,%f,%f,%f", &a, &g_look_pitch.rate, &g_look_pitch.dead, &g_look_pitch.lo, &g_look_pitch.hi) == 5 && g_look_pitch.rate > 0) {
            g_look_pitch.address = static_cast<std::uint32_t>(a);
            g_look_pitch.set = true;
        }
    }
    if (const char *dd = std::getenv("PS2_MOUSE_LOOK_DIRECT"); dd && *dd) {
        unsigned long w = 0, c = 0, v = 0;
        if (std::sscanf(dd, "%lx,%lx=%lx", &w, &c, &v) == 3) {
            g_look_direct.word = static_cast<std::uint32_t>(w), g_look_direct.check = static_cast<std::uint32_t>(c);
            g_look_direct.value = static_cast<std::uint32_t>(v), g_look_direct.set = true;
        }
    }
    if (const char *fp = std::getenv("PS2_MOUSE_LOOK_FIRST"); fp && *fp) {
        unsigned long w = 0, v = 0;
        if (std::sscanf(fp, "%lx=%lx", &w, &v) == 2) g_look_first = {true, static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(v)};
    }
    if (const char *st = std::getenv("PS2_MOUSE_LOOK_STEP"); st && *st) g_look_step = std::clamp(std::atoi(st), 1, 6);
    if (const char *w = std::getenv("PS2_MOUSE_LOOK_WHEN"); w && *w) {
        unsigned long a = 0, v = 0;
        if (std::sscanf(w, "%lx=%lu", &a, &v) == 2) {
            g_look_when_address = static_cast<std::uint32_t>(a);
            g_look_when_value = static_cast<std::uint32_t>(v);
            g_look_when_set = true;
        }
    }
    if (g_look_yaw.set)
        std::fprintf(stderr, "[mouse] mouse look: the camera angle at 0x%08X%s%s; keys.ini mouse_look = on/off, mouse_speed, mouse_invert\n", g_look_yaw.address, g_look_direct.set ? ", turned directly when the build has the code" : "",
                     g_look_pitch.set ? " and its height" : "");
}

bool mouse_look_on() {
    parse_mouse_look();
    if (!g_look_yaw.set || !g_keys.mouse_look || g_menu.is_open() || !g_window || !(SDL_GetWindowFlags(g_window) & SDL_WINDOW_INPUT_FOCUS)) return false;
    return !g_look_when_set || ps2::ld32(g_look_when_address) == g_look_when_value;
}

float ram_float(std::uint32_t address) {
    const std::uint32_t w = ps2::ld32(address);
    float f;
    std::memcpy(&f, &w, 4);
    return f;
}

// one axis of the mouse look: `counts` of mouse movement in, the stick's value out (0x80: no turn). Open loop, measured once: the
// game turns `rate` for each unit the stick is pushed past its dead zone, at each of its steps (3 frames: Raw Danger runs at 20
// steps a second). The mouse's movement of one step becomes one push held for exactly one step, so the game sees it once whatever
// the frame it reads the pad at, and turns exactly that far; only what is past the game's top speed waits for the next step.
// No smoothing and no acceleration of our own: the same movement always turns the same angle.
std::uint8_t look_axis(LookAxis &a, double counts, double per_count, int frame_in_step, int step_frames) {
    if (!a.set) return 0x80;
    a.pending += counts * per_count;
    if (frame_in_step != 0) return a.held;   // the push of this step stays for all its frames
    const double most = a.rate * (127.0 - a.dead);
    double want = a.pending;
    if (!a.angle) {
        // the height has ends: nothing is kept toward an end it is at
        const float now = ram_float(a.address);
        if (std::isfinite(now) && ((now >= a.hi - 1e-3f && a.sign * want > 0) || (now <= a.lo + 1e-3f && a.sign * want < 0))) want = a.pending = 0;
    }
    if (std::fabs(want) < a.rate * 0.5) {   // less than the smallest turn the game makes: kept for the next step
        a.held = 0x80;
        return a.held;
    }
    const double turn = std::clamp(want, -most, most);
    a.pending = std::clamp(want - turn, -most * 2.0, most * 2.0);   // past the top speed: at most two more steps, then dropped
    const double push = a.dead + std::fabs(turn) / a.rate;
    a.held = static_cast<std::uint8_t>(std::clamp(128.0 + std::copysign(std::round(push), turn), 0.0, 255.0));
    (void)step_frames;
    return a.held;
}

// the direct turn of one frame: true when the camera takes it (the stick is then left alone for the mouse's turn)
bool look_direct(double turn) {
    LookDirect &d = g_look_direct;
    if (!d.set || ps2::ld32(d.check) != d.value) return false;
    const std::uint32_t now = ps2::ld32(d.word);
    const std::uint64_t frame = ps2::frame_count();
    if (!d.active) {   // the camera did not read the word: now and then a -0.0 asks again
        if (now == 0 && d.probe == 0) {
            ps2::st32(d.word, 0x80000000u);
            d.probe = 1, d.since = frame;
        } else if (now == 0 && d.probe > 0) {
            d.active = true, d.probe = 0, d.unread = 0, d.pending = 0, d.since = frame;
            std::fprintf(stderr, "[mouse] the camera takes the mouse's turn again\n");
        } else if (frame - d.since > 30) {
            ps2::st32(d.word, 0);
            d.probe = 0;
        }
        if (!d.active) return false;
    }
    d.pending = std::clamp(d.pending + turn, -3.14159, 3.14159);   // at most half a turn a step
    if (now == 0) {
        d.unread = 0, d.since = frame;
        if (d.pending != 0) {
            const float f = static_cast<float>(d.pending);
            std::uint32_t bits;
            std::memcpy(&bits, &f, 4);
            ps2::st32(d.word, bits);
            d.pending = 0;
        }
    } else if (frame - d.since > static_cast<std::uint64_t>(3 * std::max(1, g_look_step))) {
        ps2::st32(d.word, 0);   // not read for three of the game's steps (console frames): this camera does not take it
        d.active = false, d.unread = 0, d.pending = 0, d.probe = 0;
        std::fprintf(stderr, "[mouse] this camera does not take the mouse's turn: the stick turns it\n");
    }
    return true;
}

void mouse_look_reset() {
    if (g_look_direct.set && g_look_direct.word) ps2::st32(g_look_direct.word, 0);
    g_look_direct.pending = 0, g_look_direct.unread = 0;
    g_look_yaw.pending = g_look_pitch.pending = 0;
    g_look_yaw.have_prev = g_look_pitch.have_prev = false;
    g_look_yaw.held = g_look_pitch.held = 0x80;
    g_look_dx = g_look_dy = 0;
}

void mouse_stick_value(std::uint8_t &x, std::uint8_t &y) {
    const Uint32 now = SDL_GetTicks();
    const Uint32 dt = now - g_mouse_sample;
    if (dt >= 16) {
        g_mouse_vx = g_mouse_vx * 0.5 + 0.5 * g_mouse_sum_x / dt;
        g_mouse_vy = g_mouse_vy * 0.5 + 0.5 * g_mouse_sum_y / dt;
        g_mouse_sum_x = g_mouse_sum_y = 0;
        g_mouse_sample = now;
    }
    const auto level = [&](double v) {
        double d = v * 60.0 * g_keys.mouse_speed;
        // past the game's own dead zone at once, so slow mouse moves still turn the camera
        if (std::abs(d) > 2.0) d += std::copysign(g_keys.mouse_deadzone * 127.0, d);
        return static_cast<std::uint8_t>(std::clamp(128.0 + d, 0.0, 255.0));
    };
    x = level(g_keys.mouse_invert & 1 ? -g_mouse_vx : g_mouse_vx);
    y = level(g_keys.mouse_invert & 2 ? -g_mouse_vy : g_mouse_vy);
}

// the keyboard and the mouse as the first controller
void read_keyboard(std::uint16_t &pressed, std::uint8_t &lx, std::uint8_t &ly, std::uint8_t &rx, std::uint8_t &ry) {
    static const std::uint16_t kDpad[] = {kUp, kDown, kLeft, kRight};
    const Uint8 *keys = SDL_GetKeyboardState(nullptr);
    // mouse buttons only count while the window is the one in front (a click on another window is not for the game)
    const bool focus = !g_window || (SDL_GetWindowFlags(g_window) & SDL_WINDOW_INPUT_FOCUS);
    const Uint32 mouse = focus ? SDL_GetMouseState(nullptr, nullptr) : 0;
    const Uint32 now = SDL_GetTicks();
    const auto down = [&](int input) {
        switch (input) {
        case keymap::kMouseLeft: return (mouse & SDL_BUTTON_LMASK) != 0;
        case keymap::kMouseMiddle: return (mouse & SDL_BUTTON_MMASK) != 0;
        case keymap::kMouseRight: return (mouse & SDL_BUTTON_RMASK) != 0;
        case keymap::kMouseX1: return (mouse & SDL_BUTTON_X1MASK) != 0;
        case keymap::kMouseX2: return (mouse & SDL_BUTTON_X2MASK) != 0;
        case keymap::kWheelUp: return static_cast<Sint32>(g_wheel_up_until - now) > 0;
        case keymap::kWheelDown: return static_cast<Sint32>(g_wheel_down_until - now) > 0;
        default: return input > 0 && input < SDL_NUM_SCANCODES && keys[input] != 0;
        }
    };
    const auto any = [&](int action) {
        for (int input : g_keys.inputs[action])
            if (down(input)) return true;
        return false;
    };
    for (int i = 0; i < 4; ++i)
        if (any(keymap::DpadUp + i)) pressed |= kDpad[i];
    static const struct {
        int action;
        std::uint16_t mask;
    } kButtons[] = {
        {keymap::Cross, kCross}, {keymap::Circle, kCircle}, {keymap::Square, kSquare}, {keymap::Triangle, kTriangle},
        {keymap::L1, kL1},       {keymap::R1, kR1},         {keymap::L2, kL2},         {keymap::R2, kR2},
        {keymap::L3, kL3},       {keymap::R3, kR3},         {keymap::Start, kStart},   {keymap::Select, kSelect},
    };
    for (const auto &b : kButtons)
        if (any(b.action)) pressed |= b.mask;
    if (any(keymap::Rage)) pressed |= kL3 | kR3;
    // two opposite keys together cancel each other
    const int ldx = (any(keymap::LeftRight) ? 1 : 0) - (any(keymap::LeftLeft) ? 1 : 0);
    const int ldy = (any(keymap::LeftDown) ? 1 : 0) - (any(keymap::LeftUp) ? 1 : 0);
    const int rdx = (any(keymap::RightRight) ? 1 : 0) - (any(keymap::RightLeft) ? 1 : 0);
    const int rdy = (any(keymap::RightDown) ? 1 : 0) - (any(keymap::RightUp) ? 1 : 0);
    push_stick(ldx, ldy, lx, ly);
    // rawdanger-recomp: the walk keys push the left stick only so far (the mouse wheel sets how far: a slow walk to a run, as a
    // stick pushed part of the way)
    if ((ldx || ldy) && g_walk_step < kPushSteps) {
        const double k = walk_push() / 127.0;
        lx = static_cast<std::uint8_t>(std::clamp(128.0 + (static_cast<int>(lx) - 128) * k, 0.0, 255.0));
        ly = static_cast<std::uint8_t>(std::clamp(128.0 + (static_cast<int>(ly) - 128) * k, 0.0, 255.0));
    }
    // the top step runs: Cross held with the walk keys, only while playing (in menus Cross would choose)
    if ((ldx || ldy) && g_walk_step == kWalkSteps && (!g_look_when_set || ps2::ld32(g_look_when_address) == g_look_when_value)) pressed |= kCross;
    push_stick(rdx, rdy, rx, ry);
    if (ldx || ldy) {
        g_last_dx = ldx;
        g_last_dy = ldy;
    }
    // The roll: God of War's evade is a push of the right stick. While the roll key is held the walk keys push it too (the left
    // stick keeps its push, so Kratos faces the way he rolls); held with no walk key, it rolls the way he was last walking.
    if (any(keymap::Roll)) {
        const bool walking = ldx || ldy;
        push_stick(walking ? ldx : g_last_dx, walking ? ldy : g_last_dy, rx, ry);
    }
    // rawdanger-recomp: mouse look while playing (the pointer is caught in the window then, and free again in menus)
    ps2::set_camera_orbit(g_keys.camera_orbit != 0);
    ps2::set_free_camera(g_keys.free_camera != 0);
    ps2::set_head_follow(g_keys.head_follow != 0);
    ps2::set_wet_look_on(g_keys.wet_look != 0);
    ps2::set_anim_layers(g_keys.anim_layers != 0);
    if (g_force_vblanks > 0 && debug_steps().set) {
        const std::uint32_t g = ps2::ld32(debug_steps().pointer);
        if (g >= 0x100000u && g < 0x2000000u) ps2::st32(g + debug_steps().offset, static_cast<std::uint32_t>(g_force_vblanks));
    }
    if (g_keep_hero) heal_hero();
    {
        static bool was = false;
        const bool on = mouse_look_on();
        if (on != was) {
            was = on;
            mouse_look_reset();
            apply_mouse_mode();
        }
        if (on) {
            // the modern way round by default: the mouse to the right turns the view right, the mouse up looks up (the game's own
            // stick is the other way on both); mouse_invert x / y turns either back
            const double speed = g_keys.look_speed;
            const double dx = (g_keys.mouse_invert & 1) ? g_look_dx : -g_look_dx, dy = (g_keys.mouse_invert & 2) ? g_look_dy : -g_look_dy;
            g_look_dx = g_look_dy = 0;
            static int frame_in_step = 0;
            const int step = g_look_step;
            // the turn straight into the camera when the build has the code for it (the stick's way round: right on the stick, 255,
            // makes the game's turn smaller), else as a push of the stick
            static double first_x = 0, first_y = 0;
            if (g_look_first.set && ps2::ld32(g_look_first.word) == g_look_first.value) {
                // right on the left stick turns the view right, up looks up: the mouse the same way (dx and dy are the stick's way
                // round already, so their sign flips)
                first_x = std::clamp(first_x - dx * 8.0 * speed, -1200.0, 1200.0);
                first_y = std::clamp(first_y - dy * 8.0 * speed, -1200.0, 1200.0);
                const double ox = std::clamp(first_x, -127.0, 127.0), oy = std::clamp(first_y, -127.0, 127.0);
                first_x -= ox * 0.35, first_y -= oy * 0.35;
                if (std::fabs(first_x) < 0.5) first_x = 0;
                if (std::fabs(first_y) < 0.5) first_y = 0;
                if (lx == 0x80) lx = static_cast<std::uint8_t>(std::lround(128.0 + ox));
                if (ly == 0x80) ly = static_cast<std::uint8_t>(std::lround(128.0 + oy));
                return;
            }
            first_x = first_y = 0;
            const bool direct = look_direct(-dx * 0.0022 * speed);
            const std::uint8_t sx = direct ? 0x80 : look_axis(g_look_yaw, dx, 0.0022 * speed, frame_in_step, step);
            // the orbit camera (v0.9.0) takes the mouse's up / down as a turn round the hero: up looks up (the camera goes down)
            const bool orbit = ps2::camera_orbit_available() && g_keys.camera_orbit;
            if (orbit && dy != 0) ps2::add_camera_orbit_turn(static_cast<float>(-dy * 0.0022 * speed));
            const std::uint8_t sy = orbit ? 0x80 : look_axis(g_look_pitch, dy, 0.008 * speed, frame_in_step, step);
            frame_in_step = (frame_in_step + 1) % std::max(1, step);
            if (rx == 0x80) rx = sx;
            if (ry == 0x80) ry = sy;
        }
    }
    if (g_keys.mouse_stick) {
        std::uint8_t mx = 0x80, my = 0x80;
        mouse_stick_value(mx, my);
        std::uint8_t &sx = g_keys.mouse_stick == 1 ? lx : rx, &sy = g_keys.mouse_stick == 1 ? ly : ry;
        if (sx == 0x80) sx = mx;
        if (sy == 0x80) sy = my;
    }
}

void poll_port(int port) {
    static bool plugged[2] = {};
    SDL_GameController *pad = g_pads[port];
    if (port > 0 && plugged[port] != (pad != nullptr)) {
        plugged[port] = pad != nullptr;
        ps2::set_pad_connected(port, plugged[port]);
    }
    std::uint16_t pressed = 0;
    std::uint8_t l2 = 0xFF, r2 = 0xFF;
    std::uint8_t lx = 0x80, ly = 0x80, rx = 0x80, ry = 0x80;
    if (port == 0) read_keyboard(pressed, lx, ly, rx, ry);
    if (pad) {
        for (const PadBinding &b : kPadButtons)
            if (b.button != SDL_CONTROLLER_BUTTON_BACK && SDL_GameControllerGetButton(pad, b.button)) pressed |= b.mask;
        // Select, then Start opens the quick menu, and the game must get neither. So the game gets this Select late: once it has been
        // held for 300 ms without Start, or as a short press when it is let go without Start. (Start first and then Select would
        // already have reached the game: the way is Select first.) A Select of the keyboard (Tab) is not held back.
        static Uint32 select_since[2] = {}, select_tap_until[2] = {};
        static bool select_combo[2] = {};
        const bool sel = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_BACK), st = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_START);
        const Uint32 now = SDL_GetTicks();
        bool select_to_game = false;
        if (sel) {
            if (!select_since[port]) select_since[port] = now ? now : 1;
            if (st) select_combo[port] = true;
            select_to_game = !select_combo[port] && now - select_since[port] >= 300;
        } else {
            if (select_since[port] && !select_combo[port]) select_tap_until[port] = now + 80;
            select_since[port] = 0;
            select_combo[port] = false;
        }
        if (select_to_game || static_cast<Sint32>(select_tap_until[port] - now) > 0) pressed |= kSelect;
        if (sel && st) pressed &= ~kStart;
        const auto trigger = [&](SDL_GameControllerAxis a, std::uint16_t bit, std::uint8_t &pressure) {
            const int v = SDL_GameControllerGetAxis(pad, a);
            if (v <= 2000 || (pressed & bit)) return;
            pressed |= bit;
            pressure = static_cast<std::uint8_t>(std::max(1, v * 255 / 32767));
        };
        trigger(SDL_CONTROLLER_AXIS_TRIGGERLEFT, kL2, l2);
        trigger(SDL_CONTROLLER_AXIS_TRIGGERRIGHT, kR2, r2);
        stick(pad, SDL_CONTROLLER_AXIS_LEFTX, SDL_CONTROLLER_AXIS_LEFTY, lx, ly);
        stick(pad, SDL_CONTROLLER_AXIS_RIGHTX, SDL_CONTROLLER_AXIS_RIGHTY, rx, ry);
    }
    // rawdanger-recomp: Cross on the game's own "PC settings" line, or Select anywhere on its Options screen, opens the PC settings
    // in the screen's look (the game does not get the button)
    if (port == 0 && !g_menu.is_open() && (native_scene() || native_pause()) &&
        ((pressed & kSelect) || ((pressed & kCross) && g_native_row.set && native_cursor() == static_cast<int>(g_native_row.row)))) {
        open_menu("Select on the game's Options screen");
        g_menu.open_native();
        if (quickmenu::game_font_loaded()) quickmenu::set_skin(2);
        menu_paint();
    }
    // (the memory-card check at start, PS2_AUTO_CROSS: answered in the runtime, ps2::latch_pads, in the game's own frames)
    if (g_menu.is_open()) g_block_input[port] = true;
    if (g_block_input[port]) {
        if (!pressed) g_block_input[port] = false;  // all let go: from the next pass on the game gets this port again
        pressed = 0;
        lx = ly = rx = ry = 0x80;
        l2 = r2 = 0xFF;
    } else {
        rumble(port);  // while the game waits its rumble is not sent again, so it stops after 200 ms
    }
    static std::uint64_t last[2] = {~0ull, ~0ull};
    const std::uint64_t state = pressed | (static_cast<std::uint64_t>(rx) << 16) | (static_cast<std::uint64_t>(ry) << 24) | (static_cast<std::uint64_t>(lx) << 32) |
                                (static_cast<std::uint64_t>(ly) << 40) | (static_cast<std::uint64_t>(l2) << 48) | (static_cast<std::uint64_t>(r2) << 56);
    if (state == last[port]) return;
    last[port] = state;
    if (g_pad_log) {
        static const char *const kNames[16] = {"select", "l3", "r3", "start", "up", "right", "down", "left",
                                               "l2", "r2", "l1", "r1", "triangle", "circle", "cross", "square"};
        std::string names;
        for (int i = 0; i < 16; ++i)
            if (pressed & (1u << i)) names += std::string(" ") + kNames[i];
        std::fprintf(stderr, "[pad] frame %llu port %d buttons %04X%s, left stick %u %u, right stick %u %u, l2 %u r2 %u\n",
                     static_cast<unsigned long long>(ps2::frames_run()), port + 1, pressed, names.empty() ? " none" : names.c_str(), lx, ly, rx, ry,
                     pressed & kL2 ? l2 : 0, pressed & kR2 ? r2 : 0);
    }
    if (port == 0 && g_debug_menu_on.load(std::memory_order_relaxed)) {   // the debug menu's cursor, as the game moves it
        static bool was_up = false, was_down = false;
        const bool up = (pressed & (1u << 4)) || ly < 0x40, down = (pressed & (1u << 6)) || ly > 0xC0;
        if (up && !was_up) g_debug_cursor.store(std::max(0, g_debug_cursor.load() - 1));
        if (down && !was_down) g_debug_cursor.store(std::min(2, g_debug_cursor.load() + 1));
        was_up = up, was_down = down;
    }
    ps2::set_pad_pressure(8, l2, port);
    ps2::set_pad_pressure(9, r2, port);
    ps2::set_pad_buttons(pressed, port);
    ps2::set_pad_axis(0, rx, port);
    ps2::set_pad_axis(1, ry, port);
    ps2::set_pad_axis(2, lx, port);
    ps2::set_pad_axis(3, ly, port);
}

// keys.ini with some lines changed (`change` maps the file's text to the new text, e.g. with keymap::with_look), the other lines as they are
void rewrite_keys_file(const std::function<std::string(const std::string &)> &change, const char *what) {
    if (g_keys_path.empty()) return;
    std::string text;
    if (std::FILE *f = ps2::os::open(g_keys_path, "rb")) {
        char buffer[4096];
        std::size_t n;
        while ((n = std::fread(buffer, 1, sizeof buffer, f)) > 0) text.append(buffer, n);
        std::fclose(f);
    } else {
        text = keymap::default_file_text();
    }
    text = change(text);
    if (std::FILE *out = ps2::os::open(g_keys_path, "wb")) {
        std::fwrite(text.data(), 1, text.size(), out);
        std::fclose(out);
        remember_keys_time();
    } else {
        std::fprintf(stderr, "[keys] cannot write %s: the %s is kept until the game closes\n", g_keys_path.c_str(), what);
    }
}

// F2: the next look (step -1: the one before, from the quick menu). The title says which one, and the choice goes into keys.ini (the
// other lines of the file stay as they are).
void cycle_look(int step = 1) {
    // the upscale filter (raw pixels, soft, sharp, FSR 1); the anti-aliasing (bit 0: smooth edges) stays as it is
    g_look = ((g_look / 2 + step + 4) % 4) * 2 + (g_look & 1);
    g_keys.look = g_look;
    ps2::set_video_options(video_bits());
    g_notice_text = std::string("Picture look (F2): ") + keymap::look_name(g_look);
    g_notice_until = SDL_GetTicks() + 4000;
    refresh_title();
    std::fprintf(stderr, "[video] picture look %d: %s\n", g_look, keymap::look_name(g_look));
    if (g_look_forced) return;
    rewrite_keys_file([](const std::string &t) { return keymap::with_look(t, g_look); }, "look");
}

// --- the quick menu ---

void toggle_fullscreen() {
    const bool full = SDL_GetWindowFlags(g_window) & SDL_WINDOW_FULLSCREEN;
    SDL_SetWindowFullscreen(g_window, full ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
}

void window_pixels(int &w, int &h) {
    w = h = 0;
    if (g_renderer) SDL_GetRendererOutputSize(g_renderer, &w, &h);
    else SDL_GetWindowSizeInPixels(g_window, &w, &h);
}

// Makes the panel (the menu, or the note after it) for the window's size and hands it to the one who draws (or takes it away).
// rawdanger-recomp: the keys list stays up while playing (keys.ini keys_on_screen, F10)
bool keys_on_screen_now() { return g_keys.keys_on_screen != 0; }

// v0.10.3: the PC rows' spinner values over the game's Options list (filled in namespace nopt below, read for the overlay)
namespace nopt {
struct ValueText { int x, y; std::string text; };
extern std::vector<ValueText> g_value_text;
}  // namespace nopt

void menu_paint(bool show_again) {
    if (show_again) g_menu_redraw = true;
    g_overlay_on = g_menu.is_open() || !g_note_text.empty() || g_native_hint || keys_on_screen_now() || g_stats_on || g_debug_menu_on.load(std::memory_order_relaxed) ||
                   !nopt::g_value_text.empty();
    if (!g_overlay_on) {
#ifdef PS2_GPU
        if (g_gpu_present) ps2::gs::gpu::set_overlay(nullptr, 0, 0, 0, 0);
#endif
        return;
    }
    int w = 0, h = 0;
    window_pixels(w, h);
    if (g_menu.is_open()) {
        quickmenu::Values v;
        v.look = keymap::look_name(g_look);
        v.scale_now = g_scale_now;
        v.scale_next = g_scale_next;
        v.scale_locked = g_scale_locked;
        v.scale_max = static_cast<int>(ps2::max_internal_scale());
        v.graphics_api = g_keys.graphics_api;
        v.graphics_api_now = ps2::graphics_api();
        v.sharp_now = g_sharp_now;
        v.sharp_next = g_sharp_next;
        v.sharp_locked = g_sharp_locked;
        v.fullscreen = SDL_GetWindowFlags(g_window) & SDL_WINDOW_FULLSCREEN;
        v.pause_unfocused = g_keys.pause_unfocused != 0;
        v.menu_game_runs = g_keys.menu_game_runs != 0;
        v.popup_size = g_keys.popup_size;
        v.filter_pass = ps2::game_filter_pass();
        v.ambient_occlusion = ps2::ambient_occlusion();
        v.wet_look = g_keys.wet_look;
        v.anim_layers = ps2::head_follow_available() ? g_keys.anim_layers : -1;
        v.field_of_view = g_keys.field_of_view;
        v.keys_on_screen = g_keys.keys_on_screen != 0;
        v.game_waits = g_menu_hold.load();
        v.master_volume = g_master_volume;
        v.integer_scale = g_keys.integer_scale != 0;
        v.menu_skin = quickmenu::skin();
        v.antialias = g_keys.smaa ? 1 + g_keys.smaa : (g_look & 1) ? 1 : 0;
        {
            static const char *const kFilters[] = {"Raw pixels", "Soft (TV)", "Sharp", "FSR 1"};
            v.filter = kFilters[std::clamp(g_look / 2, 0, 3)];
        }
        v.lens_rain = lens_rain::level();
        v.lens_rain_known = lens_rain::knows_when();
        v.title = g_game_title.substr(0, g_game_title.find("  -  "));  // "Raw Danger! Enhanced Edition", without the program's name
        // rawdanger-recomp: the launcher names this build's language and the other build's (PS2_LANGUAGE, PS2_LANGUAGE_OTHER)
        if (const char *now = std::getenv("PS2_LANGUAGE")) v.language_now = now;
        if (const char *other = std::getenv("PS2_LANGUAGE_OTHER")) if (std::getenv("PS2_LANGUAGE_SWITCH_FILE")) v.language_other = other;
        v.widescreen = -1;
        for (const recomp::Section &sec : recomp::sections())
            if (sec.aspect169 && sec.live) v.widescreen = sec.on ? 1 : 0;
        // rawdanger-recomp: Settings' new lines
        if (g_window) {
            if (SDL_GetWindowFlags(g_window) & SDL_WINDOW_FULLSCREEN) v.window_size = "full screen (F11 for a window)";
            else {
                int ww = 0, wh = 0;
                SDL_GetWindowSize(g_window, &ww, &wh);
                v.window_size = std::to_string(ww) + " x " + std::to_string(wh);
            }
        }
        if (const int blur = blur_section(); blur >= 0) v.game_blur = post_filter_on() ? 1 : 0;
        v.reverb = ps2::reverb();
        v.key_prompts = std::getenv("PS2_PROMPT_FONT_KEY") ? g_keys.key_prompts : -1;
        v.black_bars = g_keys.black_bars;
        v.sharpen = g_keys.sharpen;
        parse_mouse_look();
        v.mouse_look = g_look_yaw.set ? g_keys.mouse_look : -1;
        v.camera_orbit = ps2::camera_orbit_available() ? g_keys.camera_orbit : -1;
        v.free_camera = ps2::free_camera_available() ? g_keys.free_camera : -1;
        v.head_follow = ps2::head_follow_available() ? g_keys.head_follow : -1;
        v.mouse_speed = g_keys.look_speed;
        v.mouse_invert_y = (g_keys.mouse_invert & 2) != 0;
        v.mouse_invert_x = (g_keys.mouse_invert & 1) != 0;
        if (g_menu.on_keys_page()) {
            for (int a = 0; a < keymap::kChangeableCount; ++a) v.key_rows.push_back({keymap::action_label(a), keymap::keys_text(g_keys, a)});
            v.key_note = g_key_note;
        }
        if (g_menu.on_achievements_page()) {
            int won = 0, all_points = 0;
            for (const achievements::Row &r : achievements::rows()) {
                quickmenu::Values::RecompRow row;
                row.label = r.title + (r.unofficial ? " (unofficial)" : "");
                row.state = r.unsupported ? "n/a" : !r.enabled ? "off" : (r.unlocked ? "* " : "") + std::to_string(r.points);
                row.live = r.unlocked || (r.enabled && !r.unsupported);
                // the badge: in colour once unlocked, grey until then (the RetroAchievements look)
                row.icon = r.unlocked ? menu_icon(r.icon, false) : menu_icon(r.icon_locked.empty() ? r.icon : r.icon_locked, r.icon_locked.empty() || !file_exists(r.icon_locked));
                if (!r.unlocked && row.icon == nullptr && !r.icon.empty()) row.icon = menu_icon(r.icon, true);
                row.detail = r.description + (r.unlocked ? "  (unlocked " + r.when + ")" : r.unsupported ? "  (cannot be checked here)" : "");
                all_points += r.points;
                if (r.unlocked) won += r.points;
                v.recomp_rows.push_back(row);
            }
            v.progress = all_points > 0 ? static_cast<float>(won) / static_cast<float>(all_points) : 0.f;
            g_menu.set_recomp_rows(static_cast<int>(v.recomp_rows.size()));
            v.recomp_note = achievements::loaded() ? std::to_string(achievements::unlocked_count()) + " of " + std::to_string(v.recomp_rows.size()) + " unlocked, " +
                                                         std::to_string(achievements::total_points(true)) + " of " + std::to_string(achievements::total_points(false)) + " points"
                                                   : "No achievement set: start the game from RawDangerPC.exe";
            if (!g_recomp_note.empty()) v.recomp_note = g_recomp_note;
        } else if (g_menu.on_recomp_page()) {
            g_recomp_row_section.clear();
            for (std::size_t i = 0; i < recomp::sections().size(); ++i) {
                const recomp::Section &sec = recomp::sections()[i];
                if (sec.aspect169 && sec.live) continue;  // the widescreen switch lives on Settings > Display (one place only)
                if (static_cast<int>(i) == blur_section()) continue;  // and the blur on Settings > Picture
                if (!sec.live) continue;  // rawdanger-recomp: fixes built in for good (no switch) are not shown (docs/ROADMAP.md lists them)
                g_recomp_row_section.push_back(i);
                quickmenu::Values::RecompRow row;
                row.label = sec.label;
                row.live = sec.live;
                row.state = sec.on ? "<  on  >" : "<  off >";
                v.recomp_rows.push_back(row);
            }
            // v0.9.5: the test switches live on their own tab (Settings > Debug), kept in every build for reports and tests
            const auto debug_row = [&](std::size_t kind, const std::string &label, const std::string &state) {
                g_recomp_row_section.push_back(kind);
                quickmenu::Values::RecompRow row;
                row.label = label;
                row.live = true;
                row.state = state;
                v.recomp_rows.push_back(row);
            };
            if (g_menu.on_debug_tab()) {
                v.recomp_rows.clear();
                g_recomp_row_section.clear();
                static const char *const kViews[] = {"<  the game's  >", "<  AO alone  >", "<  depth  >", "<  the hero's capsules  >"};
                debug_row(kViewRow, "Picture shown", kViews[std::clamp(ps2::debug_view(), 0, 3)]);
                debug_row(kStatsRow, "Stats on screen (FPS, CPU, GPU, scene)", g_stats_on ? "<  on  >" : "<  off >");
                if (debug_info().set) debug_row(kInfoRow, "Where am I (scene, camera, place, wetness: for reports)", "show");
                if (debug_hero().set) {
                    debug_row(kHealRow, "Heal, warm and dry the hero", "now");
                    debug_row(kKeepRow, "Keep the hero healthy and warm", g_keep_hero ? "<  on  >" : "<  off >");
                }
                if (ps2::hero_wetness() >= 0.0f || ps2::head_follow_available()) {
                    debug_row(kDryRow, "Dry the hero's clothes", "now");
                    debug_row(kSoakRow, "Soak the hero's clothes", "now");
                }
                if (ps2::head_follow_available()) {
                    const float f = ps2::wet_force();
                    debug_row(kWetForceRow, "Wet look: wetness shown", f < 0.0f ? "<  the game's  >" : "<  " + std::to_string(static_cast<int>(f * 100.0f + 0.5f)) + " %  >");
                    debug_row(kWetShowRow, "Wet look: show the hero's capsules", ps2::wet_show() ? "<  on  >" : "<  off >");
                    const float t = ps2::head_test();
                    debug_row(kAnimGainRow, "Livelier hero: the layers' size", ps2::anim_gain() > 1.5f ? "<  x4 (to see them)  >" : "<  as made  >");
                    debug_row(kHeadTestRow, "Head follows camera: a fixed turn", t == 0.0f ? "<  off  >" : "<  " + std::to_string(static_cast<int>(std::lround(t * 57.29578f))) + " deg  >");
                }
                if (debug_steps().set)
                    debug_row(kStepsRow, "Game steps a second (a test: forces the pace, the game's timings stay)", g_force_vblanks == 0 ? "<  the build's  >" : "<  " + std::to_string(60 / g_force_vblanks) + "  >");
                if (debug_scenes().set) {
                    if (debug_scenes().debug) debug_row(kDebugRow, "Developers' debug menu (on the disc; Shift+F7)", "open");
                    if (debug_scenes().mapsel) debug_row(kMapSelRow, "Developers' map select (on the disc; F7)", "open");
                }
            }
            g_menu.set_recomp_rows(static_cast<int>(v.recomp_rows.size()));
            v.recomp_note = g_recomp_note;
            if (g_menu.on_debug_tab() && g_recomp_note.empty())
                v.recomp_note = v.recomp_rows.empty() ? "No test switches in this game" : "Test switches, kept in every build for reports";
        }
        g_menu.draw(w, h, v, g_menu_picture);
    } else if (g_note_text.empty() && !nopt::g_value_text.empty()) {
        // v0.10.3: the PC rows' spinner values, where the game's list puts them (the game's picture in the window: its shape fitted)
        const float aspect = ps2::g_options.widescreen ? 16.0f / 9.0f : 4.0f / 3.0f;
        const float vw = std::min(static_cast<float>(w), h * aspect), vh = vw / aspect;
        const float ox = (w - vw) * 0.5f, oy = (h - vh) * 0.5f, kx = vw / 640.0f, ky = vh / 448.0f;
        std::vector<quickmenu::Menu::NativeValue> values;
        for (const nopt::ValueText &v : nopt::g_value_text)
            values.push_back({static_cast<int>(ox + v.x * kx), static_cast<int>(oy + v.y * ky), static_cast<int>(24 * ky), v.text});
        quickmenu::Menu::draw_native_values(values, g_menu_picture);
    } else if (g_note_text.empty() && g_native_hint && g_native_label) {
        // the label of the game's line, where the game draws its words (the game's picture in the window: its shape fitted)
        const float aspect = ps2::g_options.widescreen ? 16.0f / 9.0f : 4.0f / 3.0f;
        const float vw = std::min(static_cast<float>(w), h * aspect), vh = vw / aspect;
        const float ox = (w - vw) * 0.5f, oy = (h - vh) * 0.5f, kx = vw / 640.0f, ky = vh / 448.0f;
        quickmenu::Menu::draw_native_label(static_cast<int>(ox + g_native_row.x0 * kx), static_cast<int>(oy + g_native_row.y0 * ky),
                                           static_cast<int>((g_native_row.x1 - g_native_row.x0) * kx), static_cast<int>((g_native_row.y1 - g_native_row.y0) * ky),
                                           g_native_label_selected, "PC settings", g_menu_picture);
    } else if (g_note_text.empty() && g_native_hint) {
        std::string key = "SELECT";
        if (!g_keys.inputs[keymap::Select].empty()) key += " / " + keymap::input_name(g_keys.inputs[keymap::Select].front());
        quickmenu::Menu::draw_native_hint(w, h, key + "   PC settings", g_menu_picture);
    } else if (g_debug_menu_on.load(std::memory_order_relaxed)) {
        quickmenu::Menu::draw_debug_menu(w, h, g_debug_cursor.load(), g_menu_picture);
    } else if (g_toast) {
        quickmenu::Menu::draw_toast(w, h, "ACHIEVEMENT UNLOCKED", g_toast_title, g_toast_icon, g_toast_w, g_toast_h, g_menu_picture);
    } else if (g_note_text.empty() && g_stats_on) {
        quickmenu::Menu::draw_keys_on_screen(w, h, g_stats_rows, "", g_menu_picture, "STATS   (Settings > Debug)");   // v0.9.6: the stats in the keys' place
    } else if (g_note_text.empty()) {
        // rawdanger-recomp: the keys at the left (F10); a note or an achievement takes the place for its few seconds
        quickmenu::Menu::draw_keys_on_screen(w, h, keymap::screen_rows(g_keys, g_keys.mouse_look != 0 && g_look_yaw.set), "", g_menu_picture);
    } else {
        quickmenu::Menu::draw_note(w, h, g_note_text, g_menu_picture);
    }
    const quickmenu::Picture &p = g_menu_picture;
#ifdef PS2_GPU
    if (g_gpu_present) {
        ps2::gs::gpu::set_overlay(p.rgba.data(), static_cast<std::uint32_t>(p.width), static_cast<std::uint32_t>(p.height), static_cast<std::uint32_t>(p.x),
                                  static_cast<std::uint32_t>(p.y));
        return;
    }
#endif
    if (!g_renderer) return;
    if (!g_menu_texture || g_menu_texture_w != p.width || g_menu_texture_h != p.height) {
        if (g_menu_texture) SDL_DestroyTexture(g_menu_texture);
        // ABGR8888 is a packed format: red in the low byte of the 32-bit value, as the panel is made
        g_menu_texture = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, p.width, p.height);
        if (g_menu_texture) SDL_SetTextureBlendMode(g_menu_texture, SDL_BLENDMODE_BLEND);
        g_menu_texture_w = p.width;
        g_menu_texture_h = p.height;
    }
    if (g_menu_texture) SDL_UpdateTexture(g_menu_texture, nullptr, p.rgba.data(), p.width * 4);
}

// the SDL renderer: the panel on top of the game's picture, in window pixels (the game's picture is drawn with a logical size)
void draw_menu_sdl() {
    if (!g_overlay_on || !g_menu_texture) return;
    int lw = 0, lh = 0;
    SDL_RenderGetLogicalSize(g_renderer, &lw, &lh);
    SDL_RenderSetLogicalSize(g_renderer, 0, 0);
    const SDL_Rect place{g_menu_picture.x, g_menu_picture.y, g_menu_picture.width, g_menu_picture.height};
    SDL_RenderCopy(g_renderer, g_menu_texture, nullptr, &place);
    if (lw && lh) SDL_RenderSetLogicalSize(g_renderer, lw, lh);
}

// the last picture once more, with the panel as it is now: the game sends no new ones while it waits
void menu_show_again() {
    g_menu_redraw = false;
#ifdef PS2_GPU
    if (g_gpu_present) {
        ps2::gs::gpu::present_again();
        return;
    }
#endif
    if (!g_renderer || !g_texture) return;
    SDL_RenderClear(g_renderer);
    SDL_RenderCopy(g_renderer, g_texture, nullptr, nullptr);
    draw_menu_sdl();
    SDL_RenderPresent(g_renderer);
}

void open_menu(const char *why) {
    if (g_menu.is_open()) return;
    reload_keys_if_changed();
    g_note_text.clear();
    g_key_note.clear();
    g_menu.open();
    g_menu.set_key_rows(keymap::kChangeableCount);
    // rawdanger-recomp: the game goes on behind the menu (keys.ini menu_game_runs, on by default) unless the window went to the
    // background (Pause in background): then it waits
    const bool hold = !g_keys.menu_game_runs || std::strcmp(why, "the window went to the background") == 0;
    g_menu_hold.store(hold, std::memory_order_release);
    g_menu_opened_ticks = SDL_GetTicks();
    SDL_SetRelativeMouseMode(SDL_FALSE);  // the pointer can click the entries
    SDL_ShowCursor(SDL_ENABLE);
    if (g_notice_text == kStartNotice) {
        g_notice_text.clear();
        refresh_title();
    }
    std::fprintf(stderr, "[menu] the quick menu opens (%s) at frame %llu; the game %s\n", why, static_cast<unsigned long long>(ps2::frames_run()), hold ? "waits" : "goes on");
    if (hold) perf::pause(true);
    menu_paint();
}

void close_menu(const char *why) {
    if (g_menu.native()) quickmenu::set_skin(g_skin_chosen);   // the game's look was borrowed for this page only
    if (!g_menu.is_open()) return;
    g_menu.close();
    g_block_input[0] = g_block_input[1] = true;
    menu_paint();  // takes the panel away (shown at once, before the game's next picture)
    SDL_ShowCursor(SDL_DISABLE);
    apply_mouse_mode();
    std::fprintf(stderr, "[menu] the quick menu closes (%s) after %.1f s; the game goes on\n", why, (SDL_GetTicks() - g_menu_opened_ticks) / 1000.0);
    if (g_menu_hold.load()) perf::pause(false);
    g_menu_hold.store(false, std::memory_order_release);
}

void close_menu_if_open(const char *why) { close_menu(why); }

// the scene asked for, written again each pass until the game is in it (the game's own pause holds its scene switch) or 3 s pass
std::uint32_t g_scene_request = 0;
Uint32 g_scene_request_until = 0;
std::uint32_t g_scene_before_debug = 0;

void open_debug_scene(bool map_select) {
    const DebugScenes &d = debug_scenes();
    if (!d.set || (map_select && !d.mapsel) || (!map_select && !d.debug)) return;
    const std::uint32_t target = map_select ? d.mapsel : d.debug;
    const std::uint32_t now = ps2::ld32(d.address - 4);   // the scene the game runs (Raw Danger: just before the next-scene word)
    if (g_menu.is_open()) close_menu(map_select ? "Map select" : "Debug menu");
    if (now == target) return;
    if (d.from && now != d.from) {
        set_notice(map_select ? "Map select: from the game itself (not the menus or the title)" : "Debug menu: from the game itself");
        return;
    }   // already there (it has no way out but loading a map: going back to the game scene leaves it running)
    g_scene_before_debug = now;
    g_scene_request = target;
    set_notice(map_select ? "Map select: up/down a line, left/right its value, Cross loads the map"
                          : "Debug menu: up/down a line, Cross or Circle chooses (Sound test does nothing on the released game)");
    g_scene_request_until = SDL_GetTicks() + 3000;
    ps2::st32(d.address, g_scene_request);
}

// ---------------------------------------------------------------------------------------------------------------------------------
// rawdanger-recomp: PC rows inside the game's own Options list. The game's list (OPTION.BIN, the Options screen of the title and of
// the in-game menu) draws its rows from a table of records (0x73C8A0, 0x11C bytes each): a value row has a get and a set function,
// how many values it has, its help line, and the pictures it draws (label, the cursor's bar, each value white, and the picked
// one dark on a glow). The game handles the cursor, Left / Right, the sounds and the help line itself. Here a new table is built in
// memory the game does not use (0x80000-0xFFFFF), with its rows plus ours, and the list's table pointer and row count point to it:
//   Vibration, Sound, [Resolution, Anti-aliasing, Cutscene bars, Lens rain, Mouse look], Screen color/brightness, Configuration,
//   Default Setting, EXIT   ("Adjust screen position" is left out: a PC screen needs none)
// Our rows' get and set functions are host functions the game calls (ps2::register_host_call); their words are pictures painted
// into the game's own label sheet (tools/rd_options.py: a texture override of the sheet, rows 276-511 of its 512 x 512 upload),
// placed by PS2_NATIVE_OPTIONS_ART. The rows are 30 pixels apart (50 in the game) to fit; each row carries its bar (the page's
// seven fixed bars are moved off the screen). PS2_NATIVE_OPTIONS=1 turns it on (the launcher: US program and Enhanced Edition).
namespace nopt {
// The game's Options list (OPTION.BIN, at 0x738600 in the US program) draws seven rows from a table of 0x11C-byte records at
// 0x73C8A0: a value row has its getter at +4, its setter at +8 (called with the value's index), its count at +0x10 and its help
// line at +0x1C; then sprites (index, (y << 16) | x, colour) for the label (+0x38, highlighted +0x50), the cursor (+0x44), the
// row's bar (+0x2C) and per value the white word (+0x8C + 12 v), the glow under the value in force (+0xBC + 12 v) and its dark
// word (+0xEC + 12 v). Here the list gets every PC setting as rows of its own, with records the game reads like its own: the
// getters and setters are host calls (engine patch 0021), the words are sprites of the Options pictures (tools\rd_options.py
// paints them). A value with more than two steps is a spinner: three values "<", the value now, ">" (the game moves to the
// first or the third, the host steps and the game is back on the middle). Eleven rows show at once; the list scrolls as the
// cursor nears the top or the bottom (the game's table is the eleven rows on screen, made again as it moves).
constexpr std::uint32_t kWorkPtr = 0x48F504u, kTable = 0x73C8A0u, kRows = 7, kStride = 0x11Cu, kBars = 0x73D3D0u, kSprites = 0x73C260u;
constexpr std::uint32_t kFree = 0x80000u, kSpriteArea = kFree + 0x1000u, kSpriteMax = 0x300u, kHelpArea = kFree + 0x4000u, kTextArea = kFree + 0x5000u;
constexpr std::uint32_t kGlow = 0x19, kBar = 0x29, kCursor = 0x34, kOnWhite = 0x11, kOffWhite = 0x12, kOnDark = 0x15, kOffDark = 0x16;
constexpr std::uint32_t kNone = 0xFFFFFFFFu;
constexpr int kTop = 72, kSpacing = 30, kVisible = 11, kMaxRows = 64;
constexpr int kPairX[2] = {321, 463}, kSpinX = 392, kArrowDx = 128;
struct Art { char tex = 'S'; int x = 0, y = 0, w = 0, h = 0; };
struct Row {
    std::string label, help;
    std::vector<std::string> values;      // a two-value row: its words ("ON" and "OFF" are the game's own)
    std::function<int()> get;             // two-value rows: 0 or 1
    std::function<void(int)> set;
    std::function<std::string()> text;    // a spinner: the word of the value in force
    std::function<void(int)> step;        // a spinner: -1 or +1
    bool spinner() const { return static_cast<bool>(text); }
};
std::map<std::string, Art> g_art;
std::vector<Row> g_rows;
std::atomic<int> g_pending[kMaxRows];
bool g_on = false;
// the whole list: the game's rows (>= 0, its index) and ours (-1 - our index)
std::vector<int> g_list;
std::vector<int> g_from_game;             // the game's row -> its place in g_list
std::uint32_t g_list_at = 0;              // where the game keeps the list's table, count and cursor (0: not ours now)
int g_offset = 0, g_last_cursor = 0;

int row_value(int i) {
    const Row &r = g_rows[static_cast<std::size_t>(i)];
    if (r.spinner()) return 1;            // the middle one, always
    const int p = g_pending[i].load();
    if (p >= 0) return p;
    return r.get ? r.get() : 0;
}
template <int I> void host_get(ps2::Cpu &c) { c.r[2].sd[0] = I < static_cast<int>(g_rows.size()) ? row_value(I) : 0; }
template <int I> void host_set(ps2::Cpu &c) {
    if (I < static_cast<int>(g_rows.size())) g_pending[I] = static_cast<int>(c.r[4].w[0]);
    c.r[2].sd[0] = 0;
}
template <int... I> void register_all(std::integer_sequence<int, I...>) {
    (ps2::register_host_call(static_cast<std::uint32_t>(2 * I), &host_get<I>), ...);
    (ps2::register_host_call(static_cast<std::uint32_t>(2 * I + 1), &host_set<I>), ...);
}

void load_art() {
    const char *path = std::getenv("PS2_NATIVE_OPTIONS_ART");
    if (!path || !*path) return;
    std::FILE *f = std::fopen(path, "rb");
    if (!f) return;
    char line[512];
    while (std::fgets(line, sizeof line, f)) {
        char name[400];
        char tex = 'S';
        Art a;
        if (std::sscanf(line, "%399[^\t]\t%c\t%d\t%d\t%d\t%d", name, &tex, &a.x, &a.y, &a.w, &a.h) == 6) {
            a.tex = tex;
            g_art[name] = a;
        }
    }
    std::fclose(f);
}
bool has_art(const std::string &name) { return g_art.count(name) != 0; }

// v0.10.3: looked up each time, not once a run (the title's and the pause menu's Options can load their pictures under other
// numbers). The spinners' values are no longer drawn from the background picture (the game draws a row's sprites from its word
// sheet only): the window draws them (g_value_text).
std::uint32_t texture_of(char tex) {
    const std::uint32_t sheet = ps2::ld32(kSprites) & 0xFFFFu;   // sprite 0, "Vibration", is on the label sheet
    if (tex != 'B') return sheet;
    for (std::uint32_t i = 0; i < 0x3C; ++i) {   // the 640 x 448 background
        const std::uint32_t w0 = ps2::ld32(kSprites + 16u * i);
        if ((w0 >> 16) == 640u && ps2::ld32(kSprites + 16u * i + 4) == 448u && (w0 & 0xFFFFu) != 0xFFFFu) return w0 & 0xFFFFu;
    }
    return sheet;
}

// a sprite of our own: its entry in memory the game does not use, reached through the RAM's mirror at 32 MB (the game adds
// index * 16 to its sprite table with 32-bit sums and skips negative indexes)
std::uint32_t sprite_index(std::uint32_t at) { return (0x02000000u + at - kSprites) / 16u; }
std::uint32_t g_next_sprite = 0;
std::uint32_t new_sprite() { return g_next_sprite < kSpriteMax ? kSpriteArea + 16u * g_next_sprite++ : 0; }
void write_sprite(std::uint32_t at, const Art &a, int w = -1) {
    const std::uint32_t words[4] = {(static_cast<std::uint32_t>(w < 0 ? a.w : w) << 16) | texture_of(a.tex), static_cast<std::uint32_t>(a.h),
                                    (static_cast<std::uint32_t>(a.y) << 16) | static_cast<std::uint32_t>(a.x),
                                    (static_cast<std::uint32_t>(a.y + a.h) << 16) | static_cast<std::uint32_t>(a.x + a.w)};
    for (int k = 0; k < 4; ++k)
        if (ps2::ld32(at + 4u * static_cast<std::uint32_t>(k)) != words[k]) ps2::st32(at + 4u * static_cast<std::uint32_t>(k), words[k]);
}
std::map<std::string, std::uint32_t> g_sprite_of;
std::uint32_t sprite_for(const std::string &name, int &w) {
    const auto art = g_art.find(name);
    if (art == g_art.end()) { w = 0; return kNone; }
    w = art->second.w;
    if (const auto it = g_sprite_of.find(name); it != g_sprite_of.end()) {
        write_sprite(kSprites + 16u * it->second - 0x02000000u, art->second);   // (v0.10.3: its picture's number again: it can change)
        return it->second;
    }
    const std::uint32_t at = new_sprite();
    if (!at) return kNone;
    write_sprite(at, art->second);
    return g_sprite_of[name] = sprite_index(at);
}
// the game's glow under a value (sprite 0x19, 81 pixels), stretched to a width
Art glow_art() {
    Art glow;
    const std::uint32_t g2 = ps2::ld32(kSprites + 16u * kGlow + 8), g3 = ps2::ld32(kSprites + 16u * kGlow + 12);
    glow.tex = 'S', glow.x = static_cast<int>(g2 & 0xFFFFu), glow.y = static_cast<int>(g2 >> 16);
    glow.w = static_cast<int>(g3 & 0xFFFFu) - glow.x, glow.h = static_cast<int>(g3 >> 16) - glow.y;
    return glow;
}
std::uint32_t glow_for(int w) {
    if (w <= 81) return kGlow;
    const std::string name = "G:" + std::to_string(w);
    if (const auto it = g_sprite_of.find(name); it != g_sprite_of.end()) {
        write_sprite(kSprites + 16u * it->second - 0x02000000u, glow_art(), w);
        return it->second;
    }
    const std::uint32_t at = new_sprite();
    if (!at) return kGlow;
    write_sprite(at, glow_art(), w);
    return g_sprite_of[name] = sprite_index(at);
}
// a spinner's two sprites that change with its value: the dark word, and the glow under it (the game's glow, stretched)
std::uint32_t g_dyn[kMaxRows][2];

int game_width(std::uint32_t sprite) { return static_cast<int>(ps2::ld32(kSprites + 16u * sprite) >> 16); }

using Record = std::array<std::uint32_t, kStride / 4>;
void put(Record &r, std::uint32_t off, std::uint32_t sprite, int x, int y) {
    r[off / 4] = sprite;
    r[off / 4 + 1] = (static_cast<std::uint32_t>(y & 0xFFFF) << 16) | static_cast<std::uint32_t>(x & 0xFFFF);
    r[off / 4 + 2] = sprite == kNone ? 0u : 0x80808080u;
}
Record read_record(std::uint32_t at) {
    Record r;
    for (std::uint32_t k = 0; k < kStride / 4; ++k) r[k] = ps2::ld32(at + 4u * k);
    return r;
}
// written over what is there word by word, only where it differs (the game draws from it meanwhile)
void write_record(std::uint32_t at, const Record &r) {
    for (std::uint32_t k = 0; k < kStride / 4; ++k)
        if (ps2::ld32(at + 4u * k) != r[k]) ps2::st32(at + 4u * k, r[k]);
}

std::uint32_t help_for(int i, const std::string &text) {
    const std::uint32_t s = kTextArea + 32u * static_cast<std::uint32_t>(i);
    for (std::size_t k = 0; k < 31; ++k) ps2::st8(s + static_cast<std::uint32_t>(k), k < text.size() ? static_cast<std::uint8_t>(text[k]) : 0);
    ps2::st8(s + 31, 0);
    const std::uint32_t h = kHelpArea + 32u * static_cast<std::uint32_t>(i);
    const std::uint32_t words[8] = {0x0100FFFFu, s, 0, 0, 0xFFFFFFFFu, 0, 0, 0};
    for (int k = 0; k < 8; ++k) ps2::st32(h + 4u * static_cast<std::uint32_t>(k), words[k]);
    return h;
}

// the record of one of the game's rows, moved to the slot's height
Record game_record(int g, int y) {
    Record r = read_record(kTable + static_cast<std::uint32_t>(g) * kStride);
    const int dy = y - static_cast<int>(r[0x3C / 4] >> 16);
    for (std::uint32_t k = 0x20; k + 12 <= kStride; k += 12) {
        const std::uint32_t sprite = r[k / 4], pos = r[k / 4 + 1], colour = r[k / 4 + 2];
        if (colour != 0x80808080u || sprite == kNone) continue;
        const int ny = static_cast<int>(pos >> 16) + dy;
        r[k / 4 + 1] = (static_cast<std::uint32_t>(ny & 0xFFFF) << 16) | (pos & 0xFFFFu);
    }
    r[0x2C / 4] = kBar, r[0x30 / 4] = (static_cast<std::uint32_t>((y - 1) & 0xFFFF) << 16) | 78u, r[0x34 / 4] = 0x80808080u;
    return r;
}

Record our_record(int i, int y) {
    const Row &row = g_rows[static_cast<std::size_t>(i)];
    Record r{};
    for (std::uint32_t k = 0x20; k + 12 <= kStride; k += 12) r[k / 4] = kNone;
    r[0x04 / 4] = ps2::kHostCallBase + 4u * static_cast<std::uint32_t>(2 * i);
    r[0x08 / 4] = ps2::kHostCallBase + 4u * static_cast<std::uint32_t>(2 * i + 1);
    r[0x0C / 4] = ps2::ld32(kTable + 0x0C);   // the game's drawing of a value row
    r[0x10 / 4] = row.spinner() ? 3u : static_cast<std::uint32_t>(row.values.size());
    r[0x14 / 4] = ps2::ld32(kTable + 0x14);
    r[0x18 / 4] = ps2::ld32(kTable + 0x18);
    r[0x1C / 4] = ps2::ld32(kHelpArea + 32u * static_cast<std::uint32_t>(i) + 4) ? kHelpArea + 32u * static_cast<std::uint32_t>(i) : help_for(i, row.help);
    int lw = 0;
    const std::uint32_t label = sprite_for("L:" + row.label, lw);
    put(r, 0x2C, kBar, 78, y - 1);
    put(r, 0x38, label, 83, y);   // (the game's own label pictures start 5 pixels in)
    put(r, 0x44, kCursor, 68, y - 11);
    put(r, 0x50, label, 83, y);
    if (row.spinner()) {
        int aw = 0;
        const std::uint32_t left = sprite_for("A:left", aw);
        put(r, 0x8C, left, kSpinX - kArrowDx - aw / 2, y);
        const std::uint32_t right = sprite_for("A:right", aw);
        put(r, 0x8C + 24, right, kSpinX + kArrowDx - aw / 2, y);
        // the middle value: its sprites follow the value in force (frame() keeps them so)
        const auto art = g_art.find("D:" + row.text());
        const int tw = art != g_art.end() ? art->second.w : 0;
        const int gw = std::max(81, tw + 34);
        if (!g_dyn[i][0]) {
            const std::uint32_t a = new_sprite(), b = new_sprite();
            g_dyn[i][0] = a ? sprite_index(a) : kNone;
            g_dyn[i][1] = b ? sprite_index(b) : kNone;
        }
        g_value_text.push_back({kSpinX, y - 1, row.text()});
        if (g_dyn[i][0] != kNone && g_dyn[i][1] != kNone) {
            const std::uint32_t text_at = kSprites + 16u * g_dyn[i][0] - 0x02000000u, glow_at = kSprites + 16u * g_dyn[i][1] - 0x02000000u;
            if (art != g_art.end()) write_sprite(text_at, art->second);
            else for (int k = 0; k < 4; ++k) ps2::st32(text_at + 4u * static_cast<std::uint32_t>(k), 0);
            write_sprite(glow_at, glow_art(), gw);   // the game's glow, drawn wider
            put(r, 0xBC + 12, g_dyn[i][1], kSpinX - gw / 2, y - 1);
            put(r, 0xEC + 12, g_dyn[i][0], kSpinX - tw / 2, y - 1);
            put(r, 0x8C + 12, g_dyn[i][0], kSpinX - tw / 2, y - 1);
        }
    } else {
        for (std::size_t v = 0; v < row.values.size() && v < 2; ++v) {
            const std::string &word = row.values[v];
            std::uint32_t white, dark;
            int ww = 0, dw = 0;
            if (word == "ON" || word == "OFF") {
                white = word == "ON" ? kOnWhite : kOffWhite, dark = word == "ON" ? kOnDark : kOffDark;
                ww = game_width(white), dw = game_width(dark);
            } else {
                white = sprite_for("W:" + word, ww), dark = sprite_for("D:" + word, dw);
            }
            const std::uint32_t k = 12u * static_cast<std::uint32_t>(v);
            const int cx = kPairX[v];
            put(r, 0x8C + k, white, cx - ww / 2, y - 1);
            const int gw = std::max(81, std::max(ww, dw) + 34);
            put(r, 0xBC + k, glow_for(gw), cx - gw / 2, y - 1);
            put(r, 0xEC + k, dark, cx - dw / 2, y - 1);
        }
    }
    return r;
}

int visible() { return std::min(kVisible, static_cast<int>(g_list.size())); }

// v0.10.3: the spinners' values, drawn by the engine over the list (game coordinates: centre x, top y). The game draws a row's
// sprites from its word sheet only (texture 0x7469, full), so the value words put on the background picture's spare part were
// never drawn: the values were blank. The window draws them in the game's font, on the glow the game draws.
std::vector<ValueText> g_value_text;

void build_window() {
    g_value_text.clear();
    for (int s = 0; s < visible(); ++s) {
        const int item = g_list[static_cast<std::size_t>(g_offset + s)];
        const int y = kTop + kSpacing * s;
        write_record(kFree + static_cast<std::uint32_t>(s) * kStride, item >= 0 ? game_record(item, y) : our_record(-1 - item, y));
    }
    // the page's seven fixed bars: off the screen (each row draws its own); two of their places show the scroll arrows
    for (std::uint32_t n = 0; n < kRows; ++n) {
        std::uint32_t sprite = kBar;
        int x = 78, y = 1000;
        int aw = 0;
        if (n == 0 && g_offset > 0) sprite = sprite_for("A:up", aw), x = 553 - aw / 2, y = kTop;
        if (n == 1 && g_offset + visible() < static_cast<int>(g_list.size())) sprite = sprite_for("A:down", aw), x = 553 - aw / 2, y = kTop + kSpacing * (visible() - 1);
        const std::uint32_t at = kBars + 12u * n;
        const std::uint32_t pos = (static_cast<std::uint32_t>(y & 0xFFFF) << 16) | static_cast<std::uint32_t>(x);
        if (ps2::ld32(at) != sprite) ps2::st32(at, sprite);
        if (ps2::ld32(at + 4) != pos) ps2::st32(at + 4, pos);
        if (ps2::ld32(at + 8) != 0x80808080u) ps2::st32(at + 8, 0x80808080u);
    }
}

void setup() {
    static bool done = false;
    if (done) return;
    done = true;
    const char *v = std::getenv("PS2_NATIVE_OPTIONS");
    if (!v || *v != '1') return;
    load_art();
    if (!has_art("A:up")) {
        std::fprintf(stderr, "[options] PS2_NATIVE_OPTIONS_ART has no letters (an older rd_options.py?): the PC rows are left out\n");
        return;
    }
    for (auto &p : g_pending) p = -1;
    register_all(std::make_integer_sequence<int, kMaxRows>{});
    g_on = true;
}

// the order of the list: the game's Vibration, Sound, Brightness, Button configuration and Default settings, the PC rows, the
// game's Return to Menu last ("Adjust Screen Position" is left out: the window is a PC window)
void make_list() {
    g_list = {0, 1, 3, 4, 5};
    for (int i = 0; i < static_cast<int>(g_rows.size()); ++i) g_list.push_back(-1 - i);
    g_list.push_back(6);
    g_from_game.assign(kRows, 0);
    for (std::size_t n = 0; n < g_list.size(); ++n)
        if (g_list[n] >= 0) g_from_game[static_cast<std::size_t>(g_list[n])] = static_cast<int>(n);
    g_from_game[2] = g_from_game[1];
}

// each frame of the window: on the Options screen, point its list at the rows on screen, scroll them with the cursor
void frame_body();
// v0.10.3: the window's overlay is painted again when the spinner values it shows change (or go)
void frame() {
    std::string before;
    for (const ValueText &v : g_value_text) before += std::to_string(v.x) + "," + std::to_string(v.y) + "," + v.text + ";";
    frame_body();
    std::string after;
    for (const ValueText &v : g_value_text) after += std::to_string(v.x) + "," + std::to_string(v.y) + "," + v.text + ";";
    if (after != before) menu_paint();
}

void frame_body() {
    setup();
    if (!g_on || g_rows.empty()) return;
    for (std::size_t i = 0; i < g_rows.size(); ++i) {   // the values the game set: applied here, on the window's thread
        const int p = g_pending[i].exchange(-1);
        if (p < 0) continue;
        if (g_rows[i].spinner()) { if (p != 1) g_rows[i].step(p > 1 ? 1 : -1); }
        else if (g_rows[i].set) g_rows[i].set(p);
    }
    if (ps2::ld32(0x48F488u) != 16u) { g_list_at = 0; g_value_text.clear(); return; }   // the game's Options screen (scene SETTING)
    {   // the Options overlay (US) is in
        static const char kHelp[] = "Adjust Screen Position";
        for (std::uint32_t k = 0; k < sizeof kHelp - 1; ++k)
            if (ps2::ld8(0x73D6D0u + k) != static_cast<std::uint8_t>(kHelp[k])) { g_list_at = 0; g_value_text.clear(); return; }
    }
    const std::uint32_t work = ps2::ld32(kWorkPtr);
    if (work < 0x100000u || work >= 0x2000000u - 0x1000u) return;
    if (g_list.empty()) make_list();
    const int shown = visible(), total = static_cast<int>(g_list.size());
    if (g_list_at && (ps2::ld32(g_list_at) != kFree || ps2::ld32(g_list_at + 4) != static_cast<std::uint32_t>(shown))) g_list_at = 0;
    if (!g_list_at) {
        // the game has set its own table (entering the list: from the title, the in-game menu, or back from a sub-page)
        for (std::uint32_t off = 0; off < 0x1000u; off += 4) {
            if (ps2::ld32(work + off) != kTable || ps2::ld32(work + off + 4) != kRows) continue;
            const std::uint32_t game_cursor = ps2::ld32(work + off + 8);
            const int at = game_cursor < kRows ? g_from_game[game_cursor] : 0;
            g_offset = std::clamp(at - (shown - 2), 0, total - shown);
            g_last_cursor = at - g_offset;
            build_window();
            ps2::st32(work + off + 8, static_cast<std::uint32_t>(g_last_cursor));
            ps2::st32(work + off + 4, static_cast<std::uint32_t>(shown));
            ps2::st32(work + off, kFree);
            g_list_at = work + off;
            std::fprintf(stderr, "[options] the Options list has the PC rows (%d rows, %d at a time)\n", total, shown);
            return;
        }
        g_value_text.clear();
        return;
    }
    int cursor = static_cast<int>(ps2::ld32(g_list_at + 8));
    if (cursor < 0 || cursor >= shown) return;
    const int was = g_last_cursor;
    if (std::getenv("PS2_TRACE_OPTIONS") && cursor != was) std::fprintf(stderr, "[options] cursor %d -> %d (offset %d)\n", was, cursor, g_offset);
    if (was == shown - 1 && cursor == 0 && g_offset > 0) g_offset = 0;                                   // down past the last: the top
    else if (was == 0 && cursor == shown - 1 && g_offset == 0 && total > shown) g_offset = total - shown;   // up past the first: the end
    else if (cursor >= shown - 1 && g_offset + shown < total) {
        const int by = std::min(cursor - (shown - 2), total - shown - g_offset);
        g_offset += by, cursor -= by;
    } else if (cursor <= 0 && g_offset > 0) {
        const int by = std::min(1 - cursor, g_offset);
        g_offset -= by, cursor += by;
    }
    build_window();
    if (cursor != static_cast<int>(ps2::ld32(g_list_at + 8))) ps2::st32(g_list_at + 8, static_cast<std::uint32_t>(cursor));
    g_last_cursor = cursor;
}
}  // namespace nopt

void keep_scene_request() {
    if (!g_scene_request) return;
    const DebugScenes &d = debug_scenes();
    if (ps2::ld32(d.address - 4) == g_scene_request || static_cast<Sint32>(SDL_GetTicks() - g_scene_request_until) > 0) {
        g_scene_request = 0;
        return;
    }
    if (ps2::ld32(d.address) != g_scene_request) ps2::st32(d.address, g_scene_request);
}

void set_notice(const std::string &text) {
    g_notice_text = text;
    g_notice_until = SDL_GetTicks() + 5000;
    refresh_title();
}

// the lines of keys.ini for these buttons, written with the keys now in force (comments and every other line stay as they are)
// rawdanger-recomp: the game's font shows the first key of Cross, Circle, Square and Triangle in place of their symbols
// rawdanger-recomp: the window's icon in the taskbar and Alt+Tab: the game's .ico (PS2_WINDOW_ICON, from RawDangerPC.exe), else the
// icon built into the program (PS2_EXE_ICON at the build: the one Task Manager and Explorer show)
void set_window_icon() {
#ifdef _WIN32
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(g_window, &info)) return;
    const HWND hwnd = info.info.win.window;
    HICON big = nullptr, small = nullptr;
    if (const char *path = std::getenv("PS2_WINDOW_ICON"); path && *path) {
        const int n = MultiByteToWideChar(CP_UTF8, 0, path, -1, nullptr, 0);
        std::wstring w(static_cast<std::size_t>(n > 0 ? n : 1), L'\0');
        if (n > 0) MultiByteToWideChar(CP_UTF8, 0, path, -1, w.data(), n);
        big = static_cast<HICON>(LoadImageW(nullptr, w.c_str(), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_LOADFROMFILE));
        small = static_cast<HICON>(LoadImageW(nullptr, w.c_str(), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_LOADFROMFILE));
    }
    if (!big) big = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0));
    if (!small) small = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
    if (big) SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(big));
    if (small) SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(small));
    std::fprintf(stderr, "[window] icon: %s\n", big ? "set" : "none (no PS2_WINDOW_ICON and none built in)");
#endif
}

bool g_using_pad = false;   // rawdanger-recomp: the last button pressed was a controller's (the game's texts show its symbols then)
void send_key_prompts() {
    std::string labels[4];
    if (g_keys.key_prompts && !g_using_pad) {
        const int actions[4] = {keymap::Cross, keymap::Circle, keymap::Square, keymap::Triangle};
        for (int i = 0; i < 4; ++i)
            if (!g_keys.inputs[actions[i]].empty()) labels[i] = keymap::short_input_name(g_keys.inputs[actions[i]].front());
    }
    ps2::set_button_prompts(labels);
}

void write_key_lines(const std::vector<int> &actions) {
    send_key_prompts();
    if (actions.empty()) return;
    rewrite_keys_file(
        [&](const std::string &text) {
            std::string out = text;
            for (int a : actions) out = keymap::with_inputs(out, a, g_keys.inputs[a]);
            return out;
        },
        "new keys");
}

// the key card's and the menu's idea of a mouse button
int mouse_input(Uint8 button) {
    switch (button) {
    case SDL_BUTTON_LEFT: return keymap::kMouseLeft;
    case SDL_BUTTON_MIDDLE: return keymap::kMouseMiddle;
    case SDL_BUTTON_RIGHT: return keymap::kMouseRight;
    case SDL_BUTTON_X1: return keymap::kMouseX1;
    case SDL_BUTTON_X2: return keymap::kMouseX2;
    default: return 0;
    }
}

// The keys page waits for a key: the next key or mouse button (wheel too) is the highlighted button's new key. Esc or a controller button
// cancels. Always true for the events that belong to input (nothing reaches the game).
bool capture_event(const SDL_Event &e) {
    int input = 0;
    switch (e.type) {
    case SDL_KEYDOWN:
        if (e.key.repeat) return true;
        if (e.key.keysym.scancode == SDL_SCANCODE_ESCAPE) {
            g_menu.stop_capture();
            g_key_note = "Nothing changed.";
            menu_paint();
            return true;
        }
        input = e.key.keysym.scancode;
        break;
    case SDL_MOUSEBUTTONDOWN: input = mouse_input(e.button.button); break;
    case SDL_MOUSEWHEEL: input = e.wheel.y > 0 ? keymap::kWheelUp : e.wheel.y < 0 ? keymap::kWheelDown : 0; break;
    case SDL_CONTROLLERBUTTONDOWN:
        g_menu.stop_capture();
        g_key_note = "Nothing changed.";
        menu_paint();
        return true;
    case SDL_KEYUP: case SDL_MOUSEBUTTONUP: case SDL_MOUSEMOTION: case SDL_CONTROLLERBUTTONUP: case SDL_CONTROLLERAXISMOTION: return true;
    default: return false;
    }
    if (input == 0) return true;
    std::vector<int> changed;
    std::string message;
    const int action = g_menu.key_row();
    if (!keymap::assign_input(g_keys, action, input, changed, message)) {
        g_key_note = message;  // (it stays waiting)
    } else {
        g_menu.stop_capture();
        g_key_note = message;
        write_key_lines(changed);
        std::fprintf(stderr, "[menu] keys: %s\n", message.c_str());
    }
    menu_paint();
    return true;
}

// what an entry does; step is -1 or 1 for left and right (Enter and a click go forward)
void menu_command(quickmenu::Command c, int step);
// the PC rows of the game's Options list (nopt above): every setting of the F1 menu's Settings pages, read and set through the
// same steps, then the live Game options of recomp.ini
void native_option_rows() {
    using quickmenu::Command;
    if (!nopt::g_rows.empty()) return;
    auto &rows = nopt::g_rows;
    const auto spin = [&](const char *label, const char *help, std::function<std::string()> text, std::function<void(int)> step) {
        nopt::Row r;
        r.label = label, r.help = help, r.text = std::move(text), r.step = std::move(step);
        rows.push_back(std::move(r));
    };
    const auto pick = [&](const char *label, const char *help, std::vector<std::string> values, std::function<int()> get, std::function<void(int)> set) {
        nopt::Row r;
        r.label = label, r.help = help, r.values = std::move(values), r.get = std::move(get), r.set = std::move(set);
        rows.push_back(std::move(r));
    };
    const auto command = [](Command c) { return [c](int d) { menu_command(c, d); }; };
    const auto toggle_to = [](Command c, std::function<int()> get) { return [c, get](int v) { if (get() != v) menu_command(c, 1); }; };
    // Display
    spin("Resolution", "Internal resolution", [] { return g_scale_next == keymap::kScaleAuto ? std::string("AUTO") : std::to_string(g_scale_next) + "x"; },
         command(Command::scale));
    spin("Graphics API", "Vulkan or Direct3D 12, at the next start (Vulkan allows more than 4x)", [] {
            const std::string now = ps2::graphics_api();
            if (g_keys.graphics_api == 1) return std::string("VULKAN");
            if (g_keys.graphics_api == 2) return std::string("DIRECT3D 12");
            return now == "Vulkan" ? std::string("AUTO (VULKAN)") : std::string("AUTO (DIRECT3D 12)");
        }, command(Command::graphics_api));
    spin("Window size", "The window's size", [] {
            if (!g_window || (SDL_GetWindowFlags(g_window) & SDL_WINDOW_FULLSCREEN)) return std::string("FULL SCREEN");
            int ww = 0, wh = 0;
            SDL_GetWindowSize(g_window, &ww, &wh);
            const std::string t = std::to_string(ww) + " x " + std::to_string(wh);
            return nopt::has_art("D:" + t) ? t : std::string("CUSTOM");
        }, command(Command::window_size));
    {
        const auto get = [] { return g_window && (SDL_GetWindowFlags(g_window) & SDL_WINDOW_FULLSCREEN) ? 0 : 1; };
        pick("Full screen", "Full screen or a window (F11)", {"ON", "OFF"}, get, [get](int v) { if (get() != v) toggle_fullscreen(); });
    }
    for (const recomp::Section &sec : recomp::sections())
        if (sec.aspect169 && sec.live) {
            const auto get = [] { return ps2::g_options.widescreen ? 0 : 1; };
            pick("Widescreen 16:9", "The game's picture in 16:9", {"ON", "OFF"}, get, toggle_to(Command::widescreen, get));
            break;
        }
    {
        const auto get = [] { return g_keys.integer_scale ? 0 : 1; };
        pick("Whole-number scaling", "Even pixel rows, borders", {"ON", "OFF"}, get, toggle_to(Command::integer_scale, get));
    }
    {
        const auto get = [] { return g_keys.pause_unfocused ? 0 : 1; };
        pick("Pause in background", "Wait while in the background", {"ON", "OFF"}, get, toggle_to(Command::pause_unfocused, get));
    }
    {
        const auto get = [] { return g_keys.menu_game_runs ? 0 : 1; };
        pick("Game runs behind F1", "The game goes on in the F1 menu", {"ON", "OFF"}, get, toggle_to(Command::menu_game_runs, get));
    }
    spin("Menu look", "The F1 menu's look", [] {
            static const char *const kSkins[] = {"HAZARD", "JAPANESE", "GAME"};
            return std::string(kSkins[std::clamp(quickmenu::skin(), 0, 2)]);
        }, [](int d) {
            const int n = quickmenu::game_font_loaded() ? 3 : 2;
            for (int k = 0; k < (d > 0 ? 1 : n - 1); ++k) menu_command(Command::menu_skin, 1);
        });
    spin("Pop-up size", "Achievements, notes, keys", [] {
            static const char *const kSizes[] = {"SMALL", "NORMAL", "LARGE", "HUGE"};
            return std::string(kSizes[std::clamp(g_keys.popup_size, 0, 3)]);
        }, command(Command::popup_size));
    // Picture
    spin("Upscale filter", "How the picture is enlarged", [] {
            static const char *const kFilters[] = {"RAW", "SOFT", "SHARP", "FSR 1"};
            return std::string(kFilters[std::clamp(g_look / 2, 0, 3)]);
        }, [](int d) { cycle_look(d); });
    spin("Field of view", "How wide the camera sees, in degrees across", [] {
            const int game = keymap::game_field_of_view(ps2::g_options.widescreen);
            return g_keys.field_of_view <= 0 ? std::to_string(game) + "\xC2\xB0 (PS2)" : std::to_string(g_keys.field_of_view) + "\xC2\xB0";
        }, command(Command::field_of_view));
    spin("Anti-aliasing", "Smooth edges", [] {
            static const char *const kAa[] = {"OFF", "EDGES", "SMAA LOW", "SMAA MEDIUM", "SMAA HIGH", "SMAA ULTRA", "FXAA"};
            return std::string(kAa[std::clamp(g_keys.smaa ? 1 + g_keys.smaa : (g_look & 1) ? 1 : 0, 0, 6)]);
        }, command(Command::antialias));
    spin("Sharpen", "Contrast-adaptive sharpening", [] {
            static const char *const kLevels[] = {"OFF", "LOW", "MEDIUM", "HIGH"};
            return std::string(kLevels[std::clamp(g_keys.sharpen, 0, 3)]);
        }, command(Command::sharpen));
    spin("Text and menus", "The filter of 2D art and text", [] {
            static const char *const kSharp[] = {"SOFT", "SHARP", "SHARPEST"};
            return std::string(kSharp[std::clamp(g_sharp_next, 0, 2)]);
        }, command(Command::sharp_2d));
    spin("Cutscene bars", "Black bars: how opaque", [] {
            const int b = std::clamp(g_keys.black_bars, 0, 100) / 10 * 10;
            return b == 0 ? std::string("OFF") : std::to_string(b) + " %";
        }, command(Command::black_bars));
    {
        const auto get = [] { return g_keys.wet_look ? 0 : 1; };
        pick("Wet look", "The hero darkens when soaked", {"ON", "OFF"}, get, toggle_to(Command::wet_look, get));
    }
    {
        const auto get = [] { return lens_rain::level() ? 0 : 1; };
        pick("Lens rain", "Rain drops on the camera", {"ON", "OFF"}, get, toggle_to(Command::lens_rain, get));
    }
    // Sound
    spin("Master volume", "The volume of the game", [] {
            const int v = std::clamp(static_cast<int>(g_master_volume), 0, 100) / 10 * 10;
            return v == 0 ? std::string("MUTE") : std::to_string(v) + " %";
        }, command(Command::master_volume));
    {
        const auto get = [] { return ps2::reverb() ? 0 : 1; };
        pick("Reverb", "Echo of rooms and tunnels", {"ON", "OFF"}, get, toggle_to(Command::reverb, get));
    }
    // Controls
    parse_mouse_look();
    if (g_look_yaw.set) {
        const auto get = [] { return g_keys.mouse_look ? 0 : 1; };
        pick("Mouse look", "The mouse turns the camera", {"ON", "OFF"}, get, toggle_to(Command::mouse_look, get));
        if (ps2::camera_orbit_available()) {
            const auto get_orbit = [] { return g_keys.camera_orbit ? 0 : 1; };
            pick("Camera up/down", "Circles the hero, or tilts as on the PS2", {"ORBIT", "PS2"}, get_orbit, toggle_to(Command::camera_orbit, get_orbit));
        }
        if (ps2::free_camera_available()) {
            const auto get_free = [] { return g_keys.free_camera ? 1 : 0; };
            pick("Fixed cameras", "Off: your camera in every place", {"ON", "OFF"}, get_free, toggle_to(Command::free_camera, get_free));
        }
        if (ps2::head_follow_available()) {
            const auto get_head = [] { return g_keys.head_follow ? 0 : 1; };
            pick("Head follows camera", "The hero looks where you look", {"ON", "OFF"}, get_head, toggle_to(Command::head_follow, get_head));
            const auto get_anim = [] { return g_keys.anim_layers ? 0 : 1; };
            pick("Livelier hero", "Breathing, shivers, leans into turns", {"ON", "OFF"}, get_anim, toggle_to(Command::anim_layers, get_anim));
        }
        spin("Mouse speed", "How far the camera turns", [] {
                char t[16];
                std::snprintf(t, sizeof t, "%.1f", std::clamp(std::round(g_keys.look_speed * 5.0) / 5.0, 0.2, 4.0));
                return std::string(t);
            }, command(Command::mouse_speed));
        const auto get_x = [] { return (g_keys.mouse_invert & 1) ? 1 : 0; };
        pick("Mouse left/right", "The mouse's left and right", {"NORMAL", "INVERTED"}, get_x, toggle_to(Command::mouse_invert_x, get_x));
        const auto get_y = [] { return (g_keys.mouse_invert & 2) ? 1 : 0; };
        pick("Mouse up/down", "The mouse's up and down", {"NORMAL", "INVERTED"}, get_y, toggle_to(Command::mouse_invert, get_y));
    }
    if (std::getenv("PS2_PROMPT_FONT_KEY")) {
        const auto get = [] { return g_keys.key_prompts ? 0 : 1; };
        pick("Key names in texts", "Your keys in the game's texts", {"ON", "OFF"}, get, toggle_to(Command::key_prompts, get));
    }
    {
        const auto get = [] { return g_keys.keys_on_screen ? 0 : 1; };
        pick("Keys on screen", "Your keys at the left (F10)", {"ON", "OFF"}, get, toggle_to(Command::keys_on_screen, get));
    }
    // Game options: the live sections of recomp.ini (the widescreen one is above)
    for (std::size_t i = 0; i < recomp::sections().size() && rows.size() < static_cast<std::size_t>(nopt::kMaxRows); ++i) {
        const recomp::Section &sec = recomp::sections()[i];
        if (!sec.live || sec.aspect169) continue;
        std::string label = nopt::has_art("L:" + sec.label) ? sec.label : sec.name;
        std::string help = "Game option (recomp.ini)";
        if (static_cast<int>(i) == blur_section() && nopt::has_art("L:PS2 post processing"))
            label = "PS2 post processing", help = "Original PS2 post processing";   // the long name does not fit the line
        nopt::Row r;
        r.label = label, r.help = help, r.values = {"ON", "OFF"};
        r.get = [i] { return recomp::sections()[i].on ? 0 : 1; };
        r.set = [i](int v) {
            if ((recomp::sections()[i].on ? 0 : 1) == v) return;
            std::string note;
            recomp::toggle(i, note), sync_jp_textures();
            set_notice(note.empty() ? recomp::sections()[i].label + (recomp::sections()[i].on ? ": on" : ": off") : note);
        };
        rows.push_back(std::move(r));
    }
}

// rawdanger-recomp: F10 / Settings > Controls: the keys at the left of the screen while playing, kept in keys.ini
void toggle_keys_on_screen() {
    g_keys.keys_on_screen = g_keys.keys_on_screen ? 0 : 1;
    rewrite_keys_file([](const std::string &t) { return keymap::with_keys_on_screen(t, g_keys.keys_on_screen); }, "keys on screen");
    set_notice(g_keys.keys_on_screen ? "Keys on screen: on (F10 hides them)" : "Keys on screen: off (F10 shows them)");
    menu_paint();
}

void menu_command(quickmenu::Command c, int step) {
    using quickmenu::Command;
    switch (c) {
    case Command::recomp: g_recomp_note = recomp::loaded() ? "" : "Built without RawDangerPC.exe: nothing to switch"; break;
    case Command::graphics: break;  // (the menu has switched to its Graphics page)
    case Command::back: break;
    case Command::widescreen: {
        // the game's widescreen option, the same switch as on the Game options page
        bool found = false;
        for (std::size_t i = 0; i < recomp::sections().size(); ++i)
            if (recomp::sections()[i].aspect169 && recomp::sections()[i].live) {
                std::string note;
                const bool wide = ps2::g_options.widescreen;
                recomp::toggle(i, note), sync_jp_textures();
                if (wide != ps2::g_options.widescreen) { g_tex_w = 0; fit_window_to_shape(); }
                set_notice(note);
                found = true;
                break;
            }
        if (!found) set_notice("This game has no widescreen option");
        break;
    }
    case Command::antialias: {
        // off -> smooth edges (the edge blend of the looks) -> SMAA low, medium, high, ultra -> FXAA (on the picture) -> off; at once
        int aa = g_keys.smaa ? 1 + g_keys.smaa : (g_look & 1) ? 1 : 0;
        aa = (aa + step + 7) % 7;
        g_look = (g_look & ~1) | (aa == 1 ? 1 : 0);
        g_keys.look = g_look;
        g_keys.smaa = aa >= 2 ? aa - 1 : 0;
        if (g_keys.smaa) ps2::set_smaa_quality(g_keys.smaa == 5 ? 8u : static_cast<unsigned>(g_keys.smaa));
        ps2::set_video_options(video_bits());
        if (!g_look_forced) rewrite_keys_file([](const std::string &t) { return keymap::with_look(t, g_look); }, "look");
        rewrite_keys_file([](const std::string &t) { return keymap::with_smaa(t, g_keys.smaa); }, "smaa");
        static const char *const kAa[] = {"off", "smooth edges", "SMAA low", "SMAA medium", "SMAA high", "SMAA ultra", "FXAA"};
        set_notice(std::string("Anti-aliasing: ") + kAa[aa]);
        break;
    }
    case Command::window_size: {
        // presets of the picture's shape (16:9 or 4:3), each a common screen height; full screen first goes back to a window
        if (SDL_GetWindowFlags(g_window) & SDL_WINDOW_FULLSCREEN) toggle_fullscreen();
        SDL_RestoreWindow(g_window);
        static const int kHeights[] = {720, 900, 1080, 1440, 2160};
        int ww = 0, wh = 0;
        SDL_GetWindowSize(g_window, &ww, &wh);
        SDL_DisplayMode mode{};
        const int room_h = SDL_GetDesktopDisplayMode(SDL_GetWindowDisplayIndex(g_window), &mode) == 0 && mode.h > 0 ? mode.h : 1080;
        const int room_w = mode.w > 0 ? mode.w : 1920;
        std::vector<int> fit;
        for (int hh : kHeights) {
            const int w = ps2::g_options.widescreen ? hh * 16 / 9 : hh * 4 / 3;
            if (hh <= room_h - 60 && w <= room_w) fit.push_back(hh);   // leaves room for the title bar and the task bar
        }
        if (fit.empty()) fit.push_back(std::max(448, room_h - 120));
        int at = 0;
        for (std::size_t i = 0; i < fit.size(); ++i)
            if (std::abs(fit[i] - wh) < std::abs(fit[static_cast<std::size_t>(at)] - wh)) at = static_cast<int>(i);
        if (std::abs(fit[static_cast<std::size_t>(at)] - wh) > 8) step = 0;   // a size of its own: the nearest preset first
        at = (at + step + static_cast<int>(fit.size())) % static_cast<int>(fit.size());
        const int nh = fit[static_cast<std::size_t>(at)], nw = ps2::g_options.widescreen ? nh * 16 / 9 : nh * 4 / 3;
        SDL_SetWindowSize(g_window, nw, nh);
        SDL_SetWindowPosition(g_window, SDL_WINDOWPOS_CENTERED_DISPLAY(SDL_GetWindowDisplayIndex(g_window)), SDL_WINDOWPOS_CENTERED_DISPLAY(SDL_GetWindowDisplayIndex(g_window)));
        g_keys.window_w = nw;
        g_keys.window_h = nh;
        rewrite_keys_file([nw, nh](const std::string &t) { return keymap::with_window_size(t, nw, nh); }, "window size");
        set_notice("Window: " + std::to_string(nw) + " x " + std::to_string(nh));
        break;
    }
    case Command::game_blur: {
        const int blur = blur_section();
        if (blur < 0) break;
        recomp::toggle(static_cast<std::size_t>(blur), g_recomp_note), sync_jp_textures();
        set_notice(post_filter_on() ? "PS2 post-processing filter: on (as on the console)" : "PS2 post-processing filter: off");
        break;
    }
    case Command::mouse_look:
        parse_mouse_look();
        if (!g_look_yaw.set) break;
        g_keys.mouse_look = g_keys.mouse_look ? 0 : 1;
        rewrite_keys_file([](const std::string &t) { return keymap::with_mouse_look(t, g_keys.mouse_look); }, "mouse look");
        set_notice(g_keys.mouse_look ? "Mouse look on: the mouse turns the camera while you play" : "Mouse look off");
        break;
    case Command::free_camera:
        if (!ps2::free_camera_available()) break;
        g_keys.free_camera = g_keys.free_camera ? 0 : 1;
        ps2::set_free_camera(g_keys.free_camera != 0);
        rewrite_keys_file([](const std::string &t) { return keymap::with_free_camera(t, g_keys.free_camera); }, "fixed cameras");
        set_notice(g_keys.free_camera ? "Fixed cameras off: the follow camera in every place (from the next place that has one)" : "Fixed cameras on: as on the PS2");
        break;
    case Command::anim_layers:
        if (!ps2::head_follow_available()) break;
        g_keys.anim_layers = g_keys.anim_layers ? 0 : 1;
        ps2::set_anim_layers(g_keys.anim_layers != 0);
        rewrite_keys_file([](const std::string &t) { return keymap::with_anim_layers(t, g_keys.anim_layers); }, "livelier hero");
        set_notice(g_keys.anim_layers ? "Livelier hero: on (breathing, a hunch when soaked, shivers when cold, leans into turns)" : "Livelier hero: off (the game's animation as it is)");
        break;
    case Command::wet_look:
        g_keys.wet_look = g_keys.wet_look ? 0 : 1;
        ps2::set_wet_look_on(g_keys.wet_look != 0);
        rewrite_keys_file([](const std::string &t) { return keymap::with_wet_look(t, g_keys.wet_look); }, "wet look");
        set_notice(g_keys.wet_look ? "Wet look: on (the hero darkens as the clothes get wet)" : "Wet look: off");
        break;
    case Command::head_follow:
        if (!ps2::head_follow_available()) break;
        g_keys.head_follow = g_keys.head_follow ? 0 : 1;
        ps2::set_head_follow(g_keys.head_follow != 0);
        rewrite_keys_file([](const std::string &t) { return keymap::with_head_follow(t, g_keys.head_follow); }, "head follows camera");
        set_notice(g_keys.head_follow ? "Head follows camera: on" : "Head follows camera: off (as on the PS2)");
        break;
    case Command::camera_orbit:
        if (!ps2::camera_orbit_available()) break;
        g_keys.camera_orbit = g_keys.camera_orbit ? 0 : 1;
        ps2::set_camera_orbit(g_keys.camera_orbit != 0);
        rewrite_keys_file([](const std::string &t) { return keymap::with_camera_orbit(t, g_keys.camera_orbit); }, "camera up / down");
        set_notice(g_keys.camera_orbit ? "Camera up / down: the camera circles the hero" : "Camera up / down: as on the PS2 (the camera tilts)");
        break;
    case Command::mouse_speed: {
        // 0.2 to 4 in steps of 0.2 (Left / Right), round at the ends
        double v = std::round(g_keys.look_speed * 5.0) / 5.0 + (step < 0 ? -0.2 : 0.2);
        if (v > 4.01) v = 0.2;
        if (v < 0.19) v = 4.0;
        g_keys.look_speed = v;
        rewrite_keys_file([](const std::string &t) { return keymap::with_mouse_speed(t, g_keys.look_speed); }, "mouse look speed");
        break;
    }
    case Command::mouse_invert_x:
        g_keys.mouse_invert ^= 1;
        rewrite_keys_file([](const std::string &t) { return keymap::with_mouse_invert(t, g_keys.mouse_invert); }, "mouse left and right");
        break;
    case Command::mouse_invert:
        g_keys.mouse_invert ^= 2;
        rewrite_keys_file([](const std::string &t) { return keymap::with_mouse_invert(t, g_keys.mouse_invert); }, "mouse up and down");
        break;
    case Command::sharpen: {
        g_keys.sharpen = (g_keys.sharpen + step + 4) % 4;
        ps2::set_video_options(video_bits());
        rewrite_keys_file([](const std::string &t) { return keymap::with_sharpen(t, g_keys.sharpen); }, "sharpen");
        static const char *const kLevels[] = {"off", "low", "medium", "high"};
        set_notice(std::string("Sharpen: ") + kLevels[g_keys.sharpen]);
        break;
    }
    case Command::black_bars:
        // 100 % (as the game draws them) down to 0 (left out) in steps of 10
        g_keys.black_bars = (std::clamp(g_keys.black_bars, 0, 100) / 10 * 10 + step * 10 + 110) % 110;
        ps2::set_bar_opacity(static_cast<unsigned>(g_keys.black_bars));
        rewrite_keys_file([](const std::string &t) { return keymap::with_black_bars(t, g_keys.black_bars); }, "cutscene bars");
        set_notice(g_keys.black_bars ? "Cutscene black bars: " + std::to_string(g_keys.black_bars) + " % opaque" : std::string("Cutscene black bars removed"));
        break;
    case Command::key_prompts:
        if (!std::getenv("PS2_PROMPT_FONT_KEY")) break;
        g_keys.key_prompts = g_keys.key_prompts ? 0 : 1;
        send_key_prompts();
        rewrite_keys_file([](const std::string &t) { return keymap::with_key_prompts(t, g_keys.key_prompts); }, "key pictures");
        set_notice(g_keys.key_prompts ? "The game's texts show your keys" : "The game's texts show the PlayStation symbols");
        break;
    case Command::reverb:
        g_keys.reverb = ps2::reverb() ? 0 : 1;
        ps2::set_reverb(g_keys.reverb != 0);
        rewrite_keys_file([](const std::string &t) { return keymap::with_reverb(t, g_keys.reverb); }, "reverb");
        set_notice(g_keys.reverb ? "Reverb: on (rooms and tunnels echo)" : "Reverb: off (dry sound)");
        break;
    case Command::menu_skin:
        // hazard -> Japanese -> the game's Options look (when the game's font is there) -> hazard
        g_keys.menu_skin = (quickmenu::skin() + 1) % (quickmenu::game_font_loaded() ? 3 : 2);
        quickmenu::set_skin(g_keys.menu_skin);
        g_skin_chosen = g_keys.menu_skin;
        rewrite_keys_file([](const std::string &t) { return keymap::with_menu_skin(t, g_keys.menu_skin); }, "menu look");
        set_notice(g_keys.menu_skin == 2 ? "Menu look: Raw Danger's Options screen" : g_keys.menu_skin ? "Menu look: Japanese (blue on white)" : "Menu look: hazard (yellow on black)");
        break;
    case Command::integer_scale:
        g_keys.integer_scale = g_keys.integer_scale ? 0 : 1;
        ps2::set_video_options(video_bits());
        rewrite_keys_file([](const std::string &t) { return keymap::with_integer_scale(t, g_keys.integer_scale); }, "whole-number scaling");
        set_notice(std::string("Whole-number scaling: ") + (g_keys.integer_scale ? "on (every console row the same height)" : "off (fills the window)"));
        break;
    case Command::lens_rain: {
        const int level = lens_rain::level() ? 0 : 1;
        lens_rain::set_level(level);
        g_keys.lens_rain = level;
        rewrite_keys_file([level](const std::string &t) { return keymap::with_lens_rain(t, level); }, "rain on the lens");
        set_notice(level ? "Rain on the lens: on (as hard as it rains)" : "Rain on the lens: off");
        break;
    }
    case Command::master_volume: {
        // left / right (and Enter: up): 10 % steps
        const int v = std::clamp((g_master_volume + 10 * step + (step < 0 ? 9 : 0)) / 10 * 10, 0, 100);
        g_master_volume = v;
        g_keys.master_volume = v;
        rewrite_keys_file([v](const std::string &t) { return keymap::with_master_volume(t, v); }, "master volume");
        set_notice(v == 0 ? std::string("Master volume: muted") : "Master volume: " + std::to_string(v) + "%");
        break;
    }
    case Command::field_of_view: {
        // v0.10.4: degrees across (the game's own first, then 45 to 110 in fives), round at the ends
        int at = 0;
        for (int k = 0; k < keymap::kFieldsOfViewCount; ++k)
            if (keymap::kFieldsOfView[k] == g_keys.field_of_view) at = k;
        at = (at + (step < 0 ? keymap::kFieldsOfViewCount - 1 : 1)) % keymap::kFieldsOfViewCount;
        g_keys.field_of_view = keymap::kFieldsOfView[at];
        rewrite_keys_file([](const std::string &t) { return keymap::with_field_of_view(t, g_keys.field_of_view); }, "field of view");
        const int game = keymap::game_field_of_view(ps2::g_options.widescreen);
        set_notice(g_keys.field_of_view <= 0 ? "Field of view: " + std::to_string(game) + " degrees across (as on the PS2)"
                                             : "Field of view: " + std::to_string(g_keys.field_of_view) + " degrees across");
        break;
    }
    case Command::ambient_occlusion: {
        static const char *const kLevels[] = {"off", "light", "strong"};
        g_keys.ambient_occlusion = (g_keys.ambient_occlusion + (step < 0 ? 2 : 1)) % 3;
        ps2::set_ambient_occlusion(g_keys.ambient_occlusion);
        rewrite_keys_file([](const std::string &t) { return keymap::with_ambient_occlusion(t, g_keys.ambient_occlusion); }, "ambient occlusion");
        set_notice(std::string("Ambient occlusion: ") + kLevels[g_keys.ambient_occlusion] + (g_keys.ambient_occlusion ? " (with the post processing filter on)" : ""));
        break;
    }
    case Command::filter_pass:
        g_keys.filter_pass = ps2::game_filter_pass() ? 0 : 1;
        ps2::set_game_filter_pass(g_keys.filter_pass);
        rewrite_keys_file([](const std::string &t) { return keymap::with_filter_pass(t, g_keys.filter_pass); }, "post processing");
        set_notice(g_keys.filter_pass ? "Post processing: one GPU pass" : "Post processing: the game's own draws (slow; to compare)");
        break;
    case Command::popup_size:
        g_keys.popup_size = (g_keys.popup_size + (step < 0 ? 3 : 1)) % 4;
        quickmenu::set_popup_size(g_keys.popup_size);
        rewrite_keys_file([](const std::string &t) { return keymap::with_popup_size(t, g_keys.popup_size); }, "pop-up size");
        {
            static const char *const kSizes[] = {"small", "normal", "large", "huge"};
            set_notice(std::string("Pop-up size: ") + kSizes[g_keys.popup_size]);
        }
        break;
    case Command::keys_on_screen:
        toggle_keys_on_screen();
        break;
    case Command::menu_game_runs:
        g_keys.menu_game_runs = g_keys.menu_game_runs ? 0 : 1;
        rewrite_keys_file([](const std::string &t) { return keymap::with_menu_game_runs(t, g_keys.menu_game_runs); }, "game runs behind the menu");
        set_notice(std::string("Game runs behind the F1 menu: ") + (g_keys.menu_game_runs ? "on" : "off (it waits)"));
        break;
    case Command::pause_unfocused:
        g_keys.pause_unfocused = g_keys.pause_unfocused ? 0 : 1;
        rewrite_keys_file([](const std::string &t) { return keymap::with_pause_unfocused(t, g_keys.pause_unfocused); }, "pause in the background");
        set_notice(std::string("Pause in the background: ") + (g_keys.pause_unfocused ? "on (the menu opens when the window goes to the back)" : "off"));
        break;
    case Command::achievements: g_recomp_note.clear(); break;
    case Command::achievement_toggle: achievements::toggle(static_cast<std::size_t>(g_menu.recomp_row()), g_recomp_note); break;
    case Command::recomp_toggle: {
        const bool wide = ps2::g_options.widescreen;
        const std::size_t row = static_cast<std::size_t>(g_menu.recomp_row());
        if (row < g_recomp_row_section.size() && g_recomp_row_section[row] == kHealRow) {
            g_recomp_note = heal_hero() ? "The hero's health and body temperature are full" : "No hero to heal here (only while playing)";
            break;
        }
        if (row < g_recomp_row_section.size() && g_recomp_row_section[row] == kKeepRow) {
            g_keep_hero = !g_keep_hero;
            g_recomp_note = g_keep_hero ? "The hero stays healthy and warm (until the game ends or you switch it off)" : "The hero's health and warmth are the game's again";
            break;
        }
        if (row < g_recomp_row_section.size() && g_recomp_row_section[row] == kViewRow) {
            static const char *const kViews[] = {"Picture: the game's", "Picture: the ambient occlusion alone (white: none)", "Picture: the depth (near white, far dark)",
                                                 "Picture: the wet look's capsules in blue"};
            const int k = (ps2::debug_view() + (step < 0 ? 3 : 1)) % 4;
            ps2::set_debug_view(k);
            g_recomp_note = kViews[k];
            break;
        }
        if (row < g_recomp_row_section.size() && g_recomp_row_section[row] == kStepsRow) {
            // the game's, 20, 30, 60 (vblanks a step 3, 2, 1)
            static const int kOrder[] = {0, 3, 2, 1};
            static std::uint32_t own = 0;   // v0.10.3: the build's own vblanks a step (3, or 2 / 1 in the 30 / 60 builds), put back after the test
            if (g_force_vblanks == 0 && debug_steps().set) {
                const std::uint32_t g = ps2::ld32(debug_steps().pointer);
                if (g >= 0x100000u && g < 0x2000000u) own = ps2::ld32(g + debug_steps().offset);
            }
            int k = 0;
            while (k < 4 && kOrder[k] != g_force_vblanks) ++k;
            k = ((k == 4 ? 0 : k) + (step < 0 ? 3 : 1)) % 4;
            g_force_vblanks = kOrder[k];
            if (g_force_vblanks == 0 && debug_steps().set) {
                const std::uint32_t g = ps2::ld32(debug_steps().pointer);
                if (g >= 0x100000u && g < 0x2000000u) ps2::st32(g + debug_steps().offset, own >= 1u && own <= 3u ? own : 3u);   // the build's own
            }
            g_recomp_note = g_force_vblanks == 0 ? "Game steps: the build's own (the launcher's Frame rate)"
                                                 : "Game steps: " + std::to_string(60 / g_force_vblanks) + " a second: a fixed-step game, everything runs faster (a test)";
            break;
        }
        if (row < g_recomp_row_section.size() && g_recomp_row_section[row] == kAnimGainRow) {
            ps2::set_anim_gain(ps2::anim_gain() > 1.5f ? 1.0f : 4.0f);
            g_recomp_note = ps2::anim_gain() > 1.5f ? "Livelier hero: the layers four times bigger (a test)" : "Livelier hero: the layers as made";
            break;
        }
        if (row < g_recomp_row_section.size() && g_recomp_row_section[row] == kStatsRow) {
            g_stats_on = !g_stats_on;
            g_stats_rows = {{"Stats", "in a second"}};
            g_recomp_note = g_stats_on ? "Stats on screen, at the left (in place of the keys)" : "Stats off";
            break;
        }
        if (row < g_recomp_row_section.size() && g_recomp_row_section[row] == kDryRow) {
            g_recomp_note = ps2::set_hero_wetness(0.0f) ? "The hero's clothes are dry" : "No hero here (only while playing)";
            break;
        }
        if (row < g_recomp_row_section.size() && g_recomp_row_section[row] == kSoakRow) {
            g_recomp_note = ps2::set_hero_wetness(1.0f) ? "The hero's clothes are soaked" : "No hero here (only while playing)";
            break;
        }
        if (row < g_recomp_row_section.size() && g_recomp_row_section[row] == kWetForceRow) {
            // the game's, 0, 25, 50, 75, 100 %
            const float now = ps2::wet_force();
            int k = now < 0.0f ? 0 : 1 + static_cast<int>(std::lround(now * 4.0f));
            k = (k + (step < 0 ? 5 : 1)) % 6;
            ps2::set_wet_force(k == 0 ? -1.0f : static_cast<float>(k - 1) / 4.0f);
            g_recomp_note = k == 0 ? "Wet look: the game's wetness" : "Wet look: shown as " + std::to_string((k - 1) * 25) + " % wet (a test; the game's own wetness is unchanged)";
            break;
        }
        if (row < g_recomp_row_section.size() && g_recomp_row_section[row] == kWetShowRow) {
            ps2::set_wet_show(!ps2::wet_show());
            g_recomp_note = ps2::wet_show() ? "The wet look's capsules are drawn in blue round the hero" : "The wet look's capsules are hidden";
            break;
        }
        if (row < g_recomp_row_section.size() && g_recomp_row_section[row] == kHeadTestRow) {
            // off, -60, -30, 30, 60 degrees
            static const float kTurns[] = {0.0f, -1.047f, -0.524f, 0.524f, 1.047f};
            int k = 0;
            while (k < 5 && std::fabs(kTurns[k] - ps2::head_test()) > 0.01f) ++k;
            k = ((k == 5 ? 0 : k) + (step < 0 ? 4 : 1)) % 5;
            ps2::set_head_test(kTurns[k]);
            g_recomp_note = k == 0 ? "The head follows the camera again" : "The head is held turned (a test)";
            break;
        }
        if (row < g_recomp_row_section.size() && g_recomp_row_section[row] == kInfoRow) {
            const std::string info = debug_info_text();
            std::fprintf(stderr, "[where] %s\n", info.c_str());
            g_recomp_note = info;
            break;
        }
        if (row < g_recomp_row_section.size() && (g_recomp_row_section[row] == kDebugRow || g_recomp_row_section[row] == kMapSelRow)) {
            open_debug_scene(g_recomp_row_section[row] == kMapSelRow);
            return;
        }
        if (row < g_recomp_row_section.size()) recomp::toggle(g_recomp_row_section[row], g_recomp_note), sync_jp_textures();
        if (wide != ps2::g_options.widescreen) { g_tex_w = 0; fit_window_to_shape(); }  // the SDL picture takes the new shape at the next frame
        break;
    }
    case Command::none: menu_paint(); return;  // (Quit asked once: the entry shows the question)
    case Command::resume: close_menu("Resume"); return;
    case Command::save:
        close_menu("Quick save");
        g_note_wanted = true;  // the answer ("Saved") is shown over the picture
        ps2::quick_request(ps2::QuickKey::save);
        return;
    case Command::load:
        close_menu("Quick load");
        g_note_wanted = true;
        ps2::quick_request(ps2::QuickKey::load);
        return;
    case Command::look: cycle_look(step); break;
    case Command::scale:
        if (g_scale_locked) {
            set_notice("The resolution is set by PS2_SCALE; remove it to choose here");
            break;
        }
        {
            // 1x to 6x, 8x, auto (the one at least as tall as the screen), round; it changes at once (ps2::set_internal_scale)
            const int order[8] = {1, 2, 3, 4, 5, 6, 8, keymap::kScaleAuto};
            int at = 0;
            for (int i = 0; i < 8; ++i) if (order[i] == g_scale_next) at = i;
            g_scale_next = order[(at + step + 8) % 8];
        }
        g_keys.scale = g_scale_next;
        rewrite_keys_file([](const std::string &t) { return keymap::with_scale(t, g_scale_next); }, "resolution");
        {
            int use = g_scale_next;
            if (use == keymap::kScaleAuto) {
                SDL_DisplayMode mode;
                use = SDL_GetDesktopDisplayMode(0, &mode) == 0 && mode.h > 0 ? std::clamp((mode.h + 447) / 448, 1, 6) : 2;
            }
            const int most = static_cast<int>(ps2::max_internal_scale());
            const bool held = use > most;
            if (held) use = most;   // (gpu.cpp holds it too; said here so the player knows why)
            ps2::set_internal_scale(static_cast<unsigned>(use));
            g_scale_now = g_scale_next;
            set_notice("Internal resolution " + std::to_string(640 * use) + " x " + std::to_string(448 * use) + (g_scale_next == keymap::kScaleAuto ? " (auto)" : "") +
                       (held ? std::string(" (the most ") + ps2::graphics_api() + " holds; Vulkan allows more)" : std::string()));
            std::fprintf(stderr, "[menu] resolution %dx, now\n", use);
        }
        break;
    case Command::graphics_api: {
        g_keys.graphics_api = (g_keys.graphics_api + step + 3) % 3;
        rewrite_keys_file([](const std::string &t) { return keymap::with_graphics_api(t, g_keys.graphics_api); }, "graphics API");
        static const char *const kNames[] = {"auto", "Vulkan", "Direct3D 12"};
        set_notice(std::string("Graphics API: ") + kNames[g_keys.graphics_api] + " from the next start (now " + ps2::graphics_api() + ")");
        std::fprintf(stderr, "[menu] graphics API %s at the next start\n", kNames[g_keys.graphics_api]);
        break;
    }
    case Command::sharp_2d:
        if (g_sharp_locked) {
            set_notice("Text and menus are set by PS2_SHARP_2D; remove it to choose here");
            break;
        }
        g_sharp_next = (g_sharp_next + step + 3) % 3;
        g_keys.sharp_2d = g_sharp_next;
        rewrite_keys_file([](const std::string &t) { return keymap::with_sharp_2d(t, g_sharp_next); }, "2D art filter");
        ps2::set_sharp_2d(static_cast<unsigned>(g_sharp_next));   // at once (the programs are made again)
        g_sharp_now = g_sharp_next;
        set_notice(std::string("Text and menus: ") + keymap::sharp_2d_word(g_sharp_next));
        std::fprintf(stderr, "[menu] 2D art %s, now\n", keymap::sharp_2d_word(g_sharp_next));
        break;
    case Command::fullscreen: toggle_fullscreen(); break;
    case Command::change_keys: g_key_note.clear(); break;  // (the menu has switched to its keys page)
    case Command::key_edit: g_key_note.clear(); break;     // (the highlighted line waits for a key now)
    case Command::key_defaults: {
        std::vector<int> changed;
        keymap::restore_default_inputs(g_keys, changed);
        write_key_lines(changed);
        g_key_note = changed.empty() ? "These are the default keys already." : "The default keys are back.";
        std::fprintf(stderr, "[menu] the default keys are back (%zu button%s changed)\n", changed.size(), changed.size() == 1 ? "" : "s");
        break;
    }
    case Command::keys:
        // the menu stays open behind the card (the game keeps holding); the card's own buttons close it when they save or load
        show_key_card("Keys in the quick menu");
        break;
    case Command::language: {
        // rawdanger-recomp: the launcher started this game with PS2_LANGUAGE_SWITCH_FILE; the file tells it, once the game has
        // ended, to carry the saves over to the other build's memory card and start that one (tools\rd_card.py)
        const char *file = std::getenv("PS2_LANGUAGE_SWITCH_FILE");
        const char *other = std::getenv("PS2_LANGUAGE_OTHER");
        if (!file || !*file || !other) { menu_paint(); return; }
        if (std::FILE *f = std::fopen(file, "wb")) {
            std::fprintf(f, "%s\n", other);
            std::fclose(f);
        }
        std::fprintf(stderr, "[menu] Language: restart in %s; the memory card is written, the game ends\n", other);
        ps2::flush_memory_card();
        ps2::halt("language switch from the quick menu");
        close_menu("Language");
        return;
    }
    case Command::quit:
        std::fprintf(stderr, "[menu] Quit to desktop: the memory card is written, the game ends\n");
        ps2::flush_memory_card();  // right away, as when the window is closed
        ps2::halt("quit from the quick menu");
        close_menu("Quit to desktop");  // a game that waits for the menu must go on to see the halt
        return;
    }
    menu_paint();
}

bool is_help_key(SDL_Scancode k) {
    const std::vector<int> &keys = g_keys.inputs[keymap::Help];
    return std::find(keys.begin(), keys.end(), static_cast<int>(k)) != keys.end();
}

bool is_menu_key(SDL_Scancode k) {
    const std::vector<int> &keys = g_keys.inputs[keymap::Menu];
    return k == SDL_SCANCODE_F1 || std::find(keys.begin(), keys.end(), static_cast<int>(k)) != keys.end();
}

// the pointer in window pixels (events can be in the SDL renderer's logical units; the mouse state is not)
// rawdanger-recomp: a click on Settings' tab row
bool menu_pointer_tab() {
    int x = 0, y = 0, ww = 1, wh = 1, pw = 0, ph = 0;
    SDL_GetMouseState(&x, &y);
    SDL_GetWindowSize(g_window, &ww, &wh);
    window_pixels(pw, ph);
    return g_menu.click_tab(x * pw / std::max(ww, 1), y * ph / std::max(wh, 1));
}

bool menu_pointer_select() {
    int x = 0, y = 0, ww = 1, wh = 1, pw = 0, ph = 0;
    SDL_GetMouseState(&x, &y);
    SDL_GetWindowSize(g_window, &ww, &wh);
    window_pixels(pw, ph);
    return g_menu.select_at(x * pw / std::max(ww, 1), y * ph / std::max(wh, 1));
}

// Keys, buttons and the mouse for the menu. True when the event was the menu's (while it is open every key and button is).
bool menu_event(const SDL_Event &e) {
    using quickmenu::Command;
    if (!g_menu.is_open()) {
        if (e.type == SDL_KEYDOWN && !e.key.repeat && is_menu_key(e.key.keysym.scancode)) {
            open_menu((std::string("key ") + SDL_GetScancodeName(e.key.keysym.scancode)).c_str());
            return true;
        }
        if (e.type == SDL_CONTROLLERBUTTONDOWN) {
            SDL_GameController *pad = SDL_GameControllerFromInstanceID(e.cbutton.which);
            const int b = e.cbutton.button;
            const bool both = pad && ((b == SDL_CONTROLLER_BUTTON_START && SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_BACK)) ||
                                      (b == SDL_CONTROLLER_BUTTON_BACK && SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_START)));
            if (b == SDL_CONTROLLER_BUTTON_GUIDE || both) {
                open_menu(b == SDL_CONTROLLER_BUTTON_GUIDE ? "the controller's Guide button" : "Select + Start on the controller");
                return true;
            }
        }
        return false;
    }
    const auto side = [](int step) {
        const quickmenu::Command c = g_menu.side();
        // the developers' menus are opened with Enter only (left / right on their lines does nothing)
        const std::size_t row = static_cast<std::size_t>(g_menu.recomp_row());
        if (c == Command::recomp_toggle && row < g_recomp_row_section.size() &&
            (g_recomp_row_section[row] == kDebugRow || g_recomp_row_section[row] == kMapSelRow || g_recomp_row_section[row] == kInfoRow ||
             g_recomp_row_section[row] == kHealRow || g_recomp_row_section[row] == kDryRow || g_recomp_row_section[row] == kSoakRow)) return;
        menu_command(c, step);
    };
    const auto move = [](int step) {
        g_menu.move(step);
        menu_paint();
    };
    // Esc, Circle, the right mouse button: one page back on the keys page, else the menu closes
    const auto back = [](const char *why) {
        if (g_menu.back()) menu_paint();
        else close_menu(why);
    };
    if (g_menu.capturing()) return capture_event(e);
    switch (e.type) {
    case SDL_KEYDOWN: {
        const SDL_Scancode k = e.key.keysym.scancode;
        if (!e.key.repeat && is_menu_key(k)) {
            close_menu("its key");
            return true;
        }
        switch (k) {
        case SDL_SCANCODE_UP: case SDL_SCANCODE_W: move(-1); break;
        case SDL_SCANCODE_DOWN: case SDL_SCANCODE_S: move(1); break;
        case SDL_SCANCODE_LEFT: case SDL_SCANCODE_A: side(-1); break;
        case SDL_SCANCODE_RIGHT: case SDL_SCANCODE_D: side(1); break;
        case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: case SDL_SCANCODE_SPACE:
            if (!e.key.repeat) menu_command(g_menu.choose(), 1);
            break;
        // rawdanger-recomp: Settings' tabs
        case SDL_SCANCODE_TAB: case SDL_SCANCODE_Q: case SDL_SCANCODE_E: case SDL_SCANCODE_PAGEUP: case SDL_SCANCODE_PAGEDOWN:
            if (g_menu.on_settings_page()) {
                const bool back_tab = k == SDL_SCANCODE_Q || k == SDL_SCANCODE_PAGEUP || (k == SDL_SCANCODE_TAB && (SDL_GetModState() & KMOD_SHIFT));
                g_menu.tab(back_tab ? -1 : 1);
                menu_paint();
            }
            break;
        case SDL_SCANCODE_ESCAPE: case SDL_SCANCODE_BACKSPACE:
            if (!e.key.repeat) back(k == SDL_SCANCODE_ESCAPE ? "Esc" : "Backspace");
            break;
        // the program's keys keep working; those that need the game running close the menu first
        case SDL_SCANCODE_F2: if (!e.key.repeat) menu_command(Command::look, 1); break;
        case SDL_SCANCODE_F5: if (!e.key.repeat) menu_command(Command::save, 1); break;
        case SDL_SCANCODE_F8: if (!e.key.repeat) menu_command(Command::load, 1); break;
        case SDL_SCANCODE_F9:
            if (!e.key.repeat) {
                close_menu("F9");
                ps2::quick_request(ps2::QuickKey::report);
            }
            break;
        case SDL_SCANCODE_F11: if (!e.key.repeat) menu_command(Command::fullscreen, 1); break;
        default:
            if (!e.key.repeat && is_help_key(k)) menu_command(Command::keys, 1);
            break;
        }
        return true;
    }
    case SDL_KEYUP: return true;
    case SDL_CONTROLLERBUTTONDOWN:
        switch (e.cbutton.button) {
        case SDL_CONTROLLER_BUTTON_DPAD_UP: move(-1); break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: move(1); break;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT: side(-1); break;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: side(1); break;
        case SDL_CONTROLLER_BUTTON_A: menu_command(g_menu.choose(), 1); break;  // Cross (the bottom button)
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:   // L1 / R1: Settings' tabs
            if (g_menu.on_settings_page()) {
                g_menu.tab(e.cbutton.button == SDL_CONTROLLER_BUTTON_LEFTSHOULDER ? -1 : 1);
                menu_paint();
            }
            break;
        case SDL_CONTROLLER_BUTTON_B: back("Circle"); break;              // Circle (the right one)
        case SDL_CONTROLLER_BUTTON_GUIDE: close_menu("the Guide button"); break;
        case SDL_CONTROLLER_BUTTON_START: close_menu("Start"); break;
        default: break;
        }
        return true;
    case SDL_CONTROLLERBUTTONUP: return true;
    case SDL_CONTROLLERAXISMOTION:
        if (e.caxis.axis == SDL_CONTROLLER_AXIS_LEFTY) {
            // the left stick moves the highlight once per push
            int &was = g_stick_step[g_pads[1] && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(g_pads[1])) == e.caxis.which ? 1 : 0];
            const int now = e.caxis.value < -20000 ? -1 : e.caxis.value > 20000 ? 1 : (std::abs(e.caxis.value) < 8000 ? 0 : was);
            if (now && now != was) move(now);
            was = now;
        }
        return true;
    case SDL_MOUSEMOTION:
        if ((e.motion.xrel || e.motion.yrel) && menu_pointer_select()) menu_paint();  // only a pointer that moved (not one that the window moved under)
        return true;
    case SDL_MOUSEBUTTONDOWN:
        if (e.button.button == SDL_BUTTON_LEFT && menu_pointer_tab()) menu_paint();
        else if (e.button.button == SDL_BUTTON_LEFT && menu_pointer_select()) {
            // rawdanger-recomp: a click on a line's "<" side steps back, on its ">" side forward; elsewhere the line is chosen
            int x = 0, y = 0, ww = 1, wh = 1, pw = 0, ph = 0;
            SDL_GetMouseState(&x, &y);
            SDL_GetWindowSize(g_window, &ww, &wh);
            window_pixels(pw, ph);
            const int step = g_menu.click_step(x * pw / std::max(ww, 1));
            if (step != 0 && g_menu.side() != Command::none) menu_command(g_menu.side(), step);
            else menu_command(g_menu.choose(), 1);
        }
        else if (e.button.button == SDL_BUTTON_RIGHT) back("the right mouse button");
        return true;
    case SDL_MOUSEBUTTONUP: return true;
    case SDL_MOUSEWHEEL:
        if (e.wheel.y) move(e.wheel.y > 0 ? -1 : 1);
        return true;
    default: return false;
    }
}

void show_notice() {
    ps2::QuickNotice n;
    if (ps2::quick_notice(n)) {
        g_notice_text = n.text;
        g_notice_until = SDL_GetTicks() + (n.kind == 0 ? 5000u : 6000u);
        if (g_note_wanted && n.kind > 0) {  // the answer to a save or load that the menu asked for: shown over the picture for a moment
            g_note_wanted = false;
            if (!g_menu.is_open()) {
                g_note_text = n.text;
                g_toast = false;
                g_note_until = SDL_GetTicks() + 2500;
                menu_paint(false);
            }
        }
        if (n.kind == 2 && g_pipe) g_pipe->flush();  // the sound from before the load (the tone comes on top of the game's sound, so it stays)
        if (g_beep_on && n.kind > 0) g_beep_request = n.kind;
        refresh_title();
    } else if (!g_notice_text.empty() && static_cast<Sint32>(SDL_GetTicks() - g_notice_until) >= 0) {
        g_notice_text.clear();
        refresh_title();
    }
    if (!g_note_text.empty() && static_cast<Sint32>(SDL_GetTicks() - g_note_until) >= 0) {
        g_note_text.clear();
        g_toast = false;
        if (!g_menu.is_open()) menu_paint(false);
    }
    // rawdanger-recomp: an achievement just unlocked
    if (const std::string a = achievements::take_note(); !a.empty() && !g_menu.is_open()) {
        g_note_text = a;
        g_note_until = SDL_GetTicks() + 6000;
        // the pop-up: the achievement's picture (a .bmp RawDangerPC.exe made) beside "ACHIEVEMENT UNLOCKED" and its name
        g_toast = true;
        g_toast_title = a.substr(a.find(':') == std::string::npos ? 0 : a.find(':') + 2);
        g_toast_icon.clear();
        g_toast_w = g_toast_h = 0;
        if (const std::string icon = achievements::take_note_icon(); !icon.empty()) {
            if (SDL_Surface *bmp = SDL_LoadBMP(icon.c_str())) {
                if (SDL_Surface *rgba = SDL_ConvertSurfaceFormat(bmp, SDL_PIXELFORMAT_ABGR8888, 0)) {
                    g_toast_w = rgba->w;
                    g_toast_h = rgba->h;
                    g_toast_icon.resize(static_cast<std::size_t>(g_toast_w) * g_toast_h);
                    for (int y = 0; y < g_toast_h; ++y)
                        std::memcpy(g_toast_icon.data() + static_cast<std::size_t>(y) * g_toast_w, static_cast<const std::uint8_t *>(rgba->pixels) + y * rgba->pitch,
                                    static_cast<std::size_t>(g_toast_w) * 4);
                    SDL_FreeSurface(rgba);
                }
                SDL_FreeSurface(bmp);
            } else {
                std::fprintf(stderr, "[achievements] no picture %s (%s)\n", icon.c_str(), SDL_GetError());
            }
        }
        menu_paint(false);
    }
}

// PS2_HITCH_LOG=<ms>: SDL_PollEvent also pumps the system's messages and looks for controllers; say when it, or the handling of
// one event, took long (docs/hitches.md).
bool timed_poll_event(SDL_Event &e) {
    window_stage("asking the system for window events (SDL_PollEvent)");
    if (!ps2::hitch::on()) return SDL_PollEvent(&e) != 0;
    const auto t0 = ps2::hitch::Clock::now();
    const bool got = SDL_PollEvent(&e) != 0;
    const double ms = ps2::hitch::ms_since(t0);
    if (ms > ps2::hitch::threshold_ms() / 3)
        std::fprintf(stderr, "[hitch] SDL_PollEvent took %.1f ms (it returned %s, event type 0x%x)%s", ms, got ? "an event" : "nothing", got ? e.type : 0u, "\n");
    return got;
}

struct EventTimer {
    Uint32 type;
    ps2::hitch::Clock::time_point t0 = ps2::hitch::on() ? ps2::hitch::Clock::now() : ps2::hitch::Clock::time_point{};
    ~EventTimer() {
        if (!ps2::hitch::on()) return;
        const double ms = ps2::hitch::ms_since(t0);
        if (ms > ps2::hitch::threshold_ms() / 3) std::fprintf(stderr, "[hitch] handling the event of type 0x%x took %.1f ms%s", type, ms, "\n");
    }
};

void poll() {
    window_stage("window thread: starting a pass of poll()");
    if (!g_script.empty()) play_pad_script();
    ps2::quick_watchdog();
    take_key_card_answer();
    keep_scene_request();
    if (debug_scenes().set) {   // rawdanger-recomp: the developers' scenes' text at its own size (gs.cpp, sjis)
        const DebugScenes &d = debug_scenes();
        const std::uint32_t scene = ps2::ld32(d.address - 4);
        static bool was = false;
        const bool on = (d.mapsel && scene == d.mapsel) || (d.debug && scene == d.debug);
        if (on != was) ps2::set_debug_text(was = on);
        const bool menu = d.debug && scene == d.debug;
        static int shown_cursor = -1;
        if (menu && !g_debug_menu_on.load()) g_debug_cursor.store(0);   // the menu starts on its first line
        if (menu != g_debug_menu_on.load() || (menu && shown_cursor != g_debug_cursor.load())) {
            g_debug_menu_on.store(menu);
            shown_cursor = menu ? g_debug_cursor.load() : -1;
            menu_paint();   // the panel drawn again (or taken away)
        }
    }
    native_option_rows();
    nopt::frame();
    // (black for a second more after it: the game's picture still holds its last screen until the next scene draws over it)
    static Uint32 skipped_until = 0;
    if (in_skipped_scene()) skipped_until = SDL_GetTicks() + 1000;
    if (const bool blank = static_cast<Sint32>(skipped_until - SDL_GetTicks()) > 0; blank != g_blank_picture) {
        g_blank_picture = blank;
        ps2::set_video_options(video_bits());
    }
    SDL_Event e;
    while (timed_poll_event(e)) {
        window_stage("handling a window event");
        const EventTimer event_timer{e.type};
        if (e.type == SDL_QUIT) {
            ps2::flush_memory_card();  // right away: a save made a moment ago must not wait for the emulation to wind down
            ps2::halt("window closed");
            close_menu("the window was closed");  // a game that waits for the menu must go on to see the halt
        }
        // rawdanger-recomp: the game's texts show the keys while the keyboard and mouse are used, the controller's symbols while a
        // controller is (the last one used decides)
        {
            int pad_now = -1;
            if (e.type == SDL_KEYDOWN || e.type == SDL_MOUSEBUTTONDOWN) pad_now = 0;
            if (e.type == SDL_CONTROLLERBUTTONDOWN || (e.type == SDL_CONTROLLERAXISMOTION && std::abs(static_cast<int>(e.caxis.value)) > 20000)) pad_now = 1;
            if (pad_now >= 0 && (pad_now == 1) != g_using_pad) {
                g_using_pad = pad_now == 1;
                std::fprintf(stderr, "[keys] %s in use: the game's texts show %s\n", g_using_pad ? "a controller" : "the keyboard and mouse",
                             g_using_pad ? "the controller's symbols" : "the keys");
                send_key_prompts();
            }
        }
        if (menu_event(e)) continue;
        if (e.type == SDL_KEYDOWN && e.key.keysym.scancode == SDL_SCANCODE_F11 && !e.key.repeat) toggle_fullscreen();
        // F1 the quick menu (menu_event), F5 quick save, F8 quick load, F9 "something is wrong here" (the game's own buttons: keys.ini,
        // host/keymap.cpp); a help key of the player's own shows the key card at once
        if (e.type == SDL_KEYDOWN && !e.key.repeat) {
            if (is_help_key(e.key.keysym.scancode))
                show_key_card((std::string("key ") + SDL_GetScancodeName(e.key.keysym.scancode) + " went down").c_str());
            if (e.key.keysym.scancode == SDL_SCANCODE_F2) cycle_look();
            if (e.key.keysym.scancode == SDL_SCANCODE_F10) toggle_keys_on_screen();  // rawdanger-recomp: the keys on screen
            if (e.key.keysym.scancode == SDL_SCANCODE_F4) {  // rawdanger-recomp: the achievements page at once
                open_menu("F4");
                g_recomp_note.clear();
                g_menu.show_achievements();
                menu_paint();
            }
            if (e.key.keysym.scancode == SDL_SCANCODE_F3) {  // rawdanger-recomp: the Recomp options page at once
                open_menu("F3");
                g_menu.show_recomp();
                menu_command(quickmenu::Command::recomp, 1);
            }
            if (e.key.keysym.scancode == SDL_SCANCODE_F5) ps2::quick_request(ps2::QuickKey::save);
            else if (e.key.keysym.scancode == SDL_SCANCODE_F8) ps2::quick_request(ps2::QuickKey::load);
            else if (e.key.keysym.scancode == SDL_SCANCODE_F9) ps2::quick_request(ps2::QuickKey::report);
            if (e.key.keysym.scancode == SDL_SCANCODE_F7 && !e.key.repeat && !g_menu.is_open() && debug_scenes().set)
                open_debug_scene(!(e.key.keysym.mod & KMOD_SHIFT) || !debug_scenes().debug);
            // rawdanger-recomp: F6 held: the game as fast as it can go (cutscenes, talk, walks it has no skip for); let go: normal
            if (e.key.keysym.scancode == SDL_SCANCODE_F6 && !e.key.repeat && !g_menu.is_open()) {
                g_fast_forward = true;
                set_notice("Fast forward (F6 held)");
            }
        }
        if (e.type == SDL_KEYUP && e.key.keysym.scancode == SDL_SCANCODE_F6 && g_fast_forward) {
            g_fast_forward = false;
            set_notice("Normal speed");
        }
        if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED && g_gpu_present) {
            int w = 0, h = 0;
            SDL_GetWindowSizeInPixels(g_window, &w, &h);
            ps2::resize_window(static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h));
        }
        if (e.type == SDL_WINDOWEVENT && (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED || e.window.event == SDL_WINDOWEVENT_EXPOSED) && g_overlay_on)
            menu_paint();  // the panel for the new size
        if (e.type == SDL_WINDOWEVENT && (e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED || e.window.event == SDL_WINDOWEVENT_FOCUS_LOST)) {
            if (e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) {
                g_focus_gained_ticks = SDL_GetTicks();
                reload_keys_if_changed();
            } else if (g_keys.pause_unfocused && !g_menu.is_open() && g_card.state.load() == 0) {
                open_menu("the window went to the background");  // rawdanger-recomp: the game waits until the player is back
            } else if (g_keys.pause_unfocused && g_menu.is_open() && !g_menu_hold.load()) {
                g_menu_hold.store(true, std::memory_order_release);   // the game ran behind the menu: it waits now
                perf::pause(true);
            }
            apply_mouse_mode();
        }
        if (e.type == SDL_MOUSEMOTION && SDL_GetRelativeMouseMode()) {
            g_look_dx += e.motion.xrel;
            g_look_dy += e.motion.yrel;
        }
        if (e.type == SDL_MOUSEMOTION && g_keys.mouse_stick) {
            g_mouse_sum_x += e.motion.xrel;
            g_mouse_sum_y += e.motion.yrel;
        }
        if (e.type == SDL_MOUSEWHEEL) {
            // a turn of the wheel is a press of about a tenth of a second, which the game sees for a few frames
            (e.wheel.y > 0 ? g_wheel_up_until : g_wheel_down_until) = SDL_GetTicks() + 100;
            // rawdanger-recomp: or, when no button has the wheel, the walk speed of the walk keys
            if (!g_menu.is_open() && e.wheel.y != 0 && !wheel_is_a_button()) {
                g_walk_step = std::clamp(g_walk_step + (e.wheel.y > 0 ? 1 : -1), 1, kWalkSteps);
                set_notice(g_walk_step == kWalkSteps ? "Walk keys: run (they hold Cross; wheel down: walk)" : "Walk keys: walk (wheel up: run)");
            }
        }
        if (e.type == SDL_CONTROLLERDEVICEADDED) open_pad(e.cdevice.which);
        if (e.type == SDL_CONTROLLERDEVICEREMOVED) close_pad(e.cdevice.which);
        if (e.type == SDL_JOYDEVICEADDED && !SDL_IsGameController(e.jdevice.which))
            std::fprintf(stderr, "[pad] %s is not used: SDL knows no button layout for it (docs/sound-pad.md: gamecontrollerdb.txt)\n",
                         or_unknown(SDL_JoystickNameForIndex(e.jdevice.which)));
    }
    window_stage("reading the controllers");
    const auto after_events = ps2::hitch::on() ? ps2::hitch::Clock::now() : ps2::hitch::Clock::time_point{};
    poll_port(0);
    poll_port(1);
    show_notice();
    {
        const bool want = !g_menu.is_open() && g_note_text.empty() && (native_scene() || native_pause());
        const int cursor = want ? native_cursor() : -1;
        const bool label = want && g_native_row.set && cursor >= 0 && cursor < 16;   // the list is up (not one of its pages)
        const bool selected = label && cursor == static_cast<int>(g_native_row.row);
        if (want) native_help_text();
        if (want != g_native_hint || label != g_native_label || selected != g_native_label_selected) {
            g_native_hint = want;
            g_native_label = label;
            g_native_label_selected = selected;
            menu_paint(false);
        }
    }
    // rawdanger-recomp: texture options switched in an area wait for the next area load (PS2_AREA_LOAD="ADDRESS=V1,V2,...": the
    // word has one of those values while the game loads an area; Raw Danger: the scene word 0x48F488, 0x13 and -1)
    {
        static bool parsed = false, set = false, in_area = false;
        static std::uint32_t address = 0;
        static std::vector<std::uint32_t> values;
        if (!parsed) {
            parsed = true;
            if (const char *v = std::getenv("PS2_AREA_LOAD"); v && *v) {
                const std::string t = v;
                const std::size_t eq = t.find('=');
                if (eq != std::string::npos) {
                    address = static_cast<std::uint32_t>(std::strtoul(t.substr(0, eq).c_str(), nullptr, 16));
                    for (std::size_t at = eq + 1; at < t.size();) {
                        const std::size_t end = std::min(t.find(',', at), t.size());
                        values.push_back(static_cast<std::uint32_t>(std::strtoul(t.substr(at, end - at).c_str(), nullptr, 16)));
                        at = end + 1;
                    }
                    set = address != 0 && !values.empty();
                }
            }
        }
        if (set) {
            const std::uint32_t word = ps2::ld32(address);
            const bool loading = std::find(values.begin(), values.end(), word) != values.end();
            if (g_look_when_set && word == g_look_when_value) in_area = true;
            if (loading) {
                in_area = false;
                if (recomp::apply_pending()) sync_jp_textures();
            }
            recomp::set_defer_textures(in_area);
        }
    }
    // rawdanger-recomp: the field of view (keys.ini field_of_view, % of the game's). PS2_CAMERA_ZOOM="@BLOCK,ZOOM,STATE": the camera
    // block, the offset of its zoom factor (a float, 1.0 normally: the projection's screen distance is 768 times it) and of the byte
    // that is non-zero while the game animates a zoom of its own. The factor is written only while the game's zoom is idle and
    // the factor is the game's 1.0 or the one written here last, so the game's own zooms play as they are.
    {
        static bool parsed = false, set = false;
        static std::uint32_t block = 0, zoom = 0, state = 0;
        static float written = 1.0f;
        if (!parsed) {
            parsed = true;
            if (const char *v = std::getenv("PS2_CAMERA_ZOOM"); v && *v == '@')
                set = std::sscanf(v + 1, "%x,%x,%x", &block, &zoom, &state) == 3;
        }
        if (set) {
            const auto get = [](std::uint32_t a) { const std::uint32_t w = ps2::ld32(a); float f; std::memcpy(&f, &w, 4); return f; };
            // v0.10.4: degrees across. The game's half-width at the screen distance: 320 / 768 at 4:3, 320 / 576 with the 16:9 patch
            const float game_tan = ps2::g_options.widescreen ? 320.0f / 576.0f : 320.0f / 768.0f;
            const float want = g_keys.field_of_view <= 0 ? 1.0f
                                                         : game_tan / std::tan(static_cast<float>(std::clamp(g_keys.field_of_view, 30, 130)) * 0.5f * 0.0174533f);
            const float now = get(block + zoom);
            if ((ps2::ld32(block + state) & 0xFFu) == 0 && (now == 1.0f || now == written) && now != want) {
                std::uint32_t w; std::memcpy(&w, &want, 4);
                ps2::st32(block + zoom, w);
                written = want;
            }
        }
    }

    // rawdanger-recomp: the camera's up / down range (PS2_CAMERA_PITCH="POINTER,LOW,HIGH,GAME_LOW,GAME_HIGH,NEW_LOW,NEW_HIGH": a word
    // holding the camera block's address, or "@" and the block itself, the offsets of its two limits (floats), the game's values and the
    // wider ones; written only while the block holds the game's values, so other cameras keep theirs. Raw Danger's follow camera tilts by
    // moving up and down behind the hero: its height runs 2.5 (looking up) to 6.5 (looking down), about 8 degrees up and 13 down;
    // 0.5 and 10 give about 25 up and 30 down: "@5FD7D0,14,18,2.5,6.5,0.5,10")
    {
        static bool parsed = false, set = false, absolute = false;
        static std::uint32_t pointer = 0, low = 0, high = 0;
        static float game_low = 0, game_high = 0, new_low = 0, new_high = 0;
        if (!parsed) {
            parsed = true;
            // "@ADDRESS,...": the camera block itself (Raw Danger's is at 0x5FD7D0; the pointer word is mostly 0 between the game's
            // camera updates, when the window reads it)
            if (const char *v = std::getenv("PS2_CAMERA_PITCH"); v && *v) {
                absolute = *v == '@';
                set = std::sscanf(v + (absolute ? 1 : 0), "%x,%x,%x,%f,%f,%f,%f", &pointer, &low, &high, &game_low, &game_high, &new_low, &new_high) == 7;
            }
        }
        if (set) {
            const std::uint32_t block = absolute ? pointer : ps2::ld32(pointer);
            if (block >= 0x100000u && block < 0x2000000u) {
                const auto get = [](std::uint32_t a) { const std::uint32_t w = ps2::ld32(a); float f; std::memcpy(&f, &w, 4); return f; };
                const auto put = [](std::uint32_t a, float f) { std::uint32_t w; std::memcpy(&w, &f, 4); ps2::st32(a, w); };
                if (get(block + low) == game_low && get(block + high) == game_high) {
                    put(block + low, new_low), put(block + high, new_high);
                    static bool told = false;
                    if (!told) told = true, std::fprintf(stderr, "[camera] up / down range %g..%g (the game's %g..%g)\n", new_low, new_high, game_low, game_high);
                }
            }
        }
    }
    // rawdanger-recomp: the keys on screen come and go with the setting (at the start, and when keys.ini is read again)
    if (!g_menu.is_open() && g_note_text.empty() && !g_native_hint && keys_on_screen_now() != g_overlay_on) menu_paint();
    if (g_menu_redraw) menu_show_again();
    if (ps2::hitch::on() && ps2::hitch::ms_since(after_events) > ps2::hitch::threshold_ms() / 3)
        std::fprintf(stderr, "[hitch] reading the controllers and the notice took %.1f ms%s", ps2::hitch::ms_since(after_events), "\n");
}

void show_pixels(const std::uint32_t *rgba, std::uint32_t w, std::uint32_t h) {
    if (w != g_tex_w || h != g_tex_h) {
        if (g_texture) SDL_DestroyTexture(g_texture);
        g_texture = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, static_cast<int>(w), static_cast<int>(h));
        SDL_RenderSetLogicalSize(g_renderer, static_cast<int>(w), static_cast<int>(w) * (ps2::g_options.widescreen ? 9 : 3) / (ps2::g_options.widescreen ? 16 : 4));
        g_tex_w = w;
        g_tex_h = h;
    }
    SDL_UpdateTexture(g_texture, nullptr, rgba, static_cast<int>(w * 4));
    SDL_RenderClear(g_renderer);
    SDL_RenderCopy(g_renderer, g_texture, nullptr, nullptr);
    draw_menu_sdl();
    SDL_RenderPresent(g_renderer);
}

void present() {
    if (g_gpu_present) {
        ps2::present_frame();
        return;
    }
    std::uint32_t w = 0, h = 0;
    if (ps2::display_frame(g_pixels, w, h)) show_pixels(g_pixels.data(), w, h);
}

double audio_seconds() { return static_cast<double>(SDL_GetPerformanceCounter()) / static_cast<double>(SDL_GetPerformanceFrequency()); }

double queued_ms() { return g_pipe ? g_pipe->queued_ms() : 0.0; }  // the game's sound waiting for the device, in milliseconds of game time

void write_audio_dumps();
void note_audio();

// PS2_NO_LIMIT=1 (docs/speed.md): a window run goes as fast as it can, with no wait for the sound device and no wait for the clock, to
// measure what the processor threads and the graphics chip can do. The sound plays out of step (it is dropped when it runs too far
// behind). Off unless set; "0" counts as off.
// rawdanger-recomp: a scene the player never sees. PS2_SKIP_SCENE="ADDRESS=VALUE" (Raw Danger: the scene word 0x48F488 = 3,
// the memory-card check at start): while the game is in it, it runs as fast as it can and the picture stays black, so the card
// is still read (the options and the unlocks in its system data) but no "Checking memory card" screen shows; PS2_AUTO_CROSS
// answers its one question.
std::pair<std::uint32_t, std::uint32_t> skip_scene_spec() {
    static const auto spec = [] {
        std::pair<std::uint32_t, std::uint32_t> r{0, 0};
        if (const char *v = std::getenv("PS2_SKIP_SCENE"); v && *v) {
            unsigned long a = 0, w = 0;
            if (std::sscanf(v, "%lx=%lu", &a, &w) == 2) r = {static_cast<std::uint32_t>(a), static_cast<std::uint32_t>(w)};
        }
        return r;
    }();
    return spec;
}
bool in_skipped_scene() {
    const auto spec = skip_scene_spec();
    return spec.first && ps2::ld32(spec.first) == spec.second;
}

bool no_limit() {
    if (g_fast_forward || in_skipped_scene()) return true;
    static const bool on = [] {
        const std::string value = ps2::os::env("PS2_NO_LIMIT");
        const bool set = !value.empty() && value != "0";
        if (set) std::fprintf(stderr, "[pace] PS2_NO_LIMIT is set: the game runs with no speed limit; the sound plays out of step\n");
        return set;
    }();
    return on;
}

// PS2_PACE=audio: the sound device paces the game (the game waits while more than ~80 ms of sound is waiting), as before. A device
// that takes its sound in large blocks (wireless headsets over WASAPI) then makes the frames uneven: seven of 20 ms, one of 30 ms, one
// of 3 ms, about six times a second (measured with Urban Reign and a Corsair HS80), which shows as judder in every camera pan even
// at a steady 50 fps. By default the clock paces the game instead, one frame every 1/50 or 1/60 s, and the sound only steers that
// clock: frames up to 1% longer while more sound than kSteerMs is waiting, up to 1% shorter while less. The sound stretcher still
// covers a game that runs slow.
bool audio_paced() {
    static const bool on = ps2::os::env("PS2_PACE") == "audio";
    return on;
}

constexpr double kSteerMs = 90.0;  // the sound the clock-paced game keeps waiting for the device: enough that a device taking large blocks does not dip it under the stretcher's gap guard (~45 ms with a 20 ms frame), which would stretch the sound for no reason

// Waits until the performance counter reaches `target`: sleeps while more than 2 ms are left, then spins, for a frame edge within
// a few microseconds (SDL_Delay alone wakes up to a millisecond late, which is the unevenness this is meant to remove).
void wait_until(std::uint64_t target) {
    const std::uint64_t frequency = SDL_GetPerformanceFrequency();
    for (;;) {
        const std::uint64_t now = SDL_GetPerformanceCounter();
        if (now >= target) return;
        const double left_ms = static_cast<double>(target - now) * 1000.0 / static_cast<double>(frequency);
        if (left_ms > 2.0) SDL_Delay(static_cast<Uint32>(left_ms - 1.5));
        else std::this_thread::yield();
    }
}

void pace() {
    achievements::frame();  // rawdanger-recomp: once per video frame, with or without a sound device
    lens_rain::frame(ps2::g_options.pal ? 1.0f / 50.0f : 1.0f / 60.0f);
    perf::frame();
    if (no_limit()) return;
    static bool stuck = false;  // the device took no sound for a second; the clock paces the game until it takes some again
    if (g_audio && !audio_paced() && !stuck) {
        g_pipe->frame(audio_seconds());
        const double started = audio_seconds();
        const double frequency = static_cast<double>(SDL_GetPerformanceFrequency());
        const double steer = std::clamp((g_pipe->queued_ms() - kSteerMs) / kSteerMs * 0.02, -0.01, 0.01);
        const auto period = static_cast<std::uint64_t>(frequency / (ps2::g_options.pal ? 50.0 : 60.0) * (1.0 + steer));
        const std::uint64_t now = SDL_GetPerformanceCounter();
        // more than two frames behind (a load, a hold): start counting again from now rather than rushing to catch up
        if (g_last && now < g_last + 2 * period) {
            g_last += period;
            wait_until(g_last);
        } else {
            g_last = now;
        }
        // a device that takes far less than the game makes (or none at all) must still hold the game: the old wait, as a ceiling
        const double hold = audio_seconds();
        while (g_pipe->queued_ms() > 3.0 * kSteerMs && audio_seconds() - hold < 1.0) SDL_Delay(1);
        const double waited = audio_seconds() - started;
        g_pipe->note_wait(waited);
        note_audio();
        if (audio_seconds() - hold >= 1.0) {
            std::fprintf(stderr, "[audio] the sound device took no sound for a second (unplugged?); the clock paces the game until it does\n");
            stuck = true;
        }
        return;
    }
    if (g_audio && !(stuck && g_pipe->full())) {
        // The audio device clock paces emulation: wait while more than ~80 ms of sound is waiting. The pipe times the game between
        // the frames (how much it slows down decides how much the sound is stretched) and does not count this wait as slowness.
        g_pipe->frame(audio_seconds());
        const double started = audio_seconds();
        while (g_pipe->full() && audio_seconds() - started < 1.0) SDL_Delay(1);  // at most a second: a stopped device must not hold the game
        const double waited = audio_seconds() - started;
        g_pipe->note_wait(waited);
        note_audio();
        if (!stuck && waited >= 1.0) std::fprintf(stderr, "[audio] the sound device took no sound for a second (unplugged?); the clock paces the game until it does\n");
        stuck = waited >= 1.0;
        return;
    }
    const std::uint64_t frequency = SDL_GetPerformanceFrequency();
    const std::uint64_t period = frequency / (ps2::g_options.pal ? 50 : 60);
    const std::uint64_t now = SDL_GetPerformanceCounter();
    if (g_last && now - g_last < period) {
        SDL_Delay(static_cast<Uint32>((period - (now - g_last)) * 1000 / frequency));
        g_last += period;
    } else {
        g_last = now;
    }
}

double process_cpu_seconds() {
#ifdef _WIN32
    FILETIME created, exited, kernel, user;
    GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
    const auto ticks = [](const FILETIME &t) { return static_cast<double>((static_cast<std::uint64_t>(t.dwHighDateTime) << 32) | t.dwLowDateTime); };
    return (ticks(kernel) + ticks(user)) / 1e7;
#else
    rusage usage{};
    getrusage(RUSAGE_SELF, &usage);
    return static_cast<double>(usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) + static_cast<double>(usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) / 1e6;
#endif
}

double resident_mb() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS counters{};
    GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof counters);
    return static_cast<double>(counters.WorkingSetSize) / (1024.0 * 1024.0);
#elif defined(__APPLE__)
    mach_task_basic_info_data_t info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS) return 0.0;
    return static_cast<double>(info.resident_size) / (1024.0 * 1024.0);
#else
    long pages = 0, resident = 0;
    if (std::FILE *f = std::fopen("/proc/self/statm", "r")) {
        if (std::fscanf(f, "%ld %ld", &pages, &resident) != 2) resident = 0;
        std::fclose(f);
    }
    return static_cast<double>(resident) * static_cast<double>(sysconf(_SC_PAGESIZE)) / (1024.0 * 1024.0);
#endif
}

void show_speed() {
    static std::uint64_t start = 0, frames = 0;
    static double cpu_start = 0.0, gpu_start = 0.0;
    const std::uint64_t now = SDL_GetPerformanceCounter();
    if (!start) {
        start = now;
        cpu_start = process_cpu_seconds();
        gpu_start = ps2::gpu_busy_ms();
    }
    ++frames;
    const double seconds = static_cast<double>(now - start) / static_cast<double>(SDL_GetPerformanceFrequency());
    if (seconds < 1.0) return;
    const double cpu = process_cpu_seconds(), gpu = ps2::gpu_busy_ms();
    char title[192];
    const double queued = queued_ms();
    char stretched[40] = "";  // "audio 62 ms" is the game's sound waiting; when it is played slower, by how much
    if (g_pipe && g_pipe->tempo() < 0.995) std::snprintf(stretched, sizeof stretched, ", stretched x%.1f", 1.0 / g_pipe->tempo());
    std::snprintf(title, sizeof title, "ps2-native-recomp  %.1f fps  CPU %.0f%%  GPU %.0f%%  RAM %.0f MB  audio %.0f ms%s", static_cast<double>(frames) / seconds,
                  (cpu - cpu_start) / seconds * 100.0, (gpu - gpu_start) / (seconds * 10.0), resident_mb(), queued, stretched);
    g_speed_title = title;
    refresh_title();
    if (g_stats_on) {
        char a[64], b[64], c[64];
        std::snprintf(a, sizeof a, "%.1f", static_cast<double>(frames) / seconds);
        std::snprintf(b, sizeof b, "%.0f %% / %.0f %%", (cpu - cpu_start) / seconds * 100.0, (gpu - gpu_start) / (seconds * 10.0));
        std::snprintf(c, sizeof c, "%.0f MB, audio %.0f ms", resident_mb(), queued);
        g_stats_rows = {{"FPS", a}, {"CPU / GPU", b}, {"Memory", c}};
        if (debug_info().set) {
            const DebugInfo &d = debug_info();
            const std::uint32_t cam = ps2::ld32(d.camera);
            g_stats_rows.push_back({"Scene", std::to_string(ps2::ld32(d.scene)) + (cam >= 0x100000u && cam < 0x2000000u ? ", camera mode " + std::to_string(ps2::ld32(cam + d.mode)) : "")});
        }
        if (const float wet = ps2::hero_wetness(); wet >= 0.0f) g_stats_rows.push_back({"Clothes wet", std::to_string(static_cast<int>(wet * 100.0f + 0.5f)) + " %"});
        static const char *const kViews[] = {"the game's", "AO alone", "depth", "capsules"};
        g_stats_rows.push_back({"Picture", kViews[std::clamp(ps2::debug_view(), 0, 3)]});
        // v0.10.3: the game's steps a second as measured (the hero's, once a step) and the vblanks a step the game waits for
        // (20 steps: 3, 30: 2, 60: 1). Also written to the log while the stats show, for reports on the frame-rate builds.
        {
            static std::uint64_t steps_before = 0;
            const std::uint64_t steps = ps2::hero_steps();
            const double rate = steps_before && steps >= steps_before ? static_cast<double>(steps - steps_before) / seconds : 0.0;
            steps_before = steps;
            std::uint32_t wait = 0;
            if (debug_steps().set) {
                const std::uint32_t g = ps2::ld32(debug_steps().pointer);
                if (g >= 0x100000u && g < 0x2000000u) wait = ps2::ld32(g + debug_steps().offset);
            }
            char st[96];
            if (wait > 0 && wait < 10) std::snprintf(st, sizeof st, "%.1f a second (%u vblank%s a step = %u)", rate, wait, wait == 1 ? "" : "s", 60u / wait);
            else std::snprintf(st, sizeof st, "%.1f a second", rate);
            g_stats_rows.push_back({"Game steps", st});
            std::fprintf(stderr, "[steps] frame %llu: %.1f fps, game steps %.1f a second, vblanks a step %u%s, CPU %s\n",
                         static_cast<unsigned long long>(ps2::frame_count()), static_cast<double>(frames) / seconds, rate, wait,
                         g_force_vblanks > 0 ? " (forced: Settings > Debug)" : "", b);
        }
        g_stats_rows.push_back({"Frame", std::to_string(ps2::frame_count())});
        menu_paint(true);
    }
    start = now;
    frames = 0;
    cpu_start = cpu;
    gpu_start = gpu;
}

// PS2_PRESENT_LOG=1: once a second, how long the emulation thread waited per frame for the window thread (post_present: the
// window thread is two pictures behind) and for the sound (pace), and how the window thread's time was spent (docs/speed.md).
struct WaitLog {
    using Clock = std::chrono::steady_clock;
    bool on = std::getenv("PS2_PRESENT_LOG") != nullptr;
    Clock::time_point since = Clock::now();
    double post = 0, sound = 0, seen_working = 0, seen_idle = 0;
    std::atomic<double> working{0}, idle{0};  // written by the window thread, read by the emulation thread
    std::uint32_t frames = 0;
    static double ms(Clock::time_point from) { return std::chrono::duration<double, std::milli>(Clock::now() - from).count(); }
    void emulation_frame() {
        if (++frames < 60) return;
        const double seconds = std::chrono::duration<double>(Clock::now() - since).count();
        std::fprintf(stderr, "[waits] %.1f frames/s; emulation thread per frame: waiting for the window thread %.2f ms, for the sound %.2f ms; window thread per second: rendering %.0f ms, no work %.0f ms\n",
                     frames / seconds, post / frames, sound / frames, (working.load() - seen_working) / seconds, (idle.load() - seen_idle) / seconds);
        since = Clock::now();
        post = sound = 0;
        seen_working = working.load();
        seen_idle = idle.load();
        frames = 0;
    }
};
WaitLog g_waits;

// PS2_FRAME_LOG=<file>: one line per emulated frame, to find what happens when a scene suddenly gets slow (docs/speed.md, the arena).
// Columns: frame number, milliseconds since the previous frame, CPU milliseconds all the threads used meanwhile, milliseconds the emulation
// thread waited for the window thread and for the sound, shapes drawn, vector programs run by the slow interpreter, sound queued (ms).
struct FrameLog {
    std::FILE *file = nullptr;
    std::uint64_t last_counter = 0, last_prims = 0, last_interpreted = 0;
    double last_cpu = 0;
    FrameLog() {
        const std::string path = ps2::os::env("PS2_FRAME_LOG");
        if (!path.empty()) {
            file = ps2::os::open(path, "w");
            if (file) std::fprintf(file, "frame wall_ms cpu_ms wait_window_ms wait_sound_ms shapes interpreted audio_ms\n");
        }
    }
    void frame(double wait_window_ms, double wait_sound_ms, double queued_ms) {
        const std::uint64_t now = SDL_GetPerformanceCounter();
        const double cpu = process_cpu_seconds();
        const std::uint64_t prims = ps2::primitives_drawn(), interpreted = ps2::vu1_interpreted_calls();
        if (last_counter)
            std::fprintf(file, "%llu %.2f %.2f %.2f %.2f %llu %llu %.0f\n", static_cast<unsigned long long>(ps2::frames_run()),
                         static_cast<double>(now - last_counter) * 1000.0 / static_cast<double>(SDL_GetPerformanceFrequency()), (cpu - last_cpu) * 1000.0,
                         wait_window_ms, wait_sound_ms, static_cast<unsigned long long>(prims - last_prims), static_cast<unsigned long long>(interpreted - last_interpreted), queued_ms);
        last_counter = now;
        last_cpu = cpu;
        last_prims = prims;
        last_interpreted = interpreted;
        if (ps2::frames_run() % 60 == 0) std::fflush(file);
    }
};
FrameLog g_frame_log;

void on_vblank() {
    show_speed();
    poll();
    present();
    pace();
    if (g_frame_log.file) g_frame_log.frame(0.0, 0.0, queued_ms());
    if (g_menu.is_open() && g_menu_hold.load()) {  // the quick menu: this thread is the window's too, so it answers the window while the game waits
        const double since = audio_seconds();
        while (g_menu.is_open() && g_menu_hold.load()) {
            poll();
            SDL_Delay(10);
        }
        if (g_pipe) g_pipe->note_wait(audio_seconds() - since);  // the wait is no slowness of the game: no stretched sound after it
        g_last = 0;
    }
}

// The quick menu with threads: the emulation thread waits here, at the end of its frame, until the menu closes.
void hold_for_menu() {
    if (!g_menu_hold.load(std::memory_order_acquire)) return;
    const double since = audio_seconds();
    while (g_menu_hold.load(std::memory_order_acquire)) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    if (g_pipe) g_pipe->note_wait(audio_seconds() - since);  // the wait is no slowness of the game: no stretched sound after it
    g_last = 0;
}

// PS2_AUDIO_DUMP=<file.wav>: the sound exactly as the stretcher hands it to the sound device (no silence where it ran short),
// PS2_AUDIO_HEARD=<file.wav>: what the speaker got, silence included. 48000 Hz 16-bit stereo. The header is brought up to date
// about once a second, so a run that is killed keeps a file that plays up to there.
class WavOut {
public:
    ~WavOut() { close(); }
    bool open(const std::string &path) {
        file_ = ps2::os::open(path, "wb");
        if (!file_) std::fprintf(stderr, "[audio] cannot write %s\n", path.c_str());
        path_ = path;
        if (file_) header();
        return file_ != nullptr;
    }
    void write(const std::int16_t *samples, std::size_t frames) {
        if (!file_ || frames_ + frames > (0xFFFFFFFFull - 36) / 4) return;
        std::fwrite(samples, 4, frames, file_);
        frames_ += frames;
        if (frames_ - synced_ >= 48000) header();
    }
    void close() {
        if (!file_) return;
        header();
        std::fclose(file_);
        file_ = nullptr;
        std::fprintf(stderr, "[audio] wrote %llu samples (%.2f s) to %s\n", static_cast<unsigned long long>(frames_), static_cast<double>(frames_) / 48000.0, path_.c_str());
    }

private:
    void header() {
        const std::uint32_t bytes = static_cast<std::uint32_t>(frames_ * 4);
        const std::uint32_t words[] = {0x46464952u, 36 + bytes, 0x45564157u, 0x20746D66u, 16, 0x00020001u, 48000, 48000 * 4, 0x00100004u, 0x61746164u, bytes};
        std::fseek(file_, 0, SEEK_SET);
        std::fwrite(words, 4, sizeof words / 4, file_);
        std::fseek(file_, 0, SEEK_END);
        std::fflush(file_);
        synced_ = frames_;
    }
    std::FILE *file_ = nullptr;
    std::string path_;
    std::uint64_t frames_ = 0, synced_ = 0;
};

WavOut g_dump, g_heard;

// PS2_AUDIO_PUSHLOG=<file>: when the sound chip pushed its sound and when each video frame ended (time in seconds, kind and size, waiting
// ms, frame), written at the end. tests/stretch_test.cpp replay <file> runs the sound pipe on the real timing (docs/sound.md).
struct PushLog {
    struct Row {
        double time;
        int kind, frames;
        float waiting;
        unsigned frame;
    };
    std::vector<Row> rows;
    std::string path = ps2::os::env("PS2_AUDIO_PUSHLOG");
    void add(int kind, int frames, double now) {
        if (!path.empty()) rows.push_back({now, kind, frames, static_cast<float>(queued_ms()), static_cast<unsigned>(ps2::frames_run())});
    }
    void write() {
        if (path.empty()) return;
        if (std::FILE *f = ps2::os::open(path, "w")) {
            for (const Row &r : rows) std::fprintf(f, "%.6f %d %.1f %u\n", r.time, r.kind * 100000 + r.frames, static_cast<double>(r.waiting), r.frame);
            std::fclose(f);
        }
        path.clear();
    }
};
PushLog g_pushlog;

struct Tone {
    int hz, ms;
};
constexpr Tone kBeepSaved[] = {{1047, 90}};
constexpr Tone kBeepLoaded[] = {{784, 80}, {0, 40}, {1047, 80}};
constexpr Tone kBeepReport[] = {{1047, 70}, {0, 40}, {784, 70}, {0, 40}, {523, 110}};
constexpr Tone kBeepTrouble[] = {{196, 300}};
bool g_beep_running = false;

// Adds the tone that was asked for to the game's sound (48000 Hz stereo), in place. Runs on the sound device's thread.
void mix_beep(std::int16_t *samples, std::size_t frames) {
    static const Tone *seq = nullptr;
    static std::size_t count = 0, index = 0;
    static int left = 0, total = 0;
    static double phase = 0.0;
    if (const int kind = g_beep_request.exchange(0)) {
        seq = kind == 1 ? kBeepSaved : kind == 2 ? kBeepLoaded : kind == 3 ? kBeepReport : kBeepTrouble;
        count = kind == 1 ? std::size(kBeepSaved) : kind == 2 ? std::size(kBeepLoaded) : kind == 3 ? std::size(kBeepReport) : std::size(kBeepTrouble);
        index = 0;
        total = left = seq[0].ms * 48;
        phase = 0.0;
    }
    g_beep_running = seq != nullptr;
    for (std::size_t i = 0; seq && i < frames; ++i) {
        while (left == 0) {
            if (++index >= count) {
                seq = nullptr;
                g_beep_running = false;
                return;
            }
            total = left = seq[index].ms * 48;
            phase = 0.0;
        }
        const int position = total - left--;
        if (!seq[index].hz) continue;
        const double ramp = std::min({1.0, position / 240.0, left / 240.0});  // 5 ms in and out, so the tone does not click
        const int v = static_cast<int>(std::sin(phase) * ramp * 3500.0);
        phase += 2.0 * 3.14159265358979 * seq[index].hz / 48000.0;
        for (int ch = 0; ch < 2; ++ch) samples[i * 2 + ch] = static_cast<std::int16_t>(std::clamp(samples[i * 2 + ch] + v, -32768, 32767));
    }
}

// The sound chip's batches of 32 frames arrive here on the emulation thread.
void queue_audio(const std::int16_t *samples, std::size_t frames) {
    const double now = audio_seconds();
    g_pushlog.add(0, static_cast<int>(frames), now);
    g_pipe->push(samples, frames, now);
}

// The sound device asks for its next block (its own thread): the pipe hands over the game's sound, stretched when the game is slow,
// and the tone for F5, F8 and F9 is mixed on top.
void audio_callback(void *, Uint8 *stream, int length) {
    static thread_local bool named = false;  // the sound device's own thread
    if (!named) {
        named = true;
        ps2::crash::enter_thread(ps2::crash::Thread::Sound);
    }
    ps2::crash::test_poll(ps2::crash::Thread::Sound);
    g_pipe->render(reinterpret_cast<std::int16_t *>(stream), static_cast<std::size_t>(length) / 4, audio_seconds(), mix_beep);
    // rawdanger-recomp: the master volume (keys.ini, the menu's Graphics and sound page); a curve, so each step sounds alike
    const int volume = g_master_volume.load(std::memory_order_relaxed);
    if (volume < 100) {
        const int gain = volume * volume * 256 / 10000;  // 0 ... 256
        std::int16_t *samples = reinterpret_cast<std::int16_t *>(stream);
        for (int i = 0; i < length / 2; ++i) samples[i] = static_cast<std::int16_t>(samples[i] * gain / 256);
    }
}

void write_audio_dumps() {
    static std::vector<std::int16_t> chunk;
    if (g_pipe->device_tap.on) {
        chunk.clear();
        g_pipe->device_tap.take(chunk);
        g_dump.write(chunk.data(), chunk.size() / 2);
    }
    if (g_pipe->heard_tap.on) {
        chunk.clear();
        g_pipe->heard_tap.take(chunk);
        g_heard.write(chunk.data(), chunk.size() / 2);
    }
}

// once per video frame (from pace): the dump files, and a line when the sound had to stop for lack of sound
void note_audio() {
    static std::uint64_t seen = 0, last_report = 0;
    write_audio_dumps();
    g_pushlog.add(1, 0, audio_seconds());
    const std::uint64_t dry = g_pipe->stats().dry;
    const std::uint64_t now = SDL_GetPerformanceCounter();
    if (dry > seen && (!last_report || now - last_report > 5 * SDL_GetPerformanceFrequency())) {
        std::fprintf(stderr, "[audio] the sound ran dry at frame %llu (%llu times so far): the game runs slower than real time%s\n", static_cast<unsigned long long>(ps2::frames_run()),
                     static_cast<unsigned long long>(dry), g_pipe->config().stretch ? " than the sound can be stretched to" : "");
        last_report = now;
    }
    seen = dry;
}

// at the end of a window run: how much sound the game made, how it was played, and how much silence there was
void finish_audio() {
    {
        std::string extra = std::string("picture ") + (ps2::g_options.widescreen ? "16:9" : "4:3");
        if (g_pipe) extra += ", the sound ran dry " + std::to_string(g_pipe->stats().dry) + " times";
        perf::write(extra);
    }
    if (g_audio && g_pipe) {
        SDL_CloseAudioDevice(g_audio);  // the device thread stops here
        g_audio = 0;
        write_audio_dumps();
        const ps2sound::PipeStats s = g_pipe->stats();
        const double wall = s.first >= 0.0 ? s.last - s.first : 0.0;
        std::fprintf(stderr, "[audio] the device got %.2f s of sound in %.2f s; the queue ran dry %llu times (about %.2f s of silence in between), %llu samples dropped\n",
                     static_cast<double>(s.pushed) / 48000.0, wall, static_cast<unsigned long long>(s.dry), static_cast<double>(s.silence) / 48000.0,
                     static_cast<unsigned long long>(s.dropped));
        if (g_pipe->config().stretch)
            std::fprintf(stderr, "[audio] stretching: %.2f s of game sound played as %.2f s, %.2f s of that stretched (slowest tempo x%.2f; %.2f s at the limit of x%.1f, where it runs dry like without stretching)\n",
                         s.consumed / 48000.0, static_cast<double>(s.played) / 48000.0, static_cast<double>(s.stretched) / 48000.0, 1.0 / s.slowest,
                         static_cast<double>(s.at_cap) / 48000.0, g_pipe->config().max_ratio);
        else
            std::fprintf(stderr, "[audio] stretching is off (PS2_AUDIO_STRETCH=0)\n");
        g_pushlog.write();
        g_dump.close();
        g_heard.close();
    }
}

}

void poll_input() { poll(); }

// rawdanger-recomp: a picture-shape switch also gives a window of its own size the new shape (16:9 <-> 4:3 at the same height), so
// 4:3 is not a narrow picture between black bars; a full-screen or maximized window keeps its size (bars at the sides).
void fit_window_to_shape() {
    if (!g_window) return;
    const Uint32 flags = SDL_GetWindowFlags(g_window);
    if (flags & (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_MAXIMIZED)) return;
    int w = 0, h = 0;
    SDL_GetWindowSize(g_window, &w, &h);
    const int want = ps2::g_options.widescreen ? h * 16 / 9 : h * 4 / 3;
    if (h > 0 && want != w) {
        int x = 0, y = 0;
        SDL_GetWindowPosition(g_window, &x, &y);
        SDL_SetWindowSize(g_window, want, h);
        SDL_SetWindowPosition(g_window, x - (want - w) / 2, y);
    }
}

bool open_window(bool fullscreen) {
    // rawdanger-recomp: the game's options first, so the window opens in the shape the player chose (recomp.ini)
    if (!recomp::loaded() && recomp::load(std::getenv("PS2_RECOMP_OPTIONS"))) recomp::apply_startup(), sync_jp_textures();
    const std::string script = ps2::os::env("PS2_PAD_SCRIPT"), pad_log = ps2::os::env("PS2_PAD_LOG"), no_beep = ps2::os::env("PS2_NO_BEEP");
    g_beep_on = no_beep.empty() || no_beep == "0";
    g_pad_log = !pad_log.empty() && pad_log != "0";
    if (!script.empty()) SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) {
        std::fprintf(stderr, "SDL: %s\n", SDL_GetError());
        return false;
    }
    // button layouts for pads SDL does not know by itself: a gamecontrollerdb.txt next to ps2run (SDL_GAMECONTROLLERCONFIG_FILE
    // and SDL_GAMECONTROLLERCONFIG work too)
    if (char *base = SDL_GetBasePath()) {
        const std::string db = std::string(base) + "gamecontrollerdb.txt";
        SDL_free(base);
        if (SDL_RWops *file = SDL_RWFromFile(db.c_str(), "rb")) std::fprintf(stderr, "[pad] %d button layouts from %s\n", SDL_GameControllerAddMappingsFromRW(file, 1), db.c_str());
    }
    if (!script.empty()) load_pad_script(script);
    g_keys_path = ps2::os::env("PS2_KEYS");
    if (g_keys_path.empty()) g_keys_path = ps2::quick_folder("keys.ini");  // next to the memory card, like the quick saves
    if (const std::string word = ps2::os::env("PS2_LOOK"); !word.empty()) {
        for (int i = 0; i < keymap::kLookCount; ++i)
            if (word == keymap::look_word(i) || word == std::to_string(i)) g_look = i, g_look_forced = true;
        if (!g_look_forced) std::fprintf(stderr, "[video] PS2_LOOK=%s is not crisp, crisp_edges, soft, soft_edges, sharp, sharp_edges, fsr, fsr_edges or 0 to 7, ignored\n", word.c_str());
    }
    load_keys();
    // The inner resolution: PS2_SCALE when it is set, else the 'scale' line of keys.ini (the quick menu writes it), else 1. It is read
    // when the graphics start (run_window, after this), so setting it here is in time.
    g_scale_locked = !ps2::os::env("PS2_SCALE").empty();
    if (!g_scale_locked && g_keys.scale == keymap::kScaleAuto) {
        // auto: the inner resolution whose picture is at least as tall as the screen (so the window shows it reduced, never
        // enlarged: no filter is needed), at most 4x
        SDL_DisplayMode mode;
        int fit = 2;
        if (SDL_GetDesktopDisplayMode(0, &mode) == 0 && mode.h > 0) fit = std::clamp((mode.h + 447) / 448, 1, 6);
        ps2::os::set_env("PS2_SCALE", std::to_string(fit));
        std::fprintf(stderr, "[video] resolution auto: %dx for a %dx%d screen\n", fit, mode.w, mode.h);
    } else if (!g_scale_locked && g_keys.scale >= 1 && g_keys.scale <= 8) {
        ps2::os::set_env("PS2_SCALE", std::to_string(g_keys.scale));
        std::fprintf(stderr, "[video] resolution %dx from %s (scale = %d)\n", g_keys.scale, g_keys_path.c_str(), g_keys.scale);
    }
    // rawdanger-recomp (v0.10.4): the graphics API (keys.ini graphics_api), unless PS2_GPU_BACKEND is set. Vulkan or Direct3D 12 is
    // tried first and the other one when it cannot start. Auto: Windows' default (Direct3D 12, the faster painter on the PCs measured),
    // but Vulkan first when the resolution asked for is more than Direct3D 12 can hold (it binds 128 MB of one buffer: 4x at most).
    if (ps2::os::env("PS2_GPU_BACKEND").empty()) {
        const int want_scale = std::atoi(ps2::os::env("PS2_SCALE").c_str());
        const char *api = g_keys.graphics_api == 1 ? "vulkan-first" : g_keys.graphics_api == 2 ? "dx12-first" : want_scale > 4 ? "vulkan-first" : nullptr;
        if (api) {
            ps2::os::set_env("PS2_GPU_BACKEND", api);
            std::fprintf(stderr, "[video] graphics API: %s first (%s)\n", api[0] == 'v' ? "Vulkan" : "Direct3D 12",
                         g_keys.graphics_api ? "keys.ini graphics_api" : "auto: the resolution is above 4x");
        }
    }
    // The picture pace (ps2/pace.hpp): the 'pace' line of keys.ini, unless PS2_PACE is set. The runtime reads it at boot, after this.
    if (!g_keys.pace.empty() && ps2::os::env("PS2_PACE").empty()) {
        ps2::os::set_env("PS2_PACE", g_keys.pace);
        ps2::os::set_env("PS2_PACE_FROM", g_keys_path);
    }
    g_scale_now = std::clamp(std::atoi(ps2::os::env("PS2_SCALE").c_str()), 1, 8);
    g_scale_next = g_scale_locked ? g_scale_now : (g_keys.scale >= 1 ? g_keys.scale : g_scale_now);
    if (!g_scale_locked && g_keys.scale == keymap::kScaleAuto) g_scale_now = keymap::kScaleAuto;  // shown as "auto" while it is the choice
    // the 2D art filter the same way: PS2_SHARP_2D, else the 'sharp_2d' line of keys.ini, else sharp (gpu.cpp reads it at its start)
    g_sharp_locked = !ps2::os::env("PS2_SHARP_2D").empty();
    if (!g_sharp_locked && g_keys.sharp_2d >= 0) ps2::os::set_env("PS2_SHARP_2D", std::to_string(g_keys.sharp_2d));
    g_sharp_now = ps2::os::env("PS2_SHARP_2D").empty() ? 1 : std::clamp(std::atoi(ps2::os::env("PS2_SHARP_2D").c_str()), 0, 2);
    g_sharp_next = g_sharp_locked ? g_sharp_now : (g_keys.sharp_2d >= 0 ? g_keys.sharp_2d : g_sharp_now);
    Uint32 flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | (fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0u);
#ifdef __APPLE__
    if (!std::getenv("PS2_SDL_RENDERER")) flags |= SDL_WINDOW_METAL;
#endif
    g_game_title = ps2::os::env("PS2_WINDOW_TITLE");
    const std::string initial_title = g_game_title.empty() ? g_speed_title : g_game_title + "   |   " + g_speed_title;
    // rawdanger-recomp: the size picked in F1 -> Settings -> Display (keys.ini window_size), else 960 x 720 / 1280 x 720
    const int start_w = g_keys.window_w > 0 ? g_keys.window_w : ps2::g_options.widescreen ? 1280 : 960, start_h = g_keys.window_h > 0 ? g_keys.window_h : 720;
    g_window = SDL_CreateWindow(initial_title.c_str(), SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, start_w, start_h, flags);
    if (!g_window) {
        std::fprintf(stderr, "SDL: %s\n", SDL_GetError());
        return false;
    }
    set_window_icon();
    SDL_ShowWindow(g_window);
    SDL_RaiseWindow(g_window);
#ifdef _WIN32
    SDL_SysWMinfo wm_info;
    SDL_VERSION(&wm_info.version);
    if (SDL_GetWindowWMInfo(g_window, &wm_info) && wm_info.subsystem == SDL_SYSWM_WINDOWS) {
        ShowWindow(static_cast<HWND>(wm_info.info.win.window), SW_SHOWNORMAL);
        SetForegroundWindow(static_cast<HWND>(wm_info.info.win.window));
    }
#endif
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
    SDL_AudioSpec want{}, have{};
    want.freq = 48000;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 1024;
    // PS2_AUDIO_STRETCH=0 plays the sound as it comes (silence where it runs short); PS2_AUDIO_STRETCH_MAX=<ratio> changes the limit
    static ps2sound::PipeConfig config;
    const std::string stretch = ps2::os::env("PS2_AUDIO_STRETCH"), stretch_max = ps2::os::env("PS2_AUDIO_STRETCH_MAX");
    config.stretch = stretch != "0";
    if (const double limit = std::atof(stretch_max.c_str()); limit >= 1.5 && limit <= 20.0) config.max_ratio = limit;
    static ps2sound::SoundPipe pipe(config);
    g_pipe = &pipe;
    want.callback = audio_callback;
    g_audio = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (g_audio) {
        // SDL converts the 48000 Hz stereo queue to whatever the device itself runs at
        SDL_AudioSpec device{};
        char *name = nullptr;
        if (SDL_GetDefaultAudioInfo(&name, &device, 0) == 0)
            std::fprintf(stderr, "[audio] sound device %s (%s): %d Hz, %d channels; the game's sound is 48000 Hz stereo\n", name ? name : "?",
                         SDL_GetCurrentAudioDriver(), device.freq, device.channels);
        SDL_free(name);
        const std::string dump = ps2::os::env("PS2_AUDIO_DUMP");
        if (!dump.empty() && g_dump.open(dump)) {
            g_pipe->device_tap.on = true;
            std::fprintf(stderr, "[audio] writing the sound that goes to the device to %s\n", dump.c_str());
        }
        const std::string heard = ps2::os::env("PS2_AUDIO_HEARD");
        if (!heard.empty() && g_heard.open(heard)) {
            g_pipe->heard_tap.on = true;
            std::fprintf(stderr, "[audio] writing what the speaker plays (silence included) to %s\n", heard.c_str());
        }
        std::fprintf(stderr, "[audio] %s\n", config.stretch ? "when the game runs slower than real time its sound is played slower, with the same pitch (PS2_AUDIO_STRETCH=0 switches that off)"
                                                          : "stretching is off: the sound is played as it comes, with silence where the game was too slow");
        SDL_PauseAudioDevice(g_audio, 0);
        ps2::set_audio_sink(queue_audio);
    } else {
        std::fprintf(stderr, "[audio] no sound device: %s\n", SDL_GetError());
    }
    for (int i = 0; i < SDL_NumJoysticks(); ++i)
        if (SDL_IsGameController(i)) open_pad(i);
    SDL_ShowCursor(SDL_DISABLE);  // no pointer over the game (the title bar and the window's edge still show it)
    g_notice_text = kStartNotice;
    std::fprintf(stderr, "[menu] the quick menu: F1%s on the keyboard, hold Select then press Start (or Guide) on a controller\n",
                 g_keys.inputs[keymap::Menu].empty() ? "" : (" or " + keymap::input_name(g_keys.inputs[keymap::Menu][0])).c_str());
    g_notice_until = SDL_GetTicks() + 30000;
    refresh_title();
    apply_mouse_mode();
    return true;
}

#ifdef __linux__
// A window run whose game stops (no new frame) used to stay silent: the owner closed the window, nothing answered, the compositor's
// "not responding" window killed the process, and the log ended without a word about where the threads were. This watches the frame
// counter; after kWatchdogSeconds without a new frame it writes one line and the stack of every thread (each thread is sent SIGUSR2 and
// writes its own, which is what the unwinder needs), once per stall. Nothing here touches the game.
namespace {
constexpr int kWatchdogSeconds = 10;
std::atomic<int> g_stack_done{0};

void write_number(long long value) {
    char digits[24];
    int n = 0;
    unsigned long long v = value < 0 ? 0ull : static_cast<unsigned long long>(value);
    do digits[n++] = static_cast<char>('0' + v % 10);
    while ((v /= 10) && n < 23);
    char text[24];
    for (int i = 0; i < n; ++i) text[i] = digits[n - 1 - i];
    ssize_t ignored = write(2, text, static_cast<std::size_t>(n));
    (void)ignored;
}

void on_stack_request(int) {
    const int saved = errno;
    char name[32] = {};
    prctl(PR_GET_NAME, name, 0, 0, 0);
    ssize_t ignored = write(2, "[watchdog] thread ", 18);
    write_number(syscall(SYS_gettid));
    ignored = write(2, " (", 2);
    ignored = write(2, name, std::strlen(name));
    ignored = write(2, "):\n", 3);
    (void)ignored;
#ifdef __GLIBC__
    void *frames[48];
    const int count = backtrace(frames, 48);
    backtrace_symbols_fd(frames, count, 2);
#endif
    g_stack_done.fetch_add(1);
    errno = saved;
}

void report_threads(std::uint64_t frame, int stalled_seconds) {
    std::fprintf(stderr, "[watchdog] no new frame for %d s (stuck at frame %llu); the stack of every thread, as addresses in ps2run (addr2line -f -C -e ps2run <address>):\n", stalled_seconds,
                 static_cast<unsigned long long>(frame));
    {
        const ps2::hitch::PresentStage &stage = ps2::hitch::present_stage();
        const char *where = stage.name.load(std::memory_order_acquire);
        if (where)
            std::fprintf(stderr, "[watchdog] the window thread has been in \"%s\" for %.1f s\n", where,
                         static_cast<double>(ps2::hitch::now_ns() - stage.since_ns.load(std::memory_order_relaxed)) / 1e9);
        else
            std::fprintf(stderr, "[watchdog] the window thread is not in the hand-over of a picture to the screen\n");
        const char *loop = g_window_stage.load(std::memory_order_acquire);
        if (loop)
            std::fprintf(stderr, "[watchdog] the window thread's loop has been in \"%s\" for %.1f s\n", loop,
                         static_cast<double>(ps2::hitch::now_ns() - g_window_stage_since_ns.load(std::memory_order_relaxed)) / 1e9);
        if (g_card.state.load() == 1) std::fprintf(stderr, "[watchdog] the key card is open (its box runs on a thread of its own)\n");
    }
    std::fflush(stderr);
    const pid_t self = getpid();
    std::error_code error;
    for (const auto &entry : std::filesystem::directory_iterator("/proc/self/task", error)) {
        const long tid = std::strtol(entry.path().filename().string().c_str(), nullptr, 10);
        if (tid <= 0 || tid == syscall(SYS_gettid)) continue;
        const int before = g_stack_done.load();
        if (syscall(SYS_tgkill, self, tid, SIGUSR2) != 0) continue;
        for (int waited = 0; g_stack_done.load() == before && waited < 500; ++waited) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        if (g_stack_done.load() == before) std::fprintf(stderr, "[watchdog] thread %ld did not answer in 1 s (blocked inside the kernel)\n", tid);
    }
}

void watchdog() {
    ps2::crash::enter_thread(ps2::crash::Thread::Watchdog);
    std::signal(SIGUSR2, on_stack_request);
#ifdef __GLIBC__
    void *warm[2];
    backtrace(warm, 2);  // loads the unwinder now, not inside the handler
#endif
    std::uint64_t seen = ps2::frames_run();
    auto since = std::chrono::steady_clock::now();
    bool reported = false;
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        const std::uint64_t frame = ps2::frames_run();
        const auto now = std::chrono::steady_clock::now();
        const int stalled = static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(now - since).count());
        if (g_menu_hold.load()) since = now;  // the game waits for the quick menu: no stall
        if (frame != seen) {
            if (reported) std::fprintf(stderr, "[watchdog] the game runs again at frame %llu after %d s\n", static_cast<unsigned long long>(frame), stalled);
            seen = frame;
            since = now;
            reported = false;
        } else if (!reported && stalled >= kWatchdogSeconds) {
            reported = true;
            report_threads(frame, stalled);
        }
    }
}
}  // namespace
#endif

int run_window(const std::function<int()> &emulate) {
    // rawdanger-recomp: the game's patch options (games\\<name>\\recomp.ini, from RawDangerPC.exe), applied before the game starts
    if (!recomp::loaded() && recomp::load(std::getenv("PS2_RECOMP_OPTIONS"))) recomp::apply_startup(), sync_jp_textures();
    // rawdanger-recomp: offline achievements (PS2_ACHIEVEMENTS=<set file>; unlocks kept in PS2_ACHIEVEMENTS_UNLOCKS or next to the set)
    if (const char *set = std::getenv("PS2_ACHIEVEMENTS")) {
        const char *u = std::getenv("PS2_ACHIEVEMENTS_UNLOCKS");
        achievements::load(set, u && *u ? std::string(u) : std::string(set) + ".unlocked");
    }
    ps2::init_graphics();
    g_gpu_present = attach_gpu();
    std::fprintf(stderr, "[window] %s\n", g_gpu_present ? "presenting through WebGPU" : "presenting through SDL");
    if (!g_gpu_present && !create_renderer()) return 1;
    const bool threads = !std::getenv("PS2_ONE_THREAD") && !std::getenv("PS2_TIME") && ps2::g_options.frames_dir.empty();
    if (!threads) {
        ps2::g_options.on_vblank = on_vblank;
        // The game runs on this thread, so while it waits for a live translation (15 to 35 s) it must still take the window's messages, or
        // Windows marks the window "not responding" and ends the program when it is clicked (ee_live.cpp, while_pumping).
        ps2::g_options.keep_alive = [] { poll(); };
        const int result = emulate();
        finish_audio();
#ifdef __APPLE__
        destroy_metal_view();
#endif
        return result;
    }
    ps2::g_options.on_vblank = [] {
        const auto t0 = WaitLog::Clock::now();
        ps2::post_present();
        const auto t1 = WaitLog::Clock::now();
        pace();
        hold_for_menu();
        if (ps2::hitch::on()) {  // PS2_HITCH_LOG=<ms>: a frame of the emulation thread that took long, and what it waited for (docs/hitches.md)
            static auto last = WaitLog::Clock::now();
            static double last_cpu = ps2::hitch::thread_cpu_ms();
            const auto now = WaitLog::Clock::now();
            const double frame_ms = std::chrono::duration<double, std::milli>(now - last).count(), cpu = ps2::hitch::thread_cpu_ms();
            if (frame_ms > ps2::hitch::threshold_ms())
                std::fprintf(stderr, "[hitch] frame %llu took %.1f ms: waited for the window thread %.1f ms, for the sound %.1f ms; the emulation thread itself used %.1f ms of CPU; sound queued %.0f ms\n",
                             static_cast<unsigned long long>(ps2::frames_run()), frame_ms, std::chrono::duration<double, std::milli>(t1 - t0).count(), WaitLog::ms(t1), cpu - last_cpu, queued_ms());
            last = now;
            last_cpu = cpu;
        }
        if (g_frame_log.file)
            g_frame_log.frame(std::chrono::duration<double, std::milli>(t1 - t0).count(), WaitLog::ms(t1), queued_ms());
        if (g_waits.on) {
            g_waits.post += std::chrono::duration<double, std::milli>(t1 - t0).count();
            g_waits.sound += WaitLog::ms(t1);
            g_waits.emulation_frame();
        }
    };
    ps2::start_render_thread();
    if (!std::getenv("PS2_NO_VU_THREAD")) {
        ps2::start_vu_thread();
        std::thread(ps2::run_vu_thread).detach();
    }
#ifdef __linux__
    if (!std::getenv("PS2_NO_WATCHDOG")) std::thread(watchdog).detach();
#endif
    std::atomic<bool> done{false};
    int result = 0;
    ps2::host::BigStackThread emulation([&] {
        result = emulate();
        done.store(true);
        ps2::wake_render_thread();
    });
    const std::function<void(const std::uint32_t *, std::uint32_t, std::uint32_t)> copy = show_pixels, none;
    while (!done.load()) {
        if (ps2::hitch::on()) {  // PS2_HITCH_LOG=<ms>: the window's own event handling
            const auto p0 = WaitLog::Clock::now();
            poll();
            const double p_ms = WaitLog::ms(p0);
            if (p_ms > ps2::hitch::threshold_ms() / 3) std::fprintf(stderr, "[hitch] window event polling took %.1f ms (around frame %llu)\n", p_ms, static_cast<unsigned long long>(ps2::frames_run()));
        } else {
            poll();
        }
        window_stage("drawing the pictures of the emulation (render_pending)");
        if (!g_waits.on) {
            if (ps2::render_pending(g_gpu_present, g_gpu_present ? none : copy, show_speed, 8.0) == ps2::Rendered::idle) {
                window_stage("waiting for the emulation to post work (wait_render_work)");
                ps2::wait_render_work(4);
            }
            continue;
        }
        const auto t0 = WaitLog::Clock::now();
        const bool idle = ps2::render_pending(g_gpu_present, g_gpu_present ? none : copy, show_speed, 8.0) == ps2::Rendered::idle;
        const auto t1 = WaitLog::Clock::now();
        if (idle) {
            window_stage("waiting for the emulation to post work (wait_render_work)");
            ps2::wait_render_work(4);
        }
        g_waits.working.store(g_waits.working.load() + std::chrono::duration<double, std::milli>(t1 - t0).count());
        if (idle) g_waits.idle.store(g_waits.idle.load() + WaitLog::ms(t1));
    }
    emulation.join();
    ps2::flush_memory_card();
    finish_audio();
#ifdef __APPLE__
    destroy_metal_view();
#endif
    if (g_card.state.load() == 0) SDL_Quit();  // a key card still open is on a thread inside SDL: the program ends without taking SDL down under it
    if (ps2::g_options.on_exit) ps2::g_options.on_exit();  // the host's log copy hands over the last lines before the process ends
    std::fflush(nullptr);
    std::_Exit(result);
}
