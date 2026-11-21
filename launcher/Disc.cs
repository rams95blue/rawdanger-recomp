// Raw Danger Launcher - disc image reading (ISO 9660 inside .iso, .bin/.cue with 2352-byte sectors).
// C# 5 only (the csc.exe of .NET Framework 4 that every Windows 10/11 has must be able to build it).
using System;
using System.Collections.Generic;
using System.IO;
using System.Text;

namespace RawDangerLauncher
{
    // One disc image: what it is (from SYSTEM.CNF and the boot program) and how to build it.
    public class DiscInfo
    {
        public bool Modified;   // a known disc changed by someone (an undub): listed with a warning, not built
        public string ImagePath = "";      // the file the player has (.iso, .cue or .bin)
        public string DataPath = "";       // the file with the sectors (.iso or .bin)
        public string BootProgram = "";    // SLUS_215.01
        public string Serial = "";         // SLUS-21501
        public uint Crc;                   // PCSX2's CRC of the boot program: every 32-bit word XORed
        public string Region = "";         // US, JP, JP demo
        public string Title = "";
        public string GameName = "";       // folder name in ps2-native-recomp\games (letters, digits, - and _)
        public string PnachPath = "";      // our patch file for this disc, or ""
        public List<string> Sections = new List<string>();
        public string Error = "";

        public string CrcText { get { return Crc.ToString("X8"); } }
    }

    public static class Disc
    {
        const int Sector = 2048;

        // The image's sector layout: data file, bytes per stored sector, offset of the 2048 data bytes inside it.
        class Layout
        {
            public string File;
            public int Stride = Sector;
            public int Offset;
        }

        public static string CueTarget(string cue)
        {
            foreach (string raw in System.IO.File.ReadAllLines(cue))
            {
                string line = raw.Trim();
                if (!line.StartsWith("FILE", StringComparison.OrdinalIgnoreCase)) continue;
                string rest = line.Substring(4).Trim();
                string name;
                if (rest.StartsWith("\""))
                {
                    int end = rest.IndexOf('"', 1);
                    if (end < 0) continue;
                    name = rest.Substring(1, end - 1);
                }
                else
                {
                    int space = rest.IndexOf(' ');
                    name = space < 0 ? rest : rest.Substring(0, space);
                }
                string path = Path.Combine(Path.GetDirectoryName(cue), name);
                return System.IO.File.Exists(path) ? path : "";
            }
            return "";
        }

        static Layout Open(string image)
        {
            Layout layout = new Layout();
            layout.File = image;
            if (image.EndsWith(".cue", StringComparison.OrdinalIgnoreCase))
            {
                layout.File = CueTarget(image);
                if (layout.File == "") throw new IOException("the .cue file names a .bin file that is not next to it");
            }
            using (FileStream fs = new FileStream(layout.File, FileMode.Open, FileAccess.Read, FileShare.Read))
            {
                // A raw CD sector starts with the sync pattern 00 FF*10 00; byte 15 is the mode (1: data at 16, 2: data at 24).
                byte[] head = new byte[16];
                fs.Position = 16L * 2352;
                if (fs.Read(head, 0, 16) == 16 && head[0] == 0 && head[11] == 0)
                {
                    bool sync = true;
                    for (int i = 1; i <= 10; i++) if (head[i] != 0xFF) sync = false;
                    if (sync)
                    {
                        layout.Stride = 2352;
                        layout.Offset = head[15] == 1 ? 16 : 24;
                    }
                }
            }
            return layout;
        }

        static byte[] Read(FileStream fs, Layout layout, long lba, int count)
        {
            byte[] data = new byte[count * Sector];
            for (int i = 0; i < count; i++)
            {
                fs.Position = (lba + i) * layout.Stride + layout.Offset;
                int got = 0;
                while (got < Sector)
                {
                    int n = fs.Read(data, i * Sector + got, Sector - got);
                    if (n <= 0) throw new IOException("the image ends early (sector " + (lba + i) + ")");
                    got += n;
                }
            }
            return data;
        }

        static uint U32(byte[] b, int at) { return (uint)(b[at] | b[at + 1] << 8 | b[at + 2] << 16 | b[at + 3] << 24); }

