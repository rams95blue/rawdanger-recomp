// Raw Danger PC - how long a build takes: an estimate before it starts and while it runs.
// The estimate is the last good build of the same game on this PC (app.json "took", else launcher.log "build end ... ok after N s");
// a game never built here gets a typical time (measured on a 6-core PC; the first build of all also downloads the tools).
using System;
using System.Runtime.InteropServices;
using System.Collections.Generic;
using System.IO;
using System.Text.RegularExpressions;

namespace RawDangerLauncher
{
    // How hard a build works the PC. The engine's build runs ninja, which starts one clang per thread plus two (a 16-thread PC:
    // 18 compilers at normal priority, each up to 1-2 GB on the big translated files): the mouse lags and memory can run out.
    // Gentle (the default) leaves a quarter of the threads (at least 2) and about 1.5 GB of memory per compiler, at below-normal
    // priority (the compilers inherit it), so the PC stays usable and the build is only a little slower.
    public static class BuildLoad
    {
        [StructLayout(LayoutKind.Sequential)]
        struct MemoryStatus { public uint Length, Load; public ulong Total, Avail, TotalPage, AvailPage, TotalVirtual, AvailVirtual, AvailExtended; }
        [DllImport("kernel32.dll")] static extern bool GlobalMemoryStatusEx(ref MemoryStatus m);

        public static int Jobs(Settings s)
        {
            int fixedJobs;
            if (int.TryParse(s.BuildJobs, out fixedJobs) && fixedJobs > 0) return fixedJobs;
            int cores = Math.Max(1, Environment.ProcessorCount);
            if (s.BuildGentle == "0") return cores + 2;
            int jobs = cores <= 2 ? 1 : cores <= 4 ? cores - 1 : cores - Math.Max(2, cores / 4);
            try
            {
                MemoryStatus m = new MemoryStatus(); m.Length = (uint)Marshal.SizeOf(typeof(MemoryStatus));
                if (GlobalMemoryStatusEx(ref m) && m.Total > 0)
                {
                    ulong mb = m.Total / (1024UL * 1024UL);
                    jobs = Math.Min(jobs, Math.Max(1, (int)((mb > 2048UL ? mb - 2048UL : 0UL) / 1536UL)));
                }
            }
            catch (Exception) { }
            return Math.Max(1, jobs);
        }

        public static bool Gentle(Settings s) { return s.BuildGentle != "0"; }
    }

    public static class BuildTime
    {
        // typical seconds when this PC has no build of the game yet
        static int Typical(string game)
        {
            if (game == Enhanced.Name) return 420;              // the Enhanced disc (both voice sets, about 3 GB written) + the build
            if (game.StartsWith("ZZT2-Demo", StringComparison.Ordinal)) return 90;
            if (game.StartsWith("ZZT2", StringComparison.Ordinal)) return 330;
            return 150;
        }

        static Dictionary<string, int> fromLog;

        static Dictionary<string, int> LogTimes()
        {
            if (fromLog != null) return fromLog;
            fromLog = new Dictionary<string, int>();
            try
            {
                string f = Path.Combine(Settings.ExeDir, "launcher.log");
                if (File.Exists(f))
                    foreach (string line in File.ReadAllLines(f))
                    {
                        Match m = Regex.Match(line, @"build end (\S+): ok after (\d+) s");
                        if (m.Success) fromLog[m.Groups[1].Value] = int.Parse(m.Groups[2].Value);   // the last one wins
                    }
            }
            catch (Exception) { }
            return fromLog;
        }

        public static void Remember(string game, int seconds) { LogTimes()[game] = seconds; }

        // seconds for a build of this game; known = true when it comes from a build on this PC
        public static int Estimate(Settings s, string game, bool faster, out bool known)
        {
            int t = 0;
            known = false;
            Dictionary<string, string> app = Games.ReadApp(s, game);
            if (app != null) int.TryParse(Games.Get(app, "took"), out t);
            int logged;
            if (t <= 0 && LogTimes().TryGetValue(game, out logged)) t = logged;
            if (t > 0) known = true; else t = Typical(game);
            if (faster) t = t * 3 / 4;   // "Make it faster" skips the test runs
            return Math.Max(30, t);
        }

        public static string Minutes(int seconds)
        {
            if (seconds < 90) return Lang.L("about 1 minute", "約1分");
            int m = (seconds + 30) / 60;
            return Lang.L("about " + m + " minutes", "約" + m + "分");
        }

        public static string Clock(double seconds)
        {
            int t = (int)seconds;
            return (t / 60) + ":" + (t % 60).ToString("00");
        }

        // one line for a list of builds, e.g. on the choose page
        public static string ForList(Settings s, List<DiscInfo> discs)
        {
            if (discs.Count == 0) return "";
            int total = 0;
            bool allKnown = true;
            foreach (DiscInfo d in discs)
            {
                bool known;
                total += Estimate(s, d.GameName, false, out known);
                allKnown &= known;
            }
            return Lang.L("Estimated build time: ", "ビルドの目安: ") + Minutes(total)
                   + (allKnown ? Lang.L(" (from your last build)", "（前回のビルドから）") : Lang.L(" (typical; the first build also downloads the tools, about 330 MB)", "（目安。初回はツール約330 MBのダウンロードも）"));
        }
    }
}
