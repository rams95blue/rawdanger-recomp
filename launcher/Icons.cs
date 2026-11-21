// Raw Danger PC - the icons of the desktop shortcuts: the launcher's yellow warning sign with the version on it
// ("US", "JP", "JP TYPE A", "JP TYPE B", "EX"), drawn here and saved as a .ico next to the game (games\<name>\<name>.ico).
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
using System.IO;

namespace RawDangerLauncher
{
    public static class Icons
    {
        static readonly Color Yellow = Color.FromArgb(255, 204, 0), Ink = Color.FromArgb(18, 16, 10), Blue = Color.FromArgb(24, 96, 206), White = Color.FromArgb(250, 252, 255);

        // US: yellow sign, black ink. JP and the JP demos: blue sign, white ink (the JP release's colours). Enhanced: both.
        public enum Style { US, JP, Both }

        public static Style StyleOf(string label)
        {
            if (label == "EX") return Style.Both;
            return label.StartsWith("JP") ? Style.JP : Style.US;
        }

        // The text on a game's icon, from its folder name.
        public static string Label(string gameName)
        {
            string n = gameName.ToUpperInvariant();
            if (n.StartsWith("RAWDANGER-ENHANCED")) return "EX";
            if (n.StartsWith("RAWDANGER-US")) return "US";
            if (n.StartsWith("RAWDANGER-EU")) return "EU";
            if (n.StartsWith("ZZT2-DEMOA")) return "JP\nTYPE A";
            if (n.StartsWith("ZZT2-DEMOB")) return "JP\nTYPE B";
            if (n.StartsWith("ZZT2-DEMO")) return "JP\nDEMO";
            if (n.StartsWith("ZZT2")) return "JP";
            return "";
        }

        // games\<name>\<name>.ico, made again each time (cheap), so a changed label shows up.
        public static string ForGame(Settings s, string gameName)
        {
            string file = Path.Combine(Games.Folder(s, gameName), gameName + ".ico");
            try { Save(Label(gameName), file); return file; }
            catch (Exception e) { Program.Trace("icon " + gameName + ": " + e.Message); return ""; }
        }

        public static void Save(string label, string file)
        {
            int[] sizes = { 256, 64, 48, 32, 24, 16 };
            List<byte[]> pngs = new List<byte[]>();
            using (Bitmap big = Draw(label, 1024))
                foreach (int n in sizes)
                    using (Bitmap b = new Bitmap(n, n, PixelFormat.Format32bppArgb))
                    {
                        using (Graphics g = Graphics.FromImage(b))
                        {
                            g.InterpolationMode = InterpolationMode.HighQualityBicubic;
                            g.PixelOffsetMode = PixelOffsetMode.HighQuality;
                            g.DrawImage(big, new Rectangle(0, 0, n, n));
                        }
                        // 256: PNG; the smaller ones as plain 32-bit bitmaps, which every Windows part reads (the taskbar's and
                        // the resource compiler's way with PNG entries is not to be counted on)
                        if (n >= 256) using (MemoryStream ms = new MemoryStream()) { b.Save(ms, ImageFormat.Png); pngs.Add(ms.ToArray()); }
                        else pngs.Add(Dib(b));
                    }
            // .ico: header, one directory entry per size, then the PNG images
            using (FileStream f = File.Create(file))
            using (BinaryWriter w = new BinaryWriter(f))
            {
                w.Write((short)0); w.Write((short)1); w.Write((short)sizes.Length);
                int offset = 6 + 16 * sizes.Length;
                for (int i = 0; i < sizes.Length; i++)
                {
                    w.Write((byte)(sizes[i] >= 256 ? 0 : sizes[i])); w.Write((byte)(sizes[i] >= 256 ? 0 : sizes[i]));
                    w.Write((byte)0); w.Write((byte)0); w.Write((short)1); w.Write((short)32);
                    w.Write(pngs[i].Length); w.Write(offset);
                    offset += pngs[i].Length;
                }
                foreach (byte[] p in pngs) w.Write(p);
            }
        }

        // an icon entry as a bitmap: BITMAPINFOHEADER (height twice: picture and mask), BGRA rows bottom up, then the AND mask
        static byte[] Dib(Bitmap b)
        {
            int n = b.Width, maskRow = ((n + 31) / 32) * 4;
            using (MemoryStream ms = new MemoryStream())
            using (BinaryWriter w = new BinaryWriter(ms))
            {
                w.Write(40); w.Write(n); w.Write(n * 2); w.Write((short)1); w.Write((short)32); w.Write(0); w.Write(n * n * 4 + maskRow * n);
                w.Write(0); w.Write(0); w.Write(0); w.Write(0);
                for (int y = n - 1; y >= 0; y--)
                    for (int x = 0; x < n; x++) { Color c = b.GetPixel(x, y); w.Write(c.B); w.Write(c.G); w.Write(c.R); w.Write(c.A); }
                for (int y = n - 1; y >= 0; y--)
                {
                    byte[] row = new byte[maskRow];
                    for (int x = 0; x < n; x++) if (b.GetPixel(x, y).A == 0) row[x >> 3] |= (byte)(0x80 >> (x & 7));
                    w.Write(row);
                }
                return ms.ToArray();
            }
        }

