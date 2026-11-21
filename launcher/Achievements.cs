// Raw Danger PC - offline achievements, the launcher's side: which set a game uses, their pictures (RetroAchievements' badges,
// in colour once unlocked and grey until then), and a window that lists them with the player's progress.
// The game checks them itself (engine host/achievements.cpp; F4 or F1 -> Achievements in the game).
using System;
using System.Collections.Generic;
using System.Drawing;
using System.IO;
using System.Text;
using System.Text.RegularExpressions;
using System.Windows.Forms;

namespace RawDangerLauncher
{
    public static class AchievementSet
    {
        public const string RaGameId = "5616";   // Raw Danger! on RetroAchievements (retroachievements.org/game/5616)

        // The set for a game: a RetroAchievements set file (achievements\ra\*.json: downloaded here with "Get the set", or the
        // RACache\Data\5616.json of an emulator) when there is one, else ours (achievements\RawDanger.txt). The US build and the
        // Enhanced Edition only: the JP program's addresses are not checked yet.
        // RetroAchievements' set 5616 is made for the European and Japanese programs (it reads the byte at 0x348FAA, 0 or 1, to
        // pick the layout). The Japanese build uses it as it is; the US program and the Enhanced Edition get 5616.us.json, the
        // same set moved to the US program's addresses (UsPort below, achievements\ra_us_addresses.txt; tools\rd_ra_port.py).
        public static string For(string game)
        {
            bool us = game.StartsWith("RawDanger-US", StringComparison.OrdinalIgnoreCase) || game == Enhanced.Name;
            bool jp = game.StartsWith("ZZT2-JP", StringComparison.OrdinalIgnoreCase);
            if (!us && !jp) return "";
            string dir = Path.Combine(Settings.ExeDir, "achievements");
            try
            {
                string ra = Path.Combine(dir, "ra");
                string set = Path.Combine(ra, RaGameId + ".json");
                if (!File.Exists(set) && Directory.Exists(ra))
                    foreach (string f in Directory.GetFiles(ra, "*.json")) if (!f.EndsWith(".us.json", StringComparison.OrdinalIgnoreCase)) { set = f; break; }
                if (File.Exists(set))
                {
                    Clean(set);
                    if (jp) return set;
                    string port = set.Substring(0, set.Length - 5) + ".us.json";
                    if (!File.Exists(port) || File.GetLastWriteTimeUtc(port) < File.GetLastWriteTimeUtc(set)) UsPort(set, port);
                    if (File.Exists(port)) return port;
                }
            }
            catch (Exception e) { Program.Trace("achievements: " + e.Message); }
            if (jp) return "";
            // the badge export made by our userscript (tools\userscripts, v3): it carries the logic as RA_Set_<game>.json, no sign-in
            string exported = ExportedSet();
            if (exported != "") return exported;
            string ours = Path.Combine(dir, "RawDanger.txt");
            return File.Exists(ours) ? ours : "";
        }

        // RA_Set_*.json with logic in the art folder's badge export (RetroAchievements_Badge_Logic_Exporter_v3.user.js)
        static string ExportedSet()
        {
            try
            {
                foreach (string art in new string[] { Path.Combine(Path.Combine(Settings.ExeDir, "launcher"), "art"), Path.Combine(Settings.ExeDir, "art") })
                {
                    if (!Directory.Exists(art)) continue;
                    List<string> places = new List<string>(Directory.GetDirectories(art));
                    places.Insert(0, art);
                    foreach (string d in places)
                        foreach (string f in Directory.GetFiles(d, "RA_Set_*.json"))
                            if (File.ReadAllText(f).Contains("\"MemAddr\"")) return f;
                }
            }
            catch (Exception) { }
            return "";
        }

        public static bool IsRa(string set) { return set.EndsWith(".json", StringComparison.OrdinalIgnoreCase); }

        public static string Unlocks(Settings s, string game) { return Path.Combine(Games.Folder(s, game), "achievements.unlocked"); }

        // The ids the player switched off (the game's F1 -> Achievements page writes the same file)
        public static string OffFile(Settings s, string game) { return Unlocks(s, game) + ".off"; }

        public class Item
        {
            public string Id = "", Title = "", Description = "", Badge = "", Icon = "", Author = "";
            public int Points;
            public string When = "";
            public bool Unofficial, Off;
            public bool NoLogic;      // from the badge export only: listed, but the game cannot check it (no set file yet)
        }

        public static void WriteOff(Settings s, string game, List<Item> items)
        {
            List<string> lines = new List<string>();
            lines.Add("# achievements switched off (ids), rawdanger-recomp: F1 -> Achievements or RawDangerPC.exe");
            foreach (Item it in items) if (it.Off) lines.Add(it.Id);
            try { File.WriteAllLines(OffFile(s, game), lines.ToArray()); } catch (Exception) { }
        }

