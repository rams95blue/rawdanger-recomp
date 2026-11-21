// rawdanger-recomp: a short performance summary of every run, appended to a log file the player can find (PS2_PERF_LOG, set by
// RawDangerPC.exe to logs\<game>_performance.log next to it): frames, average and 1 % low frame rate, stutters, the longest frames.
#pragma once
#include <string>

namespace perf {
void frame();                         // once per video frame (window.cpp pace())
void pause(bool paused);              // the quick menu holds the game: not counted
void write(const std::string &extra); // at the end of the run (extra: settings worth knowing, one line)
}
