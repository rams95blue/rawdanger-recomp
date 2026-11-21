// Raw Danger! (SLUS-21501) - PC recomp game module for ps2recomp.
//
// Copied into ps2xRuntime/src/runner/ by tools/rd_sync.py and linked into
// ps2EntryRunner. Runs once while the guest ELF is loaded:
//   1. reads rawdanger.ini next to the ELF (writes defaults if missing)
//   2. mounts the original disc image for raw-sector (LBA) reads
//   3. applies runtime data patches (widescreen etc.) - verified against the
//      expected original values so a wrong ELF can't be corrupted
//   4. sets the host display aspect (4:3 / 16:9)
//   5. installs the Japanese voice redirect if voice_language = jp
//
// Code (instruction) patches are NOT applied here: the game code is already
// compiled C++. Those go through config.toml [patches] at recompile time
// (tools/rd_mkconfig.py does that).

#include "game_overrides.h"
#include "ps2_runtime.h"
#include "ps2_stubs.h"
#include "ps2x_hooks.h"
#include "runtime/ps2_memory.h"

#include "rd_build_info.h"
#include "rd_debug.h"
#include "rd_overlay_tables.h"
#include "runtime/ee_scheduler.h"
#include "ps2_host_backend.h"
#include <ctime>
#include "rd_settings.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>
#include <iterator>
#include <chrono>
#include <thread>

namespace rawdanger_detail
{
    namespace fs = std::filesystem;
    using rawdanger::Settings;

    constexpr uint32_t kSector = 2048;

    struct DataPatch
    {
        const char *group;
        uint32_t address;
        uint32_t value;
        uint32_t expected;
        bool hasExpected;
    };

    const DataPatch kDataPatches[] = {
#include "rd_patch_table.inc"
        {nullptr, 0, 0, 0, false}};

    const char *kDefaultIni =
        "; Raw Danger! PC recomp settings - edit and restart the game.\n"
        "\n"
        "[disc]\n"
        "; Original US disc image. Needed when the game reads data by raw sector\n"
        "; number (LBA). Path is relative to this folder, or absolute.\n"
        "image = Raw Danger (USA).iso\n"
        "\n"
        "[display]\n"
        "; 4:3 or 16:9. 16:9 also needs [patches] widescreen = true.\n"
        "aspect = 16:9\n"
        "\n"
        "[patches]\n"
        "; Runtime data patches from patches/patchdb.json (group names).\n"
        "widescreen = true\n"
        "\n"
        "[memory]\n"
        "; game = Raw Danger's PS2 layout: heap up to its own stack (0x01FE8000),\n"
        ";        runtime pools + interrupt stacks moved into the EE kernel area.\n"
        "; upstream = ps2recomp defaults (heap capped at 0x01F00000 - the game can't boot).\n"
        "layout = game\n"
        "heap_limit = 0x01FE8000\n"
        "runtime_pools = 0x00020000\n"
        "callback_stacks = 0x00052000-0x00080000\n"
        "\n"
        "[audio]\n"
        "; en = original English cast, jp = Japanese cast (needs voice_jp/ pack,\n"
        "; built by tools/rd_voicepack.py from your JP disc).\n"
        "voice_language = en\n"
        "\n"
        "[hle]\n"
        "; Game-side SDK functions routed to the runtime's own handlers. Leave on.\n"
        "sceCdLayerSearchFile = 1\n"
        "sceCdSearchFile = 1\n"
        "; Sound-bank uploads to SPU2 RAM are answered without the IOP until\n"
        "; SPU2 audio is emulated (the IOP path deadlocks, see docs/WORKFLOW.md).\n"
        "spu_bank_upload = 1\n"
        "; Controller: libpad2 calls answered from the keyboard / first gamepad\n"
        "; (ps2xIOP has no DBCMAN pad driver, so without this there is no input).\n"
        "pad = 1\n"
        "\n"
        "[movies]\n"
        "; FMVs need an MPEG-2 decoder the runtime doesn't have yet; 1 skips them\n"
        "; (otherwise the game hangs at the first movie).\n"
        "skip = 1\n"
        "\n"
        "[debug]\n"
        "log = true\n"
        "; The game's compiled-out debug printf, routed to this console (US 0x2C6240; off to disable).\n"
        "game_printf = 0x2C6240\n"
        "; Log calls/returns of game functions, e.g. trace = 0x2C9DD0, 0x2C8CD0, 0x2C8B70\n"
        "; (also: rd run --trace ..., or the RD_TRACE environment variable)\n"
        "trace =\n"
        "; Log every guest thread's state every N seconds (0 = off; or rd run --threads N)\n"
        "thread_dump = 0\n"
        "; F9 records the next N frames of GS traffic to gs_captures/*.gsr (renderer debugging;\n"
        "; tools/gs_replay renders a capture offline). 0 disables the key.\n"
        "gs_capture_frames = 2\n"
        "; Start one capture automatically N seconds after boot (0 = off; or rd run --gscap N).\n"
        "gs_capture_at = 0\n";

    struct VoiceRoute
    {
        // file route: cdrom0 path key -> host file
        std::string pathKey;
        // lba route: [lba, lba+count) of the US disc -> host file
        uint32_t lba = 0;
        uint32_t count = 0;
        fs::path hostFile;
    };

    struct State
    {
        Settings ini;
        bool log = true;
        fs::path gameDir;
        std::vector<VoiceRoute> fileRoutes;
        std::vector<VoiceRoute> lbaRoutes;
    };

    State &state()
    {
        static State s;
        return s;
    }

    std::ostream &log()
    {
        return std::cout << "[rawdanger] ";
    }

