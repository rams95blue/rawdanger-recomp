#include "dmac.hpp"
#include "gs/gif.hpp"
#include "vif.hpp"
#include "vu_micro.hpp"
#include "gs/gs.hpp"
#include "gs/queue.hpp"
#include "hw.hpp"
#include "iop/iop.hpp"
#include "iop/sif.hpp"
#include "iop/sio2.hpp"
#include "iop/spu2.hpp"
#include "ipu/ipu.hpp"
#include "ps2/crash.hpp"
#include "ps2/disc.hpp"
#include "ps2/ee_live.hpp"
#include "kernel.hpp"
#include "load_stats.hpp"
#include "ps2/generated.hpp"
#include "ps2/memory.hpp"
#include "ps2/os.hpp"
#include "ps2/pace.hpp"
#include "ps2/state.hpp"
#include "ps2/state_request.hpp"
#include "ps2/system.hpp"
#include "ps2/video.hpp"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#include <algorithm>
#include <cmath>
#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
#include <dlfcn.h>
#include <pthread.h>
#endif
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <cstdlib>
#include <filesystem>
#include <iterator>
#include <mutex>
#include <stdexcept>
#ifndef __EMSCRIPTEN__
#include <condition_variable>
#include <ctime>
#include <deque>
#include <initializer_list>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#endif

