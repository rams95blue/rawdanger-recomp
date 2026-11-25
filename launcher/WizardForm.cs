// Raw Danger PC - the wizard: pick the disc image(s), pick a few options, it builds them with the ps2-native-recomp engine
// that ships inside this folder (engine\ps2-native-recomp), and they are ready to play. C# 5, WinForms (.NET Framework 4).
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Text;
using System.Text.RegularExpressions;
using System.Windows.Forms;

namespace RawDangerLauncher
{
    public class WizardForm : Form
    {
        // the launcher's icon for its windows and the taskbar, from the copy built in (the same file as the program's own icon)
        public static Icon AppIcon()
        {
            try
            {
                using (Stream st = System.Reflection.Assembly.GetExecutingAssembly().GetManifestResourceStream("RawDangerLauncher.app.ico"))
                    if (st != null) return new Icon(st);
            }
            catch (Exception) { }
            try { return Icon.ExtractAssociatedIcon(Application.ExecutablePath); } catch (Exception) { return null; }
        }

        readonly Settings settings;
        readonly List<DiscInfo> discs = new List<DiscInfo>();
        readonly Queue<BuildJob> queue = new Queue<BuildJob>();
        readonly List<BuildJob> finished = new List<BuildJob>();
        BuildJob current;
        bool stopping;

        // Disaster-warning look: black, hazard-tape yellow, alarm red.
        static Color Back { get { return Theme.Back; } }
        static Color Card { get { return Theme.Card; } }
        static Color Line { get { return Theme.Line; } }
        static Color Text1 { get { return Theme.Text; } }
        static Color Dim { get { return Theme.Dim; } }
        static Color Accent { get { return Theme.Accent; } }
        static Color Alarm { get { return Theme.Alarm; } }
        static Color Bad { get { return Theme.Bad; } }
        static Color Good { get { return Theme.Good; } }
        static readonly string Display = "Impact";

        Panel library, choose, options, building, done;
        ListView libList;
        CheckedListBox discList;
        Label engineNote;
        EngineUpdates.Report engineReport;
        Label libInfo, chooseInfo, chooseTime, optionsInfo, buildTitle, buildStatus, buildTime, doneText, footer;
        CheckBox fixBox, wideBox, filterBox, logosBox, shortcutBox;
        HazardBar bar;
        TextBox log;
        Button doneDetails, chooseBack, libPlay, libReport, libOptions, libAch, libFaster, libRebuild, libShortcut, libFolder, detailsButton, stopButton;
        FlowLayoutPanel doneButtons;
        Timer pump;

        public WizardForm(Settings s)
        {
            settings = s;
            Text = Program.AppName + " " + Program.Version + "  -  rawdanger-recomp";
            Font = new Font(Lang.UiFont, 10f);
            BackColor = Back;
            ForeColor = Text1;
            ClientSize = new Size(1180, 670);
            DoubleBuffered = true;
            FormBorderStyle = FormBorderStyle.FixedSingle;
            MaximizeBox = false;
            StartPosition = FormStartPosition.CenterScreen;
            // the window opens where it was last (launcher.ini Window=x,y), also after a language change; centred when that place
            // is no longer on a screen (another monitor unplugged, a smaller resolution)
            try
            {
                string[] xy = settings.Window.Split(',');
                int wx, wy;
                if (xy.Length == 2 && int.TryParse(xy[0], out wx) && int.TryParse(xy[1], out wy))
                {
                    Rectangle title = new Rectangle(wx + 40, wy + 4, 200, 24);   // the title bar must be reachable
                    foreach (Screen sc in Screen.AllScreens)
                        if (sc.WorkingArea.IntersectsWith(title)) { StartPosition = FormStartPosition.Manual; Location = new Point(wx, wy); break; }
                }
            }
            catch (Exception) { }
            Icon = AppIcon();

            Header header = new Header();
            header.Location = new Point(0, 0);
            header.Size = new Size(1180, 122);
            Controls.Add(header);
            Tape bottom = new Tape();
            bottom.Location = new Point(0, 658);
            bottom.Size = new Size(1180, 12);
            Controls.Add(bottom);
            // Language: English / 日本語 (the window opens again in the other language).
            Button lang = new Button();
            lang.Text = Lang.JP ? "English" : "日本語";
            lang.Size = new Size(96, 30);
            lang.Location = new Point(1180 - 96 - 116, 46 - 6);
            Style(lang, false);
            lang.Font = new Font(Lang.JP ? "Segoe UI" : "Meiryo UI", 9f, FontStyle.Bold);
            lang.Click += delegate
            {
                if (current != null) { MessageBox.Show(Lang.L("Wait for the build to finish first.", "ビルドが終わるまでお待ちください。"), Program.AppName); return; }
                settings.Language = Lang.JP ? "en" : "jp";
                settings.Save();
                Program.Restart = true;
                Close();
            };
            Controls.Add(lang);
            lang.BringToFront();

            // Music: the player's own music file, looped; this button switches it (remembered in launcher.ini)
            Music.Find(settings);
            if (Music.File != "")
            {
                Button music = new Button();
                music.Size = new Size(96, 26);
                music.Location = new Point(1180 - 96 - 116, 80);
                Style(music, false);
                music.Font = new Font(Lang.UiFont, 8.5f, FontStyle.Bold);
                Action label = delegate { music.Text = settings.MusicOn != "0" ? Lang.L("Music: on", "音楽：オン") : Lang.L("Music: off", "音楽：オフ"); };
                label();
                music.Click += delegate
                {
                    settings.MusicOn = settings.MusicOn != "0" ? "0" : "1";
                    settings.Save();
                    if (settings.MusicOn != "0") Music.Play(); else Music.Stop();
                    label();
                };
                Controls.Add(music);
                music.BringToFront();
                // the music's volume: a hazard bar under the button (click or drag; the mouse wheel over it), kept as MusicVolume
                VolumeBar vol = new VolumeBar();
                vol.Location = new Point(1180 - 96 - 116, 108);
                vol.Size = new Size(96, 5);
                vol.Value = Music.Volume;
                vol.Changed += delegate
                {
                    Music.SetVolume(vol.Value);
                    settings.MusicVolume = (vol.Value / 1000.0).ToString("0.00", System.Globalization.CultureInfo.InvariantCulture);
                    settings.Save();
                };
                ToolTip tip = new ToolTip();
                tip.SetToolTip(vol, Lang.L("Music volume (click, drag or use the mouse wheel)", "音楽の音量（クリック、ドラッグ、またはマウスホイール）"));
                Controls.Add(vol);
                vol.BringToFront();
                if (settings.MusicOn != "0") Music.Play();
            }

            // The cast on the right, cycling on every page (also while a build runs).
            List<CastMember> members = CastData.Load(settings);
            if (members.Count > 0)
            {
                CastPanel cast = new CastPanel(members);
                cast.Location = new Point(904, 128);
                cast.Size = new Size(260, 520);
                Controls.Add(cast);
            }
            footer = MakeLabel("", 28, 634, 850, 20, new Font(Lang.UiFont, 8.5f), Dim);
            Controls.Add(footer);
            // engine updates: a notice in the footer's place when ps2-native-recomp has changes that can help (EngineUpdates.cs)
            engineNote = MakeLabel("", 28, 634, 850, 20, new Font(Lang.UiFont, 8.5f, FontStyle.Underline), Theme.Accent);
            engineNote.Cursor = Cursors.Hand;
            engineNote.Visible = false;
            engineNote.Click += delegate
            {
                if (engineReport == null) return;
                using (EngineUpdatesDialog dlg = new EngineUpdatesDialog(settings, engineReport))
                    if (dlg.ShowDialog(this) == DialogResult.OK) { engineNote.Visible = false; footer.Visible = true; }
            };
            Controls.Add(engineNote);
            engineNote.BringToFront();
            Shown += delegate
            {
                EngineUpdates.CheckInBackground(settings, this, delegate(EngineUpdates.Report r)
                {
                    engineReport = r;
                    engineNote.Text = EngineUpdates.Summary(r);
                    engineNote.Visible = true;
                    footer.Visible = false;
                });
            };

            library = MakePage(); choose = MakePage(); options = MakePage(); building = MakePage(); done = MakePage();
            BuildLibrary(); BuildChoose(); BuildOptions(); BuildBuilding(); BuildDone();

            pump = new Timer();
            pump.Interval = 150;
            pump.Tick += delegate { Pump(); };
            pump.Start();

            Shown += delegate { Startup(); };
            FormClosed += delegate { if (!Program.Restart) Music.Close(); };
            FormClosing += delegate
            {
                if (WindowState == FormWindowState.Normal) { settings.Window = Location.X + "," + Location.Y; settings.Save(); }
            };   // a language change rebuilds the window: the music goes on
            FormClosing += delegate(object o, FormClosingEventArgs e)
            {
                if (current != null && MessageBox.Show(Lang.L("A build is running. Stop it and close?", "ビルド中です。中止して閉じますか？"), Program.AppName, MessageBoxButtons.YesNo) != DialogResult.Yes) { e.Cancel = true; return; }
                StopBuild();
            };
        }

        // ---- small helpers -----------------------------------------------------------------------------------------------

        Label MakeLabel(string text, int x, int y, int w, int h, Font f, Color c)
        {
            Label l = new Label();
            l.Text = text; l.Location = new Point(x, y); l.Size = new Size(w, h); l.Font = f; l.ForeColor = c; l.BackColor = Color.Transparent;
            return l;
        }

        Button MakeButton(Control parent, string text, int x, int y, int w, int h, bool primary, EventHandler click)
        {
            Button b = new Button();
            b.Text = text; b.Location = new Point(x, y); b.Size = new Size(w, h);
            Style(b, primary);
            b.Click += click;
            parent.Controls.Add(b);
            return b;
        }

        // a tab or a choice button, chosen (filled) or not (outlined)
        static void Pick(Button b, bool on)
        {
            b.UseVisualStyleBackColor = false;
            b.FlatAppearance.MouseDownBackColor = on ? Alarm : Theme.Pressed;
            b.BackColor = on ? Accent : Back;
            b.ForeColor = on ? Theme.OnAccent : Accent;
            b.FlatAppearance.BorderSize = on ? 0 : 2;
            b.FlatAppearance.MouseOverBackColor = on ? Theme.AccentHover : Theme.Hover;
            b.Invalidate();
        }

        static void Style(Button b, bool primary)
        {
            b.FlatStyle = FlatStyle.Flat;
            b.Cursor = Cursors.Hand;
            b.BackColor = primary ? Accent : Back;
            b.ForeColor = primary ? Theme.OnAccent : Accent;
            b.FlatAppearance.BorderSize = primary ? 0 : 2;
            b.FlatAppearance.BorderColor = Accent;
            b.FlatAppearance.MouseOverBackColor = primary ? Theme.AccentHover : Theme.Hover;
            b.FlatAppearance.MouseDownBackColor = primary ? Alarm : Theme.Pressed;
            b.Font = new Font(Lang.UiFont, 10f, FontStyle.Bold);
            b.EnabledChanged += delegate { b.ForeColor = !b.Enabled ? Theme.Disabled : primary ? Theme.OnAccent : Accent; b.BackColor = !b.Enabled ? Card : primary ? Accent : Back; };
        }

        Panel MakePage()
        {
            Panel p = new Panel();
            p.Location = new Point(0, 128);
            p.Size = new Size(900, 500);
            p.BackColor = Back;
            p.Visible = false;
            Controls.Add(p);
            return p;
        }

        void Go(Panel page)
        {
            foreach (Panel p in new Panel[] { library, choose, options, building, done }) p.Visible = p == page;
            if (page == choose) { chooseBack.Visible = AnyBuilt(); chooseTime.Text = BuildTime.ForList(settings, Ticked()); FillModern(); }
        }

        static Label Heading(Panel p, string text)
        {
            Label l = new Label();
            l.Text = text.ToUpperInvariant(); l.Location = new Point(26, 0); l.Size = new Size(840, 36); l.Font = Lang.JP ? new Font(Lang.UiFont, 15f, FontStyle.Bold) : new Font(Display, 18f); l.ForeColor = Accent;
            p.Controls.Add(l);
            return l;
        }

        // ---- pages ----------------------------------------------------------------------------------------------------