    // "cdrom0:\DATA\VOICE.BIN;1" -> "DATA/VOICE.BIN"
    std::string pathKey(std::string p)
    {
        if (auto c = p.find(':'); c != std::string::npos)
            p = p.substr(c + 1);
        if (auto sc = p.find(';'); sc != std::string::npos)
            p = p.substr(0, sc);
        for (char &ch : p)
        {
            if (ch == '\\')
                ch = '/';
            ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        }
        while (!p.empty() && p.front() == '/')
            p.erase(p.begin());
        return p;
    }

    fs::path resolveGamePath(const std::string &value)
    {
        fs::path p(value);
        if (p.is_relative())
            p = state().gameDir / p;
        return p.lexically_normal();
    }

    // voice_jp/voice_map.txt
    //   file DATA/VOICE.BIN = DATA/VOICE.BIN
    //   lba  123456 789     = DATA/STREAM.BIN
    void loadVoiceMap(const fs::path &dir)
    {
        const fs::path mapFile = dir / "voice_map.txt";
        std::ifstream in(mapFile);
        if (!in)
        {
            log() << "voice_language=jp but " << mapFile.string() << " is missing - keeping English voices." << std::endl;
            return;
        }
        std::string line;
        int lineNo = 0;
        while (std::getline(in, line))
        {
            ++lineNo;
            line = Settings::trim(line);
            if (line.empty() || line[0] == '#' || line[0] == ';')
                continue;
            const auto eq = line.find('=');
            if (eq == std::string::npos)
                continue;
            std::istringstream lhs(line.substr(0, eq));
            const fs::path target = (dir / Settings::trim(line.substr(eq + 1))).lexically_normal();
            std::string kind;
            lhs >> kind;
            VoiceRoute r;
            r.hostFile = target;
            std::error_code ec;
            if (!fs::is_regular_file(target, ec))
            {
                log() << "voice_map.txt:" << lineNo << " missing file " << target.string() << std::endl;
                continue;
            }
            if (kind == "file")
            {
                std::string p;
                lhs >> p;
                r.pathKey = pathKey(p);
                state().fileRoutes.push_back(r);
            }
            else if (kind == "lba")
            {
                lhs >> r.lba >> r.count;
                if (r.count == 0)
                    continue;
                state().lbaRoutes.push_back(r);
            }
        }
        log() << "voice routes: " << state().fileRoutes.size() << " file, " << state().lbaRoutes.size() << " LBA" << std::endl;
    }

    bool onCdHostPath(const std::string &ps2Path, const fs::path &, fs::path &out, void *)
    {
        const std::string key = pathKey(ps2Path);
        for (const auto &r : state().fileRoutes)
        {
            if (r.pathKey == key)
            {
                out = r.hostFile;
                return true;
            }
        }
        return false;
    }

    bool onCdSectorRead(uint32_t lbn, uint32_t sectors, uint8_t *dst, size_t byteCount, void *)
    {
        (void)sectors;
        for (const auto &r : state().lbaRoutes)
        {
            if (lbn < r.lba || lbn >= r.lba + r.count)
                continue;
            std::ifstream f(r.hostFile, std::ios::binary);
            if (!f)
                return false;
            std::memset(dst, 0, byteCount);
            f.seekg(static_cast<std::streamoff>(lbn - r.lba) * kSector);
            f.read(reinterpret_cast<char *>(dst), static_cast<std::streamsize>(byteCount));
            return true; // short read past EOF stays zero-filled, like padding on disc
        }
        return false;
    }

    void applyDataPatches(PS2Runtime &runtime)
    {
        PS2Memory &mem = runtime.memory();
        int applied = 0, skipped = 0, refused = 0;
        for (const DataPatch *p = kDataPatches; p->group; ++p)
        {
            if (!state().ini.flag(std::string("patches.") + p->group, false))
            {
                ++skipped;
                continue;
            }
            const uint32_t current = mem.read32(p->address);
            if (p->hasExpected && current != p->expected && current != p->value)
            {
                log() << "REFUSED " << p->group << " @0x" << std::hex << p->address << ": found 0x" << current
                      << ", expected 0x" << p->expected << std::dec << " (different ELF build?)" << std::endl;
                ++refused;
                continue;
            }
            // write32 is the runtime's official path. Data inside the ELF's load
            // segment logs "Marked code ... as modified"; that flag is bookkeeping
            // only (nothing reads it at the pinned commit), and the game's own
            // writes there do the same.
            mem.write32(p->address, p->value);
            ++applied;
            if (state().log)
                log() << "patch " << p->group << " @0x" << std::hex << p->address << " = 0x" << p->value << std::dec << std::endl;
        }
        log() << "data patches: " << applied << " applied, " << skipped << " disabled, " << refused << " refused" << std::endl;
    }

    void mountDisc()
    {
        const std::string image = state().ini.str("disc.image");
        if (image.empty())
            return;
        const fs::path iso = resolveGamePath(image);
        std::error_code ec;
        if (!fs::is_regular_file(iso, ec))
        {
            log() << "disc image not found: " << iso.string()
                  << " (only name-based file access will work)" << std::endl;
            return;
        }
        PS2Runtime::IoPaths paths = PS2Runtime::getIoPaths();
        paths.cdImage = iso;
        PS2Runtime::setIoPaths(paths);
        log() << "disc image: " << iso.string() << std::endl;
    }