namespace ps2 {

Cpu g_cpu;
Options g_options;

bool frame_capture_requested(std::uint64_t frame) {
    if (g_options.frame_every && frame % g_options.frame_every == 0) return true;
    return std::binary_search(g_options.frame_capture_frames.begin(), g_options.frame_capture_frames.end(), frame);
}

// What the game's controller port gets: the pad state of one port as the host tells it. The host (window thread) writes g_pad_in at any
// time; the emulation takes it over once per frame (latch_pads), so a run is the same however the host's timing falls. The recording
// (quick_keys.inc) notes what was taken over.
struct PadSample {
    std::uint16_t buttons = 0;
    std::uint8_t axes[4] = {0x80, 0x80, 0x80, 0x80};  // right x, right y, left x, left y (as set_pad_axis numbers them)
    std::uint8_t l2 = 0xFF, r2 = 0xFF;                // pressure of L2 and R2
    bool connected = false;
    bool operator==(const PadSample &o) const {
        return buttons == o.buttons && std::memcmp(axes, o.axes, 4) == 0 && l2 == o.l2 && r2 == o.r2 && connected == o.connected;
    }
};

// the quick keys F5, F8 and F9 (quick_keys.inc, further down in this file)
namespace quick {
// Called by vblank with the pad state the host asked for; a recording notes it, a replay replaces it by the recorded one. `apply` is the
// set of ports (bit 0 = port 1) that must be given to the game: the ones the host changed, and all of them at the first frame.
void pads_hook(PadSample (&pads)[2], unsigned &apply);
// every frame: the checkpoints of a recording or a replay
void frame_check();
bool vblank_tick();
void service();
void finish();
void on_present(const gs::DisplayRegs &regs);
std::string card_note(const std::filesystem::path &state_file);
}

namespace {

constexpr std::int64_t kMaxSlice = 20000;

struct Event {
    int id;
    std::uint64_t at;
    std::function<void()> action;
};

std::uint64_t g_base = 0;
std::int64_t g_given = 0;
std::vector<Event> g_events;
std::atomic<bool> g_halted{false};  // set by the window thread when it is closed, read by the emulation thread (and the vector unit loops)
std::string g_halt_reason;
std::uint64_t g_frames = 0;
std::uint64_t g_iop_synced = 0;
bool g_state_request = false;
bool g_file_state_request = false;  // the request is for --save-state, not (only) for a key
bool g_resumed = false;
std::atomic<int> g_state_operation{0};
std::mutex g_state_mutex;
std::string g_state_path;
void (*g_state_complete)(bool, bool) = nullptr;

std::atomic<std::uint32_t> g_video_options{0};

std::uint64_t next_event_time() {
    std::uint64_t t = UINT64_MAX;
    for (const Event &e : g_events) t = std::min(t, e.at);
    return t;
}

void run_due_events() {
    sync_iop();
    for (;;) {
        const std::uint64_t t = now();
        auto it = std::min_element(g_events.begin(), g_events.end(), [](const Event &a, const Event &b) { return a.at < b.at; });
        if (it == g_events.end() || it->at > t) return;
        auto action = std::move(it->action);
        g_events.erase(it);
        action();
    }
}

void refill(Cpu &c) {
    const std::uint64_t t = now(), next = next_event_time();
    std::int64_t slice = kMaxSlice;
    if (next != UINT64_MAX) slice = std::clamp<std::int64_t>(static_cast<std::int64_t>(next - std::min(next, t)), 1, kMaxSlice);
    set_budget(c, slice);
}

std::uint64_t frame_cycles() { return kEeClock / (g_options.pal ? 50u : 60u); }

void dump_frame() {
    std::vector<std::uint32_t> rgba;
    std::uint32_t w = 0, h = 0;
    if (!gs::display(rgba, w, h)) return;
    char name[32];
    std::snprintf(name, sizeof name, "/frame_%05llu.ppm", static_cast<unsigned long long>(g_frames));
    std::FILE *f = os::open(g_options.frames_dir + name, "wb");
    if (!f) return;
    std::fprintf(f, "P6\n%u %u\n255\n", w, h);
    for (std::uint32_t p : rgba) {
        const std::uint8_t px[3] = {static_cast<std::uint8_t>(p), static_cast<std::uint8_t>(p >> 8), static_cast<std::uint8_t>(p >> 16)};
        std::fwrite(px, 1, 3, f);
    }
    std::fclose(f);
}

void apply_presses() {
    // Once the last press is over the pad is left alone, so the web page's live input works after a scripted start.
    std::uint64_t last = 0;
    for (const Press &p : g_options.presses) last = std::max<std::uint64_t>(last, p.frame + p.frames);
    if (!g_options.presses.empty() && g_frames <= last) {
        std::uint16_t mask = 0;
        std::uint8_t stick = 0;
        for (const Press &p : g_options.presses)
            if (g_frames >= p.frame && g_frames < p.frame + p.frames) {
                mask |= p.buttons;
                stick |= p.stick;
            }
        set_pad_buttons(mask, 0);
        set_pad_axis(2, (stick & 4u) ? 0x00 : (stick & 8u) ? 0xFF : 0x80, 0);
        set_pad_axis(3, (stick & 1u) ? 0x00 : (stick & 2u) ? 0xFF : 0x80, 0);
    }
    if (!g_options.axis_presses.empty()) {
        // Outside a hold the stick rests; within one it takes the given tilt.
        std::uint8_t axes[4] = {0x80, 0x80, 0x80, 0x80};
        for (const AxisPress &p : g_options.axis_presses)
            if (g_frames >= p.frame && g_frames < p.frame + p.frames) axes[p.axis] = p.value;
        for (int a = 0; a < 4; ++a) set_pad_axis(a, axes[a], 0);
    }
}

std::mutex g_pad_lock;
PadSample g_pad_in[2];
unsigned g_pad_dirty = 0;  // ports whose state the host changed since the last frame

void pad_init() {
    static const bool once = [] {
        g_pad_in[0].connected = true;  // port 1 is plugged in from the start (g_plugged in sio2.cpp), port 2 is not
        return true;
    }();
    (void)once;
}

void latch_pads() {
    PadSample pads[2];
    unsigned apply;
    {
        std::lock_guard<std::mutex> hold(g_pad_lock);
        pad_init();
        pads[0] = g_pad_in[0];
        pads[1] = g_pad_in[1];
        apply = g_pad_dirty;
        g_pad_dirty = 0;
    }
    quick::pads_hook(pads, apply);
    // rawdanger-recomp: the memory-card check at start answers itself. PS2_AUTO_CROSS="ADDRESS=VALUE" (Raw Danger: the scene word
    // 0x48F488 = 3, MCCHECK): while the game is there, Cross is down for 6 frames in every 12, counted in the game's own frames, so
    // the "Starting the game without using System data" notice of a card with no system data goes on by itself, however fast the
    // scene runs (PS2_SKIP_SCENE) and whether or not the window has the keyboard (v0.8.9: it was timed by the window's clock before,
    // and could miss)
    {
        static const auto spec = [] {
            std::pair<std::uint32_t, std::uint32_t> r{0, 0};
            if (const char *v = std::getenv("PS2_AUTO_CROSS"); v && *v) {
                unsigned long a = 0, w = 0;
                if (std::sscanf(v, "%lx=%lu", &a, &w) == 2) r = {static_cast<std::uint32_t>(a), static_cast<std::uint32_t>(w)};
            }
            return r;
        }();
        static bool was_on = false;
        const bool here = spec.first && ps2::ld32(spec.first) == spec.second;
        if (here || was_on) {
            const bool down = here && (g_frames / 6) % 2 == 0;
            const std::uint16_t want = down ? static_cast<std::uint16_t>(pads[0].buttons | (1u << 14)) : pads[0].buttons;
            if (down != was_on) apply |= 1u;
            pads[0].buttons = want;
            was_on = down;
        }
    }
    for (int p = 0; p < 2; ++p) {
        if (!(apply & (1u << p))) continue;
        iop::sio2::set_buttons(p, pads[p].buttons);
        for (int a = 0; a < 4; ++a) iop::sio2::set_axis(p, a, pads[p].axes[a]);
        iop::sio2::set_pressure(p, 8, pads[p].l2);
        iop::sio2::set_pressure(p, 9, pads[p].r2);
        iop::sio2::set_connected(p, pads[p].connected);
    }
}

void time_frames() {
    static const char *spec = std::getenv("PS2_TIME");
    if (!spec) return;
    static const std::uint64_t from = std::strtoull(spec, nullptr, 10);
    static const std::uint64_t count = std::strchr(spec, ',') ? std::strtoull(std::strchr(spec, ',') + 1, nullptr, 10) : 500;
    static std::chrono::steady_clock::time_point start;
    if (g_frames == from) {
        gs::flush_and_wait();
        start = std::chrono::steady_clock::now();
    } else if (g_frames == from + count) {
        gs::flush_and_wait();
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        std::fprintf(stderr, "[time] frames %llu-%llu: %.2f ms per frame\n", static_cast<unsigned long long>(from), static_cast<unsigned long long>(from + count), ms / static_cast<double>(count));
    }
}

void apply_patches(bool boot) {
    for (const Patch &p : g_options.patches) {
        if (boot ? p.place == 1 : p.place == 0) continue;
        switch (p.bytes) {
        case 1: st8(p.address, static_cast<std::uint8_t>(p.value)); break;
        case 2: st16(p.address, static_cast<std::uint16_t>(p.value)); break;
        case 4: st32(p.address, static_cast<std::uint32_t>(p.value)); break;
        default: st64(p.address, p.value); break;
        }
    }
}

// PS2_TRACK=address,length (hex, bytes of main memory): at every frame print the words of that range that changed since the
// last frame ("[track] frame N addr value"), the first frame prints them all. For finding what a game keeps in a state variable.
void track_words() {
    static const char *spec = std::getenv("PS2_TRACK");
    static const bool small_only = std::getenv("PS2_TRACK_SMALL") != nullptr;  // only changes between two values below 256 (state numbers, flags)
    static std::uint32_t base = 0, len = 0;
    static std::vector<std::uint32_t> last;
    static bool first = true;
    if (!spec) return;
    if (first) {
        first = false;
        if (std::sscanf(spec, "%x,%x", &base, &len) != 2) len = 0;
        len &= ~3u;
        last.assign(len / 4, 0);
    }
    for (std::uint32_t i = 0; i < len / 4; ++i) {
        const std::uint32_t v = ld32(base + i * 4);
        if (v != last[i] || g_frames == 0) {
            if (small_only && (v >= 256 || last[i] >= 256)) { last[i] = v; continue; }
            std::fprintf(stderr, "[track] frame %llu %08X %08X\n", static_cast<unsigned long long>(g_frames), base + i * 4, v);
            last[i] = v;
        }
    }
}

// PS2_IOP_LOAD=1: every 100 frames, the real time per frame the emulation thread spent on the IOP side (see load_stats.hpp)
void report_load() {
    if (!cost::g_on || g_frames % 100 != 0) return;
    static std::uint64_t last_ns[cost::kParts] = {}, last_count[cost::kParts] = {}, last_cycles = 0, last_bytes = 0, last_frame = 0;
    const double frames = static_cast<double>(g_frames - last_frame);
    if (frames <= 0) return;
    auto ms = [&](int p) { return static_cast<double>(cost::g_ns[p] - last_ns[p]) / 1e6 / frames; };
    auto calls = [&](int p) { return static_cast<double>(cost::g_count[p] - last_count[p]) / frames; };
    std::fprintf(stderr,
                 "[load] frames %llu-%llu, ms per frame: sync_iop %.2f (%.0f calls) = iop run %.2f [interpreter %.2f for %.0f k cycles, jobs %.2f of which sound chip %.2f (%.0f ticks)] + sif pump %.2f; disc %.3f (%.2f reads, %.1f KB)\n",
                 static_cast<unsigned long long>(last_frame), static_cast<unsigned long long>(g_frames), ms(cost::kSync), calls(cost::kSync), ms(cost::kIopRun), ms(cost::kIopExec),
                 static_cast<double>(cost::g_exec_cycles - last_cycles) / 1000.0 / frames, ms(cost::kIopJobs), ms(cost::kSpuTick), calls(cost::kSpuTick), ms(cost::kSifPump), ms(cost::kDisc),
                 calls(cost::kDisc), static_cast<double>(cost::g_disc_bytes - last_bytes) / 1024.0 / frames);
    for (int p = 0; p < cost::kParts; ++p) {
        last_ns[p] = cost::g_ns[p];
        last_count[p] = cost::g_count[p];
    }
    last_cycles = cost::g_exec_cycles;
    last_bytes = cost::g_disc_bytes;
    last_frame = g_frames;
}

void vblank() {
    ++g_frames;
    crash::test_poll(crash::Thread::Game);
    // Offline EE cache preparation: capture only explicitly requested frames.
    // Disabled for normal runs; the filenames retain the frame and address so
    // a headless preparer can associate each image with the scripted input.
    static const auto aot_capture = [] {
        struct Config {
            std::string dir;
            std::vector<std::uint64_t> frames;
            std::vector<std::uint32_t> bases;
        } config;
        config.dir = os::env("PS2_CAPTURE_AOT_DIR");
        const std::string list = os::env("PS2_CAPTURE_AOT_FRAMES");
        const std::string base_list = os::env("PS2_CAPTURE_AOT_BASES");
        if (config.dir.empty() || list.empty() || base_list.empty()) return config;
        const auto parse_list = [](const std::string &text, bool addresses, auto &output) {
            std::size_t at = 0;
            while (at < text.size()) {
                const std::size_t next = text.find(',', at);
                const std::string token = text.substr(at, next == std::string::npos ? next : next - at);
                const char *digits = token.c_str();
                int radix = 10;
                if (addresses && token.size() > 2 && token[0] == '0' &&
                    (token[1] == 'x' || token[1] == 'X')) {
                    digits += 2;
                    radix = 16;
                }
                char *end = nullptr;
                const auto value = std::strtoull(digits, &end, radix);
                if (end == digits || *end) { output.clear(); return false; }
                output.push_back(value);
                if (next == std::string::npos) break;
                at = next + 1;
            }
            return !output.empty();
        };
        if (!parse_list(list, false, config.frames)) {
            std::fprintf(stderr, "[aot-capture] invalid PS2_CAPTURE_AOT_FRAMES list\n");
            return config;
        }
        std::vector<std::uint64_t> parsed_bases;
        if (!parse_list(base_list, true, parsed_bases)) {
            std::fprintf(stderr, "[aot-capture] invalid PS2_CAPTURE_AOT_BASES list\n");
            config.frames.clear();
            return config;
        }
        for (const auto base : parsed_bases) {
            constexpr std::uint32_t unit = 0x4000u;
            if (base == 0 || base > kRamSize - unit || base % unit != 0) {
                std::fprintf(stderr, "[aot-capture] invalid EE RAM base: %llX\n",
                             static_cast<unsigned long long>(base));
                config.frames.clear();
                config.bases.clear();
                return config;
            }
            config.bases.push_back(static_cast<std::uint32_t>(base));
        }
        std::sort(config.frames.begin(), config.frames.end());
        config.frames.erase(std::unique(config.frames.begin(), config.frames.end()), config.frames.end());
        std::sort(config.bases.begin(), config.bases.end());
        config.bases.erase(std::unique(config.bases.begin(), config.bases.end()), config.bases.end());
        return config;
    }();
    if (!aot_capture.frames.empty() &&
        std::binary_search(aot_capture.frames.begin(), aot_capture.frames.end(), g_frames)) {
        std::error_code error;
        std::filesystem::create_directories(aot_capture.dir, error);
        if (error) std::fprintf(stderr, "[aot-capture] cannot create directory: %s\n", error.message().c_str());
        else for (const std::uint32_t base : aot_capture.bases) {
            constexpr std::uint32_t unit = 0x4000u, cap = 0x400000u;
            std::uint32_t size = 0;
            while (size < cap && base + size + unit <= kRamSize &&
                   std::any_of(g_ram + base + size, g_ram + base + size + unit,
                               [](std::uint8_t byte) { return byte != 0; })) size += unit;
            if (!size) continue;
            char filename[80];
            std::snprintf(filename, sizeof filename, "frame_%010llu_%08X.bin",
                          static_cast<unsigned long long>(g_frames), base);
            const auto path = (std::filesystem::path(aot_capture.dir) / filename).string();
            if (std::FILE *file = os::open(path, "wb")) {
                const auto written = std::fwrite(g_ram + base, 1, size, file);
                std::fclose(file);
                std::fprintf(stderr, "[aot-capture] frame %llu base %08X bytes %u written %zu\n",
                             static_cast<unsigned long long>(g_frames), base, size, written);
            } else std::fprintf(stderr, "[aot-capture] cannot write %s\n", path.c_str());
        }
    }
    report_load();
    time_frames();
    apply_presses();
    latch_pads();
    quick::frame_check();
    apply_patches(false);
    track_words();
    ee::live::frame_tick();   // code windows that settled are translated in the background, finished libraries come in here (docs/gvz.md)
    if (g_options.on_vblank) g_options.on_vblank();
    iop::sio2::flush_cards();
    if (g_options.trace_iop_modules && g_frames % 25 == 0) std::fprintf(stderr, "[frame] %llu prims %llu\n", static_cast<unsigned long long>(g_frames), static_cast<unsigned long long>(gs::primitives_drawn()));
    if (!g_options.frames_dir.empty() && frame_capture_requested(g_frames)) dump_frame();
    hw::vblank_start();
    iop::vblank(true);
    schedule(hw::kEventVblankEnd + 100, now() + hblank_cycles() * 22u, [] { iop::vblank(false); });
    kernel::on_vblank_start();
    if (g_options.max_frames && g_frames >= g_options.max_frames) halt("frame limit reached");
    if (g_options.save_state_frame && g_frames == g_options.save_state_frame) g_state_request = g_file_state_request = true;
    if (quick::vblank_tick()) g_state_request = true;  // F5, F8 or F9 was pressed: served at the same safe point
    schedule(hw::kEventVblank, now() + frame_cycles(), vblank);
}

void idle(Cpu &c) {
    std::uint64_t next = next_event_time();
    if (next == UINT64_MAX && iop::idle()) {
        halt("deadlock: every thread waits and no event is pending");
        return;
    }
    next = std::min(next, now() + kEeClock / 2000);
    g_base = std::max(now(), next);
    g_given = 0;
    c.budget = 0;
    run_due_events();
}

}

void save_context(const Cpu &c, ThreadContext &t) {
    std::memcpy(t.r, c.r, sizeof t.r);
    t.hi = c.hi;
    t.lo = c.lo;
    t.sa = c.sa;
    t.pc = c.pc & ~kUnwind;
    std::memcpy(t.f, c.f, sizeof t.f);
    t.facc = c.facc;
    t.fcr31 = c.fcr31;
}

void load_context(Cpu &c, const ThreadContext &t) {
    std::memcpy(c.r, t.r, sizeof t.r);
    c.hi = t.hi;
    c.lo = t.lo;
    c.sa = t.sa;
    c.pc = t.pc;
    std::memcpy(c.f, t.f, sizeof t.f);
    c.facc = t.facc;
    c.fcr31 = t.fcr31;
}

std::uint64_t now() { return g_base + static_cast<std::uint64_t>(g_given - g_cpu.budget); }

std::uint64_t rtc_seconds() {
    if (g_options.rtc_epoch) return g_options.rtc_epoch + now() / kEeClock;
    return static_cast<std::uint64_t>(std::time(nullptr));
}

std::uint64_t hblank_cycles() { return frame_cycles() * 2u / (g_options.pal ? 625u : 525u); }

void set_budget(Cpu &c, std::int64_t cycles) {
    g_base = now();
    g_given = cycles;
    c.budget = static_cast<std::int32_t>(cycles);
}

void schedule(int event, std::uint64_t at, std::function<void()> action) {
    g_events.erase(std::remove_if(g_events.begin(), g_events.end(), [&](const Event &e) { return e.id == event; }), g_events.end());
    g_events.push_back({event, at, std::move(action)});
}

std::uint64_t scheduled_at(int event) {
    for (const Event &e : g_events)
        if (e.id == event) return e.at;
    return UINT64_MAX;
}

bool preempt(Cpu &c) {
    run_due_events();
    refill(c);
    return g_halted || g_state_request || g_state_operation.load(std::memory_order_relaxed) || kernel::must_unwind();
}

void halt(const std::string &reason) {
    if (!g_halted) std::fprintf(stderr, "[ee] halted: %s\n", reason.c_str());
    g_halted = true;
    g_halt_reason = reason;
}

bool halted() { return g_halted; }
const std::string &halt_reason() { return g_halt_reason; }

bool request_state_operation(bool loading, const char *path, void (*complete)(bool, bool)) {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    if (g_state_operation.load(std::memory_order_acquire)) return false;
    g_state_path = path;
    g_state_complete = complete;
    g_state_operation.store(loading ? 2 : 1, std::memory_order_release);
    return true;
}

void set_video_options(std::uint32_t options) { g_video_options.store(options, std::memory_order_relaxed); }
std::uint32_t video_options() { return g_video_options.load(std::memory_order_relaxed); }

namespace {

constexpr char kStateMagic[8] = {'P', 'S', '2', 'S', 'T', 'A', 'T', 'E'};
constexpr std::uint32_t kStateVersion = 10;

std::function<void()> event_action(int id, std::uint64_t at) {
    if (id == hw::kEventVblank) return vblank;
    if (id == hw::kEventVblankEnd + 100) return [] { iop::vblank(false); };
    if (id >= hw::kEventCount) return kernel::alarm_action(id - hw::kEventCount);
    return hw::event_action(id, at);
}

std::atomic<std::uint32_t> g_vu_fence{0};

void sync_vu_thread() {
    if (!gs::queue::vu_active()) return;
    const std::uint32_t seen = g_vu_fence.load();
    gs::queue::vu().post(gs::queue::kVuFence, 0, 0, nullptr, 0);
    while (g_vu_fence.load() == seen) gs::queue::wait(g_vu_fence, seen);
}

void machine_state(state::Stream &s) {
    char magic[8];
    std::memcpy(magic, kStateMagic, sizeof magic);
    s.bytes(magic, sizeof magic);
    std::uint32_t version = kStateVersion;
    s.pod(version);
    if (std::memcmp(magic, kStateMagic, sizeof magic) != 0) throw std::runtime_error("not a state file");
    // Our version 4 differs from upstream's 4 (Cpu and thread layouts); 5 adds upstream's waiting HLE RPC replies; 6 adds the disc stream
    // of the IOP (sceCdSt*, where it is and whether it runs), so a state saved while a film or a music stream plays goes on streaming;
    // 7 adds the SIF RPC endpoints kept per server (guest servers, their order) and the IOP-to-EE request packet ring (SIF RPC rework, PR #27);
    // 8 adds the IOP's low-level hardware timers (AllocHardTimer, SetTimerCompare, SetTimerMode) that raise an interrupt;
    // 9 adds the count of GS FINISH events that wait until the game has cleared CSR.FINISH;
    // 10 adds the picture pace's last picture and a hold that is due (kernel.cpp, ps2/pace.hpp).
    if (version != kStateVersion && !(s.loading() && (version >= 4 && version <= 9))) throw std::runtime_error("state file from another version");
    s.set_version(version);
    s.tag("ee");
    s.pod(g_cpu);
    s.bytes(g_ram, kRamSize);
    s.bytes(g_spr, kScratchpadSize);
    s.pod(g_base);
    s.pod(g_given);
    s.pod(g_frames);
    s.pod(g_iop_synced);
    const bool forced_rate = g_options.rate_forced, forced_pal = g_options.pal;
    s.pod(g_options.pal);
    if (forced_rate) g_options.pal = forced_pal;  // --pal / --ntsc win over the state (a 50 Hz disc can be run at 60 Hz to see what a 60 Hz one asks)
    s.vec(g_events, [&s](Event &e) {
        s.pod(e.id);
        s.pod(e.at);
        if (s.loading()) e.action = event_action(e.id, e.at);
    });
    kernel::state(s);
    hw::state(s);
    dmac::state(s);
    sync_vu_thread();
    vif::state(s);
    vu::state(s);
    ipu::state(s);
    gs::state(s);
    gif::state(s);
    iop::state(s);
    sif::state(s);
    s.tag("end");
}

}

bool save_state(const std::string &path) {
    try {
        state::Writer w;
        machine_state(w);
        // temp file + rename: a crash, a kill or a full disk during the write keeps the state that was there before
        std::string why;
        if (!os::replace_file(path, w.out.data(), w.out.size(), &why)) throw std::runtime_error(why);
        std::fprintf(stderr, "[state] saved frame %llu to %s (%zu KB)\n", static_cast<unsigned long long>(g_frames), path.c_str(), w.out.size() >> 10);
        return true;
    } catch (const std::exception &e) {
        std::fprintf(stderr, "[state] cannot save: %s\n", e.what());
        return false;
    }
}

bool load_state(const std::uint8_t *data, std::size_t size) {
    try {
        state::Reader r(data, size);
        machine_state(r);
        g_resumed = true;
        std::fprintf(stderr, "[state] loaded frame %llu\n", static_cast<unsigned long long>(g_frames));
        return true;
    } catch (const std::exception &e) {
        std::fprintf(stderr, "[state] cannot load: %s\n", e.what());
        return false;
    }
}

bool load_state(const std::string &path) {
    std::ifstream in(os::path(path), std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "[state] cannot open %s\n", path.c_str());
        return false;
    }
    const std::vector<std::uint8_t> data{std::istreambuf_iterator<char>(in), {}};
    if (!load_state(data.data(), data.size())) return false;
    quick::card_note(os::path(path));  // a state made by a quick key has a copy of its memory card beside it; says so if the card differs
    return true;
}