        // ---- the badge export in the art folder ------------------------------------------------------------------------------
        // A folder (e.g. launcher\art\Raw Danger! - Achievement Icons) with Achievements.txt from the "RetroAchievements Badge
        // Exporter": per achievement its TITLE, AUTHOR, DESCRIPTION, POINTS, ACHIEVEMENT ID and the paths of its COLOR IMAGE and
        // GRAYSCALE IMAGE (the locked look), inside that folder.
        public class Exported { public string Id = "", Title = "", Author = "", Description = "", Color = "", Grey = ""; public int Points; public bool Unofficial; }

        static List<Exported> export;
        static string exportDir = "";

        public static List<Exported> Export(Settings s)
        {
            if (export != null) return export;
            export = new List<Exported>();
            try
            {
                foreach (string art in CastData.Candidates(s))
                {
                    string txt = "";
                    if (File.Exists(Path.Combine(art, "Achievements.txt"))) txt = Path.Combine(art, "Achievements.txt");
                    else
                        foreach (string sub in Directory.GetDirectories(art))
                            if (File.Exists(Path.Combine(sub, "Achievements.txt"))) { txt = Path.Combine(sub, "Achievements.txt"); break; }
                    if (txt == "") continue;
                    exportDir = Path.GetDirectoryName(txt);
                    Exported cur = null;
                    bool unofficial = false;
                    foreach (string raw in File.ReadAllLines(txt, Encoding.UTF8))
                    {
                        string line = raw.Trim();
                        if (line.StartsWith("=====")) { unofficial = line.IndexOf("UNOFFICIAL", StringComparison.OrdinalIgnoreCase) >= 0; continue; }
                        int colon = line.IndexOf(": ");
                        if (colon <= 0) continue;
                        string k = line.Substring(0, colon), v = line.Substring(colon + 2).Trim();
                        if (k == "TITLE") { cur = new Exported(); cur.Title = v; cur.Unofficial = unofficial; export.Add(cur); }
                        else if (cur == null) continue;
                        else if (k == "AUTHOR") cur.Author = v.StartsWith("Not provided") ? "" : v;
                        else if (k == "DESCRIPTION") cur.Description = v;
                        else if (k == "COLOR IMAGE") cur.Color = Path.Combine(exportDir, v.Replace('/', Path.DirectorySeparatorChar));
                        else if (k == "GRAYSCALE IMAGE") cur.Grey = Path.Combine(exportDir, v.Replace('/', Path.DirectorySeparatorChar));
                        else if (k == "ACHIEVEMENT ID") cur.Id = v;
                        else if (k == "POINTS") int.TryParse(v, out cur.Points);
                    }
                    Program.Trace("achievement badges: " + export.Count + " from " + txt);
                    break;
                }
            }
            catch (Exception e) { Program.Trace("achievement badges: " + e.Message); }
            return export;
        }

        public static string ExportFolder(Settings s) { Export(s); return exportDir; }

        static Exported Match(Settings s, Item it)
        {
            foreach (Exported e in Export(s)) if (e.Id != "" && e.Id == it.Id) return e;
            foreach (Exported e in Export(s)) if (string.Equals(e.Title, it.Title, StringComparison.OrdinalIgnoreCase)) return e;
            return null;
        }

        // ---- the pictures the game shows ------------------------------------------------------------------------------------------
        // PS2_ACHIEVEMENTS_ICONS: <id>.bmp (unlocked, in colour) and <id>_lock.bmp (locked, grey), 64 x 64, in games\<name>\achievement_icons.
        // From the badge export when it has the achievement; else RetroAchievements' own badge (media.retroachievements.org, fetched
        // once, kept); else a picture drawn here (the warning sign with the achievement's number). Made again each time, so a new
        // export in the art folder is used at once.
        public static string Icons(Settings s, string game)
        {
            string dir = Path.Combine(Games.Folder(s, game), "achievement_icons");
            try
            {
                Directory.CreateDirectory(dir);
                List<Item> items = Read(s, game);
                int n = 0;
                foreach (Item it in items)
                {
                    n++;
                    if (it.NoLogic) continue;
                    Bitmap color = null, grey = null;
                    Exported e = Match(s, it);
                    if (e != null) { color = Load(e.Color); grey = Load(e.Grey); }
                    if (color == null && it.Icon != "")
                    {
                        string set = For(game);
                        if (set != "") color = Load(Path.Combine(Path.GetDirectoryName(set), it.Icon));
                    }
                    if (color == null && it.Badge != "") { color = Badge(it.Badge); if (grey == null) grey = Badge(it.Badge + "_lock"); }
                    if (color == null) color = Drawn(n, it.Points);
                    if (grey == null) grey = Greyed(color);
                    using (color) color.Save(Path.Combine(dir, Safe(it.Id) + ".bmp"), System.Drawing.Imaging.ImageFormat.Bmp);
                    using (grey) grey.Save(Path.Combine(dir, Safe(it.Id) + "_lock.bmp"), System.Drawing.Imaging.ImageFormat.Bmp);
                }
            }
            catch (Exception ex) { Program.Trace("achievement icons: " + ex.Message); }
            return dir;
        }