        void BuildLibrary()
        {
            Heading(library, Lang.L("Your games", "ゲーム一覧"));
            libList = new ListView();
            libList.View = View.Details;
            libList.FullRowSelect = true;
            libList.HideSelection = false;
            libList.MultiSelect = false;
            libList.HeaderStyle = ColumnHeaderStyle.Nonclickable;
            libList.BackColor = Card;
            libList.ForeColor = Text1;
            libList.BorderStyle = BorderStyle.None;
            libList.Location = new Point(28, 44);
            libList.Size = new Size(620, 330);
            libList.Columns.Add(Lang.L("Game", "ゲーム"), 330);
            libList.Columns.Add(Lang.L("Version", "バージョン"), 110);
            libList.Columns.Add(Lang.L("Status", "状態"), 160);
            OwnerDraw(libList);
            libList.SelectedIndexChanged += delegate { UpdateLibraryButtons(); };
            libList.DoubleClick += delegate { PlaySelected(); };
            library.Controls.Add(libList);
            libInfo = MakeLabel("", 28, 382, 620, 70, new Font(Lang.UiFont, 9f), Dim);
            library.Controls.Add(libInfo);
            int x = 668;
            libPlay = MakeButton(library, Lang.L("Play", "プレイ"), x, 44, 200, 44, true, delegate { PlaySelected(); });
            libShortcut = MakeButton(library, Lang.L("Desktop shortcut", "デスクトップにショートカットを作成"), x, 100, 200, 34, false, delegate { ShortcutFor(SelectedDisc, true); });
            libFaster = MakeButton(library, Lang.L("Make it faster", "高速化"), x, 142, 200, 34, false, delegate { BuildDiscs(new List<DiscInfo> { SelectedDisc }, true); });
            libRebuild = MakeButton(library, Lang.L("Rebuild", "再ビルド"), x, 184, 200, 34, false, delegate
            {
                if (Enhanced.Is(SelectedDisc)) { Go(choose); ShowTab(true); return; }   // the MODERN tab: its disc list and the build button
                SelectOnly(SelectedDisc); ShowTab(false); FillOptions(); Go(options);
            });
            libFolder = MakeButton(library, Lang.L("Open its folder", "フォルダを開く"), x, 226, 200, 34, false, delegate { OpenFolder(SelectedDisc); });
            libReport = MakeButton(library, Lang.L("Performance report", "パフォーマンスレポート"), x, 268, 200, 34, false, delegate { PerformanceReport(); });
            libAch = MakeButton(library, Lang.L("Achievements", "実績"), x, 352, 200, 34, false, delegate
            {
                DiscInfo d = SelectedDisc;
                if (d == null) return;
                using (AchievementsDialog dlg = new AchievementsDialog(settings, d.GameName, d.Title)) dlg.ShowDialog(this);
            });
            libOptions = MakeButton(library, Lang.L("Game options", "ゲームオプション"), x, 310, 200, 34, false, delegate
            {
                DiscInfo d = SelectedDisc;
                if (d == null) return;
                using (RecompDialog dlg = new RecompDialog(settings, d.GameName, d.Title)) dlg.ShowDialog(this);
            });
            Label tip = MakeLabel(Lang.L("Make it faster: rebuilds the game with the code it ran while you played, so it stutters less.\nEvery run adds a short line to logs\\<game>_performance.log. Performance report: a detailed log of one run.", "高速化：プレイ中に実行されたコードを組み込んで再ビルドし、カクつきを減らします。\n毎回のプレイの要約は logs\\<ゲーム>_performance.log に追記されます。パフォーマンスレポート：1回分の詳しいログ。"), x, 350, 200, 58, new Font(Lang.UiFont, 7.5f), Dim);
            library.Controls.Add(tip);
            MakeButton(library, Lang.L("Build a game  >", "ゲームをビルド  >"), 668, 410, 200, 34, false, delegate { Go(choose); ShowTab(true); });
        }

        // The disc page has two tabs. MODERN (first, recommended): the Enhanced Edition, everything this project adds, built from the
        // four discs. LEGACY: one disc as it is, converted to a PC program with a few options (the way the earlier versions built).
        Panel modernPane, legacyPane;
        Button tabModern, tabLegacy, modernBuild, modernFind, legacyNext;
        Label modernDiscs, modernInfo, modernTime;
        readonly List<Action> speedViews = new List<Action>();

        void BuildChoose()
        {
            tabModern = MakeButton(choose, Lang.L("MODERN  -  Enhanced Edition", "モダン  -  強化版"), 28, 0, 420, 40, true, delegate { ShowTab(true); });
            tabLegacy = MakeButton(choose, Lang.L("LEGACY  -  the PS2 game as it was", "レガシー  -  PS2版そのまま"), 452, 0, 416, 40, false, delegate { ShowTab(false); });
            foreach (Button t in new Button[] { tabModern, tabLegacy }) t.Font = Lang.JP ? new Font(Lang.UiFont, 11f, FontStyle.Bold) : new Font(Display, 13f);
            modernPane = new Panel(); legacyPane = new Panel();
            foreach (Panel pane in new Panel[] { modernPane, legacyPane })
            {
                pane.Location = new Point(0, 46); pane.Size = new Size(900, 404); pane.BackColor = Back;
                choose.Controls.Add(pane);
            }

            // MODERN
            Label title = MakeLabel(Lang.L("RAW DANGER!  ENHANCED EDITION", "RAW DANGER!  強化版"), 26, 6, 560, 36, Lang.JP ? new Font(Lang.UiFont, 16f, FontStyle.Bold) : new Font(Display, 21f), Accent);
            modernPane.Controls.Add(title);
            Label badge = MakeLabel(Lang.L("RECOMMENDED", "おすすめ"), 600, 12, 130, 24, new Font(Lang.UiFont, 9.5f, FontStyle.Bold), Theme.OnAccent);
            badge.BackColor = Alarm; badge.TextAlign = ContentAlignment.MiddleCenter;
            modernPane.Controls.Add(badge);
            modernPane.Controls.Add(MakeLabel(Lang.L(
                "The PC port with everything this project adds, all switched in the game (F1):\n"
                + "   - both voice tracks, English and the original Japanese (48 kHz), shouts and calls included\n"
                + "   - the US or the original Japanese textures, logo and opening film (the US release by default)\n"
                + "   - widescreen, higher resolution, anti-aliasing, the PS2's colour filter on the GPU, ambient occlusion, rain on the lens\n"
                + "   - mouse and keyboard, a wider camera, achievements, fast loading, the debug menus\n"
                + "Built from four discs: your US and Japanese game discs and the two Japanese demos. They are only read.",
                "このプロジェクトのすべての機能を入れたPC版。すべてゲーム内（F1）で切り替えられます：\n"
                + "   - 英語と日本語オリジナル（48 kHz）の両方の音声（叫び声や呼び声も）\n"
                + "   - 北米版または日本版のテクスチャ・ロゴ・オープニング映像（初期設定は北米版）\n"
                + "   - ワイド画面、高解像度、アンチエイリアス、GPUによるPS2の色フィルター、アンビエントオクルージョン、レンズの雨\n"
                + "   - マウスとキーボード、広いカメラ、実績、高速ロード、デバッグメニュー\n"
                + "北米版と日本版のゲームディスク、日本の体験版2枚の計4枚から作ります。ディスクは読み込むだけです。"),
                28, 48, 840, 118, new Font(Lang.UiFont, 9.5f), Text1));
            modernDiscs = MakeLabel("", 28, 172, 520, 88, new Font(Lang.UiFont, 9.5f), Text1);
            modernPane.Controls.Add(modernDiscs);
            modernFind = MakeButton(modernPane, Lang.L("Find the missing discs...", "足りないディスクを探す..."), 568, 176, 300, 34, false, delegate { LocateMissing(); });
            modernBuild = MakeButton(modernPane, Lang.L("BUILD THE ENHANCED EDITION", "強化版をビルド"), 568, 218, 300, 48, true, delegate
            {
                DiscInfo plus = discs.Find(Enhanced.Is);
                if (plus == null || plus.Error != "") return;
                BuildDiscs(new List<DiscInfo> { plus }, false);
            });
            modernBuild.Font = Lang.JP ? new Font(Lang.UiFont, 11f, FontStyle.Bold) : new Font(Display, 12.5f);
            modernTime = MakeLabel("", 568, 270, 300, 34, new Font(Lang.UiFont, 8.5f), Dim);
            modernPane.Controls.Add(modernTime);
            modernInfo = MakeLabel("", 28, 262, 520, 30, new Font(Lang.UiFont, 8.5f), Dim);
            modernPane.Controls.Add(modernInfo);
            SpeedPicker(modernPane, 28, 300);
            MakeButton(modernPane, Lang.L("Build tools...", "ビルドツール..."), 708, 306, 160, 30, false, delegate { using (BuildToolsDialog d = new BuildToolsDialog(settings)) d.ShowDialog(this); FillModern(); });

            // LEGACY
            legacyPane.Controls.Add(MakeLabel(Lang.L("One disc, converted to a PC program as it is, with a few options. For everything this project adds, use MODERN.",
                                                      "1枚のディスクをそのままPC用に変換します（オプション少なめ）。すべての機能を使うには「モダン」を。"), 28, 4, 840, 22, new Font(Lang.UiFont, 9f), Dim));
            discList = new CheckedListBox();
            discList.Location = new Point(28, 30);
            discList.Size = new Size(840, 220);
            discList.BackColor = Card;
            discList.ForeColor = Text1;
            discList.BorderStyle = BorderStyle.None;
            discList.CheckOnClick = true;
            discList.Font = new Font(Lang.UiFont, 10.5f);
            discList.SelectedIndexChanged += delegate { ShowDiscInfo(); };
            legacyPane.Controls.Add(discList);
            chooseInfo = MakeLabel("", 28, 256, 840, 70, new Font(Lang.UiFont, 9f), Dim);
            legacyPane.Controls.Add(chooseInfo);
            MakeButton(legacyPane, Lang.L("Browse for a disc image...", "ディスクイメージを参照..."), 28, 334, 240, 34, false, delegate { BrowseDisc(); });
            chooseTime = MakeLabel("", 284, 342, 584, 22, new Font(Lang.UiFont, 9f), Dim);
            legacyPane.Controls.Add(chooseTime);
            discList.ItemCheck += delegate { BeginInvoke((MethodInvoker)delegate { chooseTime.Text = BuildTime.ForList(settings, Ticked()); }); };

            chooseBack = MakeButton(choose, Lang.L("<  Back", "<  戻る"), 28, 456, 120, 36, false, delegate { RefreshLibrary(); Go(AnyBuilt() ? library : choose); });
            legacyNext = MakeButton(choose, Lang.L("Next  >", "次へ  >"), 748, 456, 120, 36, true, delegate
            {
                if (Ticked().Count == 0) { chooseInfo.Text = Lang.L("Tick at least one disc.", "ディスクを1つ以上選んでください。"); chooseInfo.ForeColor = Bad; return; }
                FillOptions(); Go(options);
            });
            ShowTab(true);
        }

        void ShowTab(bool modern)
        {
            modernPane.Visible = modern; legacyPane.Visible = !modern; legacyNext.Visible = !modern;
            foreach (Button t in new Button[] { tabModern, tabLegacy })
            {
                Pick(t, (t == tabModern) == modern);
            }
            if (modern) FillModern();
        }

        // The four discs the Enhanced Edition is made from, found or not, and the build button.
        void FillModern()
        {
            if (modernDiscs == null) return;
            string[][] want = {
                new string[] { "RawDanger-US", Lang.L("Raw Danger! (USA), SLUS-21501", "Raw Danger!（北米版）SLUS-21501") },
                new string[] { "ZZT2-JP", Lang.L("Zettai Zetsumei Toshi 2 (Japan), SLPS-25606", "絶体絶命都市2（日本版）SLPS-25606") },
                new string[] { "ZZT2-DemoA", Lang.L("Zettai Zetsumei Toshi 2 demo Type-A", "絶体絶命都市2 体験版 Type-A") },
                new string[] { "ZZT2-DemoB", Lang.L("Zettai Zetsumei Toshi 2 demo Type-B", "絶体絶命都市2 体験版 Type-B") } };
            StringBuilder sb = new StringBuilder();
            int found = 0;
            foreach (string[] w in want)
            {
                bool here = discs.Exists(delegate(DiscInfo d) { return d.Error == "" && !d.Modified && d.GameName.StartsWith(w[0], StringComparison.OrdinalIgnoreCase); });
                if (here) found++;
                sb.AppendLine((here ? "\u2714  " : "\u2716  ") + w[1] + (here ? "" : Lang.L("   (missing)", "   （ありません）")));
            }
            modernDiscs.Text = sb.ToString();
            modernDiscs.ForeColor = found == 4 ? Good : Text1;
            DiscInfo plus = discs.Find(Enhanced.Is);
            bool ready = plus != null && plus.Error == "";
            modernBuild.Enabled = ready && current == null;
            modernFind.Visible = !ready;
            modernBuild.Text = ready && Built(plus) ? Lang.L("REBUILD THE ENHANCED EDITION", "強化版を再ビルド") : Lang.L("BUILD THE ENHANCED EDITION", "強化版をビルド");
            modernTime.Text = (ready ? BuildTime.ForList(settings, new List<DiscInfo> { plus }) + "\n" : "") + BuildTools.Note(settings);
            modernInfo.Text = ready ? (Built(plus) ? Lang.L("Built. Rebuilding keeps your saves and settings.", "ビルド済み。再ビルドしてもセーブと設定は残ります。") : "")
                                    : Lang.L(found + " of 4 discs found. Without all four, the LEGACY tab can still build each disc on its own.", "4枚中" + found + "枚が見つかりました。そろわない場合も「レガシー」で1枚ずつビルドできます。");
        }