#ifndef __EMSCRIPTEN__
#include "quick_keys.inc"
#else
namespace quick {
void pads_hook(PadSample (&)[2], unsigned &) {}
void frame_check() {}
bool vblank_tick() { return false; }
void service() {}
void finish() {}
void on_present(const gs::DisplayRegs &) {}
std::string card_note(const std::filesystem::path &) { return {}; }
}
bool replay_begin() { return false; }
bool replay_active() { return false; }
int replay_status() { return 0; }
void record_begin_run() {}
void quick_request(QuickKey) {}
bool quick_notice(QuickNotice &) { return false; }
void quick_watchdog() {}
std::string quick_folder(const char *) { return {}; }
std::string quick_newest_state() { return {}; }
#endif

std::uint64_t frame_count() { return g_frames; }

bool load_elf(const std::string &path, std::uint32_t &entry) {
    std::ifstream file(os::path(path), std::ios::binary);
    if (!file) return false;
    const std::vector<std::uint8_t> data{std::istreambuf_iterator<char>(file), {}};
    return load_elf(data, entry);
}

bool load_elf(std::span<const std::uint8_t> data, std::uint32_t &entry) {
    if (data.size() < 52 || std::memcmp(data.data(), "\x7f" "ELF", 4) != 0) return false;
    const auto u32 = [&](std::size_t o) { std::uint32_t v; std::memcpy(&v, data.data() + o, 4); return v; };
    const auto u16 = [&](std::size_t o) { std::uint16_t v; std::memcpy(&v, data.data() + o, 2); return v; };
    entry = u32(24);
    const std::uint32_t phoff = u32(28);
    // The zero fill of every segment comes first and the file contents after it, so a later segment's zero fill (Tekken 5's
    // overlay area is a 24 MiB segment with no file contents that lies over its own loader code) never wipes the contents of an
    // earlier one. Memory is zero at power-on, so this is what a loader that only copies the file contents gives.
    for (const bool copy : {false, true}) {
        for (std::uint32_t i = 0; i < u16(44); ++i) {
            const std::uint64_t ph = phoff + std::uint64_t{i} * u16(42);
            // The ELF comes from the disc: a program header past the end of the file, or a segment whose sums wrap round 4 GiB,
            // would read past the file or write past the 32 MiB of memory. ELF spec: p_offset + p_filesz lies inside the file and
            // the segment (p_vaddr up to p_memsz, and the p_filesz bytes copied) inside memory.
            if (ph > data.size() || data.size() - ph < 24) {
                std::fprintf(stderr, "[elf] damaged ELF: program header %u lies past the end of the file\n", i);
                return false;
            }
            if (u32(ph) != 1) continue;
            // A segment placed in the scratchpad (0x70000000..0x70003FFF) loads there instead of into main memory
            // (follows upstream 8ebb09b); the same checks apply, against the 16 KiB.
            if (const std::uint32_t at = u32(ph + 8); (at >> 14) == (kScratchpadBase >> 14)) {
                const std::uint32_t offset = u32(ph + 4), filesz = u32(ph + 16), memsz = u32(ph + 20);
                const std::uint32_t start = at & (kScratchpadSize - 1);
                if (std::uint64_t{start} + std::max(memsz, filesz) > kScratchpadSize || std::uint64_t{offset} + filesz > data.size()) {
                    std::fprintf(stderr, "[elf] damaged ELF: segment %u does not fit in the file or in the scratchpad\n", i);
                    return false;
                }
                if (copy) std::memcpy(g_spr + start, data.data() + offset, filesz);
                else if (memsz > filesz) std::memset(g_spr + start + filesz, 0, memsz - filesz);
                continue;
            }
            const std::uint32_t offset = u32(ph + 4), vaddr = physical(u32(ph + 8)), filesz = u32(ph + 16), memsz = u32(ph + 20);
            // A segment with no file contents that starts wholly outside main memory has nothing to load and nothing to clear
            // (Pro Evolution Soccer's boot ELFs end with an empty 0x800 byte entry at 0x03000000 or 0x04000000): it is left out,
            // before any write. One that has contents, or one that starts inside memory and runs past its end, stays refused.
            if (filesz == 0 && vaddr >= kRamSize) {
                if (!copy) std::fprintf(stderr, "[elf] segment %u (empty, %u bytes at 0x%08X) lies outside main memory: ignored\n", i, memsz, vaddr);
                continue;
            }
            if (std::uint64_t{vaddr} + std::max(memsz, filesz) > kRamSize || std::uint64_t{offset} + filesz > data.size()) {
                std::fprintf(stderr, "[elf] damaged ELF: segment %u does not fit in the file or in memory\n", i);
                return false;
            }
            if (copy) {
                std::memcpy(g_ram + vaddr, data.data() + offset, filesz);
                watch_span(vaddr, filesz, "boot:load_exec");
            } else if (memsz > filesz) {
                std::memset(g_ram + vaddr + filesz, 0, memsz - filesz);
            }
        }
    }
    return true;
}

void sync_iop() {
    const cost::Timer whole(cost::kSync);
    const std::uint64_t t = now();
    if (t > g_iop_synced) {
        iop::run(t - g_iop_synced);
        g_iop_synced = t;
    }
    const cost::Timer pump_time(cost::kSifPump);
    sif::pump();
}

void request_iop_sync() {
    if (g_cpu.budget > 64) set_budget(g_cpu, 64);
}

// The picture pace (ps2/pace.hpp): PS2_PACE (a window run sets it from the game's keys.ini, PS2_PACE_FROM names the
// file) wins over the built-in list of serials. A replay takes it from the recording instead (quick_keys.inc).
static void setup_pace() {
    const std::string serial = pace::serial_of(g_options.program_path);
    if (const std::string setting = os::env("PS2_PACE"); !setting.empty()) {
        if (const int every = pace::parse(setting)) {
            const std::string from = os::env("PS2_PACE_FROM");
            kernel::pace_setup(every, (from.empty() ? "PS2_PACE=" + setting : from + ": pace = " + setting).c_str());
            return;
        }
        std::fprintf(stderr, "[pace] PS2_PACE=%s is not 30, 20, 15, 60 or off: ignored\n", setting.c_str());
    }
    if (const pace::Listed *l = pace::listed(serial)) {
        kernel::pace_setup(l->every, ("built-in list for " + serial + ", " + l->why).c_str());
        return;
    }
    kernel::pace_setup(1, "");
}

void boot(std::uint32_t entry, const std::vector<std::string> &args) {
    init_watch();
    hw::reset();
    kernel::reset();
    if (!g_options.disc_path.empty() && (!g_options.disc_program || !disc::is_open()) && !disc::open(g_options.disc_path))
        std::fprintf(stderr, "[ee] cannot open disc image %s\n", g_options.disc_path.c_str());
    iop::set_trace(g_options.trace_iop_modules, g_options.trace_iop_hle);
    iop::reset();
    std::vector<std::string> guest_args = args;
    std::size_t device = guest_args.empty() ? 0 : guest_args[0].find(':');
#ifdef _WIN32
    if (device == 1) device = std::string::npos;
#endif
    if (!guest_args.empty() && device == std::string::npos) {
        std::string name = guest_args[0].substr(guest_args[0].find_last_of("/\\") + 1);
        for (char &ch : name) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        guest_args[0] = "cdrom0:\\" + name + ";1";
    }
    std::uint32_t argv = 0x00000800u, payload = argv + 4u * static_cast<std::uint32_t>(guest_args.size() + 1);
    for (std::size_t i = 0; i < guest_args.size(); ++i) {
        store<std::uint32_t>(g_ram + argv + 4 * i, payload);
        std::memcpy(g_ram + payload, guest_args[i].c_str(), guest_args[i].size() + 1);
        payload += static_cast<std::uint32_t>(guest_args[i].size() + 1);
    }
    store<std::uint32_t>(g_ram + argv + 4 * guest_args.size(), 0);
    kernel::start_main_thread(entry, 0, static_cast<std::uint32_t>(guest_args.size()), argv);
    setup_pace();
    apply_patches(true);
    schedule(hw::kEventVblank, frame_cycles(), vblank);
}

