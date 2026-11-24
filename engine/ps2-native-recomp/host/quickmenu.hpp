#pragma once

// The quick menu (F1, or Select then Start / Guide on a controller): a panel drawn over the game picture in the window, while the game
// waits. This file only knows the entries, the highlight and how the panel looks; host/window.cpp opens it, holds the game, feeds it the
// keys and does what an entry asks for. The panel is a picture made here on the CPU (a built-in 5 x 8 pixel font, our own), which the
// window lays over the game's picture: the WebGPU present pass (runtime/src/gs/gpu/overlay.hpp) or the SDL renderer. The game itself,
// its saved pictures and its save states never see it.

#include <utility>
#include <cstdint>
#include <string>
#include <vector>

namespace quickmenu {

// rawdanger-recomp: the menu's look. 0: hazard tape, yellow on black (the launcher's English skin); 1: Japanese, blue on white
// (the launcher's Japanese skin). Takes effect at the next paint.
void set_skin(int skin);
// rawdanger-recomp: the game's own font for the "game" skin (2: the look of Raw Danger's Options screen). The file is the font's
// coverage, 8 bits a pixel, after a 12-byte head ("RDF1", width, height); tools\rd_prompts.py writes it. False when not there.
bool load_game_font(const std::string &path);
bool game_font_loaded();
int skin();
// rawdanger-recomp: the size of the pop-ups (achievement, notes, the keys on screen): 0 small, 1 normal, 2 large, 3 huge
void set_popup_size(int size);
int popup_size();

enum class Command {
    none, resume, save, load, look, scale, sharp_2d, fullscreen, keys, quit,
    change_keys,   // the main menu's entry that opens the keys page (the menu switches page by itself)
    key_edit,      // keys page: the highlighted line waits for a key (capturing() is true from now on)
    key_defaults,  // keys page: put back the default keys (asked once, like Quit)
    recomp,        // rawdanger-recomp: the main menu's entry that opens the Recomp options page
    recomp_toggle, // Recomp options page: switch the highlighted option (recomp_row())
    achievements,  // rawdanger-recomp: the main menu's entry that opens the achievements page
    achievement_toggle,  // achievements page: switch the highlighted achievement on / off (recomp_row())
    graphics,      // rawdanger-recomp: the main menu's entry that opens the Graphics page
    widescreen,    // Graphics page: the game's widescreen option (its recomp.ini section with aspect = 16:9)
    menu_game_runs,   // Settings > Display: the game goes on behind this menu (off: it waits)
    popup_size,       // Settings > Display: the size of the pop-ups (small / normal / large / huge)
    field_of_view,    // Settings > Picture: the camera's field of view (80 % to 150 % of the game's)
    ambient_occlusion, // Settings > Picture: shading in creases and corners from the game's depth (off / light / strong)
    filter_pass,      // Settings > Picture: the post processing in one GPU pass or drawn the game's way (to compare)
    keys_on_screen,   // Settings > Controls: the player's keys at the left of the screen while playing (F10)
    pause_unfocused,  // Graphics page: the game waits (this menu opens) when the window goes to the background
    integer_scale, // Graphics and sound page: whole-number enlargement only (even pixel rows)
    antialias,     // Graphics and sound page: off / smooth edges / SMAA
    lens_rain,     // Graphics and sound page: rain drops on the lens while playing (on / off; how hard follows the map)
    master_volume, // Graphics and sound page: the volume of the game's sound (left / right: 10 % steps)
    menu_skin,     // Graphics and sound page: the menu's look (hazard / Japanese)
    window_size,   // Settings > Display: the window's size (presets; windowed only)
    game_blur,     // Settings > Picture: Original PS2 Post Processing Filter (the game's own glow, tint and haze)
    reverb,        // Settings > Sound: the sound chip's reverb (echo of rooms and tunnels)
    sharpen,       // Settings > Picture: contrast-adaptive sharpening (off / low / medium / high)
    black_bars,    // Settings > Picture: the cutscenes' black bars shown or left out
    key_prompts,   // Settings > Controls: the game's texts show the keys (key caps) or the PlayStation symbols
    mouse_look,    // Settings > Controls: the mouse turns the camera while playing
    mouse_speed,   // Settings > Controls: how far the camera turns for a move of the mouse
    mouse_invert,  // Settings > Controls: the mouse's up and down the other way round
    mouse_invert_x, // Settings > Controls: the mouse's left and right the other way round
    language,      // rawdanger-recomp: restart the game in its other language (the launcher's other build; asked once, like Quit)
    back           // a page's Back line
};

// What the entries show; the window fills it in before each drawing.
struct Values {
    std::string look;       // "Sharp + smooth edges"
    int scale_now = 1;      // the inner resolution the game runs at
    int scale_next = 1;     // the one written in keys.ini for the next start
    bool scale_locked = false;  // PS2_SCALE is set: keys.ini does not count
    int sharp_now = 1, sharp_next = 1;  // the 2D art filter (0 soft, 1 sharp, 2 sharpest): this run's, and keys.ini's for the next start
    bool sharp_locked = false;  // PS2_SHARP_2D is set
    bool fullscreen = false;
    int widescreen = -1;           // the game's widescreen option: -1 the game has none, 0 off, 1 on
    bool menu_game_runs = true;    // the game goes on behind this menu
    int popup_size = 1;            // 0 small, 1 normal, 2 large, 3 huge
    int field_of_view = 100;       // % of the game's
    int ambient_occlusion = 1;     // 0 off, 1 light, 2 strong
    int filter_pass = 1;           // 1 one GPU pass, 0 the game's own draws
    bool keys_on_screen = false;   // the keys at the left of the screen while playing
    bool game_waits = true;        // the game waits while this menu is open now (the title says PAUSED)
    bool pause_unfocused = true;   // the menu opens when the window goes to the background
    int master_volume = 100;       // %
    bool integer_scale = false;
    int antialias = 0;             // 0 off, 1 smooth edges, 2 SMAA
    std::string filter;            // the upscale filter's name (raw pixels, soft, sharp, FSR 1)
    int lens_rain = 1;             // 0 off, 1 on
    bool lens_rain_known = true;   // the game says when it is playing (PS2_LENS_WHEN); false: the drops would also fall on menus
    std::string title;             // the game's name for the panel's title ("Raw Danger! Enhanced Edition")
    std::string language_now, language_other;
    int menu_skin = 0;             // 0 hazard, 1 Japanese
    std::string window_size;       // "1600 x 900", or "full screen"
    int game_blur = -1;            // the game's post processing: -1 this build has no switch for it, 0 off, 1 on (as on the console)
    bool reverb = true;
    int key_prompts = -1;
    int black_bars = 100;
    int sharpen = 0;
    int mouse_look = -1;           // -1: this game has no mouse look (PS2_MOUSE_LOOK_YAW), 0 off, 1 on
    double mouse_speed = 1.0;
    bool mouse_invert_y = false, mouse_invert_x = false;          // -1: this game has no prompt font (PS2_PROMPT_FONT_KEY), 0 symbols, 1 keys
    // the keys page: one row per button (label and its keys, in the order of keys.ini) and a message line (what the last change did)
    struct KeyRow {
        std::string label, keys;
    };
    std::vector<KeyRow> key_rows;
    std::string key_note;
    // the Recomp options page (rawdanger-recomp): one row per patch of the game
    struct Icon {  // a small picture: straight RGBA, red in the low byte
        std::vector<std::uint32_t> rgba;
        int width = 0, height = 0;
    };
    struct RecompRow {
        std::string label, state;  // "Widescreen 16:9", "on" / "off" / "built in"
        bool live = false;
        const Icon *icon = nullptr;  // achievements page: its badge (in colour when unlocked, grey while locked)
        std::string detail;          // achievements page: what it asks for, shown for the highlighted line
    };
    std::vector<RecompRow> recomp_rows;
    std::string recomp_note;
    float progress = -1;           // achievements page: share of the points won (0 ... 1), a bar under the title; < 0: none
};

// The panel: straight (not premultiplied) RGBA, one std::uint32_t per pixel with red in the low byte, placed at x, y in window pixels.
struct Picture {
    std::vector<std::uint32_t> rgba;
    int width = 0, height = 0, x = 0, y = 0;
};

class Menu {
public:
    bool is_open() const { return open_; }
    void open();
    // rawdanger-recomp: Settings opened from the game's own Options screen (Select there): drawn in the game's look, and Back or
    // Esc on it gives the game back at once
    void open_native() { open(); native_ = true; page_ = Page::graphics; tab_ = 0; selected_ = 0; top_ = 0; }
    bool native() const { return native_; }
    // the small sign over the game's Options screen that says which button opens these settings
    static void draw_native_hint(int window_width, int window_height, const std::string &text, Picture &out);
    // rawdanger-recomp: one line of the game's own Options list drawn again with other words (x, y, w, h: where its words are, in
    // window pixels; `selected`: the game's cursor is on it, so the lighter fill of a chosen line)
    static void draw_native_label(int x, int y, int w, int h, bool selected, const std::string &text, Picture &out);
    void close() { open_ = false; }
    void move(int delta);                      // up (-1) and down (+1), round at the ends
    Command choose();                          // Enter, Cross, a click: what the highlighted entry does (Quit asks once: the first time it only arms itself)
    Command side() const;                      // left and right: the entries that go through values (look, resolution), else none
    bool quit_armed() const { return quit_armed_; }   // Quit or Language waits for its second Enter
    bool select_at(int x, int y);
    // rawdanger-recomp: a click at x on the highlighted line: -1 on its "<" side, 1 on its ">" side, 0 elsewhere or no arrows
    int click_step(int x) const;
    // --- the keys page (Change keys): the same panel, one line per button ---
    bool on_keys_page() const { return page_ == Page::keys; }
    bool on_recomp_page() const { return page_ == Page::recomp || (page_ == Page::graphics && tab_ == kGameTab); }
    bool on_achievements_page() const { return page_ == Page::achievements; }
    bool on_graphics_page() const { return page_ == Page::graphics; }
    void show_achievements() { main_selected_ = 5; page_ = Page::achievements; selected_ = 0; top_ = 0; }
    void set_recomp_rows(int count) { recomp_rows_ = count; }
    int recomp_row() const { return selected_; }
    void show_recomp() { main_selected_ = 3; page_ = Page::graphics; tab_ = kGameTab; selected_ = 0; top_ = 0; }  // F3: Settings > Game
    // rawdanger-recomp: Settings is one page with tabs (Display, Picture, Sound, Controls, Game); Tab / Q / E / Page Up / Page Down
    // / L1 / R1 or a click on a tab switch them
    static constexpr int kTabs = 5, kGameTab = 4;
    void tab(int delta);
    int tab_index() const { return tab_; }
    bool on_settings_page() const { return page_ == Page::graphics; }
    bool click_tab(int x, int y);   // a click on the tab row: switches to that tab (true), else false
    void set_key_rows(int count) { key_rows_ = count; }  // how many lines the page has (the window knows)
    bool capturing() const { return capturing_; }        // a line waits for the next key or mouse button
    int key_row() const { return selected_; }            // the highlighted line (a button of the controller, in the order of keys.ini)
    void stop_capture() { capturing_ = false; }
    bool back();                                         // from the keys page to the main page; false when already there              // the mouse: highlight the entry under the pointer (window pixels); false when none is
    void draw(int window_width, int window_height, const Values &values, Picture &out);
    // A small panel with one line of text at the top of the window (for a few seconds after the menu is closed: "Saved", "Loaded")
    static void draw_note(int window_width, int window_height, const std::string &text, Picture &out);
    // rawdanger-recomp: the player's keys at the left of the screen while playing (F10); note: a line under it, may be empty
    static void draw_keys_on_screen(int window_width, int window_height, const std::vector<std::pair<std::string, std::string>> &rows,
                                    const std::string &note, Picture &out);
    // rawdanger-recomp: the achievement pop-up - its picture (straight RGBA, red in the low byte; may be empty) beside two lines
    static void draw_toast(int window_width, int window_height, const std::string &header, const std::string &title,
                           const std::vector<std::uint32_t> &icon, int icon_width, int icon_height, Picture &out);

private:
    bool open_ = false;
    int selected_ = 0;
    bool quit_armed_ = false;  // Quit was chosen once; the next Enter ends the game, anything else takes it back
    enum class Page { main, keys, recomp, achievements, graphics } page_ = Page::main;
    int tab_ = 0;
    bool keys_from_settings_ = false;      // the keys page was opened from Settings > Controls: Back returns there
    int tab_x_[kTabs + 1] = {}, tab_y_ = 0, tab_h_ = 0;
    int settings_s_ = 0, settings_w_ = 0, settings_h_ = 0;
    bool native_ = false;   // the Settings panel's last size, so the Game tab has the same panel   // where the last drawing put the tabs (window pixels), for the mouse
    void draw_list(int window_width, int window_height, const Values &values, Picture &out);
    int recomp_rows_ = 0;
    void draw_recomp(int window_width, int window_height, const Values &values, Picture &out);
    int main_selected_ = 0;    // the main page's highlight while the keys page is shown
    int key_rows_ = 0, top_ = 0;  // keys page: number of lines, first line shown
    int keys_shown_ = 0;       // lines the last drawing showed
    bool capturing_ = false;
    bool defaults_armed_ = false;  // "Put back the default keys" was chosen once
    void fit();                // scrolls the keys page so that the highlighted line is shown
    int count() const;         // lines that can be highlighted on the current page
    void draw_keys(int window_width, int window_height, const Values &values, Picture &out);
    int rows_y_ = 0, footer_y_ = 0;
    // where the last drawing put the entries, for the mouse
    int entry_x_ = 0, entry_y_ = 0, entry_w_ = 0, entry_h_ = 0;
    // rawdanger-recomp: where each shown line's "<" and the middle between "<" and ">" are (window pixels; -1: no arrows), so a
    // click left of the middle steps back and right of it forward
    std::vector<int> arrow_lo_, arrow_mid_;
    int arrow_top_ = 0;   // the item the first shown line is
};

}  // namespace quickmenu
