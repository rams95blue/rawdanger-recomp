// Raw Danger PC - the build tools: the nine files the first build needs (the compiler, CMake, Ninja, bash, Python, the GPU and
// window libraries, the shader compiler), each from its official page. The engine downloads them once, or uses a copy saved in
// engine\ps2-native-recomp\downloads (checked by SHA-256, so a wrong file is only skipped). This class finds such copies in a
// few more places (a "downloads" or "build tools" folder next to RawDangerPC.exe, another copy of the engine), copies them where
// the engine looks before a build, and shows the list with where to get each file (docs/BUILD_TOOLS.md says the same).
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Windows.Forms;

namespace RawDangerLauncher
{
    public static class BuildTools
    {
        public class Item
        {
            public string Title, File, Url;
            public long Size;   // bytes, 0 when not pinned here
            public Item(string title, string file, string url, long size) { Title = title; File = file; Url = url; Size = size; }
        }

        public static readonly Item[] All = {
            new Item("llvm-mingw 20260908 (the C++ compiler)", "llvm-mingw-20260908-ucrt-x86_64.zip", "https://github.com/mstorsjo/llvm-mingw/releases/download/20260908/llvm-mingw-20260908-ucrt-x86_64.zip", 190677197),
            new Item("CMake 4.4.4", "cmake-4.4.4-windows-x86_64.zip", "https://github.com/Kitware/CMake/releases/download/v4.4.4/cmake-4.4.4-windows-x86_64.zip", 54400562),
            new Item("Ninja 1.13.2", "ninja-win.zip", "https://github.com/ninja-build/ninja/releases/download/v1.13.2/ninja-win.zip", 291570),
            new Item("Python 3.13.16 (embeddable)", "python-3.13.16-embed-amd64.zip", "https://www.python.org/ftp/python/3.13.16/python-3.13.16-embed-amd64.zip", 11439094),
            new Item("MinGit 2.56.0.2 (bash and the Unix tools)", "MinGit-2.56.0.2-64-bit.zip", "https://github.com/git-for-windows/git/releases/download/v2.56.0.windows.2/MinGit-2.56.0.2-64-bit.zip", 39806486),
            new Item("BusyBox for Windows FRP-6075", "busybox-w64u-FRP-6075-g169694ebd.exe", "https://frippery.org/files/busybox/busybox-w64u-FRP-6075-g169694ebd.exe", 675840),
            new Item("wgpu-native v29.0.1.1 (the GPU library)", "wgpu-windows-x86_64-gnu-release.zip", "https://github.com/gfx-rs/wgpu-native/releases/download/v29.0.1.1/wgpu-windows-x86_64-gnu-release.zip", 0),
            new Item("SDL2 2.32.10 (window, sound, controllers)", "SDL2-devel-2.32.10-mingw.zip", "https://github.com/libsdl-org/SDL/releases/download/release-2.32.10/SDL2-devel-2.32.10-mingw.zip", 0),
            new Item("DirectX Shader Compiler v1.9.2609", "dxc_2026_09_29.zip", "https://github.com/microsoft/DirectXShaderCompiler/releases/download/v1.9.2609/dxc_2026_09_29.zip", 0),
        };

        // The engine has its tools unpacked already: nothing to download or copy.
        public static bool Installed(Settings s)
        {
            return s.PnrDir != "" && System.IO.File.Exists(Path.Combine(s.PnrDir, Path.Combine("tools", Path.Combine("portable", "env.ps1"))));
        }

        public static string EngineDownloads(Settings s) { return s.PnrDir == "" ? "" : Path.Combine(s.PnrDir, "downloads"); }

        // Folders where a saved copy may be: the engine's downloads first, then next to this program, then another engine copy.
        static List<string> Places(Settings s)
        {
            List<string> list = new List<string>();
            if (s.PnrDir != "") list.Add(EngineDownloads(s));
            foreach (string name in new string[] { "downloads", "build tools", "build-tools", "tools pack", "tools-pack" })
                list.Add(Path.Combine(Settings.ExeDir, name));
            string up = Path.GetDirectoryName(Settings.ExeDir.TrimEnd('\\', '/'));
            if (up != null) list.Add(Path.Combine(up, "downloads"));
            string other = s.OtherEngine();
            if (other != "") list.Add(Path.Combine(other, "downloads"));
            return list;
        }

        static bool Plausible(string path, Item it)
        {
            try { long n = new FileInfo(path).Length; return it.Size > 0 ? n == it.Size : n > 1024; }
            catch (Exception) { return false; }
        }

        // Where the file is on this PC ("" when nowhere).
        public static string Where(Settings s, Item it)
        {
            foreach (string dir in Places(s))
            {
                string p = Path.Combine(dir, it.File);
                if (System.IO.File.Exists(p) && Plausible(p, it)) return p;
            }
            return "";
        }

        public static int Found(Settings s)
        {
            int n = 0;
            foreach (Item it in All) if (Where(s, it) != "") n++;
            return n;
        }

        // Before a build: copies saved files from the other places into the engine's downloads, where its scripts look.
        public static int Gather(Settings s)
        {
            if (Installed(s) || s.PnrDir == "") return 0;
            string target = EngineDownloads(s);
            int copied = 0;
            foreach (Item it in All)
            {
                string from = Where(s, it), to = Path.Combine(target, it.File);
                if (from == "" || string.Equals(Path.GetFullPath(from), Path.GetFullPath(to), StringComparison.OrdinalIgnoreCase)) continue;
                try { Directory.CreateDirectory(target); System.IO.File.Copy(from, to, true); copied++; }
                catch (Exception e) { Program.Trace("build tools: copy " + from + ": " + e.Message); }
            }
            return copied;
        }

