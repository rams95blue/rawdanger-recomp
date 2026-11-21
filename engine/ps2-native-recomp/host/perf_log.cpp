// rawdanger-recomp: see perf_log.hpp
#include "perf_log.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <vector>

namespace perf {
namespace {
using Clock = std::chrono::steady_clock;
Clock::time_point g_start, g_last;
bool g_started = false, g_paused = false;
std::uint64_t g_frames = 0, g_over33 = 0, g_over50 = 0, g_over100 = 0, g_over500 = 0;
double g_counted = 0;                         // seconds of play that were measured (menu time left out)
std::array<std::uint32_t, 10001> g_hist{};   // frame times in 0.1 ms steps, the last one is "1 s or more"
struct Long { double ms; std::uint64_t frame; };
std::vector<Long> g_longest;                  // the 5 longest
}

void pause(bool paused) {
    g_paused = paused;
    g_last = Clock::now();
}

void frame() {
    const auto now = Clock::now();
    if (!g_started) { g_started = true; g_start = g_last = now; return; }
    const double ms = std::chrono::duration<double, std::milli>(now - g_last).count();
    g_last = now;
    if (g_paused) return;
    ++g_frames;
    g_counted += ms / 1000.0;
    g_hist[std::min<std::size_t>(static_cast<std::size_t>(ms * 10.0), g_hist.size() - 1)]++;
    if (ms > 33.4) ++g_over33;
    if (ms > 50.0) ++g_over50;
    if (ms > 100.0) ++g_over100;
    if (ms > 500.0) ++g_over500;
    if (g_longest.size() < 5 || ms > g_longest.back().ms) {
        g_longest.push_back({ms, g_frames});
        std::sort(g_longest.begin(), g_longest.end(), [](const Long &a, const Long &b) { return a.ms > b.ms; });
        if (g_longest.size() > 5) g_longest.pop_back();
    }
}

void write(const std::string &extra) {
    const char *path = std::getenv("PS2_PERF_LOG");
    if (!path || !*path || g_frames < 60) return;
    // the frame time that 99 % of the frames are faster than: the "1 % low" frame rate
    std::uint64_t seen = 0, p99 = 0, p50 = 0;
    for (std::size_t i = 0; i < g_hist.size(); ++i) {
        seen += g_hist[i];
        if (!p50 && seen * 2 >= g_frames) p50 = i;
        if (seen * 100 >= g_frames * 99) { p99 = i; break; }
    }
    const double avg_fps = g_frames / std::max(g_counted, 1e-6);
    const double low_fps = 1000.0 / std::max(p99 / 10.0, 0.1);
    char when[64];
    const std::time_t t = std::time(nullptr);
    std::strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", std::localtime(&t));
    std::ofstream f(path, std::ios::app);
    if (!f) return;
    char line[512];
    std::snprintf(line, sizeof line,
                  "%s  %llu frames in %.0f s of play: %.1f fps average, %.1f fps 1%% low, median frame %.1f ms; slow frames: %llu over 33 ms, %llu over 50 ms, %llu over 100 ms, %llu over 0.5 s\n",
                  when, static_cast<unsigned long long>(g_frames), g_counted, avg_fps, low_fps, p50 / 10.0, static_cast<unsigned long long>(g_over33),
                  static_cast<unsigned long long>(g_over50), static_cast<unsigned long long>(g_over100), static_cast<unsigned long long>(g_over500));
    f << line;
    f << "    longest frames:";
    for (const Long &l : g_longest) {
        std::snprintf(line, sizeof line, " %.0f ms (frame %llu)", l.ms, static_cast<unsigned long long>(l.frame));
        f << line;
    }
    f << "\n";
    if (!extra.empty()) f << "    " << extra << "\n";
    std::fprintf(stderr, "[perf] %.1f fps average, %.1f fps 1%% low, %llu frames over 50 ms; written to %s\n", avg_fps, low_fps,
                 static_cast<unsigned long long>(g_over50), path);
}
}  // namespace perf
