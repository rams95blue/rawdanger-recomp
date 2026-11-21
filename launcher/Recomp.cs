// Raw Danger PC - recomp.ini: the game's patches as options, shared with the in-game "Recomp options" page (F1 menu, F3).
// Written when a game is built; the live options (patches that only change data, marked "// recomp: live" in the .pnach)
// can be switched here or in the game, and both read and write the same on= lines. Built-in options are compiled into the
// game and change only with a new build.
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Globalization;
using System.IO;
using System.Text;
using System.Text.RegularExpressions;
using System.Windows.Forms;

namespace RawDangerLauncher
{
    public class RecompOption
    {
        public string Name = "", Label = "";
        public bool Live, On = true, Aspect;
        public List<string> Words = new List<string>();   // "ADDRESS PATCHED ORIGINAL", hex
        public string RemapOff = "";                        // "FROM TO COUNT" disc sectors read elsewhere while switched off
        public string RemapOn = "";                         // the same while switched on (the Enhanced Edition's Japanese textures)
        public string RemapOff2 = "";                       // a second area moved while switched off (the voice switch: the sound banks with voices)
    }

    public static class RecompIni
    {
        public static string PathOf(Settings s, string game) { return Path.Combine(Games.Folder(s, game), "recomp.ini"); }

        public static List<RecompOption> Read(string file)
        {
            List<RecompOption> list = new List<RecompOption>();
            if (!File.Exists(file)) return list;
            RecompOption cur = null;
            foreach (string raw in File.ReadAllLines(file))
            {
                string t = raw.Trim();
                if (t == "" || t.StartsWith("#")) continue;
                if (t.StartsWith("[") && t.EndsWith("]")) { cur = new RecompOption(); cur.Name = cur.Label = t.Substring(1, t.Length - 2); list.Add(cur); continue; }
                int eq = t.IndexOf('=');
                if (cur == null || eq < 0) continue;
                string k = t.Substring(0, eq).Trim(), v = t.Substring(eq + 1).Trim();
                if (k == "label") cur.Label = v;
                else if (k == "live") cur.Live = v == "1";
                else if (k == "on") cur.On = v != "0";
                else if (k == "aspect") cur.Aspect = v == "16:9";
                else if (k == "word") cur.Words.Add(v);
                else if (k == "remap_off") cur.RemapOff = v;
                else if (k == "remap_on") cur.RemapOn = v;
                else if (k == "remap2_off") cur.RemapOff2 = v;
            }
            return list;
        }

        public static void Write(string file, List<RecompOption> options)
        {
            StringBuilder sb = new StringBuilder();
            sb.AppendLine("# Game options - rawdanger-recomp. Shared by RawDangerPC.exe and the game's F1 menu (Game options, F3).");
            sb.AppendLine("# live = 1: switched in the game or the launcher (on = 1 / 0). live = 0: built in, changed by building again.");
            foreach (RecompOption o in options)
            {
                sb.AppendLine();
                sb.AppendLine("[" + o.Name + "]");
                sb.AppendLine("label = " + o.Label);
                sb.AppendLine("live = " + (o.Live ? "1" : "0"));
                sb.AppendLine("on = " + (o.On ? "1" : "0"));
                if (o.Aspect) sb.AppendLine("aspect = 16:9");
                foreach (string w in o.Words) sb.AppendLine("word = " + w);
                if (o.RemapOff != "") sb.AppendLine("remap_off = " + o.RemapOff);
                if (o.RemapOn != "") sb.AppendLine("remap_on = " + o.RemapOn);
                if (o.RemapOff2 != "") sb.AppendLine("remap2_off = " + o.RemapOff2);
            }
            File.WriteAllText(file, sb.ToString(), new UTF8Encoding(false));
        }