        static Bitmap Load(string file)
        {
            try { if (file != "" && File.Exists(file)) using (Image im = Image.FromFile(file)) return new Bitmap(im, 64, 64); } catch (Exception) { }
            return null;
        }

        // the locked look RetroAchievements uses: grey, a little darker
        public static Bitmap Greyed(Image src)
        {
            Bitmap b = new Bitmap(src.Width, src.Height);
            using (Graphics g = Graphics.FromImage(b))
            {
                System.Drawing.Imaging.ColorMatrix m = new System.Drawing.Imaging.ColorMatrix(new float[][] {
                    new float[] { 0.22f, 0.22f, 0.22f, 0, 0 }, new float[] { 0.44f, 0.44f, 0.44f, 0, 0 }, new float[] { 0.08f, 0.08f, 0.08f, 0, 0 },
                    new float[] { 0, 0, 0, 1, 0 }, new float[] { 0, 0, 0, 0, 1 } });
                using (System.Drawing.Imaging.ImageAttributes a = new System.Drawing.Imaging.ImageAttributes())
                {
                    a.SetColorMatrix(m);
                    g.DrawImage(src, new Rectangle(0, 0, b.Width, b.Height), 0, 0, src.Width, src.Height, GraphicsUnit.Pixel, a);
                }
            }
            return b;
        }

        static string Safe(string id) { foreach (char c in Path.GetInvalidFileNameChars()) id = id.Replace(c, '_'); return id; }

        // a RetroAchievements badge: from the cache next to the set (achievements\ra\badges), else fetched once from their site
        static bool offline;   // one badge could not be fetched: no more tries this time (no long waits without a connection)

        static Bitmap Badge(string badge)
        {
            string cache = Path.Combine(Path.Combine(Path.Combine(Settings.ExeDir, "achievements"), "ra"), "badges");
            string png = Path.Combine(cache, Safe(badge) + ".png");
            try
            {
                if (!File.Exists(png)) return null;   // offline: badges come from the export in launcher\art (or a cache left by older versions)
                using (Image im = Image.FromFile(png)) return new Bitmap(im, 64, 64);
            }
            catch (Exception e) { offline = true; Program.Trace("badge " + badge + ": " + e.Message); try { File.Delete(png); } catch (Exception) { } return null; }
        }

        // our own picture: the launcher's warning sign (yellow diamond, black edge) with the achievement's number on it
        static Bitmap Drawn(int number, int points)
        {
            Bitmap b = new Bitmap(64, 64);
            using (Graphics g = Graphics.FromImage(b))
            {
                g.SmoothingMode = System.Drawing.Drawing2D.SmoothingMode.AntiAlias;
                g.Clear(Color.FromArgb(20, 18, 12));
                using (Pen stripe = new Pen(Color.FromArgb(60, 255, 206, 0), 5))
                    for (int x = -64; x < 128; x += 14) g.DrawLine(stripe, x, 64, x + 64, 0);
                Point[] diamond = { new Point(32, 3), new Point(61, 32), new Point(32, 61), new Point(3, 32) };
                g.FillPolygon(points >= 25 ? new SolidBrush(Color.FromArgb(214, 40, 30)) : new SolidBrush(Color.FromArgb(255, 206, 0)), diamond);
                using (Pen edge = new Pen(Color.FromArgb(16, 14, 8), 3)) g.DrawPolygon(edge, diamond);
                using (Font f = new Font("Arial", 17f, FontStyle.Bold, GraphicsUnit.Pixel))
                using (StringFormat sf = new StringFormat())
                {
                    sf.Alignment = StringAlignment.Center; sf.LineAlignment = StringAlignment.Center;
                    g.DrawString(number.ToString("00"), f, new SolidBrush(points >= 25 ? Color.White : Color.FromArgb(16, 14, 8)), new RectangleF(0, 1, 64, 64), sf);
                }
            }
            return b;
        }

