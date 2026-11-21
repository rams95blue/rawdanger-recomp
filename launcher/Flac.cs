// Raw Danger PC - a small FLAC decoder, so the launcher can play music.flac on any Windows (MCI has no FLAC support of its
// own, and Windows N editions have no Media Foundation). The whole file is decoded once to a 16-bit WAV in the temp folder,
// kept while the .flac is unchanged, and that WAV is what MCI plays. Follows the FLAC format specification (RFC 9639):
// STREAMINFO, frames with constant / verbatim / fixed / LPC subframes, Rice-coded residuals, the three stereo decorrelations.
using System;
using System.IO;

namespace RawDangerLauncher
{
    public static class Flac
    {
        class Bits
        {
            readonly byte[] d;
            public long Pos;   // in bits
            public Bits(byte[] data, long bytePos) { d = data; Pos = bytePos * 8; }
            public bool End { get { return (Pos >> 3) >= d.Length; } }
            public uint U(int n)
            {
                uint v = 0;
                for (int i = 0; i < n; i++)
                {
                    long b = Pos >> 3;
                    if (b >= d.Length) throw new EndOfStreamException();
                    v = (v << 1) | (uint)((d[b] >> (7 - (int)(Pos & 7))) & 1);
                    Pos++;
                }
                return v;
            }
            public long ULong(int n) { long v = 0; for (int i = 0; i < n; i++) v = (v << 1) | U(1); return v; }
            public int S(int n)
            {
                if (n == 0) return 0;
                uint v = U(n);
                if ((v & (1u << (n - 1))) != 0) return (int)(v | (n == 32 ? 0 : ~0u << n));
                return (int)v;
            }
            public uint Unary()
            {
                uint n = 0;
                while (true)
                {
                    long b = Pos >> 3;
                    if (b >= d.Length) throw new EndOfStreamException();
                    int bit = (d[b] >> (7 - (int)(Pos & 7))) & 1;
                    Pos++;
                    if (bit == 1) return n;
                    n++;
                }
            }
            public void Align() { Pos = (Pos + 7) & ~7L; }
        }