        // After a build: one option per built section, with the original values read from the game's own (unpatched) program.
        // Choices already in the file are kept.
        public static void Make(Settings s, string game, string pnach, List<string> sections, string originalElf)
        {
            string file = PathOf(s, game);
            Dictionary<string, bool> before = new Dictionary<string, bool>();
            foreach (RecompOption o in Read(file)) before[o.Name] = o.On;
            byte[] elf = null;
            try { if (File.Exists(originalElf)) elf = File.ReadAllBytes(originalElf); } catch (Exception) { }
            Dictionary<string, List<string>> body = Sections(pnach);
            List<RecompOption> list = new List<RecompOption>();
            foreach (string sec in sections)
            {
                RecompOption o = new RecompOption();
                o.Name = sec;
                o.Label = English(sec);
                List<string> lines;
                bool live = false, defaultOn = true;
                Match sw = null;
                if (body.TryGetValue(sec, out lines))
                    foreach (string l in lines)
                    {
                        Match m = Regex.Match(l, @"^//\s*recomp:\s*live\s+switch\s+([0-9A-Fa-f]{1,8})(\s+default\s+off)?(\s+inverted)?", RegexOptions.IgnoreCase);
                        if (m.Success) { sw = m; break; }
                    }
                if (sw != null)
                {
                    // code the build made switchable (pnach.py toggles): the game reads a variable, 0 = the patch, 1 = as on the disc
                    o.Live = true;
                    // "inverted": the option on is the game as on the disc (the variable 1), off the patch (0)
                    o.Words.Add(uint.Parse(sw.Groups[1].Value, NumberStyles.HexNumber).ToString("X8") + (sw.Groups[3].Success ? " 00000001 00000000" : " 00000000 00000001"));
                    defaultOn = !sw.Groups[2].Success;
                    bool was;
                    o.On = before.TryGetValue(sec, out was) ? was : defaultOn;
                    list.Add(o);
                    continue;
                }
                if (body.TryGetValue(sec, out lines))
                {
                    live = lines.Exists(delegate(string l) { return Regex.IsMatch(l, @"^//\s*recomp:\s*live", RegexOptions.IgnoreCase); });
                    foreach (string l in lines)
                    {
                        if (Regex.IsMatch(l, @"^gsaspectratio\s*=\s*16:9", RegexOptions.IgnoreCase)) o.Aspect = true;
                        Match m = Regex.Match(l, @"^patch\s*=\s*1\s*,\s*EE\s*,\s*([0-9A-Fa-f]{8})\s*,\s*word\s*,\s*([0-9A-Fa-f]{1,8})", RegexOptions.IgnoreCase);
                        if (!m.Success) continue;
                        uint addr = uint.Parse(m.Groups[1].Value, NumberStyles.HexNumber), val = uint.Parse(m.Groups[2].Value, NumberStyles.HexNumber);
                        uint orig;
                        if (elf == null || !ElfWord(elf, addr, out orig)) { live = false; continue; }   // no original value: cannot switch it off
                        o.Words.Add(addr.ToString("X8") + " " + val.ToString("X8") + " " + orig.ToString("X8"));
                    }
                }
                o.Live = live && o.Words.Count > 0;
                bool on;
                o.On = !o.Live || !before.TryGetValue(sec, out on) || on;
                list.Add(o);
            }
            RecompOption voices = Voices(s, game);
            if (voices != null)
            {
                bool on;
                if (before.TryGetValue(voices.Name, out on)) voices.On = on;
                list.Add(voices);
            }
            foreach (RecompOption t in Textures(s, game))
            {
                bool on;
                if (before.TryGetValue(t.Name, out on)) t.On = on;
                list.Add(t);
            }
            Write(file, list);
        }

        public const string VoicesSection = "Voices: Japanese";

