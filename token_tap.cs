// Claude token LED — indító a VS Code Claude-bővítményhez (claudeCode.claudeProcessWrapper).
// A bővítmény így hívja: token_tap.exe <claude.exe> <paraméterek...>
// Változatlanul elindítja a claude.exe-t, a kimenetét bájtra pontosan továbbadja, közben a
// stream_event sorokból (élő tokenfolyam) UDP-n szól a LED-nek. Nincs proxy és háttérfolyamat:
// ha a LED vagy a hálózat nem elérhető, a Claude ugyanúgy működik.
//
// Fordítás (a .NET Framework minden Windowson megvan):
//   C:\Windows\Microsoft.NET\Framework64\v4.0.30319\csc.exe /nologo /optimize /r:System.Web.Extensions.dll token_tap.cs
// LED címe: TOKENLED_HOST környezeti változó, alapból tokenled.local (UDP 4210, "T <típus> <token>")

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Net;
using System.Net.Sockets;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using System.Web.Script.Serialization;

static class TokenTap
{
    const int LED_PORT = 4210;
    const int MAX_LINE = 65536;           // a stream_event sorok kicsik; a hosszabbakat (tool-eredmény) nem bontja ki
    const int CHARS_PER_TOKEN = 4;        // élő becslés; az üzenet végén a valódi output_tokens pótolja a különbséget

    // ---------- Indítás és továbbítás ----------
    static int Main()
    {
        string cl = Environment.CommandLine;
        int i = 0;
        NextToken(cl, ref i);                // saját magunk
        string exe = NextToken(cl, ref i);   // a bővítmény claude.exe-je; a többi paraméter érintetlenül megy tovább
        if (exe.Length == 0)
        {
            Console.Error.WriteLine("token_tap: hiányzik a claude.exe útvonala");
            return 2;
        }

        var psi = new ProcessStartInfo(exe, cl.Substring(i)) { UseShellExecute = false, RedirectStandardOutput = true };
        Process p = Process.Start(psi);
        KillWithUs(p);

        Stream src = p.StandardOutput.BaseStream, dst = Console.OpenStandardOutput();
        var buf = new byte[65536];
        int n;
        while ((n = src.Read(buf, 0, buf.Length)) > 0)
        {
            dst.Write(buf, 0, n);
            dst.Flush();
            try { Feed(buf, n); } catch { }   // a LED-figyelés hibája sosem érintheti a Claude-ot
        }
        p.WaitForExit();
        if (sender != null) Thread.Sleep(150);   // az utolsó tokenek még kimenjenek
        return p.ExitCode;
    }

    // Windows parancssori token (a program neve és egy útvonal: idézőjelben vagy szóközig).
    static string NextToken(string s, ref int i)
    {
        while (i < s.Length && char.IsWhiteSpace(s[i])) i++;
        int start;
        string tok;
        if (i < s.Length && s[i] == '"')
        {
            start = ++i;
            while (i < s.Length && s[i] != '"') i++;
            tok = s.Substring(start, i - start);
            if (i < s.Length) i++;
        }
        else
        {
            start = i;
            while (i < s.Length && !char.IsWhiteSpace(s[i])) i++;
            tok = s.Substring(start, i - start);
        }
        while (i < s.Length && char.IsWhiteSpace(s[i])) i++;
        return tok;
    }

    // ---------- A claude.exe ne éljen túl minket (a libuv is így csinálja) ----------
    [StructLayout(LayoutKind.Sequential)]
    struct BasicLimits
    {
        public long PerProcessUserTimeLimit, PerJobUserTimeLimit;
        public uint LimitFlags;
        public UIntPtr MinimumWorkingSetSize, MaximumWorkingSetSize;
        public uint ActiveProcessLimit;
        public UIntPtr Affinity;
        public uint PriorityClass, SchedulingClass;
    }

    [StructLayout(LayoutKind.Sequential)]
    struct ExtendedLimits
    {
        public BasicLimits Basic;
        public ulong ReadOps, WriteOps, OtherOps, ReadBytes, WriteBytes, OtherBytes;
        public UIntPtr ProcessMemoryLimit, JobMemoryLimit, PeakProcessMemoryUsed, PeakJobMemoryUsed;
    }

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
    static extern IntPtr CreateJobObject(IntPtr attrs, string name);
    [DllImport("kernel32.dll")]
    static extern bool SetInformationJobObject(IntPtr job, int infoClass, ref ExtendedLimits info, int size);
    [DllImport("kernel32.dll")]
    static extern bool AssignProcessToJobObject(IntPtr job, IntPtr process);

    static IntPtr job;   // a folyamat végéig nyitva marad; bezáródásakor a claude.exe is leáll

    static void KillWithUs(Process p)
    {
        job = CreateJobObject(IntPtr.Zero, null);
        var info = new ExtendedLimits();
        info.Basic.LimitFlags = 0x2000 | 0x1000;   // KILL_ON_JOB_CLOSE | SILENT_BREAKAWAY_OK (a claude gyerekei szabadok)
        SetInformationJobObject(job, 9, ref info, Marshal.SizeOf(typeof(ExtendedLimits)));
        AssignProcessToJobObject(job, p.Handle);
    }

    // ---------- Élő tokenfolyam a kimenetből ----------
    // Egy üzenet karakterei és az ezekből már elküldött tokenek típusonként (T/ET: szöveg, K/EK: thinking, J/EJ: tool).
    class Msg { public long T, K, J, ET, EK, EJ, Sent; public bool Thinking; }

    static readonly JavaScriptSerializer json = new JavaScriptSerializer { MaxJsonLength = int.MaxValue };
    static readonly MemoryStream line = new MemoryStream();
    static readonly Dictionary<string, Msg> msgs = new Dictionary<string, Msg>();   // fő szál és subagentek külön
    static bool lineTooLong;

