#pragma once

#include "../gs.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace ps2::gs::gpu {

enum class Mode { off, mirror, primary };

struct DisplaySource {
    std::uint32_t fbp, fbw, psm, dbx, dby, width, height;
    bool halved;
    DisplayOverlay overlay;
};

struct SurfaceTarget {
    enum Kind { X11, Wayland, Canvas, Win32, Metal } kind;
    void *display;
    std::uint64_t window;
    void *surface;
    const char *selector;
    std::uint32_t width, height;
};

extern Mode g_current_mode;  // set by init(); read through mode() on every shape, so it is not a call
inline Mode mode() { return g_current_mode; }
bool init(Mode m);
void draw(const State &s, const Vertex *v, int count);
void image_begin(const State &s);
void image_data(const std::uint8_t *data, std::size_t bytes);
void local_copy(const State &s);
void game_filter(const std::uint32_t *p, std::uint32_t n);
// rawdanger-recomp (v0.10.5): frame generation (interp.wgsl)
void interp_capture(std::uint32_t zbp, std::uint32_t zpsm, std::uint32_t bw, std::uint32_t w, std::uint32_t h, float ofx, float ofy, std::uint32_t draws);
void interp_mark(const float *data, int n);
void set_frame_generation(int mode);
int frame_generation();
void set_frame_generation_view(int view);
// rawdanger-recomp (v0.10.7): pictures at the window's pace (frame generation on, a limit other than 60; 0 none)
void set_fps_limit(int fps);
int fps_limit();
void set_paced_threads(bool on);
bool paced();
bool present_paced();
void paced_counts(std::uint64_t &made, std::uint64_t &taken);
int frame_generation_view();
void set_frame_generation_log(bool on);
bool frame_generation_log();
void dump_presents(const std::string &base, int count);
void game_ao(const std::uint32_t *p);   // rawdanger-recomp: ambient occlusion from the game's depth, before its colour filter, and the wet look (64 words)   // rawdanger-recomp: the game's colour filter in one pass (gs.cpp rdfilter; n words)
void load_clut(std::uint64_t tex0, std::uint64_t texclut);
void flush();
void texflush();
void wait_idle();
void download(std::uint8_t *memory, std::uint32_t offset = 0, std::uint32_t bytes = kMemorySize);
bool download_async(std::uint8_t *memory, std::uint32_t offset, std::uint32_t bytes, std::uint32_t serial);
bool finish_download(std::uint32_t serial);
// The depth round trip: trip_save copies graphics memory offset..offset+bytes (every sample) into one of
// kTripSlots copies, trip_restore puts it back in place of the upload under way (false, and nothing done, unless that upload has had
// no data yet), trip_read gives the copy's bytes as a download would have. trip_supported: the compute path only, not PS2_GPU_HW.
inline constexpr std::uint32_t kTripSlots = 4;
bool trip_supported();
void trip_save(std::uint32_t slot, std::uint32_t offset, std::uint32_t bytes);
bool trip_restore(std::uint32_t slot, std::uint32_t offset, std::uint32_t bytes);
void trip_read(std::uint32_t slot, std::uint32_t bytes, std::uint8_t *out);
void read_state(std::vector<std::uint32_t> &vram, std::uint32_t &scale, std::vector<std::uint32_t> &clut);
void write_state(const std::vector<std::uint32_t> &vram, std::uint32_t scale, const std::vector<std::uint32_t> &clut);
bool display(const DisplaySource &src, std::vector<std::uint32_t> &rgba, std::uint32_t &width, std::uint32_t &height);
bool display_async(const DisplaySource &src, std::vector<std::uint32_t> &rgba, std::uint32_t &width, std::uint32_t &height);
bool attach_surface(const SurfaceTarget &target);
void resize_surface(std::uint32_t width, std::uint32_t height);
bool has_surface();
bool present(const DisplaySource &src);
double busy_ms();
void stats();

}
