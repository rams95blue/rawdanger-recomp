// vu_replay - replays the VIF1/VU1 part of a GS capture (.gsr with patch 0014
// records 9/10) through ps2recomp's VIF1 interpreter and VU1 interpreter, and
// reports what VU1 sends to the GS (XGKICK packets): vertex positions, STQ,
// colours. Used to debug 3D models that come out wrong.
//
//   vu_replay capture.gsr [--dump-vu N] [--stream N] [--verbose]
//
// --stream N   stop after N VIF1 streams
// --dump-vu N  write VU1 data memory to vu_data_<N>.bin before stream N
// --dump-gif F write every GIF packet VU1/VIF1 produce to F ([u32 len][bytes]...)
//
// Build (Linux, against a ps2recomp build with patches 0001-0014):
//   g++ -std=c++20 -O1 -msse4.2 -mavx2 -I<ps2recomp>/ps2xRuntime/include \
//       -I<ps2recomp>/ps2xRuntime/include/runtime vu_replay.cpp test_function_table.cpp.o \
//       libps2_runtime.a libps2_iop.a libfmt.a libraylib.a -lGL -lav* -lpthread -ldl
// (test_function_table.cpp.o comes from the ps2x_tests build.)
#include "runtime/ps2_memory.h"
#include "runtime/ps2_vu1.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/gs/ps2_gif_arbiter.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace
{
    struct Stats
    {
        uint64_t packets = 0, vertices = 0;
        std::map<uint32_t, uint64_t> xyzHist; // screen (x>>4, y>>4) bucket count
        double minX = 1e9, maxX = -1e9, minY = 1e9, maxY = -1e9;
        uint64_t zeroST = 0;
    };

    float asF(uint32_t v)
    {
        float f;
        std::memcpy(&f, &v, 4);
        return f;
    }

    void inspectGif(const uint8_t *p, uint32_t len, Stats &st, bool verbose)
    {
        ++st.packets;
        uint32_t off = 0;
        while (off + 16 <= len)
        {
            uint64_t lo, hi;
            std::memcpy(&lo, p + off, 8);
            std::memcpy(&hi, p + off + 8, 8);
            off += 16;
            const uint32_t nloop = lo & 0x7FFF;
            const bool eop = (lo >> 15) & 1;
            const uint32_t flg = (lo >> 58) & 3;
            uint32_t nreg = (lo >> 60) & 0xF;
            if (!nreg)
                nreg = 16;
            if (flg == 0)
            {
                float lastS = 0, lastT = 0, lastQ = 0;
                for (uint32_t l = 0; l < nloop; ++l)
                    for (uint32_t r = 0; r < nreg && off + 16 <= len; ++r, off += 16)
                    {
                        const uint32_t reg = (hi >> (4 * r)) & 0xF;
                        uint64_t a, b;
                        std::memcpy(&a, p + off, 8);
                        std::memcpy(&b, p + off + 8, 8);
                        if (reg == 2)
                        {
                            lastS = asF(uint32_t(a));
                            lastT = asF(uint32_t(a >> 32));
                            lastQ = asF(uint32_t(b));
                        }
                        if (reg == 4 || reg == 5)
                        {
                            const double x = (a & 0xFFFF) / 16.0, y = ((a >> 32) & 0xFFFF) / 16.0;
                            const uint32_t z = reg == 5 ? uint32_t(b) : uint32_t((b >> 4) & 0xFFFFFF);
                            ++st.vertices;
                            st.minX = std::min(st.minX, x); st.maxX = std::max(st.maxX, x);
                            st.minY = std::min(st.minY, y); st.maxY = std::max(st.maxY, y);
                            if (lastS == 0 && lastT == 0)
                                ++st.zeroST;
                            if (verbose && st.vertices <= 64)
                                std::printf("    v x=%.2f y=%.2f z=%#x s=%g t=%g q=%g\n", x, y, z, lastS, lastT, lastQ);
                        }
                    }
            }
            else if (flg == 1)
                off += ((nloop * nreg + 1) / 2) * 16;
            else
                off += nloop * 16;
            if (eop && off >= len)
                break;
        }
    }
}

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: vu_replay capture.gsr [--stream N] [--dump-vu N] [--verbose]\n");
        return 1;
    }
    long stopStream = -1, dumpAt = -1;
    bool verbose = false;
    FILE *gifDump = nullptr;
    for (int i = 2; i < argc; ++i)
    {
        if (!std::strcmp(argv[i], "--stream") && i + 1 < argc) stopStream = std::atol(argv[++i]);
        else if (!std::strcmp(argv[i], "--dump-vu") && i + 1 < argc) dumpAt = std::atol(argv[++i]);
        else if (!std::strcmp(argv[i], "--verbose")) verbose = true;
        else if (!std::strcmp(argv[i], "--dump-gif") && i + 1 < argc) gifDump = std::fopen(argv[++i], "wb");
    }
    std::ifstream f(argv[1], std::ios::binary);
    std::vector<uint8_t> file((std::istreambuf_iterator<char>(f)), {});
    uint32_t hdr[5];
    std::memcpy(hdr, file.data(), 20);
    size_t off = 20 + size_t(hdr[2]) * 8 + size_t(hdr[3]) * 8 + hdr[4];

    PS2Memory mem;
    if (!mem.initialize())
        return 1;
    static GSRegisters regs{};
    GS gs;
    gs.init(mem.getGSVRAM(), PS2_GS_VRAM_SIZE, &regs);
    Stats vuStats, recStats;
    std::map<std::string, Stats> perClass;
    std::string currentClass = "none";
    uint64_t vuStreamPackets = 0;
    GifArbiter arbiter([&](const uint8_t *d, uint32_t n) {
        inspectGif(d, n, vuStats, verbose);
        if (std::getenv("VUR_CLASSES"))
            inspectGif(d, n, perClass[currentClass], false);
        if (gifDump) { std::fwrite(&n, 4, 1, gifDump); std::fwrite(d, 1, n, gifDump); }
    });
    mem.setGifArbiter(&arbiter);
    VU1Interpreter vu1;
    uint64_t mscals = 0;
    std::map<uint32_t, uint64_t> mscalPcs;
    mem.setVu1MscalCallback([&](uint32_t pc, uint32_t top, uint32_t itop) {
        ++mscals;
        if (pc == 0)
        {
            float q[16];
            std::memcpy(q, mem.getVU1Data() + 4 * 16, 64);
            const bool identical = std::memcmp(q, q + 4, 16) == 0 && std::memcmp(q, q + 8, 16) == 0;
            char buf[96];
            std::snprintf(buf, sizeof(buf), identical ? "rows-identical(%.3g,%.3g,%.3g,%.3g)" : "matrix", q[0], q[1], q[2], q[3]);
            currentClass = identical ? buf : "matrix";
        }
        if (mscalPcs[pc]++ == 0 && std::getenv("VUR_DUMP"))
        {
            char name[64];
            std::snprintf(name, sizeof(name), "vu_code_%04x.bin", pc);
            std::ofstream(name, std::ios::binary).write(reinterpret_cast<const char *>(mem.getVU1Code()), PS2_VU1_CODE_SIZE);
            std::snprintf(name, sizeof(name), "vu_mem_%04x.bin", pc);
            std::ofstream(name, std::ios::binary).write(reinterpret_cast<const char *>(mem.getVU1Data()), PS2_VU1_DATA_SIZE);
        }
        const bool firstDump = std::getenv("VUR_DUMP") && mscalPcs[pc] == 1;
        vu1.execute(mem.getVU1Code(), PS2_VU1_CODE_SIZE, mem.getVU1Data(), PS2_VU1_DATA_SIZE, gs, &mem, pc, top, itop, 65536);
        if (firstDump)
        {
            char name[64];
            std::snprintf(name, sizeof(name), "vu_mem_after_%04x.bin", pc);
            std::ofstream(name, std::ios::binary).write(reinterpret_cast<const char *>(mem.getVU1Data()), PS2_VU1_DATA_SIZE);
        }
    });
    mem.setVu1MscntCallback([&](uint32_t top, uint32_t itop) {
        ++mscals;
        vu1.resume(mem.getVU1Code(), PS2_VU1_CODE_SIZE, mem.getVU1Data(), PS2_VU1_DATA_SIZE, gs, &mem, top, itop, 65536);
    });

    bool haveState = false;
    long streams = 0;
    while (off + 5 <= file.size())
    {
        const uint8_t type = file[off];
        uint32_t len;
        std::memcpy(&len, &file[off + 1], 4);
        off += 5;
        if (type == 0xFF || off + len > file.size())
            break;
        const uint8_t *p = &file[off];
        off += len;
        if (type == 10)
        {
            uint32_t o = 0, n;
            std::memcpy(&n, p + o, 4); o += 4;
            std::memcpy(mem.getVU1Code(), p + o, n); o += n;
            std::memcpy(&n, p + o, 4); o += 4;
            std::memcpy(mem.getVU1Data(), p + o, n); o += n;
            if (o + sizeof(VIFRegisters) + sizeof(VU1State) <= len)
            {
                std::memcpy(&mem.vif1_regs, p + o, sizeof(VIFRegisters)); o += sizeof(VIFRegisters);
                std::memcpy(&vu1.state(), p + o, sizeof(VU1State));
            }
            else
                std::printf("record 10 has no VIF/VU register block (or a different build); using defaults\n");
            haveState = true;
            std::printf("VU1 start state restored (pc=%#x top=%#x)\n", vu1.state().pc, mem.vif1_regs.top);
        }
        else if (type == 9)
        {
            if (stopStream >= 0 && streams >= stopStream)
                break;
            if (streams == dumpAt)
            {
                char name[64];
                std::snprintf(name, sizeof(name), "vu_data_%ld.bin", streams);
                std::ofstream(name, std::ios::binary).write(reinterpret_cast<const char *>(mem.getVU1Data()), PS2_VU1_DATA_SIZE);
                std::printf("dumped %s\n", name);
            }
            const uint64_t before = vuStats.packets;
            mem.processVIF1Data(p, len);
            arbiter.drain();
            vuStreamPackets += vuStats.packets - before;
            ++streams;
        }
        else if (type == 1 || type == 2)
            inspectGif(p, len, recStats, false);
    }
    if (!haveState)
        std::printf("no VU1 start state in this capture (record 10) - needs patch 0014\n");
    std::printf("VIF1 streams %ld, MSCAL/MSCNT %llu\n", streams, (unsigned long long)mscals);
    for (const auto &e : mscalPcs)
        std::printf("  MSCAL pc %#06x x%llu\n", e.first, (unsigned long long)e.second);
    std::printf("replayed GIF output: %llu packets, %llu vertices, x[%.1f,%.1f] y[%.1f,%.1f], vertices with S=T=0: %llu\n",
                (unsigned long long)vuStats.packets, (unsigned long long)vuStats.vertices, vuStats.minX, vuStats.maxX, vuStats.minY,
                vuStats.maxY, (unsigned long long)vuStats.zeroST);
    for (const auto &e : perClass)
        std::printf("  [%s] packets %llu vertices %llu x[%.1f,%.1f] y[%.1f,%.1f]\n", e.first.c_str(),
                    (unsigned long long)e.second.packets, (unsigned long long)e.second.vertices, e.second.minX - 1728,
                    e.second.maxX - 1728, e.second.minY - 1824, e.second.maxY - 1824);
    std::printf("recorded GIF (all paths): %llu packets, %llu vertices, x[%.1f,%.1f] y[%.1f,%.1f], S=T=0: %llu\n",
                (unsigned long long)recStats.packets, (unsigned long long)recStats.vertices, recStats.minX, recStats.maxX, recStats.minY,
                recStats.maxY, (unsigned long long)recStats.zeroST);
    return 0;
}