// ---------------------------------------------------------------------------------------------------------------------
// Where the three threads run (Windows). The laptop has 2 fast cores (4 logical processors, 0 to 3) and 8 small ones; the
// switches below are for trying placements without a new build, and PS2_CORES=1 shows where each thread really ran.
//   PS2_QOS=1          the runner opts out of Windows' power throttling (EcoQoS) for the process and for each of its three threads
//   PS2_IDEAL=a,b,c    ideal logical processor of the emulation, vector-unit and window threads ('-' leaves one alone)
//   PS2_AFFINITY=a,b,c hex masks of the logical processors each of the same three threads may use ('-' leaves one alone)
//   PS2_PRIO=a,b,c     thread priority (-2 to 2) of the same three
//   PS2_CORES=1        every 3 s: on which logical processors each thread was, from samples taken once per frame or work item
#ifdef _WIN32
extern "C" {
void *__stdcall GetCurrentThread(void);
void *__stdcall GetCurrentProcess(void);
unsigned long __stdcall SetThreadIdealProcessor(void *, unsigned long);
unsigned long long __stdcall SetThreadAffinityMask(void *, unsigned long long);
int __stdcall SetThreadPriority(void *, int);
int __stdcall SetThreadInformation(void *, int, void *, unsigned long);
int __stdcall SetProcessInformation(void *, int, void *, unsigned long);
unsigned long __stdcall GetCurrentProcessorNumber(void);
}
#endif

namespace {

enum class Who { Emulation = 0, Vu = 1, Render = 2 };

struct CoreLog {
    std::atomic<std::uint32_t> seen[3][64];
    std::uint64_t last_print = 0;
};
CoreLog g_core_log;

#ifdef _WIN32
std::string tune_item(const char *name, int index) {
    std::string text = os::env(name);
    for (int i = 0; i < index && !text.empty(); ++i) {
        const auto comma = text.find(',');
        text = comma == std::string::npos ? std::string() : text.substr(comma + 1);
    }
    const auto comma = text.find(',');
    return comma == std::string::npos ? text : text.substr(0, comma);
}
#endif

void thread_tune(Who who) {
#ifdef _WIN32
    static const bool cores = !os::env("PS2_CORES").empty();
    (void)cores;
    struct Throttling {
        unsigned long version, control, state;
    };
    if (!os::env("PS2_QOS").empty()) {
        Throttling t{1, 1, 0};  // control EXECUTION_SPEED, state 0: never throttle
        static const bool process = SetProcessInformation(GetCurrentProcess(), 4, &t, sizeof t) != 0;  // ProcessPowerThrottling
        (void)process;
        SetThreadInformation(GetCurrentThread(), 4, &t, sizeof t);  // ThreadPowerThrottling
    }
    const std::string ideal = tune_item("PS2_IDEAL", static_cast<int>(who));
    if (!ideal.empty() && ideal != "-") SetThreadIdealProcessor(GetCurrentThread(), static_cast<unsigned long>(std::atoi(ideal.c_str())));
    const std::string mask = tune_item("PS2_AFFINITY", static_cast<int>(who));
    if (!mask.empty() && mask != "-") SetThreadAffinityMask(GetCurrentThread(), std::strtoull(mask.c_str(), nullptr, 16));
    const std::string prio = tune_item("PS2_PRIO", static_cast<int>(who));
    if (!prio.empty() && prio != "-") SetThreadPriority(GetCurrentThread(), std::atoi(prio.c_str()));
#else
    (void)who;
#endif
}

// Called once per frame (emulation thread), per work item (vector-unit thread) or per loop (window thread); cheap.
void core_sample(Who who) {
#ifdef _WIN32
    static const bool on = !os::env("PS2_CORES").empty();
    if (!on) return;
    const unsigned long cpu = GetCurrentProcessorNumber();
    g_core_log.seen[static_cast<int>(who)][cpu & 63u].fetch_add(1, std::memory_order_relaxed);
    if (who != Who::Emulation) return;
    const std::uint64_t t = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    if (t - g_core_log.last_print < 3000) return;
    if (g_core_log.last_print) {
        static const char *const kNames[3] = {"emulation", "vector units", "window"};
        for (int w = 0; w < 3; ++w) {
            std::uint32_t total = 0, n[64];
            for (int i = 0; i < 64; ++i) total += n[i] = g_core_log.seen[w][i].exchange(0);
            if (!total) continue;
            std::string line;
            for (int i = 0; i < 64; ++i)
                if (n[i] * 50 >= total) line += " #" + std::to_string(i) + ":" + std::to_string(n[i] * 100 / total) + "%";
            std::fprintf(stderr, "[cores] %-12s%s\n", kNames[w], line.c_str());
        }
    }
    g_core_log.last_print = t;
#else
    (void)who;
#endif
}

}

int run() {
    crash::enter_thread(crash::Thread::Game);
    thread_tune(Who::Emulation);
    Cpu &c = g_cpu;
    const std::uint64_t started_at = now();  // for the share of waiting loops skipped (hw.cpp)
    if (!g_resumed) set_budget(c, kMaxSlice);
    while (!g_halted) {
        core_sample(Who::Emulation);
        if (const int operation = g_state_operation.load(std::memory_order_acquire)) {
            // Request fields stay immutable until completion; never serialize
            // from the browser thread while the emulated CPUs are running.
            const bool loading = operation == 2;
            const bool success = loading ? load_state(g_state_path) : save_state(g_state_path);
            const auto complete = g_state_complete;
            g_state_operation.store(0, std::memory_order_release);
            if (complete) complete(loading, success);
            // A malformed state may have partially replaced machine memory.
            if (loading && !success) halt("Quick load failed. Reload the player to continue safely.");
            if (g_halted) break;
        }
        if (g_state_request) {
            g_state_request = false;
            if (g_file_state_request) {
                g_file_state_request = false;
                save_state(g_options.save_state_path);
            }
            quick::service();
        }
        c.pc &= ~kUnwind;
        run_due_events();
        kernel::service(c);
        if (g_halted) break;
        if (c.pc == kIdle) {
            idle(c);
            continue;
        }
        if (c.budget <= 0) refill(c);
        dispatch(c, 0, 0);
    }
    quick::finish();  // a quick save or a report that is still being written to the disk
    kernel::dump();
    iop::dump();
    sif::dump();
    if (const char *peek = std::getenv("PS2_PEEK")) {
        std::uint32_t at = 0, len = 0;
        if (std::sscanf(peek, "%x,%x", &at, &len) == 2)
            for (std::uint32_t i = 0; i < len; i += 16) {
                std::fprintf(stderr, "[peek] %08X:", at + i);
                for (std::uint32_t j = 0; j < 16 && i + j < len; j += 4) std::fprintf(stderr, " %08X", ld32(at + i + j));
                std::fprintf(stderr, "\n");
            }
    }
    if (const std::string path = os::env("PS2_DUMP"); !path.empty()) {
        if (std::FILE *f = os::open(path, "wb")) {
            std::fwrite(g_ram, 1, kRamSize, f);
            std::fwrite(iop::ram_ptr(0), 1, iop::kRamSize, f);
            std::fwrite(iop::spu2::ram(), 2, 1u << 20, f);
            std::fclose(f);
        }
    }
    gs::stats();
    kernel::pace_report();
    ipu::stats();
    if (const std::string path = os::env("PS2_DUMP_AUDIO"); !path.empty()) iop::spu2::dump_audio(path.c_str());
    if (std::getenv("PS2_TRACE_SPU2")) iop::spu2::dump_voices();
    dmac::stats();
    gif::stats();
    vif::stats();
    if (const std::uint64_t skipped = hw::spin_skipped_cycles())
        std::fprintf(stderr, "[spin] %llu cycles of waiting loops skipped (%.1f %% of the time since this run started)\n", static_cast<unsigned long long>(skipped),
                     100.0 * static_cast<double>(skipped) / static_cast<double>(std::max<std::uint64_t>(now() - started_at, 1)));
    std::fprintf(stderr, "[ee] stopped after %llu frames, %llu cycles: %s\n",
                 static_cast<unsigned long long>(g_frames), static_cast<unsigned long long>(now()), g_halt_reason.c_str());
    return 0;
}

bool display_frame(std::vector<std::uint32_t> &rgba, std::uint32_t &width, std::uint32_t &height) { return gs::display(rgba, width, height); }
bool attach_window(const WindowTarget &t) { return gs::attach_window(static_cast<int>(t.kind), t.display, t.window, t.surface, t.selector, t.width, t.height); }
void resize_window(std::uint32_t width, std::uint32_t height) { gs::resize_window(width, height); }
bool present_frame() { return gs::present(); }
void init_graphics() { gs::init(); }
void start_render_thread() { gs::start_thread(); }
void start_vu_thread() { gs::queue::vu().start(32u << 20); }

void drain_vu_thread() {
    if (gs::queue::role() != gs::queue::Role::Emulation || !gs::queue::vu_active() || gs::queue::vu().empty()) return;
    sync_vu_thread();
}

std::atomic<double> g_vu_idle_ms{0.0};

double vu_thread_idle_ms() { return g_vu_idle_ms.load(); }
std::uint64_t vu1_interpreted_calls() { return vu::interpreted_calls(); }

