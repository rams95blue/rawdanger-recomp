// Raw Danger PC - the cast panel: the characters cycle on the right of the launcher, each with a name plate.
// The text is characters.txt (built into the exe; a characters.txt next to the pictures replaces it). The pictures are read from
// the art folder (launcher\art, or art next to the exe; launcher.ini's CastPictures= can name another folder).
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.IO;
using System.Text;
using System.Windows.Forms;

namespace RawDangerLauncher
{
    public class CastMember
    {
        public string Key = "", File = "", Look = "", En = "", Jp = "", Kana = "", Romaji = "", RoleEn = "", RoleJp = "", BioEn = "", BioJp = "";
        public Image Picture;
        public string Name { get { return Lang.JP ? Jp : En; } }
        public string Other { get { return Lang.JP ? Kana + "  /  " + En : Jp + "  /  " + Romaji; } }
        public string Role { get { return Lang.JP ? RoleJp : RoleEn; } }
        public string Bio { get { return Lang.JP ? BioJp : BioEn; } }
    }

    public static class CastData
    {
        public static List<CastMember> Load(Settings settings)
        {
            List<CastMember> list = new List<CastMember>();
            string text = null;
            try
            {
                Stream st = typeof(CastData).Assembly.GetManifestResourceStream("RawDangerLauncher.characters.txt");
                if (st != null) using (StreamReader r = new StreamReader(st, Encoding.UTF8)) text = r.ReadToEnd();
            }
            catch (Exception e) { Program.Trace("cast text: " + e.Message); }
            // the pictures: the folder where the most of them are found (by file name, or by what the picture looks like)
            string folder = "";
            Dictionary<string, string> files = new Dictionary<string, string>();
            List<string> candidates = Candidates(settings);
            foreach (string dir in candidates)
            {
                // a characters.txt next to the pictures replaces the built-in text
                try { if (File.Exists(Path.Combine(dir, "characters.txt"))) { text = File.ReadAllText(Path.Combine(dir, "characters.txt"), Encoding.UTF8); break; } }
                catch (Exception) { }
            }
            if (text == null) return list;
            Parse(text, list);
            foreach (string dir in candidates)
            {
                Dictionary<string, string> found = Match(dir, list);
                if (found.Count > files.Count) { files = found; folder = dir; }
                if (files.Count == list.Count) break;
            }
            int pictures = 0;
            foreach (CastMember c in list)
            {
                string file;
                if (!files.TryGetValue(c.Key, out file)) continue;
                try { using (FileStream f = File.OpenRead(file)) c.Picture = Cutout(new Bitmap(Image.FromStream(f))); pictures++; }
                catch (Exception e) { Program.Trace("cast picture " + file + ": " + e.Message); }
            }
            int all = list.Count;
            list.RemoveAll(delegate(CastMember c) { return c.Picture == null; });   // only the characters with a picture
            Program.Trace("cast: " + all + " characters, " + pictures + " pictures from " + (folder == "" ? "(none; looked in " + candidates.Count + " folders)" : folder));
            return list;
        }

        // The first of these files in the art folder (where the cast pictures are).
        public static string FindFile(Settings settings, string[] names)
        {
            List<string> dirs = Candidates(settings);
            foreach (string dir in dirs)
                foreach (string n in names)
                {
                    string f = Path.Combine(dir, n);
                    if (File.Exists(f)) return f;
                }
            return "";
        }

        static readonly string[] AudioTypes = { ".flac", ".mp3", ".wav", ".wma", ".m4a" };

        // The launcher's music, from the art folder like the pictures: a file named music.* first, else any music file there (any name).
        public static string FindAudio(Settings settings)
        {
            List<string> named = new List<string>();
            foreach (string t in AudioTypes) named.Add("music" + t);
            string f = FindFile(settings, named.ToArray());
            if (f != "") return f;
            foreach (string d in Candidates(settings)) { f = Audio(d); if (f != "") return f; }
            return "";
        }

        // The first music file of a folder (by name, FLAC first), not huge
        static string Audio(string dir)
        {
            try
            {
                string[] all = Directory.GetFiles(dir);
                Array.Sort(all, StringComparer.OrdinalIgnoreCase);
                foreach (string t in AudioTypes)
                    foreach (string f in all)
                        if (f.EndsWith(t, StringComparison.OrdinalIgnoreCase) && new FileInfo(f).Length < 200L * 1024 * 1024) return f;
            }
            catch (Exception) { }
            return "";
        }

