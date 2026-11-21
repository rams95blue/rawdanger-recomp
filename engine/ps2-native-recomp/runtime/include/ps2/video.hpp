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
// rawdanger-recomp: the SMAA preset when SMAA is on: 1 low, 2 medium, 3 high, 4 ultra
void set_smaa_quality(unsigned quality);
// rawdanger-recomp: the filter of upscaled 2D art (0 soft, 1 sharp, 2 sharpest), at the next picture
void set_sharp_2d(unsigned sharp);
// rawdanger-recomp: the Japanese release's pictures (texture overrides under jp/) on or off, at once
void set_jp_textures(bool on);
// rawdanger-recomp: the opacity of the cutscenes' black bars (full-width untextured black sprites at the top and bottom edge),
// 0 (left out: the picture fills the screen) to 100 (as the game draws them)
void set_bar_opacity(unsigned percent);
// rawdanger-recomp: note where the game's 2D lies (HUD, texts) so the rain on the lens stays under it (on while drops fall)
void set_ui_boxes(bool on);
void set_debug_text(bool on);
// rawdanger-recomp: Raw Danger's colour filter done in one pass (1, PS2_GAME_FILTER_PASS=1) or drawn the game's way (0, slow); from
// the next frame
void set_game_filter_pass(int mode);
// rawdanger-recomp: ambient occlusion from the game's depth before its colour filter (0 off, 1 light, 2 strong; the GPU only)
void set_ambient_occlusion(int level);
int ambient_occlusion();
int game_filter_pass();   // rawdanger-recomp: a developers' scene is on screen: its font drawn at its own size (gs.cpp, sjis)
// rawdanger-recomp: the keys shown instead of the PlayStation symbols in the game's font (cross, circle, square, triangle; short
// names of 1 to 3 letters, "" keeps the symbol); see gpu.cpp, draw_prompt_caps
void set_button_prompts(const std::string labels[4]);
bool reverb();
}
