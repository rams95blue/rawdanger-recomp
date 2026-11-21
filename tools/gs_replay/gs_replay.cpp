// gs_replay - renders a GS capture (.gsr, written by the runtime with F9 /
// [debug] gs_capture) offline with ps2recomp's GS frontend + CPU backend.
//
//   gs_replay capture.gsr outdir [--vram] [--log]
//
// Writes outdir/frame_NNN.png for every presented frame, outdir/vram_start.png
// and vram_end.png (local memory viewed as PSMCT32, 1024 wide) with --vram, and
// outdir/draws.txt (every GS draw / register / transfer event, per frame) with --log.
// No game, disc or GPU needed: this is how renderer bugs get reproduced and fixed.

#include "runtime/gs/gs_frontend.h"
#include "runtime/gs/gs_cpu_backend.h"
#include "runtime/ps2_memory.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <algorithm>
#include <cstdlib>

namespace
{
    // ---------------------------------------------------------- tiny PNG writer (stored deflate)
    uint32_t crcTable[256];
    void initCrc()
    {
        for (uint32_t i = 0; i < 256; ++i)
        {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            crcTable[i] = c;
        }
    }
    uint32_t crc(const uint8_t *p, size_t n, uint32_t c = 0xFFFFFFFFu)
    {
        for (size_t i = 0; i < n; ++i)
            c = crcTable[(c ^ p[i]) & 0xFFu] ^ (c >> 8);
        return c;
    }
    void be32(std::vector<uint8_t> &v, uint32_t x)
    {
        v.push_back(uint8_t(x >> 24)); v.push_back(uint8_t(x >> 16)); v.push_back(uint8_t(x >> 8)); v.push_back(uint8_t(x));
    }
    void chunk(std::vector<uint8_t> &out, const char *type, const std::vector<uint8_t> &data)
    {
        be32(out, uint32_t(data.size()));
        std::vector<uint8_t> td(type, type + 4);
        td.insert(td.end(), data.begin(), data.end());
        out.insert(out.end(), td.begin(), td.end());
        be32(out, crc(td.data(), td.size()) ^ 0xFFFFFFFFu);
    }
    bool writePng(const std::string &path, const uint8_t *rgba, uint32_t w, uint32_t h, bool opaque)
    {
        std::vector<uint8_t> raw;
        raw.reserve(size_t(w) * h * 4 + h);
        for (uint32_t y = 0; y < h; ++y)
        {
            raw.push_back(0);
            for (uint32_t x = 0; x < w; ++x)
            {
                const uint8_t *p = rgba + (size_t(y) * w + x) * 4;
                raw.push_back(p[0]); raw.push_back(p[1]); raw.push_back(p[2]);
                raw.push_back(opaque ? 255 : p[3]);
            }
        }
        std::vector<uint8_t> z{0x78, 0x01};
        uint32_t a = 1, b = 0;
        for (uint8_t c : raw) { a = (a + c) % 65521u; b = (b + a) % 65521u; }
        for (size_t off = 0; off < raw.size() || off == 0; off += 65535)
        {
            const size_t n = std::min<size_t>(65535, raw.size() - off);
            z.push_back(off + n >= raw.size() ? 1 : 0);
            z.push_back(uint8_t(n)); z.push_back(uint8_t(n >> 8));
            z.push_back(uint8_t(~n)); z.push_back(uint8_t((~n) >> 8));
            z.insert(z.end(), raw.begin() + off, raw.begin() + off + n);
            if (off + n >= raw.size())
                break;
        }
        be32(z, (b << 16) | a);
        std::vector<uint8_t> out{0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
        std::vector<uint8_t> ihdr;
        be32(ihdr, w); be32(ihdr, h);
        ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});
        chunk(out, "IHDR", ihdr);
        chunk(out, "IDAT", z);
        chunk(out, "IEND", {});
        std::ofstream f(path, std::ios::binary);
        f.write(reinterpret_cast<const char *>(out.data()), std::streamsize(out.size()));
        return bool(f);
    }

    void dumpVram(GS &gs, const std::string &path)
    {
        std::vector<uint8_t> img(1024u * 1024u * 4u);
        for (uint32_t y = 0; y < 1024; ++y)
            for (uint32_t x = 0; x < 1024; ++x)
            {
                const uint32_t v = gs.ReadVram(0x00 /* PSMCT32 */, 0, 16, x, y);
                std::memcpy(&img[(size_t(y) * 1024 + x) * 4], &v, 4);
            }
        writePng(path, img.data(), 1024, 1024, true);
    }

    const char *kindName(GSDebugEventKind k)
    {
        switch (k)
        {
        case GSDebugEventKind::GifTag: return "giftag";
        case GSDebugEventKind::Register: return "reg";
        case GSDebugEventKind::Draw: return "DRAW";
        case GSDebugEventKind::Transfer: return "xfer";
        case GSDebugEventKind::Present: return "PRESENT";
        }
        return "?";
    }

    void logHistory(GS &gs, std::ofstream &log)
    {
        for (const GSDebugHistoryEntry &e : gs.getDebugHistory())
        {
            log << kindName(e.kind);
            switch (e.kind)
            {
            case GSDebugEventKind::Register:
                log << " r" << std::hex << unsigned(e.reg) << "=" << e.regValue << std::dec;
                break;
            case GSDebugEventKind::Draw:
                log << " prim=" << int(e.prim.type) << " tme=" << e.prim.tme << " abe=" << e.prim.abe << " fst=" << e.prim.fst
                    << " iip=" << e.prim.iip << " ctxt=" << e.prim.ctxt << " verts=" << e.vertexCount
                    << " fbp=0x" << std::hex << e.frame.fbp << " fbw=" << std::dec << unsigned(e.frame.fbw) << " fpsm=0x" << std::hex << unsigned(e.frame.psm)
                    << " tbp=0x" << e.tex0.tbp0 << " tbw=" << std::dec << unsigned(e.tex0.tbw) << " tpsm=0x" << std::hex << unsigned(e.tex0.psm)
                    << " tw=" << std::dec << unsigned(e.tex0.tw) << " th=" << unsigned(e.tex0.th) << " cbp=0x" << std::hex << e.tex0.cbp
                    << " cpsm=0x" << unsigned(e.tex0.cpsm) << " csm=" << unsigned(e.tex0.csm) << " csa=" << std::dec << unsigned(e.tex0.csa)
                    << " tfx=" << unsigned(e.tex0.tfx) << " tcc=" << unsigned(e.tex0.tcc)
                    << " test=0x" << std::hex << e.test << " alpha=0x" << e.alpha << std::dec
                    << " x=[" << e.xMin << "," << e.xMax << "] y=[" << e.yMin << "," << e.yMax << "]";
                break;
            case GSDebugEventKind::Transfer:
                log << " dbp=0x" << std::hex << e.bitbltbuf.dbp << " dbw=" << std::dec << unsigned(e.bitbltbuf.dbw) << " dpsm=0x" << std::hex
                    << unsigned(e.bitbltbuf.dpsm) << std::dec << " dir=" << e.trxdir << " pixels=" << e.transferPixels;
                break;
            case GSDebugEventKind::Present:
                log << " dispfbp=0x" << std::hex << e.displayFbp << " srcfbp=0x" << e.sourceFbp << std::dec << " " << e.width << "x" << e.height
                    << (e.usedPreferred ? " (preferred source)" : "");
                break;
            default:
                log << " nloop=" << e.gifNloop << " flg=" << unsigned(e.gifFlg) << " nreg=" << unsigned(e.gifNreg);
                break;
            }
            log << "\n";
        }
        gs.clearDebugHistory();
    }
}