void run_vu_thread() {
    gs::queue::role() = gs::queue::Role::Vu;
    gs::queue::Ring &in = gs::queue::vu();
    crash::enter_thread(crash::Thread::VectorUnit);
    thread_tune(Who::Vu);
    // PS2_VU_SPIN=<microseconds>: look for new work this long before going to sleep (a sleep and a wake-up cost a system call each)
    static const long spin_us = std::atol(os::env("PS2_VU_SPIN").c_str());
    // What this thread hands to the drawing thread goes in batches: the packets of the vector programs and of
    // PATH2/3 are published together when the batch reaches Ring::kBatchBytes or this thread runs out of work, every other message
    // (present, download, state, reset, ...) at once with what came before it. The order of the messages stays as it was.
    gs::queue::Ring &out = gs::queue::render();
    static const int batch = [] { const std::string v = os::env("PS2_HANDOVER_BATCH"); return v.empty() ? 2 : std::atoi(v.c_str()); }();
    for (;;) {
        gs::queue::Header h;
        const std::uint8_t *payload = nullptr;
        core_sample(Who::Vu);
        crash::test_poll(crash::Thread::VectorUnit);
        if (batch) out.hold();
        while (in.peek(h, payload)) {
            switch (h.type) {
            case gs::queue::kVif1: vif::transfer_words(1, reinterpret_cast<const std::uint32_t *>(payload), h.arg0); break;
            case gs::queue::kVif1Write: vif::write(1, h.arg0, h.arg1); break;
            case gs::queue::kGif: gif::transfer(static_cast<int>(h.arg0), payload, h.arg1); break;
            case gs::queue::kPath3Idle: gif::vif1_idle(); break;
            case gs::queue::kGifReset:
                gif::drop_held();
                gs::queue::render().post(gs::queue::kGifReset, h.arg0, h.arg1, payload, h.bytes);
                break;
            case gs::queue::kVuReset:
                vif::reset_vu_side();
                vu::reset();
                break;
            case gs::queue::kVuFence:
                g_vu_fence.fetch_add(1);
                gs::queue::notify(g_vu_fence);
                break;
            default: gs::queue::render().post(static_cast<gs::queue::Type>(h.type), h.arg0, h.arg1, payload, h.bytes); break;
            }
            in.pop(h);
            if (batch == 1 || (h.type != gs::queue::kVif1 && h.type != gs::queue::kVif1Write && h.type != gs::queue::kGif && h.type != gs::queue::kPath3Idle)) {
                out.flush();
                if (batch) out.hold();
            }
        }
        out.flush();
        const auto start = std::chrono::steady_clock::now();
        bool found = false;
        if (spin_us > 0) {
            const auto until = start + std::chrono::microseconds(spin_us);
            while (!(found = in.peek(h, payload)) && std::chrono::steady_clock::now() < until) {
#if defined(__x86_64__) || defined(_M_X64)
                __builtin_ia32_pause();
#endif
            }
        }
        if (!found) in.wait_for_work(50);
        g_vu_idle_ms.store(g_vu_idle_ms.load() + std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
    }
}
void post_present() { gs::post_present(); }
std::uint32_t picture_frame() { return gs::present_frame(); }

Rendered render_pending(bool surface, const std::function<void(const std::uint32_t *, std::uint32_t, std::uint32_t)> &copy,
                        const std::function<void()> &before_present, double budget_ms) {
    static std::vector<std::uint32_t> frame;
    static thread_local bool tuned = false;
    if (!tuned) {
        tuned = true;
        crash::enter_thread(crash::Thread::Gpu);
        thread_tune(Who::Render);
    }
    core_sample(Who::Render);
    crash::test_poll(crash::Thread::Gpu);
    return gs::render_pending([&](const gs::DisplayRegs &regs) {
        quick::on_present(regs);  // the picture for an F9 report is taken here, on the thread that may read the GPU
        if (before_present) before_present();
        if (surface) {
            gs::present(regs);
            if (!copy) return;
        }
        std::uint32_t width = 0, height = 0;
        if (copy && (g_options.sync_pictures ? gs::display(regs, frame, width, height) : gs::copy_frame(regs, frame, width, height))) copy(frame.data(), width, height);
    }, budget_ms);
}

void wait_render_work(int timeout_ms) { gs::queue::render().wait_for_work(timeout_ms); }
void wake_render_thread() { gs::queue::render().wake(); }
bool copy_frame(std::vector<std::uint32_t> &rgba, std::uint32_t &width, std::uint32_t &height) { return gs::copy_frame(rgba, width, height); }

bool parse_press(const std::string &spec, Press &press) {
    static const char *const kNames[16] = {"select", "l3", "r3", "start", "up", "right", "down", "left",
                                           "l2", "r2", "l1", "r1", "triangle", "circle", "cross", "square"};
    const auto colon = spec.find(':');
    if (colon == std::string::npos) return false;
    press.frame = std::strtoull(spec.c_str(), nullptr, 10);
    std::string rest = spec.substr(colon + 1);
    press.frames = 6;
    const auto second = rest.find(':');
    if (second != std::string::npos) {
        press.frames = static_cast<std::uint32_t>(std::strtoul(rest.c_str() + second + 1, nullptr, 10));
        rest = rest.substr(0, second);
    }
    press.buttons = 0;
    press.stick = 0;
    static const char *const kStick[4] = {"lup", "ldown", "lleft", "lright"};
    std::size_t start = 0;
    while (start <= rest.size()) {
        const auto plus = rest.find('+', start);
        const std::string name = rest.substr(start, plus == std::string::npos ? std::string::npos : plus - start);
        int bit = -1;
        for (int i = 0; i < 16; ++i)
            if (name == kNames[i]) bit = i;
        int axis = -1;
        for (int i = 0; i < 4; ++i)
            if (name == kStick[i]) axis = i;
        if (bit < 0 && axis < 0) return false;
        if (bit >= 0) press.buttons |= static_cast<std::uint16_t>(1u << bit);
        else press.stick |= static_cast<std::uint8_t>(1u << axis);
        if (plus == std::string::npos) break;
        start = plus + 1;
    }
    return true;
}

bool load_patches(const std::string &path) {
    std::FILE *f = os::open(path, "r");
    if (!f) return false;
    unsigned place = 0, bytes = 0;
    unsigned long address = 0;
    unsigned long long value = 0;
    while (std::fscanf(f, "%u %lx %u %llx", &place, &address, &bytes, &value) == 4)
        g_options.patches.push_back({static_cast<std::uint8_t>(place), static_cast<std::uint8_t>(bytes), static_cast<std::uint32_t>(address), value});
    std::fclose(f);
    return true;
}

bool parse_axis(const std::string &spec, AxisPress &press) {
    static const char *const kNames[4] = {"rx", "ry", "lx", "ly"};
    std::size_t at = 0;
    const auto field = [&](std::string &out) {
        const auto colon = spec.find(':', at);
        if (colon == std::string::npos) return false;
        out = spec.substr(at, colon - at);
        at = colon + 1;
        return true;
    };
    std::string frame, axis, value;
    if (!field(frame) || !field(axis)) return false;
    const auto tail = spec.find(':', at);
    value = spec.substr(at, tail == std::string::npos ? std::string::npos : tail - at);
    press.frame = std::strtoull(frame.c_str(), nullptr, 10);
    press.axis = -1;
    for (int i = 0; i < 4; ++i)
        if (axis == kNames[i]) press.axis = i;
    if (press.axis < 0) return false;
    press.value = static_cast<std::uint8_t>(std::strtoul(value.c_str(), nullptr, 0));
    press.frames = 6;
    if (tail != std::string::npos) press.frames = static_cast<std::uint32_t>(std::strtoul(spec.c_str() + tail + 1, nullptr, 10));
    return press.frame || press.frames;
}

double gpu_busy_ms() { return gs::gpu_busy_ms(); }

#ifdef __EMSCRIPTEN__
EM_ASYNC_JS(void, ps2_browser_yield, (), {
    if (!Module.yieldPort) {
        const channel = new MessageChannel();
        Module.yieldWaiters = [];
        channel.port1.onmessage = () => Module.yieldWaiters.shift()();
        Module.yieldPort = channel.port2;
    }
    await new Promise((resolve) => { Module.yieldWaiters.push(resolve); Module.yieldPort.postMessage(0); });
});
void browser_yield() { ps2_browser_yield(); }
#else
void browser_yield() {}
#endif

std::uint64_t frames_run() { return g_frames; }
std::uint64_t primitives_drawn() { return gs::primitives_drawn(); }
// The host's pad state waits in g_pad_in until the next frame (latch_pads); only the values that change mark a port as changed.
void set_pad_buttons(std::uint16_t pressed, int pad) {
    if (pad < 0 || pad > 1) return;
    std::lock_guard<std::mutex> hold(g_pad_lock);
    pad_init();
    if (g_pad_in[pad].buttons != pressed) g_pad_dirty |= 1u << pad;
    g_pad_in[pad].buttons = pressed;
}
void set_pad_axis(int axis, std::uint8_t value, int pad) {
    if (pad < 0 || pad > 1 || axis < 0 || axis > 3) return;
    std::lock_guard<std::mutex> hold(g_pad_lock);
    pad_init();
    if (g_pad_in[pad].axes[axis] != value) g_pad_dirty |= 1u << pad;
    g_pad_in[pad].axes[axis] = value;
}
void set_pad_pressure(int button, std::uint8_t value, int pad) {
    if (pad < 0 || pad > 1 || (button != 8 && button != 9)) return;
    std::lock_guard<std::mutex> hold(g_pad_lock);
    pad_init();
    std::uint8_t &slot = button == 8 ? g_pad_in[pad].l2 : g_pad_in[pad].r2;
    if (slot != value) g_pad_dirty |= 1u << pad;
    slot = value;
}
void set_pad_connected(int pad, bool connected) {
    if (pad < 0 || pad > 1) return;
    std::lock_guard<std::mutex> hold(g_pad_lock);
    pad_init();
    if (g_pad_in[pad].connected != connected) g_pad_dirty |= 1u << pad;
    g_pad_in[pad].connected = connected;
}
// rawdanger-recomp: the orbit camera. Raw Danger's follow camera (0x177480 on) puts the camera BASE + LOOK over the point it turns
// round (camera +0x354, 0.4, plus the look-at offset +0x3B8, which the mouse and the stick change) and keeps it between a nearest and
// a farthest distance along the ground (+0x360, +0x364: 5 and 6). Up / down on the right stick (the height step at 0x177270) moves
// all of them together: the distances grow by the step and the offset by 1.2 times it, so the camera goes up and back at once, a
// zoom more than a turn. With PS2_CAMERA_ORBIT="POINTER,HEIGHT,LOOK,BASE,CALL,AT" (hex: the word holding the camera block's address,
// the offsets of the height (+0x35C, with the two distances after it), of the look-at offset and of the base height, the host
// call's number and where the game's code calls it; the setting shows only when that word is the call) the pnach section
// [Orbit camera] puts a call to the host where the height step starts (the load of the camera block moved into the call's delay
// slot). The call does the load it stands in for (f2 = the height); with the setting on it reads the camera's angle and distance
// from the height over the pivot and the distance along the ground, turns the angle by the step (f20, about 0.2 a game step at full
// tilt, the game's way round: up on the stick lowers the camera, so the view looks up) and by the mouse's turn, keeps the distance,
// writes the height over the pivot and the distance back (the follow then moves the camera there smoothly, and keeps it under
// ceilings as before), and sets the step to 0 so the game's own height code that follows changes nothing. From 1.2 under the pivot
// (the pivot is the follow point, about 1.5 over the hero's feet) to 75 degrees over it.
namespace {
struct Orbit {
    bool set = false;
    std::uint32_t pointer = 0, height = 0, look = 0, base = 0, call = 0, at = 0;
};
Orbit g_orbit;
std::atomic<bool> g_orbit_on{true};
std::atomic<float> g_orbit_turn{0.0f};
float word_float(std::uint32_t w) { float f; std::memcpy(&f, &w, 4); return f; }
std::uint32_t float_word(float f) { std::uint32_t w; std::memcpy(&w, &f, 4); return w; }
void orbit_call(Cpu &c) {
    const std::uint32_t cam = c.r[2].w[0];   // the delay slot loaded it
    if (cam < 0x100000u || cam >= 0x2000000u) return;
    c.f[2] = ld32(cam + g_orbit.height);
    const float turn_mouse = g_orbit_turn.exchange(0.0f);
    if (!g_orbit_on.load(std::memory_order_relaxed)) return;
    const float step = word_float(c.f[20]);
    const float turn = (std::isfinite(step) ? step * 0.25f : 0.0f) + turn_mouse;
    c.f[20] = 0;
    if (turn == 0.0f) return;
    const float base = word_float(ld32(cam + g_orbit.base)), look = word_float(ld32(cam + g_orbit.look));
    const float near_d = word_float(ld32(cam + g_orbit.height + 4)), far_d = word_float(ld32(cam + g_orbit.height + 8));
    const float over = base + look, along = (near_d + far_d) * 0.5f, slack = std::max(0.0f, (far_d - near_d) * 0.5f);
    const float dist = std::sqrt(over * over + along * along);
    if (!std::isfinite(dist) || dist < 0.5f || along <= 0.0f) return;
    const float now = std::atan2(over, along);
    const float low = -std::asin(std::min(1.2f / dist, 0.99f));
    const float to = std::clamp(now + turn, std::min(low, now), std::max(1.31f, now));
    const float new_over = dist * std::sin(to), new_along = std::max(0.3f, dist * std::cos(to));
    st32(cam + g_orbit.look, float_word(new_over - base));
    st32(cam + g_orbit.height, float_word(new_along));
    st32(cam + g_orbit.height + 4, float_word(std::max(0.2f, new_along - slack)));
    st32(cam + g_orbit.height + 8, float_word(new_along + slack));
    c.f[2] = ld32(cam + g_orbit.height);
}
struct OrbitSetup {
    OrbitSetup() {
        // v0.9.2: without the setting, Raw Danger's (US) own: a build that has the call but was started without RawDangerPC.exe
        // (or by an older one) stopped at the first camera step with "missing code at 0x0FF00320"; the call only runs when the
        // game's code calls it, so it does nothing for any other program
        const char *v = std::getenv("PS2_CAMERA_ORBIT");
        if (!v || !*v) v = "48F4E0,35C,3B8,354,C8,177270";
        unsigned long p = 0, h = 0, l = 0, b = 0, n = 0, at = 0;
        if (std::sscanf(v, "%lx,%lx,%lx,%lx,%lx,%lx", &p, &h, &l, &b, &n, &at) != 6 || n >= kHostCalls) return;
        g_orbit = {true, static_cast<std::uint32_t>(p), static_cast<std::uint32_t>(h), static_cast<std::uint32_t>(l), static_cast<std::uint32_t>(b),
                   static_cast<std::uint32_t>(n), static_cast<std::uint32_t>(at)};
        register_host_call(g_orbit.call, &orbit_call);
    }
};
OrbitSetup g_orbit_setup;
}  // namespace
bool camera_orbit_available() {
    // the build has the call (an older build of the game, made before the pnach section, does not)
    return g_orbit.set && ld32(g_orbit.at) == (0x0C000000u | (((kHostCallBase + 4u * g_orbit.call) >> 2) & 0x03FFFFFFu));
}
void set_camera_orbit(bool on) { g_orbit_on = on; }

// rawdanger-recomp: the follow camera everywhere. Raw Danger keeps one camera block per camera mode (0x5FD7D0 + mode * 0x490, the
// active one's address at POINTER, its mode at +MODE) and switches between them in one function (SETMODE, 0x17F610: a0 the mode,
// f12 the blend); the places that show the scene from a fixed spot use mode 10, set from the map's data, and so do the cutscenes'
// shots. PS2_FREE_CAMERA="CALL,AT,RUN,SETMODE,POINTER,MODE,FIXED,WHEN=VALUE" (hex): the pnach section [Free camera everywhere] makes
// the call that runs the active camera each frame (AT, "jal RUN" with a nop after it) a call to the host, which runs RUN itself;
// before it, with the setting on, while the game is playing (the word WHEN holds VALUE) and the active camera is FIXED, and no
// cutscene bars were drawn for a second (the cutscenes keep their shots), it switches to the follow camera (mode 0) the way the
// game does. Off by default.
namespace {
struct FreeCamera {
    bool set = false;
    std::uint32_t call = 0, at = 0, run = 0, setmode = 0, pointer = 0, mode = 0, fixed = 0, when = 0, when_value = 0;
};
FreeCamera g_free_camera;
std::atomic<bool> g_free_camera_on{false};
void free_camera_call(Cpu &c) {
    const std::uint32_t back = c.r[31].w[0];
    if (g_free_camera_on.load(std::memory_order_relaxed) && ld32(g_free_camera.when) == g_free_camera.when_value) {
        const std::uint32_t cam = ld32(g_free_camera.pointer);
        if (cam >= 0x100000u && cam < 0x2000000u && ld32(cam + g_free_camera.mode) == g_free_camera.fixed && ms_since_cinema_bars() > 1000) {
            static std::uint32_t told = 0;
            if (told < 4) ++told, std::fprintf(stderr, "[camera] a fixed camera left for the follow camera (Fixed cameras: OFF)\n");
            c.r[4].sd[0] = 0;
            c.f[12] = 0;
            c.r[31].sd[0] = static_cast<std::int32_t>(back);
            call(c, g_free_camera.setmode, 1);
        }
    }
    c.r[31].sd[0] = static_cast<std::int32_t>(back);
    call(c, g_free_camera.run, 1);
    c.r[31].sd[0] = static_cast<std::int32_t>(back);
}
struct FreeCameraSetup {
    FreeCameraSetup() {
        const char *v = std::getenv("PS2_FREE_CAMERA");   // without it, Raw Danger's (US) own, as for the orbit camera
        if (!v || !*v) v = "C9,17C9B0,17C250,17F610,48F4E0,3EC,A,48F488=15";
        unsigned long n = 0, at = 0, run = 0, sm = 0, p = 0, m = 0, f = 0, w = 0, wv = 0;
        if (std::sscanf(v, "%lx,%lx,%lx,%lx,%lx,%lx,%lx,%lx=%lx", &n, &at, &run, &sm, &p, &m, &f, &w, &wv) != 9 || n >= kHostCalls) return;
        auto u = [](unsigned long x) { return static_cast<std::uint32_t>(x); };
        g_free_camera = {true, u(n), u(at), u(run), u(sm), u(p), u(m), u(f), u(w), u(wv)};
        register_host_call(g_free_camera.call, &free_camera_call);
        if (const char *on = std::getenv("PS2_FREE_CAMERA_ON"); on && *on == '1') g_free_camera_on = true;   // runs without a window (tests)
    }
};
FreeCameraSetup g_free_camera_setup;
}  // namespace
bool free_camera_available() {
    return g_free_camera.set && ld32(g_free_camera.at) == (0x0C000000u | (((kHostCallBase + 4u * g_free_camera.call) >> 2) & 0x03FFFFFFu));
}
void set_free_camera(bool on) { g_free_camera_on = on; }

// rawdanger-recomp: the hero's head follows the camera. Raw Danger keeps each model as a list of bones (the model at character
// +MODEL, its next-bone link at +4, each bone's parent at +0, the world matrix's rows at +0x10..+0x40); one function (0x323E40)
// walks a model's list and builds each bone's world matrix from its parent's and its own (FN, 0x323FB0). The pnach section [Head
// follows camera] makes that call (AT, "jal FN" with the bone's address set in its delay slot) a call to the host, which builds the
// matrix itself and then, for the hero's head (bone INDEX of the player's model, a bone with no children), turns the head's axes in
// the world toward where the camera looks: by the difference between the camera's heading (the active camera block's place +PLACE
// to the point it looks at +PIVOT) and the head's own (its -Z axis), at most 60 degrees each way and none when the camera looks at
// the hero's face, and up or down by half the camera's pitch, eased over a few frames. Only while playing (WHEN = VALUE) with the
// follow camera (the block's +MODE 0) and no cutscene bars. PS2_HEAD_FOLLOW="CALL,AT,FN,TABLE,PLAYER,MODEL,INDEX,POINTER,MODE,
// PLACE,PIVOT,WHEN=VALUE" (hex), without it Raw Danger's (US) own. On by default (keys.ini head_follow).
namespace {
struct HeadFollow {
    bool set = false;
    std::uint32_t call = 0, at = 0, fn = 0, table = 0, player = 0, model = 0, index = 0, pointer = 0, mode = 0, place = 0, pivot = 0, when = 0, when_value = 0;
};
HeadFollow g_head;
std::atomic<bool> g_head_on{true};
float g_head_yaw = 0.0f, g_head_pitch = 0.0f;
std::atomic<float> g_head_test{0.0f};   // debug (Debug tab): a fixed turn in radians, 0 the camera's
std::uint32_t head_bone() {
    const std::uint32_t pid = ld32(g_head.player);
    if (pid > 7u) return 0;
    const std::uint32_t ch = ld32(g_head.table + 4u * pid);
    if (ch < 0x100000u || ch >= 0x2000000u) return 0;
    const std::uint32_t model = ld32(ch + g_head.model);
    if (model < 0x100000u || model >= 0x2000000u) return 0;
    std::uint32_t n = ld32(model + 4u);
    for (std::uint32_t i = 0; i < g_head.index && n >= 0x100000u && n < 0x2000000u; ++i) n = ld32(n + 4u);
    return n >= 0x100000u && n < 0x2000000u ? n : 0;
}
void rotate_rows(std::uint32_t bone, const float axis[3], float angle) {
    const float s = std::sin(angle), co = std::cos(angle);
    for (int r = 0; r < 3; ++r) {
        const std::uint32_t a = bone + 0x10u + 0x10u * r;
        const float v[3] = {word_float(ld32(a)), word_float(ld32(a + 4)), word_float(ld32(a + 8))};
        const float d = axis[0] * v[0] + axis[1] * v[1] + axis[2] * v[2];
        const float x[3] = {axis[1] * v[2] - axis[2] * v[1], axis[2] * v[0] - axis[0] * v[2], axis[0] * v[1] - axis[1] * v[0]};
        for (int i = 0; i < 3; ++i) st32(a + 4u * i, float_word(v[i] * co + x[i] * s + axis[i] * d * (1.0f - co)));
    }
}
// rawdanger-recomp (v0.9.5): how wet the hero is, and the wet look. Raw Danger keeps the characters' things in a pool (the word ROOT
// holds its address): sets at ROOT + SETS (a list: +0 the entries, +4 an entry's size, +0xC how many), one per character (+0xE the
// character's number, +0xD 0 when in use), each with a chain of things (+0x14 their list, +FIRST the first, +NEXT the next in the
// chain, -1 at the end); a thing worn has +WORN 1 and its wetness at +WET (0 ... MOST: the HUD shows "Dry" at 0 and drops above).
// PS2_HERO_WET="ROOT,SETS,FIRST,ITEMS,NEXT,WORN,WET,PLAYER,MOST,MATRIX,POINTER,PLACE" (hex; MATRIX the game's world-to-screen matrix,
// POINTER / PLACE the active camera block and its place), Raw Danger's (US) own without it.
struct HeroWet {
    bool set = false;
    std::uint32_t root = 0, sets = 0, first = 0, items = 0, next = 0, worn = 0, wet = 0, player = 0, most = 0, matrix = 0, pointer = 0, place = 0;
};
const HeroWet &hero_wet_setup() {
    static const HeroWet h = [] {
        HeroWet r;
        const char *v = std::getenv("PS2_HERO_WET");
        if (!v || !*v) v = "48F730,5C38,24,14,8,1C,30,48F3E4,64,5FD690,48F4E0,160";
        unsigned long x[12] = {};
        if (std::sscanf(v, "%lx,%lx,%lx,%lx,%lx,%lx,%lx,%lx,%lx,%lx,%lx,%lx", &x[0], &x[1], &x[2], &x[3], &x[4], &x[5], &x[6], &x[7], &x[8], &x[9], &x[10],
                        &x[11]) == 12) {
            auto u = [&](int i) { return static_cast<std::uint32_t>(x[i]); };
            r = {true, u(0), u(1), u(2), u(3), u(4), u(5), u(6), u(7), u(8), u(9), u(10), u(11)};
        }
        return r;
    }();
    return h;
}
bool in_ram(std::uint32_t a) { return a >= 0x100000u && a < 0x2000000u; }
// calls `fn(address of the thing)` for each thing the player has; false when there is no player's set
bool head_call_in_place();
template <typename F> bool for_hero_things(F fn) {
    const HeroWet &h = hero_wet_setup();
    // Raw Danger's own addresses only for its US release: given by RawDangerPC.exe, or with [Head follows camera] in place
    static const bool given = std::getenv("PS2_HERO_WET") != nullptr;
    if (!h.set || (!given && !head_call_in_place())) return false;
    const std::uint32_t root = ld32(h.root);
    if (!in_ram(root)) return false;
    const std::uint32_t list = root + h.sets, base = ld32(list), size = ld32(list + 4), count = ld32(list + 0xC);
    const std::uint32_t pid = ld32(h.player);
    if (!in_ram(base) || size == 0 || size > 0x1000 || count > 256) return false;
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint32_t e = base + size * i;
        if (ld8(e + 0xD) != 0 || static_cast<std::uint32_t>(static_cast<std::int16_t>(ld16(e + 0xE))) != pid) continue;
        const std::uint32_t ib = ld32(e + h.items), isz = ld32(e + h.items + 4);
        if (!in_ram(ib) || isz == 0 || isz > 0x1000) return false;
        std::uint32_t n = ld32(e + h.first);
        for (int guard = 0; n != 0xFFFFFFFFu && n < 0x1000u && guard < 64; ++guard) {
            fn(ib + isz * n);
            n = ld32(ib + isz * n + h.next);
        }
        return true;
    }
    return false;
}
std::atomic<float> g_wet_force{[] { const char *v = std::getenv("PS2_WET_FORCE"); return v && *v ? static_cast<float>(std::atof(v)) : -1.0f; }()};   // debug: a wetness forced (0 ... 1), -1 the game's
std::atomic<bool> g_wet_look_on{true};
std::atomic<bool> g_wet_show{[] { const char *v = std::getenv("PS2_WET_SHOW"); return v && *v == '1'; }()};   // debug: the capsules drawn plainly
float g_wet_shown = 0.0f;                   // eased
// the hero's limbs: bone pairs (from, to: the list's order, 0 the model's first bone) and the radius in metres
struct Limb { int from, to; float radius; };
constexpr Limb kLimbs[] = {{4, 6, 0.19f}, {6, 7, 0.09f}, {7, -1, 0.14f}, {16, 18, 0.075f}, {18, 20, 0.06f}, {23, 25, 0.075f}, {25, 27, 0.06f},
                           {8, 9, 0.09f},  {9, 11, 0.075f}, {12, 13, 0.09f}, {13, 15, 0.075f}, {4, -2, 0.24f}};
void update_wet_look(std::uint32_t head) {
    const HeroWet &h = hero_wet_setup();
    if (!h.set) return;
    float wet = 0.0f;
    for_hero_things([&](std::uint32_t t) {
        if (ld8(t + h.worn) == 1) wet = std::max(wet, word_float(ld32(t + h.wet)) / static_cast<float>(h.most ? h.most : 100));
    });
    if (!std::isfinite(wet)) wet = 0.0f;
    if (const float forced = g_wet_force.load(std::memory_order_relaxed); forced >= 0.0f) wet = forced;
    if (!g_wet_look_on.load(std::memory_order_relaxed)) wet = 0.0f;
    g_wet_shown += (std::clamp(wet, 0.0f, 1.0f) - g_wet_shown) * 0.05f;
    const bool show = g_wet_show.load(std::memory_order_relaxed);
    if (g_wet_shown < 0.01f && !show) {
        set_wet_look(0.0f, nullptr, 0);
        return;
    }
    // the bones in the list's order (from the head's model: the head is bone g_head.index)
    std::uint32_t bones[48] = {};
    {
        const std::uint32_t pid = ld32(g_head.player), ch = ld32(g_head.table + 4u * pid), model = ld32(ch + g_head.model);
        std::uint32_t n = ld32(model + 4u);
        for (int i = 0; i < 48 && in_ram(n); ++i) bones[i] = n, n = ld32(n + 4u);
        if (bones[g_head.index < 48 ? g_head.index : 0] != head) return;
    }
    float m[16];
    for (int i = 0; i < 16; ++i) m[i] = word_float(ld32(h.matrix + 4u * i));
    const std::uint32_t cam = ld32(h.pointer);
    if (!in_ram(cam)) return;
    const float cx = word_float(ld32(cam + h.place)), cy = word_float(ld32(cam + h.place + 4)), cz = word_float(ld32(cam + h.place + 8));
    struct P { float x, y, z; bool ok; };
    const auto project = [&](float x, float y, float z) {
        const float px = x * m[0] + y * m[4] + z * m[8] + m[12], py = x * m[1] + y * m[5] + z * m[9] + m[13];
        const float pz = x * m[2] + y * m[6] + z * m[10] + m[14], pw = x * m[3] + y * m[7] + z * m[11] + m[15];
        if (!(pw > 0.05f) || !std::isfinite(px) || !std::isfinite(py)) return P{0, 0, 0, false};
        return P{px / pw, py / pw, pz / pw, true};
    };
    const auto place = [&](int i, float out[3]) {
        if (i < 0 || i >= 48 || !in_ram(bones[i])) return false;
        for (int k = 0; k < 3; ++k) out[k] = word_float(ld32(bones[i] + 0x40u + 4u * k));
        return std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]);
    };
    std::vector<float> caps;
    for (const Limb &l : kLimbs) {
        float a[3], b[3];
        if (!place(l.from, a)) continue;
        if (l.to == -1) b[0] = a[0], b[1] = a[1] - 0.2f, b[2] = a[2];   // the head: from the neck up (y points down)
        else if (l.to == -2) {                                            // the skirt or the coat: down to the tails' ends
            float s[3] = {0, 0, 0};
            int got = 0;
            for (int t = 32; t < 48; t += 3) {
                float e[3];
                if (place(t, e)) s[0] += e[0], s[1] += e[1], s[2] += e[2], ++got;
            }
            if (!got) continue;
            b[0] = s[0] / got, b[1] = s[1] / got, b[2] = s[2] / got;
        } else if (!place(l.to, b)) continue;
        P pa = project(a[0], a[1], a[2]), pb = project(b[0], b[1], b[2]);
        if (!pa.ok || !pb.ok) continue;
        // the radius on the screen, and the depths from the nearest to the farthest side
        float dir[3] = {a[0] - cx, a[1] - cy, a[2] - cz};
        const float dl = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
        if (!(dl > 0.1f)) continue;
        for (float &v : dir) v /= dl;
        float side[3] = {dir[2], 0.0f, -dir[0]};
        const float sl = std::sqrt(side[0] * side[0] + side[2] * side[2]);
        if (sl > 1e-3f) side[0] /= sl, side[2] /= sl; else side[0] = 1.0f, side[2] = 0.0f;
        const P ps = project(a[0] + side[0] * l.radius, a[1], a[2] + side[2] * l.radius);
        if (!ps.ok) continue;
        const float r = std::hypot(ps.x - pa.x, ps.y - pa.y);
        float znear = 0.0f, zfar = 1e30f;
        for (const float *e : {a, b})
            for (const float sgn : {-1.0f, 1.0f}) {
                const P q = project(e[0] + dir[0] * l.radius * sgn, e[1] + dir[1] * l.radius * sgn, e[2] + dir[2] * l.radius * sgn);
                if (q.ok) znear = std::max(znear, q.z), zfar = std::min(zfar, q.z);
            }
        if (!(zfar < znear)) continue;
        caps.insert(caps.end(), {pa.x, pa.y, pb.x, pb.y, r, znear, zfar});
    }
    set_wet_look(show ? -1.0f : g_wet_shown, caps.data(), static_cast<int>(caps.size() / 7));
}
void head_follow_call(Cpu &c) {
    const std::uint32_t back = c.r[31].w[0];
    const std::uint32_t bone = c.r[4].w[0];
    call(c, g_head.fn, 1);
    c.r[31].sd[0] = static_cast<std::int32_t>(back);
    if (bone == 0 || bone != head_bone()) return;
    update_wet_look(bone);
    float want_yaw = 0.0f, want_pitch = 0.0f;
    const std::uint32_t cam = ld32(g_head.pointer);
    const bool live = g_head_on.load(std::memory_order_relaxed) && ld32(g_head.when) == g_head.when_value && cam >= 0x100000u &&
                      cam < 0x2000000u && ld32(cam + g_head.mode) == 0 && ms_since_cinema_bars() > 1000;
    if (live) {
        const float cx = word_float(ld32(cam + g_head.pivot)) - word_float(ld32(cam + g_head.place));
        const float cy = word_float(ld32(cam + g_head.pivot + 4)) - word_float(ld32(cam + g_head.place + 4));
        const float cz = word_float(ld32(cam + g_head.pivot + 8)) - word_float(ld32(cam + g_head.place + 8));
        const float hx = -word_float(ld32(bone + 0x30)), hz = -word_float(ld32(bone + 0x38));
        const float ground = std::sqrt(cx * cx + cz * cz);
        if (std::isfinite(ground) && ground > 0.05f && hx * hx + hz * hz > 0.01f) {
            float d = std::atan2(cx, cz) - std::atan2(hx, hz);
            while (d > 3.14159265f) d -= 6.28318531f;
            while (d < -3.14159265f) d += 6.28318531f;
            const float limit = 1.05f;
            // past 60 degrees the head stays at the limit; past 120 (the camera in front of the hero) it comes back to the middle
            const float fade = 1.0f - std::clamp((std::fabs(d) - 2.0f) / 0.6f, 0.0f, 1.0f);
            want_yaw = std::clamp(d, -limit, limit) * fade;
            // y grows downward in Raw Danger: a camera looking down has the pivot under (greater y than) its place
            want_pitch = std::clamp(std::atan2(cy, ground) * 0.5f, -0.35f, 0.35f) * fade;
        }
    }
    g_head_yaw += (want_yaw - g_head_yaw) * 0.12f;
    g_head_pitch += (want_pitch - g_head_pitch) * 0.12f;
    if (const char *t = std::getenv("PS2_HEAD_TEST")) {   // tests: a fixed turn "yaw,pitch" in radians
        float y = 0, p = 0;
        if (std::sscanf(t, "%f,%f", &y, &p) == 2) g_head_yaw = y, g_head_pitch = p;
    }
    if (const float t = g_head_test.load(std::memory_order_relaxed); t != 0.0f) g_head_yaw = t, g_head_pitch = 0.0f;   // debug: a fixed turn
    if (std::fabs(g_head_yaw) < 1e-4f && std::fabs(g_head_pitch) < 1e-4f) return;
    const float up[3] = {0.0f, 1.0f, 0.0f};
    rotate_rows(bone, up, g_head_yaw);
    float right[3] = {word_float(ld32(bone + 0x10)), 0.0f, word_float(ld32(bone + 0x18))};
    const float rl = std::sqrt(right[0] * right[0] + right[2] * right[2]);
    if (rl > 0.1f) {
        right[0] /= rl, right[2] /= rl;
        rotate_rows(bone, right, g_head_pitch);
    }
}
struct HeadFollowSetup {
    HeadFollowSetup() {
        const char *v = std::getenv("PS2_HEAD_FOLLOW");
        if (!v || !*v) v = "CA,323EA4,323FB0,5EAC80,48F3E4,F0,7,48F4E0,3EC,160,310,48F488=15";
        unsigned long x[13] = {};
        if (std::sscanf(v, "%lx,%lx,%lx,%lx,%lx,%lx,%lx,%lx,%lx,%lx,%lx,%lx=%lx", &x[0], &x[1], &x[2], &x[3], &x[4], &x[5], &x[6], &x[7], &x[8],
                        &x[9], &x[10], &x[11], &x[12]) != 13 || x[0] >= kHostCalls)
            return;
        auto u = [&](int i) { return static_cast<std::uint32_t>(x[i]); };
        g_head = {true, u(0), u(1), u(2), u(3), u(4), u(5), u(6), u(7), u(8), u(9), u(10), u(11), u(12)};
        register_host_call(g_head.call, &head_follow_call);
        if (const char *on = std::getenv("PS2_HEAD_FOLLOW_ON"); on && *on == '0') g_head_on = false;
    }
};
HeadFollowSetup g_head_setup;
}  // namespace
float hero_wetness() {
    const HeroWet &h = hero_wet_setup();
    float wet = -1.0f;
    for_hero_things([&](std::uint32_t t) {
        if (ld8(t + h.worn) == 1) wet = std::max(wet, word_float(ld32(t + h.wet)) / static_cast<float>(h.most ? h.most : 100));
    });
    return wet;
}
bool set_hero_wetness(float amount) {
    const HeroWet &h = hero_wet_setup();
    const float v = std::clamp(amount, 0.0f, 1.0f) * static_cast<float>(h.most ? h.most : 100);
    bool any = false;
    for_hero_things([&](std::uint32_t t) {
        if (ld8(t + h.worn) == 1) st32(t + h.wet, float_word(v)), any = true;
    });
    return any;
}
void set_wet_look_on(bool on) { g_wet_look_on = on; }
void set_wet_force(float amount) { g_wet_force = amount; }
float wet_force() { return g_wet_force.load(); }
void set_wet_show(bool on) { g_wet_show = on; }
bool wet_show() { return g_wet_show.load(); }
bool head_follow_available() {
    return g_head.set && ld32(g_head.at) == (0x0C000000u | (((kHostCallBase + 4u * g_head.call) >> 2) & 0x03FFFFFFu));
}
namespace {
bool head_call_in_place() { return head_follow_available(); }
}  // namespace
void set_head_follow(bool on) { g_head_on = on; }
void set_head_test(float radians) { g_head_test = radians; }
float head_test() { return g_head_test.load(); }
void add_camera_orbit_turn(float radians) {
    float now = g_orbit_turn.load();
    while (!g_orbit_turn.compare_exchange_weak(now, std::clamp(now + radians, -0.6f, 0.6f))) {}
}