    void setupDisplay()
    {
        const std::string aspect = Settings::lower(state().ini.str("display.aspect", "4:3"));
        float value = 4.0f / 3.0f;
        if (aspect == "16:9")
            value = 16.0f / 9.0f;
        else if (aspect == "16:10")
            value = 16.0f / 10.0f;
        else if (aspect == "native" || aspect == "0")
            value = 0.0f;
        ps2x_hooks::setPresentAspect(value);
        if (value > 0.0f && !state().ini.flag("patches.widescreen", false) && value > 1.5f)
            log() << "aspect " << aspect << " without [patches] widescreen = true will look stretched" << std::endl;
    }

    void setupVoices()
    {
        const std::string lang = Settings::lower(state().ini.str("audio.voice_language", "en"));
        if (lang != "jp" && lang != "ja")
            return;
        loadVoiceMap(state().gameDir / "voice_jp");
        if (!state().fileRoutes.empty())
            ps2x_hooks::setCdHostPathHook(&onCdHostPath, nullptr);
        if (!state().lbaRoutes.empty())
            ps2x_hooks::setCdSectorReadHook(&onCdSectorRead, nullptr);
    }

    uint32_t parseAddr(const std::string &text, uint32_t fallback)
    {
        try
        {
            return text.empty() ? fallback : static_cast<uint32_t>(std::stoul(text, nullptr, 0));
        }
        catch (...)
        {
            log() << "bad address '" << text << "', using 0x" << std::hex << fallback << std::dec << std::endl;
            return fallback;
        }
    }

    // Raw Danger sizes its heap to "all RAM below my stack": SetupThread puts the
    // main stack at 0x01FE8000-0x02000000 and MemutilInit memaligns ~24.4 MB in one
    // go. Upstream ps2recomp caps the guest heap at 0x01F00000 and keeps its own
    // pools / callback stacks in the top 1 MB, so the allocation fails
    // ("MemutilAllocAlign: Alloc Error") and every file load returns nothing.
    void setupMemory(PS2Runtime &runtime)
    {
        const std::string layout = Settings::lower(state().ini.str("memory.layout", "game"));
        if (layout == "upstream" || layout == "off")
        {
            log() << "memory layout: upstream ps2recomp defaults" << std::endl;
            return;
        }
        const uint32_t heapLimit = parseAddr(state().ini.str("memory.heap_limit"), 0x01FE8000u);
        const uint32_t pools = parseAddr(state().ini.str("memory.runtime_pools"), 0x00020000u);
        uint32_t stackFloor = 0x00052000u, stackTop = 0x00080000u;
        const std::string stacks = state().ini.str("memory.callback_stacks");
        if (const auto dash = stacks.find('-'); dash != std::string::npos)
        {
            stackFloor = parseAddr(Settings::trim(stacks.substr(0, dash)), stackFloor);
            stackTop = parseAddr(Settings::trim(stacks.substr(dash + 1)), stackTop);
        }
        constexpr uint32_t kPoolBytes = 0x31000u; // RPC packet + server + TLS + boot pools
        const bool overlap = (pools < stackTop && stackFloor < pools + kPoolBytes) || stackFloor >= stackTop ||
                             pools + kPoolBytes > 0x00100000u || stackTop > 0x00100000u;
        if (overlap)
        {
            log() << "memory layout: pools/stacks overlap or leave the kernel area - keeping upstream layout" << std::endl;
            return;
        }
        ps2x_hooks::setGuestMemoryLayout(heapLimit, pools, stackTop, stackFloor);
        runtime.configureGuestHeap(0u, heapLimit);
        log() << std::hex << "memory layout: heap <= 0x" << heapLimit << ", runtime pools 0x" << pools << "-0x"
              << (pools + kPoolBytes) << ", callback stacks 0x" << stackFloor << "-0x" << stackTop << std::dec << std::endl;
    }

    // Thread watchdog: logs the EE scheduler's snapshot (every guest thread's
    // state, wait object, PC/RA, plus semaphores/event flags with waiters) every
    // N seconds - the tool for deadlocks where the game just goes quiet.
    const char *statusName(EeThreadStatus s)
    {
        switch (s)
        {
        case EeThreadStatus::Running: return "RUN";
        case EeThreadStatus::Ready: return "READY";
        case EeThreadStatus::Waiting: return "WAIT";
        case EeThreadStatus::WaitingSuspended: return "WAIT+SUSP";
        case EeThreadStatus::Suspended: return "SUSP";
        case EeThreadStatus::Dormant: return "DORMANT";
        }
        return "?";
    }

    const char *waitName(EeWaitReason w)
    {
        switch (w)
        {
        case EeWaitReason::None: return "";
        case EeWaitReason::Sleep: return "sleep";
        case EeWaitReason::Semaphore: return "sema";
        case EeWaitReason::EventFlag: return "evflag";
        case EeWaitReason::VSync: return "vsync";
        case EeWaitReason::External: return "external";
        case EeWaitReason::Mpeg: return "mpeg";
        }
        return "?";
    }

