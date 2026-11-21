// Raw Danger PC - does ps2-native-recomp (the engine our fork in engine\ps2-native-recomp comes from) have changes that can
// help Raw Danger? Checked when the launcher starts (at most once a day, in the background, launcher.ini EngineCheck2=1 turns it
// on; off by default since v0.7.4): GitHub's compare of our upstream base (engine\FORK.md, "Upstream base: commit ...") with upstream's branch, each change
// sorted into an area by launcher\engine_watch.txt, and the files it touches compared with the ones our patches change
// (patches\native\*.patch). Only reads public data from api.github.com; nothing about this PC is sent.
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Globalization;
using System.IO;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
using System.Windows.Forms;

namespace RawDangerLauncher
{
    public static class EngineUpdates
    {
        public class Change
        {
            public string Sha = "", Date = "", Title = "", Area = "", Url = "";
            public int Weight;
        }

        public class Report
        {
            public string Upstream = "", Branch = "", Base = "", Head = "", Problem;
            public int Ahead, Points;
            public bool Significant, FromCache;
            public DateTime When;
            public List<Change> Changes = new List<Change>();      // the ones that count (weight > 0), newest first
            public int Ignored;                                     // skipped or weight 0
            public List<string> Clashes = new List<string>();      // "host/window.cpp (0002, 0005, 0007)": our patches touch it too
            public Dictionary<string, int> Areas = new Dictionary<string, int>();
        }

        class Rule { public string Area; public int Weight; public Regex Words; }

        static string upstream = "exoteric33/ps2-native-recomp", branch = "main";
        static readonly List<Rule> rules = new List<Rule>();
        static readonly List<Regex> skips = new List<Regex>();

        static void LoadRules()
        {
            rules.Clear(); skips.Clear();
            string file = Path.Combine(Path.Combine(Settings.ExeDir, "launcher"), "engine_watch.txt");
            string[] lines;
            try { lines = File.ReadAllLines(file); }
            catch (Exception)
            {
                // no rules file next to the program: a short built-in set
                lines = new string[] {
                    "skip = ^Merge |^README\\b|games that run|^Tests? ",
                    "rule = Films | 3 | \\bfilm|video decoder|\\bIPU\\b",
                    "rule = Sound and voices | 3 | \\bsound|\\bSPU|\\baudio|\\bvoice",
                    "rule = Speed | 3 | faster|speed|stutter|\\bpace\\b|AVX2",
                    "rule = Crashes, hangs and wrong behaviour | 3 | \\bfix|crash|\\bhang|\\bwait|interrupt|delay.slot",
                    "rule = Textures | 3 | texture|\\bHD\\b",
                    "rule = Other engine changes | 1 | ." };
            }
            foreach (string raw in lines)
            {
                string t = raw.Trim();
                if (t == "" || t.StartsWith("#")) continue;
                int eq = t.IndexOf('=');
                if (eq < 0) continue;
                string k = t.Substring(0, eq).Trim(), v = t.Substring(eq + 1).Trim();
                try
                {
                    if (k == "upstream") upstream = v;
                    else if (k == "branch") branch = v;
                    else if (k == "skip") skips.Add(new Regex(v, RegexOptions.IgnoreCase));
                    else if (k == "rule")
                    {
                        string[] p = v.Split(new char[] { '|' }, 3);
                        if (p.Length < 3) continue;
                        Rule r = new Rule();
                        r.Area = p[0].Trim(); r.Weight = int.Parse(p[1].Trim()); r.Words = new Regex(p[2].Trim(), RegexOptions.IgnoreCase);
                        rules.Add(r);
                    }
                }
                catch (Exception e) { Program.Trace("engine_watch.txt: " + t + ": " + e.Message); }
            }
        }