        // Build speed: three buttons (light, balanced, full) and what the choice does; every copy of it on the pages stays in step.
        void SpeedPicker(Panel parent, int x, int y)
        {
            parent.Controls.Add(MakeLabel(Lang.L("Build speed", "ビルド速度"), x, y + 6, 110, 22, new Font(Lang.UiFont, 9.5f, FontStyle.Bold), Text1));
            Button[] b = new Button[3];
            string[] names = { Lang.L("Light", "軽め"), Lang.L("Balanced", "標準"), Lang.L("Full", "最大") };
            int[] value = { 2, 1, 0 };
            Label note = MakeLabel("", x, y + 38, 840, 40, new Font(Lang.UiFont, 8.5f), Dim);
            parent.Controls.Add(note);
            Action view = delegate
            {
                for (int i = 0; i < 3; i++)
                {
                    Pick(b[i], BuildLoad.Speed(settings) == value[i]);
                }
                note.Text = BuildLoad.Describe(settings);
                if (modernTime != null && modernPane != null && modernPane.Visible) FillModern();
                if (chooseTime != null) chooseTime.Text = BuildTime.ForList(settings, Ticked());
            };
            for (int i = 0; i < 3; i++)
            {
                int v = value[i];
                b[i] = MakeButton(parent, names[i], x + 120 + i * 124, y, 118, 32, false, delegate
                {
                    settings.BuildGentle = v.ToString(); settings.Save();
                    foreach (Action a in speedViews) a();
                });
            }
            speedViews.Add(view);
            view();
        }

        void BuildOptions()
        {
            Heading(options, Lang.L("Options", "オプション"));
            wideBox = new CheckBox(); wideBox.Text = Lang.L("Widescreen 16:9 (the game is drawn for wide monitors; can be switched in the game)", "ワイド画面 16:9（ワイドモニター向けに描画します。ゲーム内でも切替可）"); wideBox.Checked = true;
            filterBox = new CheckBox(); filterBox.Text = Lang.L("PS2 post-processing filter (the console's glow, tint and haze, drawn in one fast GPU pass; can be switched in the game)", "オリジナルPS2ポストプロセスフィルター（本体と同じ光彩・色調・霞。高速なGPU処理。ゲーム内でも切替可）"); filterBox.Checked = true;
            fixBox = new CheckBox(); fixBox.Text = Lang.L("Fast-loading fix (always on: without it, the game hangs on its first loading screen)", "高速ロード修正（常に有効：これがないと最初のロード画面で止まります）"); fixBox.Checked = true; fixBox.AutoCheck = false; fixBox.Cursor = Cursors.No;   // always on, but drawn normally so it stays readable
            logosBox = new CheckBox(); logosBox.Text = Lang.L("Skip the intro logos (Irem, Agetec): straight to the opening movie (can be switched in the game)", "起動時のロゴ（Irem、Agetec）を飛ばしてオープニングムービーへ（ゲーム内でも切替可）"); logosBox.Checked = false;
            shortcutBox = new CheckBox(); shortcutBox.Text = Lang.L("Add a desktop shortcut for each game", "各ゲームのショートカットをデスクトップに作成する"); shortcutBox.Checked = true;
            int y = 48;
            foreach (CheckBox c in new CheckBox[] { fixBox, wideBox, filterBox, logosBox, shortcutBox })
            {
                c.Location = new Point(32, y); c.Size = new Size(820, 30); c.ForeColor = Text1; c.BackColor = Back; c.Font = new Font(Lang.UiFont, 10.5f);
                options.Controls.Add(c);
                y += 38;
            }
            SpeedPicker(options, 32, 240);
            optionsInfo = MakeLabel("", 32, 322, 830, 128, new Font(Lang.UiFont, 9.5f), Dim);
            options.Controls.Add(optionsInfo);
            MakeButton(options, Lang.L("<  Back", "<  戻る"), 28, 456, 120, 36, false, delegate { Go(choose); });
            MakeButton(options, Lang.L("Build tools...", "ビルドツール..."), 168, 456, 160, 36, false, delegate { using (BuildToolsDialog d = new BuildToolsDialog(settings)) d.ShowDialog(this); FillOptions(); });
            MakeButton(options, Lang.L("Build", "ビルド"), 688, 456, 180, 36, true, delegate { BuildDiscs(Ticked(), false); });
        }

        void BuildBuilding()
        {
            buildTitle = Heading(building, Lang.L("Building", "ビルド中"));
            buildStatus = MakeLabel("", 28, 44, 840, 48, new Font(Lang.UiFont, 10.5f), Text1);
            building.Controls.Add(buildStatus);
            bar = new HazardBar();
            bar.Location = new Point(28, 98);
            bar.Size = new Size(840, 18);
            building.Controls.Add(bar);
            buildTime = MakeLabel("", 28, 120, 840, 20, new Font(Lang.UiFont, 9f), Dim);
            building.Controls.Add(buildTime);
            log = new TextBox();
            log.Multiline = true; log.ReadOnly = true; log.ScrollBars = ScrollBars.Vertical; log.WordWrap = false;
            log.Font = new Font("Consolas", 8.5f);
            log.BackColor = Theme.LogBack; log.ForeColor = Theme.LogText;
            log.BorderStyle = BorderStyle.None;
            log.Location = new Point(28, 144);
            log.Size = new Size(840, 296);
            log.Visible = false;
            building.Controls.Add(log);
            detailsButton = MakeButton(building, Lang.L("Show details", "詳細を表示"), 28, 456, 160, 36, false, delegate { log.Visible = !log.Visible; detailsButton.Text = log.Visible ? Lang.L("Hide details", "詳細を隠す") : Lang.L("Show details", "詳細を表示"); });
            stopButton = MakeButton(building, Lang.L("Stop", "中止"), 748, 456, 120, 36, false, delegate { if (MessageBox.Show(Lang.L("Stop the build?", "ビルドを中止しますか？"), Program.AppName, MessageBoxButtons.YesNo) == DialogResult.Yes) StopBuild(); });
        }

        void BuildDone()
        {
            Heading(done, Lang.L("Done", "完了"));
            doneText = MakeLabel("", 28, 40, 840, 66, new Font(Lang.UiFont, 10.5f), Text1);
            done.Controls.Add(doneText);
            doneButtons = new FlowLayoutPanel();
            doneButtons.Location = new Point(28, 110);
            doneButtons.Size = new Size(840, 336);
            doneButtons.AutoScroll = true;
            doneButtons.WrapContents = false;
            doneButtons.FlowDirection = FlowDirection.TopDown;
            doneButtons.BackColor = Back;
            done.Controls.Add(doneButtons);
            doneDetails = MakeButton(done, Lang.L("Show build details", "ビルドの詳細を表示"), 28, 456, 220, 36, false, delegate
            {
                bool show = log.Parent != done || !log.Visible;
                if (show) { log.Parent = done; log.Location = new Point(28, 110); log.Size = new Size(840, 330); log.BringToFront(); }
                log.Visible = show;
                doneButtons.Visible = !show;
                doneDetails.Text = show ? Lang.L("Hide build details", "ビルドの詳細を隠す") : Lang.L("Show build details", "ビルドの詳細を表示");
            });
            MakeButton(done, Lang.L("My games  >", "ゲーム一覧  >"), 718, 456, 150, 36, true, delegate { RefreshLibrary(); Go(library); });
        }

        void OwnerDraw(ListView list)
        {
            list.OwnerDraw = true;
            list.DrawColumnHeader += delegate(object o, DrawListViewColumnHeaderEventArgs e)
            {
                using (SolidBrush b = new SolidBrush(Theme.Header)) e.Graphics.FillRectangle(b, e.Bounds);
                using (Pen pen = new Pen(Accent, 2)) e.Graphics.DrawLine(pen, e.Bounds.Left, e.Bounds.Bottom - 1, e.Bounds.Right, e.Bounds.Bottom - 1);
                TextRenderer.DrawText(e.Graphics, e.Header.Text, Font, new Rectangle(e.Bounds.X + 8, e.Bounds.Y, e.Bounds.Width - 8, e.Bounds.Height), Dim,
                                      TextFormatFlags.VerticalCenter | TextFormatFlags.Left | TextFormatFlags.EndEllipsis);
            };
            list.DrawSubItem += delegate(object o, DrawListViewSubItemEventArgs e)
            {
                bool sel = e.Item.Selected;
                using (SolidBrush b = new SolidBrush(sel ? Accent : Card)) e.Graphics.FillRectangle(b, e.Bounds);
                if (sel && e.ColumnIndex == 0) using (SolidBrush r = new SolidBrush(Alarm)) e.Graphics.FillRectangle(r, e.Bounds.X, e.Bounds.Y, 5, e.Bounds.Height);
                Color c = sel ? Color.Black : e.ColumnIndex == 2 ? e.Item.ForeColor : Text1;
                TextRenderer.DrawText(e.Graphics, e.SubItem.Text, Font, new Rectangle(e.Bounds.X + 8, e.Bounds.Y, e.Bounds.Width - 8, e.Bounds.Height), c,
                                      TextFormatFlags.VerticalCenter | TextFormatFlags.Left | TextFormatFlags.EndEllipsis);
            };
            list.SelectedIndexChanged += delegate { list.Invalidate(); };
        }

        // ---- data ---------------------------------------------------------------------------------------------------------

        void Startup()
        {
            footer.Text = settings.PnrDir == "" ? Lang.L("Engine not found: keep the engine\\ps2-native-recomp folder next to this program.", "エンジンが見つかりません：engine\\ps2-native-recomp フォルダをこのプログラムと同じ場所に置いてください。") : Lang.L("Engine: ", "エンジン: ") + settings.PnrDir;
            footer.ForeColor = settings.PnrDir == "" ? Bad : Dim;
            Scan();
            List<string> modded = new List<string>();
            foreach (DiscInfo d in discs) if (d.Modified) modded.Add(Path.GetFileName(d.ImagePath));
            if (modded.Count > 0)
                MessageBox.Show(Lang.L("These disc images are modified (an undub: Japanese voices put into the US disc) and are not compatible:\n\n",
                                       "次のディスクイメージは改造版（北米版に日本語音声を入れた音声差し替え版）のため非対応です：\n\n")
                    + string.Join("\n", modded.ToArray())
                    + Lang.L("\n\nUse the retail US disc. For Japanese voices, build the Enhanced Edition: it makes them from your retail US and JP discs, and lets you switch the voices in the game.",
                             "\n\n市販の北米版ディスクを使ってください。日本語音声は強化版で：市販の北米版と日本版から自動で作り、ゲーム中に音声を切り替えられます。"),
                    Program.AppName, MessageBoxButtons.OK, MessageBoxIcon.Warning);
            RefreshLibrary();
            Go(AnyBuilt() ? library : choose);
        }

        void Scan()
        {
            discs.Clear();
            List<string> images = new List<string>();
            foreach (string folder in settings.GameFolders.Split(';'))
            {
                string f = folder.Trim();
                if (f == "" || !Directory.Exists(f)) continue;
                string[] files = new string[0];
                try { files = Directory.GetFiles(f, "*.*", SearchOption.AllDirectories); } catch (Exception e) { Program.Trace("scan " + f + ": " + e.Message); }
                foreach (string file in files)
                {
                    string ext = Path.GetExtension(file).ToLowerInvariant();
                    if (ext == ".iso" || ext == ".cue" || (ext == ".bin" && !File.Exists(Path.ChangeExtension(file, ".cue")))) images.Add(file);
                }
            }
            foreach (string extra in settings.ExtraImages()) if (File.Exists(extra) && !images.Contains(extra)) images.Add(extra);
            images.Sort(StringComparer.OrdinalIgnoreCase);
            foreach (string image in images) AddDisc(image);
            // The Enhanced Edition needs all four discs: when one is still missing, look for it near this program too (its folder and
            // the folder above, four levels down: GAME US, GAME JP\DEMO ...), and remember the folders where discs were found.
            if (Enhanced.Missing(discs).Count > 0)
            {
                List<string> near = new List<string>();
                string up = Path.GetDirectoryName(Settings.ExeDir.TrimEnd('\\', '/'));
                foreach (string root in new string[] { Settings.ExeDir, up }) if (root != null) NearImages(root, 4, near);
                bool added = false;
                foreach (string image in near)
                {
                    if (images.Exists(delegate(string x) { return string.Equals(x, image, StringComparison.OrdinalIgnoreCase); })) continue;
                    images.Add(image);
                    DiscInfo d = AddDisc(image);
                    if (d.Error != "" || d.Modified) continue;
                    string folder = Path.GetDirectoryName(image);
                    if (string.Equals(Path.GetFileName(folder), "DEMO", StringComparison.OrdinalIgnoreCase)) folder = Path.GetDirectoryName(folder);
                    bool known = false;
                    foreach (string g in settings.GameFolders.Split(';')) if (string.Equals(g.Trim().TrimEnd('\\'), folder.TrimEnd('\\'), StringComparison.OrdinalIgnoreCase)) known = true;
                    if (!known) { settings.GameFolders = (settings.GameFolders.Trim(';') + ";" + folder).Trim(';'); added = true; }
                }
                if (added) settings.Save();
                Program.Trace("enhanced: looked for the missing discs near the program; still missing: " + string.Join(", ", Enhanced.Missing(discs).ToArray()));
            }
            DiscInfo plus = Enhanced.Make(discs, settings);
            if (plus != null) discs.Add(plus);
            FillDiscList();
        }