        // The extent (first sector, size) of a file, its path split on '\' or '/', from the root directory down.
        static bool Find(FileStream fs, Layout layout, string path, out uint lba, out uint size)
        {
            byte[] pvd = Read(fs, layout, 16, 1);
            if (Encoding.ASCII.GetString(pvd, 1, 5) != "CD001") throw new IOException("no ISO 9660 file system (not a PS2 disc image?)");
            lba = U32(pvd, 156 + 2);
            size = U32(pvd, 156 + 10);
            string[] parts = path.Split(new char[] { '\\', '/' }, StringSplitOptions.RemoveEmptyEntries);
            for (int p = 0; p < parts.Length; p++)
            {
                byte[] dir = Read(fs, layout, lba, (int)((size + Sector - 1) / Sector));
                bool found = false;
                int at = 0;
                while (at < dir.Length)
                {
                    int len = dir[at];
                    if (len == 0) { at = (at / Sector + 1) * Sector; continue; }   // records do not cross a sector
                    int nameLen = dir[at + 32];
                    string name = Encoding.ASCII.GetString(dir, at + 33, nameLen);
                    int semi = name.IndexOf(';');
                    if (semi >= 0) name = name.Substring(0, semi);
                    if (name.EndsWith(".")) name = name.Substring(0, name.Length - 1);
                    if (string.Equals(name, parts[p], StringComparison.OrdinalIgnoreCase))
                    {
                        lba = U32(dir, at + 2);
                        size = U32(dir, at + 10);
                        found = true;
                        break;
                    }
                    at += len;
                }
                if (!found) return false;
            }
            return true;
        }

        public static long FileSize(string image, string path)
        {
            Layout layout = Open(image);
            using (FileStream fs = new FileStream(layout.File, FileMode.Open, FileAccess.Read, FileShare.Read))
            {
                uint lba, size;
                return Find(fs, layout, path, out lba, out size) ? (long)size : -1L;
            }
        }

        public static byte[] ReadFile(string image, string path)
        {
            Layout layout = Open(image);
            using (FileStream fs = new FileStream(layout.File, FileMode.Open, FileAccess.Read, FileShare.Read))
            {
                uint lba, size;
                if (!Find(fs, layout, path, out lba, out size)) return null;
                byte[] data = Read(fs, layout, lba, (int)((size + Sector - 1) / Sector));
                byte[] exact = new byte[size];
                Array.Copy(data, exact, size);
                return exact;
            }
        }

        // PCSX2's CRC of a program: every 32-bit word of the file XORed together. Names the .pnach file of the game.
        public static uint ElfCrc(byte[] elf)
        {
            uint crc = 0;
            for (int i = 0; i + 4 <= elf.Length; i += 4) crc ^= U32(elf, i);
            return crc;
        }

        // Reads SYSTEM.CNF and the boot program. Fills BootProgram, Serial, Crc; leaves Error set when it cannot.
        public static DiscInfo Identify(string image)
        {
            DiscInfo d = new DiscInfo();
            d.ImagePath = image;
            try
            {
                Layout layout = Open(image);
                d.DataPath = layout.File;
                byte[] cnf = ReadFile(image, "SYSTEM.CNF");
                if (cnf == null) { d.Error = "no SYSTEM.CNF on this disc"; return d; }
                string boot = "";
                foreach (string line in Encoding.ASCII.GetString(cnf).Split('\n'))
                {
                    string t = line.Trim();
                    if (t.StartsWith("BOOT2", StringComparison.OrdinalIgnoreCase) && t.Contains("="))
                    {
                        boot = t.Substring(t.IndexOf('=') + 1).Trim();
                        break;
                    }
                }
                if (boot == "") { d.Error = "SYSTEM.CNF has no BOOT2 line (not a PS2 game disc?)"; return d; }
                string program = boot;
                int colon = program.IndexOf(':');
                if (colon >= 0) program = program.Substring(colon + 1);
                int semi = program.IndexOf(';');
                if (semi >= 0) program = program.Substring(0, semi);
                program = program.TrimStart('\\', '/');
                d.BootProgram = program.Substring(program.LastIndexOfAny(new char[] { '\\', '/' }) + 1);
                byte[] elf = ReadFile(image, program);
                if (elf == null) { d.Error = "the boot program " + program + " is not on the disc"; return d; }
                d.Crc = ElfCrc(elf);
                // SLUS_215.01 -> SLUS-21501, the form PCSX2 patch files use.
                string s = d.BootProgram.Replace(".", "");
                d.Serial = s.Length > 4 && s[4] == '_' ? s.Substring(0, 4) + "-" + s.Substring(5) : s;
                // A US disc whose voice archive is not the retail one: an "undub" (JP voices put into the US disc). Not
                // supported: its voice index does not match the US program the patches were made for, and the Enhanced
                // Edition makes its own JP-voice disc from the two retail discs.
                if (d.Serial == "SLUS-21501")
                {
                    long fat = FileSize(image, "STR/STR.FAT"), dat = FileSize(image, "STR/STR.DAT");
                    if (fat != 203173 || dat != 781946880)
                    {
                        d.Modified = true;
                        d.Error = Lang.L("modified US disc (an undub / JP-voice mod?): not supported. Use the retail US disc; the Enhanced Edition adds the Japanese voices itself.",
                                         "改造された北米版ディスク（音声差し替え版？）：非対応です。市販の北米版ディスクを使ってください。日本語音声は強化版が自動で収録します。");
                    }
                }
            }
            catch (Exception e)
            {
                d.Error = e.Message;
            }
            return d;
        }
    }
}
