param(
    [Parameter(Mandatory)][string]$Client,   # a scratch copy of a client folder, never the shipped Game Files
    [Parameter(Mandatory)][string]$Dll,      # the Kaentake.dll to test
    [int]$SettleSeconds = 25
)
# Checks that a normal quit ends the client cleanly. The client's config.ini must point at the login stub
# (host=127.0.0.1, port=18484, no patch_url); login_stub.py sends the v83 handshake so the client reaches its login
# screen. The window is then closed the way the title bar's X does (WM_NCLBUTTONUP HTCLOSE -> PostQuitMessage).
# Exit code 0 = the client exited with 0; a crashing exit path ends in TerminateProcess(0xC0000005).
#   python test\login_stub.py      (in another console)
#   powershell -ExecutionPolicy Bypass -File test\client_exit_test.ps1 -Client <copy> -Dll build\Release\Kaentake.dll
$ErrorActionPreference = 'Stop'
$ini = Join-Path $Client 'config.ini'
$cfg = Get-Content $ini -Raw
if ($cfg -match '(?m)^\s*patch_url\s*=\s*\S' -or $cfg -notmatch '(?m)^\s*port\s*=\s*18484\s*$') {
    "refused: $ini must have port=18484 and no patch_url (use a scratch copy pointed at login_stub.py)"; exit 2
}
$target = Join-Path $Client 'Kaentake.dll'
Remove-Item $target -ErrorAction SilentlyContinue  # a hard link would write through to the file it shares
Copy-Item $Dll $target
Add-Type -Namespace KaentakeTest -Name U -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool PostMessage(System.IntPtr h, uint m, System.IntPtr w, System.IntPtr l);
public delegate bool EnumProc(System.IntPtr h, System.IntPtr l);
[DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc f, System.IntPtr l);
[DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(System.IntPtr h, out uint pid);
[DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassName(System.IntPtr h, System.Text.StringBuilder s, int n);
public static System.IntPtr GameWindow(uint want) {
    System.IntPtr found = System.IntPtr.Zero;
    EnumWindows((h, l) => {
        uint pid; GetWindowThreadProcessId(h, out pid);
        var c = new System.Text.StringBuilder(64); GetClassName(h, c, 64);
        if (pid == want && c.ToString() == "MapleStoryClass") { found = h; return false; }
        return true;
    }, System.IntPtr.Zero);
    return found;
}
'@
$exe = Join-Path (Resolve-Path $Client) 'MapleStory.exe'
$before = @(Get-Process MapleStory -ErrorAction SilentlyContinue | ForEach-Object Id)
$cmd = "cmd.exe /c set __COMPAT_LAYER=RunAsInvoker&& start `"`" `"$(Join-Path (Resolve-Path $Client) 'Launch Game.exe')`""
$null = Invoke-CimMethod -ClassName Win32_Process -MethodName Create -Arguments @{ CommandLine = $cmd; CurrentDirectory = "$(Resolve-Path $Client)" }
$p = $null
for ($i = 0; $i -lt 60 -and -not $p; $i++) {
    Start-Sleep -Milliseconds 500
    $p = Get-Process MapleStory -ErrorAction SilentlyContinue | Where-Object { $before -notcontains $_.Id -and $_.Path -eq $exe } | Select-Object -First 1
}
if (-not $p) { "FAIL: no MapleStory.exe started from $Client"; exit 1 }
$null = $p.Handle  # keeps the exit code readable
$hw = [IntPtr]::Zero
for ($i = 0; $i -lt 120 -and $hw -eq [IntPtr]::Zero -and -not $p.HasExited; $i++) {
    Start-Sleep -Milliseconds 500
    $hw = [KaentakeTest.U]::GameWindow([uint32]$p.Id)
}
Start-Sleep -Seconds $SettleSeconds
if ($p.HasExited -or $hw -eq [IntPtr]::Zero) { "FAIL: the client ended before the quit (code 0x{0:X8})" -f $p.ExitCode; exit 1 }
[KaentakeTest.U]::PostMessage($hw, 0xA2, [IntPtr]20, [IntPtr]0) | Out-Null  # WM_NCLBUTTONUP, HTCLOSE
if (-not $p.WaitForExit(60000)) { Stop-Process -Id $p.Id -Force; "FAIL: still running 60 s after the quit"; exit 1 }
if ($p.ExitCode -ne 0) { "FAIL: exit code 0x{0:X8}" -f $p.ExitCode; exit 1 }
"client_exit_test: the client exited with 0"
exit 0