    void startThreadWatchdog(PS2Runtime &runtime, int seconds)
    {
        PS2Runtime *rt = &runtime;
        std::thread([rt, seconds]()
                    {
            uint64_t lastCycle = ~0ull;
            for (;;)
            {
                std::this_thread::sleep_for(std::chrono::seconds(seconds));
                const EeKernelSnapshot snap = rt->eeScheduler().snapshot();
                std::ostringstream out;
                out << "threads @cycle " << snap.eeCycle << (snap.eeCycle == lastCycle ? " (NO PROGRESS)" : "")
                    << " running=" << snap.runningThreadId << "\n";
                lastCycle = snap.eeCycle;
                for (const EeThreadSnapshot &t : snap.threads)
                {
                    out << "    T" << t.id << " " << statusName(t.status);
                    if (t.waitReason != EeWaitReason::None)
                        out << " " << waitName(t.waitReason) << "#" << t.waitId;
                    out << std::hex << " pc=0x" << t.pc << " ra=0x" << t.ra << " entry=0x" << t.entry << std::dec
                        << " prio=" << t.currentPriority << " depth=" << t.invocationDepth << "\n";
                }
                for (const EeSemaphoreSnapshot &sm : snap.semaphores)
                    if (sm.waiters)
                        out << "    sema#" << sm.id << " count=" << sm.count << "/" << sm.maxCount << " waiters=" << sm.waiters << "\n";
                for (const EeEventFlagSnapshot &ef : snap.eventFlags)
                    if (ef.waiters)
                        out << "    evflag#" << ef.id << " bits=0x" << std::hex << ef.bits << std::dec << " waiters=" << ef.waiters << "\n";
                log() << out.str() << std::flush;
            } })
            .detach();
        log() << "thread watchdog: snapshot every " << seconds << " s" << std::endl;
    }

    void setupDebug(PS2Runtime &runtime)
    {
        int dumpSeconds = static_cast<int>(state().ini.number("debug.thread_dump", 0.0f));
        if (const char *env = std::getenv("RD_THREADS"))
            dumpSeconds = std::atoi(env);
        if (dumpSeconds > 0)
            startThreadWatchdog(runtime, dumpSeconds);

        std::string printfAddr = state().ini.str("debug.game_printf", "0x2C6240");
        if (const char *env = std::getenv("RD_GAME_PRINTF"))
            printfAddr = env;
        const std::string lowered = Settings::lower(printfAddr);
        if (!printfAddr.empty() && lowered != "off" && lowered != "0" && lowered != "false")
        {
            try
            {
                const uint32_t addr = static_cast<uint32_t>(std::stoul(printfAddr, nullptr, 0));
                if (rawdanger_debug::bindGamePrintf(runtime, addr))
                    log() << "game debug printf -> console (0x" << std::hex << addr << std::dec << ")" << std::endl;
            }
            catch (...)
            {
                log() << "game_printf: bad address '" << printfAddr << "'" << std::endl;
            }
        }
        std::string trace = state().ini.str("debug.trace");
        if (const char *env = std::getenv("RD_TRACE"))
            trace = env;
        if (!trace.empty())
            rawdanger_debug::installTraces(runtime, trace);
    }

    // ------------------------------------------------------------------ HLE bindings
    //
    // Game-side SDK functions the analyzer didn't map to a runtime handler. They
    // were recompiled as-is and talk to IOP RPC servers that only exist in the
    // real IOP BIOS (e.g. CDVDFSV 0x80000597), so they would spin forever.
    // Each binding checks the first instruction words before replacing anything.

    struct HleBinding
    {
        const char *name;
        uint32_t address;
        uint32_t words[4];
        PS2Runtime::RecompiledFunction handler;
    };

    const HleBinding kHleBindings[] = {
        // sceCdLayerSearchFile(file, name, layer) -> runtime sceCdSearchFile (layer ignored:
        // the US disc is single-layer). Busy-waits on SIF RPC 0x80000597 otherwise.
        {"sceCdLayerSearchFile", 0x002BF6B8u, {0x27BDFF40u, 0xFFB30050u, 0xFFB20040u, 0x0080982Du}, &ps2_stubs::sceCdSearchFile},
        // sceCdSearchFile(file, name) = sceCdLayerSearchFile(file, name, 0)
        {"sceCdSearchFile", 0x002BF9B8u, {0x27BDFFF0u, 0xFFBF0000u, 0x0C0AFDAEu, 0x0000302Du}, &ps2_stubs::sceCdSearchFile},
    };

    // ------------------------------------------------------------------ sound bank upload
    //
    // FUN_00309e50(cmd, arg) sends a command to Irem's IOP sound driver (RPC
    // 0x12346). Command 0x90001000 ("FileReadAllocSPU") streams a sample bank
    // from ROOT.DAT into SPU2 RAM through sceSdVoiceTrans on core 1. Under
    // ps2xIOP that core is already running IREMSND2's looping PCM auto-DMA, so
    // LIBSD refuses the transfer (-210) and the game retries forever, leaking
    // one IOP file descriptor per try. ps2recomp has no SPU2 sound emulation
    // yet, so the upload would have no audible effect anyway: answer it here
    // with the size the driver reports on success, without involving the IOP.
    // [hle] spu_bank_upload = 0 sends it to the IOP as normal.

    constexpr uint32_t kSndCommand = 0x00309E50u;
    constexpr uint32_t kSndCmdLoadSpu = 0x90001000u;

    PS2Runtime::RecompiledFunction g_sndCommandOriginal = nullptr;