        // ---- the list --------------------------------------------------------------------------------------------------------
        public static List<Item> Read(Settings s, string game)
        {
            List<Item> list = new List<Item>();
            string set = For(game);
            if (set == "") return list;
            string text = File.ReadAllText(set);
            if (text.Contains("\"Achievements\""))
            {
                foreach (Match m in Regex.Matches(text, @"\{[^{}]*""MemAddr""[^{}]*\}"))
                {
                    Item it = new Item();
                    it.Id = Field(m.Value, "ID"); it.Title = Field(m.Value, "Title"); it.Description = Field(m.Value, "Description");
                    int.TryParse(Field(m.Value, "Points"), out it.Points);
                    it.Unofficial = Field(m.Value, "Flags") == "5";
                    it.Badge = Field(m.Value, "BadgeName");
                    it.Author = Field(m.Value, "Author");
                    if (!it.Unofficial || s.Unofficial == "1") list.Add(it);
                }
            }
            else
            {
                Item cur = null;
                foreach (string raw in text.Split('\n'))
                {
                    string t = raw.Trim();
                    if (t.StartsWith("#") || t == "") continue;
                    if (t.StartsWith("[") && t.EndsWith("]")) { cur = new Item(); cur.Id = t.Substring(1, t.Length - 2); cur.Author = "rawdanger-recomp"; list.Add(cur); continue; }
                    int eq = t.IndexOf('=');
                    if (cur == null || eq < 0) continue;
                    string k = t.Substring(0, eq).Trim(), v = t.Substring(eq + 1).Trim();
                    if (k == "title") cur.Title = v; else if (k == "description") cur.Description = v; else if (k == "points") int.TryParse(v, out cur.Points);
                    else if (k == "unofficial") cur.Unofficial = v == "1";
                    else if (k == "icon") cur.Icon = v;
                }
                if (s.Unofficial != "1") list.RemoveAll(delegate(Item x) { return x.Unofficial; });
            }
            foreach (Item it in list)
            {
                Exported e = Match(s, it);
                if (e != null && it.Author == "") it.Author = e.Author;
            }
            string off = OffFile(s, game);
            if (File.Exists(off))
                foreach (string line in File.ReadAllLines(off))
                    foreach (Item it in list) if (it.Id == line.Trim()) it.Off = true;
            string unlocks = Unlocks(s, game);
            if (File.Exists(unlocks))
                foreach (string line in File.ReadAllLines(unlocks))
                {
                    string[] p = line.Split('\t');
                    foreach (Item it in list) if (it.Id == p[0] && p.Length > 1) it.When = p[1];
                }
            return list;
        }

        // the RetroAchievements set as the badge export lists it, when there is no set file with its logic yet: shown (with its
        // pictures) in the launcher, never checked in the game
        public static List<Item> ExportOnly(Settings s, string game)
        {
            List<Item> list = new List<Item>();
            if (For(game) == "" || IsRa(For(game))) return list;
            foreach (Exported e in Export(s))
            {
                if (e.Unofficial && s.Unofficial != "1") continue;
                Item it = new Item();
                it.Id = e.Id; it.Title = e.Title; it.Description = e.Description; it.Points = e.Points; it.Author = e.Author; it.Unofficial = e.Unofficial;
                it.NoLogic = true;
                list.Add(it);
            }
            return list;
        }

        static string Field(string obj, string key)
        {
            Match m = Regex.Match(obj, "\"" + key + "\"\\s*:\\s*(\"((?:[^\"\\\\]|\\\\.)*)\"|([^,}\\s]+))");
            return !m.Success ? "" : m.Groups[2].Success && m.Groups[1].Value.StartsWith("\"") ? Regex.Unescape(m.Groups[2].Value) : m.Groups[3].Value;
        }