        // The Enhanced Edition's disc carries the English and the Japanese recordings in two areas with the same layout
        // (tools\rd_hybrid.py writes hybrid.iso.layout). The game reads the Japanese area; switched off, the engine sends
        // those reads to the English area instead, so the voices change in the game at the next line, without a new build.
        static RecompOption Voices(Settings s, string game)
        {
            string layout = Path.Combine(Games.Folder(s, game), "hybrid.iso.layout");
            if (!File.Exists(layout)) return null;
            long jp = -1, en = -1, n = -1;
            string banks = "";   // the sound banks with voices (rd_hybrid.py v5): "JAPANESE ENGLISH SECTORS COUNT"
            try
            {
                foreach (string raw in File.ReadAllLines(layout))
                {
                    string[] kv = raw.Split('=');
                    if (kv.Length != 2) continue;
                    if (kv[0].Trim() == "sounds_jp_slots")
                    {
                        string[] p = kv[1].Trim().Split(new char[] { ' ' }, StringSplitOptions.RemoveEmptyEntries);
                        if (p.Length >= 3) banks = p[0] + " " + p[1] + " " + p[2];
                        continue;
                    }
                    long v;
                    if (!long.TryParse(kv[1].Trim(), out v)) continue;
                    string k = kv[0].Trim();
                    if (k == "voices_jp_start") jp = v; else if (k == "voices_en_start") en = v; else if (k == "voices_sectors") n = v;
                }
            }
            catch (Exception) { return null; }
            if (jp < 0 || en < 0 || n <= 0) return null;
            RecompOption o = new RecompOption();
            o.Name = VoicesSection;
            o.Label = "Japanese voices (off: English)";
            o.Live = true;
            o.On = true;
            o.RemapOff = jp + " " + en + " " + n;
            o.RemapOff2 = banks;   // English voices: the English shouts too
            return o;
        }

        public const string FacesSection = "Textures: Japanese faces", SignsSection = "Textures: Japanese signs";

        // The textures the US release redrew are on the Enhanced Edition's disc twice (tools\rd_hybrid.py, the list in
        // tools\jp_textures.txt): the US copy where ROOT.FAT points, the Japanese copy in the same layout further on. Switched
        // on, the engine reads the Japanese copies instead; a texture already on screen changes the next time the game loads it
        // (another area, a cutscene, the menu). Two groups: the characters (faces, hair, skin, clothes colours) and the writing.
        static List<RecompOption> Textures(Settings s, string game)
        {
            List<RecompOption> list = new List<RecompOption>();
            string layout = Path.Combine(Games.Folder(s, game), "hybrid.iso.layout");
            if (!File.Exists(layout)) return list;
            try
            {
                foreach (string raw in File.ReadAllLines(layout))
                {
                    string[] kv = raw.Split('=');
                    if (kv.Length != 2) continue;
                    string k = kv[0].Trim();
                    string[] v = kv[1].Trim().Split(new char[] { ' ' }, StringSplitOptions.RemoveEmptyEntries);
                    if (!k.StartsWith("textures_") || v.Length < 3) continue;
                    RecompOption o = new RecompOption();
                    o.Live = true;
                    o.On = false;
                    o.RemapOn = v[0] + " " + v[1] + " " + v[2];
                    if (k == "textures_faces") { o.Name = FacesSection; o.Label = "Japanese faces and hair (original look)"; }
                    else if (k == "textures_signs") { o.Name = SignsSection; o.Label = "Japanese signs and packets"; }
                    else if (k == "textures_logos") { o.Name = "Media: Japanese logo"; o.Label = "Japanese logo (title and start)"; }
                    else if (k == "textures_movies") { o.Name = "Media: Japanese opening film"; o.Label = "Japanese opening film"; }
                    else continue;
                    list.Add(o);
                }
            }
            catch (Exception) { }
            return list;
        }

        public static void SetOn(string file, string name, bool on)
        {
            List<RecompOption> list = Read(file);
            foreach (RecompOption o in list) if (o.Name == name && o.Live) o.On = on;
            Write(file, list);
        }

        // The in-game font is plain ASCII: English labels.
        static string English(string section)
        {
            bool jp = Lang.JP;
            Lang.JP = false;
            string text = PatchNames.One(section);
            Lang.JP = jp;
            return text.Length > 0 ? char.ToUpperInvariant(text[0]) + text.Substring(1) : section;
        }

        static Dictionary<string, List<string>> Sections(string pnach)
        {
            Dictionary<string, List<string>> d = new Dictionary<string, List<string>>();
            if (pnach == "" || !File.Exists(pnach)) return d;
            List<string> cur = null;
            foreach (string raw in File.ReadAllLines(pnach))
            {
                string t = raw.Trim();
                if (t.StartsWith("[") && t.EndsWith("]")) { cur = new List<string>(); d[t.Substring(1, t.Length - 2)] = cur; continue; }
                if (cur != null && t != "") cur.Add(t);
            }
            return d;
        }

