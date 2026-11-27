#pragma once

// The keyboard and mouse layout of a window run (docs/keys.md): which key or mouse button works which button of the console's
// controller, the key file the player can edit (keys.ini), and the key card (the quick menu's Keys entry) made from what is in force.

#include <utility>
#include <string>
#include <vector>

namespace keymap {

// What a key can do. The first 16 are the pad's buttons in the order of the controller layout in window.cpp.
enum Action {
    DpadUp, DpadDown, DpadLeft, DpadRight,
    LeftUp, LeftDown, LeftLeft, LeftRight,      // left stick, pushed all the way
    RightUp, RightDown, RightLeft, RightRight,  // right stick, pushed all the way
    Cross, Circle, Square, Triangle,
    L1, R1, L2, R2, L3, R3,
    Start, Select,
    Roll,  // while held, the direction keys of the left stick push the right stick as well (God of War's evade roll)
    Rage,  // L3 and R3 pressed together, which one key can do and two keys can hardly
    Help,  // a key for the key card (none by default; the quick menu's Keys entry shows it)
    Menu,  // another key for the quick menu (none by default; F1 always opens it), host/quickmenu.cpp
    ActionCount
};

// An input is a number: a keyboard scancode (SDL's, below 512: the key by its place on the keyboard), or one of these.
constexpr int kMouseLeft = 1000, kMouseMiddle = 1001, kMouseRight = 1002, kMouseX1 = 1003, kMouseX2 = 1004;
constexpr int kWheelUp = 1010, kWheelDown = 1011;

struct Config {
    std::vector<int> inputs[ActionCount];
    int mouse_stick = 0;       // 0 the mouse moves no stick, 1 it pushes the left stick, 2 the right stick
    double mouse_speed = 1.0;  // 0.1 to 10
    int mouse_invert = 0;      // 0 none, 1 left-right, 2 up-down, 3 both
    double mouse_deadzone = 0; // 0 to 0.9: how far the stick jumps out as soon as the mouse moves
    int look = -1;             // the picture look (F2) written in keys.ini, -1 when the file has no 'look' line
    int scale = 0;             // the inner resolution (1 to 4 times the console's, or kScaleAuto) used at the next start, 0 when the file has no 'scale' line
    int sharp_2d = -1;         // how upscaled 2D art is filtered (0 soft, 1 sharp, 2 sharpest) at the next start, -1 when the file has no line
    std::string pace;          // the picture pace ('30', '60', 'off', ...; ps2/pace.hpp) used at the next start, "" when the file has no 'pace' line

