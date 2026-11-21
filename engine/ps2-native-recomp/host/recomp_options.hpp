#pragma once

// rawdanger-recomp: the game's patches as options (the "Recomp options" page of the F1 menu). RawDangerPC.exe writes
// games\<name>\recomp.ini when it builds a game and passes its path in PS2_RECOMP_OPTIONS; this side reads it, applies the
// choices at the start, lets the player switch the live ones in the game, and writes the choices back, so the launcher
// and the menu always show the same thing.
//
// recomp.ini, one [section] per patch section of the game's .pnach:
//   label = Widescreen 16:9        what the menu shows
//   live  = 1                      the patch only writes data: it can be switched while the game runs
//   on    = 1                      the player's choice (live sections); built-in ones are always as built
//   aspect = 16:9                  the section also sets the picture's shape
//   word  = 003A83E0 43E00000 43A00000    address, patched value, original value (live sections)
//   remap_off = 791280 1173090 381810     disc sectors read elsewhere while the option is off (live, no words needed)
// Code patches (live = 0) are compiled into the game: the menu lists them as built in.

#include <string>
#include <vector>
#include <cstdint>

namespace recomp {

struct Word {
    std::uint32_t address = 0, patched = 0, original = 0;
};

struct Section {
    std::string name, label;
    bool live = false, on = true, aspect169 = false;
    std::vector<Word> words;
    // remap_off = FROM TO COUNT: while the option is off, disc reads of sectors FROM.. come from TO.. (the Enhanced Edition's
    // voices: on = Japanese, the area STR.DAT points to; off = English, the same layout further on)
    std::uint32_t remap_from = 0, remap_to = 0, remap_count = 0;
    // remap_on = FROM TO COUNT: the same, while the option is on (the Enhanced Edition's Japanese textures: the disc's index
    // points to the US copies, read from the Japanese copies' area while the option is on)
    bool remap_when_on = false;
    // rawdanger-recomp: remap2_off / remap2_on = FROM TO COUNT: a second area the same option moves (the Enhanced Edition's
    // voice switch also swaps the sound banks with voices in them: Japanese where ROOT.FAT points, English further on)
    std::uint32_t remap2_from = 0, remap2_to = 0, remap2_count = 0;
    bool remap2_when_on = false;
    // rawdanger-recomp: a texture option switched while an area is loaded waits for the next area load (the game keeps
    // where each texture goes from the first load: the other version's pictures read into that place garbled the characters)
    bool pending = false;
    bool applied_on() const { return pending ? !on : on; }
};

bool load(const char *path);                // false when there is no file (then the menu page says so)
void apply_startup();                       // the choices of the file, before the game starts
std::vector<Section> &sections();
bool toggle(std::size_t index, std::string &note);  // live sections: on <-> off now, written back to the file
bool loaded();
// rawdanger-recomp: while set, a switched texture option (a section named "Textures: ...") waits; apply_pending() makes the waiting
// ones take effect (the window calls it while the game loads an area)
void set_defer_textures(bool defer);
bool apply_pending();   // true when one took effect

}  // namespace recomp