        // our upstream base, from engine\FORK.md ("Upstream base: commit `<sha>`")
        public static string Base(Settings s)
        {
            foreach (string f in new string[] { Path.Combine(Path.Combine(Settings.ExeDir, "engine"), "FORK.md"),
                                                s.PnrDir != "" ? Path.Combine(Path.GetDirectoryName(s.PnrDir.TrimEnd('\\', '/')), "FORK.md") : "" })
                try
                {
                    if (f == "" || !File.Exists(f)) continue;
                    Match m = Regex.Match(File.ReadAllText(f), @"Upstream base: commit `([0-9a-f]{7,40})`");
                    if (m.Success) return m.Groups[1].Value;
                }
                catch (Exception) { }
            return "";
        }

        static string CacheFile { get { return Path.Combine(Path.Combine(Settings.ExeDir, "logs"), "engine_updates.json"); } }

        // The check. Uses the answer saved in logs\engine_updates.json when it is less than a day old (force: ask again).
        public static Report Check(Settings s, bool force)
        {
            Report r = new Report();
            LoadRules();
            r.Upstream = upstream; r.Branch = branch; r.Base = Base(s);
            if (r.Base == "") { r.Problem = Lang.L("engine\\FORK.md does not say which upstream commit our engine is based on.", "engine\\FORK.md に元のコミットが書かれていません。"); return r; }
            string json = null;
            try
            {
                if (!force && File.Exists(CacheFile) && (DateTime.Now - File.GetLastWriteTime(CacheFile)).TotalHours < 24)
                {
                    string saved = File.ReadAllText(CacheFile, Encoding.UTF8);
                    if (saved.StartsWith(r.Base + "\n")) { json = saved.Substring(r.Base.Length + 1); r.FromCache = true; r.When = File.GetLastWriteTime(CacheFile); }
                }
                if (json == null)
                {
                    System.Net.ServicePointManager.SecurityProtocol = (System.Net.SecurityProtocolType)3072;   // TLS 1.2
                    using (System.Net.WebClient wc = new System.Net.WebClient())
                    {
                        wc.Headers[System.Net.HttpRequestHeader.UserAgent] = "RawDangerPC/" + Program.Version + " (rawdanger-recomp engine update check)";
                        wc.Headers[System.Net.HttpRequestHeader.Accept] = "application/vnd.github+json";
                        wc.Encoding = Encoding.UTF8;
                        json = wc.DownloadString("https://api.github.com/repos/" + upstream + "/compare/" + r.Base + "..." + branch + "?per_page=250");
                    }
                    r.When = DateTime.Now;
                    try { Directory.CreateDirectory(Path.GetDirectoryName(CacheFile)); File.WriteAllText(CacheFile, r.Base + "\n" + json, new UTF8Encoding(false)); } catch (Exception) { }
                }
            }
            catch (Exception e) { r.Problem = Lang.L("Could not reach GitHub: ", "GitHub に接続できませんでした: ") + e.Message; return r; }
            try { Fill(r, JsonTree.Parse(json) as Dictionary<string, object>); }
            catch (Exception e) { r.Problem = Lang.L("GitHub's answer could not be read: ", "GitHub の応答を読めませんでした: ") + e.Message; }
            return r;
        }

