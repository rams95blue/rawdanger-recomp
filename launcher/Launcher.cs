// Raw Danger Launcher: finds the Raw Danger / Zettai Zetsumei Toshi 2 disc images (US, JP, JP demos), builds each one into a
// native PC program with ps2-native-recomp (its scripts\port.sh, the same commands its own app runs) with the
// rawdanger-recomp patch files, and starts them. No game data is copied anywhere except into ps2-native-recomp\games.
//
// C# 5 and .NET Framework 4 only: build.cmd compiles it with the csc.exe that ships with Windows 10/11.
//
//   RawDangerLauncher.exe                 the window
//   RawDangerLauncher.exe --play <name>   start a built game (what the desktop shortcuts run)
using System;
using System.Threading;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Windows.Forms;

[assembly: System.Reflection.AssemblyTitle("Raw Danger PC - rawdanger-recomp")]
[assembly: System.Reflection.AssemblyProduct("rawdanger-recomp")]
[assembly: System.Reflection.AssemblyDescription("rawdanger-recomp: builds Raw Danger! / Zettai Zetsumei Toshi 2 discs into an enhanced native PC port")]
[assembly: System.Reflection.AssemblyCompany("rams95")]
[assembly: System.Reflection.AssemblyCopyright("rawdanger-recomp; engine: ps2-native-recomp (MIT)")]
[assembly: System.Reflection.AssemblyVersion("0.6.3.0")]
[assembly: System.Reflection.AssemblyFileVersion("0.6.3.0")]

namespace RawDangerLauncher
{
    public static class Program
    {
        public const string Version = "0.9.9";
        public const string AppName = "Raw Danger PC";
        public static bool Restart;

        public static void Trace(string line)
        {
            try { System.IO.File.AppendAllText(Path.Combine(Settings.ExeDir, "launcher.log"), DateTime.Now.ToString("s") + "  " + line + "\r\n"); } catch (Exception) { }
        }

        static void Crash(object e)
        {
            Trace("CRASH " + e);
            try { MessageBox.Show("Something went wrong:\n\n" + e + "\n\nThe details are in launcher.log next to the program.", AppName, MessageBoxButtons.OK, MessageBoxIcon.Error); } catch (Exception) { }
        }

        [STAThread]
        public static int Main(string[] args)
        {
            Trace("start " + Version + " in " + Settings.ExeDir + " args: " + string.Join(" ", args));
            Application.ThreadException += delegate(object o, System.Threading.ThreadExceptionEventArgs e) { Crash(e.Exception); };
            AppDomain.CurrentDomain.UnhandledException += delegate(object o, UnhandledExceptionEventArgs e) { Crash(e.ExceptionObject); };
            Application.SetUnhandledExceptionMode(UnhandledExceptionMode.CatchException);
            try
            {
                Application.EnableVisualStyles();
                Application.SetCompatibleTextRenderingDefault(false);
                Settings settings = Settings.Load();
                Lang.Choose(settings);
                Trace("engine: " + settings.PnrDir + " | discs: " + settings.GameFolders + " | patches: " + settings.PatchDir);
                if (args.Length >= 2 && args[0] == "--look")   // the look= line of a cast picture, for characters.txt
                {
                    string look;
                    using (System.Drawing.Bitmap b = new System.Drawing.Bitmap(args[1])) look = CastData.Fingerprint(b);
                    MessageBox.Show("look=" + look, AppName);
                    Clipboard.SetText("look=" + look);
                    return 0;
                }
                if (args.Length >= 2 && args[0] == "--play")
                {
                    string problem = Games.Play(settings, args[1]);
                    if (problem != null) { MessageBox.Show(problem, AppName, MessageBoxButtons.OK, MessageBoxIcon.Information); return 1; }
                    LanguageSwitch.WaitAll();   // stays (without a window) while the game may ask for its other language
                    return 0;
                }
                // Changing the language closes the window and opens it again in the other language.
                new System.Threading.Thread(delegate() { WizardForm.RemoveOldBuildTemps(); }) { IsBackground = true }.Start();   // v0.9.9
                do { Restart = false; Lang.Choose(settings); Application.Run(new WizardForm(settings)); } while (Restart);
                LanguageSwitch.WaitAll();   // the window closed while a game runs: a language switch still works
            }
            catch (Exception e) { Crash(e); return 1; }
            return 0;
        }
    }

    // English / Japanese text: Lang.L("English", "日本語").
    public static class Lang
    {
        public static bool JP;
        public static string L(string en, string jp) { return JP ? jp : en; }
        public static string UiFont { get { return JP ? "Meiryo UI" : "Segoe UI"; } }
        public static void Choose(Settings s)
        {
            if (s.Language == "jp") JP = true;
            else if (s.Language == "en") JP = false;
            else JP = System.Globalization.CultureInfo.CurrentUICulture.TwoLetterISOLanguageName == "ja";
        }
    }

    // Patch sections in plain words, for the Play buttons and the game list.
    public static class PatchNames
    {
        public static string One(string section)
        {
            string x = section.ToLowerInvariant();
            if (x.Contains("fast loading")) return Lang.L("fast-loading fix", "高速ロード修正");
            if (x.StartsWith("widescreen")) return Lang.L("widescreen 16:9", "ワイド画面 16:9");
            if (x.Contains("original ps2 post processing")) return Lang.L("PS2 post-processing filter", "PS2ポストプロセスフィルター");
            if (x.Contains("post-processing") || x.Contains("post processing")) return Lang.L("no blur filter", "ぼかし無効");
            if (x.StartsWith("skip intro logos")) return Lang.L("no intro logos", "起動ロゴなし");
            if (x.StartsWith("free camera tilt")) return Lang.L("free camera tilt (look further up and down)", "カメラの上下を広く");
            if (x.StartsWith("direct mouse look")) return Lang.L("direct mouse look", "マウスで直接視点操作");
            if (x.Contains("debug")) return Lang.L("debug text", "デバッグ表示");
            if (x.StartsWith("voices: japanese")) return Lang.L("Japanese voices (off: English)", "日本語音声（オフ：英語）");
            return section;
        }

        public static string List(IEnumerable<string> sections, bool enhanced)
        {
            List<string> words = new List<string>();
            foreach (string sec in sections) if (sec.Trim() != "") words.Add(One(sec.Trim()));
            if (enhanced) words.Add(Lang.L("2x resolution", "解像度2倍"));
            if (words.Count == 0) return Lang.L("No patches: the game as it is on the disc.", "パッチなし：ディスクのままのゲームです。");
            return Lang.L("Patches: ", "パッチ: ") + string.Join(Lang.L(", ", "、"), words.ToArray());
        }

        // The sections an already built game was built with (app.json)
        public static List<string> Built(Settings s, string game)
        {
            Dictionary<string, string> app = Games.ReadApp(s, game);
            List<string> list = new List<string>();
            if (app == null) return list;
            foreach (string x in Games.Get(app, "sections").Split('|')) if (x.Trim() != "") list.Add(x.Trim());
            return list;
        }
    }