        static void Parse(string text, List<CastMember> list)
        {
            CastMember m = null;
            foreach (string raw in text.Split('\n'))
            {
                string line = raw.Trim();
                if (line == "" || line.StartsWith("#")) continue;
                if (line.StartsWith("[") && line.EndsWith("]")) { m = new CastMember(); m.Key = line.Substring(1, line.Length - 2); list.Add(m); continue; }
                int eq = line.IndexOf('=');
                if (m == null || eq <= 0) continue;
                string k = line.Substring(0, eq).Trim(), v = line.Substring(eq + 1).Trim();
                switch (k)
                {
                    case "picture": m.File = v; break;
                    case "look": m.Look = v; break;
                    case "en": m.En = v; break;
                    case "jp": m.Jp = v; break;
                    case "kana": m.Kana = v; break;
                    case "romaji": m.Romaji = v; break;
                    case "role_en": m.RoleEn = v; break;
                    case "role_jp": m.RoleJp = v; break;
                    case "bio_en": m.BioEn = v; break;
                    case "bio_jp": m.BioJp = v; break;
                }
            }
        }

        // The folders that hold the pictures (and the launcher's music): the art folder only - launcher\art in the project, or an art
        // folder next to the exe - and a folder chosen in launcher.ini (CastPictures=) if there is one.
        public static List<string> Candidates(Settings settings)
        {
            List<string> list = new List<string>();
            Action<string> add = delegate(string d)
            {
                if (d == null || d == "" || !Directory.Exists(d)) return;
                foreach (string x in list) if (string.Equals(x, d, StringComparison.OrdinalIgnoreCase)) return;
                list.Add(d);
            };
            add(settings.CastPictures);
            add(Path.Combine(Path.Combine(Settings.ExeDir, "launcher"), "art"));
            add(Path.Combine(Settings.ExeDir, "art"));
            return list;
        }