        // Disc images under a folder (not in the engine's own folders, which hold the built games and their disc copies)
        static void NearImages(string dir, int depth, List<string> found)
        {
            string name = Path.GetFileName(dir.TrimEnd('\\', '/'));
            if (name == "engine" || name == "games" || name == "build" || name == "generated" || name == "work" || name.StartsWith(".") || name.StartsWith("$")) return;
            try
            {
                foreach (string file in Directory.GetFiles(dir))
                {
                    string ext = Path.GetExtension(file).ToLowerInvariant();
                    if (ext == ".iso" || ext == ".cue" || (ext == ".bin" && !File.Exists(Path.ChangeExtension(file, ".cue")))) found.Add(file);
                }
                if (depth > 0) foreach (string sub in Directory.GetDirectories(dir)) NearImages(sub, depth - 1, found);
            }
            catch (Exception) { }
        }

        DiscInfo AddDisc(string image)
        {
            foreach (DiscInfo known in discs) if (string.Equals(known.ImagePath, image, StringComparison.OrdinalIgnoreCase)) return known;
            DiscInfo d = Disc.Identify(image);
            if (d.Error == "")
            {
                Catalog.Name(d, settings);
                string baseName = d.GameName;
                for (int n = 2; discs.Exists(delegate(DiscInfo o) { return string.Equals(o.GameName, d.GameName, StringComparison.OrdinalIgnoreCase); }) && n < 100; n++)
                    d.GameName = baseName + "-" + n;
            }
            else d.Title = Path.GetFileName(image);
            Program.Trace("disc " + image + " -> " + (d.Error == "" ? d.Serial + " " + d.CrcText + " games\\" + d.GameName + " pnach " + d.PnachPath : "error " + d.Error));
            discs.Add(d);
            return d;
        }

        void FillDiscList()
        {
            discList.Items.Clear();
            foreach (DiscInfo d in discs)
            {
                if (Enhanced.Is(d)) continue;   // on the MODERN tab
                string line = d.Title + "   -   " + (Enhanced.Is(d) ? (d.Error != "" ? d.Error : Lang.L("from all four discs", "4枚すべてのディスクから") + (Built(d) ? Lang.L(", built", "、ビルド済み") : ""))
                    : d.Modified ? Lang.L("NOT COMPATIBLE: modified disc (undub?)", "非対応：改造ディスク（音声差し替え版？）")
                    : d.Error != "" ? Lang.L("cannot be read", "読み込めません") : d.Region + ", " + d.Serial + (Built(d) ? Lang.L(", built", "、ビルド済み") : ""));
                discList.Items.Add(new DiscItem(d, line), false);
            }
            FillModern();
            if (discs.Count == 0) chooseInfo.Text = Lang.L("No disc images were found near this program. Use 'Browse for a disc image...' (.iso, or a .bin with its .cue).", "ディスクイメージが見つかりませんでした。「ディスクイメージを参照...」から選んでください（.iso、または .cue 付きの .bin）。");
        }

        class DiscItem
        {
            public DiscInfo Disc; public string Text;
            public DiscItem(DiscInfo d, string t) { Disc = d; Text = t; }
            public override string ToString() { return Text; }
        }

        List<DiscInfo> Ticked()
        {
            List<DiscInfo> list = new List<DiscInfo>();
            foreach (object o in discList.CheckedItems) { DiscInfo d = ((DiscItem)o).Disc; if (d.Error == "") list.Add(d); }
            return list;
        }

        void SelectOnly(DiscInfo d)
        {
            for (int i = 0; i < discList.Items.Count; i++) discList.SetItemChecked(i, ((DiscItem)discList.Items[i]).Disc == d);
            FillOptions();
        }

        void ShowDiscInfo()
        {
            if (discList.SelectedItem == null) return;
            DiscInfo d = ((DiscItem)discList.SelectedItem).Disc;
            chooseInfo.ForeColor = d.Error != "" ? Bad : Dim;
            chooseInfo.Text = Enhanced.Is(d) ? Enhanced.Describe(d)
                : d.Error != "" ? d.ImagePath + "\n" + d.Error
                : d.ImagePath + Lang.L("\nBoot program ", "\n起動プログラム ") + d.BootProgram + ", CRC " + d.CrcText + "  ->  games\\" + d.GameName + "\n"
                  + (d.PnachPath != "" ? Lang.L("Patches: ", "パッチ: ") + string.Join(", ", d.Sections.ToArray()) : Lang.L("There is no patch file for this disc yet, so it will be built unpatched.", "このディスク用のパッチファイルはまだないため、パッチなしでビルドします。"));
        }

        bool locating;

        // Asks for each disc the Enhanced Edition still misses, then offers it again (ticked when all four are there).
        void LocateMissing()
        {
            locating = true;
            try
            {
                foreach (string want in Enhanced.Missing(discs))
                {
                    using (OpenFileDialog f = new OpenFileDialog())
                    {
                        f.Title = Lang.L("Enhanced Edition needs: ", "強化版に必要なディスク: ") + want;
                        f.Filter = Lang.L("PS2 disc images (*.iso;*.cue;*.bin)|*.iso;*.cue;*.bin|All files (*.*)|*.*", "PS2 ディスクイメージ (*.iso;*.cue;*.bin)|*.iso;*.cue;*.bin|すべてのファイル (*.*)|*.*");
                        if (f.ShowDialog(this) != DialogResult.OK) break;
                        DiscInfo d = AddDisc(f.FileName);
                        if (d.Error == "") settings.AddExtraImage(f.FileName);
                        else MessageBox.Show(f.FileName + "\n" + d.Error, Program.AppName);
                    }
                }
                discs.RemoveAll(delegate(DiscInfo x) { return Enhanced.Is(x); });
                DiscInfo plus = Enhanced.Make(discs, settings);
                discs.Add(plus);
                FillDiscList();
                ShowTab(true);
            }
            finally { locating = false; }
        }

        void BrowseDisc()
        {
            using (OpenFileDialog f = new OpenFileDialog())
            {
                f.Title = Lang.L("Choose a Raw Danger! / Zettai Zetsumei Toshi 2 disc image", "Raw Danger! / 絶体絶命都市2 のディスクイメージを選択");
                f.Filter = Lang.L("PS2 disc images (*.iso;*.cue;*.bin)|*.iso;*.cue;*.bin|All files (*.*)|*.*", "PS2 ディスクイメージ (*.iso;*.cue;*.bin)|*.iso;*.cue;*.bin|すべてのファイル (*.*)|*.*");
                if (f.ShowDialog(this) != DialogResult.OK) return;
                DiscInfo d = AddDisc(f.FileName);
                settings.AddExtraImage(f.FileName);
                FillDiscList();
                for (int i = 0; i < discList.Items.Count; i++) if (((DiscItem)discList.Items[i]).Disc == d) { discList.SetItemChecked(i, d.Error == ""); discList.SelectedIndex = i; }
            }
        }

        bool Built(DiscInfo d) { return d.Error == "" && settings.PnrDir != "" && Games.IsBuilt(settings, d.GameName); }
        bool AnyBuilt() { foreach (DiscInfo d in discs) if (Built(d)) return true; return false; }

        void RefreshLibrary()
        {
            libList.Items.Clear();
            foreach (DiscInfo d in discs)
            {
                if (!Built(d)) continue;
                ListViewItem item = new ListViewItem(d.Title);
                item.SubItems.Add(d.Region);
                string built = Games.Get(Games.ReadApp(settings, d.GameName), "built");
                item.SubItems.Add(Lang.L("built ", "ビルド済み ") + (built.Length >= 10 ? built.Substring(0, 10) : built));
                item.ForeColor = Good;
                item.Tag = d;
                libList.Items.Add(item);
            }
            if (libList.Items.Count > 0) libList.Items[0].Selected = true;
            UpdateLibraryButtons();
        }

        DiscInfo SelectedDisc { get { return libList.SelectedItems.Count == 0 ? null : (DiscInfo)libList.SelectedItems[0].Tag; } }

        void UpdateLibraryButtons()
        {
            bool on = SelectedDisc != null && current == null;
            foreach (Button b in new Button[] { libPlay, libReport, libOptions, libAch, libShortcut, libFaster, libRebuild, libFolder }) b.Enabled = on;
            DiscInfo sel = SelectedDisc;
            libInfo.Text = sel == null ? "" : PatchNames.List(PatchNames.Built(settings, sel.GameName), Enhanced.Is(sel))
                + "\n" + Lang.L("Folder: ", "フォルダ: ") + "games\\" + sel.GameName;
        }

        void FillOptions()
        {
            List<DiscInfo> list = Ticked();
            StringBuilder sb = new StringBuilder();
            sb.AppendLine(Lang.L("To build:", "ビルドするもの:"));
            foreach (DiscInfo d in list)
                sb.AppendLine("   " + d.Title + "  ->  games\\" + d.GameName + (d.PnachPath == "" ? Lang.L("   (no patch file yet)", "   （パッチファイルなし）") : "") + (Built(d) ? Lang.L("   (already built: will be rebuilt)", "   （ビルド済み：再ビルドします）") : ""));
            if (list.Exists(Enhanced.Is))
                sb.AppendLine(Lang.L("The Enhanced Edition has no build options: everything is built in, and you switch it in the game (F1 -> Game options, or F3) or with Game options here.",
                                     "強化版は上のオプションを使いません。すべての機能が組み込まれ、ゲーム内（F1 →「Game options」、または F3）またはここの「ゲームオプション」で切り替えます。"));
            sb.AppendLine();
            sb.AppendLine(ToolsNote());
            sb.AppendLine(BuildTime.ForList(settings, list));
            sb.AppendLine(Lang.L("The game reads your .iso where it is (the copy made while building is removed afterwards); a .cue/.bin disc is converted to an .iso once and kept. The Enhanced Edition keeps its own disc (about 3 GB).",
                                 "ゲームは .iso を元の場所から読み込みます（ビルド中のコピーは後で削除）。.cue/.bin は一度 .iso に変換して保存します。強化版は専用ディスク（約3 GB）を保存します。"));
            optionsInfo.Text = sb.ToString();
        }

        string ToolsNote() { return BuildTools.Note(settings); }

        // ---- play, shortcut, folder ----------------------------------------------------------------------------------------