    // The look: US = disaster poster (black, hazard-tape yellow, alarm red); JP = the Japanese release's colours (white, ice
    // blue, deep blue). Picked with the language.
    public static class Theme
    {
        static bool J { get { return Lang.JP; } }
        public static System.Drawing.Color C(int r, int g, int b) { return System.Drawing.Color.FromArgb(r, g, b); }
        public static System.Drawing.Color Back { get { return J ? C(246, 250, 254) : C(12, 12, 11); } }
        public static System.Drawing.Color Card { get { return J ? C(226, 237, 250) : C(30, 28, 22); } }
        public static System.Drawing.Color Line { get { return J ? C(150, 184, 226) : C(92, 80, 24); } }
        public static System.Drawing.Color Text { get { return J ? C(14, 30, 64) : C(242, 238, 226); } }
        public static System.Drawing.Color Dim { get { return J ? C(84, 108, 146) : C(168, 160, 136); } }
        public static System.Drawing.Color Accent { get { return J ? C(18, 86, 196) : C(255, 204, 0); } }       // headings, primary buttons
        public static System.Drawing.Color OnAccent { get { return J ? System.Drawing.Color.White : System.Drawing.Color.Black; } }
        public static System.Drawing.Color AccentHover { get { return J ? C(40, 116, 228) : C(255, 226, 80); } }
        public static System.Drawing.Color Alarm { get { return J ? C(0, 160, 230) : C(222, 40, 28); } }     // second colour (marks, pressed)
        public static System.Drawing.Color Bad { get { return J ? C(200, 36, 36) : C(255, 92, 70); } }
        public static System.Drawing.Color Good { get { return J ? C(20, 132, 76) : C(150, 214, 96); } }
        public static System.Drawing.Color Tape1 { get { return J ? C(40, 110, 214) : C(255, 204, 0); } }      // the stripes
        public static System.Drawing.Color Tape2 { get { return J ? C(236, 244, 253) : C(14, 14, 12); } }
        public static System.Drawing.Color HeaderTop { get { return J ? C(204, 226, 250) : C(70, 28, 8); } }
        public static System.Drawing.Color HeaderBottom { get { return J ? C(250, 252, 255) : C(12, 12, 11); } }
        public static System.Drawing.Color Hover { get { return J ? C(214, 230, 250) : C(48, 42, 14); } }
        public static System.Drawing.Color Pressed { get { return J ? C(190, 214, 246) : C(70, 60, 16); } }
        public static System.Drawing.Color Disabled { get { return J ? C(150, 160, 176) : C(90, 86, 74); } }
        public static System.Drawing.Color Header { get { return J ? C(208, 226, 248) : C(40, 36, 14); } }
        public static System.Drawing.Color LogBack { get { return J ? C(234, 242, 252) : C(12, 12, 14); } }
        public static System.Drawing.Color LogText { get { return J ? C(20, 40, 80) : C(185, 200, 185); } }
        public static System.Drawing.Color Dot { get { return J ? C(180, 200, 228) : C(70, 66, 52); } }
        public static System.Drawing.Color Glow { get { return J ? System.Drawing.Color.FromArgb(40, 18, 86, 196) : System.Drawing.Color.FromArgb(14, 255, 204, 0); } }
        public static System.Drawing.Color PicTop { get { return J ? C(214, 232, 252) : C(60, 26, 8); } }
    }

    // launcher.ini next to the exe: key=value lines.
    public class Settings
    {
        public string File = "";
        public string PnrDir = "";
        public string GameFolders = "";    // ; separated disc folders, searched with their subfolders
        public string PatchDir = "";
        public string Extra = "";          // ; separated disc images picked with Browse
        public string Language = "";       // "en" or "jp"; empty = from Windows' language
        public string CastPictures = "";   // another folder with the cast pictures and music; empty = the art folder
        public string Unofficial = "0";    // also RetroAchievements' unofficial (experimental) achievements, 1 / 0
        public string MusicOn = "1";       // launcher music (a music file in the art folder), 1 on / 0 off
        public string MusicVolume = "0.25";  // launcher music volume, 0 ... 1
        public string BuildGentle = "1";   // builds leave the PC usable: fewer compiler jobs, below-normal priority (0 = all cores)
        public string BuildJobs = "";      // a fixed number of compiler jobs (empty = worked out from the cores and memory)
        public string EngineCheck = "0";   // check ps2-native-recomp for changes that can help, at start (once a day); off by default since
                                           // v0.7.4: the fork in engine\ is maintained for this game on its own (1 = check again)
        public string EngineSeen = "";     // the upstream change the player hid the notice at ("Hide until new ones")
        public string TexturesAsked = "0";  // the Enhanced Edition's "make the disc again for the Japanese textures" question was shown
        public string Window = "";         // where the window was last ("x,y"), so it opens there again (also after a language change)
        public Dictionary<string, string> Choices = new Dictionary<string, string>();   // game name -> ticked sections, | separated

        public static string ExeDir { get { return Path.GetDirectoryName(Application.ExecutablePath); } }

        public static Settings Load()
        {
            Settings s = new Settings();
            s.File = Path.Combine(ExeDir, "launcher.ini");
            if (System.IO.File.Exists(s.File))
            {
                foreach (string line in System.IO.File.ReadAllLines(s.File, Encoding.UTF8))
                {
                    int eq = line.IndexOf('=');
                    if (eq <= 0 || line.StartsWith("#")) continue;
                    string key = line.Substring(0, eq).Trim(), value = line.Substring(eq + 1).Trim();
                    if (key == "PnrDir") s.PnrDir = value;
                    else if (key == "GameFolders") s.GameFolders = value;
                    else if (key == "PatchDir") s.PatchDir = value;
                    else if (key == "ExtraImages") s.Extra = value;
                    else if (key == "Language") s.Language = value;
                    else if (key == "CastPictures") s.CastPictures = value;
                    else if (key == "Music") s.MusicOn = value;
                    else if (key == "MusicVolume") s.MusicVolume = value;
                    else if (key == "Window") s.Window = value;
                    else if (key == "EngineCheck2") s.EngineCheck = value;   // (v0.7.4: a new name, so the old "on" of earlier versions is not kept)
                    else if (key == "BuildGentle") s.BuildGentle = value;
                    else if (key == "BuildJobs") s.BuildJobs = value;
                    else if (key == "EngineSeen") s.EngineSeen = value;
                    else if (key == "AchievementsUnofficial") s.Unofficial = value;
                    else if (key == "EnhancedTexturesAsked") s.TexturesAsked = value;
                    else if (key.StartsWith("Sections.")) s.Choices[key.Substring(9)] = value;
                }
            }
            s.FillDefaults();
            return s;
        }