        // the report from a compare answer (also used by tests with a saved answer)
        public static void Fill(Report r, Dictionary<string, object> d)
        {
            if (d == null) throw new Exception("not an object");
            r.Ahead = JsonTree.Int(d, "ahead_by");
            List<object> commits = d.ContainsKey("commits") ? d["commits"] as List<object> : null;
            if (commits == null) throw new Exception(d.ContainsKey("message") ? Convert.ToString(d["message"]) : "no commits in the answer");
            for (int i = commits.Count - 1; i >= 0; i--)
            {
                Dictionary<string, object> c = commits[i] as Dictionary<string, object>;
                if (c == null) continue;
                Dictionary<string, object> inner = c.ContainsKey("commit") ? c["commit"] as Dictionary<string, object> : null;
                string message = inner != null && inner.ContainsKey("message") ? Convert.ToString(inner["message"]) : "";
                string title = message.Split('\n')[0].Trim();
                string date = "";
                if (inner != null && inner.ContainsKey("committer") && inner["committer"] is Dictionary<string, object>)
                {
                    string iso = Convert.ToString(((Dictionary<string, object>)inner["committer"])["date"]);
                    date = iso.Length >= 10 ? iso.Substring(0, 10) : iso;
                }
                if (i == commits.Count - 1) r.Head = Convert.ToString(c["sha"]);
                bool skipped = false;
                foreach (Regex k in skips) if (k.IsMatch(title)) { skipped = true; break; }
                Rule hit = null;
                if (!skipped) foreach (Rule u in rules) if (u.Words.IsMatch(title)) { hit = u; break; }
                if (hit == null || hit.Weight <= 0) { r.Ignored++; continue; }
                Change ch = new Change();
                ch.Sha = Convert.ToString(c["sha"]); ch.Date = date; ch.Title = title; ch.Area = hit.Area; ch.Weight = hit.Weight;
                ch.Url = c.ContainsKey("html_url") ? Convert.ToString(c["html_url"]) : "https://github.com/" + r.Upstream + "/commit/" + ch.Sha;
                r.Changes.Add(ch);
                r.Points += hit.Weight;
                int n;
                r.Areas.TryGetValue(hit.Area, out n);
                r.Areas[hit.Area] = n + 1;
            }
            r.Significant = r.Changes.Exists(delegate(Change x) { return x.Weight >= 3; }) || r.Points >= 8;
            // the files upstream changed that our patches change too: those patches need a look when the engine is updated
            Dictionary<string, List<string>> ours = OurFiles();
            List<object> files = d.ContainsKey("files") ? d["files"] as List<object> : null;
            if (files != null)
                foreach (object o in files)
                {
                    Dictionary<string, object> f = o as Dictionary<string, object>;
                    if (f == null) continue;
                    string name = Convert.ToString(f["filename"]);
                    List<string> which;
                    if (ours.TryGetValue(name, out which)) { which.Sort(string.CompareOrdinal); r.Clashes.Add(name + " (" + string.Join(", ", which.ToArray()) + ")"); }
                }
        }

        // file -> our patch numbers that change it ("0002", "0005")
        static Dictionary<string, List<string>> OurFiles()
        {
            Dictionary<string, List<string>> map = new Dictionary<string, List<string>>();
            string dir = Path.Combine(Path.Combine(Settings.ExeDir, "patches"), "native");
            try
            {
                foreach (string p in Directory.GetFiles(dir, "*.patch"))
                {
                    string number = Path.GetFileName(p).Split('-')[0];
                    foreach (string line in File.ReadAllLines(p))
                    {
                        if (!line.StartsWith("diff --git ")) continue;
                        string[] parts = line.Split(' ');
                        string b = parts[parts.Length - 1];
                        if (b.StartsWith("b/")) b = b.Substring(2);
                        if (b.StartsWith("b/")) b = b.Substring(2);   // a few new files were written as b/b/...
                        List<string> l;
                        if (!map.TryGetValue(b, out l)) map[b] = l = new List<string>();
                        if (!l.Contains(number)) l.Add(number);
                    }
                }
            }
            catch (Exception) { }
            return map;
        }

        // one line for the launcher's notice
        public static string Summary(Report r)
        {
            List<string> top = new List<string>();
            foreach (Rule u in rules) if (u.Weight >= 2 && r.Areas.ContainsKey(u.Area) && top.Count < 3) top.Add(Word(u.Area).ToLowerInvariant());
            return Lang.L("Engine update: ps2-native-recomp has " + r.Ahead + " new changes, " + r.Changes.Count + " can help Raw Danger (" + string.Join(", ", top.ToArray()) + ", ...). Details",
                          "エンジン更新: ps2-native-recomp に " + r.Ahead + " 件の新しい変更、うち " + r.Changes.Count + " 件が Raw Danger に役立つ可能性（" + string.Join("、", top.ToArray()) + "）。 詳細...");
        }