        void PlaySelected()
        {
            DiscInfo d = SelectedDisc;
            if (d == null) return;
            bool known;
            if (Enhanced.Is(d) && Enhanced.WorkingDisc(settings) && !Enhanced.HasTextures(settings) && settings.TexturesAsked != "1")
            {
                // a disc from v0.6.4-0.6.6: it plays, only the Japanese texture options are missing. Asked once.
                settings.TexturesAsked = "1"; settings.Save();
                DialogResult t = MessageBox.Show(Lang.L("New for the Enhanced Edition: the Japanese release's textures (the characters' original faces and hair, the writing in Japanese), switched in the game. They need the disc made again from your US and JP images (" + BuildTime.Minutes(BuildTime.Estimate(settings, d.GameName, true, out known)) + "). Do it now? (No: play now; the Build page can do it later)",
                                                        "強化版の新機能：日本版のテクスチャ（キャラクター本来の顔と髪、日本語の文字）をゲーム内で切り替えられます。北米版と日本版のイメージからディスクを作り直す必要があります（" + BuildTime.Minutes(BuildTime.Estimate(settings, d.GameName, true, out known)) + "）。今すぐ行いますか？（いいえ：このままプレイ。後でビルド画面から可能）"),
                                                 Program.AppName, MessageBoxButtons.YesNoCancel, MessageBoxIcon.Question);
                if (t == DialogResult.Cancel) return;
                if (t == DialogResult.Yes) { BuildDiscs(new List<DiscInfo>(new DiscInfo[] { d }), true); return; }
            }
            else if (Enhanced.Is(d) && !Enhanced.WorkingDisc(settings))
            {
                // an Enhanced Edition disc from before v0.6.4: its voice index lacks two header words the game needs, so it finds
                // no voice line at all (English or Japanese) and waits for each one
                DialogResult r = MessageBox.Show(Lang.L("This Enhanced Edition was built with an older disc on which no voice line plays (English or Japanese): the game waits for them and gets stuck. Rebuild it now? (Yes: rebuild, " + BuildTime.Minutes(BuildTime.Estimate(settings, d.GameName, false, out known)) + "; No: play anyway)",
                                                        "この強化版は古いディスクでビルドされており、音声（英語・日本語とも）が再生されず、ゲームが止まります。今すぐ再ビルドしますか？（はい：再ビルド、" + BuildTime.Minutes(BuildTime.Estimate(settings, d.GameName, false, out known)) + "　いいえ：このままプレイ）"),
                                                 Program.AppName, MessageBoxButtons.YesNoCancel, MessageBoxIcon.Warning);
                if (r == DialogResult.Cancel) return;
                if (r == DialogResult.Yes) { BuildDiscs(new List<DiscInfo>(new DiscInfo[] { d }), false); return; }
            }
            string problem = Games.Play(settings, d.GameName);
            if (problem != null) MessageBox.Show(problem, Program.AppName);
            else Music.Stop();   // the game has its own sound
        }

        // The engine's own measuring: frames that took longer than 25 ms and what they waited for, and GPU statistics,
        // all written to games\<name>\logs\ps2run_<date>.log.
        void PerformanceReport()
        {
            DiscInfo d = SelectedDisc;
            if (d == null) return;
            Dictionary<string, string> env = new Dictionary<string, string>();
            env["PS2_HITCH_LOG"] = "25";
            env["PS2_GPU_STAT"] = "1";
            string problem = Games.Play(settings, d.GameName, env);
            if (problem != null) { MessageBox.Show(problem, Program.AppName); return; }
            MessageBox.Show(Lang.L("The game is running with performance logging on. Play the slow parts for a few minutes, close the game, then send the newest file from:\n", "パフォーマンスログを有効にしてゲームを起動しました。重い場面を数分プレイしてからゲームを閉じ、次のフォルダ内の最新ファイルを送ってください:\n")
                + Path.Combine(Games.Folder(settings, d.GameName), "logs"), Program.AppName);
        }

        void ShortcutFor(DiscInfo d, bool tell)
        {
            if (d == null) return;
            string desktop = Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory);
            string lnk = Path.Combine(desktop, d.Title.Replace(":", "").Replace("!", "").Replace("?", "") + ".lnk");
            try
            {
                string icon = Icons.ForGame(settings, d.GameName);
                Shortcut.Create(lnk, Application.ExecutablePath, "--play " + d.GameName, Settings.ExeDir, icon != "" ? icon : Application.ExecutablePath, d.Title + "  -  rawdanger-recomp");
                if (tell) MessageBox.Show(Lang.L("Desktop shortcut created: ", "デスクトップにショートカットを作成しました: ") + Path.GetFileName(lnk), Program.AppName);
            }
            catch (Exception e) { Program.Trace("shortcut: " + e); if (tell) MessageBox.Show(Lang.L("Could not create the shortcut: ", "ショートカットを作成できませんでした: ") + e.Message, Program.AppName); }
        }

        void OpenFolder(DiscInfo d)
        {
            if (d == null) return;
            string folder = Games.Folder(settings, d.GameName);
            if (Directory.Exists(folder)) Process.Start("explorer.exe", "\"" + folder + "\"");
        }

        // ---- building ------------------------------------------------------------------------------------------------------

        class BuildJob
        {
            public DiscInfo Disc;
            public bool Faster;
            public bool Shortcut;
            public List<string> Sections = new List<string>();
            public Process Process;
            public Stopwatch Clock;
            public int Estimate;          // seconds, see BuildTime
            public bool EstimateKnown;
            public StreamWriter LogFile;
            public readonly Queue<string> Lines = new Queue<string>();
            public bool Wide, Done;
            public string Image = "";     // the disc image built from: the disc itself, or the Enhanced Edition's hybrid disc
            public int Step, ExitCode;
            public string Problem;
            public string TempDir = "";   // v0.9.9: the build's own temp folder (TEMP / TMP / TMPDIR), deleted when it ends
        }

        // v0.9.9: the build's temporary files (the compilers', CMake's, the shell's) go to a folder of its own under %TEMP%, removed
        // when the build ends, so a build leaves nothing in %TEMP%; folders left by a build that was cut off (the PC turned off) are
        // removed at the next start.
        const string BuildTempPrefix = "RawDangerPC-build-";
        static string MakeBuildTemp()
        {
            string dir = Path.Combine(Path.GetTempPath(), BuildTempPrefix + Process.GetCurrentProcess().Id + "-" + DateTime.Now.ToString("yyyyMMddHHmmss"));
            Directory.CreateDirectory(dir);
            return dir;
        }
        static long RemoveBuildTemp(string dir)
        {
            if (string.IsNullOrEmpty(dir) || !Directory.Exists(dir)) return 0;
            long bytes = 0;
            try { foreach (FileInfo f in new DirectoryInfo(dir).GetFiles("*", SearchOption.AllDirectories)) bytes += f.Length; } catch (Exception) { }
            for (int attempt = 0; attempt < 5; ++attempt)
            {
                try
                {
                    foreach (string f in Directory.GetFiles(dir, "*", SearchOption.AllDirectories)) try { File.SetAttributes(f, FileAttributes.Normal); } catch (Exception) { }
                    Directory.Delete(dir, true);
                    return bytes;
                }
                catch (Exception) { System.Threading.Thread.Sleep(400); }   // a compiler still closing a file
            }
            Program.Trace("build temp: could not remove " + dir);
            return 0;
        }
        public static void RemoveOldBuildTemps()
        {
            try
            {
                foreach (string dir in Directory.GetDirectories(Path.GetTempPath(), BuildTempPrefix + "*"))
                {
                    // the folder of a launcher still running (its process id in the name) is left alone
                    string rest = Path.GetFileName(dir).Substring(BuildTempPrefix.Length);
                    int dash = rest.IndexOf('-'), pid;
                    if (dash > 0 && int.TryParse(rest.Substring(0, dash), out pid) && pid != Process.GetCurrentProcess().Id)
                    {
                        bool alive = false;
                        try { alive = !Process.GetProcessById(pid).HasExited && Process.GetProcessById(pid).ProcessName.StartsWith("RawDangerPC", StringComparison.OrdinalIgnoreCase); } catch (Exception) { }
                        if (alive) continue;
                    }
                    long freed = RemoveBuildTemp(dir);
                    if (freed > 0) Program.Trace("build temp: removed " + dir + " (" + (freed >> 20) + " MB) left by an earlier build");
                }
            }
            catch (Exception) { }
        }

        List<string> SectionsFor(DiscInfo d)
        {
            List<string> list = new List<string>();
            foreach (string s in d.Sections)
            {
                if (Enhanced.Is(d)) { list.Add(s); continue; }   // every feature built in; the switchable ones start as the patch file says and change in the game
                if (s.IndexOf("fix", StringComparison.OrdinalIgnoreCase) >= 0) list.Add(s);
                else if (s.StartsWith("Direct mouse look", StringComparison.OrdinalIgnoreCase)) list.Add(s);   // always built in: the mouse turns the camera itself (v0.7.9)
                else if (s.StartsWith("Free camera tilt", StringComparison.OrdinalIgnoreCase)) list.Add(s);   // a switch, on by default (v0.8.6)
                else if (s.StartsWith("Orbit camera", StringComparison.OrdinalIgnoreCase)) list.Add(s);   // built in: up / down circles the hero, a setting in the game (v0.9.0)
                else if (s.StartsWith("Free camera everywhere", StringComparison.OrdinalIgnoreCase)) list.Add(s);   // built in: a setting in the game, off by default (v0.9.0)
                else if (s.StartsWith("Head follows camera", StringComparison.OrdinalIgnoreCase)) list.Add(s);   // built in: a setting in the game, on by default (v0.9.4)
                else if (s.StartsWith("Debug menu", StringComparison.OrdinalIgnoreCase)) list.Add(s);   // built in: the developers' menu answers Cross (v0.8.6)
                else if (s.StartsWith("Widescreen", StringComparison.OrdinalIgnoreCase) && (wideBox.Checked || Enhanced.Is(d))) list.Add(s);
                else if (s.IndexOf("post", StringComparison.OrdinalIgnoreCase) >= 0) list.Add(s);   // always built in: a switch (Original PS2 Post Processing Filter, on by default)
                else if (s.StartsWith("Skip intro logos", StringComparison.OrdinalIgnoreCase) && (logosBox.Checked || Enhanced.Is(d))) list.Add(s);
            }
            return list;
        }

        void BuildDiscs(List<DiscInfo> list, bool faster)
        {
            if (settings.PnrDir == "") { MessageBox.Show(Lang.L("The engine folder (engine\\ps2-native-recomp) is missing next to this program.", "エンジンフォルダ（engine\\ps2-native-recomp）がこのプログラムの隣にありません。"), Program.AppName); return; }
            list.RemoveAll(delegate(DiscInfo d) { return d == null || d.Error != ""; });
            if (list.Count == 0) return;
            int gathered = BuildTools.Gather(settings);   // build tools saved elsewhere on this PC: no download for those
            if (gathered > 0) Program.Trace("build tools: copied " + gathered + " saved file(s) into the engine's downloads");
            finished.Clear();
            stopping = false;
            log.Clear();
            foreach (DiscInfo d in list)
            {
                BuildJob job = new BuildJob();
                job.Disc = d;
                job.Faster = faster;
                job.Shortcut = !faster && shortcutBox.Checked;
                job.Sections = faster ? new List<string>() : SectionsFor(d);
                job.Image = d.ImagePath;
                if (Enhanced.Is(d) && Enhanced.Japanese(discs) != null)
                    job.Image = Path.Combine(Games.Folder(settings, d.GameName), "hybrid.iso");
                queue.Enqueue(job);
            }
            Go(building);
            StartNext();
        }

        static string Posix(string path) { return path.Replace('\\', '/'); }
        static string Ps(string s) { return "'" + s.Replace("'", "''") + "'"; }

