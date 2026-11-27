#pragma once
#include <string>
#include <cstdint>

namespace ps2 {
// Display-only options, independent of emulated GS state and save states.
// bit 0: edge antialiasing.
// bit 2: sharp enlargement (cubic plus light contrast-adaptive sharpening), the other two bits are not used with it.
// bit 1: soft enlargement (a smooth blend everywhere) instead of the sharp one (hard pixels, only their borders blend).
void set_video_options(std::uint32_t options);
std::uint32_t video_options();
// bit 3 (rawdanger-recomp): whole-number scaling, every console row the same height in the window.
// rawdanger-recomp: rain drops on the lens (count drops of x, y, radius, opacity; up to 48), drawn over the picture
// rawdanger-recomp: rain on the lens (host/lens_rain.cpp): count drops of 8 floats (x, y, radius, opacity, trail, stretch, seed,
// 0), how misted the lens is (0 ... 1) and a clock in seconds
void set_lens_drops(const float *drops, unsigned count, float mist, float clock);
// rawdanger-recomp: the sound chip's reverb (the game's echo of rooms and tunnels), on unless the player switches it off
void set_reverb(bool on);
// rawdanger-recomp: the inner resolution while the game runs (1 to 8 times the console's), at the next picture; no restart
void set_internal_scale(unsigned scale);
unsigned internal_scale();
// rawdanger-recomp (v0.10.4): the highest inner resolution the graphics device can hold (Direct3D 12: 4x), and the graphics API in use
// ("Direct3D 12", "Vulkan", ...; "none" before the GPU started or without it)
unsigned max_internal_scale();
const char *graphics_api();
// rawdanger-recomp: the SMAA preset when SMAA is on: 1 low, 2 medium, 3 high, 4 ultra
void set_smaa_quality(unsigned quality);
// rawdanger-recomp: the filter of upscaled 2D art (0 soft, 1 sharp, 2 sharpest), at the next picture
void set_sharp_2d(unsigned sharp);
// rawdanger-recomp: the Japanese release's pictures (texture overrides under jp/) on or off, at once
void set_jp_textures(bool on);
// rawdanger-recomp: the opacity of the cutscenes' black bars (full-width untextured black sprites at the top and bottom edge),
// 0 (left out: the picture fills the screen) to 100 (as the game draws them)
void set_bar_opacity(unsigned percent);
// rawdanger-recomp: milliseconds since the cutscenes' black bars were last drawn (a large number when never; the first call starts the watch)
std::int64_t ms_since_cinema_bars();
// rawdanger-recomp: note where the game's 2D lies (HUD, texts) so the rain on the lens stays under it (on while drops fall)
void set_ui_boxes(bool on);
// rawdanger-recomp (v0.10.5): frame generation: 0 off, 1 smooth (the camera between the last two game steps), 2 predict (carried on
// past the last step). frame_step: a game step began; data: the world-to-screen matrix (16 floats, row-major, row vectors) the pictures
// before it were drawn with, then the hero's limbs (a count, then 8 floats each: number, both ends, radius); n floats in all.
void set_frame_generation(int mode);
int frame_generation();
void frame_step(const float *data, int n);
// Settings > Debug: what frame generation draws (0 the picture, 1 the places nothing landed on in magenta, 2 the depth moved), its log
// lines, and the next presents saved as the window gets them (base.0.ppm, base.1.ppm, ...: after SMAA and frame generation, with
// SMAA's edges and weights, frame generation's depth and keys beside them)
void set_frame_generation_view(int view);
int frame_generation_view();
void set_frame_generation_log(bool on);
bool frame_generation_log();
void dump_presents(const char *base, int count);
void set_debug_text(bool on);
// rawdanger-recomp: Raw Danger's colour filter done in one pass (1, PS2_GAME_FILTER_PASS=1) or drawn the game's way (0, slow); from
// the next frame
void set_game_filter_pass(int mode);
// rawdanger-recomp: ambient occlusion from the game's depth before its colour filter (0 off, 1 light, 2 strong; the GPU only)
void set_ambient_occlusion(int level);
int ambient_occlusion();
// rawdanger-recomp: the wet look: how wet the hero is (0 to 1) and the hero's limbs as capsules in GS window coordinates, 7 floats
// each (x0, y0, x1, y1, radius, nearest depth, farthest depth); darkened in the AO pass (the GPU only)
void set_wet_look(float amount, const float *capsules, int count);
// rawdanger-recomp (v0.9.6, Settings > Debug): what the 3D picture shows: 0 itself, 1 the AO alone (white: none, dark: shaded),
// 2 the depth (near white, far dark, the sky black), 3 the wet look's capsules on the picture (the GPU only)
void set_debug_view(int view);
int debug_view();
int game_filter_pass();   // rawdanger-recomp: a developers' scene is on screen: its font drawn at its own size (gs.cpp, sjis)
// rawdanger-recomp: the keys shown instead of the PlayStation symbols in the game's font (cross, circle, square, triangle; short
// names of 1 to 3 letters, "" keeps the symbol); see gpu.cpp, draw_prompt_caps
void set_button_prompts(const std::string labels[4]);
bool reverb();
}