        // Walks up from the exe: the folder with GAME US / GAME JP and the ps2-native-recomp copy (the one with scripts\port.sh).
        public void FillDefaults()
        {
            // a project folder moved somewhere else (another drive, renamed): saved paths that are gone are found again
            if (PnrDir != "" && !System.IO.File.Exists(Path.Combine(PnrDir, Path.Combine("scripts", "port.sh")))) { Program.Trace("settings: engine gone from " + PnrDir + ", looking again"); PnrDir = ""; }
            if (PatchDir != "" && !Directory.Exists(PatchDir)) PatchDir = "";
            if (CastPictures != "" && !Directory.Exists(CastPictures)) CastPictures = "";
            if (GameFolders != "")
            {
                List<string> kept = new List<string>();
                foreach (string g in GameFolders.Split(';')) if (g.Trim() != "" && Directory.Exists(g.Trim())) kept.Add(g.Trim());
                string now = string.Join(";", kept.ToArray());
                if (now != GameFolders) { Program.Trace("settings: missing disc folders dropped (" + GameFolders + " -> " + now + ")"); GameFolders = now; }
            }
            if (Extra != "")
            {
                List<string> kept = new List<string>();
                foreach (string g in Extra.Split(';')) if (g.Trim() != "" && System.IO.File.Exists(g.Trim())) kept.Add(g.Trim());
                Extra = string.Join(";", kept.ToArray());
            }
            string dir = ExeDir;
            for (int up = 0; up < 5 && dir != null; up++, dir = Path.GetDirectoryName(dir))
            {
                if (PatchDir == "" && Directory.Exists(Path.Combine(dir, "patches", "pnach"))) PatchDir = Path.Combine(dir, "patches", "pnach");
                if (PnrDir == "" && System.IO.File.Exists(Path.Combine(dir, "engine", "ps2-native-recomp", "scripts", "port.sh")))
                    PnrDir = Path.Combine(dir, "engine", "ps2-native-recomp");
                if (PnrDir == "" && Directory.Exists(dir))
                {
                    if (System.IO.File.Exists(Path.Combine(dir, "scripts", "port.sh"))) PnrDir = dir;
                    else
                    {
                        string[] children = new string[0];
                        try { children = Directory.GetDirectories(dir); } catch (Exception) { }
                        foreach (string child in children)
                            if (System.IO.File.Exists(Path.Combine(child, "scripts", "port.sh"))) { PnrDir = child; break; }
                    }
                }
                if (GameFolders == "" && Directory.Exists(dir))
                {
                    List<string> found = new List<string>();
                    foreach (string name in new string[] { "GAME US", "GAME JP", "GAME EU", "GAMES", "isos" })
                        if (Directory.Exists(Path.Combine(dir, name))) found.Add(Path.Combine(dir, name));
                    if (found.Count > 0) GameFolders = string.Join(";", found.ToArray());
                }
            }
        }

        public List<string> ExtraImages()
        {
            return new List<string>(Extra.Split(new char[] { ';' }, StringSplitOptions.RemoveEmptyEntries));
        }

        public void AddExtraImage(string image)
        {
            List<string> list = ExtraImages();
            if (!list.Exists(delegate(string x) { return string.Equals(x, image, StringComparison.OrdinalIgnoreCase); })) list.Add(image);
            Extra = string.Join(";", list.ToArray());
            Save();
        }

        // Another ps2-native-recomp folder near the exe (e.g. the ps2-native-recomp-main the player downloaded and set up before):
        // its portable tools and downloaded libraries are copied instead of downloaded again.
        public string OtherEngine()
        {
            string dir = ExeDir;
            for (int up = 0; up < 5 && dir != null; up++, dir = Path.GetDirectoryName(dir))
            {
                string[] children = new string[0];
                try { children = Directory.GetDirectories(dir); } catch (Exception) { }
                List<string> candidates = new List<string>(children);
                candidates.Add(dir);
                foreach (string c in candidates)
                {
                    if (PnrDir != "" && string.Equals(Path.GetFullPath(c).TrimEnd('\\', '/'), Path.GetFullPath(PnrDir).TrimEnd('\\', '/'), StringComparison.OrdinalIgnoreCase)) continue;
                    if (System.IO.File.Exists(Path.Combine(c, "scripts", "port.sh")) && System.IO.File.Exists(Path.Combine(c, "tools", "portable", "env.ps1"))) return c;
                }
            }
            return "";
        }

        public void Save()
        {
            StringBuilder sb = new StringBuilder();
            sb.AppendLine("# Raw Danger Launcher settings");
            sb.AppendLine("PnrDir=" + PnrDir);
            sb.AppendLine("GameFolders=" + GameFolders);
            sb.AppendLine("PatchDir=" + PatchDir);
            sb.AppendLine("ExtraImages=" + Extra);
            sb.AppendLine("Language=" + Language);
            sb.AppendLine("CastPictures=" + CastPictures);
            sb.AppendLine("Music=" + MusicOn);
            sb.AppendLine("MusicVolume=" + MusicVolume);
            sb.AppendLine("Window=" + Window);
            sb.AppendLine("EngineCheck2=" + EngineCheck);
            sb.AppendLine("BuildGentle=" + BuildGentle);
            sb.AppendLine("BuildJobs=" + BuildJobs);
            sb.AppendLine("EngineSeen=" + EngineSeen);
            sb.AppendLine("AchievementsUnofficial=" + Unofficial);
            sb.AppendLine("EnhancedTexturesAsked=" + TexturesAsked);
            foreach (KeyValuePair<string, string> kv in Choices) sb.AppendLine("Sections." + kv.Key + "=" + kv.Value);
            try { System.IO.File.WriteAllText(File, sb.ToString(), new UTF8Encoding(false)); } catch (Exception) { }
        }
    }

    // What each known disc is called, and the name of its folder in ps2-native-recomp\games.
    public static class Catalog
    {
        public static void Name(DiscInfo d, Settings settings)
        {
            string file = Path.GetFileNameWithoutExtension(d.ImagePath);
            bool demo = file.IndexOf("Taikenban", StringComparison.OrdinalIgnoreCase) >= 0 || file.IndexOf("demo", StringComparison.OrdinalIgnoreCase) >= 0
                        || d.ImagePath.Replace('/', '\\').IndexOf("\\DEMO\\", StringComparison.OrdinalIgnoreCase) >= 0;
            string tag = "";
            if (file.IndexOf("Type-A", StringComparison.OrdinalIgnoreCase) >= 0) tag = "A";
            else if (file.IndexOf("Type-B", StringComparison.OrdinalIgnoreCase) >= 0) tag = "B";
            if (d.Serial == "SLUS-21501") { d.Region = "US"; d.Title = Lang.L("Raw Danger! (USA)", "Raw Danger!（北米版）"); d.GameName = "RawDanger-US"; }
            else if (d.Serial == "SLPS-25606" && !demo) { d.Region = "JP"; d.Title = Lang.L("Zettai Zetsumei Toshi 2 (Japan)", "絶体絶命都市2 -凍てついた記憶たち-（日本版）"); d.GameName = "ZZT2-JP"; }
            else if (d.Serial.StartsWith("SLE")) { d.Region = "EU"; d.Title = file; d.GameName = "RawDanger-EU"; }
            else
            {
                d.Region = demo ? "JP demo" : (d.Serial.StartsWith("SLU") ? "US" : "JP");
                d.Title = demo ? Lang.L("Zettai Zetsumei Toshi 2 demo", "絶体絶命都市2 体験版") + (tag != "" ? " Type-" + tag : "") + Lang.L(" (Japan)", "（日本）") : file;
                d.GameName = demo ? "ZZT2-Demo" + (tag != "" ? tag : "-" + d.Serial.Replace("-", "")) : d.Serial;
            }
            // A game the ps2-native-recomp app (or this launcher) already built from the same image keeps its folder.
            string existing = Games.FindBySource(settings, d.ImagePath);
            if (existing != null) d.GameName = existing;
            // Our patch file: <serial>_<crc>.pnach, the PCSX2 naming.
            d.PnachPath = "";
            d.Sections.Clear();
            if (settings.PatchDir != "" && d.Serial != "")
            {
                string p = Path.Combine(settings.PatchDir, d.Serial + "_" + d.CrcText + ".pnach");
                if (System.IO.File.Exists(p)) { d.PnachPath = p; d.Sections.AddRange(Pnach.Sections(p)); }
            }
        }
    }

    public static class Pnach
    {
        public static List<string> Sections(string file)
        {
            List<string> list = new List<string>();
            foreach (string raw in System.IO.File.ReadAllLines(file))
            {
                string line = raw.Trim();
                if (line.StartsWith("[") && line.EndsWith("]") && line.Length > 2) list.Add(line.Substring(1, line.Length - 2));
            }
            return list;
        }

