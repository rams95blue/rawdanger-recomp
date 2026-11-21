#pragma once

// rawdanger-recomp: offline achievements. The game's memory is checked once per frame against the conditions of an
// achievement set; an achievement unlocks when its conditions become true (after having been false at least once, so a
// loaded save does not unlock everything at once), a note shows it over the picture, and the unlocks are kept in a file
// next to the saves. Nothing goes over the network.
//
// The set comes from PS2_ACHIEVEMENTS=<file>, in either form:
//   - our own text form (RawDangerPC.exe ships one):
//       [id]
//       title = Into the water
//       description = ...
//       points = 5
//       logic = 0xX48F488=21          (the condition, in RetroAchievements' memory syntax, below)
//   - a RetroAchievements cache file of the player's own (RACache\Data\<game id>.json, written by a RetroAchievements
//     emulator after logging in): its "Achievements" list with ID, Title, Description, Points and MemAddr is read as is.
//
// Supported condition syntax (RetroAchievements memory strings, following rcheevos): sizes 0xH 8 bit, 0x / 0x  16 bit,
// 0xW 24 bit, 0xX 32 bit, 0xI 0xJ 0xG 16/24/32 bit big-endian, 0xM..0xT bit 0..7, 0xL / 0xU nibbles, 0xK bit count, fF / fB
// float (little / big endian); prefixes d (delta), p (prior), b (BCD), ~ (inverted); values decimal, -decimal, hFF, f1.5
// (float), {recall}; = != < <= > >=; modifiers * / & ^ % + - on A: B: I: K: lines; hit counts .N. or (N); flags R: P: A:
// B: N: O: C: D: I: Z: (reset next if) K: (remember) M: G: Q: T: (as plain conditions); core and alternatives separated by
// S. Not supported (the achievement is listed but never checked): double / MBF floats (fH fI fM fL).
// PS2_ACHIEVEMENTS_UNOFFICIAL=1 also loads RetroAchievements' unofficial ones (Flags 5; "unofficial = 1" in the text
// form). PS2 addresses are EE main memory (0 ... 0x1FFFFFF).

#include <string>
#include <vector>

namespace achievements {

struct Row {
    std::string title, description;
    std::string icon, icon_locked;  // its pictures (.bmp, PS2_ACHIEVEMENTS_ICONS/<id>.bmp and <id>_lock.bmp; empty: none)
    std::string when;               // when it was unlocked ("2026-11-13 07:30"), empty while locked
    int points = 0;
    bool unlocked = false;
    bool enabled = true;      // switched off by the player: not checked
    bool unofficial = false;  // RetroAchievements' unofficial (test) set, only with PS2_ACHIEVEMENTS_UNOFFICIAL=1
    bool unsupported = false; // its condition uses something this checker does not have: listed, never checked
};

bool load(const char *set_path, const std::string &unlocks_path);  // false: no set
void frame();                                                         // once per video frame: check, unlock
std::vector<Row> rows();                                              // for the menu page
std::string take_note();
std::string take_note_icon();  // the picture of the achievement just unlocked (a .bmp path; empty: none), right after take_note()
bool toggle(std::size_t index, std::string &note);                   // switch one on / off (kept in PS2_ACHIEVEMENTS_OFF)                                              // "Achievement unlocked: ..." once, then empty
bool loaded();
int unlocked_count();
int total_points(bool unlocked_only);

}  // namespace achievements
