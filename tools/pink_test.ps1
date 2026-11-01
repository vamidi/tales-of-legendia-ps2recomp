# pink_test.ps1 - run ps2EntryRunner for ~55s and capture the runner window mid-run.
#
# RUN IT THROUGH THE WRAPPER:  scripts\pink_test.cmd -Seconds 45
# ------------------------------------------------------------------------------
# A .ps1 CANNOT lift its own execution policy: the block happens before this file
# is ever read, so nothing written inside it can help. The wrapper is just:
#     powershell -NoProfile -ExecutionPolicy Bypass -File pink_test.ps1 %*
# Calling .\pink_test.ps1 directly on a restricted machine fails in 0.2 s AND
# leaves the PREVIOUS logs on disk, which then look like fresh results - so the
# script prints the mtimes/sizes of everything it wrote at the end. Always check
# that block before believing a verdict.
# The game window is brought TOPMOST (and briefly resized) so the capture is not
# occluded by the terminal. Two captures are attempted per shot:
#   logs\pink_screen.png     - CopyFromScreen of the raised window (true pixels)
#   logs\pink_screen_pw.png  - PrintWindow(PW_RENDERFULLCONTENT) fallback
# Runtime's own log is ps2_log.txt in the project root (copied to logs\).
param(
    [int]$Seconds = 55
)
$ErrorActionPreference = 'Continue'

$root   = 'D:\Personal\C++\tales-of-legendia-ps2recomp'
$runner = "$root\PS2Recomp\out\build\ps2xRuntime\ps2EntryRunner.exe"
$elf    = "$root\data\SLUS_212.01"
$log    = "$root\logs\pink_test.log"
$errlog = "$root\logs\pink_test.err.log"
$png    = "$root\logs\pink_screen.png"
$pngpw  = "$root\logs\pink_screen_pw.png"
$rtlog  = "$root\ps2_log.txt"

Remove-Item $log, $errlog, $png, $pngpw -ErrorAction SilentlyContinue
Get-Process ps2EntryRunner -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Seconds 1
if (Test-Path $rtlog) { Copy-Item $rtlog "$root\logs\ps2_log_prev.txt" -Force }

$p = Start-Process -FilePath $runner -ArgumentList "`"$elf`"" -WorkingDirectory $root `
        -RedirectStandardOutput $log -RedirectStandardError $errlog -PassThru

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class WinCap {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left; public int Top; public int Right; public int Bottom; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT lpRect);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hWnd, IntPtr hdcBlt, uint nFlags);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr hWnd, IntPtr after, int X, int Y, int cx, int cy, uint flags);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hWnd, int cmd);
}
"@

function Capture-Shot {
    try {
        $proc = Get-Process ps2EntryRunner -ErrorAction SilentlyContinue |
                Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
        if (-not $proc) { return $false }
        $h = $proc.MainWindowHandle
        [void][WinCap]::ShowWindow($h, 9)   # SW_RESTORE
        # HWND_TOPMOST = -1, SWP_SHOWWINDOW = 0x40
        [void][WinCap]::SetWindowPos($h, [IntPtr]::new(-1), 60, 60, 1280, 720, 0x0040)
        Start-Sleep -Milliseconds 700

        $r = New-Object WinCap+RECT
        [void][WinCap]::GetWindowRect($h, [ref]$r)
        $w = $r.Right - $r.Left
        $ht = $r.Bottom - $r.Top
        if ($w -le 0 -or $ht -le 0) { return $false }

        # fallback: PrintWindow
        $b1 = New-Object System.Drawing.Bitmap $w, $ht
        $g1 = [System.Drawing.Graphics]::FromImage($b1)
        $hdc = $g1.GetHdc()
        [void][WinCap]::PrintWindow($h, $hdc, 2)
        $g1.ReleaseHdc($hdc)
        $b1.Save($pngpw, [System.Drawing.Imaging.ImageFormat]::Png)
        $g1.Dispose(); $b1.Dispose()

        # primary: screen grab of the now-topmost window
        $b2 = New-Object System.Drawing.Bitmap $w, $ht
        $g2 = [System.Drawing.Graphics]::FromImage($b2)
        $g2.CopyFromScreen($r.Left, $r.Top, 0, 0, $b2.Size)
        $b2.Save($png, [System.Drawing.Imaging.ImageFormat]::Png)
        $g2.Dispose(); $b2.Dispose()
        return $true
    } catch { return $false }
}

$shots = 0
for ($i = 0; $i -lt ($Seconds + 3); $i++) {
    Start-Sleep -Seconds 1
    if ($p.HasExited) { break }
    if ($i -ge 30 -and ($i % 5 -eq 0)) { if (Capture-Shot) { $shots++ } }
}

if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
Start-Sleep -Seconds 2

"SHOTS TAKEN: $shots"
"PNG(screen): $(Test-Path $png) bytes=$(if (Test-Path $png) { (Get-Item $png).Length } else { 0 })"
"PNG(print):  $(Test-Path $pngpw) bytes=$(if (Test-Path $pngpw) { (Get-Item $pngpw).Length } else { 0 })"
"runtime log lines: $((Get-Content $rtlog -ErrorAction SilentlyContinue).Count)"
"--- freshness (must be seconds old - if these are stale, the run never happened) ---"
foreach ($f in $log, $errlog, $png, $pngpw, $rtlog) {
    if (Test-Path $f) { "{0,-24} {1}  {2,12} bytes" -f (Split-Path $f -Leaf), (Get-Item $f).LastWriteTime, (Get-Item $f).Length }
    else              { "{0,-24} MISSING" -f (Split-Path $f -Leaf) }
}