        public static string Description(string file, string section)
        {
            bool inside = false;
            foreach (string raw in System.IO.File.ReadAllLines(file))
            {
                string line = raw.Trim();
                if (line.StartsWith("[") && line.EndsWith("]")) { inside = line == "[" + section + "]"; continue; }
                if (inside && line.StartsWith("description=", StringComparison.OrdinalIgnoreCase)) return line.Substring(12);
            }
            return "";
        }

        // Ticked unless the player said otherwise: the fixes the game needs here, and widescreen.
        public static bool DefaultOn(string section)
        {
            return section.IndexOf("fix", StringComparison.OrdinalIgnoreCase) >= 0 || section.StartsWith("Widescreen", StringComparison.OrdinalIgnoreCase);
        }
    }

    // ps2-native-recomp's games\<name> folders: app.json (the format its app writes, so both see the same games) and playing.
    public static class Games
    {
        public static string Folder(Settings s, string name) { return Path.Combine(s.PnrDir, "games", name); }
        public static string Runner(Settings s, string name) { return Path.Combine(s.PnrDir, "build", "native-" + name, "ps2run.exe"); }

        public static Dictionary<string, string> ReadApp(Settings s, string name)
        {
            string file = Path.Combine(Folder(s, name), "app.json");
            if (!System.IO.File.Exists(file)) return null;
            return Json.ReadFlat(System.IO.File.ReadAllText(file, Encoding.UTF8));
        }

        public static string FindBySource(Settings s, string image)
        {
            string games = Path.Combine(s.PnrDir, "games");
            if (s.PnrDir == "" || !Directory.Exists(games)) return null;
            foreach (string dir in Directory.GetDirectories(games))
            {
                if (Path.GetFileName(dir) == Enhanced.Name) continue;   // built from the US disc too, but it is its own game
                Dictionary<string, string> app = ReadApp(s, Path.GetFileName(dir));
                string src;
                if (app == null || !app.TryGetValue("source", out src) || string.IsNullOrEmpty(src)) continue;
                try { if (string.Equals(Path.GetFullPath(src), Path.GetFullPath(image), StringComparison.OrdinalIgnoreCase)) return Path.GetFileName(dir); }
                catch (Exception) { }
            }
            return null;
        }

        public static string Resolve(Settings s, string path) { return Path.IsPathRooted(path) ? path : Path.Combine(s.PnrDir, path); }

        public static string Relative(Settings s, string path)
        {
            string root = s.PnrDir.TrimEnd('\\') + "\\";
            return path.StartsWith(root, StringComparison.OrdinalIgnoreCase) ? path.Substring(root.Length) : path;
        }

        public static bool IsBuilt(Settings s, string name)
        {
            Dictionary<string, string> app = ReadApp(s, name);
            string built;
            return app != null && app.TryGetValue("built", out built) && built != "" && System.IO.File.Exists(Runner(s, name));
        }

        // tools\rd_prompts.py make: <game folder>\prompts with the font picture, its index and prompts.env (the variables to set)
        static List<KeyValuePair<string, string>> KeyPrompts(Settings s, string name, string iso)
        {
            List<KeyValuePair<string, string>> vars = new List<KeyValuePair<string, string>>();
            try
            {
                string dir = Path.Combine(Folder(s, name), "prompts"), envFile = Path.Combine(dir, "prompts.env");
                if (!System.IO.File.Exists(envFile) || System.IO.File.GetLastWriteTimeUtc(envFile) < System.IO.File.GetLastWriteTimeUtc(iso) ||
                    !System.IO.File.ReadAllText(envFile).Contains("PS2_PROMPTS_VERSION=4"))   // made by an older rd_prompts.py (4: the menus' button pictures too, v0.9.3)
                {
                    string python = Path.Combine(Path.Combine(Path.Combine(Path.Combine(s.PnrDir, "tools"), "portable"), "python"), "python.exe");
                    if (!System.IO.File.Exists(python)) python = "python";
                    string tool = Path.Combine(Path.Combine(Settings.ExeDir, "tools"), "rd_prompts.py");
                    ProcessStartInfo p = new ProcessStartInfo(python, "-I \"" + tool + "\" make --iso \"" + iso + "\" --out \"" + dir + "\"");
                    p.UseShellExecute = false; p.CreateNoWindow = true;
                    p.RedirectStandardOutput = true; p.RedirectStandardError = true;
                    using (Process run = Process.Start(p))
                    {
                        string output = run.StandardOutput.ReadToEnd() + run.StandardError.ReadToEnd();
                        run.WaitForExit();
                        Program.Trace("key pictures: " + output.Trim() + " (exit " + run.ExitCode + ")");
                    }
                }
                // the PC rows of the game's own Options list: their words painted into the Options screen's label sheet
                // and background (tools\rd_options.py, well under a second): made again for a new disc, a recomp.ini with other
                // options (their names are rows too) or an older rd_options.py (no letters file: no "A:up" line; no v0.8.0 rows)
                string optIndex = Path.Combine(dir, "options_index.tsv"), optArt = Path.Combine(dir, "options_art.txt");
                string recompIni = RecompIni.PathOf(s, name);
                bool recompNewer = System.IO.File.Exists(recompIni) && System.IO.File.Exists(optIndex) &&
                                   System.IO.File.GetLastWriteTimeUtc(optIndex) < System.IO.File.GetLastWriteTimeUtc(recompIni);
                // (and a newer Japanese list: the Japanese look's Options sheet gets the words too, options\jp\)
                string jpList = Path.Combine(dir, "jp_index.tsv");
                bool jpNewer = System.IO.File.Exists(jpList) && System.IO.File.Exists(optIndex) &&
                               System.IO.File.GetLastWriteTimeUtc(optIndex) < System.IO.File.GetLastWriteTimeUtc(jpList);
                if (!System.IO.File.Exists(optIndex) || !System.IO.File.Exists(optArt) || System.IO.File.GetLastWriteTimeUtc(optIndex) < System.IO.File.GetLastWriteTimeUtc(iso) ||
                    recompNewer || jpNewer || !System.IO.File.ReadAllText(optArt).Contains("A:up\t") ||
                    !System.IO.File.ReadAllText(optArt).Contains("L:Keys on screen\t") ||   // v0.8.0 rows
                    !System.IO.File.ReadAllText(optArt).Contains("L:Livelier hero\t"))   // v0.9.7 rows (and v0.9.5's Wet look, v0.9.4's Head follows camera, v0.9.2's Fixed cameras)
                {
                    string python = Path.Combine(Path.Combine(Path.Combine(Path.Combine(s.PnrDir, "tools"), "portable"), "python"), "python.exe");
                    if (!System.IO.File.Exists(python)) python = "python";
                    string tool = Path.Combine(Path.Combine(Settings.ExeDir, "tools"), "rd_options.py");
                    string recompArg = System.IO.File.Exists(recompIni) ? " --recomp \"" + recompIni + "\"" : "";
                    ProcessStartInfo p = new ProcessStartInfo(python, "-I \"" + tool + "\" make --iso \"" + iso + "\" --out \"" + dir + "\"" + recompArg);
                    p.UseShellExecute = false; p.CreateNoWindow = true;
                    p.RedirectStandardOutput = true; p.RedirectStandardError = true;
                    using (Process run = Process.Start(p))
                    {
                        string output = run.StandardOutput.ReadToEnd() + run.StandardError.ReadToEnd();
                        run.WaitForExit();
                        Program.Trace("option rows: " + output.Trim() + " (exit " + run.ExitCode + ")");
                    }
                }
                if (System.IO.File.Exists(optArt))
                {
                    vars.Add(new KeyValuePair<string, string>("PS2_NATIVE_OPTIONS", "1"));
                    vars.Add(new KeyValuePair<string, string>("PS2_NATIVE_OPTIONS_ART", optArt));
                }
                if (!System.IO.File.Exists(envFile)) return vars;
                // the texture overrides of this start: the font, and while the Japanese faces are on (Enhanced Edition) the Japanese
                // pictures inside the maps and models (tools\rd_jp_overrides.py, made with the disc)
                string fontIndex = Path.Combine(dir, "font_index.tsv"), jpIndex = Path.Combine(dir, "jp_index.tsv");
                if (System.IO.File.Exists(fontIndex))
                {
                    // the Japanese pictures are always listed: the engine uses them only while the in-game switch is on, so the look
                    // changes at once
                    // (the Options sheet with the PC rows' words first: the Japanese list has a line for the same sheet)
                    string index = (System.IO.File.Exists(optIndex) ? System.IO.File.ReadAllText(optIndex) : "") + System.IO.File.ReadAllText(fontIndex) +
                                   (System.IO.File.Exists(jpIndex) ? System.IO.File.ReadAllText(jpIndex) : "");
                    System.IO.File.WriteAllText(Path.Combine(dir, "textures_index.tsv"), index);
                }
                foreach (string line in System.IO.File.ReadAllLines(envFile))
                {
                    int eq = line.IndexOf('=');
                    if (eq > 0) vars.Add(new KeyValuePair<string, string>(line.Substring(0, eq).Trim(), line.Substring(eq + 1).Trim()));
                }
            }
            catch (Exception e) { Program.Trace("key pictures: not made: " + e.Message); }
            return vars;
        }

