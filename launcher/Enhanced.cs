// Raw Danger PC - the fifth game: Raw Danger! Enhanced Edition. Built from the US disc with every enhancement from the
// roadmap that works today, and offered only when all four discs (US, JP, JP demo Type-A, JP demo Type-B) are found, because
// the roadmap's next enhancements (JP voices, JP-region textures, the demos' extras) are taken from those discs.
using System;
using System.Collections.Generic;
using System.IO;
using System.Text;
using System.Text.RegularExpressions;

namespace RawDangerLauncher
{
    public static class Enhanced
    {
        public const string Name = "RawDanger-Enhanced";

        public static bool Is(DiscInfo d) { return d != null && d.GameName == Name; }

        // The disc made by this version of tools\rd_hybrid.py (v4: a voice index the game can read, and the Japanese lines at
        // their original 48 kHz; the older discs had a voice index the US program could not use: no voice line at all, a stuck game)
        // v0.6.7 adds the Japanese textures (both versions on the disc, switched in the game): a v4 disc without them still
        // plays well, it only lacks those two options.
        // v0.7.5 adds the sound banks with voices in them (the heroes' shouts, wake-up and recorder lines, four scenes' banks), in
        // Japanese with the English copies switched with the voices: a disc without them is made again.
        // v0.8.1 adds the Japanese logo and opening film (textures_logos, textures_movies): a disc without them is made again.
        public static bool CurrentDisc(Settings s) { return Layout(s).Contains("version = 4") && HasTextures(s) && Layout(s).Contains("sounds_jp_slots") && Layout(s).Contains("textures_movies") && Layout(s).Contains("media = 2"); }
        public static bool WorkingDisc(Settings s) { return Layout(s).Contains("version = 4"); }
        public static bool HasTextures(Settings s) { return Layout(s).Contains("textures_faces"); }

        static string Layout(Settings s)
        {
            string layout = Path.Combine(Games.Folder(s, Name), "hybrid.iso.layout");
            try { return File.Exists(layout) ? File.ReadAllText(layout) : ""; }
            catch (Exception) { return ""; }
        }

        static DiscInfo Find(List<DiscInfo> discs, string prefix)
        {
            foreach (DiscInfo d in discs) if (d.Error == "" && d.GameName.StartsWith(prefix, StringComparison.OrdinalIgnoreCase)) return d;
            return null;
        }

        public static DiscInfo Japanese(List<DiscInfo> discs) { return Find(discs, "ZZT2-JP"); }

        // What is still missing, in the words the file dialog shows.
        public static List<string> Missing(List<DiscInfo> discs)
        {
            List<string> list = new List<string>();
            if (Find(discs, "RawDanger-US") == null) list.Add(Lang.L("Raw Danger! (USA) - SLUS-21501", "Raw Danger!（北米版）SLUS-21501"));
            if (Find(discs, "ZZT2-JP") == null) list.Add(Lang.L("Zettai Zetsumei Toshi 2 (Japan) - SLPS-25606", "絶体絶命都市2（日本版）SLPS-25606"));
            if (Find(discs, "ZZT2-DemoA") == null) list.Add(Lang.L("Zettai Zetsumei Toshi 2 demo Type-A (.cue)", "絶体絶命都市2 体験版 Type-A（.cue）"));
            if (Find(discs, "ZZT2-DemoB") == null) list.Add(Lang.L("Zettai Zetsumei Toshi 2 demo Type-B (.cue)", "絶体絶命都市2 体験版 Type-B（.cue）"));
            return list;
        }

        public static DiscInfo Make(List<DiscInfo> discs, Settings settings)
        {
            DiscInfo us = Find(discs, "RawDanger-US"), jp = Find(discs, "ZZT2-JP"), a = Find(discs, "ZZT2-DemoA"), b = Find(discs, "ZZT2-DemoB");
            DiscInfo plus = new DiscInfo();
            plus.GameName = Name;
            plus.Region = Lang.L("US + JP", "北米 + 日本");
            plus.Title = Lang.L("Raw Danger! Enhanced Edition", "Raw Danger! 強化版");
            List<string> missing = new List<string>();
            if (us == null) missing.Add(Lang.L("Raw Danger! (USA)", "Raw Danger!（北米版）"));
            if (jp == null) missing.Add(Lang.L("Zettai Zetsumei Toshi 2 (Japan)", "絶体絶命都市2（日本版）"));
            if (a == null) missing.Add(Lang.L("demo Type-A", "体験版 Type-A"));
            if (b == null) missing.Add(Lang.L("demo Type-B", "体験版 Type-B"));
            if (us != null)
            {
                plus.ImagePath = us.ImagePath; plus.DataPath = us.DataPath; plus.BootProgram = us.BootProgram; plus.Serial = us.Serial;
                plus.Crc = us.Crc; plus.PnachPath = us.PnachPath; plus.Sections.AddRange(us.Sections);
            }
            if (missing.Count > 0) plus.Error = Lang.L("needs all four discs; missing: ", "4枚すべてのディスクが必要です。不足: ") + string.Join(", ", missing.ToArray());
            return plus;
        }

        public static string Describe(DiscInfo d)
        {
            StringBuilder sb = new StringBuilder();
            sb.AppendLine(Lang.L("Built from a new disc made from your US and JP images: the US game (English text) with both voice tracks, Japanese (original 48 kHz recordings) and English, switched in the game (F1 -> Game options). The heroes' shouts and calls (the sound banks with voices in them) are Japanese too, and switch with the voices. The 137 JP-only lines are on the disc too, but the US script does not ask for them. Also from the JP disc: the 151 textures the US release redrew, in both versions, switched in the game (F1 -> Game options): the characters' original faces, hair and clothes colours, and the writing in the world in Japanese. Also: fast-loading fix, widescreen 16:9, no intro logos, offline achievements, internal resolution and modern filters (F1 -> Graphics and sound).",
                                 "北米版と日本版のイメージから新しいディスクを作ってビルドします：英語テキストの北米版に日本語と英語の両方の音声を収録し、ゲーム内（F1 →「Game options」）で切り替えられます。日本版のみの台詞（主人公の名前を呼ぶ声）もディスクに入れますが、北米版のスクリプトはまだ再生しません。日本版ディスクからは、北米版で描き直された151枚のテクスチャも両方収録し、ゲーム内（F1 →「Game options」）で切り替えられます：キャラクター本来の顔・髪・服の色、街の文字（日本語）。さらに高速ロード修正、ワイド画面 16:9、起動ロゴのスキップ、オフライン実績、内部解像度と最新のフィルター（F1 →「Graphics and sound」）。"));
            if (d.Error != "") sb.Append(d.Error);
            return sb.ToString();
        }

        // After the build: the enhanced copy starts at 2x the console's resolution unless the player already chose one.
        public static void Settle(string folder)
        {
            string keys = Path.Combine(folder, "keys.ini");
            try
            {
                string text = File.Exists(keys) ? File.ReadAllText(keys, Encoding.UTF8) : "";
                if (Regex.IsMatch(text, @"(?m)^\s*scale\s*=")) return;
                text = (text.Length > 0 && !text.EndsWith("\n") ? text + "\r\n" : text) + "scale = 2\r\n";
                File.WriteAllText(keys, text, new UTF8Encoding(false));
            }
            catch (Exception e) { Program.Trace("enhanced keys.ini: " + e.Message); }
        }
    }
}
