// Raw Danger PC - background music for the launcher: a music file of the player's own (.flac / .mp3 / .wav / .wma / .m4a,
// any name; music.* first) in the art folder, like the cast pictures (CastData.FindAudio), looped through Windows' MCI player
// at the volume in launcher.ini (MusicVolume=0.25), carried on across a language change. A .flac is decoded
// once to a WAV in the temp folder (Flac.cs; MCI cannot play FLAC). Nothing is bundled.
// The header's music button switches it on and off; the choice is kept in launcher.ini (Music=1/0).
using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;

namespace RawDangerLauncher
{
    public static class Music
    {
        [DllImport("winmm.dll", CharSet = CharSet.Unicode)]
        static extern int mciSendString(string command, StringBuilder ret, int retLength, IntPtr callback);

        static string file = "";
        static bool open, playing;

        public static string File { get { return file; } }
        public static bool Playing { get { return playing; } }

        static int volume = 250;   // MCI: 0 ... 1000

        public static void Find(Settings s)
        {
            double v;
            if (double.TryParse(s.MusicVolume, System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out v))
                volume = (int)Math.Round(Math.Max(0.0, Math.Min(1.0, v)) * 1000);
            if (open) { Send("setaudio rdbgm volume to " + volume); return; }   // still playing from before a language change: keep it
            file = CastData.FindAudio(s);
            Program.Trace("music: " + (file == "" ? "no music file (.flac / .mp3 / .wav / .wma / .m4a, any name) in the art folder" : file));
        }

        static int Send(string command)
        {
            try { return mciSendString(command, null, 0, IntPtr.Zero); }
            catch (Exception e) { Program.Trace("music: " + e.Message); return -1; }   // no winmm (not Windows)
        }

        static readonly object gate = new object();
        static bool wanted, decoding;
        static System.Threading.SynchronizationContext ui;

        // The file MCI plays: the music file itself, or for a .flac its decoded WAV (empty while that is still being made).
        static string Playable()
        {
            if (!file.EndsWith(".flac", StringComparison.OrdinalIgnoreCase)) return file;
            FileInfo fi = new FileInfo(file);
            string dir = Path.Combine(Path.GetTempPath(), "RawDangerPC");
            string wav = Path.Combine(dir, "music_" + fi.Length + "_" + fi.LastWriteTimeUtc.Ticks + ".wav");
            if (System.IO.File.Exists(wav)) return wav;
            if (!decoding)
            {
                decoding = true;
                ui = System.Threading.SynchronizationContext.Current;
                System.Threading.Thread t = new System.Threading.Thread((System.Threading.ThreadStart)delegate()
                {
                    try
                    {
                        Directory.CreateDirectory(dir);
                        foreach (string old in Directory.GetFiles(dir, "music_*.wav")) try { System.IO.File.Delete(old); } catch (Exception) { }
                        Flac.ToWav(file, wav + ".part");
                        System.IO.File.Move(wav + ".part", wav);
                        Program.Trace("music: decoded " + file);
                    }
                    catch (Exception e) { Program.Trace("music: cannot decode " + file + ": " + e.Message); file = ""; }
                    // MCI is used from the window's thread only (a device opened on this short-lived thread could stop with it)
                    System.Threading.SendOrPostCallback go = delegate { lock (gate) { decoding = false; if (wanted && file != "") Start(wav); } };
                    if (ui != null) ui.Post(go, null); else go(null);
                });
                t.IsBackground = true;
                t.Start();
            }
            return "";
        }

        public static int Volume { get { return volume; } }

        // the launcher's volume bar: 0 ... 1000, applied at once when the music is open, kept in launcher.ini by the caller
        public static void SetVolume(int v)
        {
            volume = Math.Max(0, Math.Min(1000, v));
            if (open) Send("setaudio rdbgm volume to " + volume);
        }

        public static void Play()
        {
            lock (gate)
            {
                if (file == "" || playing) return;
                wanted = true;
                string what = Playable();
                if (what != "") Start(what);   // else it starts when the FLAC is decoded
            }
        }

        static void Start(string what)
        {
            if (open)
            {
                // paused (music button, or the game was started): go on from where it was
                playing = Send("play rdbgm repeat") == 0 || Send("play rdbgm") == 0;
                return;
            }
            // mpegvideo (DirectShow) plays WAV too and, unlike waveaudio, can repeat
            if (Send("open \"" + what + "\" type mpegvideo alias rdbgm") != 0 && Send("open \"" + what + "\" type waveaudio alias rdbgm") != 0) { Program.Trace("music: cannot open " + what); return; }
            open = true;
            Send("setaudio rdbgm volume to " + volume);
            playing = Send("play rdbgm from 0 repeat") == 0 || Send("play rdbgm from 0") == 0;
        }

        public static void Stop()
        {
            lock (gate)
            {
                wanted = false;
                if (!open) return;
                Send("stop rdbgm");
                playing = false;
            }
        }

        public static void Close()
        {
            wanted = false;
            if (!open) return;
            Send("close rdbgm");
            open = playing = false;
        }
    }
}