        // The command the ps2-native-recomp app uses (Get-PlayCommand): program, disc, window, 16:9, data patches, memory card.
        public static string Play(Settings s, string name) { return Play(s, name, null); }

        // env: extra variables for this run (the performance report sets the engine's own measuring switches).
        public static string Play(Settings s, string name, Dictionary<string, string> env)
        {
            Dictionary<string, string> app = ReadApp(s, name);
            string exe = Runner(s, name);
            if (app == null || !System.IO.File.Exists(exe)) return Lang.L("'" + name + "' is not built yet. Open the launcher and build it.", "「" + name + "」はまだビルドされていません。ランチャーを開いてビルドしてください。");
            string elf = Resolve(s, Get(app, "elf")), iso = Resolve(s, Get(app, "iso"));
            if (!System.IO.File.Exists(iso)) return Lang.L("The disc image of '" + name + "' is gone:\n" + iso + "\nBuild it again.", "「" + name + "」のディスクイメージが見つかりません:\n" + iso + "\nもう一度ビルドしてください。");
            StringBuilder a = new StringBuilder();
            a.Append(Quote(elf)).Append(" --disc ").Append(Quote(iso)).Append(" --window");
            string options = RecompIni.PathOf(s, name);
            bool optionsHaveShape = System.IO.File.Exists(options) && System.IO.File.ReadAllText(options).Contains("aspect = ");
            // --widescreen also tells the game the console is set to 16:9; with the widescreen patch switched in recomp.ini that would
            // be widescreen twice (and a squeezed 4:3 picture when the patch is switched off), so the picture shape comes from recomp.ini
            if (Get(app, "widescreen") != "" && !optionsHaveShape) a.Append(" --widescreen");
            if (Get(app, "patches") != "") a.Append(" --patches ").Append(Quote(Resolve(s, Get(app, "patches"))));
            a.Append(" --memcard ").Append(Quote(Path.Combine(Folder(s, name), name + ".ps2")));
            ProcessStartInfo psi = new ProcessStartInfo(exe, a.ToString());
            psi.WorkingDirectory = Folder(s, name);
            psi.UseShellExecute = false;
            psi.CreateNoWindow = true;
            string title = Get(app, "title");
            psi.EnvironmentVariables["PS2_WINDOW_TITLE"] = title != "" ? title : name;   // a PC game's window: the game's name only
            // the finished picture is copied into the shown buffer at 449 rows from 448: read on its own inner samples, not with
            // the console's one-pixel blur (engine patch 0020, gs.wgsl RT_TENT): crisp at every internal resolution
            psi.EnvironmentVariables["PS2_RT_COPY_TENT"] = "0";
            // the game's colour filter (its brightness tint and depth haze, about 2,800 small draws a frame) done by the engine in one
            // pass with the same result to the last bit (engine patch 0023, docs/GRAPHICS.md): only used while that filter is on
            psi.EnvironmentVariables["PS2_GAME_FILTER_PASS"] = "1";
            string set = AchievementSet.For(name);
            if (set != "") { psi.EnvironmentVariables["PS2_ACHIEVEMENTS"] = set; psi.EnvironmentVariables["PS2_ACHIEVEMENTS_UNLOCKS"] = AchievementSet.Unlocks(s, name); psi.EnvironmentVariables["PS2_ACHIEVEMENTS_UNOFFICIAL"] = s.Unofficial == "1" ? "1" : "0"; psi.EnvironmentVariables["PS2_ACHIEVEMENTS_ICONS"] = AchievementSet.Icons(s, name); }
            if (System.IO.File.Exists(options)) psi.EnvironmentVariables["PS2_RECOMP_OPTIONS"] = options;
            // a short performance summary of every run (frame rate, stutters), in logs\<game>_performance.log next to this program
            try
            {
                string logs = Path.Combine(Settings.ExeDir, "logs");
                Directory.CreateDirectory(logs);
                psi.EnvironmentVariables["PS2_PERF_LOG"] = Path.Combine(logs, name + "_performance.log");
                // rain on the lens (F1 -> Graphics and sound) only while playing: Raw Danger's scene word (0x48F488) is 21 in the game
                // itself (US program; the Enhanced Edition runs it too); 0x48E008 in the JP program (the same word, found by matching
                // the two programs' code; RetroAchievements' set reads it there too)
                // and the map's weather (0 dry, 1-3 rain, 4-6 snow; set with each map by the function that prints "MAP Wether"):
                // drops only where it rains, as hard as it rains
                if (name.StartsWith("RawDanger-US", StringComparison.OrdinalIgnoreCase) || name == Enhanced.Name)
                {
                    psi.EnvironmentVariables["PS2_LENS_WHEN"] = "0x48F488=21";
                    psi.EnvironmentVariables["PS2_LENS_WEATHER"] = "0x48F310";
                    psi.EnvironmentVariables["PS2_LENS_YAW"] = "0x5FDBAC";   // the camera's angle: the drops slant when it turns
                    psi.EnvironmentVariables["PS2_LENS_MOTION"] = "@5FD7D0,310,1B0";   // the player's movement seen from the camera
                    psi.EnvironmentVariables["PS2_FREE_CAMERA"] = "C9,17C9B0,17C250,17F610,48F4E0,3EC,A,48F488=15";   // v0.9.0: the follow camera in the places with a fixed one (pnach [Free camera everywhere]; keys.ini free_camera, off): host call 201 at 0x17C9B0 runs 0x17C250, switching mode 10 to 0 with 0x17F610 while playing (scene 21) and no cutscene bars
                    psi.EnvironmentVariables["PS2_CAMERA_ORBIT"] = "48F4E0,35C,3B8,354,C8,177270";   // v0.9.0: the orbit camera (pnach [Orbit camera]): the camera block's pointer, its height (the two distances after it), look-at offset and base height, host call 200, called at 0x177270
                    psi.EnvironmentVariables["PS2_HEAD_FOLLOW"] = "CA,323EA4,323FB0,5EAC80,48F3E4,F0,7,48F4E0,3EC,160,310,48F488=15";   // v0.9.4: the hero's head looks where the camera looks (pnach [Head follows camera]; keys.ini head_follow, on): host call 202 at 0x323EA4 builds each bone's world matrix (0x323FB0) and turns the player's head (bone 7 of the model at character +0xF0) toward the active camera's heading (+0x160 to +0x310) while the follow camera (mode 0 at +0x3EC) runs in play (scene 21)
                    psi.EnvironmentVariables["PS2_HERO_WET"] = "48F730,5C38,24,14,8,1C,30,48F3E4,64,5FD690,48F4E0,160";   // v0.9.5: the wet look and the Debug tab's dry / soak: the thing pool (gp-0x4740), the player's worn things' wetness (+0x30, 0-100), the world-to-screen matrix 0x5FD690 and the camera's place
                    psi.EnvironmentVariables["PS2_DEBUG_STEPS"] = "490000,24C";   // v0.9.8: Settings > Debug > game steps a second (a test): the graphics block *(gp-0x3E70) +0x24C, vblanks a step (3), docs/FRAMERATE.md
                    psi.EnvironmentVariables["PS2_CAMERA_PITCH"] = "@5FD7D0,14,18,2.5,6.5,0.5,10";   // the camera's up / down range, wider
                    psi.EnvironmentVariables["PS2_CAMERA_ZOOM"] = "@5FD7D0,450,448";   // field of view (keys.ini field_of_view): the camera's zoom factor
                    // the game's own Options list (from the title and from the in-game menu's Options tab) gets real PC rows of its
                    // own (every setting of the F1 menu's Settings, and the live Game options of recomp.ini, in a list that scrolls):
                    // PS2_NATIVE_OPTIONS, set with the key
                    // pictures below (engine patch 0021); the old line over "Adjust screen position" (PS2_NATIVE_ROW) is gone
                    // mouse look: the camera's angle (radians, 0x5FDBAC) and height offset (0x5FDB88, -3.6 to 1.2) in the US program's camera
                    // block; the right stick turns them 0.001565 rad / 0.00358 per game step for each unit past 59 (measured in the banquet hall)
                    psi.EnvironmentVariables["PS2_MOUSE_LOOK_YAW"] = "0x5FDBAC,0.001565,59.1";
                    psi.EnvironmentVariables["PS2_MOUSE_LOOK_PITCH"] = "0x5FDB88,0.00358,59,-9,12";   // v0.8.6: -9 (about 55 degrees up, with [Free camera tilt]) to 12 (about 65 down); the game: -3.6 to 1.2
                    psi.EnvironmentVariables["PS2_MOUSE_LOOK_WHEN"] = "0x48F488=21";
                    psi.EnvironmentVariables["PS2_DEBUG_HERO"] = "5EAC80,48F3E4,EB,368,36C,370,374";   // v0.9.2: Settings > Game > Heal and warm / Keep healthy and warm: the characters' table, the player's number, 235 of them, health and body temperature (value, most)
                    psi.EnvironmentVariables["PS2_DEBUG_INFO"] = "48F488,48F4E0,3EC,310,48F3E4";   // v0.9.1: Settings > Game > Where am I: the scene, the camera block's mode and follow point, the player's character
                    psi.EnvironmentVariables["PS2_MOUSE_LOOK_FIRST"] = "48F4E0=5FDC60";   // v0.9.1: in the first-person view (Triangle; the active camera block is mode 1's) the mouse pushes the left stick, which looks round there
                    // (v0.8.1 made texture switches wait for the next area load; v0.8.3 found the real cause of the garbled
                    // characters, the Japanese pictures' headers, and they switch at once again: no PS2_AREA_LOAD)
                    // the mouse turns the camera itself (the pnach section [Direct mouse look], built in since v0.7.9: the camera adds
                    // the float at 0x7F10 to its turn of each step); a game built before that is turned through the stick as before
                    psi.EnvironmentVariables["PS2_MOUSE_LOOK_DIRECT"] = "0x7F10,0x176FD0=C4027F10";
                    // the developers' map select left on the disc (F7, or F1 -> Settings -> Game): the game's next-scene word and
                    // its scene MAP SEL (41); and the debug menu (DEBSEL, 40: map select, sound test, exit), whose text reads through
                    // PS2_SJIS_TEXT below and which took only Circle or Start until [Debug menu answers Cross] (v0.8.6)
                    psi.EnvironmentVariables["PS2_DEBUG_SCENES"] = "0x48F48C;0;41;21";   // v0.8.9: the map select only (F7): the debug menu (DEBSEL) only led to it, a sound test wired to nothing and a restart, so Shift+F7 is gone; was   // v0.8.6: the debug menu too (Shift+F7; it answers Cross with [Debug menu answers Cross])   // (from the game itself, scene 21)
                    // the developers' text through the US font sheet (texture base 3592): Shift-JIS strings shown in ASCII, and the map
                    // select's letters at the font's own size (engine patch 0024, gs.cpp sjis)
                    psi.EnvironmentVariables["PS2_SJIS_TEXT"] = "3592";
                    // the memory-card check at start answers itself (scene MCCHECK, 3): no "Starting without System data" wait
                    psi.EnvironmentVariables["PS2_AUTO_CROSS"] = "0x48F488=3";
                    // and is never seen: run at full speed with a black picture (the card is still read: options, unlocks)
                    psi.EnvironmentVariables["PS2_SKIP_SCENE"] = "0x48F488=3";
                }
                else if (name.StartsWith("ZZT2-JP", StringComparison.OrdinalIgnoreCase))
                {
                    psi.EnvironmentVariables["PS2_LENS_WHEN"] = "0x48E008=21";
                    psi.EnvironmentVariables["PS2_LENS_WEATHER"] = "0x48DE90";
                    psi.EnvironmentVariables["PS2_NATIVE_MENU"] = "0x48E008=16";
                    psi.EnvironmentVariables["PS2_AUTO_CROSS"] = "0x48E008=3";
                    psi.EnvironmentVariables["PS2_SKIP_SCENE"] = "0x48E008=3";
                    // v0.8.7: the developers' map select (F7) and debug menu (Shift+F7) here too: the same scenes (40, 41) and the
                    // scene request at gp - 0x49E4 as on the US program (0x16EAE4), 0x48E00C; the debug menu is drawn in the PC font
                    psi.EnvironmentVariables["PS2_DEBUG_SCENES"] = "0x48E00C;0;41;21";   // v0.8.9: the map select only (as above)
                }
            }
            catch (Exception) { }   // the F1 menu's Recomp options page
            // the game's texts show the player's keys in place of the PlayStation symbols (F1 -> Settings -> Controls): the game's
            // font is made into a texture override once (tools\rd_prompts.py, a second or two) and the engine draws the key caps
            if (name.StartsWith("RawDanger-US", StringComparison.OrdinalIgnoreCase) || name == Enhanced.Name)
                foreach (KeyValuePair<string, string> kv in KeyPrompts(s, name, iso)) psi.EnvironmentVariables[kv.Key] = kv.Value;
            if (env != null) foreach (KeyValuePair<string, string> kv in env) psi.EnvironmentVariables[kv.Key] = kv.Value;
            LanguageSwitch.Prepare(s, name, psi);
            LanguageSwitch.SavesBefore(s, name);   // the card image made from the saves folder (saves without the memory card)
            // the F1 menu's look follows the launcher's skin (Japanese: blue on white) until it is changed in the menu (keys.ini)
            Lang.Choose(s);
            psi.EnvironmentVariables["PS2_MENU_SKIN"] = Lang.JP ? "japanese" : "hazard";
            // the game window's icon in the taskbar (builds before v0.8.1 have none of their own)
            string gameIcon = Icons.ForGame(s, name);
            if (gameIcon != "") psi.EnvironmentVariables["PS2_WINDOW_ICON"] = gameIcon;
            Program.Trace("play " + name + (env != null ? " (performance report)" : "") + ": " + exe + " " + a);
            Process game = Process.Start(psi);
            LanguageSwitch.Watch(s, name, game);
            return null;
        }