        // Decodes `flac` into a 16-bit PCM WAV file. Throws on a file that is not FLAC or is damaged.
        public static void ToWav(string flac, string wav)
        {
            byte[] d = File.ReadAllBytes(flac);
            if (d.Length < 42 || d[0] != 'f' || d[1] != 'L' || d[2] != 'a' || d[3] != 'C') throw new InvalidDataException("not a FLAC file");
            int pos = 4, rate = 0, channels = 0, bps = 0;
            long total = 0;
            bool last = false;
            while (!last)
            {
                last = (d[pos] & 0x80) != 0;
                int type = d[pos] & 0x7F, len = (d[pos + 1] << 16) | (d[pos + 2] << 8) | d[pos + 3];
                if (type == 0)
                {
                    Bits b = new Bits(d, pos + 4 + 10);
                    rate = (int)b.U(20);
                    channels = (int)b.U(3) + 1;
                    bps = (int)b.U(5) + 1;
                    total = b.ULong(36);
                }
                pos += 4 + len;
            }
            if (rate == 0) throw new InvalidDataException("FLAC without STREAMINFO");

            using (FileStream fs = new FileStream(wav, FileMode.Create, FileAccess.Write))
            using (BinaryWriter w = new BinaryWriter(fs))
            {
                w.Write(new byte[44]);   // header written at the end, when the length is known
                long frames = 0;
                Bits r = new Bits(d, pos);
                int[][] ch = new int[8][];
                byte[] buf = new byte[0];
                while (!r.End)
                {
                    // frame header: find the sync code (14 bits 11111111111110) at a byte boundary
                    r.Align();
                    long at = r.Pos >> 3;
                    if (at + 2 > d.Length) break;
                    if (d[at] != 0xFF || (d[at + 1] & 0xFE) != 0xF8) { r.Pos += 8; continue; }
                    r.U(15); r.U(1);
                    int bsCode = (int)r.U(4), srCode = (int)r.U(4), chCode = (int)r.U(4), ssCode = (int)r.U(3);
                    r.U(1);
                    // the UTF-8-like coded frame / sample number
                    uint first = r.U(8);
                    int more = 0;
                    for (uint m = 0x80; (first & m) != 0 && m > 1; m >>= 1) more++;
                    for (int i = 0; i < Math.Max(0, more - 1); i++) r.U(8);
                    int block;
                    if (bsCode == 1) block = 192;
                    else if (bsCode >= 2 && bsCode <= 5) block = 576 << (bsCode - 2);
                    else if (bsCode == 6) block = (int)r.U(8) + 1;
                    else if (bsCode == 7) block = (int)r.U(16) + 1;
                    else if (bsCode >= 8) block = 256 << (bsCode - 8);
                    else { r.Pos = (at + 1) * 8; continue; }   // reserved: not a frame
                    if (srCode == 12) r.U(8); else if (srCode == 13 || srCode == 14) r.U(16);
                    r.U(8);   // CRC-8 (not checked)
                    int sampleBits = ssCode == 0 ? bps : ssCode == 1 ? 8 : ssCode == 2 ? 12 : ssCode == 4 ? 16 : ssCode == 5 ? 20 : ssCode == 6 ? 24 : ssCode == 7 ? 32 : bps;
                    int nch = chCode < 8 ? chCode + 1 : 2;
                    for (int c = 0; c < nch; c++)
                    {
                        if (ch[c] == null || ch[c].Length < block) ch[c] = new int[block];
                        int extra = (chCode == 8 && c == 1) || (chCode == 9 && c == 0) || (chCode == 10 && c == 1) ? 1 : 0;   // the side channel has one more bit
                        Subframe(r, ch[c], block, sampleBits + extra);
                    }
                    r.Align();
                    r.U(16);   // CRC-16 (not checked)
                    if (chCode == 8) for (int i = 0; i < block; i++) ch[1][i] = ch[0][i] - ch[1][i];          // left / side
                    else if (chCode == 9) for (int i = 0; i < block; i++) ch[0][i] = ch[0][i] + ch[1][i];     // side / right
                    else if (chCode == 10)
                        for (int i = 0; i < block; i++)                                                         // mid / side
                        {
                            long mid = ((long)ch[0][i] << 1) | (uint)(ch[1][i] & 1), side = ch[1][i];
                            ch[0][i] = (int)((mid + side) >> 1);
                            ch[1][i] = (int)((mid - side) >> 1);
                        }
                    if (buf.Length < block * nch * 2) buf = new byte[block * nch * 2];
                    int k = 0;
                    for (int i = 0; i < block; i++)
                        for (int c = 0; c < nch; c++)
                        {
                            int v = ch[c][i];
                            if (sampleBits > 16) v >>= sampleBits - 16; else if (sampleBits < 16) v <<= 16 - sampleBits;
                            if (v > 32767) v = 32767; else if (v < -32768) v = -32768;
                            buf[k++] = (byte)v; buf[k++] = (byte)(v >> 8);
                        }
                    w.Write(buf, 0, k);
                    frames += block;
                    if (total > 0 && frames >= total) break;
                    channels = nch;
                }
                long dataBytes = fs.Length - 44;
                fs.Seek(0, SeekOrigin.Begin);
                w.Write(new char[] { 'R', 'I', 'F', 'F' }); w.Write((int)(36 + dataBytes));
                w.Write(new char[] { 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ' }); w.Write(16); w.Write((short)1); w.Write((short)channels);
                w.Write(rate); w.Write(rate * channels * 2); w.Write((short)(channels * 2)); w.Write((short)16);
                w.Write(new char[] { 'd', 'a', 't', 'a' }); w.Write((int)dataBytes);
            }
        }

        static void Subframe(Bits r, int[] o, int n, int bits)
        {
            r.U(1);
            int type = (int)r.U(6);
            int wasted = 0;
            if (r.U(1) == 1) wasted = (int)r.Unary() + 1;
            bits -= wasted;
            if (type == 0)
            {
                int v = r.S(bits);
                for (int i = 0; i < n; i++) o[i] = v;
            }
            else if (type == 1)
            {
                for (int i = 0; i < n; i++) o[i] = r.S(bits);
            }
            else if (type >= 8 && type <= 12)
            {
                int order = type - 8;
                for (int i = 0; i < order; i++) o[i] = r.S(bits);
                Residual(r, o, n, order);
                for (int i = order; i < n; i++)
                {
                    long p;
                    switch (order)
                    {
                        case 0: p = 0; break;
                        case 1: p = o[i - 1]; break;
                        case 2: p = 2L * o[i - 1] - o[i - 2]; break;
                        case 3: p = 3L * o[i - 1] - 3L * o[i - 2] + o[i - 3]; break;
                        default: p = 4L * o[i - 1] - 6L * o[i - 2] + 4L * o[i - 3] - o[i - 4]; break;
                    }
                    o[i] = (int)(p + o[i]);
                }
            }
            else if (type >= 32)
            {
                int order = type - 31;
                for (int i = 0; i < order; i++) o[i] = r.S(bits);
                int precision = (int)r.U(4) + 1;
                int shift = r.S(5);
                int[] coef = new int[order];
                for (int i = 0; i < order; i++) coef[i] = r.S(precision);
                Residual(r, o, n, order);
                for (int i = order; i < n; i++)
                {
                    long sum = 0;
                    for (int j = 0; j < order; j++) sum += (long)coef[j] * o[i - 1 - j];
                    o[i] = (int)((sum >> shift) + o[i]);
                }
            }
            else throw new InvalidDataException("reserved FLAC subframe type " + type);
            if (wasted > 0) for (int i = 0; i < n; i++) o[i] <<= wasted;
        }

        // Rice-coded residuals, written into o[order..n-1] (the predictor adds its prediction to them afterwards)
        static void Residual(Bits r, int[] o, int n, int order)
        {
            int method = (int)r.U(2);
            int paramBits = method == 0 ? 4 : 5, escape = method == 0 ? 15 : 31;
            int porder = (int)r.U(4);
            int parts = 1 << porder, at = order;
            for (int p = 0; p < parts; p++)
            {
                int count = (n >> porder) - (p == 0 ? order : 0);
                int k = (int)r.U(paramBits);
                if (k == escape)
                {
                    int raw = (int)r.U(5);
                    for (int i = 0; i < count; i++) o[at++] = r.S(raw);
                    continue;
                }
                for (int i = 0; i < count; i++)
                {
                    uint q = r.Unary();
                    uint v = (q << k) | (k > 0 ? r.U(k) : 0);
                    o[at++] = (int)(v >> 1) ^ -(int)(v & 1);
                }
            }
        }
    }
}
