#include "recomp_options.hpp"

#include "ps2/disc.hpp"
#include "ps2/memory.hpp"
#include "ps2/system.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace recomp {
namespace {

std::vector<Section> g_sections;
std::string g_path;
bool g_loaded = false;

std::string trim(const std::string &s) {
    const auto a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

// Puts the section's words in (or back to the original values) in memory now, and in the patch list the game thread
// writes every frame (patch=1 lines are written again at each frame, so the list has to say the same thing).
void set_words(const Section &s, bool on) {
    for (const Word &w : s.words) {
        const std::uint32_t value = on ? w.patched : w.original;
        for (ps2::Patch &p : ps2::g_options.patches)
            if (p.address == w.address && p.bytes == 4) p.value = value;
        ps2::st32(w.address, value);
    }
}

void save() {
    if (g_path.empty()) return;
    // only the on= lines change: the rest of the file is the launcher's, so it is copied as it is
    std::ifstream in(g_path);
    if (!in) return;
    std::stringstream out;
    std::string line, section;
    while (std::getline(in, line)) {
        const std::string t = trim(line);
        if (t.size() > 2 && t.front() == '[' && t.back() == ']') section = t.substr(1, t.size() - 2);
        else if (t.rfind("on", 0) == 0 && t.find('=') != std::string::npos && trim(t.substr(0, t.find('='))) == "on") {
            for (const Section &s : g_sections)
                if (s.name == section) { line = std::string("on = ") + (s.on ? "1" : "0"); break; }
        }
        out << line << "\n";
    }
    in.close();
    std::ofstream w(g_path, std::ios::trunc);
    w << out.str();
}

// each section with a remap gets its own slot (its place in the file), so the voices and the texture options work together
void apply_remap(const Section &s) {
    const int index = static_cast<int>(&s - g_sections.data());
    if (s.remap_count)
        ps2::disc::set_remap_slot(index % ps2::disc::kRemapSlots, s.remap_from, s.remap_to, s.remap_count, s.remap_when_on ? s.on : !s.on);
    // the second area of a section takes a slot from the other half (the sections with a remap are few: voices and two textures)
    if (s.remap2_count)
        ps2::disc::set_remap_slot((index + ps2::disc::kRemapSlots / 2) % ps2::disc::kRemapSlots, s.remap2_from, s.remap2_to, s.remap2_count,
                                  s.remap2_when_on ? s.on : !s.on);
}

}  // namespace

bool loaded() { return g_loaded; }

namespace {
bool g_defer_textures = false;
}
void set_defer_textures(bool defer) { g_defer_textures = defer; }
bool apply_pending() {
    bool any = false;
    for (Section &s : g_sections)
        if (s.pending) {
            s.pending = false;
            set_words(s, s.on);
            apply_remap(s);
            any = true;
            std::fprintf(stderr, "[recomp] %s: %s from this area load\n", s.label.c_str(), s.on ? "on" : "off");
        }
    return any;
}
std::vector<Section> &sections() { return g_sections; }

bool load(const char *path) {
    g_sections.clear();
    g_loaded = false;
    if (!path || !*path) return false;
    std::ifstream in(path);
    if (!in) {
        std::fprintf(stderr, "[recomp] no options file %s\n", path);
        return false;
    }
    g_path = path;
    std::string line;
    Section *cur = nullptr;
    while (std::getline(in, line)) {
        const std::string t = trim(line);
        if (t.empty() || t[0] == '#' || t[0] == ';') continue;
        if (t.front() == '[' && t.back() == ']') {
            g_sections.push_back(Section());
            cur = &g_sections.back();
            cur->name = cur->label = t.substr(1, t.size() - 2);
            continue;
        }
        const auto eq = t.find('=');
        if (!cur || eq == std::string::npos) continue;
        const std::string k = trim(t.substr(0, eq)), v = trim(t.substr(eq + 1));
        if (k == "label") cur->label = v;
        else if (k == "live") cur->live = v == "1";
        else if (k == "on") cur->on = v != "0";
        else if (k == "aspect") cur->aspect169 = v == "16:9";
        else if (k == "remap_off" || k == "remap_on") {
            unsigned f = 0, t = 0, n = 0;
            if (std::sscanf(v.c_str(), "%u %u %u", &f, &t, &n) == 3) { cur->remap_from = f; cur->remap_to = t; cur->remap_count = n; cur->remap_when_on = k == "remap_on"; }
        }
        else if (k == "remap2_off" || k == "remap2_on") {
            unsigned f = 0, t = 0, n = 0;
            if (std::sscanf(v.c_str(), "%u %u %u", &f, &t, &n) == 3) { cur->remap2_from = f; cur->remap2_to = t; cur->remap2_count = n; cur->remap2_when_on = k == "remap2_on"; }
        }
        else if (k == "word") {
            Word w;
            unsigned a = 0, p = 0, o = 0;
            if (std::sscanf(v.c_str(), "%x %x %x", &a, &p, &o) == 3) {
                w.address = a; w.patched = p; w.original = o;
                cur->words.push_back(w);
            }
        }
    }
    g_loaded = true;
    std::fprintf(stderr, "[recomp] %zu options from %s\n", g_sections.size(), path);
    return true;
}

void apply_startup() {
    for (Section &s : g_sections) {
        if (!s.live) continue;
        // the patch list was read from patches.txt (everything the build turned on); a live section switched off keeps its
        // original values instead, from the first frame on
        for (const Word &w : s.words) {
            bool listed = false;
            for (ps2::Patch &p : ps2::g_options.patches)
                if (p.address == w.address && p.bytes == 4) { p.value = s.on ? w.patched : w.original; listed = true; }
            // a switch variable (recomp.ini "word" of an option whose code the build made switchable, pnach.py toggles) is in no
            // patch file: it becomes a patch written every frame, so the translated code reads the chosen value from the start
            if (!listed) ps2::g_options.patches.push_back(ps2::Patch{1, 4, w.address, s.on ? w.patched : w.original});
        }
        if (s.aspect169) ps2::g_options.widescreen = s.on;
        apply_remap(s);
        std::fprintf(stderr, "[recomp] %s: %s\n", s.label.c_str(), s.on ? "on" : "off");
    }
}

bool toggle(std::size_t index, std::string &note) {
    if (index >= g_sections.size()) return false;
    Section &s = g_sections[index];
    if (!s.live) {
        note = s.label + " is built into the game: change it in RawDangerPC.exe and build again";
        return false;
    }
    s.on = !s.on;
    save();
    if (g_defer_textures && s.name.rfind("Textures:", 0) == 0) {
        s.pending = !s.pending;   // switched back before the load: nothing waits
        note = s.label + (s.on ? ": on" : ": off") + (s.pending ? " from the next area load (a door, a map, a loaded save)" : "");
        std::fprintf(stderr, "[recomp] %s switched %s in the game; %s\n", s.label.c_str(), s.on ? "on" : "off", s.pending ? "waits for the next area load" : "as it was");
        return true;
    }
    s.pending = false;
    set_words(s, s.on);
    if (s.aspect169) ps2::g_options.widescreen = s.on;
    apply_remap(s);
    note = s.label + (s.on ? ": on" : ": off");
    std::fprintf(stderr, "[recomp] %s switched %s in the game\n", s.label.c_str(), s.on ? "on" : "off");
    return true;
}

}  // namespace recomp