        public static string Note(Settings s)
        {
            if (s.PnrDir == "") return "";
            if (Installed(s)) return Lang.L("Build tools: ready (nothing to download).", "ビルドツール：準備済み（ダウンロード不要）。");
            int have = Found(s);
            if (have == All.Length) return Lang.L("Build tools: all " + All.Length + " found on this PC, nothing to download.", "ビルドツール：" + All.Length + " 個すべてこのPCにあります。ダウンロード不要です。");
            if (have > 0) return Lang.L("Build tools: " + have + " of " + All.Length + " found on this PC; the rest is downloaded once.", "ビルドツール：" + All.Length + " 個中 " + have + " 個がこのPCにあります。残りは初回のみダウンロードします。");
            return Lang.L("Build tools: downloaded once from their official pages (about 330 MB). Build tools... lists them, for an offline build.", "ビルドツール：初回のみ公式ページからダウンロードします（約330 MB）。オフラインでビルドするには「ビルドツール...」を参照。");
        }
    }

    // The list of the build tools: each file, found or not, and its official link; a button opens the folder to save them in.
    public class BuildToolsDialog : Form
    {
        public BuildToolsDialog(Settings s)
        {
            Text = Lang.L("Build tools", "ビルドツール");
            Font = new Font(Lang.UiFont, 9.5f);
            BackColor = Theme.Back; ForeColor = Theme.Text;
            FormBorderStyle = FormBorderStyle.FixedDialog; MaximizeBox = false; MinimizeBox = false;
            StartPosition = FormStartPosition.CenterParent;
            ClientSize = new Size(820, 150 + BuildTools.All.Length * 44);
            Icon = WizardForm.AppIcon();
            Label intro = new Label();
            intro.Text = BuildTools.Installed(s)
                ? Lang.L("The build tools are installed: builds need no download. To build on another PC without internet, copy these files there into engine\\ps2-native-recomp\\downloads (or a \"downloads\" folder next to RawDangerPC.exe).",
                         "ビルドツールはインストール済みで、ダウンロードは不要です。インターネットのない別のPCでビルドするには、これらのファイルを engine\\ps2-native-recomp\\downloads（または RawDangerPC.exe の隣の downloads フォルダー）にコピーしてください。")
                : Lang.L("The first build downloads these files from their official pages. Already have them? Put them into engine\\ps2-native-recomp\\downloads, or a \"downloads\" folder next to RawDangerPC.exe: they are used instead (checked first, so a wrong file is only skipped).",
                         "初回のビルドで、これらのファイルを公式ページからダウンロードします。すでにある場合は engine\\ps2-native-recomp\\downloads、または RawDangerPC.exe の隣の downloads フォルダーに置くと、そちらを使います（事前に確認し、違うファイルは使いません）。");
            intro.Location = new Point(16, 12); intro.Size = new Size(788, 52); intro.ForeColor = Theme.Dim;
            Controls.Add(intro);
            int y = 72;
            foreach (BuildTools.Item it in BuildTools.All)
            {
                string where = BuildTools.Where(s, it);
                Label mark = new Label();
                mark.Text = where != "" || BuildTools.Installed(s) ? "✔" : "✖";
                mark.ForeColor = where != "" || BuildTools.Installed(s) ? Theme.Good : Theme.Dim;
                mark.Location = new Point(16, y); mark.Size = new Size(22, 20);
                Controls.Add(mark);
                Label title = new Label();
                title.Text = it.Title + (it.Size > 0 ? "  (" + (it.Size / 1048576.0).ToString("0.#") + " MB)" : "") + (where != "" ? Lang.L("   found: ", "   場所: ") + where : "");
                title.Location = new Point(40, y); title.Size = new Size(764, 20); title.ForeColor = Theme.Text; title.AutoEllipsis = true;
                Controls.Add(title);
                LinkLabel link = new LinkLabel();
                link.Text = it.Url;
                link.LinkColor = Theme.Accent; link.ActiveLinkColor = Theme.Alarm; link.VisitedLinkColor = Theme.Accent;
                link.Location = new Point(40, y + 19); link.Size = new Size(764, 20); link.AutoEllipsis = true;
                string url = it.Url;
                link.LinkClicked += delegate { try { Process.Start(url); } catch (Exception) { } };
                Controls.Add(link);
                y += 44;
            }
            Button open = new Button();
            open.Text = Lang.L("Open the downloads folder", "downloads フォルダーを開く");
            open.Location = new Point(16, y + 16); open.Size = new Size(260, 34);
            open.FlatStyle = FlatStyle.Flat; open.ForeColor = Theme.Accent; open.FlatAppearance.BorderColor = Theme.Accent;
            open.Enabled = s.PnrDir != "";
            open.Click += delegate
            {
                string dir = BuildTools.EngineDownloads(s);
                try { Directory.CreateDirectory(dir); Process.Start("explorer.exe", "\"" + dir + "\""); } catch (Exception) { }
            };
            Controls.Add(open);
            Button close = new Button();
            close.Text = Lang.L("Close", "閉じる");
            close.Location = new Point(684, y + 16); close.Size = new Size(120, 34);
            close.FlatStyle = FlatStyle.Flat; close.BackColor = Theme.Accent; close.ForeColor = Theme.OnAccent; close.FlatAppearance.BorderSize = 0;
            close.DialogResult = DialogResult.OK;
            Controls.Add(close);
            AcceptButton = close; CancelButton = close;
        }
    }
}