        public static string Get(Dictionary<string, string> d, string key) { string v; return d != null && d.TryGetValue(key, out v) && v != null ? v : ""; }
        public static string Quote(string s) { return "\"" + s + "\""; }
    }

    // Just enough JSON for app.json: a flat object of strings (and the sections array, kept as | separated text).
    public static class Json
    {
        public static Dictionary<string, string> ReadFlat(string text)
        {
            Dictionary<string, string> d = new Dictionary<string, string>();
            int i = 0;
            Func<string> str = null;
            str = delegate
            {
                StringBuilder sb = new StringBuilder();
                i++;   // opening quote
                while (i < text.Length && text[i] != '"')
                {
                    if (text[i] == '\\' && i + 1 < text.Length)
                    {
                        i++;
                        char c = text[i];
                        if (c == 'n') sb.Append('\n');
                        else if (c == 't') sb.Append('\t');
                        else if (c == 'u' && i + 4 < text.Length) { sb.Append((char)Convert.ToInt32(text.Substring(i + 1, 4), 16)); i += 4; }
                        else sb.Append(c);
                    }
                    else sb.Append(text[i]);
                    i++;
                }
                i++;
                return sb.ToString();
            };
            while (i < text.Length)
            {
                if (text[i] != '"') { i++; continue; }
                string key = str();
                while (i < text.Length && (char.IsWhiteSpace(text[i]) || text[i] == ':')) i++;
                if (i >= text.Length) break;
                if (text[i] == '"') d[key] = str();
                else if (text[i] == '[')
                {
                    List<string> items = new List<string>();
                    i++;
                    while (i < text.Length && text[i] != ']') { if (text[i] == '"') items.Add(str()); else i++; }
                    i++;
                    d[key] = string.Join("|", items.ToArray());
                }
                else
                {
                    int start = i;
                    while (i < text.Length && text[i] != ',' && text[i] != '}' && text[i] != '\n') i++;
                    string v = text.Substring(start, i - start).Trim();
                    d[key] = v == "null" ? "" : v;
                }
            }
            return d;
        }