        // The ps2-native-recomp app's stages as one PowerShell script: the portable tools (copied from another
        // ps2-native-recomp folder when one is next to us, else downloaded), env.ps1, setup.sh, port.sh.
        string WriteScript(BuildJob job)
        {
            string pnr = settings.PnrDir;
            string tools = Path.Combine(pnr, "tools", "portable");
            string bash = Path.Combine(tools, "mingit", "usr", "bin", "bash.exe");
            DiscInfo d = job.Disc;
            StringBuilder s = new StringBuilder();
            if (BuildLoad.Gentle(settings))   // first, so every program the build starts (bash, cmake, ninja, clang) inherits it
                s.AppendLine("try { (Get-Process -Id $PID).PriorityClass = '" + (BuildLoad.Speed(settings) == 2 ? "Idle" : "BelowNormal") + "' } catch { }");
            s.AppendLine("$ErrorActionPreference = 'Continue'");
            s.AppendLine("$ProgressPreference = 'SilentlyContinue'");
            s.AppendLine("Set-Location -LiteralPath " + Ps(pnr));
            s.AppendLine("if (-not (Test-Path -LiteralPath " + Ps(Path.Combine(tools, "env.ps1")) + ") -or -not (Test-Path -LiteralPath " + Ps(bash) + ")) {");
            string other = settings.OtherEngine();
            if (other != "")
            {
                s.AppendLine("  Write-Output '== launcher: copying the build tools from " + other.Replace("'", "''") + "'");
                s.AppendLine("  & robocopy.exe " + Ps(Path.Combine(other, "tools", "portable")) + " " + Ps(tools) + " /E /NFL /NDL /NJH /NJS /NP | Out-Null");
            }
            s.AppendLine("  & powershell.exe -NoProfile -ExecutionPolicy Bypass -File " + Ps(Path.Combine(pnr, "scripts", "portable.ps1")) + " -ForApp 2>&1 | ForEach-Object { \"$_\" }");
            s.AppendLine("  if ($LASTEXITCODE) { exit $LASTEXITCODE }");
            s.AppendLine("}");
            s.AppendLine(". " + Ps(Path.Combine(tools, "env.ps1")) + " -Quiet -Keep PS2_JOBS,PS2_SCALE");
            if (other != "")
                foreach (string lib in new string[] { "wgpu-native", "sdl2", "dxc" })
                {
                    string from = Path.Combine(other, "third_party", lib), to = Path.Combine(pnr, "third_party", lib);
                    s.AppendLine("if ((Test-Path -LiteralPath " + Ps(from) + ") -and -not (Test-Path -LiteralPath " + Ps(to) + ")) { Write-Output '== launcher: copying " + lib + "'; & robocopy.exe " + Ps(from) + " " + Ps(to) + " /E /NFL /NDL /NJH /NJS /NP | Out-Null }");
                }
            s.AppendLine("$need = $false");
            foreach (string f in new string[] { "build/tools/eerecomp.exe", "build/tools/vurecomp.exe", "third_party/wgpu-native/lib/libwgpu_native.a", "third_party/sdl2/lib/libSDL2.dll.a" })
                s.AppendLine("if (-not (Test-Path -LiteralPath " + Ps(Path.Combine(pnr, f.Replace('/', Path.DirectorySeparatorChar))) + ")) { $need = $true }");
            s.AppendLine("if ($need) {");
            s.AppendLine("  Write-Output '== launcher: preparing the translator tools (setup.sh)'");
            s.AppendLine("  & " + Ps(bash) + " " + Ps(Posix(Path.Combine(pnr, "scripts", "setup.sh"))) + " --native-only 2>&1 | ForEach-Object { \"$_\" }");
            s.AppendLine("  if ($LASTEXITCODE) { exit $LASTEXITCODE }");
            s.AppendLine("}");
            StringBuilder port = new StringBuilder();
            port.Append("& ").Append(Ps(bash)).Append(' ').Append(Ps(Posix(Path.Combine(pnr, "scripts", "port.sh")))).Append(' ').Append(Ps(d.GameName)).Append(' ')
                .Append(Ps(Posix(job.Faster ? Games.Resolve(settings, Games.Get(Games.ReadApp(settings, d.GameName), "iso")) : job.Image))).Append(" --native-only");
            string pnach = d.PnachPath;
            List<string> sections = job.Sections;
            if (job.Faster)
            {
                // Make it faster: the same patches as the build (port.sh applies them again).
                Dictionary<string, string> app = Games.ReadApp(settings, d.GameName);
                pnach = Games.Get(app, "pnach");
                sections = new List<string>(Games.Get(app, "sections").Split(new char[] { '|' }, StringSplitOptions.RemoveEmptyEntries));
                job.Sections = sections;
            }
            if (pnach != "" && sections.Count > 0)
            {
                port.Append(" --pnach ").Append(Ps(Posix(Games.Resolve(settings, pnach))));
                foreach (string sec in sections) port.Append(" --pnach-section ").Append(Ps(sec));
            }
            if (job.Faster) port.Append(" --no-explore");
            if ((!job.Faster || !Enhanced.CurrentDisc(settings)) && Enhanced.Is(d) && job.Image != d.ImagePath)
            {
                // the Enhanced Edition's disc: the US disc with the Japanese voices, made from the two images (tools\rd_hybrid.py)
                DiscInfo jp = Enhanced.Japanese(discs);
                s.AppendLine("Write-Output '== launcher: making the Enhanced Edition disc (US disc + Japanese voices)'");
                s.AppendLine("& python -I " + Ps(Path.Combine(Path.Combine(Settings.ExeDir, "tools"), "rd_hybrid.py")) + " --us " + Ps(d.ImagePath) + " --jp " + Ps(jp.ImagePath)
                             + " --out " + Ps(job.Image) + " --version " + Ps(Program.Version) + " 2>&1 | ForEach-Object { \"$_\" }");
                s.AppendLine("if ($LASTEXITCODE) { exit $LASTEXITCODE }");
                // the Japanese pictures inside maps and models, as texture overrides (used while the Japanese faces are on)
                s.AppendLine("Write-Output '== launcher: the Japanese pictures inside the maps and models'");
                s.AppendLine("& python -I " + Ps(Path.Combine(Path.Combine(Settings.ExeDir, "tools"), "rd_jp_overrides.py")) + " make --us " + Ps(d.ImagePath) + " --jp " + Ps(jp.ImagePath)
                             + " --out " + Ps(Path.Combine(Games.Folder(settings, d.GameName), "prompts")) + " 2>&1 | ForEach-Object { \"$_\" }");
            }
            s.AppendLine("Write-Output ('command: ' + " + Ps(port.ToString()) + ")");
            s.AppendLine(port.ToString() + " 2>&1 | ForEach-Object { \"$_\" }");
            s.AppendLine("exit $LASTEXITCODE");
            string folder = Games.Folder(settings, d.GameName);
            Directory.CreateDirectory(folder);
            string file = Path.Combine(folder, "launcher_build.ps1");
            File.WriteAllText(file, s.ToString(), new UTF8Encoding(true));
            return file;
        }

        void Log(string line)
        {
            if (log.TextLength > 300000) log.Text = log.Text.Substring(150000);
            log.AppendText(line + "\r\n");
        }

        // Files dated in the future (a zip made in another time zone stores local times) make ninja rebuild its own build files
        // forever ("manifest 'build.ninja' still dirty after 100 tries"). Before each build, any engine source dated later than
        // now is dated now. The engine's downloads, builds, games and generated code are left alone.
        // The project folder moved (another drive, renamed): CMake refuses a build folder made somewhere else ("the current
        // CMakeCache.txt directory ... is different"), so a build folder whose CMakeCache.txt names another place is removed
        // and made again by the build (the runtime and the tools take a few minutes once).
        void FixMovedBuilds()
        {
            string build = Path.Combine(settings.PnrDir, "build");
            if (!Directory.Exists(build)) return;
            try
            {
                foreach (string dir in Directory.GetDirectories(build))
                {
                    string cache = Path.Combine(dir, "CMakeCache.txt");
                    if (!File.Exists(cache)) continue;
                    string made = "";
                    foreach (string line in File.ReadAllLines(cache))
                        if (line.StartsWith("CMAKE_CACHEFILE_DIR:")) { made = line.Substring(line.IndexOf('=') + 1).Trim(); break; }
                    if (made == "") continue;
                    string a = Path.GetFullPath(made.Replace('/', Path.DirectorySeparatorChar)).TrimEnd(Path.DirectorySeparatorChar);
                    string b = Path.GetFullPath(dir).TrimEnd(Path.DirectorySeparatorChar);
                    if (string.Equals(a, b, StringComparison.OrdinalIgnoreCase)) continue;
                    long freed = Remove(dir);
                    Log(Lang.L("==== the project folder moved: build folder made again (" + Path.GetFileName(dir) + ", was in " + made + ")",
                               "==== プロジェクトのフォルダが移動しました: ビルドフォルダを作り直します（" + Path.GetFileName(dir) + "、元: " + made + "）"));
                    Program.Trace("moved build folder removed: " + dir + " (made in " + made + ", " + freed / (1024 * 1024) + " MB)");
                }
            }
            catch (Exception e) { Program.Trace("moved builds: " + e.Message); }
        }

        void FixFutureTimes()
        {
            int fixedCount = 0;
            DateTime now = DateTime.UtcNow;
            Action<string> walk = null;
            walk = delegate(string dir)
            {
                string name = Path.GetFileName(dir.TrimEnd('\\', '/'));
                if (name == "downloads" || name == "build" || name == "games" || name == "generated" || name == ".git") return;
                try
                {
                    foreach (string f in Directory.GetFiles(dir))
                        try { if (File.GetLastWriteTimeUtc(f) > now.AddMinutes(1)) { File.SetLastWriteTimeUtc(f, now); fixedCount++; } } catch (Exception) { }
                    foreach (string sub in Directory.GetDirectories(dir)) walk(sub);
                }
                catch (Exception) { }
            };
            if (Directory.Exists(settings.PnrDir)) walk(settings.PnrDir);
            string tools = Path.Combine(Settings.ExeDir, "tools");
            if (Directory.Exists(tools)) walk(tools);
            if (fixedCount > 0) { Log(Lang.L("== fixed the date of ", "== 日付を修正したファイル: ") + fixedCount + Lang.L(" files dated in the future", " 件（未来の日付）")); Program.Trace("future-dated files fixed: " + fixedCount); }
        }

        void StartNext()
        {
            if (stopping || queue.Count == 0) { current = null; ShowDone(); return; }
            BuildJob job = queue.Dequeue();
            current = job;
            if (log.Parent != building) { log.Parent = building; log.Location = new Point(28, 144); log.Size = new Size(840, 296); }
            log.Visible = detailsButton.Text == Lang.L("Hide details", "詳細を隠す");
            DiscInfo d = job.Disc;
            int index = finished.Count + 1, total = finished.Count + queue.Count + 1;
            buildTitle.Text = (job.Faster ? Lang.L("Making it faster: ", "高速化中: ") : Lang.L("Building: ", "ビルド中: ")) + d.Title + (total > 1 ? "   (" + index + " of " + total + ")" : "");
            buildStatus.Text = Lang.L("Starting...", "開始しています...");
            bar.Value = 0;
            FixFutureTimes();
            FixMovedBuilds();
            try
            {
                string script = WriteScript(job);
                string folder = Games.Folder(settings, d.GameName);
                job.LogFile = new StreamWriter(Path.Combine(folder, "build.log"), false, new UTF8Encoding(false));
                job.LogFile.AutoFlush = true;
                if (!job.Faster)
                {
                    Dictionary<string, string> app = new Dictionary<string, string>();
                    app["name"] = d.GameName; app["source"] = job.Image; app["title"] = d.Title;
                    app["pnach"] = d.PnachPath != "" && job.Sections.Count > 0 ? d.PnachPath : "";
                    File.WriteAllText(Path.Combine(folder, "app.json"), Json.WriteApp(app, job.Sections), new UTF8Encoding(true));
                }
                ProcessStartInfo psi = new ProcessStartInfo(Path.Combine(Environment.SystemDirectory, "WindowsPowerShell", "v1.0", "powershell.exe"),
                    "-NoProfile -ExecutionPolicy Bypass -File \"" + script + "\"");
                psi.WorkingDirectory = settings.PnrDir;
                psi.UseShellExecute = false;
                psi.CreateNoWindow = true;
                psi.RedirectStandardOutput = true;
                psi.RedirectStandardError = true;
                psi.StandardOutputEncoding = Encoding.UTF8;
                psi.StandardErrorEncoding = Encoding.UTF8;
                // v0.9.9: the build's temporary files in a folder of its own, removed when it ends
                try
                {
                    job.TempDir = MakeBuildTemp();
                    psi.EnvironmentVariables["TEMP"] = job.TempDir;
                    psi.EnvironmentVariables["TMP"] = job.TempDir;
                    psi.EnvironmentVariables["TMPDIR"] = job.TempDir;
                }
                catch (Exception e) { job.TempDir = ""; Program.Trace("build temp: " + e.Message); }
                // compiler jobs: the engine's scripts read PS2_JOBS (env.ps1 keeps it); one the player set himself wins
                if (string.IsNullOrEmpty(Environment.GetEnvironmentVariable("PS2_JOBS")))
                    psi.EnvironmentVariables["PS2_JOBS"] = BuildLoad.Jobs(settings).ToString();
                // the game's icon built into its program (Explorer, Task Manager, the taskbar)
                string exeIcon = Icons.ForGame(settings, d.GameName);
                if (exeIcon != "") psi.EnvironmentVariables["PS2_EXE_ICON"] = exeIcon.Replace('\\', '/');
                Process p = new Process();
                p.StartInfo = psi;
                p.EnableRaisingEvents = true;
                p.OutputDataReceived += delegate(object o, DataReceivedEventArgs e) { if (e.Data != null) lock (job.Lines) job.Lines.Enqueue(e.Data); };
                p.ErrorDataReceived += delegate(object o, DataReceivedEventArgs e) { if (e.Data != null) lock (job.Lines) job.Lines.Enqueue(e.Data); };
                p.Exited += delegate
                {
                    try { p.WaitForExit(); } catch (Exception) { }
                    job.ExitCode = p.ExitCode;
                    long freed = RemoveBuildTemp(job.TempDir);
                    if (job.TempDir != "") lock (job.Lines) job.Lines.Enqueue("== launcher: temporary files removed (" + (freed >> 20) + " MB in " + job.TempDir + ")");
                    job.Done = true;
                };
                job.Estimate = BuildTime.Estimate(settings, d.GameName, job.Faster, out job.EstimateKnown);
                job.Clock = Stopwatch.StartNew();
                p.Start();
                if (BuildLoad.Gentle(settings)) try { p.PriorityClass = BuildLoad.Priority(settings); } catch (Exception) { }
                Log("== launcher: build speed " + (BuildLoad.Speed(settings) == 0 ? "full" : BuildLoad.Speed(settings) == 2 ? "light" : "balanced") + ", " + psi.EnvironmentVariables["PS2_JOBS"] + " compiler jobs, " + BuildLoad.Priority(settings) + " priority");
                p.BeginOutputReadLine();
                p.BeginErrorReadLine();
                job.Process = p;
                Log("==== " + buildTitle.Text + " -> games\\" + d.GameName + (job.Sections.Count > 0 ? "  [" + string.Join(", ", job.Sections.ToArray()) + "]" : "  [no patches]"));
                Program.Trace("build start " + d.GameName + " script " + script);
            }
            catch (Exception e)
            {
                job.Problem = Lang.L("could not start: ", "開始できません: ") + e.Message;
                Program.Trace("build start failed " + e);
                finished.Add(job);
                current = null;
                StartNext();
            }
        }