        // The sign: black rim, yellow diamond, thin black inner line; a small "!" above the label (or a big one without a label).
        public static Bitmap Draw(string label, int size)
        {
            Style style = StyleOf(label);
            Bitmap bmp = new Bitmap(size, size, PixelFormat.Format32bppArgb);
            using (Graphics g = Graphics.FromImage(bmp))
            {
                g.SmoothingMode = SmoothingMode.AntiAlias;
                g.TextRenderingHint = System.Drawing.Text.TextRenderingHint.AntiAliasGridFit;
                float c = size / 2f, r = c - size * 0.02f, r2 = r - size * 0.045f, r3 = r2 - size * 0.075f;
                Color face = style == Style.JP ? Blue : Yellow, inkColor = style == Style.JP ? White : Ink;
                using (SolidBrush rim = new SolidBrush(Ink)) g.FillPolygon(rim, Diamond(c, r));
                using (SolidBrush y = new SolidBrush(face)) g.FillPolygon(y, Diamond(c, r2));
                if (style == Style.Both)
                {
                    // the right half blue: the US and JP signs in one
                    Region old = g.Clip;
                    g.SetClip(new RectangleF(c, 0, c, size));
                    using (SolidBrush b = new SolidBrush(Blue)) g.FillPolygon(b, Diamond(c, r2));
                    g.Clip = old;
                }
                using (Pen p = new Pen(style == Style.JP ? White : Ink, size * 0.035f)) { p.LineJoin = LineJoin.Round; g.DrawPolygon(p, Diamond(c, r3)); }
                using (SolidBrush ink = new SolidBrush(style == Style.Both ? White : inkColor))
                {
                    if (label == "")
                    {
                        Mark(g, ink, c, c - r3 * 0.62f, c + r3 * 0.18f, size * 0.11f, c + r3 * 0.45f);
                        return bmp;
                    }
                    string[] lines = label.Split('\n');
                    // One line: a small "!" above it. Two lines: no "!", the text centred on the diamond's widest part, and each
                    // line kept inside the diamond at its own height.
                    if (lines.Length == 1)
                        using (SolidBrush markInk = new SolidBrush(style == Style.Both ? Ink : (style == Style.JP ? White : Ink)))
                            Mark(g, markInk, c, c - r3 * 0.70f, c - r3 * 0.40f, size * 0.06f, c - r3 * 0.27f);
                    float boxW = lines.Length == 1 ? r3 * 1.25f : r3 * 1.02f, boxH = lines.Length == 1 ? r3 * 0.62f : r3 * 0.92f;
                    float boxTop = lines.Length == 1 ? c - r3 * 0.12f : c - boxH / 2f;
                    using (FontFamily family = Family())
                    {
                        float em = boxH / lines.Length * 1.05f;
                        for (int tries = 0; tries < 40; tries++, em *= 0.95f)
                        {
                            float widest = 0;
                            foreach (string line in lines) widest = Math.Max(widest, Measure(g, family, line, em).Width);
                            if (widest <= boxW) break;
                        }
                        float lineH = em * 0.98f, y = boxTop + (boxH - lineH * lines.Length) / 2f;
                        foreach (string line in lines)
                        {
                            RectangleF m = Measure(g, family, line, em);
                            using (GraphicsPath path = new GraphicsPath())
                            {
                                path.AddString(line, family, (int)FontStyle.Bold, em, new PointF(0, 0), StringFormat.GenericTypographic);
                                RectangleF b = path.GetBounds();
                                Matrix move = new Matrix();
                                move.Translate(c - b.Width / 2f - b.X, y + (lineH - b.Height) / 2f - b.Y);
                                path.Transform(move);
                                if (style == Style.Both) using (Pen edge = new Pen(Ink, size * 0.03f)) { edge.LineJoin = LineJoin.Round; g.DrawPath(edge, path); }
                                g.FillPath(ink, path);
                            }
                            y += lineH;
                        }
                    }
                }
            }
            return bmp;
        }

        static FontFamily Family()
        {
            foreach (string name in new string[] { "Impact", "Arial Black", "Segoe UI" })
                try { return new FontFamily(name); } catch (Exception) { }
            return FontFamily.GenericSansSerif;
        }

        static RectangleF Measure(Graphics g, FontFamily family, string text, float em)
        {
            using (GraphicsPath path = new GraphicsPath())
            {
                path.AddString(text, family, (int)FontStyle.Bold, em, new PointF(0, 0), StringFormat.GenericTypographic);
                return path.GetBounds();
            }
        }

        static void Mark(Graphics g, Brush ink, float c, float top, float bottom, float width, float dotY)
        {
            using (GraphicsPath bar = new GraphicsPath())
            {
                float rr = width * 0.35f;
                RectangleF rect = new RectangleF(c - width / 2f, top, width, bottom - top);
                bar.AddArc(rect.X, rect.Y, rr * 2, rr * 2, 180, 90);
                bar.AddArc(rect.Right - rr * 2, rect.Y, rr * 2, rr * 2, 270, 90);
                bar.AddArc(rect.Right - rr * 2, rect.Bottom - rr * 2, rr * 2, rr * 2, 0, 90);
                bar.AddArc(rect.X, rect.Bottom - rr * 2, rr * 2, rr * 2, 90, 90);
                bar.CloseFigure();
                g.FillPath(ink, bar);
            }
            float d = width * 0.62f;
            g.FillEllipse(ink, c - d, dotY - d, 2 * d, 2 * d);
        }

        static PointF[] Diamond(float c, float r) { return new PointF[] { new PointF(c, c - r), new PointF(c + r, c), new PointF(c, c + r), new PointF(c - r, c) }; }
    }
}
