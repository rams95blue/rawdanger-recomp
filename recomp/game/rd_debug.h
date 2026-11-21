#pragma once
// Raw Danger recomp - runtime debugging aids (included by rd_game.cpp).
//
//  * game printf: Irem compiled their debug printf down to an empty function
//    (US: 0x002C6240). Rebinding it to rd_debug's formatter prints the game's
//    own diagnostics ("FatSearchFile: Not Found %s", "HfileReadAlloc: ...").
//    The runtime's printf only prints in aggressive-log builds, so we format here.
//  * tracer: wraps up to 16 game functions and logs arguments / return values.
//
// Both go through PS2Runtime::replaceFunction, so only this file needs
// rebuilding - no re-recompile. Every message is rate-limited per call site.

#include "ps2_runtime.h"
#include "runtime/ps2_memory.h"

#include <array>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace rawdanger_debug
{
    // ------------------------------------------------------------------ guest memory

    inline bool guestPtr(PS2Runtime &rt, uint32_t addr, uint32_t len, const uint8_t *&out)
    {
        if (addr >= PS2_SCRATCHPAD_BASE && addr + len <= PS2_SCRATCHPAD_BASE + PS2_SCRATCHPAD_SIZE)
        {
            out = rt.memory().getScratchpad() + (addr - PS2_SCRATCHPAD_BASE); // 16 KB SPR at 0x70000000
            return true;
        }
        const uint32_t seg = addr & 0xF0000000u;
        if (!(seg == 0x00000000u || seg == 0x20000000u || seg == 0x30000000u || seg == 0x80000000u || seg == 0xA0000000u))
            return false;
        const uint32_t phys = addr & 0x01FFFFFFu;
        if (phys + len > PS2_RAM_SIZE || (addr & 0x0E000000u) != 0)
            return false;
        out = rt.memory().getRDRAM() + phys;
        return true;
    }

    inline std::string guestString(PS2Runtime &rt, uint32_t addr, size_t maxLen = 256)
    {
        std::string s;
        const uint8_t *p = nullptr;
        for (size_t i = 0; i < maxLen; ++i)
        {
            if (!guestPtr(rt, addr + static_cast<uint32_t>(i), 1, p))
                break;
            if (*p == 0)
                break;
            s.push_back(static_cast<char>(*p));
        }
        return s;
    }

    inline bool printable(const std::string &s)
    {
        if (s.size() < 2)
            return false;
        for (unsigned char c : s)
            if (c < 0x20 || c > 0x7E)
                return false;
        return true;
    }

    // ------------------------------------------------------------------ printf formatting

    // Formats a C printf string. nextArg() yields successive 32-bit integer args,
    // readStr(addr) reads a guest string. Floats aren't passed in GPRs: shown as '?'.
    inline std::string formatPrintf(const std::string &fmt,
                                    const std::function<uint32_t()> &nextArg,
                                    const std::function<std::string(uint32_t)> &readStr)
    {
        std::string out;
        for (size_t i = 0; i < fmt.size(); ++i)
        {
            const char c = fmt[i];
            if (c != '%')
            {
                out.push_back(c);
                continue;
            }
            if (i + 1 < fmt.size() && fmt[i + 1] == '%')
            {
                out.push_back('%');
                ++i;
                continue;
            }
            std::string spec = "%";
            size_t j = i + 1;
            while (j < fmt.size() && std::strchr("-+ #0", fmt[j]))
                spec.push_back(fmt[j++]);
            while (j < fmt.size() && (std::isdigit(static_cast<unsigned char>(fmt[j])) || fmt[j] == '.'))
                spec.push_back(fmt[j++]);
            if (j < fmt.size() && fmt[j] == '*')
            {
                spec += std::to_string(static_cast<int32_t>(nextArg()));
                ++j;
            }
            while (j < fmt.size() && std::strchr("hlLqjzt", fmt[j]))
                ++j; // length modifiers: guest ints are 32-bit here
            if (j >= fmt.size())
            {
                out += spec;
                break;
            }
            const char conv = fmt[j];
            char buf[512];
            switch (conv)
            {
            case 'd':
            case 'i':
                std::snprintf(buf, sizeof(buf), (spec + "d").c_str(), static_cast<int32_t>(nextArg()));
                out += buf;
                break;
            case 'u':
            case 'x':
            case 'X':
            case 'o':
                std::snprintf(buf, sizeof(buf), (spec + conv).c_str(), nextArg());
                out += buf;
                break;
            case 'c':
                out.push_back(static_cast<char>(nextArg() & 0xFF));
                break;
            case 'p':
                std::snprintf(buf, sizeof(buf), "0x%08x", nextArg());
                out += buf;
                break;
            case 's':
            {
                const uint32_t a = nextArg();
                const std::string s = a ? readStr(a) : std::string("(null)");
                std::snprintf(buf, sizeof(buf), (spec + "s").c_str(), s.c_str());
                out += buf;
                break;
            }
            case 'f':
            case 'g':
            case 'e':
            case 'F':
            case 'G':
            case 'E':
                out += "?";
                break;
            default:
                out += spec;
                out.push_back(conv);
                break;
            }
            i = j;
        }
        return out;
    }

    // EE EABI: integer args in $a0-$a3, $t0-$t3 (regs 4..11).
    inline std::string formatGuestPrintf(PS2Runtime &rt, R5900Context *ctx, int fmtReg)
    {
        const std::string fmt = guestString(rt, getRegU32(ctx, fmtReg), 1024);
        int reg = fmtReg + 1;
        return formatPrintf(
            fmt,
            [&]() -> uint32_t { return reg <= 11 ? getRegU32(ctx, reg++) : 0u; },
            [&](uint32_t a) { return guestString(rt, a); });
    }

    // ------------------------------------------------------------------ rate limiting

    struct RateLimiter
    {
        std::mutex mutex;
        std::map<uint64_t, uint64_t> counts;
        // first 40 occurrences, then every 5000th (with the running count)
        bool allow(uint64_t key, uint64_t &count)
        {
            std::lock_guard<std::mutex> lock(mutex);
            count = ++counts[key];
            return count <= 40 || count % 5000 == 0;
        }
    };

    inline RateLimiter &limiter()
    {
        static RateLimiter r;
        return r;
    }

    // ------------------------------------------------------------------ game printf

    inline void gamePrintf(uint8_t *, R5900Context *ctx, PS2Runtime *rt)
    {
        const uint32_t fmtAddr = getRegU32(ctx, 4);
        uint64_t n = 0;
        if (!limiter().allow((1ull << 32) | fmtAddr, n))
            return;
        std::string msg = formatGuestPrintf(*rt, ctx, 4);
        while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r'))
            msg.pop_back();
        std::cout << "[game] " << msg;
        if (n > 40)
            std::cout << "   (x" << n << ")";
        std::cout << std::endl;
        setReturnS32(ctx, static_cast<int32_t>(msg.size()));
    }

    // Bind only if the target still looks like the empty debug printf:
    //   addiu $sp,$sp,-0x50 ; sd $a1,0x18($sp)
    inline bool bindGamePrintf(PS2Runtime &rt, uint32_t addr)
    {
        const uint8_t *p = nullptr;
        if (!guestPtr(rt, addr, 8, p))
            return false;
        uint32_t w0 = 0, w1 = 0;
        std::memcpy(&w0, p, 4);
        std::memcpy(&w1, p + 4, 4);
        if (w0 != 0x27BDFFB0u || w1 != 0xFFA50018u)
        {
            std::cout << "[rawdanger] game_printf: 0x" << std::hex << addr << " doesn't look like the debug printf ("
                      << w0 << " " << w1 << ") - not bound" << std::dec << std::endl;
            return false;
        }
        return rt.replaceFunction(addr, &gamePrintf);
    }

    // ------------------------------------------------------------------ tracer

    constexpr int kMaxTrace = 16;

    struct TraceSlot
    {
        uint32_t addr = 0;
        PS2Runtime::RecompiledFunction original = nullptr;
        bool onlyZero = false; // "addr!z": log only calls that return v0 == 0 (failed lookups)
        bool matrices = false; // "addr!m": dump 4x4 float matrices behind a0/a1/a2 before, a0 after
        bool objectPacket = false; // "addr!o": Raw Danger renderer: current object matrix + VIF packet
    };

    inline TraceSlot *traceSlots()
    {
        static TraceSlot slots[kMaxTrace];
        return slots;
    }

    inline std::string describeArg(PS2Runtime &rt, uint32_t v)
    {
        std::ostringstream o;
        o << "0x" << std::hex << v;
        const uint8_t *p = nullptr;
        if (v >= 0x00100000u && guestPtr(rt, v, 16, p))
        {
            const std::string s = guestString(rt, v, 48);
            if (printable(s))
                o << " \"" << s << "\"";
            else
            {
                o << " [";
                for (int i = 0; i < 16; ++i)
                    o << (i ? " " : "") << std::hex << (p[i] >> 4) << (p[i] & 0xF);
                o << "]";
            }
        }
        return o.str();
    }

    inline void traceCall(int index, uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt)
    {
        TraceSlot &slot = traceSlots()[index];
        if (slot.objectPacket)
        {
            // Raw Danger's object setup (FUN_00311060): the current object is [gp-0x3D50],
            // its world matrix at +0x40; the VIF1 packet pointer is [gp-0x4040]. Dump the
            // matrix before the call and the 5 qwords written at the old packet pointer.
            uint64_t n = 0;
            const bool show = limiter().allow((5ull << 32) | slot.addr, n);
            const uint32_t gp = getRegU32(ctx, 28);
            auto word = [&](uint32_t addr) -> uint32_t
            {
                const uint8_t *p = nullptr;
                uint32_t v = 0;
                if (guestPtr(*rt, addr, 4, p))
                    std::memcpy(&v, p, 4);
                return v;
            };
            auto dumpQwords = [&](const char *tag, uint32_t addr, int count)
            {
                const uint8_t *p = nullptr;
                if (!guestPtr(*rt, addr, 16u * count, p))
                {
                    std::cout << "[trace] #" << n << "   " << tag << " 0x" << std::hex << addr << std::dec << " (unreadable)" << std::endl;
                    return;
                }
                std::ostringstream o;
                o << "[trace] #" << n << "   " << tag << " 0x" << std::hex << addr << std::dec;
                for (int r = 0; r < count; ++r)
                {
                    float f[4];
                    uint32_t w[4];
                    std::memcpy(f, p + 16 * r, 16);
                    std::memcpy(w, p + 16 * r, 16);
                    o << " |";
                    if (r == 0 && std::string(tag).find("packet") != std::string::npos)
                        o << std::hex << " " << w[0] << " " << w[1] << " " << w[2] << " " << w[3] << std::dec;
                    else
                        for (int c = 0; c < 4; ++c)
                            o << " " << f[c];
                }
                std::cout << o.str() << std::endl;
            };
            const uint32_t obj = word(gp - 0x3D50u);
            const uint32_t pkt = word(gp - 0x4040u);
            if (show)
            {
                std::cout << "[trace] #" << n << " 0x" << std::hex << slot.addr << "(a0=0x" << getRegU32(ctx, 4) << ") ra=0x"
                          << getRegU32(ctx, 31) << " obj=0x" << obj << " pkt=0x" << pkt << std::dec << std::endl;
                dumpQwords("obj+0x40 before", obj + 0x40u, 4);
            }
            slot.original(rdram, ctx, rt);
            if (show)
            {
                const uint32_t obj2 = word(gp - 0x3D50u);
                dumpQwords("obj+0x40 after", obj2 + 0x40u, 4);
                dumpQwords("packet", pkt, 5);
            }
            return;
        }
        if (slot.matrices)
        {
            uint64_t n = 0;
            const bool show = limiter().allow((4ull << 32) | slot.addr, n);
            const uint32_t ra = getRegU32(ctx, 31);
            const uint32_t a0 = getRegU32(ctx, 4);
            auto dumpMatrix = [&](const char *tag, uint32_t addr)
            {
                const uint8_t *p = nullptr;
                if ((addr < 0x00100000u && addr < PS2_SCRATCHPAD_BASE) || !guestPtr(*rt, addr, 64, p))
                    return;
                float m[16];
                std::memcpy(m, p, 64);
                std::ostringstream o;
                o << "[trace] #" << n << "   " << tag << " 0x" << std::hex << addr << std::dec;
                for (int r = 0; r < 4; ++r)
                {
                    o << " |";
                    for (int c = 0; c < 4; ++c)
                        o << " " << m[4 * r + c];
                }
                std::cout << o.str() << std::endl;
            };
            if (show)
            {
                std::cout << "[trace] #" << n << " 0x" << std::hex << slot.addr << " ra=0x" << ra << std::dec << std::endl;
                dumpMatrix("a0", a0);
                dumpMatrix("a1", getRegU32(ctx, 5));
                dumpMatrix("a2", getRegU32(ctx, 6));
            }
            slot.original(rdram, ctx, rt);
            if (show)
                dumpMatrix("a0 after", a0);
            return;
        }
        if (slot.onlyZero)
        {
            uint32_t args[4];
            for (int r = 4; r <= 7; ++r)
                args[r - 4] = getRegU32(ctx, r);
            const std::string desc0 = describeArg(*rt, args[0]), desc1 = describeArg(*rt, args[1]);
            const uint32_t ra = getRegU32(ctx, 31);
            slot.original(rdram, ctx, rt);
            if (ctx->pc == ra && getRegU32(ctx, 2) == 0u)
            {
                uint64_t n = 0;
                if (limiter().allow((3ull << 32) | slot.addr, n))
                    std::cout << "[trace] #" << n << " 0x" << std::hex << slot.addr << "(" << desc0 << ", " << desc1
                              << ", 0x" << args[2] << ", 0x" << args[3] << ") ra=0x" << ra << std::dec << " -> 0"
                              << std::endl;
            }
            return;
        }
        uint64_t n = 0;
        const bool show = limiter().allow((2ull << 32) | slot.addr, n);
        const uint32_t ra = getRegU32(ctx, 31);
        if (show)
        {
            std::cout << "[trace] #" << n << " 0x" << std::hex << slot.addr << "(";
            for (int r = 4; r <= 7; ++r)
                std::cout << (r > 4 ? ", " : "") << describeArg(*rt, getRegU32(ctx, r));
            std::cout << ") ra=0x" << ra << std::dec << std::endl;
        }
        slot.original(rdram, ctx, rt);
        if (show)
        {
            if (ctx->pc == ra)
                std::cout << "[trace] #" << n << " 0x" << std::hex << slot.addr << " -> v0="
                          << describeArg(*rt, getRegU32(ctx, 2)) << std::dec << std::endl;
            else
                std::cout << "[trace] #" << n << " 0x" << std::hex << slot.addr << " yielded at pc=0x"
                          << ctx->pc << std::dec << std::endl;
        }
    }

    template <int I>
    void traceThunk(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt)
    {
        traceCall(I, rdram, ctx, rt);
    }

    template <int... Is>
    constexpr std::array<PS2Runtime::RecompiledFunction, sizeof...(Is)> makeThunks(std::integer_sequence<int, Is...>)
    {
        return {&traceThunk<Is>...};
    }

    inline int installTraces(PS2Runtime &rt, const std::string &list)
    {
        static const auto thunks = makeThunks(std::make_integer_sequence<int, kMaxTrace>{});
        std::string token;
        std::istringstream in(list);
        int used = 0;
        while (std::getline(in, token, ','))
        {
            token.erase(0, token.find_first_not_of(" \t"));
            token.erase(token.find_last_not_of(" \t\r") + 1);
            if (token.empty())
                continue;
            if (used >= kMaxTrace)
            {
                std::cout << "[rawdanger] trace: more than " << kMaxTrace << " addresses, ignoring " << token << std::endl;
                continue;
            }
            bool onlyZero = false, matrices = false, objectPacket = false;
            if (token.size() > 2 && token.compare(token.size() - 2, 2, "!z") == 0)
            {
                onlyZero = true;
                token.resize(token.size() - 2);
            }
            else if (token.size() > 2 && token.compare(token.size() - 2, 2, "!m") == 0)
            {
                matrices = true;
                token.resize(token.size() - 2);
            }
            else if (token.size() > 2 && token.compare(token.size() - 2, 2, "!o") == 0)
            {
                objectPacket = true;
                token.resize(token.size() - 2);
            }
            uint32_t addr = 0;
            try
            {
                addr = static_cast<uint32_t>(std::stoul(token, nullptr, 0));
            }
            catch (...)
            {
                std::cout << "[rawdanger] trace: bad address '" << token << "'" << std::endl;
                continue;
            }
            if (!rt.hasFunction(addr))
            {
                std::cout << "[rawdanger] trace: no recompiled function at 0x" << std::hex << addr << std::dec << std::endl;
                continue;
            }
            TraceSlot &slot = traceSlots()[used];
            slot.addr = addr;
            slot.original = rt.lookupFunction(addr);
            slot.onlyZero = onlyZero;
            slot.matrices = matrices;
            slot.objectPacket = objectPacket;
            if (rt.replaceFunction(addr, thunks[used]))
            {
                std::cout << "[rawdanger] tracing 0x" << std::hex << addr << std::dec
                          << (onlyZero ? " (only calls returning 0)" : matrices ? " (4x4 matrices)" : objectPacket ? " (object matrix + packet)" : "") << std::endl;
                ++used;
            }
        }
        return used;
    }
}