        static string Esc(string s)
        {
            StringBuilder sb = new StringBuilder("\"");
            foreach (char c in s)
            {
                if (c == '"' || c == '\\') sb.Append('\\').Append(c);
                else if (c < 32) sb.Append("\\u").Append(((int)c).ToString("x4"));
                else sb.Append(c);
            }
            return sb.Append('"').ToString();
        }

        public static string WriteApp(Dictionary<string, string> d, List<string> sections)
        {
            StringBuilder sb = new StringBuilder("{\r\n");
            List<string> lines = new List<string>();
            foreach (KeyValuePair<string, string> kv in d) if (kv.Key != "sections") lines.Add("    " + Esc(kv.Key) + ":  " + Esc(kv.Value));
            List<string> items = new List<string>();
            foreach (string s in sections) items.Add(Esc(s));
            lines.Add("    \"sections\":  [" + string.Join(", ", items.ToArray()) + "]");
            sb.Append(string.Join(",\r\n", lines.ToArray())).Append("\r\n}\r\n");
            return sb.ToString();
        }
    }

    // A desktop shortcut through IShellLinkW (Unicode paths), the same way the ps2-native-recomp app makes its own.
    public static class Shortcut
    {
        [ComImport, Guid("00021401-0000-0000-C000-000000000046")] class CShellLink { }

        [ComImport, InterfaceType(ComInterfaceType.InterfaceIsIUnknown), Guid("000214F9-0000-0000-C000-000000000046")]
        interface IShellLinkW
        {
            void GetPath([Out, MarshalAs(UnmanagedType.LPWStr)] StringBuilder pszFile, int cch, IntPtr pfd, uint fFlags);
            void GetIDList(out IntPtr ppidl);
            void SetIDList(IntPtr pidl);
            void GetDescription([Out, MarshalAs(UnmanagedType.LPWStr)] StringBuilder pszName, int cch);
            void SetDescription([MarshalAs(UnmanagedType.LPWStr)] string pszName);
            void GetWorkingDirectory([Out, MarshalAs(UnmanagedType.LPWStr)] StringBuilder pszDir, int cch);
            void SetWorkingDirectory([MarshalAs(UnmanagedType.LPWStr)] string pszDir);
            void GetArguments([Out, MarshalAs(UnmanagedType.LPWStr)] StringBuilder pszArgs, int cch);
            void SetArguments([MarshalAs(UnmanagedType.LPWStr)] string pszArgs);
            void GetHotkey(out short pwHotkey);
            void SetHotkey(short wHotkey);
            void GetShowCmd(out int piShowCmd);
            void SetShowCmd(int iShowCmd);
            void GetIconLocation([Out, MarshalAs(UnmanagedType.LPWStr)] StringBuilder pszIconPath, int cch, out int piIcon);
            void SetIconLocation([MarshalAs(UnmanagedType.LPWStr)] string pszIconPath, int iIcon);
            void SetRelativePath([MarshalAs(UnmanagedType.LPWStr)] string pszPathRel, uint dwReserved);
            void Resolve(IntPtr hwnd, uint fFlags);
            void SetPath([MarshalAs(UnmanagedType.LPWStr)] string pszFile);
        }

        [ComImport, InterfaceType(ComInterfaceType.InterfaceIsIUnknown), Guid("0000010b-0000-0000-C000-000000000046")]
        interface IPersistFile
        {
            void GetClassID(out Guid pClassID);
            [PreserveSig] int IsDirty();
            void Load([MarshalAs(UnmanagedType.LPWStr)] string pszFileName, uint dwMode);
            void Save([MarshalAs(UnmanagedType.LPWStr)] string pszFileName, [MarshalAs(UnmanagedType.Bool)] bool fRemember);
            void SaveCompleted([MarshalAs(UnmanagedType.LPWStr)] string pszFileName);
            void GetCurFile([MarshalAs(UnmanagedType.LPWStr)] out string ppszFileName);
        }

        public static void Create(string lnk, string target, string arguments, string workDir, string icon, string description)
        {
            IShellLinkW link = (IShellLinkW)new CShellLink();
            link.SetPath(target);
            link.SetArguments(arguments);
            link.SetWorkingDirectory(workDir);
            link.SetIconLocation(icon, 0);
            link.SetDescription(description);
            ((IPersistFile)link).Save(lnk, true);
        }
    }