        // the area's first word or two for the one-line notice: "Crashes, hangs and wrong behaviour" -> "Crashes"
        static string Word(string area)
        {
            int p = area.IndexOf(" (");
            string a = p > 0 ? area.Substring(0, p) : area;
            foreach (string cut in new string[] { ",", " and " }) { int q = a.IndexOf(cut); if (q > 0) a = a.Substring(0, q); }
            return a;
        }

        // Started by the main window: the check on a background thread, the notice on the UI thread when it is worth one
        public static void CheckInBackground(Settings s, Control owner, Action<Report> show)
        {
            if (s.EngineCheck == "0") return;
            Thread t = new Thread((ThreadStart)delegate
            {
                Report r = Check(s, false);
                Program.Trace("engine update check: " + (r.Problem ?? (r.Ahead + " new upstream, " + r.Changes.Count + " useful, " + r.Points + " points, significant " + r.Significant + (r.FromCache ? " (saved answer)" : "") + ", head " + r.Head)));
                if (r.Problem != null || !r.Significant || r.Head == "" || r.Head == s.EngineSeen) return;
                try { owner.BeginInvoke((MethodInvoker)delegate { show(r); }); } catch (Exception) { }
            });
            t.IsBackground = true;
            t.Start();
        }
    }

    // The details: what changed upstream, sorted by what it can do for Raw Danger
    public class EngineUpdatesDialog : Form
    {
        public EngineUpdatesDialog(Settings settings, EngineUpdates.Report r)
        {
            Text = Lang.L("Engine updates - ps2-native-recomp", "エンジン更新 - ps2-native-recomp");
            Font = new Font(Lang.UiFont, 9.5f);
            BackColor = Theme.Back; ForeColor = Theme.Text;
            FormBorderStyle = FormBorderStyle.FixedDialog; MaximizeBox = MinimizeBox = false;
            Icon = WizardForm.AppIcon();
            StartPosition = FormStartPosition.CenterParent;
            ClientSize = new Size(900, 600);
            Label head = new Label();
            head.Location = new Point(14, 10); head.Size = new Size(872, 58); head.ForeColor = Theme.Dim;
            head.Text = Lang.L(r.Upstream + " (" + r.Branch + ") is " + r.Ahead + " changes past the engine in this folder (based on " + r.Base.Substring(0, Math.Min(7, r.Base.Length)) + "). "
                               + r.Changes.Count + " can help Raw Danger, " + r.Ignored + " do not concern it (other games, their app, tests). Checked " + r.When.ToString("yyyy-MM-dd HH:mm", CultureInfo.InvariantCulture) + ". Double-click a change to open it on GitHub.",
                               r.Upstream + "（" + r.Branch + "）には、このフォルダのエンジン（" + r.Base.Substring(0, Math.Min(7, r.Base.Length)) + "）以降の変更が " + r.Ahead + " 件あります。Raw Danger に役立つ可能性: " + r.Changes.Count + " 件、関係なし: " + r.Ignored + " 件。確認日時 " + r.When.ToString("yyyy-MM-dd HH:mm", CultureInfo.InvariantCulture) + "。ダブルクリックで GitHub を開きます。");
            Controls.Add(head);
            ListView list = new ListView();
            list.View = View.Details; list.FullRowSelect = true; list.HeaderStyle = ColumnHeaderStyle.Nonclickable;
            list.Location = new Point(14, 72); list.Size = new Size(872, 330);
            list.BackColor = Theme.Card; list.ForeColor = Theme.Text; list.BorderStyle = BorderStyle.None; list.ShowGroups = true;
            list.Columns.Add(Lang.L("Date", "日付"), 90); list.Columns.Add(Lang.L("Change", "変更"), 640); list.Columns.Add("", 120);
            Dictionary<string, ListViewGroup> groups = new Dictionary<string, ListViewGroup>();
            List<EngineUpdates.Change> sorted = new List<EngineUpdates.Change>(r.Changes);
            sorted.Sort(delegate(EngineUpdates.Change a, EngineUpdates.Change b) { int w = b.Weight.CompareTo(a.Weight); return w != 0 ? w : string.CompareOrdinal(b.Date, a.Date); });
            foreach (EngineUpdates.Change c in sorted)
            {
                ListViewGroup g;
                if (!groups.TryGetValue(c.Area, out g))
                {
                    g = new ListViewGroup(c.Area + "  (" + r.Areas[c.Area] + ")");
                    groups[c.Area] = g;
                    list.Groups.Add(g);
                }
                ListViewItem it = new ListViewItem(c.Date);
                it.SubItems.Add(c.Title);
                it.SubItems.Add(c.Weight >= 3 ? Lang.L("can help", "役立つ") : c.Weight == 2 ? Lang.L("likely helps", "たぶん役立つ") : "");
                it.Group = g;
                it.Tag = c;
                it.ToolTipText = c.Area;
                if (c.Weight >= 3) it.ForeColor = Theme.Accent;
                list.Items.Add(it);
            }
            list.DoubleClick += delegate
            {
                if (list.SelectedItems.Count == 0) return;
                try { System.Diagnostics.Process.Start(((EngineUpdates.Change)list.SelectedItems[0].Tag).Url); } catch (Exception) { }
            };
            Controls.Add(list);
            Label how = new Label();
            how.Location = new Point(14, 408); how.Size = new Size(872, 136); how.ForeColor = Theme.Text;
            how.Text = Lang.L("Nothing changes by itself: the engine in this folder is our fork, with Raw Danger's work on top (patches\\native, see engine\\FORK.md). "
                              + "Taking these changes in means a new upstream copy with our patches applied again, built and tested with Raw Danger; that is a project step (rd.py engine-check lists the same report).\n"
                              + (r.Clashes.Count > 0 ? "Files changed on both sides (our patches need a look there): " + string.Join(", ", r.Clashes.ToArray()) : "None of the files our patches change were changed upstream."),
                              "自動では何も変わりません。このフォルダのエンジンは Raw Danger 用の変更を加えたフォーク版です（patches\\native、engine\\FORK.md 参照）。"
                              + "これらを取り込むには、新しい上流のコピーにパッチを当て直し、Raw Danger でビルドとテストを行います（rd.py engine-check でも同じ一覧）。\n"
                              + (r.Clashes.Count > 0 ? "両方で変更されたファイル（パッチの確認が必要）: " + string.Join(", ", r.Clashes.ToArray()) : "パッチが変更するファイルは上流で変更されていません。"));
            Controls.Add(how);
            CheckBox check = new CheckBox();
            check.Text = Lang.L("Check for engine updates when the launcher starts (once a day)", "起動時にエンジン更新を確認（1日1回）");
            check.Location = new Point(14, 556); check.Size = new Size(470, 24); check.ForeColor = Theme.Text;
            check.Checked = settings.EngineCheck != "0";
            check.CheckedChanged += delegate { settings.EngineCheck = check.Checked ? "1" : "0"; settings.Save(); };
            Controls.Add(check);
            Button github = MakeButton(Lang.L("Open on GitHub", "GitHub で開く"), 494, false);
            github.Click += delegate { try { System.Diagnostics.Process.Start("https://github.com/" + r.Upstream + "/compare/" + r.Base + "..." + r.Branch); } catch (Exception) { } };
            Button seen = MakeButton(Lang.L("Hide until new ones", "新しい変更まで非表示"), 650, false);
            seen.Click += delegate { settings.EngineSeen = r.Head; settings.Save(); DialogResult = DialogResult.OK; Close(); };
            Button ok = MakeButton("OK", 806, true);
            ok.Width = 80;
            ok.Click += delegate { Close(); };
            AcceptButton = ok;
        }

