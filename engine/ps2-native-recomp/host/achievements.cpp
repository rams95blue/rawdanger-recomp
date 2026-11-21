#include "achievements.hpp"

#include "ps2/memory.hpp"

#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>

namespace achievements {
namespace {

enum class Size { b8, b16, b24, b32, b16be, b24be, b32be, bit0, bit1, bit2, bit3, bit4, bit5, bit6, bit7, low4, high4, bitcount, f32, f32be };
enum class Kind { value, mem, delta, prior, recall };

// A number as RetroAchievements keeps it: an unsigned 32-bit integer (sums wrap) or a float when a float takes part.
struct Num {
    bool f = false;
    std::uint32_t u = 0;
    double d = 0;
    double real() const { return f ? d : static_cast<double>(u); }
};
Num num(std::uint32_t u) { Num n; n.u = u; return n; }
Num numf(double d) { Num n; n.f = true; n.d = d; return n; }

struct Operand {
    Kind kind = Kind::value;
    Size size = Size::b8;
    bool bcd = false, invert = false;
    std::uint32_t address = 0;
    Num value;
    // per-frame memory of a memory operand (delta / prior)
    std::uint32_t last = 0, prior = 0;
    bool seen = false;
};

enum class Flag { none, reset, pause, add_source, sub_source, and_next, or_next, add_hits, sub_hits, add_address, reset_next_if, remember, measured, measured_if, trigger };
enum class Cmp { eq, ne, lt, le, gt, ge, none };
enum class Op { none, mul, div, band, bxor, mod, add, sub };

struct Cond {
    Flag flag = Flag::none;
    Operand left, right;
    Cmp cmp = Cmp::none;
    Op op = Op::none;  // A: B: I: K: lines: left op right
    std::uint32_t target = 0, hits = 0;  // .N. (0: no hit count)
};

struct Group {
    std::vector<Cond> conds;
};

struct Achievement {
    std::string id, title, description, why;  // why: the reason it cannot be checked (empty: it can)
    int points = 0;
    std::vector<Group> groups;  // [0] core, then alternatives
    bool unlocked = false, primed = false;  // primed: seen false once since loading
    bool unofficial = false, enabled = true;
};

std::vector<Achievement> g_list;
std::string g_unlocks_path, g_off_path, g_note, g_note_icon, g_icons_dir;
std::map<std::string, std::string> g_when;  // id -> when it was unlocked (the unlocks file's second column)
std::mutex g_lock;
bool g_loaded = false;

std::string trim(const std::string &s) {
    const auto a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

// ---- reading the conditions ------------------------------------------------------------------------------------------

bool parse_operand(const std::string &t, std::size_t &i, Operand &o) {
    auto hex = [&](std::uint32_t &v) {
        std::size_t start = i;
        v = 0;
        while (i < t.size() && std::isxdigit(static_cast<unsigned char>(t[i]))) v = v * 16 + static_cast<std::uint32_t>(std::stoi(std::string(1, t[i++]), nullptr, 16));
        return i > start;
    };
    o = Operand();
    if (t.compare(i, 8, "{recall}") == 0) { i += 8; o.kind = Kind::recall; return true; }
    Kind k = Kind::mem;
    for (bool more = true; more && i < t.size();) {
        switch (t[i]) {
        case 'd': k = Kind::delta; ++i; break;
        case 'p': k = Kind::prior; ++i; break;
        case 'b': o.bcd = true; ++i; break;
        case '~': o.invert = true; ++i; break;
        default: more = false; break;
        }
    }
    if (t.compare(i, 2, "0x") == 0) {
        i += 2;
        o.kind = k;
        o.size = Size::b16;
        if (i < t.size()) {
            switch (t[i]) {
            case 'H': case 'h': o.size = Size::b8; ++i; break;
            case 'W': case 'w': o.size = Size::b24; ++i; break;
            case 'X': case 'x': o.size = Size::b32; ++i; break;
            case 'I': case 'i': o.size = Size::b16be; ++i; break;
            case 'J': case 'j': o.size = Size::b24be; ++i; break;
            case 'G': case 'g': o.size = Size::b32be; ++i; break;
            case 'K': case 'k': o.size = Size::bitcount; ++i; break;
            case ' ': o.size = Size::b16; ++i; break;
            case 'M': case 'm': o.size = Size::bit0; ++i; break;
            case 'N': case 'n': o.size = Size::bit1; ++i; break;
            case 'O': case 'o': o.size = Size::bit2; ++i; break;
            case 'P': o.size = Size::bit3; ++i; break;
            case 'Q': case 'q': o.size = Size::bit4; ++i; break;
            case 'R': case 'r': o.size = Size::bit5; ++i; break;
            case 'S': case 's': o.size = Size::bit6; ++i; break;
            case 'T': case 't': o.size = Size::bit7; ++i; break;
            case 'L': case 'l': o.size = Size::low4; ++i; break;
            case 'U': case 'u': o.size = Size::high4; ++i; break;
            default: break;  // 0x1234: 16 bit
            }
        }
        return hex(o.address);
    }
    if (t.compare(i, 2, "fF") == 0 || t.compare(i, 2, "fB") == 0) {  // floats in memory
        o.kind = k;
        o.size = t[i + 1] == 'F' ? Size::f32 : Size::f32be;
        i += 2;
        return hex(o.address);
    }
    o.kind = Kind::value;
    if (i + 1 < t.size() && t[i] == 'f' && (std::isdigit(static_cast<unsigned char>(t[i + 1])) || t[i + 1] == '-' || t[i + 1] == '.')) {
        ++i;  // f1.5: a float constant
        const std::size_t start = i;
        if (t[i] == '-') ++i;
        while (i < t.size() && (std::isdigit(static_cast<unsigned char>(t[i])) || t[i] == '.')) ++i;
        o.value = numf(std::atof(t.substr(start, i - start).c_str()));
        return i > start;
    }
    if (i < t.size() && t[i] == 'f') return false;  // fH fI fM fL (double / MBF floats): not supported here
    if (i < t.size() && (t[i] == 'h' || t[i] == 'H')) {
        ++i;
        std::uint32_t v = 0;
        const bool ok = hex(v);
        o.value = num(v);
        return ok;
    }
    if (i < t.size() && (t[i] == 'v' || t[i] == 'V')) ++i;  // v123
    const std::size_t start = i;
    if (i < t.size() && (t[i] == '-' || t[i] == '+')) ++i;
    while (i < t.size() && std::isdigit(static_cast<unsigned char>(t[i]))) ++i;  // integers only: "9.1." is 9 with a hit count
    if (i == start) return false;
    o.value = num(static_cast<std::uint32_t>(std::strtoll(t.substr(start, i - start).c_str(), nullptr, 10)));
    return true;
}

bool parse_cond(const std::string &t, Cond &c) {
    std::size_t i = 0;
    c = Cond();
    if (t.size() > 2 && t[1] == ':') {
        switch (t[0]) {
        case 'R': c.flag = Flag::reset; break;
        case 'P': c.flag = Flag::pause; break;
        case 'A': c.flag = Flag::add_source; break;
        case 'B': c.flag = Flag::sub_source; break;
        case 'N': c.flag = Flag::and_next; break;
        case 'O': c.flag = Flag::or_next; break;
        case 'C': c.flag = Flag::add_hits; break;
        case 'D': c.flag = Flag::sub_hits; break;
        case 'I': c.flag = Flag::add_address; break;
        case 'Z': c.flag = Flag::reset_next_if; break;
        case 'K': c.flag = Flag::remember; break;
        case 'M': case 'G': c.flag = Flag::measured; break;
        case 'Q': c.flag = Flag::measured_if; break;
        case 'T': c.flag = Flag::trigger; break;
        default: return false;
        }
        i = 2;
    }
    if (!parse_operand(t, i, c.left)) return false;
    if (i >= t.size()) return true;  // a lone operand (A:/B:/I:/K: lines)
    if (t.compare(i, 2, "!=") == 0) { c.cmp = Cmp::ne; i += 2; }
    else if (t.compare(i, 2, "<=") == 0) { c.cmp = Cmp::le; i += 2; }
    else if (t.compare(i, 2, ">=") == 0) { c.cmp = Cmp::ge; i += 2; }
    else if (t[i] == '=') { c.cmp = Cmp::eq; ++i; }
    else if (t[i] == '<') { c.cmp = Cmp::lt; ++i; }
    else if (t[i] == '>') { c.cmp = Cmp::gt; ++i; }
    else if (std::string("*/&^%+-").find(t[i]) != std::string::npos) {
        const char o = t[i++];
        c.op = o == '*' ? Op::mul : o == '/' ? Op::div : o == '&' ? Op::band : o == '^' ? Op::bxor : o == '%' ? Op::mod : o == '+' ? Op::add : Op::sub;
        return parse_operand(t, i, c.right) && i >= t.size();
    }
    else return false;
    if (!parse_operand(t, i, c.right)) return false;
    if (i < t.size() && (t[i] == '.' || t[i] == '(')) {
        ++i;
        std::uint32_t h = 0;
        while (i < t.size() && std::isdigit(static_cast<unsigned char>(t[i]))) h = h * 10 + static_cast<unsigned>(t[i++] - '0');
        c.target = h;
        if (i < t.size() && (t[i] == '.' || t[i] == ')')) ++i;
    }
    return i >= t.size();
}

bool parse_logic(const std::string &logic, std::vector<Group> &groups) {
    groups.clear();
    // groups are separated by S (core first, then alternatives); "0xS..." is a bit-6 size, not a separator
    std::vector<std::string> parts(1);
    for (std::size_t i = 0; i < logic.size(); ++i) {
        if (logic[i] == 'S' && !(i >= 2 && logic[i - 1] == 'x' && logic[i - 2] == '0')) parts.emplace_back();
        else parts.back() += logic[i];
    }
    for (std::size_t k = 0; k < parts.size(); ++k) {
        if (k > 0 && parts[k].empty()) continue;
        Group g;
        std::stringstream cs(parts[k]);
        std::string cond;
        while (std::getline(cs, cond, '_')) {
            if (cond.empty()) continue;
            Cond c;
            if (!parse_cond(cond, c)) return false;
            g.conds.push_back(c);
        }
        groups.push_back(g);
    }
    return !groups.empty() && !groups[0].conds.empty();
}

// ---- checking them ---------------------------------------------------------------------------------------------------

std::uint32_t read_raw(Size s, std::uint32_t a) {
    a &= 0x01FFFFFFu;  // EE main memory
    auto b = [](std::uint32_t x) { return static_cast<std::uint32_t>(ps2::ld8(x & 0x01FFFFFFu)); };
    switch (s) {
    case Size::b8: return b(a);
    case Size::b16: return b(a) | b(a + 1) << 8;
    case Size::b24: return b(a) | b(a + 1) << 8 | b(a + 2) << 16;
    case Size::b32: case Size::f32: return b(a) | b(a + 1) << 8 | b(a + 2) << 16 | b(a + 3) << 24;
    case Size::b16be: return b(a) << 8 | b(a + 1);
    case Size::b24be: return b(a) << 16 | b(a + 1) << 8 | b(a + 2);
    case Size::b32be: case Size::f32be: return b(a) << 24 | b(a + 1) << 16 | b(a + 2) << 8 | b(a + 3);
    case Size::low4: return b(a) & 0xF;
    case Size::high4: return b(a) >> 4;
    case Size::bitcount: { std::uint32_t v = b(a), n = 0; for (; v; v &= v - 1) ++n; return n; }
    default: return (b(a) >> (static_cast<int>(s) - static_cast<int>(Size::bit0))) & 1;
    }
}

Num shape(const Operand &o, std::uint32_t raw) {
    if (o.size == Size::f32 || o.size == Size::f32be) {
        float f;
        std::memcpy(&f, &raw, 4);
        return numf(f);
    }
    std::uint32_t v = raw;
    if (o.bcd) {
        std::uint32_t out = 0, mul = 1;
        for (; raw; raw >>= 4, mul *= 10) out += (raw & 0xF) * mul;
        v = out;
    }
    if (o.invert) {
        switch (o.size) {
        case Size::b8: v = ~v & 0xFF; break;
        case Size::b16: case Size::b16be: v = ~v & 0xFFFF; break;
        case Size::b24: case Size::b24be: v = ~v & 0xFFFFFF; break;
        case Size::low4: case Size::high4: v = ~v & 0xF; break;
        case Size::b32: case Size::b32be: v = ~v; break;
        default: v = v ^ 1; break;
        }
    }
    return num(v);
}

// The operand's value this frame; memory operands also move their delta / prior history (once per frame: `step`).
Num value_of(Operand &o, std::uint32_t offset, bool step, const Num &recall) {
    if (o.kind == Kind::value) return o.value;
    if (o.kind == Kind::recall) return recall;
    const std::uint32_t now = read_raw(o.size, o.address + offset);
    std::uint32_t result = now;
    if (o.kind == Kind::delta) result = o.seen ? o.last : now;
    else if (o.kind == Kind::prior) result = o.seen ? o.prior : now;
    if (step) {
        if (o.seen && now != o.last) o.prior = o.last;
        o.last = now;
        o.seen = true;
    }
    return shape(o, result);
}

Num apply(Op op, const Num &a, const Num &b) {
    if (op == Op::none) return a;
    if (a.f || b.f) {
        const double x = a.real(), y = b.real();
        switch (op) {
        case Op::mul: return numf(x * y);
        case Op::div: return numf(y == 0 ? 0 : x / y);
        case Op::add: return numf(x + y);
        case Op::sub: return numf(x - y);
        case Op::mod: return numf(y == 0 ? 0 : std::fmod(x, y));
        default: break;  // & ^ on floats: integer parts
        }
    }
    const std::uint32_t x = a.f ? static_cast<std::uint32_t>(a.d) : a.u, y = b.f ? static_cast<std::uint32_t>(b.d) : b.u;
    switch (op) {
    case Op::mul: return num(x * y);
    case Op::div: return num(y ? x / y : 0);
    case Op::band: return num(x & y);
    case Op::bxor: return num(x ^ y);
    case Op::mod: return num(y ? x % y : 0);
    case Op::add: return num(x + y);
    case Op::sub: return num(x - y);
    default: return a;
    }
}

bool compare(Cmp c, const Num &a, const Num &b) {
    if (a.f || b.f) {
        const double x = a.real(), y = b.real();
        switch (c) {
        case Cmp::eq: return x == y; case Cmp::ne: return x != y; case Cmp::lt: return x < y;
        case Cmp::le: return x <= y; case Cmp::gt: return x > y; case Cmp::ge: return x >= y; default: return x != 0;
        }
    }
    switch (c) {
    case Cmp::eq: return a.u == b.u; case Cmp::ne: return a.u != b.u; case Cmp::lt: return a.u < b.u;
    case Cmp::le: return a.u <= b.u; case Cmp::gt: return a.u > b.u; case Cmp::ge: return a.u >= b.u; default: return a.u != 0;
    }
}

// One group: true when every plain condition holds (hit counts reached); reset / pause seen separately.
// Follows rcheevos: modifiers (A: B: I: K:) build the next condition's value or address, N:/O: chain truth into the next
// condition, C:/D: add or remove their hits from the next condition with a hit target, Z: resets the next condition's hits.
bool check_group(Group &g, bool &reset, bool &paused, bool pause_pass) {
    bool all = true;
    Num add = num(0), recall;
    bool have_add = false;
    std::uint32_t address_offset = 0;
    int chain = -1;  // -1 none, 0 false, 1 true: AndNext / OrNext carried into the next condition
    bool chain_or = false;  // the chain carried came from O: (else N:)
    std::int64_t extra_hits = 0;
    bool reset_next = false;
    for (Cond &c : g.conds) {
        if (pause_pass && c.flag != Flag::pause && c.flag != Flag::add_source && c.flag != Flag::sub_source && c.flag != Flag::add_address &&
            c.flag != Flag::and_next && c.flag != Flag::or_next && c.flag != Flag::remember) {
            // the pause pass only works out P: conditions (with what builds them); the rest waits for the main pass
            add = num(0); have_add = false; address_offset = 0; chain = -1;
            continue;
        }
        if (!pause_pass && c.flag == Flag::pause) { add = num(0); have_add = false; address_offset = 0; chain = -1; continue; }
        const bool step = pause_pass ? c.flag == Flag::pause : true;  // each operand moves its history once a frame
        const Num l = value_of(c.left, address_offset, step, recall);
        const Num r = value_of(c.right, address_offset, step, recall);
        if (c.flag == Flag::add_address) { address_offset = apply(c.op, l, r).u; continue; }
        address_offset = 0;
        if (c.flag == Flag::add_source || c.flag == Flag::sub_source || c.flag == Flag::remember) {
            Num v = apply(c.op, l, r);
            if (have_add) v = apply(Op::add, add, v);
            if (c.flag == Flag::sub_source) v = apply(Op::sub, have_add ? add : num(0), apply(c.op, l, r));
            if (c.flag == Flag::remember) { recall = v; add = num(0); have_add = false; }
            else { add = v; have_add = true; }
            continue;
        }
        const Num left = have_add ? apply(Op::add, add, l) : l;
        add = num(0);
        have_add = false;
        bool truth = compare(c.cmp, left, r);
        if (chain != -1) truth = chain_or ? (truth || chain == 1) : (truth && chain == 1);  // the line before was N: or O:
        if (c.flag == Flag::and_next) { chain = truth ? 1 : 0; chain_or = false; continue; }
        if (c.flag == Flag::or_next) { chain = truth ? 1 : 0; chain_or = true; continue; }
        chain = -1;
        if (c.flag == Flag::add_hits) { if (truth && c.hits < 0xFFFFFFFFu) ++c.hits; extra_hits += c.hits; continue; }
        if (c.flag == Flag::sub_hits) { if (truth && c.hits < 0xFFFFFFFFu) ++c.hits; extra_hits -= c.hits; continue; }
        if (c.flag == Flag::reset_next_if) { reset_next = truth; continue; }
        if (reset_next) { c.hits = 0; reset_next = false; }
        if (c.flag == Flag::pause) {
            if (c.target) { if (truth && c.hits < c.target) ++c.hits; truth = c.hits >= c.target; }
            if (truth) paused = true;
            extra_hits = 0;
            continue;
        }
        if (c.flag == Flag::reset) {
            if (c.target) { if (truth && c.hits < c.target) ++c.hits; truth = c.hits >= c.target; }
            if (truth) reset = true;
            extra_hits = 0;
            continue;
        }
        if (c.target) {
            if (truth && c.hits < c.target) ++c.hits;
            if (static_cast<std::int64_t>(c.hits) + extra_hits < c.target) all = false;
        } else if (!truth) {
            all = false;
        }
        extra_hits = 0;
    }
    return all;
}

void clear_hits(Achievement &a) {
    for (Group &g : a.groups)
        for (Cond &c : g.conds) c.hits = 0;
}

bool check(Achievement &a) {
    bool reset = false, paused = false, core_paused = false;
    // the pause conditions first, as rcheevos does: a paused group keeps its hit counts, is not true and does not move on
    check_group(a.groups[0], reset, core_paused, true);
    bool core = false;
    if (!core_paused) core = check_group(a.groups[0], reset, paused, false);
    bool any_alt = a.groups.size() == 1;
    for (std::size_t i = 1; i < a.groups.size(); ++i) {
        bool p = false, dummy = false;
        check_group(a.groups[i], dummy, p, true);
        if (p) continue;
        if (check_group(a.groups[i], reset, dummy, false)) any_alt = true;
    }
    if (reset) { clear_hits(a); return false; }
    return core && any_alt && !core_paused;
}

void save_unlock(const Achievement &a) {
    if (g_unlocks_path.empty()) return;
    std::ofstream f(g_unlocks_path, std::ios::app);
    const std::time_t now = std::time(nullptr);
    char when[32];
    std::strftime(when, sizeof when, "%Y-%m-%d %H:%M", std::localtime(&now));
    f << a.id << "\t" << when << "\t" << a.title << "\n";
    g_when[a.id] = when;
}

// ---- the two file forms ----------------------------------------------------------------------------------------------

bool g_with_unofficial = false;  // PS2_ACHIEVEMENTS_UNOFFICIAL=1

// Every achievement of the set is listed; one whose condition uses something this checker does not have stays in the list,
// marked, and is never checked (so the list shows the whole set, and nothing unlocks by a wrong reading).
void add(Achievement a, const std::string &logic) {
    if (!parse_logic(logic, a.groups)) {
        a.groups.clear();
        a.why = "condition not supported";
        std::fprintf(stderr, "[achievements] %s: condition not understood: %s\n", a.title.c_str(), logic.c_str());
    }
    g_list.push_back(a);
}

void save_off() {
    if (g_off_path.empty()) return;
    std::ofstream f(g_off_path, std::ios::trunc);
    f << "# achievements switched off (ids), rawdanger-recomp: F1 -> Achievements or RawDangerPC.exe\n";
    for (const Achievement &a : g_list)
        if (!a.enabled) f << a.id << "\n";
}

// RACache\Data\<id>.json: only the fields used here, read without a JSON library (strings with \" and \\ escapes)
std::string json_field(const std::string &obj, const std::string &key) {
    const std::string k = "\"" + key + "\"";
    std::size_t at = obj.find(k);
    if (at == std::string::npos) return "";
    at = obj.find(':', at + k.size());
    if (at == std::string::npos) return "";
    ++at;
    while (at < obj.size() && (obj[at] == ' ' || obj[at] == '\t' || obj[at] == '\n' || obj[at] == '\r')) ++at;
    if (at < obj.size() && obj[at] == '"') {
        std::string out;
        for (++at; at < obj.size() && obj[at] != '"'; ++at) {
            if (obj[at] == '\\' && at + 1 < obj.size()) { ++at; out += obj[at] == 'n' ? '\n' : obj[at]; }
            else out += obj[at];
        }
        return out;
    }
    std::size_t end = at;
    while (end < obj.size() && obj[end] != ',' && obj[end] != '}') ++end;
    return trim(obj.substr(at, end - at));
}

void load_json(const std::string &text) {
    std::size_t at = text.find("\"Achievements\"");
    if (at == std::string::npos) return;
    at = text.find('[', at);
    int depth = 0;
    std::size_t start = 0;
    for (std::size_t i = at; i < text.size(); ++i) {
        if (text[i] == '"') { for (++i; i < text.size() && text[i] != '"'; ++i) if (text[i] == '\\') ++i; continue; }
        if (text[i] == '{') { if (depth++ == 0) start = i; }
        else if (text[i] == '}') {
            if (--depth == 0) {
                const std::string obj = text.substr(start, i - start + 1);
                Achievement a;
                a.id = json_field(obj, "ID");
                a.title = json_field(obj, "Title");
                a.description = json_field(obj, "Description");
                a.points = std::atoi(json_field(obj, "Points").c_str());
                a.unofficial = json_field(obj, "Flags") == "5";  // RetroAchievements' unofficial (still in test) ones
                if (a.unofficial && !g_with_unofficial) continue;
                add(a, json_field(obj, "MemAddr"));
            }
        } else if (text[i] == ']' && depth == 0) break;
    }
}

void load_text(const std::string &text) {
    std::stringstream in(text);
    std::string line, logic;
    Achievement cur;
    bool have = false;
    auto flush = [&] {
        if (have && (!cur.unofficial || g_with_unofficial)) add(cur, logic);
        cur = Achievement();
        logic.clear();
        have = false;
    };
    while (std::getline(in, line)) {
        const std::string t = trim(line);
        if (t.empty() || t[0] == '#') continue;
        if (t.front() == '[' && t.back() == ']') {
            flush();
            cur.id = t.substr(1, t.size() - 2);
            have = true;
            continue;
        }
        const auto eq = t.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = trim(t.substr(0, eq)), v = trim(t.substr(eq + 1));
        if (k == "title") cur.title = v;
        else if (k == "description") cur.description = v;
        else if (k == "points") cur.points = std::atoi(v.c_str());
        else if (k == "logic") logic = v;
        else if (k == "unofficial") cur.unofficial = v == "1";
    }
    flush();
}

}  // namespace

bool load(const char *set_path, const std::string &unlocks_path) {
    std::lock_guard<std::mutex> hold(g_lock);
    g_list.clear();
    g_loaded = false;
    if (!set_path || !*set_path) return false;
    std::ifstream in(set_path, std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "[achievements] no set file %s\n", set_path);
        return false;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string text = ss.str();
    const char *icons = std::getenv("PS2_ACHIEVEMENTS_ICONS");
    g_icons_dir = icons && *icons ? icons : "";
    const char *uo = std::getenv("PS2_ACHIEVEMENTS_UNOFFICIAL");
    g_with_unofficial = uo && *uo == '1';
    if (text.find("\"Achievements\"") != std::string::npos) load_json(text);
    else load_text(text);
    // PS2_ACHIEVEMENTS_OFF=<file>: ids the player switched off (default: next to the unlocks file)
    const char *off = std::getenv("PS2_ACHIEVEMENTS_OFF");
    g_off_path = off && *off ? std::string(off) : unlocks_path + ".off";
    {
        std::set<std::string> ids;
        std::ifstream o(g_off_path);
        std::string l;
        while (std::getline(o, l)) { l = trim(l); if (!l.empty() && l[0] != '#') ids.insert(l); }
        for (Achievement &a : g_list) a.enabled = ids.count(a.id) == 0;
    }
    g_unlocks_path = unlocks_path;
    std::set<std::string> done;
    std::ifstream u(unlocks_path);
    std::string line;
    while (std::getline(u, line)) {
        const std::size_t tab = line.find('\t');
        done.insert(line.substr(0, tab));
        if (tab != std::string::npos) {
            const std::string rest = line.substr(tab + 1);
            g_when[line.substr(0, tab)] = trim(rest.substr(0, rest.find('\t')));
        }
    }
    for (Achievement &a : g_list) a.unlocked = done.count(a.id) != 0;
    g_loaded = !g_list.empty();
    std::fprintf(stderr, "[achievements] %zu achievements from %s, %zu already unlocked\n", g_list.size(), set_path, done.size());
    return g_loaded;
}

void frame() {
    if (!g_loaded) return;
    std::lock_guard<std::mutex> hold(g_lock);
    for (Achievement &a : g_list) {
        if (a.unlocked || !a.enabled || a.groups.empty()) continue;
        const bool now = check(a);
        if (!now) { a.primed = true; continue; }
        if (!a.primed) continue;  // true from the first frame on (a loaded save): wait for it to be false once
        a.unlocked = true;
        save_unlock(a);
        g_note = "Achievement unlocked: " + a.title + (a.points ? " (" + std::to_string(a.points) + ")" : "");
        // its picture: PS2_ACHIEVEMENTS_ICONS/<id>.bmp (RawDangerPC.exe makes them: RetroAchievements' badges, or its own)
        g_note_icon = g_icons_dir.empty() ? std::string() : g_icons_dir + "/" + a.id + ".bmp";
        std::fprintf(stderr, "[achievements] unlocked %s - %s\n", a.id.c_str(), a.title.c_str());
    }
}

std::vector<Row> rows() {
    std::lock_guard<std::mutex> hold(g_lock);
    std::vector<Row> out;
    for (const Achievement &a : g_list) {
        Row r;
        r.title = a.title;
        r.description = a.description;
        if (!g_icons_dir.empty()) {
            r.icon = g_icons_dir + "/" + a.id + ".bmp";
            r.icon_locked = g_icons_dir + "/" + a.id + "_lock.bmp";
        }
        const auto w = g_when.find(a.id);
        if (a.unlocked && w != g_when.end()) r.when = w->second;
        r.points = a.points;
        r.unlocked = a.unlocked;
        r.enabled = a.enabled;
        r.unofficial = a.unofficial;
        r.unsupported = a.groups.empty();
        out.push_back(r);
    }
    return out;
}

bool toggle(std::size_t index, std::string &note) {
    std::lock_guard<std::mutex> hold(g_lock);
    if (index >= g_list.size()) return false;
    Achievement &a = g_list[index];
    if (a.groups.empty()) { note = a.title + ": its condition is not supported, it cannot be checked"; return false; }
    a.enabled = !a.enabled;
    a.primed = false;
    save_off();
    note = a.title + (a.enabled ? ": on" : ": off (not checked)");
    return true;
}

std::string take_note() {
    std::lock_guard<std::mutex> hold(g_lock);
    std::string n;
    n.swap(g_note);
    return n;
}

std::string take_note_icon() {
    std::lock_guard<std::mutex> hold(g_lock);
    std::string n;
    n.swap(g_note_icon);
    return n;
}

bool loaded() { return g_loaded; }

int unlocked_count() {
    std::lock_guard<std::mutex> hold(g_lock);
    int n = 0;
    for (const Achievement &a : g_list) n += a.unlocked;
    return n;
}

int total_points(bool unlocked_only) {
    std::lock_guard<std::mutex> hold(g_lock);
    int n = 0;
    for (const Achievement &a : g_list)
        if (!unlocked_only || a.unlocked) n += a.points;
    return n;
}

}  // namespace achievements