        // ---- RAIntegration's copy -------------------------------------------------------------------------------------------
        // RAIntegration (the RetroAchievements developer toolkit; PCSX2 loads it when RA_Integration-x64.dll is in its folder and
        // "Enable RAIntegration" is on) keeps the set it downloaded as RACache\Data\<game>.json next to the emulator, logic
        // included, and the code notes as <game>-Notes.json. Looked for in the usual PCSX2 places and the folders of the
        // launcher's settings; the player can also point at the file.
        public static string FindEmulatorCopy()
        {
            List<string> roots = new List<string>();
            try
            {
                string docs = Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments);
                roots.Add(Path.Combine(docs, "PCSX2"));
                foreach (Environment.SpecialFolder f in new Environment.SpecialFolder[] { Environment.SpecialFolder.ProgramFiles, Environment.SpecialFolder.ProgramFilesX86, Environment.SpecialFolder.LocalApplicationData, Environment.SpecialFolder.ApplicationData })
                {
                    string b = Environment.GetFolderPath(f);
                    if (b != "") { roots.Add(Path.Combine(b, "PCSX2")); roots.Add(Path.Combine(b, "PCSX2-Qt")); }
                }
                string desktop = Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory);
                if (desktop != "") foreach (string d in Directory.GetDirectories(desktop, "*pcsx2*")) roots.Add(d);
                roots.Add(Path.GetDirectoryName(Settings.ExeDir));
                if (Directory.Exists(Path.GetDirectoryName(Settings.ExeDir)))
                    foreach (string d in Directory.GetDirectories(Path.GetDirectoryName(Settings.ExeDir), "*pcsx2*")) roots.Add(d);
            }
            catch (Exception) { }
            foreach (string r in roots)
            {
                try
                {
                    string f = Path.Combine(Path.Combine(Path.Combine(r, "RACache"), "Data"), RaGameId + ".json");
                    if (File.Exists(f) && File.ReadAllText(f).Contains("\"MemAddr\"")) return f;
                }
                catch (Exception) { }
            }
            return "";
        }

        // RetroAchievements answers a client it does not know with an extra achievement, "Warning: Unknown Emulator" (ID 101000001,
        // logic 1=1: it would unlock at once) and a "Warning" note. Both are taken out of the kept set, once.
        public static void Clean(string file)
        {
            try
            {
                string text = File.ReadAllText(file);
                string cleaned = WithoutPlaceholders(text);
                if (cleaned != text) { File.WriteAllText(file, cleaned, new UTF8Encoding(false)); Program.Trace("achievements: RetroAchievements' placeholder achievement taken out of " + file); }
            }
            catch (Exception e) { Program.Trace("achievements: clean " + file + ": " + e.Message); }
        }

        public static string WithoutPlaceholders(string text)
        {
            for (int guard = 0; guard < 16; guard++)
            {
                Match m = Regex.Match(text, "\"ID\"\\s*:\\s*\"?(1010\\d{5,})\"?");
                if (!m.Success) break;
                int open = text.LastIndexOf('{', m.Index), close = -1, depth = 0;
                if (open < 0) break;
                bool str = false;
                for (int i = open; i < text.Length; i++)
                {
                    char c = text[i];
                    if (str) { if (c == '\\') i++; else if (c == '"') str = false; continue; }
                    if (c == '"') str = true;
                    else if (c == '{') depth++;
                    else if (c == '}' && --depth == 0) { close = i; break; }
                }
                if (close < 0) break;
                int a = open, b = close + 1;
                int after = b; while (after < text.Length && char.IsWhiteSpace(text[after])) after++;
                if (after < text.Length && text[after] == ',') b = after + 1;
                else { int before = a - 1; while (before >= 0 && char.IsWhiteSpace(text[before])) before--; if (before >= 0 && text[before] == ',') a = before; }
                text = text.Substring(0, a) + text.Substring(b);
            }
            return Regex.Replace(text, ",\\s*\"Warning\"\\s*:\\s*\"(?:[^\"\\\\]|\\\\.)*\"", "");
        }

        // ---- the set on the US program ---------------------------------------------------------------------------------------
        // Each read of the set starts with "I:0xH348faa*N": the version byte times a negative number, the step from the European
        // layout to the Japanese one. Here every such read goes to its Japanese address, then through the table of the Japanese
        // program's data addresses and the same data's US addresses (found by matching the two programs' code) to the US one,
        // and the step is dropped. Reads after another add-address step are offsets inside what the game points to: kept.
        // A read that cannot be placed switches its achievement off ("0=1") instead of letting it fire at random.
        static readonly Regex Operand = new Regex("(?<![0-9A-Za-z])([dpb~]?)(0x[A-Z ]?|f[FB])([0-9a-fA-F]+)");
        static readonly Regex VersionStep = new Regex("^I:0xH348faa\\*(\\d+)$", RegexOptions.IgnoreCase);

        public static bool UsPort(string set, string port)
        {
            try
            {
                List<uint> jpAt = new List<uint>(), usAt = new List<uint>();
                string table = Path.Combine(Path.Combine(Settings.ExeDir, "achievements"), "ra_us_addresses.txt");
                foreach (string raw in File.ReadAllLines(table))
                {
                    string line = raw.Split('#')[0].Trim();
                    string[] two = line.Split(new char[] { ' ', '\t' }, StringSplitOptions.RemoveEmptyEntries);
                    if (two.Length != 2) continue;
                    jpAt.Add(Convert.ToUInt32(two[0], 16)); usAt.Add(Convert.ToUInt32(two[1], 16));
                }
                string text = File.ReadAllText(set);
                int off = 0;
                text = Regex.Replace(text, "\"MemAddr\"\\s*:\\s*\"((?:[^\"\\\\]|\\\\.)*)\"", delegate(Match m)
                {
                    string moved = PortTrigger(m.Groups[1].Value, jpAt, usAt);
                    if (moved == null) { off++; moved = "0=1"; }
                    return "\"MemAddr\":\"" + moved + "\"";
                });
                text = Regex.Replace(text, "\"RichPresencePatch\"\\s*:\\s*\"(?:[^\"\\\\]|\\\\.)*\"", "\"RichPresencePatch\":\"\"");
                File.WriteAllText(port, text, new UTF8Encoding(false));
                Program.Trace("achievements: set moved to the US program: " + port + (off > 0 ? " (" + off + " left off)" : ""));
                return true;
            }
            catch (Exception e) { Program.Trace("achievements: US port: " + e.Message); return false; }
        }

        static string PortTrigger(string mem, List<uint> jpAt, List<uint> usAt)
        {
            string[] groups = Regex.Split(mem, "(?<!0x)S");
            List<string> outGroups = new List<string>();
            foreach (string group in groups)
            {
                List<string> conds = new List<string>();
                long shift = 0; bool shifted = false, relative = false;
                foreach (string c in group.Split('_'))
                {
                    if (c == "") continue;
                    Match v = VersionStep.Match(c);
                    if (v.Success)
                    {
                        ulong n = ulong.Parse(v.Groups[1].Value);
                        shift = n >= 0x80000000UL ? (long)n - 0x100000000L : (long)n; shifted = true;
                        continue;
                    }
                    string moved = c;
                    if (shifted || !relative)
                    {
                        string head = Regex.IsMatch(c, "^[A-Z]:") ? c.Substring(0, 2) : "";
                        bool failed = false;
                        long s = shifted ? shift : 0;
                        string body = Operand.Replace(c.Substring(head.Length), delegate(Match m)
                        {
                            uint a = (uint)((Convert.ToUInt32(m.Groups[3].Value, 16) + s) & 0xFFFFFFFFL);
                            int k = jpAt.BinarySearch(a);
                            if (k < 0) k = ~k - 1;
                            if (k < 0 || a - jpAt[k] > 0x4000) { failed = true; return m.Value; }
                            return m.Groups[1].Value + m.Groups[2].Value + (a - jpAt[k] + usAt[k]).ToString("x");
                        });
                        if (failed) return null;
                        moved = head + body;
                    }
                    conds.Add(moved);
                    relative = c.StartsWith("I:");
                    shifted = false;
                }
                outGroups.Add(string.Join("_", conds.ToArray()));
            }
            return string.Join("S", outGroups.ToArray());
        }

        public static string Import(string file)
        {
            try
            {
                string text = File.ReadAllText(file);
                if (!text.Contains("\"MemAddr\"")) return Lang.L("This file has no achievement logic (no MemAddr): ", "このファイルには実績の条件（MemAddr）がありません: ") + file;
                if (!text.Contains("\"" + "392823" + "\"") && !text.Contains(":392823") && !text.Contains(": 392823"))
                    return Lang.L("This is not Raw Danger!'s set (RetroAchievements game " + RaGameId + "): ", "これは Raw Danger! のセット（RetroAchievements ゲーム " + RaGameId + "）ではありません: ") + file;
                string dir = Path.Combine(Path.Combine(Settings.ExeDir, "achievements"), "ra");
                Directory.CreateDirectory(dir);
                File.WriteAllText(Path.Combine(dir, RaGameId + ".json"), WithoutPlaceholders(text), new UTF8Encoding(false));
                string notes = Path.Combine(Path.GetDirectoryName(file), RaGameId + "-Notes.json");
                if (File.Exists(notes)) File.Copy(notes, Path.Combine(dir, RaGameId + "-Notes.json.txt"), true);
                Program.Trace("achievements: set taken from " + file + " (" + text.Length + " bytes)");
                return null;
            }
            catch (Exception e) { return Lang.L("Could not take the set: ", "セットを取り込めませんでした: ") + e.Message; }
        }

    }

    public class AchievementsDialog : Form
    {
        readonly Settings settings;
        readonly string game;
        List<AchievementSet.Item> items;
        readonly Label head = new Label();
        readonly ProgressPanel progress = new ProgressPanel();
        readonly ListView list = new ListView();
        readonly Label detail = new Label();
        bool filling;

        // the share of points won, as the hazard bar the launcher uses elsewhere
        class ProgressPanel : Panel
        {
            public float Share;
            public ProgressPanel() { DoubleBuffered = true; }
            protected override void OnPaint(PaintEventArgs e)
            {
                Graphics g = e.Graphics;
                g.Clear(Theme.Card);
                using (Pen stripe = new Pen(Color.FromArgb(70, 255, 206, 0), 5))
                    for (int x = -Height; x < Width + Height; x += 14) g.DrawLine(stripe, x, Height, x + Height, 0);
                int w = (int)(Width * Math.Max(0f, Math.Min(1f, Share)));
                using (SolidBrush b = new SolidBrush(Theme.Accent)) g.FillRectangle(b, 0, 0, w, Height);
            }
        }

        public AchievementsDialog(Settings s, string game, string title)
        {
            settings = s; this.game = game;
            Text = Lang.L("Achievements - ", "実績 - ") + title;
            Font = new Font(Lang.UiFont, 9.5f);
            BackColor = Theme.Back; ForeColor = Theme.Text;
            FormBorderStyle = FormBorderStyle.FixedDialog; MaximizeBox = MinimizeBox = false;
            Icon = WizardForm.AppIcon();
            StartPosition = FormStartPosition.CenterParent;
            ClientSize = new Size(820, 690);
            head.Location = new Point(14, 10); head.Size = new Size(792, 40); head.ForeColor = Theme.Dim;
            Controls.Add(head);
            progress.Location = new Point(14, 52); progress.Size = new Size(792, 10);
            Controls.Add(progress);
            list.View = View.Details; list.FullRowSelect = true; list.HeaderStyle = ColumnHeaderStyle.Nonclickable; list.CheckBoxes = true;
            list.Location = new Point(14, 70); list.Size = new Size(792, 372);
            list.BackColor = Theme.Card; list.ForeColor = Theme.Text; list.BorderStyle = BorderStyle.None;
            list.ShowGroups = true;
            list.Columns.Add(Lang.L("On", "有効"), 66); list.Columns.Add(Lang.L("Achievement", "実績"), 220); list.Columns.Add(Lang.L("How", "条件"), 270);
            list.Columns.Add(Lang.L("Points", "点"), 50); list.Columns.Add(Lang.L("Unlocked", "解除"), 116);
            list.ItemChecked += delegate(object o, ItemCheckedEventArgs e)
            {
                if (filling) return;
                AchievementSet.Item it = (AchievementSet.Item)e.Item.Tag;
                if (it.NoLogic) { filling = true; e.Item.Checked = false; filling = false; return; }
                it.Off = !e.Item.Checked;
                AchievementSet.WriteOff(settings, this.game, items);
            };
            list.SelectedIndexChanged += delegate { ShowDetail(); };
            Controls.Add(list);
            detail.Location = new Point(14, 446); detail.Size = new Size(792, 40); detail.ForeColor = Theme.Text;
            Controls.Add(detail);
            CheckBox unofficial = new CheckBox();
            unofficial.Text = Lang.L("Also RetroAchievements' unofficial (experimental) achievements", "RetroAchievements の非公式（試験中）の実績も含める");
            unofficial.Location = new Point(14, 490); unofficial.Size = new Size(480, 24); unofficial.ForeColor = Theme.Text;
            unofficial.Checked = s.Unofficial == "1";
            unofficial.CheckedChanged += delegate { settings.Unofficial = unofficial.Checked ? "1" : "0"; settings.Save(); Fill(); };
            Controls.Add(unofficial);
            Label source = new Label();
            source.Location = new Point(506, 490); source.Size = new Size(300, 30); source.ForeColor = Theme.Dim;
            source.TextAlign = ContentAlignment.MiddleLeft;
            source.Text = AchievementSet.IsRa(AchievementSet.For(game)) ? Lang.L("RetroAchievements' set: kept on this PC (offline)", "RetroAchievements のセット：このPCに保存（オフライン）") : Lang.L("Our own set (RetroAchievements' not taken yet)", "独自のセット（RetroAchievements のセットは未取得）");
            Controls.Add(source);
            Button import = new Button();
            import.Text = Lang.L("Take a set file (PCSX2 RAIntegration)...", "セットのファイルを取り込む（PCSX2 RAIntegration）...");
            import.Size = new Size(300, 30); import.Location = new Point(506, 524);
            import.FlatStyle = FlatStyle.Flat; import.BackColor = Theme.Card; import.ForeColor = Theme.Text; import.FlatAppearance.BorderColor = Theme.Line;
            import.Click += delegate { ImportSet(); };
            Controls.Add(import);
            Label note = new Label();
            note.Location = new Point(14, 522); note.Size = new Size(480, 160); note.ForeColor = Theme.Dim;
            note.Text = Lang.L("Everything works offline: the game checks the achievements on this PC from achievements\\ra\\5616.json (RetroAchievements' set, what each one needs) and nothing is ever sent. A newer set can be taken from PCSX2's RAIntegration copy (RACache\\Data\\5616.json). Untick one to switch it off; F4 in the game does too. Badges from launcher\\art: colour once unlocked, grey until then.",
                               "すべてオフラインで動作します：ゲームは achievements\\ra\\5616.json（RetroAchievements のセット：各実績の条件）でこのPC上で判定し、何も送信しません。新しいセットは PCSX2 の RAIntegration の写し（RACache\\Data\\5616.json）から取り込めます。チェックを外すと判定しません（ゲーム内 F4 でも可）。バッジは launcher\\art から：解除済みはカラー、未解除はグレー。");

            Controls.Add(note);
            Button ok = new Button();
            ok.Text = "OK"; ok.Size = new Size(110, 32); ok.Location = new Point(696, 646);
            ok.FlatStyle = FlatStyle.Flat; ok.BackColor = Theme.Accent; ok.ForeColor = Theme.OnAccent; ok.FlatAppearance.BorderSize = 0;
            ok.Click += delegate { Close(); };
            Controls.Add(ok); AcceptButton = ok;
            Fill();
        }

        void ImportSet()
        {
            string file = AchievementSet.FindEmulatorCopy();
            if (file == "")
            {
                using (OpenFileDialog d = new OpenFileDialog())
                {
                    d.Title = Lang.L("RAIntegration's copy of the set: PCSX2 folder \\ RACache \\ Data \\ 5616.json", "RAIntegration のセットの写し: PCSX2 フォルダ \\ RACache \\ Data \\ 5616.json");
                    d.Filter = Lang.L("RetroAchievements set", "RetroAchievements セット") + " (5616.json;*.json)|5616.json;*.json";
                    d.FileName = AchievementSet.RaGameId + ".json";
                    if (d.ShowDialog(this) != DialogResult.OK) return;
                    file = d.FileName;
                }
            }
            string problem = AchievementSet.Import(file);
            if (problem != null) { MessageBox.Show(problem, Program.AppName, MessageBoxButtons.OK, MessageBoxIcon.Information); return; }
            MessageBox.Show(Lang.L("Taken from ", "取り込みました: ") + file + Lang.L(": the game now checks RetroAchievements' set (start it again if it is running).", "\nゲームは RetroAchievements のセットで判定します（起動中なら再起動してください）。"), Program.AppName);
            Fill();
        }

        void ShowDetail()
        {
            if (list.SelectedItems.Count == 0) { detail.Text = ""; return; }
            AchievementSet.Item it = (AchievementSet.Item)list.SelectedItems[0].Tag;
            detail.Text = it.Title + "  -  " + it.Description + "\n"
                + it.Points + Lang.L(it.Points == 1 ? " point" : " points", " 点") + (it.Author != "" ? Lang.L("   ·   by ", "   ·   作成: ") + it.Author : "")
                + (it.When != "" ? Lang.L("   ·   unlocked ", "   ·   解除 ") + it.When : it.NoLogic ? Lang.L("   ·   not checked yet: get the set (button below)", "   ·   未判定：下のボタンでセットを取得") : "");
        }

        void Fill()
        {
            items = AchievementSet.Read(settings, game);
            List<AchievementSet.Item> extra = AchievementSet.ExportOnly(settings, game);
            int got = 0, pts = 0, all = 0;
            foreach (AchievementSet.Item it in items) { all += it.Points; if (it.When != "") { got++; pts += it.Points; } }
            string set = AchievementSet.For(game);
            bool ra = AchievementSet.IsRa(set);
            int pct = all > 0 ? pts * 100 / all : 0;
            head.Text = items.Count == 0 ? Lang.L("No achievements for this game (US, Japanese and Enhanced Edition only).", "このゲームの実績はありません（北米版・日本版・強化版のみ）。")
                : Lang.L((ra ? "RetroAchievements set. " : "rawdanger-recomp's own set. ") + got + " of " + items.Count + " unlocked   ·   " + pts + " of " + all + " points (" + pct + "%)   ·   in the game: F4, or F1 -> Achievements",
                         (ra ? "RetroAchievements のセット。" : "rawdanger-recomp 独自のセット。") + items.Count + " 個中 " + got + " 個解除   ·   " + all + " 点中 " + pts + " 点（" + pct + "%）   ·   ゲーム中は F4 または F1 → Achievements");
            progress.Share = all > 0 ? (float)pts / all : 0f;
            progress.Invalidate();
            filling = true;
            list.BeginUpdate();
            list.Items.Clear();
            list.Groups.Clear();
            ListViewGroup active = new ListViewGroup(ra ? Lang.L("RetroAchievements - checked in the game", "RetroAchievements - ゲーム内で判定") : Lang.L("rawdanger-recomp - checked in the game", "rawdanger-recomp - ゲーム内で判定"));
            ListViewGroup waiting = new ListViewGroup(Lang.L("RetroAchievements (" + extra.Count + ") - pictures and details from the art folder; not checked until you get the set", "RetroAchievements（" + extra.Count + "）- art フォルダの画像と情報。セット取得まで未判定"));
            list.Groups.Add(active);
            if (extra.Count > 0) list.Groups.Add(waiting);
            ImageList pictures = new ImageList();
            pictures.ImageSize = new Size(32, 32);
            pictures.ColorDepth = ColorDepth.Depth32Bit;
            string icons = AchievementSet.Icons(settings, game);
            foreach (AchievementSet.Item it in items)
            {
                // unlocked: the badge in colour; locked: grey (the _lock picture)
                string bmp = Path.Combine(icons, it.Id + (it.When != "" ? ".bmp" : "_lock.bmp"));
                try { using (Image im = Image.FromFile(bmp)) pictures.Images.Add(it.Id, new Bitmap(im)); } catch (Exception) { }
            }
            Dictionary<string, AchievementSet.Exported> byId = new Dictionary<string, AchievementSet.Exported>();
            foreach (AchievementSet.Exported e in AchievementSet.Export(settings)) if (e.Id != "") byId[e.Id] = e;
            foreach (AchievementSet.Item it in extra)
            {
                AchievementSet.Exported e;
                if (!byId.TryGetValue(it.Id, out e)) continue;
                try { using (Image im = Image.FromFile(e.Grey != "" && File.Exists(e.Grey) ? e.Grey : e.Color)) pictures.Images.Add("x" + it.Id, new Bitmap(im, 32, 32)); } catch (Exception) { }
            }
            list.SmallImageList = pictures;
            foreach (AchievementSet.Item it in items) Add(it, active, it.Id);
            foreach (AchievementSet.Item it in extra) Add(it, waiting, "x" + it.Id);
            list.EndUpdate();
            filling = false;
            if (list.Items.Count > 0) list.Items[0].Selected = true;
            ShowDetail();
        }

        void Add(AchievementSet.Item it, ListViewGroup group, string image)
        {
            ListViewItem row = new ListViewItem("");
            row.ImageKey = image;
            row.Tag = it;
            row.Group = group;
            row.Checked = !it.Off && !it.NoLogic;
            row.SubItems.Add((it.When != "" ? "✓ " : "") + it.Title + (it.Unofficial ? Lang.L(" (unofficial)", "（非公式）") : ""));
            row.SubItems.Add(it.Description); row.SubItems.Add(it.Points.ToString()); row.SubItems.Add(it.When);
            row.ForeColor = it.When != "" ? Theme.Good : it.Off || it.NoLogic ? Theme.Disabled : Theme.Dim;
            list.Items.Add(row);
        }
    }
}