    int menu_game_runs = 1;    // rawdanger-recomp: the game goes on behind the quick menu (off: it waits, as before)
    int filter_pass = -1;      // rawdanger-recomp: the post processing in one GPU pass (1) or drawn the game's way (0); -1: as the launcher says
    int field_of_view = 100;   // rawdanger-recomp: the camera's field of view, % of the game's (80 to 150; 100 = as on the PS2)
    int ambient_occlusion = 1; // rawdanger-recomp: 0 off, 1 light, 2 strong (the game's depth, before its colour filter)
    int popup_size = 1;        // rawdanger-recomp: pop-ups (achievements, notes, the keys on screen): 0 small, 1 normal, 2 large, 3 huge
    int keys_on_screen = 0;    // rawdanger-recomp: the list of the player's keys stays on the screen while playing (F10)
    int pause_unfocused = 0;   // rawdanger-recomp: the quick menu opens (the game waits) when the window goes to the background (v0.10.3: off by default)
    int master_volume = 100;
    int menu_skin = -1;        // rawdanger-recomp: the F1 menu's look: 0 hazard (yellow on black), 1 Japanese (blue on white, the
                               // launcher's Japanese skin); -1 = from the game's language (PS2_LANGUAGE / PS2_MENU_SKIN)
    int reverb = 1;            // rawdanger-recomp: the sound chip's reverb (0 off)
    int key_prompts = 1;       // rawdanger-recomp: the game's texts show the keys instead of the PlayStation symbols (0: the symbols)
    int black_bars = 100;      // rawdanger-recomp: the cutscenes' black bars, opacity in % (0: left out, the picture fills the screen)
    int sharpen = 0;           // rawdanger-recomp: contrast-adaptive sharpening of the picture: 0 off (the picture is crisp by itself since the copy fix), 1 low, 2 medium, 3 high
    int mouse_look = 1;        // rawdanger-recomp: the mouse turns the camera while playing (PS2_MOUSE_LOOK_* from the launcher); 0 off
    int free_camera = 0;       // rawdanger-recomp: the follow camera also where the game has a fixed one (PS2_FREE_CAMERA); off by default
    int anim_layers = 1;       // rawdanger-recomp: the hero breathes, hunches when wet, shivers, leans into turns; snaps smoothed
    int wet_look = 1;          // rawdanger-recomp: the hero looks wet when the clothes are (PS2_HERO_WET, the GPU); 0 off
    int head_follow = 1;       // rawdanger-recomp: the hero's head turns toward where the camera looks (PS2_HEAD_FOLLOW); 0: as on the PS2
    int camera_orbit = 1;      // rawdanger-recomp: up / down on the camera (stick or mouse) circles the hero (PS2_CAMERA_ORBIT); 0: the game's own
    double look_speed = 1.0;   // rawdanger-recomp: how far the mouse look turns (0.2 to 4; 1.0 by default since v0.8.3)
    int window_w = 0, window_h = 0;  // rawdanger-recomp: the window's size from the menu's presets (0: the default)
    int integer_scale = 0;     // rawdanger-recomp: the picture is enlarged by whole numbers only (every console pixel row the same height)
    int lens_rain = 1;         // rawdanger-recomp: rain drops on the lens while playing (keys.ini rain_on_lens): 0 off, 1 on (how hard
                               // follows the map's rain and its gusts); the old lens_rain line (off / light / heavy) is not read any more
    int smaa = 0;              // rawdanger-recomp: SMAA 1x on the console picture (anti-aliasing), 0 off, 1 on   // rawdanger-recomp: the volume of everything the game plays, 0 to 100 (%)
};

// The picture looks the F2 key goes through. The number is also the video option bits (ps2/video.hpp): bit 0 smooth edges, bit 1 soft, bit 2 sharp (look 4: cubic enlargement + light sharpening; look 5 = sharp + smooth edges).
constexpr int kLookCount = 8;  // rawdanger-recomp: + FSR 1 (6, 7)
constexpr int kDefaultLook = 0;   // rawdanger-recomp: raw pixels (hard square pixels, only their borders blended when enlarged)
constexpr int kScaleAuto = 9;     // keys.ini scale = auto: the inner resolution that fits the screen, chosen at the start
const char *look_name(int look);           // "Soft + smooth edges", for the title and the key card
const char *look_word(int look);           // the word written in keys.ini
// keys.ini text with its 'look' line set to `look` (other lines untouched; the line is added at the end when missing)
std::string with_look(const std::string &file_text, int look);
// the same for the 'scale' line (the quick menu's resolution, used when the game starts the next time)
std::string with_scale(const std::string &file_text, int scale);
// the same for the 'sharp_2d' line (the quick menu's 2D art filter, used when the game starts the next time)
std::string with_sharp_2d(const std::string &file_text, int sharp);
const char *sharp_2d_word(int sharp);  // "soft", "sharp", "sharpest"
std::string with_menu_game_runs(const std::string &file_text, int on);  // the 'menu_game_runs' line
std::string with_popup_size(const std::string &file_text, int size);
std::string with_filter_pass(const std::string &file_text, int fast);
std::string with_field_of_view(const std::string &file_text, int percent);  // the 'field_of_view' line
std::string with_ambient_occlusion(const std::string &file_text, int level);  // the 'ambient_occlusion' line: off / light / strong  // the 'post_processing' line: fast / original    // the 'popup_size' line: small / normal / large / huge
std::string with_keys_on_screen(const std::string &file_text, int on);  // the 'keys_on_screen' line
std::string with_pause_unfocused(const std::string &file_text, int on);  // the 'pause_unfocused' line
std::string with_master_volume(const std::string &file_text, int percent);
std::string with_integer_scale(const std::string &file_text, int on);
std::string with_menu_skin(const std::string &file_text, int skin);
std::string with_reverb(const std::string &file_text, int on);
std::string with_key_prompts(const std::string &file_text, int on);
std::string with_black_bars(const std::string &file_text, int percent);
std::string with_sharpen(const std::string &file_text, int level);
std::string with_mouse_look(const std::string &file_text, int on);
std::string with_camera_orbit(const std::string &file_text, int on);
std::string with_anim_layers(const std::string &file_text, int on);    // the 'anim_layers' line
std::string with_wet_look(const std::string &file_text, int on);    // the 'wet_look' line
std::string with_head_follow(const std::string &file_text, int on);    // the 'head_follow' line
std::string with_free_camera(const std::string &file_text, int on);    // the 'free_camera' line  // the 'camera_orbit' line
std::string with_mouse_speed(const std::string &file_text, double speed);
std::string with_mouse_invert(const std::string &file_text, int invert);
// rawdanger-recomp: a key's name in at most 3 capitals for the caps drawn in the game's font ("E", "SP", "ML", "F10")
std::string short_input_name(int input);
std::string with_window_size(const std::string &file_text, int w, int h);   // the 'menu_skin' line: hazard / japanese
std::string with_lens_rain(const std::string &file_text, int level);
std::string with_smaa(const std::string &file_text, int on);  // the 'master_volume' line

const char *action_name(int action);  // the word used in keys.ini

// The quick menu's "Change keys" page (docs/keys.md): one line for every button of the controller and for roll and rage, in the order of
// keys.ini (the actions 0 to kChangeableCount - 1; help and menu are left to the file).
constexpr int kChangeableCount = Rage + 1;
const char *action_label(int action);                      // "Square", "Left stick Up", "Rage (L3 + R3)"
std::string keys_text(const Config &config, int action);   // "Left Mouse", "Space, Enter, Keypad Enter", "none"
// keys.ini text with the line of `action` set to the keys in `config` (other lines, comments included, untouched; the line is added at the end when missing)
std::string with_inputs(const std::string &file_text, int action, const std::vector<int> &inputs);
// Gives `input` (a key or mouse button) to `action`, which loses the keys it had. A button that already had `input` swaps: it gets the keys
// `action` had. Returns false and a reason in `message` when the input belongs to the program. Otherwise `message` says what happened and
// `changed` holds every action whose keys are different now (the one asked for first).
bool assign_input(Config &config, int action, int input, std::vector<int> &changed, std::string &message);
// The keys of the controller's buttons, roll and rage back to the defaults (the mouse and picture lines are not touched). `changed` as above.
void restore_default_inputs(Config &config, std::vector<int> &changed);
Config defaults();

// Reads the text of a key file on top of `config` (which starts as defaults()). Every problem becomes a line in `problems`; the
// line it is about is ignored, the rest still counts.
void parse(const std::string &text, Config &config, std::vector<std::string> &problems);

std::string input_name(int input);              // "Left Shift", "Left Mouse"
std::string default_file_text();                // keys.ini as written on the first start
std::string card_text(const Config &config);    // the lines of the key card
// rawdanger-recomp: the keys on screen (F10): {what, keys} lines, short ("Move", "W A S D"); the mouse's look when it turns the camera
std::vector<std::pair<std::string, std::string>> screen_rows(const Config &config, bool mouse_camera);
// the program's own keys cannot be given to the game: F1, F2, F5, F8, F9, F11
bool is_reserved(int input);

}  // namespace keymap