        // A word of the program as loaded (PT_LOAD segments; zero inside the part that is only reserved).
        public static bool ElfWord(byte[] elf, uint addr, out uint value)
        {
            value = 0;
            if (elf.Length < 0x34 || elf[0] != 0x7F || elf[1] != (byte)'E') return false;
            uint phoff = BitConverter.ToUInt32(elf, 0x1C);
            int phnum = BitConverter.ToUInt16(elf, 0x2C), phent = BitConverter.ToUInt16(elf, 0x2A);
            for (int i = 0; i < phnum; i++)
            {
                long p = phoff + (long)i * phent;
                if (p + 32 > elf.Length) break;
                uint type = BitConverter.ToUInt32(elf, (int)p), off = BitConverter.ToUInt32(elf, (int)p + 4), va = BitConverter.ToUInt32(elf, (int)p + 8);
                uint filesz = BitConverter.ToUInt32(elf, (int)p + 16), memsz = BitConverter.ToUInt32(elf, (int)p + 20);
                if (type != 1 || addr < va || addr + 4 > va + memsz) continue;
                if (addr + 4 <= va + filesz && off + (addr - va) + 4 <= elf.Length) value = BitConverter.ToUInt32(elf, (int)(off + (addr - va)));
                return true;
            }
            return false;
        }
    }

    // The launcher's side of the same options: a small window from "Your games".
    public class RecompDialog : Form
    {
        public RecompDialog(Settings s, string game, string title)
        {
            string file = RecompIni.PathOf(s, game);
            List<RecompOption> options = RecompIni.Read(file);
            Text = Lang.L("Game options - ", "ゲームオプション - ") + title;
            Font = new Font(Lang.UiFont, 10f);
            BackColor = Theme.Back;
            ForeColor = Theme.Text;
            FormBorderStyle = FormBorderStyle.FixedDialog;
            MaximizeBox = MinimizeBox = false;
            Icon = WizardForm.AppIcon();
            StartPosition = FormStartPosition.CenterParent;
            int y = 16;
            Label head = new Label();
            head.Text = Lang.L("The same options as the game's own menu (F1 -> Game options, or F3). Most switch at once, even while the game runs; the ones marked built in change when the game is built again.",
                               "ゲーム内メニュー（F1 →「Game options」、または F3）と同じ設定です。「すぐ切替」の項目はゲーム中でも即座に切り替わります。「組み込み」の項目は再ビルドで変更します。");
            head.Location = new Point(16, y); head.Size = new Size(520, 54); head.ForeColor = Theme.Dim;
            Controls.Add(head);
            y += 60;
            if (options.Count == 0)
            {
                Label none = new Label();
                none.Text = Lang.L("No options yet: build this game again with this version of RawDangerPC.exe.", "設定がまだありません：このバージョンの RawDangerPC.exe で再ビルドしてください。");
                none.Location = new Point(16, y); none.Size = new Size(520, 40);
                Controls.Add(none);
                y += 44;
            }
            foreach (RecompOption o in options)
            {
                if (!o.Live) continue;   // fixes built in for good have no switch: not listed (docs\ROADMAP.md)
                CheckBox c = new CheckBox();
                c.Text = PatchNames.One(o.Name) + "   " + (o.Live ? Lang.L("(live)", "（すぐ切替）") : Lang.L("(built in)", "（組み込み）"));
                c.Checked = o.On;
                c.AutoCheck = o.Live;
                c.Location = new Point(20, y); c.Size = new Size(510, 28);
                c.ForeColor = o.Live ? Theme.Text : Theme.Dim;
                string name = o.Name;
                c.CheckedChanged += delegate { RecompIni.SetOn(file, name, c.Checked); Program.Trace("recomp " + game + ": " + name + " " + (c.Checked ? "on" : "off")); };
                Controls.Add(c);
                y += 32;
            }
            Button ok = new Button();
            ok.Text = "OK"; ok.Size = new Size(110, 32); ok.Location = new Point(420, y + 10);
            ok.FlatStyle = FlatStyle.Flat; ok.BackColor = Theme.Accent; ok.ForeColor = Theme.OnAccent; ok.FlatAppearance.BorderSize = 0;
            ok.Click += delegate { Close(); };
            Controls.Add(ok);
            AcceptButton = ok;
            ClientSize = new Size(550, y + 56);
        }
    }
}
