// Claude token LED — launcher for the VS Code Claude extension (claudeCode.claudeProcessWrapper).
// The extension calls it as: token_tap.exe <claude.exe> <arguments...>
// It starts claude.exe unchanged, passes its output through byte for byte, and meanwhile reads the
// stream_event lines (the live token stream) and tells the LED over UDP. It also notices when Claude
// asks the user for approval (control_request / can_use_tool), when a turn ends ("result": finished / error)
// and what the usage limits are (rate_limit_event). No proxy, no background process: if the
// LED or the network is unreachable, Claude works exactly the same.
//
// Build (the .NET Framework is on every Windows):
//   C:\Windows\Microsoft.NET\Framework64\v4.0.30319\csc.exe /nologo /optimize /r:System.Web.Extensions.dll token_tap.cs
// LED address: TOKENLED_HOST environment variable, default tokenled.local
// (UDP 4210, "T <type> <tokens>", "A 1", "D <0|1>", "U <5h%> <7d%> <5h reset> <7d reset>")

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
    const int MAX_LINE = 65536;           // stream_event lines are small; longer ones (tool results) are not parsed
    const int CHARS_PER_TOKEN = 4;        // live estimate; the real output_tokens at the end of a message fixes the difference

    // ---------- Start and pass-through ----------
    static int Main()
    {
        string cl = Environment.CommandLine;
        int i = 0;
        NextToken(cl, ref i);                // ourselves
        string exe = NextToken(cl, ref i);   // the extension's claude.exe; the other arguments go on untouched
        if (exe.Length == 0)
        {
            Console.Error.WriteLine("token_tap: path of claude.exe is missing");
            return 2;
        }

        var psi = new ProcessStartInfo(exe, cl.Substring(i)) { UseShellExecute = false, RedirectStandardOutput = true, RedirectStandardInput = true };
        Process p = Process.Start(psi);
        KillWithUs(p);
        new Thread(() => PumpStdin(p)) { IsBackground = true }.Start();

        Stream src = p.StandardOutput.BaseStream, dst = Console.OpenStandardOutput();
        var buf = new byte[65536];
        int n;
        while ((n = src.Read(buf, 0, buf.Length)) > 0)
        {
            dst.Write(buf, 0, n);
            dst.Flush();
            try { Feed(buf, n); } catch { }   // a failure of the LED tap must never affect Claude
        }
        p.WaitForExit();
        waiting = false;
        if (sender != null) Thread.Sleep(150);   // let the last packets out
        return p.ExitCode;
    }

    // ---------- Input to claude.exe: passed through byte for byte; the user's answer to an approval ends the wait ----------
    static readonly MemoryStream inLine = new MemoryStream();

    static void PumpStdin(Process p)
    {
        try
        {
            Stream inp = Console.OpenStandardInput(), outp = p.StandardInput.BaseStream;
            var buf = new byte[65536];
            int n;
            while ((n = inp.Read(buf, 0, buf.Length)) > 0)
            {
                outp.Write(buf, 0, n);
                outp.Flush();
                try { ScanInput(buf, n); } catch { }
            }
        }
        catch { }
        try { p.StandardInput.Close(); } catch { }
    }

    static void ScanInput(byte[] b, int n)
    {
        for (int k = 0; k < n; k++)
        {
            if (b[k] == (byte)'\n')
            {
                string head = Encoding.UTF8.GetString(inLine.GetBuffer(), 0, (int)inLine.Length);
                if (head.IndexOf("\"control_response\"", StringComparison.Ordinal) >= 0) waiting = false;   // approved or denied
                inLine.SetLength(0);
            }
            else if (inLine.Length < 400) inLine.WriteByte(b[k]);
        }
    }

    // One Windows command-line token (the program name and a path: quoted, or up to the next space).
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

    // ---------- claude.exe must not outlive us (libuv does the same) ----------
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

    static IntPtr job;   // stays open until the process ends; closing it stops claude.exe too

    static void KillWithUs(Process p)
    {
        job = CreateJobObject(IntPtr.Zero, null);
        var info = new ExtendedLimits();
        info.Basic.LimitFlags = 0x2000 | 0x1000;   // KILL_ON_JOB_CLOSE | SILENT_BREAKAWAY_OK (claude's children stay free)
        SetInformationJobObject(job, 9, ref info, Marshal.SizeOf(typeof(ExtendedLimits)));
        AssignProcessToJobObject(job, p.Handle);
    }

    // ---------- Live token stream from the output ----------
    // Characters of one message and the tokens already sent for them, per type
    // (T/ET: text, K/EK: thinking, J/EJ: tool input).
    class Msg { public long T, K, J, ET, EK, EJ, Sent; public bool Thinking; }

    static readonly JavaScriptSerializer json = new JavaScriptSerializer { MaxJsonLength = int.MaxValue };
    static readonly MemoryStream line = new MemoryStream();
    static readonly Dictionary<string, Msg> msgs = new Dictionary<string, Msg>();   // main thread and subagents separately
    static bool lineTooLong;
    static volatile bool waiting;   // Claude waits for the user's approval
    static volatile int doneReq = -1;           // turn finished: 0 ok, 1 error, -1 nothing to send
    static volatile int use5 = -1, use7 = -1;   // usage limits in percent
    static volatile int reset5, reset7;         // when those windows reset (Unix time, 0 = unknown)

    static void Feed(byte[] b, int n)
    {
        for (int k = 0; k < n; k++)
        {
            if (b[k] == (byte)'\n')
            {
                Line(Encoding.UTF8.GetString(line.GetBuffer(), 0, (int)line.Length), !lineTooLong);
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

    static void Line(string s, bool complete)
    {
        // Approval: Claude asks (control_request / can_use_tool); anything it prints afterwards means it was answered.
        string head = s.Length > 300 ? s.Substring(0, 300) : s;
        if (head.IndexOf("\"control_request\"", StringComparison.Ordinal) >= 0 &&
            head.IndexOf("\"can_use_tool\"", StringComparison.Ordinal) >= 0)
        {
            SetWaiting(true);
            return;
        }
        if (head.IndexOf("\"type\":\"user\"", StringComparison.Ordinal) >= 0 ||
            head.IndexOf("\"type\":\"result\"", StringComparison.Ordinal) >= 0 ||
            head.IndexOf("\"control_cancel_request\"", StringComparison.Ordinal) >= 0)
            waiting = false;

        // End of a turn: "result" (is_error tells success from failure).
        if (head.IndexOf("\"type\":\"result\"", StringComparison.Ordinal) >= 0)
        {
            doneReq = s.IndexOf("\"is_error\":true", StringComparison.Ordinal) >= 0 ? 1 : 0;
            EnsureSender();
            return;
        }

        // Usage limits: rate_limit_event carries the utilisation (0..1) and reset time of the 5-hour and 7-day windows.
        if (complete && head.IndexOf("\"rate_limit_event\"", StringComparison.Ordinal) >= 0)
        {
            object w = Get(Get(Get(json.DeserializeObject(s), "rate_limit_info"), "unifiedWindows"), "five_hour");
            object w7 = Get(Get(Get(json.DeserializeObject(s), "rate_limit_info"), "unifiedWindows"), "seven_day");
            object u5 = Get(w, "utilization"), u7 = Get(w7, "utilization");
            if (u5 != null) use5 = (int)Math.Round(Convert.ToDouble(u5) * 100);
            if (u7 != null) use7 = (int)Math.Round(Convert.ToDouble(u7) * 100);
            if (Get(w, "resetsAt") != null) reset5 = (int)Num(w, "resetsAt");
            if (Get(w7, "resetsAt") != null) reset7 = (int)Num(w7, "resetsAt");
            EnsureSender();
            return;
        }

        if (!complete || s.IndexOf("\"stream_event\"", StringComparison.Ordinal) < 0) return;
        object o = json.DeserializeObject(s);
        if ((Get(o, "type") as string) != "stream_event") return;
        object ev = Get(o, "event");
        string key = (Get(o, "parent_tool_use_id") as string) ?? "";
        Msg m;
        if (!msgs.TryGetValue(key, out m)) msgs[key] = m = new Msg();

        switch (Get(ev, "type") as string)
        {
            case "message_start":
                waiting = false;
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
                    // Redacted thinking streams no text, only "estimated_tokens" (tokens added since the last frame).
                    case "thinking_delta":   m.K += Len(d, "thinking") + Num(d, "estimated_tokens") * CHARS_PER_TOKEN; m.EK += Emit(m, 'k', m.K, m.EK); break;
                    case "input_json_delta": m.J += Len(d, "partial_json"); m.EJ += Emit(m, 'j', m.J, m.EJ); break;
                }
                break;
            case "message_delta":   // the real output_tokens: tops up what the estimate missed
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

    // Sends the estimated tokens of this type that were not sent yet; returns how many.
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

    // ---------- Sending to the LED (own thread, so nothing slows the pass-through down) ----------
    static readonly object gate = new object();
    static readonly Dictionary<char, long> pending = new Dictionary<char, long>();
    static Thread sender;

    static void EnsureSender()
    {
        if (sender != null) return;
        sender = new Thread(Sender) { IsBackground = true };
        sender.Start();
    }

    static void Add(char type, long n)
    {
        if (n <= 0) return;
        lock (gate)
        {
            long v;
            pending.TryGetValue(type, out v);
            pending[type] = v + n;
        }
        EnsureSender();
    }

    static void SetWaiting(bool on)
    {
        waiting = on;
        if (on) EnsureSender();
    }

    static void Sender()
    {
        string host = Environment.GetEnvironmentVariable("TOKENLED_HOST") ?? "tokenled.local";
        var udp = new UdpClient();
        IPEndPoint ep = null;
        DateTime nextResolve = DateTime.MinValue, nextPing = DateTime.MinValue, nextWait = DateTime.MinValue, nextUse = DateTime.MinValue;
        bool sentWaiting = false;
        int sent5 = -2, sent7 = -2, sentR5 = 0, sentR7 = 0;
        while (true)
        {
            DateTime now = DateTime.UtcNow;
            if (now >= nextResolve)
            {
                try   // if resolving fails, the last address is kept
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
                bool w = waiting;
                if (w && now >= nextWait) { sb.Append("A 1\n"); nextWait = now.AddSeconds(1); }   // repeated: the LED expires it
                if (!w && sentWaiting) sb.Append("A 0\n");
                sentWaiting = w;
                int d = doneReq;
                if (d >= 0) { doneReq = -1; sb.Append("D ").Append(d).Append('\n'); }
                int a = use5, b = use7, r5 = reset5, r7 = reset7;
                if ((a != sent5 || b != sent7 || r5 != sentR5 || r7 != sentR7 || now >= nextUse) && (a >= 0 || b >= 0))
                {
                    sb.Append("U ").Append(a).Append(' ').Append(b).Append(' ').Append(r5).Append(' ').Append(r7).Append('\n');   // repeated: the LED forgets old values
                    sent5 = a; sent7 = b; sentR5 = r5; sentR7 = r7; nextUse = now.AddSeconds(60);
                }
                if (sb.Length > 0)
                {
                    try { byte[] data = Encoding.ASCII.GetBytes(sb.ToString()); udp.Send(data, data.Length, ep); }
                    catch { }
                }
            }
            Thread.Sleep(20);
        }
    }
}
