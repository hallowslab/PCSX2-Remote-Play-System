using System.IO.Pipes;
using HIDMaestro;

// HIDMaestro bridge: long-lived, elevated, WINDOWLESS process that owns a virtual
// gamepad. Launched by the C++ host via CreateProcess (its manifest requires
// elevation, so UAC shows; WinExe means no console window to steal focus).
// Logs go to hidmaestro-bridge.log next to the exe.
//
//   HidMaestroBridge --cleanup          remove any leftover virtual controllers
//   HidMaestroBridge [--profile id]     install driver, create pad, connect to the
//                                       host's named pipe, read 20-byte InputPacket
//                                       frames and SubmitState them.

const string pipeName = "pcsx2rps-hidmaestro";

Log("bridge starting args=" + string.Join(" ", args));

// Host console HWND passed by the launcher. PnP/UMDF device start during
// CreateController steals foreground, so once we are done we hand focus back.
long focusHwnd = 0;
int fi = Array.IndexOf(args, "--focus");
if (fi >= 0 && fi + 1 < args.Length && long.TryParse(args[fi + 1], out long h))
    focusHwnd = h;
if (focusHwnd != 0)
    Log($"focus target: {focusHwnd}");

if (args.Contains("--cleanup")) {
    Log("removing all virtual controllers...");
    try {
        HMContext.RemoveAllVirtualControllers();
        Log("cleanup done.");
    } catch (Exception e) {
        Log("cleanup failed: " + e.Message);
    }
    return;
}

string profileId = "xbox-360-wired";
int pi = Array.IndexOf(args, "--profile");
if (pi >= 0 && pi + 1 < args.Length)
    profileId = args[pi + 1];

using var ctx = new HMContext();
ctx.LoadDefaultProfiles();
Log($"after LoadDefaultProfiles fg: {ForegroundInfo()}");

if (!ctx.IsDriverInstalled) {
    Log("installing driver...");
    ctx.InstallDriver();
    Log("driver installed.");
} else {
    Log("driver already installed.");
}

var profile = ctx.GetProfile(profileId);
if (profile == null) {
    Log($"profile '{profileId}' not found");
    return;
}

using var pad = ctx.CreateController(profile);
Log($"pad ready: {profile.Name}");
Log($"after CreateController fg: {ForegroundInfo()}");
RestoreFocus((IntPtr)focusHwnd);

using var pipe = new NamedPipeClientStream(".", pipeName, PipeDirection.In);
Log("connecting to host pipe...");
pipe.Connect(30000);
Log("connected, reading state.");
RestoreFocus((IntPtr)focusHwnd);

byte[] frame = new byte[20];
while (true) {
    int read = 0;
    while (read < frame.Length) {
        int n = pipe.Read(frame, read, frame.Length - read);
        if (n <= 0) {
            Log("pipe closed, exiting");
            return;
        }
        read += n;
    }
    pad.SubmitState(MapFrame(frame, profile));
}

static void Log(string msg) {
    try {
        string path = Path.Combine(AppContext.BaseDirectory, "hidmaestro-bridge.log");
        File.AppendAllText(path,
            DateTime.Now.ToString("HH:mm:ss.fff") + " " + msg + Environment.NewLine);
    } catch {
        // ignore logging failures
    }
}

// PnP/UMDF device start during CreateController steals foreground. The host
// passes its console HWND (--focus); once device setup is done we hand focus
// back. Being elevated, SetForegroundWindow on the (non-elevated) host window
// is permitted; AttachThreadInput makes it stick when the foreground thread
// differs from ours.
static void RestoreFocus(IntPtr hwnd) {
    if (hwnd == IntPtr.Zero)
        return;
    uint targetTid = Native.GetWindowThreadProcessId(hwnd, out _);
    uint curTid = Native.GetCurrentThreadId();
    if (targetTid != 0 && targetTid != curTid)
        Native.AttachThreadInput(curTid, targetTid, true);
    Native.SetForegroundWindow(hwnd);
    if (targetTid != 0 && targetTid != curTid)
        Native.AttachThreadInput(curTid, targetTid, false);
}

static string ForegroundInfo() {
    IntPtr h = Native.GetForegroundWindow();
    if (h == IntPtr.Zero)
        return "<none>";
    Native.GetWindowThreadProcessId(h, out uint pid);
    var title = new System.Text.StringBuilder(256);
    var cls = new System.Text.StringBuilder(256);
    Native.GetWindowText(h, title, title.Capacity);
    Native.GetClassName(h, cls, cls.Capacity);
    return $"pid={pid} class={cls} title={title}";
}