    // F1 -> Language in the game (engine patch 0013): the text language of Raw Danger is the program itself (the Japanese text and
    // font are only in the JP program and on the JP disc), so switching language means the other build: the US build or the
    // Enhanced Edition <-> the JP build (ZZT2-JP). The game gets PS2_LANGUAGE (its language), PS2_LANGUAGE_OTHER and
    // PS2_LANGUAGE_SWITCH_FILE; choosing the entry writes that file and ends the game. Then the saves are carried over to the
    // other build's memory card (tools\rd_card.py mirror: same save format in both programs, folders BASLUS-21501* <->
    // BISLPS-25606*, the old card kept as .bak) and the other build starts, at the title, ready to load.
    static class LanguageSwitch
    {
        static int running;

        static bool Japanese(string name) { return name.StartsWith("ZZT2-JP", StringComparison.OrdinalIgnoreCase); }
        static bool English(string name) { return name.StartsWith("RawDanger-US", StringComparison.OrdinalIgnoreCase) || name == Enhanced.Name; }
        static string Code(string name) { return Japanese(name) ? "BISLPS-25606" : "BASLUS-21501"; }
        static string Card(Settings s, string name) { return Path.Combine(Games.Folder(s, name), name + ".ps2"); }
        static string FileOf(Settings s, string name) { return Path.Combine(Games.Folder(s, name), "language.switch"); }

        public static string Partner(Settings s, string name)
        {
            if (English(name)) return Games.IsBuilt(s, "ZZT2-JP") ? "ZZT2-JP" : "";
            if (!Japanese(name)) return "";
            if (Games.IsBuilt(s, Enhanced.Name)) return Enhanced.Name;
            try
            {
                foreach (string dir in Directory.GetDirectories(Path.Combine(s.PnrDir, "games"), "RawDanger-US*"))
                    if (Games.IsBuilt(s, Path.GetFileName(dir))) return Path.GetFileName(dir);
            }
            catch (Exception) { }
            return "";
        }

        public static void Prepare(Settings s, string name, ProcessStartInfo psi)
        {
            if (!English(name) && !Japanese(name)) return;
            psi.EnvironmentVariables["PS2_LANGUAGE"] = Japanese(name) ? "Japanese" : "English";
            string partner = Partner(s, name);
            if (partner == "") return;
            string file = FileOf(s, name);
            try { if (File.Exists(file)) File.Delete(file); } catch (Exception) { }
            psi.EnvironmentVariables["PS2_LANGUAGE_OTHER"] = Japanese(partner) ? "Japanese" : "English";
            psi.EnvironmentVariables["PS2_LANGUAGE_SWITCH_FILE"] = file;
        }

        public static void Watch(Settings s, string name, Process game)
        {
            if (game == null || (!English(name) && !Japanese(name))) return;
            Interlocked.Increment(ref running);
            try
            {
                game.EnableRaisingEvents = true;
                game.Exited += delegate
                {
                    try { SavesAfter(s, name); if (Partner(s, name) != "") Switch(s, name); }
                    catch (Exception e) { Program.Trace("language switch: " + e.Message); }
                    finally { Interlocked.Decrement(ref running); }
                };
            }
            catch (Exception e) { Program.Trace("language switch: " + e.Message); }
        }

        static void Switch(Settings s, string name)
        {
            string file = FileOf(s, name);
            if (!File.Exists(file)) return;
            try { File.Delete(file); } catch (Exception) { }
            string partner = Partner(s, name);
            if (partner == "") return;
            Program.Trace("language switch: " + name + " -> " + partner);   // the saves come along: the partner's card is made from the saves folder
            string problem = Games.Play(s, partner);
            if (problem != null) { Program.Trace("language switch: " + problem); MessageBox.Show(problem, Program.AppName); }
        }

        // Saves without the memory card (v0.7.8, tools\rd_card.py): the saves are plain folders in "saves" beside RawDangerPC.exe, one
        // per save (BASLUS-21501 the system data, BASLUS-21501000... the slots; BISLPS-25606... for the Japanese build), shared by
        // every build. Before a start the build's card image is made from them (the other language's saves of a slot, when newer,
        // under this build's name); after the game ends the card's saves are written back (only the changed ones; a save deleted in
        // the game goes to saves\_deleted). The first time, the build's old card is read in and kept as <game>.ps2.before-save-folders.
        public static string SavesDir() { return Path.Combine(Settings.ExeDir, "saves"); }
        static string Stamp(string name) { return Path.Combine(SavesDir(), "." + name + ".written"); }

        public static void SavesBefore(Settings s, string name)
        {
            if (!English(name) && !Japanese(name)) return;
            string card = Card(s, name), stamp = Stamp(name);
            try
            {
                Directory.CreateDirectory(SavesDir());
                bool first = !File.Exists(stamp);
                if (File.Exists(card) && (first || File.GetLastWriteTimeUtc(card) > File.GetLastWriteTimeUtc(stamp)))
                {
                    // saves the folder does not have yet: the old card (first time), or a run whose end was not seen (a crash)
                    if (first && !File.Exists(card + ".before-save-folders")) File.Copy(card, card + ".before-save-folders");
                    if (Tool(s, "export --card \"" + card + "\" --to \"" + SavesDir() + "\" --code " + Code(name) + (first ? " --keep-others" : "")) != 0) return;
                }
                string other = Japanese(name) ? "BASLUS-21501" : "BISLPS-25606";
                if (Tool(s, "build --from \"" + SavesDir() + "\" --card \"" + card + "\" --code " + Code(name) + " --other-code " + other) == 0)
                    File.WriteAllText(stamp, DateTime.Now.ToString("s"));
            }
            catch (Exception e) { Program.Trace("saves: " + e.Message); }
        }

        static void SavesAfter(Settings s, string name)
        {
            string card = Card(s, name);
            if (!File.Exists(card)) return;
            if (Tool(s, "export --card \"" + card + "\" --to \"" + SavesDir() + "\" --code " + Code(name)) == 0)
                try { File.WriteAllText(Stamp(name), DateTime.Now.ToString("s")); } catch (Exception) { }
        }

        static int Tool(Settings s, string args)
        {
            string python = Path.Combine(Path.Combine(Path.Combine(Path.Combine(s.PnrDir, "tools"), "portable"), "python"), "python.exe");
            if (!File.Exists(python)) python = "python";
            string tool = Path.Combine(Path.Combine(Settings.ExeDir, "tools"), "rd_card.py");
            ProcessStartInfo psi = new ProcessStartInfo(python, "-I \"" + tool + "\" " + args);
            psi.UseShellExecute = false; psi.CreateNoWindow = true;
            psi.RedirectStandardOutput = true; psi.RedirectStandardError = true;
            psi.EnvironmentVariables["PS2_SCRIPTS"] = Path.Combine(s.PnrDir, "scripts");   // mcformat.py: a freshly formatted card
            try
            {
                using (Process p = Process.Start(psi))
                {
                    string output = p.StandardOutput.ReadToEnd() + p.StandardError.ReadToEnd();
                    p.WaitForExit();
                    Program.Trace("saves: " + output.Trim() + " (exit " + p.ExitCode + ")");
                    return p.ExitCode;
                }
            }
            catch (Exception e) { Program.Trace("saves: rd_card.py did not run: " + e.Message); return 1; }
        }

        public static void WaitAll()
        {
            while (Thread.VolatileRead(ref running) > 0) Thread.Sleep(300);
        }
    }
}