// For tests without a running emulation (host/pad_test.cpp): take over the host's pad state now, as the next frame boundary would.
void latch_pads_now() { latch_pads(); }
std::uint16_t pad_rumble(int pad) { return iop::sio2::rumble(pad); }
void set_audio_sink(std::function<void(const std::int16_t *, std::size_t)> sink) { iop::spu2::set_sink(std::move(sink)); }


// ---------------------------------------------------------------------------------------------------------------- crash information

#ifdef _WIN32
extern "C" {
void *__stdcall GetModuleHandleW(const wchar_t *);
void *__stdcall GetProcAddress(void *, const char *);
void *__stdcall GetCurrentThread();
int __cdecl _close(int);
}
#endif
namespace crash {

namespace {

// the name for the crash report and the shorter one the system shows (Linux keeps 15 characters)
struct ThreadInfo {
    const char *report;
    const char *system;
    const char *test;  // the name PS2_CRASH_TEST uses
};
constexpr ThreadInfo kThreads[] = {
    {"main (window)", "ps2 main", "main"},
    {"emulation (EE)", "ps2 emulation", "game"},
    {"vector unit (VU1)", "ps2 vu1", "vu"},
    {"window and drawing (GPU)", "ps2 gpu", "gpu"},
    {"sound", "ps2 sound", "sound"},
    {"live compiler (EE code)", "ps2 live-ee", "live-ee"},
    {"live compiler (VU programs)", "ps2 live-vu", "live-vu"},
    {"log copy", "ps2 log", "tee"},
    {"watchdog", "ps2 watchdog", "watchdog"},
    {"drawing helper (tiles)", "ps2 tiles", "tiles"},
};
static_assert(sizeof kThreads / sizeof kThreads[0] == static_cast<std::size_t>(Thread::Count));

thread_local const char *t_name = nullptr;
void (*g_hook)() = nullptr;

enum class Kind { access, read, stack, divide, illegal, abort, thrown, param, live };
std::uint64_t g_test_frame = 0;
Kind g_test_kind = Kind::access;
std::string g_test_here;  // main, emu or tee: PS2_CRASH_TEST's thread when it is one of the fixed places

constexpr int kMaxLive = 1024;
LiveLibrary g_live[kMaxLive];
std::atomic<int> g_live_count{0};
std::mutex g_live_mutex;  // only between two loaders; the report reads without it

[[gnu::noinline]] int recurse(int depth) {
    // A frame this big is probed from the top down, so the overflow is found while most of a page is still left (under Wine the
    // handler has no room at all when the last bytes are used up one small frame after the other).
    volatile char pad[1000000];  // not a power of two: the last level then ends well above the limit
    pad[0] = static_cast<char>(depth);
    return recurse(depth + 1) + pad[0];
}

[[noreturn]] void fail(Kind kind) {
    switch (kind) {
    case Kind::access: *reinterpret_cast<volatile int *>(static_cast<std::uintptr_t>(g_test_frame & 0)) = 1; break;
    case Kind::read: {
        volatile int value = *reinterpret_cast<volatile int *>(static_cast<std::uintptr_t>(g_test_frame & 0));
        (void)value;
        break;
    }
    case Kind::stack: std::exit(recurse(0)); break;
    case Kind::divide: {
        volatile int zero = 0, some = 7;  // both unknown: 1 / x alone is turned into a comparison by the compiler, without a division
        std::exit(some / zero);
        break;
    }
    case Kind::illegal: __builtin_trap();
    case Kind::abort: std::abort();
    case Kind::thrown: throw std::runtime_error("PS2_CRASH_TEST");
    case Kind::live: {
        // a fault inside live-compiled code: the newest live library's init function (ps2_ee_live_init / ps2_vu_live_init) is called
        // with null pointers, which it writes through (test_fire waits until a library is loaded)
        int count = 0;
        const LiveLibrary *libraries = live_libraries(count);
        if (count > 0) {
            using InitFn = const void *(*)(const void *, std::uint32_t *);
            std::fprintf(stderr, "%p\n", reinterpret_cast<InitFn>(const_cast<void *>(libraries[count - 1].inside))(nullptr, nullptr));
        }
        break;
    }
    case Kind::param:
#ifdef _WIN32
        _close(12345);  // the C runtime calls its invalid-parameter handler for a file number that is not open
#endif
        break;
    }
    std::abort();  // a kind that did not end the program here (param outside Windows)
}

}  // namespace

int g_test_thread = -1;
std::atomic<std::uint64_t> g_last_syscall{0};

void set_thread_hook(void (*hook)()) { g_hook = hook; }

const char *thread_name() { return t_name; }

void enter_thread(Thread thread) {
    const ThreadInfo &info = kThreads[static_cast<std::size_t>(thread)];
    t_name = info.report;
#ifdef _WIN32
    // SetThreadDescription (Windows 10 1607 on) names the thread for debuggers and profilers; looked up so older systems still start
    using SetDescription = long(__stdcall *)(void *, const wchar_t *);
    if (void *kernel = GetModuleHandleW(L"kernel32.dll"))
        if (const auto set = reinterpret_cast<SetDescription>(GetProcAddress(kernel, "SetThreadDescription"))) {
            wchar_t wide[32];
            std::size_t i = 0;
            for (; info.system[i] && i < 31; ++i) wide[i] = static_cast<unsigned char>(info.system[i]);
            wide[i] = 0;
            set(GetCurrentThread(), wide);
        }
#elif !defined(__EMSCRIPTEN__) && defined(__linux__)
    pthread_setname_np(pthread_self(), info.system);
#elif defined(__APPLE__)
    pthread_setname_np(info.system);  // macOS names only the calling thread
#endif
    if (g_hook) g_hook();
}

Where where() {
    const std::uint64_t call = g_last_syscall.load(std::memory_order_relaxed);
    return Where{g_frames, g_cpu.pc, g_cpu.r[31].w[0], g_cpu.r[29].w[0], kernel::current_thread(), static_cast<std::uint32_t>(call >> 32),
                 static_cast<std::uint32_t>(call), call != 0};
}

void note_live_library(Thread kind, const void *inside) {
    std::lock_guard lock(g_live_mutex);
    const int n = g_live_count.load(std::memory_order_relaxed);
    if (n >= kMaxLive) return;
    std::uint64_t base = 0;
#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
    Dl_info info{};
    if (dladdr(inside, &info) && info.dli_fbase) base = reinterpret_cast<std::uint64_t>(info.dli_fbase);
#endif
    g_live[n] = LiveLibrary{base, inside, kind};
    g_live_count.store(n + 1, std::memory_order_release);
}

const LiveLibrary *live_libraries(int &count) {
    count = g_live_count.load(std::memory_order_acquire);
    return g_live;
}

void test_parse(const char *spec) {
    if (!spec || !*spec) return;
    const char *colon = std::strchr(spec, ':');
    const std::string key = colon ? std::string(spec, colon) : std::string(spec);
    const char *second = colon ? std::strchr(colon + 1, ':') : nullptr;
    const std::string what = colon ? (second ? std::string(colon + 1, second) : std::string(colon + 1)) : std::string();
    g_test_frame = second ? std::strtoull(second + 1, nullptr, 10) : 0;
    static const std::pair<const char *, Kind> kinds[] = {{"access", Kind::access}, {"write", Kind::access}, {"read", Kind::read},
                                                          {"stack", Kind::stack},   {"divide", Kind::divide}, {"illegal", Kind::illegal},
                                                          {"abort", Kind::abort},   {"throw", Kind::thrown},  {"param", Kind::param},
                                                          {"live", Kind::live}};
    bool known_kind = what.empty();
    for (const auto &[name, kind] : kinds)
        if (what == name) {
            g_test_kind = kind;
            known_kind = true;
        }
    int which = -1;
    if (key == "main" || key == "emu" || key == "tee") g_test_here = key;
    else if (key == "window") which = static_cast<int>(Thread::Gpu);
    else
        for (std::size_t i = 0; i < sizeof kThreads / sizeof kThreads[0]; ++i)
            if (key == kThreads[i].test) which = static_cast<int>(i);
    if ((which < 0 && g_test_here.empty()) || !known_kind) {
        g_test_here.clear();
        std::fprintf(stderr, "[crash-test] PS2_CRASH_TEST=%s not understood: <thread>:<kind>[:<frame>], thread main, emu, game, vu, gpu, sound, live-ee, "
                             "live-vu, tee; kind access, read, stack, divide, illegal, abort, throw, param, live\n", spec);
        return;
    }
    g_test_thread = which;
    if (which >= 0)
        std::fprintf(stderr, "[crash-test] the %s thread will fail on purpose at frame %llu or later (PS2_CRASH_TEST=%s)\n", kThreads[which].report,
                     static_cast<unsigned long long>(g_test_frame), spec);
}

void test_fire(Thread thread) {
    if (g_frames < g_test_frame) return;
    if (g_test_kind == Kind::live && g_live_count.load(std::memory_order_acquire) == 0) return;  // no live library yet
    g_test_thread = -1;  // once
    std::fprintf(stderr, "[crash-test] the %s thread fails on purpose now, frame %llu\n", kThreads[static_cast<std::size_t>(thread)].report,
                 static_cast<unsigned long long>(g_frames));
    std::fflush(stderr);
    fail(g_test_kind);
}

void test_here(const char *where) {
    if (g_test_here.empty() || g_test_here != where) return;
    g_test_here.clear();
    std::fprintf(stderr, "[crash-test] fails on purpose now at %s\n", where);
    std::fflush(stderr);
    fail(g_test_kind);
}

}


}