        static readonly Regex StepLine = new Regex(@"^== \[(\d)/6\]\s*(.*)$");
        static readonly Regex NinjaLine = new Regex(@"^\[(\d+)/(\d+)\]");
        string[] StepText { get { return new string[] { "", Lang.L("Reading the disc", "ディスクを読み込み中"), Lang.L("Translating the game's code", "ゲームのコードを変換中"), Lang.L("Test runs (the game plays itself for a while to find more code)", "テスト実行（ゲームを自動で動かして、さらにコードを探します）"),
                                              Lang.L("Compiling the graphics programs", "グラフィックプログラムをコンパイル中"), Lang.L("Compiling the game (the long part)", "ゲームをコンパイル中（一番長い工程）"), Lang.L("Finishing", "仕上げ中") }; } }

        void Pump()
        {
            BuildJob job = current;
            if (job == null) return;
            if (job.Clock != null)
            {
                double t = job.Clock.Elapsed.TotalSeconds, left = job.Estimate - t;
                // past half way, the bar says better how much is left than the last build's time
                double f = bar.Value / 1000.0;
                if (f > 0.5 && t > 30) left = Math.Min(Math.Max(left, 0), t * (1 - f) / f + 15);
                buildTime.Text = Lang.L("Elapsed ", "経過 ") + BuildTime.Clock(t) + "   ·   "
                    + (left > 20 ? Lang.L("left: ", "残り: ") + BuildTime.Minutes((int)left) + (job.EstimateKnown ? Lang.L(" (your last build took ", "（前回 ") + BuildTime.Clock(job.Estimate) + Lang.L(")", "）") : Lang.L(" (typical time)", "（目安）"))
                                 : Lang.L("almost done (it can take a little longer than last time)", "まもなく完了（前回より少し長くかかる場合があります）"));
            }
            List<string> lines = new List<string>();
            lock (job.Lines) while (job.Lines.Count > 0) lines.Add(job.Lines.Dequeue());
            foreach (string raw in lines)
            {
                string line = raw.TrimEnd();
                if (line == "") continue;
                if (job.LogFile != null) job.LogFile.WriteLine(line);
                Match m = StepLine.Match(line);
                if (m.Success)
                {
                    job.Step = int.Parse(m.Groups[1].Value);
                    bar.Value = Math.Min(1000, (job.Step - 1) * 1000 / 6);
                    buildStatus.Text = Lang.L("Step ", "手順 ") + job.Step + Lang.L(" of 6: ", " / 6: ") + StepText[Math.Min(6, job.Step)];
                }
                else if (line.StartsWith("== launcher: ")) buildStatus.Text = line.Substring(13);
                else if (line.StartsWith("@app download ")) buildStatus.Text = Lang.L("Downloading the build tools (about 330 MB, first time only)...", "ビルドツールをダウンロード中（約330 MB、初回のみ）...");
                else
                {
                    Match n = NinjaLine.Match(line);
                    if (n.Success && job.Step > 0)
                    {
                        double f = double.Parse(n.Groups[1].Value) / Math.Max(1.0, double.Parse(n.Groups[2].Value));
                        bar.Value = Math.Max(bar.Value, Math.Min(1000, (int)(((job.Step - 1) + f) * 1000 / 6)));
                        if (line.Length > 140) line = line.Substring(0, 140);
                    }
                }
                if (line.StartsWith("Play it natively:") && line.Contains("--widescreen")) job.Wide = true;
                Log(line);
            }
            if (job.Done && (job.Process == null || job.Process.HasExited))
            {
                lock (job.Lines) if (job.Lines.Count > 0) return;
                Finish(job);
            }
        }

        void Finish(BuildJob job)
        {
            string seconds = ((int)job.Clock.Elapsed.TotalSeconds).ToString();
            job.Problem = stopping ? Lang.L("stopped by you", "中止されました") : job.ExitCode != 0 ? Lang.L("the build failed with exit code ", "ビルドに失敗しました。終了コード ") + job.ExitCode : Complete(job, seconds);
            if (job.LogFile != null) { job.LogFile.Dispose(); job.LogFile = null; }
            Program.Trace("build end " + job.Disc.GameName + ": " + (job.Problem ?? "ok") + " after " + seconds + " s");
            if (job.Problem == null && !job.Faster) BuildTime.Remember(job.Disc.GameName, int.Parse(seconds));
            Log(job.Problem == null ? "==== built " + job.Disc.Title + " in " + (int.Parse(seconds) / 60) + " min" : "==== " + job.Disc.Title + ": " + job.Problem);
            Cleanup(job, job.Problem == null);
            if (job.Problem == null && job.Shortcut) ShortcutFor(job.Disc, false);
            finished.Add(job);
            current = null;
            StartNext();
        }

        // After a good build: the translated C++, the compiler's objects and the test-run leftovers go; the game program
        // (ps2run.exe and its DLLs), the patched start program, the code caches "Make it faster" uses and the saves stay.
        // The engine's downloads folder (the tools pack) is never touched. A copy of an .iso disc image is dropped too: the
        // game then reads the original image (a .cue/.bin disc keeps its converted copy).
        // A build that failed or was stopped loses its translated C++ and objects too (they are made again by the next build, and
        // can be several GB); its build.log stays for a look at what went wrong. Afterwards each removed thing is checked again:
        // whatever could not be removed (a file still open, no permission) is named in the log.
        void Cleanup(BuildJob job, bool good)
        {
            string name = job.Disc.GameName, pnr = settings.PnrDir;
            long freed = 0;
            List<string> gone = new List<string>();
            try
            {
                string generated = Path.Combine(Path.Combine(pnr, "generated"), name);
                gone.Add(generated);
                freed += Remove(generated);
                string build = Path.Combine(Path.Combine(pnr, "build"), "native-" + name);
                if (Directory.Exists(build))
                {
                    foreach (string f in Directory.GetFiles(build))
                    {
                        string n = Path.GetFileName(f).ToLowerInvariant();
                        if (good && (n == "ps2run.exe" || n == "ps2run" || n.EndsWith(".dll"))) continue;
                        if (!good && n.EndsWith(".dll")) continue;   // a failed build keeps the DLLs (a working ps2run.exe from before may need them)
                        if (!good && (n == "ps2run.exe" || n == "ps2run")) continue;
                        gone.Add(f);
                        freed += Remove(f);
                    }
                    foreach (string d in Directory.GetDirectories(build)) { gone.Add(d); freed += Remove(d); }
                }
                string game = Games.Folder(settings, name);
                foreach (string n in new string[] { "report.state", "rec.inputs", "rec.inputs.ps2", "launcher_build.ps1" }) { gone.Add(Path.Combine(game, n)); freed += Remove(Path.Combine(game, n)); }
                if (!good)
                {
                    // a hybrid disc left half written by a stopped Enhanced build
                    string part = Path.Combine(game, "hybrid.iso.part");
                    gone.Add(part);
                    freed += Remove(part);
                }
                // the disc copy, when the original is an .iso that is still there
                Dictionary<string, string> app = Games.ReadApp(settings, name);
                string copy = Path.Combine(game, "disc.iso"), source = app == null ? "" : Games.Get(app, "source");
                if (good && app != null && File.Exists(copy) && source != "" && File.Exists(source) && source.EndsWith(".iso", StringComparison.OrdinalIgnoreCase)
                    && !string.Equals(Path.GetFullPath(source), Path.GetFullPath(copy), StringComparison.OrdinalIgnoreCase))
                {
                    app["iso"] = source;
                    File.WriteAllText(Path.Combine(game, "app.json"), Json.WriteApp(app, new List<string>(Games.Get(app, "sections").Split(new char[] { '|' }, StringSplitOptions.RemoveEmptyEntries))), new UTF8Encoding(true));
                    gone.Add(copy);
                    freed += Remove(copy);
                }
            }
            catch (Exception e) { Program.Trace("cleanup " + name + ": " + e.Message); }
            List<string> left = new List<string>();
            foreach (string g in gone) if (File.Exists(g) || Directory.Exists(g)) left.Add(g);
            Log(Lang.L("==== cleaned up: ", "==== 一時ファイルを削除: ") + (freed / (1024 * 1024)) + " MB" + (good ? "" : Lang.L(" (the failed build's temporary files; build.log stays)", "（失敗したビルドの一時ファイル。build.log は残します）")));
            foreach (string l in left) Log(Lang.L("     could not remove (still in use?): ", "     削除できませんでした（使用中？）: ") + l);
            Program.Trace("cleanup " + name + (good ? "" : " (failed build)") + ": " + freed / (1024 * 1024) + " MB freed" + (left.Count > 0 ? ", " + left.Count + " left: " + string.Join(" | ", left.ToArray()) : ", nothing left"));
        }

        static long Remove(string path)
        {
            long size = 0;
            try
            {
                if (File.Exists(path)) { size = new FileInfo(path).Length; File.SetAttributes(path, FileAttributes.Normal); File.Delete(path); }
                else if (Directory.Exists(path))
                {
                    foreach (string f in Directory.GetFiles(path, "*", SearchOption.AllDirectories))
                        try { size += new FileInfo(f).Length; File.SetAttributes(f, FileAttributes.Normal); } catch (Exception) { }
                    Directory.Delete(path, true);
                }
            }
            catch (Exception e) { Program.Trace("remove " + path + ": " + e.Message); }
            return size;
        }

        // What the ps2-native-recomp app does after a build (Complete-Build): app.json with the program, disc and patches.
        string Complete(BuildJob job, string seconds)
        {
            DiscInfo d = job.Disc;
            string name = d.GameName, folder = Games.Folder(settings, name);
            if (!File.Exists(Games.Runner(settings, name))) return Lang.L("the game executable was not created", "ゲームの実行ファイルが作成されませんでした");
            string program = "";
            string cnf = Path.Combine(folder, "disc", "SYSTEM.CNF");
            if (File.Exists(cnf))
                foreach (string line in File.ReadAllLines(cnf))
                {
                    Match m = Regex.Match(line, @"^\s*BOOT2\s*=\s*.*\\([^\\;]+);");
                    if (m.Success) { program = m.Groups[1].Value; break; }
                }
            string elf = Path.Combine(folder, "core.elf");
            if (!File.Exists(elf)) elf = Path.Combine(folder, "disc", program);
            if (program == "" || !File.Exists(elf)) return Lang.L("the game's boot program was not found", "ゲームの起動プログラムが見つかりませんでした");
            string iso = Path.Combine(folder, "disc.iso");
            if (!File.Exists(iso)) iso = job.Image;
            Dictionary<string, string> old = Games.ReadApp(settings, name);
            string pnach = d.PnachPath != "" && job.Sections.Count > 0 ? d.PnachPath : "";
            if (job.Faster && old != null) { pnach = Games.Get(old, "pnach"); if (Games.Get(old, "took") != "") seconds = Games.Get(old, "took"); }
            string patchList = "";
            if (pnach != "" && job.Sections.Count > 0)
            {
                string patched = Path.Combine(folder, "patched", Path.GetFileName(elf));
                patchList = Path.Combine(folder, "patches.txt");
                if (!File.Exists(patched) || !File.Exists(patchList)) return Lang.L("the patched boot program was not found", "パッチ適用済みの起動プログラムが見つかりませんでした");
                elf = patched;
            }
            bool wide = job.Wide || (job.Faster && old != null && Games.Get(old, "widescreen") != "");
            Dictionary<string, string> app = new Dictionary<string, string>();
            app["name"] = name;
            app["source"] = job.Faster && old != null && Games.Get(old, "source") != "" ? Games.Get(old, "source") : job.Image;
            app["built"] = DateTime.Now.ToString("s");
            app["took"] = seconds;
            app["elf"] = Games.Relative(settings, elf);
            app["iso"] = Games.Relative(settings, iso);
            app["pnach"] = pnach;
            app["patches"] = patchList == "" ? "" : Games.Relative(settings, patchList);
            app["widescreen"] = wide ? "1" : "";
            app["title"] = d.Title;
            File.WriteAllText(Path.Combine(folder, "app.json"), Json.WriteApp(app, job.Sections), new UTF8Encoding(true));
            if (Enhanced.Is(d)) Enhanced.Settle(folder);
            try { RecompIni.Make(settings, name, pnach != "" ? Games.Resolve(settings, pnach) : "", job.Sections, Path.Combine(Path.Combine(folder, "disc"), program)); }
            catch (Exception e) { Program.Trace("recomp.ini " + name + ": " + e.Message); }
            return null;
        }