    void sndCommandHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt)
    {
        if (getRegU32(ctx, 4) == kSndCmdLoadSpu)
        {
            const uint32_t param = getRegU32(ctx, 5);
            const uint8_t *p = nullptr;
            if (rawdanger_debug::guestPtr(*rt, param, 0x40, p))
            {
                uint32_t size = 0, spu = 0, offset = 0;
                std::memcpy(&size, p + 0x08, 4);
                std::memcpy(&spu, p + 0x0C, 4);
                std::memcpy(&offset, p + 0x3C, 4);
                static uint32_t count = 0;
                if (++count <= 20u)
                    log() << "hle: SPU bank upload skipped (" << size << " bytes from " << rawdanger_debug::guestString(*rt, param + 0x10)
                          << " +0x" << std::hex << offset << " -> SPU 0x" << spu << std::dec << ")" << std::endl;
                setReturnS32(ctx, static_cast<int32_t>(size));
                return;
            }
        }
        g_sndCommandOriginal(rdram, ctx, rt);
    }

    void setupSoundHle(PS2Runtime &runtime)
    {
        if (!state().ini.flag("hle.spu_bank_upload", true))
            return;
        const uint8_t *p = nullptr;
        const uint32_t expect[4] = {0x27BDFFA0u, 0xFFBF0050u, 0x7FB30040u, 0x7FB20030u};
        bool match = rawdanger_debug::guestPtr(runtime, kSndCommand, 16, p);
        for (int i = 0; match && i < 4; ++i)
        {
            uint32_t w = 0;
            std::memcpy(&w, p + i * 4, 4);
            match = (w == expect[i]);
        }
        g_sndCommandOriginal = match ? runtime.lookupFunction(kSndCommand) : nullptr;
        if (!g_sndCommandOriginal || !runtime.replaceFunction(kSndCommand, &sndCommandHook))
        {
            log() << "hle: sound command hook not installed (code at 0x309e50 doesn't match)" << std::endl;
            return;
        }
        log() << "hle: SPU sample-bank uploads answered on the EE side (no SPU2 emulation yet)" << std::endl;
    }

    // ------------------------------------------------------------------ code overlays
    //
    // The game loads MWo3 code overlays (OVERLAY\\DISC\\*.BIN) into fixed slots
    // with FUN_002c99f0, which then calls FUN_00100290(address, size) to clear
    // the BSS and run the overlay's static constructors. We wrap that function:
    // identify the overlay that was just read (header name, load address, size,
    // CRC32), drop whatever overlay previously occupied that memory from the
    // dispatch table, register the new one's recompiled entry points, and only
    // then let the original run its constructors.

    constexpr uint32_t kOverlayPostLoad = 0x00100290u;
    PS2Runtime::RecompiledFunction g_overlayPostLoadOriginal = nullptr;

    struct ActiveOverlay
    {
        const RdOverlayInfo *info;
        uint32_t lo, hi;
    };
    std::vector<ActiveOverlay> g_activeOverlays;

    uint32_t crc32(const uint8_t *p, size_t n)
    {
        static uint32_t table[256];
        static bool init = false;
        if (!init)
        {
            for (uint32_t i = 0; i < 256; ++i)
            {
                uint32_t c = i;
                for (int k = 0; k < 8; ++k)
                    c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
                table[i] = c;
            }
            init = true;
        }
        uint32_t c = 0xFFFFFFFFu;
        for (size_t i = 0; i < n; ++i)
            c = table[(c ^ p[i]) & 0xFFu] ^ (c >> 8);
        return c ^ 0xFFFFFFFFu;
    }

    std::string overlayStem(const std::string &file)
    {
        std::string stem = Settings::lower(file);
        const auto slash = stem.find_last_of("\\/");
        if (slash != std::string::npos)
            stem = stem.substr(slash + 1);
        const auto dot = stem.find('.');
        if (dot != std::string::npos)
            stem.resize(dot);
        return stem;
    }

    void activateOverlay(PS2Runtime &rt, uint32_t address, uint32_t size)
    {
        const uint8_t *p = nullptr;
        if (size < 0x40u || !rawdanger_debug::guestPtr(rt, address, size, p) || std::memcmp(p, "MWo3", 4) != 0)
        {
            log() << "overlay: no MWo3 header at 0x" << std::hex << address << std::dec << " (" << size << " bytes)" << std::endl;
            return;
        }
        const std::string file(reinterpret_cast<const char *>(p + 0x20), strnlen(reinterpret_cast<const char *>(p + 0x20), 32));
        const std::string stem = overlayStem(file);
        const RdOverlayInfo *info = nullptr;
        for (uint32_t i = 0; i < g_rdOverlayCount; ++i)
            if (g_rdOverlays[i].load == address && g_rdOverlays[i].size == size && overlayStem(g_rdOverlays[i].name) == stem)
                info = &g_rdOverlays[i];

        // whatever was mapped here before is gone either way
        const uint32_t lo = address, hi = address + size + (info ? info->bss : 0u);
        for (auto it = g_activeOverlays.begin(); it != g_activeOverlays.end();)
        {
            if (it->lo < hi && lo < it->hi)
            {
                for (uint32_t k = 0; k < it->info->count; ++k)
                    rt.replaceFunction(it->info->functions[k].address, nullptr);
                it = g_activeOverlays.erase(it);
            }
            else
                ++it;
        }

        if (!info)
        {
            log() << "overlay " << file << " @0x" << std::hex << address << std::dec << " (" << size
                  << " bytes): not recompiled - run `python rd.py overlays`" << std::endl;
            return;
        }
        const uint32_t crc = crc32(p, size);
        if (crc != info->crc32)
        {
            log() << "overlay " << file << ": CRC32 " << std::hex << crc << " != " << info->crc32 << std::dec
                  << " (different disc version?) - not registered" << std::endl;
            return;
        }
        uint32_t ok = 0;
        for (uint32_t k = 0; k < info->count; ++k)
            ok += rt.replaceFunction(info->functions[k].address, info->functions[k].function) ? 1u : 0u;
        g_activeOverlays.push_back({info, lo, hi});
        log() << "overlay " << file << " @0x" << std::hex << address << std::dec << ": " << ok << "/" << info->count
              << " entry points registered" << std::endl;
    }

    void overlayPostLoadHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt)
    {
        activateOverlay(*rt, getRegU32(ctx, 4), getRegU32(ctx, 5));
        g_overlayPostLoadOriginal(rdram, ctx, rt);
    }

    void setupOverlays(PS2Runtime &runtime)
    {
        const uint8_t *p = nullptr;
        const uint32_t expect[4] = {0x27BDFFC0u, 0xFFBF0030u, 0x7FB20020u, 0x7FB10010u};
        bool match = rawdanger_debug::guestPtr(runtime, kOverlayPostLoad, 16, p);
        for (int i = 0; match && i < 4; ++i)
        {
            uint32_t w = 0;
            std::memcpy(&w, p + i * 4, 4);
            match = (w == expect[i]);
        }
        g_overlayPostLoadOriginal = match ? runtime.lookupFunction(kOverlayPostLoad) : nullptr;
        if (!g_overlayPostLoadOriginal || !runtime.replaceFunction(kOverlayPostLoad, &overlayPostLoadHook))
        {
            log() << "overlays: loader hook not installed (code at 0x100290 doesn't match)" << std::endl;
            return;
        }
        size_t entries = 0;
        for (uint32_t i = 0; i < g_rdOverlayCount; ++i)
            entries += g_rdOverlays[i].count;
        log() << "overlays: " << g_rdOverlayCount << " recompiled overlay(s), " << entries << " entry points"
              << (g_rdOverlayCount ? "" : " - run `python rd.py overlays`") << std::endl;
    }

    // ------------------------------------------------------------------ GS capture hotkey
    //
    // F9 asks the GS frontend (patch 0010) to record the next N presented frames:
    // register state + local memory at the start, then every packet/upload. The
    // file goes to gs_captures/ next to the ELF; tools/gs_replay renders it.
    // raylib's IsKeyPressed() is only true for the one render-thread frame after
    // the press; the game thread polls at its own rate and can miss it, so the key
    // is edge-detected here from IsKeyDown(). [debug] gs_capture_at = N (or env
    // RD_GSCAP=N) also starts one capture automatically N seconds after boot.
    void pollGsCaptureKey(PS2Runtime &rt)
    {
        static const int frames = []
        {
            if (const char *env = std::getenv("RD_GSCAP_FRAMES"))
                return std::atoi(env);
            return static_cast<int>(state().ini.number("debug.gs_capture_frames", 2.0f));
        }();
        static const double autoAt = []
        {
            if (const char *env = std::getenv("RD_GSCAP"))
                return std::atof(env);
            return static_cast<double>(state().ini.number("debug.gs_capture_at", 0.0f));
        }();
        static const auto start = std::chrono::steady_clock::now();
        static bool wasDown = false;
        static bool autoDone = false;
        if (frames <= 0)
            return;

        const bool down = IsKeyDown(KEY_F9);
        bool trigger = down && !wasDown;
        wasDown = down;
        if (!autoDone && autoAt > 0.0 &&
            std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() >= autoAt)
        {
            autoDone = true;
            trigger = true;
        }
        if (!trigger)
            return;
        if (rt.gs().isCapturing())
        {
            rt.gs().stopCapture(); // F9 again: end the capture at the next frame
            log() << "GS capture stop requested (F9)" << std::endl;
            return;
        }
        const fs::path dir = state().gameDir / "gs_captures";
        std::error_code ec;
        fs::create_directories(dir, ec);
        const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        char stamp[32];
        std::strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", std::localtime(&now));
        const fs::path file = dir / (std::string("cap_") + stamp + ".gsr");
        if (rt.gs().requestCapture(file.string(), static_cast<uint32_t>(frames)))
            log() << "GS capture requested: " << frames << " frame(s) -> " << file.string() << std::endl;
        else
            log() << "GS capture request refused" << std::endl;
    }

    // ------------------------------------------------------------------ controller (libpad2)
    //
    // Raw Danger reads the pad through libpad2 (scePad2*), which gets its data
    // from DBCMAN/DS2U on the IOP by DMA. ps2xIOP only answers DBCMAN's version
    // query, so no pad data ever arrives and the game sees no input. The game's
    // per-frame pad routine (FUN_002dc110) only uses six libpad2 calls:
    // CreateSocket, GetState (must return 1 = stable), GetButtonProfile, Read,
    // GetButtonInfo(index 0-31) and sceVibGetProfile. Those are answered here
    // from the runtime's host pad backend (keyboard / first gamepad).
    //
    // GetButtonInfo indices, from the game's own mask table at 0x43F170:
    //   0 SELECT 1 L3 2 R3 3 START 4 UP 5 RIGHT 6 DOWN 7 LEFT
    //   8 L2 9 R2 10 L1 11 R1 12 TRIANGLE 13 CIRCLE 14 CROSS 15 SQUARE
    // i.e. bit i of the raw DualShock2 button word (active low), returning 1
    // when pressed. 16-19 are the sticks (LX, LY, RX, RY; FUN_002da9a0 turns
    // them into stick-direction bits), 20-31 pressure (0 or 255 here).
    // Our Read() buffer layout (consumed only by our GetButtonInfo):
    //   +0 buttons lo, +1 buttons hi (active low), +2 LX +3 LY +4 RX +5 RY, +6..+17 pressure.

    constexpr uint32_t kPad2ReadSize = 18u;
    std::vector<uint32_t> g_pad2SocketAreas; // socket number = first-seen order of the work area

    uint8_t *guestWritable(PS2Runtime &rt, uint32_t addr, uint32_t len)
    {
        const uint8_t *p = nullptr;
        return rawdanger_debug::guestPtr(rt, addr, len, p) ? const_cast<uint8_t *>(p) : nullptr;
    }

    void hleReturn(R5900Context *ctx, int32_t v)
    {
        setReturnS32(ctx, v);
        ctx->pc = getRegU32(ctx, 31);
    }

    // scePad2CreateSocket(param, workArea) -> socket number (stable per work area, so a
    // re-created socket for port 1 keeps number 1 and port 0 keeps 0)
    void hlePad2CreateSocket(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        const uint32_t area = getRegU32(ctx, 5);
        size_t i = 0;
        while (i < g_pad2SocketAreas.size() && g_pad2SocketAreas[i] != area)
            ++i;
        if (i == g_pad2SocketAreas.size())
            g_pad2SocketAreas.push_back(area);
        hleReturn(ctx, static_cast<int32_t>(i));
    }

    // scePad2GetState(socket) -> 1 (stable) for socket 0, 0 (no device) otherwise
    void hlePad2GetState(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        hleReturn(ctx, getRegU32(ctx, 4) == 0u ? 1 : 0);
    }

    // scePad2GetButtonProfile(socket, profile) -> profile size: buttons 0-31 present
    void hlePad2GetButtonProfile(uint8_t *, R5900Context *ctx, PS2Runtime *rt)
    {
        if (uint8_t *out = guestWritable(*rt, getRegU32(ctx, 5), 5))
        {
            out[0] = out[1] = out[2] = out[3] = 0xFF;
            out[4] = 0x00;
        }
        hleReturn(ctx, 4);
    }

    // scePad2Read(socket, data) -> bytes written, -1 when no device
    void hlePad2Read(uint8_t *, R5900Context *ctx, PS2Runtime *rt)
    {
        pollGsCaptureKey(*rt);
        uint8_t *out = guestWritable(*rt, getRegU32(ctx, 5), kPad2ReadSize);
        if (!out || getRegU32(ctx, 4) != 0u)
        {
            hleReturn(ctx, -1);
            return;
        }
        uint8_t raw[32] = {};
        if (!rt->padBackend().readState(0, 0, raw, sizeof(raw)))
        {
            raw[2] = raw[3] = 0xFF;
            raw[4] = raw[5] = raw[6] = raw[7] = 0x80;
        }
        out[0] = raw[2];
        out[1] = raw[3];
        out[2] = raw[6]; // LX
        out[3] = raw[7]; // LY
        out[4] = raw[4]; // RX
        out[5] = raw[5]; // RY
        // pressure 20-31 follow the DualShock2 order: R L U D tri circle cross square L1 R1 L2 R2
        static const uint8_t kPressureButton[12] = {5, 7, 4, 6, 12, 13, 14, 15, 10, 11, 8, 9};
        const uint16_t buttons = static_cast<uint16_t>(raw[2] | (raw[3] << 8));
        for (int k = 0; k < 12; ++k)
            out[6 + k] = ((buttons >> kPressureButton[k]) & 1u) ? 0x00 : 0xFF;
        hleReturn(ctx, static_cast<int32_t>(kPad2ReadSize));
    }

    // scePad2GetButtonInfo(socket, data, index) -> 0/1 for buttons, 0-255 for analog, -1 unknown
    void hlePad2GetButtonInfo(uint8_t *, R5900Context *ctx, PS2Runtime *rt)
    {
        const uint32_t index = getRegU32(ctx, 6);
        const uint8_t *data = nullptr;
        if (index >= 32u || !rawdanger_debug::guestPtr(*rt, getRegU32(ctx, 5), kPad2ReadSize, data))
        {
            hleReturn(ctx, -1);
            return;
        }
        if (index < 16u)
        {
            const uint16_t buttons = static_cast<uint16_t>(data[0] | (data[1] << 8));
            hleReturn(ctx, ((buttons >> index) & 1u) ? 0 : 1);
            return;
        }
        hleReturn(ctx, data[2 + (index - 16u)]);
    }

    // sceVibGetProfile(socket, profile) -> 0: no actuators (the game then skips vibration)
    void hleVibGetProfile(uint8_t *, R5900Context *ctx, PS2Runtime *rt)
    {
        if (uint8_t *out = guestWritable(*rt, getRegU32(ctx, 5), 4))
            std::memset(out, 0, 4);
        hleReturn(ctx, 0);
    }

    // sceVibSetActParam(...) -> accepted, no-op
    void hleVibSetActParam(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        hleReturn(ctx, 1);
    }

    const HleBinding kPadBindings[] = {
        {"scePad2CreateSocket", 0x002DD5F8u, {0x27BDFF90u, 0x0080302Du, 0xFFB10040u, 0x00A0882Du}, &hlePad2CreateSocket},
        {"scePad2Read", 0x002DD758u, {0x27BDFFC0u, 0x24020334u, 0xFFB10010u, 0x3C03006Cu}, &hlePad2Read},
        {"scePad2GetButtonProfile", 0x002DD830u, {0x27BDFFC0u, 0xFFB10010u, 0xFFB20020u, 0x0080882Du}, &hlePad2GetButtonProfile},
        {"scePad2GetState", 0x002DD900u, {0x27BDFFB0u, 0x24030334u, 0xFFB10020u, 0x0080882Du}, &hlePad2GetState},
        {"scePad2GetButtonInfo", 0x002DDA40u, {0x24030334u, 0x00063100u, 0x00832018u, 0x3C02006Cu}, &hlePad2GetButtonInfo},
        {"sceVibGetProfile", 0x002DDE08u, {0x2402C000u, 0x24030002u, 0x00421024u, 0x3C06FFFFu}, &hleVibGetProfile},
        {"sceVibSetActParam", 0x002DDE88u, {0x27BDFF60u, 0x2402C000u, 0xFFB40080u, 0x2403000Bu}, &hleVibSetActParam},
    };

    // Binds a table of HLE handlers after checking each function's first 4 words.
    int bindHle(PS2Runtime &runtime, const HleBinding *begin, const HleBinding *end, const char *group, int &skipped)
    {
        int bound = 0;
        for (const HleBinding *b = begin; b != end; ++b)
        {
            const uint8_t *p = nullptr;
            bool match = rawdanger_debug::guestPtr(runtime, b->address, 16, p);
            for (int i = 0; match && i < 4; ++i)
            {
                uint32_t w = 0;
                std::memcpy(&w, p + i * 4, 4);
                match = (w == b->words[i]);
            }
            if (!match)
            {
                log() << "hle: " << group << " " << b->name << " @0x" << std::hex << b->address << std::dec
                      << " doesn't match the expected code - not bound" << std::endl;
                ++skipped;
                continue;
            }
            if (runtime.replaceFunction(b->address, b->handler))
                ++bound;
            else
                ++skipped;
        }
        return bound;
    }

    void setupPadHle(PS2Runtime &runtime)
    {
        if (!state().ini.flag("hle.pad", true))
        {
            log() << "hle: controller HLE off ([hle] pad = 0) - the game gets no input" << std::endl;
            return;
        }
        int skipped = 0;
        const int bound = bindHle(runtime, std::begin(kPadBindings), std::end(kPadBindings), "pad", skipped);
        log() << "hle: controller (libpad2) answered from the host pad: " << bound << " function(s) bound, " << skipped
              << " skipped. Keys: arrows/WASD d-pad, X/Space cross, C/Esc circle, Z square, V triangle, Q/E L1/R1, "
                 "Shift L2/R2, Enter start, Tab select" << std::endl;
    }

    // ------------------------------------------------------------------ FMV
    //
    // FMVs (opening movie, story cutscenes) play through FUN_00212bc0, called
    // only by the play-movie wrapper FUN_00212f00, which sets the GS mode up
    // before and restores it after. The player streams the PSS file from
    // ROOT.DAT (sceCdSt*) and decodes it with libmpeg on the IPU. ps2recomp has
    // no IPU/MPEG-2 decoder, so no frame ever completes and the player waits
    // for the end of the movie forever (main thread looping at 0x212DC8).
    // [movies] skip = 1 returns from the player at once, as if the movie had
    // been skipped; the wrapper still does its setup/restore.

    void hleMovieSkip(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        static uint32_t count = 0;
        if (++count <= 50u)
            log() << "movie skipped (FMV playback needs an MPEG decoder - [movies] skip)" << std::endl;
        hleReturn(ctx, 0);
    }

    const HleBinding kMovieBindings[] = {
        {"moviePlayer", 0x00212BC0u, {0x27BDFF80u, 0xFFBF0050u, 0x7FB40040u, 0x7FB30030u}, &hleMovieSkip},
    };

    void setupMovieHle(PS2Runtime &runtime)
    {
        if (!state().ini.flag("movies.skip", true))
        {
            log() << "movies: played normally ([movies] skip = 0) - expect a hang at the first FMV" << std::endl;
            return;
        }
        int skipped = 0;
        if (bindHle(runtime, std::begin(kMovieBindings), std::end(kMovieBindings), "movies", skipped) > 0)
            log() << "movies: FMVs are skipped (no MPEG decoder yet)" << std::endl;
    }

    void setupHle(PS2Runtime &runtime)
    {
        setupSoundHle(runtime);
        setupPadHle(runtime);
        setupMovieHle(runtime);
        int bound = 0, skipped = 0;
        for (const HleBinding &b : kHleBindings)
        {
            if (!state().ini.flag(std::string("hle.") + b.name, true))
            {
                ++skipped;
                continue;
            }
            const uint8_t *p = nullptr;
            bool match = rawdanger_debug::guestPtr(runtime, b.address, 16, p);
            for (int i = 0; match && i < 4; ++i)
            {
                uint32_t w = 0;
                std::memcpy(&w, p + i * 4, 4);
                match = (w == b.words[i]);
            }
            if (!match)
            {
                log() << "hle: " << b.name << " @0x" << std::hex << b.address << std::dec
                      << " doesn't match the expected code - not bound" << std::endl;
                ++skipped;
                continue;
            }
            if (runtime.replaceFunction(b.address, b.handler))
                ++bound;
            else
                ++skipped;
        }
        log() << "hle: " << bound << " SDK function(s) routed to runtime handlers, " << skipped << " skipped" << std::endl;
    }

    void apply(PS2Runtime &runtime)
    {
        State &s = state();
        s.gameDir = PS2Runtime::getIoPaths().elfDirectory;
        if (s.gameDir.empty())
            s.gameDir = fs::current_path();
        const fs::path iniPath = s.gameDir / "rawdanger.ini";
        if (!s.ini.load(iniPath))
        {
            std::ofstream(iniPath) << kDefaultIni;
            s.ini.load(iniPath);
            log() << "wrote default settings: " << iniPath.string() << std::endl;
        }
        s.log = s.ini.flag("debug.log", true);
        log() << "Raw Danger! PC recomp module for " << RD_SERIAL << " (" << RD_ELF_NAME << ")" << std::endl;
        setupMemory(runtime);
        mountDisc();
        applyDataPatches(runtime);
        setupDisplay();
        setupVoices();
        setupHle(runtime);
        setupOverlays(runtime);
        setupDebug(runtime);
    }
}

PS2_REGISTER_GAME_OVERRIDE("Raw Danger! (USA) PC recomp", RD_ELF_NAME, RD_ELF_ENTRY, RD_ELF_CRC32, &rawdanger_detail::apply);