        Button MakeButton(string text, int x, bool accent)
        {
            Button b = new Button();
            b.Text = text; b.Size = new Size(accent ? 80 : 148, 32); b.Location = new Point(x, 552);
            b.FlatStyle = FlatStyle.Flat; b.BackColor = accent ? Theme.Accent : Theme.Card; b.ForeColor = accent ? Theme.OnAccent : Theme.Text;
            b.FlatAppearance.BorderColor = Theme.Accent; b.FlatAppearance.BorderSize = accent ? 0 : 1;
            Controls.Add(b);
            return b;
        }
    }

    // A small JSON reader for nested answers (objects -> Dictionary, arrays -> List, numbers -> double, true/false/null); enough for GitHub's answers
    public static class JsonTree
    {
        public static object Parse(string s) { int i = 0; object v = Value(s, ref i); return v; }

        public static int Int(Dictionary<string, object> d, string key)
        {
            object v;
            return d.TryGetValue(key, out v) && v is double ? (int)(double)v : 0;
        }

        static void Ws(string s, ref int i) { while (i < s.Length && char.IsWhiteSpace(s[i])) i++; }

        static object Value(string s, ref int i)
        {
            Ws(s, ref i);
            if (i >= s.Length) throw new FormatException("end of text");
            char c = s[i];
            if (c == '{')
            {
                Dictionary<string, object> d = new Dictionary<string, object>();
                i++; Ws(s, ref i);
                if (s[i] == '}') { i++; return d; }
                while (true)
                {
                    Ws(s, ref i);
                    string k = Str(s, ref i);
                    Ws(s, ref i);
                    if (s[i] != ':') throw new FormatException("':' expected at " + i);
                    i++;
                    d[k] = Value(s, ref i);
                    Ws(s, ref i);
                    if (s[i] == ',') { i++; continue; }
                    if (s[i] == '}') { i++; return d; }
                    throw new FormatException("',' or '}' expected at " + i);
                }
            }
            if (c == '[')
            {
                List<object> l = new List<object>();
                i++; Ws(s, ref i);
                if (s[i] == ']') { i++; return l; }
                while (true)
                {
                    l.Add(Value(s, ref i));
                    Ws(s, ref i);
                    if (s[i] == ',') { i++; continue; }
                    if (s[i] == ']') { i++; return l; }
                    throw new FormatException("',' or ']' expected at " + i);
                }
            }
            if (c == '"') return Str(s, ref i);
            if (s.Length - i >= 4 && string.CompareOrdinal(s, i, "true", 0, 4) == 0) { i += 4; return true; }
            if (s.Length - i >= 5 && string.CompareOrdinal(s, i, "false", 0, 5) == 0) { i += 5; return false; }
            if (s.Length - i >= 4 && string.CompareOrdinal(s, i, "null", 0, 4) == 0) { i += 4; return null; }
            int start = i;
            while (i < s.Length && "+-0123456789.eE".IndexOf(s[i]) >= 0) i++;
            if (i == start) throw new FormatException("unexpected '" + c + "' at " + i);
            return double.Parse(s.Substring(start, i - start), CultureInfo.InvariantCulture);
        }

        static string Str(string s, ref int i)
        {
            if (s[i] != '"') throw new FormatException("string expected at " + i);
            i++;
            StringBuilder sb = new StringBuilder();
            while (s[i] != '"')
            {
                char c = s[i++];
                if (c != '\\') { sb.Append(c); continue; }
                char e = s[i++];
                switch (e)
                {
                    case 'n': sb.Append('\n'); break;
                    case 't': sb.Append('\t'); break;
                    case 'r': sb.Append('\r'); break;
                    case 'b': sb.Append('\b'); break;
                    case 'f': sb.Append('\f'); break;
                    case 'u': sb.Append((char)int.Parse(s.Substring(i, 4), NumberStyles.HexNumber)); i += 4; break;
                    default: sb.Append(e); break;
                }
            }
            i++;
            return sb.ToString();
        }
    }
}