        void ShowDone()
        {
            FillDiscList();
            RefreshLibrary();
            doneButtons.Controls.Clear();
            StringBuilder sb = new StringBuilder();
            int ok = 0;
            foreach (BuildJob j in finished)
            {
                if (j.Problem == null)
                {
                    ok++;
                    DiscInfo d = j.Disc;
                    Button play = new Button();
                    play.Text = Lang.L("Play  ", "プレイ  ") + d.Title;
                    play.Size = new Size(420, 40);
                    Style(play, true);
                    play.Click += delegate { string p = Games.Play(settings, d.GameName); if (p != null) MessageBox.Show(p, Program.AppName); };
                    doneButtons.Controls.Add(play);
                    Label what = new Label();
                    what.AutoSize = false; what.Size = new Size(820, 34); what.ForeColor = Dim; what.Font = new Font(Lang.UiFont, 8.5f);
                    what.Text = PatchNames.List(j.Sections, Enhanced.Is(d));
                    doneButtons.Controls.Add(what);
                }
                else sb.AppendLine(j.Disc.Title + ": " + j.Problem + Lang.L(". Details: games\\", "。詳細: games\\") + j.Disc.GameName + "\\build.log");
            }
            doneText.ForeColor = sb.Length > 0 ? Bad : Text1;
            doneText.Text = (ok > 0 ? (ok == 1 ? Lang.L("Your game is ready.", "ゲームの準備ができました。") : ok + Lang.L(" games are ready.", " 本のゲームの準備ができました。")) + (finished.Count > 0 && finished[0].Shortcut ? Lang.L(" Their shortcuts are on the desktop.", " ショートカットはデスクトップにあります。") : "") + "\n\n" : "") + sb.ToString();
            log.Visible = false;
            doneButtons.Visible = true;
            doneDetails.Text = Lang.L("Show build details", "ビルドの詳細を表示");
            Go(done);
        }

        void StopBuild()
        {
            stopping = true;
            queue.Clear();
            BuildJob job = current;
            if (job == null || job.Process == null) return;
            try
            {
                if (!job.Process.HasExited)
                {
                    ProcessStartInfo k = new ProcessStartInfo("taskkill.exe", "/PID " + job.Process.Id + " /T /F");
                    k.CreateNoWindow = true; k.UseShellExecute = false;
                    Process kill = Process.Start(k);
                    if (kill != null) kill.WaitForExit(5000);
                }
            }
            catch (Exception) { }
        }
    }

    // Diagonal black and yellow warning tape.
    static class Hazard
    {
        public static void Stripes(Graphics g, Rectangle r, Color a, Color b, int width)
        {
            using (SolidBrush back = new SolidBrush(b)) g.FillRectangle(back, r);
            g.SmoothingMode = System.Drawing.Drawing2D.SmoothingMode.AntiAlias;
            Region old = g.Clip;
            g.SetClip(r);
            using (SolidBrush fore = new SolidBrush(a))
                for (int x = r.Left - r.Height - width * 2; x < r.Right + r.Height; x += width * 2)
                    g.FillPolygon(fore, new Point[] { new Point(x, r.Bottom), new Point(x + width, r.Bottom), new Point(x + width + r.Height, r.Top), new Point(x + r.Height, r.Top) });
            g.Clip = old;
        }
    }

    class Tape : Control
    {
        public Tape() { DoubleBuffered = true; SetStyle(ControlStyles.Selectable, false); TabStop = false; }
        protected override void OnPaint(PaintEventArgs e) { Hazard.Stripes(e.Graphics, ClientRectangle, Theme.Tape1, Theme.Tape2, 14); }
    }

    // The banner: warning tape, the title in poster letters, a warning sign.
    class Header : Control
    {
        static Image logo;
        static bool tried, triedJP;

        // The title art: logo.png next to the exe if there is one, else the copy built into the exe.
        static Image Logo()
        {
            if (tried && triedJP == Lang.JP) return logo;
            tried = true;
            triedJP = Lang.JP;
            logo = null;
            string name = Lang.JP ? "logo_jp.png" : "logo.png";
            try
            {
                string file = Path.Combine(Settings.ExeDir, name);
                if (File.Exists(file)) { using (FileStream f = File.OpenRead(file)) logo = new Bitmap(Image.FromStream(f)); }
                else
                {
                    Stream st = typeof(Header).Assembly.GetManifestResourceStream("RawDangerLauncher." + name);
                    if (st != null) using (st) logo = new Bitmap(Image.FromStream(st));
                }
            }
            catch (Exception e) { Program.Trace("logo: " + e.Message); logo = null; }
            return logo;
        }

        public Header() { DoubleBuffered = true; TabStop = false; }

        protected override void OnPaint(PaintEventArgs e)
        {
            Graphics g = e.Graphics;
            Color yellow = Theme.Accent, red = Color.FromArgb(222, 40, 28), black = Color.FromArgb(12, 12, 11);
            using (System.Drawing.Drawing2D.LinearGradientBrush sky = new System.Drawing.Drawing2D.LinearGradientBrush(ClientRectangle,
                   Theme.HeaderTop, Theme.HeaderBottom, System.Drawing.Drawing2D.LinearGradientMode.Vertical))
                g.FillRectangle(sky, ClientRectangle);
            Hazard.Stripes(g, new Rectangle(0, 0, Width, 10), yellow, black, 14);
            Hazard.Stripes(g, new Rectangle(0, Height - 6, Width, 6), yellow, black, 10);
            g.SmoothingMode = System.Drawing.Drawing2D.SmoothingMode.AntiAlias;
            g.TextRenderingHint = System.Drawing.Text.TextRenderingHint.AntiAlias;
            float x = 20;
            Image art = Logo();
            if (art != null)
            {
                g.InterpolationMode = System.Drawing.Drawing2D.InterpolationMode.HighQualityBicubic;
                g.PixelOffsetMode = System.Drawing.Drawing2D.PixelOffsetMode.HighQuality;
                int h = Height - 26, w = art.Width * h / art.Height;
                g.DrawImage(art, new Rectangle((int)x, 14, w, h));
                x += w;
            }
            else
            {
                x = Word(g, "RAW", x, Color.FromArgb(236, 240, 246), Color.FromArgb(40, 70, 130));
                x = Word(g, "DANGER!", x + 10, red, black);
            }
            using (Font f = new Font(Lang.UiFont, 8f))
            using (SolidBrush b = new SolidBrush(Theme.Dim))
                g.DrawString("v" + Program.Version + Lang.L("  -  powered by ps2-native-recomp", "  -  エンジン: ps2-native-recomp"), f, b, x + 22, 44);
            using (Font f = new Font(Lang.UiFont, 9.5f, FontStyle.Bold))
            using (SolidBrush b = new SolidBrush(yellow))
            {
                g.DrawString(Lang.L("PC EDITION BUILDER", "PC版ビルダー"), f, b, x + 22, 62);
                g.DrawString(Lang.L("YOUR DISCS, BUILT INTO AN ENHANCED NATIVE PORT", "あなたのディスクを、強化されたネイティブ移植版に"), f, b, x + 22, 80);
            }
            Sign(g, Width - 70, (Height + 4) / 2, 34);
        }

        float Word(Graphics g, string text, float x, Color fill, Color edge)
        {
            using (System.Drawing.Drawing2D.GraphicsPath path = new System.Drawing.Drawing2D.GraphicsPath())
            {
                FontFamily family;
                try { family = new FontFamily("Impact"); } catch (Exception) { family = FontFamily.GenericSansSerif; }
                path.AddString(text, family, (int)FontStyle.Regular, 52f, new PointF(x, 14), StringFormat.GenericTypographic);
                using (Pen shadow = new Pen(Color.FromArgb(160, 0, 0, 0), 9)) { g.TranslateTransform(3, 3); g.DrawPath(shadow, path); g.ResetTransform(); }
                using (Pen outline = new Pen(edge, 5)) { outline.LineJoin = System.Drawing.Drawing2D.LineJoin.Round; g.DrawPath(outline, path); }
                using (SolidBrush b = new SolidBrush(fill)) g.FillPath(b, path);
                using (Pen hi = new Pen(Color.FromArgb(255, 204, 0), 1.5f)) g.DrawPath(hi, path);
                return path.GetBounds().Right;
            }
        }

        static void Sign(Graphics g, int cx, int cy, int r)
        {
            Point[] diamond = { new Point(cx, cy - r), new Point(cx + r, cy), new Point(cx, cy + r), new Point(cx - r, cy) };
            using (SolidBrush b = new SolidBrush(Theme.Accent)) g.FillPolygon(b, diamond);
            using (Pen p = new Pen(Theme.OnAccent, 3)) g.DrawPolygon(p, new Point[] { new Point(cx, cy - r + 5), new Point(cx + r - 5, cy), new Point(cx, cy + r - 5), new Point(cx - r + 5, cy) });
            using (Font f = new Font("Impact", 26f))
            using (SolidBrush b = new SolidBrush(Theme.OnAccent))
            {
                StringFormat sf = new StringFormat(); sf.Alignment = StringAlignment.Center; sf.LineAlignment = StringAlignment.Center;
                g.DrawString("!", f, b, new RectangleF(cx - r, cy - r + 2, 2 * r, 2 * r), sf);
            }
        }
    }

    // Progress as warning tape filling up.
    class HazardBar : Control
    {
        int value;
        public int Value { get { return value; } set { this.value = Math.Max(0, Math.Min(1000, value)); Invalidate(); } }
        public HazardBar() { DoubleBuffered = true; TabStop = false; }
        protected override void OnPaint(PaintEventArgs e)
        {
            Graphics g = e.Graphics;
            using (SolidBrush b = new SolidBrush(Theme.Card)) g.FillRectangle(b, ClientRectangle);
            int w = (int)((long)(Width - 4) * value / 1000);
            if (w > 0) Hazard.Stripes(g, new Rectangle(2, 2, w, Height - 4), Theme.Tape1, Theme.Tape2, 10);
            using (Pen p = new Pen(Theme.Line, 1)) g.DrawRectangle(p, 0, 0, Width - 1, Height - 1);
        }
    }

    // the launcher music's volume: yellow for the part that is on, hazard tape for the rest
    public class VolumeBar : Control
    {
        int value;
        public event EventHandler Changed;
        public int Value { get { return value; } set { this.value = Math.Max(0, Math.Min(1000, value)); Invalidate(); } }

        public VolumeBar() { DoubleBuffered = true; Cursor = Cursors.Hand; SetStyle(ControlStyles.Selectable, true); }

        protected override void OnPaint(PaintEventArgs e)
        {
            Graphics g = e.Graphics;
            // the theme's colours (US: yellow on black; JP: blue on white), so it follows a language / theme change
            g.Clear(Theme.Tape2);
            using (Pen stripe = new Pen(Color.FromArgb(110, Theme.Tape1), 3))
                for (int x = -Height; x < Width + Height; x += 8) g.DrawLine(stripe, x, Height, x + Height, 0);
            using (SolidBrush b = new SolidBrush(Theme.Accent)) g.FillRectangle(b, 0, 0, Width * value / 1000, Height);
            using (Pen edge = new Pen(Theme.Line)) g.DrawRectangle(edge, 0, 0, Width - 1, Height - 1);
        }

        void Set(int x)
        {
            Value = (int)Math.Round(Math.Max(0, Math.Min(Width, x)) * 1000.0 / Math.Max(1, Width) / 50) * 50;   // 5 % steps
            if (Changed != null) Changed(this, EventArgs.Empty);
        }

        protected override void OnMouseDown(MouseEventArgs e) { base.OnMouseDown(e); Focus(); Set(e.X); }
        protected override void OnMouseMove(MouseEventArgs e) { base.OnMouseMove(e); if (e.Button == MouseButtons.Left) Set(e.X); }
        protected override void OnMouseWheel(MouseEventArgs e)
        {
            base.OnMouseWheel(e);
            Value = value + (e.Delta > 0 ? 50 : -50);
            if (Changed != null) Changed(this, EventArgs.Empty);
        }
    }
}
