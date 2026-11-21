#pragma once
// rawdanger-recomp: rain drops on the lens while playing (see lens_rain.cpp)
namespace lens_rain {
void set_level(int level);   // 0 off, 1 light, 2 heavy
int level();
bool knows_when();           // PS2_LENS_WHEN is set: drops only while the game is playing
void frame(float seconds);   // once per video frame, on the game's thread (reads its memory)
}