int main(int argc, char **argv)
{
    if (argc < 3)
    {
        std::cerr << "usage: gs_replay capture.gsr outdir [--vram] [--log] [--every N]\n";
        return 1;
    }
    initCrc();
    const std::string in = argv[1], outDir = argv[2];
    bool wantVram = false, wantLog = false;
    uint32_t every = 1;
    for (int i = 3; i < argc; ++i)
    {
        wantVram |= std::strcmp(argv[i], "--vram") == 0;
        wantLog |= std::strcmp(argv[i], "--log") == 0;
        if (std::strcmp(argv[i], "--every") == 0 && i + 1 < argc)
            every = std::max(1, std::atoi(argv[++i]));
    }
    std::ifstream f(in, std::ios::binary);
    std::vector<uint8_t> file((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (file.size() < 20)
    {
        std::cerr << "can't read " << in << "\n";
        return 1;
    }
    uint32_t hdr[5];
    std::memcpy(hdr, file.data(), sizeof(hdr));
    if (hdr[0] != 0x53474452u || hdr[1] != 1u)
    {
        std::cerr << "not a GS capture (RDGS v1)\n";
        return 1;
    }
    size_t off = sizeof(hdr);
    const uint64_t *raw = reinterpret_cast<const uint64_t *>(file.data() + off);
    off += size_t(hdr[2]) * 8;
    uint64_t priv[20];
    std::memcpy(priv, file.data() + off, sizeof(priv));
    off += size_t(hdr[3]) * 8;
    const uint8_t *vramStart = file.data() + off;
    off += hdr[4];

    static GSRegisters regs{};
    auto setPriv = [](const uint64_t *p)
    {
        regs.pmode = p[0]; regs.smode1 = p[1]; regs.smode2 = p[2]; regs.srfsh = p[3]; regs.synch1 = p[4];
        regs.synch2 = p[5]; regs.syncv = p[6]; regs.dispfb1 = p[7]; regs.display1 = p[8]; regs.dispfb2 = p[9];
        regs.display2 = p[10]; regs.extbuf = p[11]; regs.extdata = p[12]; regs.extwrite = p[13]; regs.bgcolor = p[14];
        regs.csr.store(p[15]); regs.vsyncTick.store(p[16]); regs.imr = p[17]; regs.busdir = p[18]; regs.siglblid = p[19];
    };
    setPriv(priv);

    std::vector<uint8_t> vram(4u * 1024u * 1024u, 0u);
    GS gs;
    gs.init(vram.data(), uint32_t(vram.size()), &regs);
    gs.restoreCaptureState(raw, hdr[2], vramStart, hdr[4]);
    gs.setDebugHistoryPaused(!wantLog);
    gs.clearDebugHistory();
    if (wantVram)
        dumpVram(gs, outDir + "/vram_start.png");
    std::ofstream log;
    if (wantLog)
        log.open(outDir + "/draws.txt");

    uint32_t frame = 0, records = 0;
    while (off + 5 <= file.size())
    {
        const uint8_t type = file[off];
        uint32_t len = 0;
        std::memcpy(&len, file.data() + off + 1, 4);
        off += 5;
        if (type == 0xFF || off + len > file.size())
            break;
        const uint8_t *p = file.data() + off;
        off += len;
        ++records;
        switch (type)
        {
        case 1: gs.processGIFPacket(p, len); break;
        case 2: gs.processNativePackedGIFPacket(p, len); break;
        case 3:
        {
            uint64_t s[4];
            std::memcpy(s, p, 32);
            gs.uploadImageNative(s[0], s[1], s[2], s[3], p + 32, len - 32);
            break;
        }
        case 4:
        {
            uint64_t v;
            std::memcpy(&v, p + 1, 8);
            gs.writeRegister(p[0], v);
            break;
        }
        case 5:
        {
            setPriv(reinterpret_cast<const uint64_t *>(p));
            gs.latchHostPresentationFrame();
            std::vector<uint8_t> px;
            uint32_t w = 0, h = 0, dfbp = 0, sfbp = 0;
            bool pref = false;
            if (frame % every != 0)
            {
                if (wantLog)
                {
                    logHistory(gs, log);
                    log << "==== end of frame " << frame << "\n";
                }
                ++frame;
                break;
            }
            if (gs.copyLatchedHostPresentationFrame(px, w, h, &dfbp, &sfbp, &pref) && w && h)
            {
                char name[64];
                std::snprintf(name, sizeof(name), "/frame_%03u.png", frame);
                writePng(outDir + name, px.data(), w, h, true);
                std::cout << "frame " << frame << ": " << w << "x" << h << " display fbp 0x" << std::hex << dfbp << " source fbp 0x" << sfbp
                          << std::dec << (pref ? " (preferred)" : "") << " PMODE=0x" << std::hex << regs.pmode << " DISPFB1=0x" << regs.dispfb1
                          << " DISPLAY1=0x" << regs.display1 << " DISPFB2=0x" << regs.dispfb2 << " DISPLAY2=0x" << regs.display2 << std::dec << "\n";
            }
            else
                std::cout << "frame " << frame << ": nothing presented\n";
            if (wantLog)
            {
                logHistory(gs, log);
                log << "==== end of frame " << frame << "\n";
            }
            ++frame;
            break;
        }
        case 6:
        {
            uint32_t r[2];
            std::memcpy(r, p, 8);
            gs.clearFramebufferContext(r[0], r[1]);
            break;
        }
        case 7:
        {
            uint32_t r;
            std::memcpy(&r, p, 4);
            gs.clearActiveFramebuffer(r);
            break;
        }
        case 8:
        {
            uint32_t r[6];
            std::memcpy(r, p, 24);
            gs.WriteVram(r[0], r[1], r[2], r[3], r[4], r[5]);
            break;
        }
        default: break;
        }
        if (wantLog && type != 5)
        {
            if (!gs.getDebugHistory().empty())
                log << "# record " << records << "\n";
            logHistory(gs, log);
        }
    }
    if (wantVram)
        dumpVram(gs, outDir + "/vram_end.png");
    std::cout << records << " records, " << frame << " frame(s)\n";
    return 0;
}