    static void Feed(byte[] b, int n)
    {
        for (int k = 0; k < n; k++)
        {
            if (b[k] == (byte)'\n')
            {
                if (!lineTooLong) Line(Encoding.UTF8.GetString(line.GetBuffer(), 0, (int)line.Length));
                line.SetLength(0);
                lineTooLong = false;
            }
            else if (line.Length < MAX_LINE) line.WriteByte(b[k]);
            else lineTooLong = true;
        }
    }

    static object Get(object o, string key)
    {
        var d = o as Dictionary<string, object>;
        object v;
        return d != null && d.TryGetValue(key, out v) ? v : null;
    }

    static long Num(object o, string key)
    {
        object v = Get(o, key);
        return v == null ? 0 : Convert.ToInt64(v);
    }

    static int Len(object o, string key)
    {
        var s = Get(o, key) as string;
        return s == null ? 0 : s.Length;
    }

    static void Line(string s)
    {
        if (s.IndexOf("\"stream_event\"", StringComparison.Ordinal) < 0) return;
        object o = json.DeserializeObject(s);
        if ((Get(o, "type") as string) != "stream_event") return;
        object ev = Get(o, "event");
        string key = (Get(o, "parent_tool_use_id") as string) ?? "";
        Msg m;
        if (!msgs.TryGetValue(key, out m)) msgs[key] = m = new Msg();

        switch (Get(ev, "type") as string)
        {
            case "message_start":
                msgs[key] = m = new Msg();
                object u = Get(Get(ev, "message"), "usage");
                Add('i', Num(u, "input_tokens") + Num(u, "cache_creation_input_tokens"));
                Add('r', Num(u, "cache_read_input_tokens"));
                break;
            case "content_block_start":
                string ct = Get(Get(ev, "content_block"), "type") as string;
                if (ct == "thinking" || ct == "redacted_thinking") m.Thinking = true;
                break;
            case "content_block_delta":
                object d = Get(ev, "delta");
                switch (Get(d, "type") as string)
                {
                    case "text_delta":       m.T += Len(d, "text");         m.ET += Emit(m, 't', m.T, m.ET); break;
                    case "thinking_delta":   m.K += Len(d, "thinking");     m.EK += Emit(m, 'k', m.K, m.EK); break;
                    case "input_json_delta": m.J += Len(d, "partial_json"); m.EJ += Emit(m, 'j', m.J, m.EJ); break;
                }
                break;
            case "message_delta":   // a valódi output_tokens: a becslésből hiányzót pótolja
                object fu = Get(ev, "usage");
                long outTok = Num(fu, "output_tokens");
                object th = Get(Get(fu, "output_tokens_details"), "thinking_tokens");
                if (th != null)
                {
                    long thinking = Convert.ToInt64(th);
                    m.EK += Fix(m, 'k', thinking - m.EK);
                    if (m.EJ > m.ET) m.EJ += Fix(m, 'j', outTok - thinking - m.ET - m.EJ);
                    else m.ET += Fix(m, 't', outTok - thinking - m.ET - m.EJ);
                }
                else Fix(m, m.Thinking ? 'k' : 't', outTok - m.Sent);
                break;
        }
    }

    // A típus eddigi karaktereiből becsült tokenek közül a még el nem küldötteket küldi; visszaadja, mennyit.
    static long Emit(Msg m, char type, long chars, long done)
    {
        long tokens = chars / CHARS_PER_TOKEN - done;
        if (tokens <= 0) return 0;
        m.Sent += tokens;
        Add(type, tokens);
        return tokens;
    }

    static long Fix(Msg m, char type, long missing)
    {
        if (missing <= 0) return 0;
        m.Sent += missing;
        Add(type, missing);
        return missing;
    }

    // ---------- Küldés a LED-nek (külön szálon, hogy a továbbítást semmi ne lassítsa) ----------
    static readonly object gate = new object();
    static readonly Dictionary<char, long> pending = new Dictionary<char, long>();
    static Thread sender;

    static void Add(char type, long n)
    {
        if (n <= 0) return;
        lock (gate)
        {
            long v;
            pending.TryGetValue(type, out v);
            pending[type] = v + n;
        }
        if (sender == null)
        {
            sender = new Thread(Sender) { IsBackground = true };
            sender.Start();
        }
    }

    static void Sender()
    {
        string host = Environment.GetEnvironmentVariable("TOKENLED_HOST") ?? "tokenled.local";
        var udp = new UdpClient();
        IPEndPoint ep = null;
        DateTime nextResolve = DateTime.MinValue, nextPing = DateTime.MinValue;
        while (true)
        {
            DateTime now = DateTime.UtcNow;
            if (now >= nextResolve)
            {
                try   // sikertelen feloldásnál a legutóbbi címet megtartja
                {
                    foreach (IPAddress a in Dns.GetHostAddresses(host))
                        if (a.AddressFamily == AddressFamily.InterNetwork) { ep = new IPEndPoint(a, LED_PORT); break; }
                }
                catch { }
                nextResolve = now.AddSeconds(ep == null ? 5 : 60);
            }
            if (ep != null)
            {
                var sb = new StringBuilder();
                lock (gate)
                {
                    foreach (var kv in pending) sb.Append("T ").Append(kv.Key).Append(' ').Append(kv.Value).Append('\n');
                    pending.Clear();
                }
                if (now >= nextPing) { sb.Append("P\n"); nextPing = now.AddSeconds(2); }
                if (sb.Length > 0)
                {
                    try { byte[] data = Encoding.ASCII.GetBytes(sb.ToString()); udp.Send(data, data.Length, ep); }
                    catch { }
                }
            }
            Thread.Sleep(50);
        }
    }
}