        static List<string> Pictures(string dir)
        {
            List<string> list = new List<string>();
            try
            {
                foreach (string f in Directory.GetFiles(dir))
                {
                    string ext = Path.GetExtension(f).ToLowerInvariant();
                    if ((ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp") && new FileInfo(f).Length < 8 * 1024 * 1024) list.Add(f);
                    if (list.Count > 200) break;
                }
            }
            catch (Exception) { }
            return list;
        }

        // Which picture in this folder is which character: by file name (picture=, or <key>.png/.jpg), and otherwise by
        // what it looks like (the look= fingerprint in characters.txt), so renamed files are still found.
        static Dictionary<string, string> Match(string dir, List<CastMember> cast)
        {
            Dictionary<string, string> result = new Dictionary<string, string>();
            List<string> pictures = Pictures(dir);
            if (pictures.Count == 0) return result;
            List<string> used = new List<string>();
            foreach (CastMember c in cast)
                foreach (string name in new string[] { c.File, c.Key + ".png", c.Key + ".jpg", c.Key + ".jpeg", c.Key + ".bmp" })
                {
                    if (name == "") continue;
                    string file = Path.Combine(dir, name);
                    if (File.Exists(file)) { result[c.Key] = file; used.Add(file.ToLowerInvariant()); break; }
                }
            if (result.Count == cast.Count) return result;
            // the rest by look: every unused picture against every character still missing, closest pairs first
            List<KeyValuePair<int, KeyValuePair<string, string>>> pairs = new List<KeyValuePair<int, KeyValuePair<string, string>>>();
            foreach (string file in pictures)
            {
                if (used.Contains(file.ToLowerInvariant())) continue;
                string look = "";
                try { using (FileStream f = File.OpenRead(file)) using (Bitmap b = new Bitmap(Image.FromStream(f))) look = Fingerprint(b); }
                catch (Exception) { continue; }
                foreach (CastMember c in cast)
                {
                    if (result.ContainsKey(c.Key) || c.Look == "") continue;
                    int d = Distance(look, c.Look);
                    if (d <= 18) pairs.Add(new KeyValuePair<int, KeyValuePair<string, string>>(d, new KeyValuePair<string, string>(c.Key, file)));
                }
            }
            pairs.Sort(delegate(KeyValuePair<int, KeyValuePair<string, string>> a, KeyValuePair<int, KeyValuePair<string, string>> b) { return a.Key.CompareTo(b.Key); });
            foreach (KeyValuePair<int, KeyValuePair<string, string>> p in pairs)
            {
                if (result.ContainsKey(p.Value.Key) || used.Contains(p.Value.Value.ToLowerInvariant())) continue;
                result[p.Value.Key] = p.Value.Value;
                used.Add(p.Value.Value.ToLowerInvariant());
            }
            return result;
        }

        // A picture's look as 128 bits (32 hex digits): the figure (non-white, non-transparent pixels) cut out, laid on white,
        // shrunk to 8 x 16 grey cells by plain averaging, each cell darker or lighter than the mean. Works the same for the
        // cut-out .png files and the originals on a white card, and for any file name.
        public static string Fingerprint(Bitmap src)
        {
            int w = src.Width, h = src.Height;
            int[] px = new int[w * h];
            using (Bitmap b = new Bitmap(w, h, System.Drawing.Imaging.PixelFormat.Format32bppArgb))
            {
                using (Graphics g = Graphics.FromImage(b)) { g.CompositingMode = CompositingMode.SourceCopy; g.DrawImageUnscaled(src, 0, 0); }
                System.Drawing.Imaging.BitmapData data = b.LockBits(new Rectangle(0, 0, w, h), System.Drawing.Imaging.ImageLockMode.ReadOnly, System.Drawing.Imaging.PixelFormat.Format32bppArgb);
                System.Runtime.InteropServices.Marshal.Copy(data.Scan0, px, 0, px.Length);
                b.UnlockBits(data);
            }
            double[] grey = new double[w * h];
            int x0 = w, y0 = h, x1 = -1, y1 = -1;
            for (int i = 0; i < px.Length; i++)
            {
                int v = px[i], a = (v >> 24) & 0xFF, r = (v >> 16) & 0xFF, gg = (v >> 8) & 0xFF, bl = v & 0xFF;
                double lum = 0.299 * r + 0.587 * gg + 0.114 * bl;
                grey[i] = (lum * a + 255.0 * (255 - a)) / 255.0;
                if (a > 32 && (r < 232 || gg < 232 || bl < 232)) { int x = i % w, y = i / w; if (x < x0) x0 = x; if (x > x1) x1 = x; if (y < y0) y0 = y; if (y > y1) y1 = y; }
            }
            if (x1 < x0) return "";
            const int CW = 8, CH = 16;
            double[] cell = new double[CW * CH];
            int bw = x1 - x0 + 1, bh = y1 - y0 + 1;
            for (int cy = 0; cy < CH; cy++)
                for (int cx = 0; cx < CW; cx++)
                {
                    int sx = x0 + cx * bw / CW, ex = x0 + (cx + 1) * bw / CW, sy = y0 + cy * bh / CH, ey = y0 + (cy + 1) * bh / CH;
                    if (ex <= sx) ex = sx + 1;
                    if (ey <= sy) ey = sy + 1;
                    double sum = 0; int n = 0;
                    for (int y = sy; y < ey && y < h; y++) for (int x = sx; x < ex && x < w; x++) { sum += grey[y * w + x]; n++; }
                    cell[cy * CW + cx] = n > 0 ? sum / n : 255;
                }
            double mean = 0;
            foreach (double c in cell) mean += c;
            mean /= cell.Length;
            StringBuilder hex = new StringBuilder();
            for (int i = 0; i < cell.Length; i += 4)
            {
                int nib = 0;
                for (int k = 0; k < 4; k++) if (cell[i + k] < mean) nib |= 8 >> k;
                hex.Append("0123456789abcdef"[nib]);
            }
            return hex.ToString();
        }

        static int Distance(string a, string b)
        {
            if (a.Length != b.Length) return 999;
            int d = 0;
            for (int i = 0; i < a.Length; i++)
            {
                int x = Convert.ToInt32(a[i].ToString(), 16) ^ Convert.ToInt32(b[i].ToString(), 16);
                while (x != 0) { d += x & 1; x >>= 1; }
            }
            return d;
        }

        // A picture on a plain white card: the white touching the border becomes see-through (flood fill from the edges, soft
        // edge), so the character stands on the launcher's background. A picture that already has transparency is kept as is.
        static Bitmap Cutout(Bitmap src)
        {
            Bitmap bmp = new Bitmap(src.Width, src.Height, System.Drawing.Imaging.PixelFormat.Format32bppArgb);
            using (Graphics g = Graphics.FromImage(bmp)) g.DrawImage(src, 0, 0, src.Width, src.Height);
            src.Dispose();
            int w = bmp.Width, h = bmp.Height;
            Rectangle all = new Rectangle(0, 0, w, h);
            System.Drawing.Imaging.BitmapData data = bmp.LockBits(all, System.Drawing.Imaging.ImageLockMode.ReadWrite, System.Drawing.Imaging.PixelFormat.Format32bppArgb);
            int[] px = new int[w * h];
            System.Runtime.InteropServices.Marshal.Copy(data.Scan0, px, 0, px.Length);
            bool hasAlpha = false;
            foreach (int v in px) if (((v >> 24) & 0xFF) < 250) { hasAlpha = true; break; }
            if (!hasAlpha)
            {
                Func<int, bool> white = delegate(int v) { return ((v >> 16) & 0xFF) > 232 && ((v >> 8) & 0xFF) > 232 && (v & 0xFF) > 232; };
                bool[] bg = new bool[w * h];
                Stack<int> todo = new Stack<int>();
                for (int x = 0; x < w; x++) { todo.Push(x); todo.Push((h - 1) * w + x); }
                for (int y = 0; y < h; y++) { todo.Push(y * w); todo.Push(y * w + w - 1); }
                while (todo.Count > 0)
                {
                    int i = todo.Pop();
                    if (bg[i] || !white(px[i])) continue;
                    bg[i] = true;
                    int x = i % w, y = i / w;
                    if (x > 0) todo.Push(i - 1);
                    if (x < w - 1) todo.Push(i + 1);
                    if (y > 0) todo.Push(i - w);
                    if (y < h - 1) todo.Push(i + w);
                }
                for (int i = 0; i < px.Length; i++)
                {
                    if (bg[i]) { px[i] = 0; continue; }
                    // soften the outline: pixels next to the removed white get partly see-through
                    int x = i % w, y = i / w, near = 0;
                    if (x > 0 && bg[i - 1]) near++;
                    if (x < w - 1 && bg[i + 1]) near++;
                    if (y > 0 && bg[i - w]) near++;
                    if (y < h - 1 && bg[i + w]) near++;
                    if (near > 0) px[i] = (px[i] & 0x00FFFFFF) | ((255 - near * 40) << 24);
                }
                System.Runtime.InteropServices.Marshal.Copy(px, 0, data.Scan0, px.Length);
            }
            bmp.UnlockBits(data);
            return bmp;
        }

    }

    // The panel: one character at a time, a slow drift on the picture, a cross-fade every few seconds, a hazard-tape name
    // plate, dots for where in the cast it is. Mouse over: holds. Click: next.
    public class CastPanel : Control
    {
        readonly List<CastMember> cast;
        readonly Timer tick = new Timer();
        int index, previous = -1;
        float fade = 1f, drift;
        int shown;            // ticks the current character has been shown
        bool hover;
        const int Hold = 7000 / 40, FadeTicks = 18;

        static Color Yellow { get { return Theme.Accent; } }
        static Color Black { get { return Theme.Back; } }
        static Color Card { get { return Theme.Card; } }
        static Color Text1 { get { return Theme.Text; } }
        static Color Dim { get { return Theme.Dim; } }
        static Color Red { get { return Theme.Alarm; } }

        public CastPanel(List<CastMember> members)
        {
            cast = members;
            DoubleBuffered = true;
            SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.UserPaint | ControlStyles.ResizeRedraw, true);
            Cursor = Cursors.Hand;
            index = cast.Count > 0 ? new Random().Next(cast.Count) : 0;
            tick.Interval = 40;
            tick.Tick += delegate { Step(); };
            tick.Start();
            MouseEnter += delegate { hover = true; };
            MouseLeave += delegate { hover = false; };
            MouseClick += delegate(object o, MouseEventArgs e) { Next(e.Button == MouseButtons.Right ? -1 : 1); };
        }

        protected override void Dispose(bool disposing) { if (disposing) tick.Dispose(); base.Dispose(disposing); }

        void Next(int dir)
        {
            if (cast.Count < 2) return;
            previous = index;
            index = (index + dir + cast.Count) % cast.Count;
            fade = 0f;
            shown = 0;
            drift = 0f;
            Invalidate();
        }

        void Step()
        {
            if (!Visible || cast.Count == 0) return;
            if (fade < 1f) fade = Math.Min(1f, fade + 1f / FadeTicks);
            drift += 1f;
            if (!hover && ++shown > Hold) Next(1);
            Invalidate();
        }

        protected override void OnPaint(PaintEventArgs e)
        {
            Graphics g = e.Graphics;
            g.SmoothingMode = SmoothingMode.AntiAlias;
            g.InterpolationMode = InterpolationMode.HighQualityBicubic;
            g.TextRenderingHint = System.Drawing.Text.TextRenderingHint.ClearTypeGridFit;
            using (SolidBrush b = new SolidBrush(Black)) g.FillRectangle(b, ClientRectangle);
            if (cast.Count == 0) return;
            Rectangle pic = new Rectangle(0, 0, Width, Height - 196);
            using (LinearGradientBrush back = new LinearGradientBrush(pic, Theme.PicTop, Black, LinearGradientMode.Vertical)) g.FillRectangle(back, pic);
            // diagonal light stripes behind the picture
            using (SolidBrush s = new SolidBrush(Theme.Glow))
                for (int x = -pic.Height; x < pic.Width; x += 46)
                    g.FillPolygon(s, new Point[] { new Point(x, pic.Bottom), new Point(x + 18, pic.Bottom), new Point(x + 18 + pic.Height, 0), new Point(x + pic.Height, 0) });
            if (previous >= 0 && fade < 1f) DrawPicture(g, cast[previous], pic, 1f - fade, drift + Hold);
            DrawPicture(g, cast[index], pic, fade, drift);
            Plate(g, cast[index], new Rectangle(0, Height - 196, Width, 196));
            using (Pen p = new Pen(Theme.Line, 1)) g.DrawRectangle(p, 0, 0, Width - 1, Height - 1);
        }

        static void DrawPicture(Graphics g, CastMember m, Rectangle area, float alpha, float t)
        {
            if (alpha <= 0f) return;
            if (m.Picture == null) { Placeholder(g, area, alpha); return; }
            Image img = m.Picture;
            float scale = Math.Min((float)(area.Width - 16) / img.Width, (float)(area.Height - 12) / img.Height);
            scale *= 1f + 0.035f * (float)Math.Min(1.0, t / Hold);          // slow push-in while it is shown
            float w = img.Width * scale, h = img.Height * scale;
            RectangleF dst = new RectangleF(area.X + (area.Width - w) / 2f, area.Bottom - h, w, h);
            Region old = g.Clip;
            g.SetClip(area);
            using (System.Drawing.Imaging.ImageAttributes ia = new System.Drawing.Imaging.ImageAttributes())
            {
                System.Drawing.Imaging.ColorMatrix cm = new System.Drawing.Imaging.ColorMatrix();
                cm.Matrix33 = alpha;
                ia.SetColorMatrix(cm);
                g.DrawImage(img, Rectangle.Round(dst), 0, 0, img.Width, img.Height, GraphicsUnit.Pixel, ia);
            }
            // fade the picture's bottom edge into the plate
            Rectangle edge = new Rectangle(area.X, area.Bottom - 40, area.Width, 41);
            using (LinearGradientBrush f = new LinearGradientBrush(edge, Color.FromArgb(0, Black), Color.FromArgb((int)(220 * alpha), Black), LinearGradientMode.Vertical))
                g.FillRectangle(f, edge);
            g.Clip = old;
        }

        // No picture for this one: a plain warning sign with a question mark.
        static void Placeholder(Graphics g, Rectangle area, float alpha)
        {
            int cx = area.X + area.Width / 2, cy = area.Y + area.Height / 2, r = Math.Min(area.Width, area.Height) / 4;
            int a = (int)(255 * alpha);
            Point[] d = { new Point(cx, cy - r), new Point(cx + r, cy), new Point(cx, cy + r), new Point(cx - r, cy) };
            using (SolidBrush y = new SolidBrush(Color.FromArgb(a, 255, 204, 0))) g.FillPolygon(y, d);
            using (Pen p = new Pen(Color.FromArgb(a, 0, 0, 0), 4))
                g.DrawPolygon(p, new Point[] { new Point(cx, cy - r + 8), new Point(cx + r - 8, cy), new Point(cx, cy + r - 8), new Point(cx - r + 8, cy) });
            using (Font f = new Font("Impact", r * 0.75f, GraphicsUnit.Pixel))
            using (SolidBrush b = new SolidBrush(Color.FromArgb(a, 0, 0, 0)))
            {
                StringFormat sf = new StringFormat(); sf.Alignment = StringAlignment.Center; sf.LineAlignment = StringAlignment.Center;
                g.DrawString("?", f, b, new RectangleF(cx - r, cy - r + 4, 2 * r, 2 * r), sf);
            }
        }

        void Plate(Graphics g, CastMember m, Rectangle r)
        {
            using (SolidBrush b = new SolidBrush(Card)) g.FillRectangle(b, r);
            Hazard.Stripes(g, new Rectangle(r.X, r.Y, r.Width, 8), Theme.Tape1, Theme.Tape2, 10);
            int x = r.X + 12, w = r.Width - 24, y = r.Y + 14;
            string ui = Lang.UiFont;
            // long names (Stephanie McMurrough) get a smaller size instead of being cut off
            for (float size = Lang.JP ? 15f : 17f; ; size -= 1f)
                using (Font big = new Font(Lang.JP ? ui : "Impact", size, Lang.JP ? FontStyle.Bold : FontStyle.Regular))
                {
                    if (size > 11f && TextRenderer.MeasureText(g, m.Name, big, new Size(9999, 30), TextFormatFlags.NoPadding).Width > w) continue;
                    TextRenderer.DrawText(g, m.Name, big, new Rectangle(x, y, w, 30), Yellow, TextFormatFlags.Left | TextFormatFlags.VerticalCenter | TextFormatFlags.EndEllipsis | TextFormatFlags.NoPadding);
                    break;
                }
            y += 30;
            using (Font small = new Font(ui, 8.5f))
                TextRenderer.DrawText(g, m.Other, small, new Rectangle(x, y, w, 16), Dim, TextFormatFlags.Left | TextFormatFlags.EndEllipsis | TextFormatFlags.NoPadding);
            y += 18;
            using (SolidBrush red = new SolidBrush(Red)) g.FillRectangle(red, x, y + 2, 4, 14);
            int roleH;
            using (Font role = new Font(ui, 8.5f, FontStyle.Bold))
            {
                roleH = Math.Min(34, TextRenderer.MeasureText(g, m.Role, role, new Size(w - 9, 40), TextFormatFlags.WordBreak | TextFormatFlags.NoPadding).Height);
                TextRenderer.DrawText(g, m.Role, role, new Rectangle(x + 9, y, w - 9, roleH), Text1, TextFormatFlags.Left | TextFormatFlags.WordBreak | TextFormatFlags.NoPadding);
            }
            y += roleH + 6;
            using (Font bio = new Font(ui, 8.5f))
                TextRenderer.DrawText(g, m.Bio, bio, new Rectangle(x, y, w, r.Bottom - y - 22), Text1, TextFormatFlags.Left | TextFormatFlags.WordBreak | TextFormatFlags.EndEllipsis | TextFormatFlags.NoPadding);
            // where in the cast: one dot each
            int n = cast.Count, dot = 7, gap = 5, total = n * dot + (n - 1) * gap, dx = r.X + (r.Width - total) / 2, dy = r.Bottom - 14;
            for (int i = 0; i < n; i++)
                using (SolidBrush b = new SolidBrush(i == index ? Yellow : Theme.Dot))
                    g.FillEllipse(b, dx + i * (dot + gap), dy, dot, dot);
        }
    }
}