static HMGamepadState MapFrame(byte[] f, HMProfile profile) {
    // Field offsets (packed little-endian) — match src/shared/protocol/protocol.h:
    //   0..7   timestamp_us (uint64)  — ignored
    //   8..9   buttons (uint16)
    //   10..11 left_stick_x (int16)
    //   12..13 left_stick_y (int16)
    //   14..15 right_stick_x (int16)
    //   16..17 right_stick_y (int16)
    //   18     left_trigger (uint8)
    //   19     right_trigger (uint8)
    ushort buttons = BitConverter.ToUInt16(f, 8);
    short lx = BitConverter.ToInt16(f, 10);
    short ly = BitConverter.ToInt16(f, 12);
    short rx = BitConverter.ToInt16(f, 14);
    short ry = BitConverter.ToInt16(f, 16);
    byte lt = f[18];
    byte rt = f[19];

    // Buttons (ButtonMask bits from protocol.h)
    HMButton btn = HMButton.None;
    if ((buttons & 0x1000) != 0) btn |= HMButton.A;
    if ((buttons & 0x2000) != 0) btn |= HMButton.B;
    if ((buttons & 0x4000) != 0) btn |= HMButton.X;
    if ((buttons & 0x8000) != 0) btn |= HMButton.Y;
    if ((buttons & 0x0100) != 0) btn |= HMButton.LeftBumper;
    if ((buttons & 0x0200) != 0) btn |= HMButton.RightBumper;
    if ((buttons & 0x0010) != 0) btn |= HMButton.Start;
    if ((buttons & 0x0020) != 0) btn |= HMButton.Back;
    if ((buttons & 0x0040) != 0) btn |= HMButton.LeftStick;
    if ((buttons & 0x0080) != 0) btn |= HMButton.RightStick;

    // D-pad (bits 0..3) -> hat
    bool up = (buttons & 0x0001) != 0;
    bool down = (buttons & 0x0002) != 0;
    bool left = (buttons & 0x0004) != 0;
    bool right = (buttons & 0x0008) != 0;
    HMHat hat = HMHat.None;
    if (up && left) hat = HMHat.NorthWest;
    else if (up && right) hat = HMHat.NorthEast;
    else if (down && left) hat = HMHat.SouthWest;
    else if (down && right) hat = HMHat.SouthEast;
    else if (up) hat = HMHat.North;
    else if (down) hat = HMHat.South;
    else if (left) hat = HMHat.West;
    else if (right) hat = HMHat.East;

    // int16 -> [0,1] (0.5 = center); triggers uint8 -> [0,1]
    static float Axis(short v) => Math.Clamp(v / 32767.0f * 0.5f + 0.5f, 0f, 1f);

    return new HMGamepadState {
        Buttons = btn,
        Hat = hat,
        Axes = HMGamepadStateHelpers.StandardAxes(
            profile,
            leftStickX: Axis(lx), leftStickY: Axis(ly),
            rightStickX: Axis(rx), rightStickY: Axis(ry),
            leftTrigger: lt / 255.0f, rightTrigger: rt / 255.0f),
    };
}

internal static class Native {
    [System.Runtime.InteropServices.DllImport("user32.dll")]
    internal static extern IntPtr GetForegroundWindow();
    [System.Runtime.InteropServices.DllImport("user32.dll")]
    internal static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint pid);
    [System.Runtime.InteropServices.DllImport("user32.dll", CharSet = System.Runtime.InteropServices.CharSet.Unicode)]
    internal static extern int GetWindowText(IntPtr hWnd, System.Text.StringBuilder sb, int max);
    [System.Runtime.InteropServices.DllImport("user32.dll", CharSet = System.Runtime.InteropServices.CharSet.Unicode)]
    internal static extern int GetClassName(IntPtr hWnd, System.Text.StringBuilder sb, int max);
    [System.Runtime.InteropServices.DllImport("user32.dll")]
    internal static extern bool SetForegroundWindow(IntPtr hWnd);
    [System.Runtime.InteropServices.DllImport("user32.dll")]
    internal static extern bool AttachThreadInput(uint idAttach, uint idAttachTo, bool fAttach);
    [System.Runtime.InteropServices.DllImport("kernel32.dll")]
    internal static extern uint GetCurrentThreadId();
}
