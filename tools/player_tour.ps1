# Drive the running player through a key sequence and capture a PNG after each step.
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools/player_tour.ps1
# Produces dbgtest/tour/*.png so a whole menu path can be reviewed offline.
param([string]$OutDir = "dbgtest/tour")

$ErrorActionPreference = "Stop"
New-Item -ItemType Directory -Force $OutDir | Out-Null
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class Uix {
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern void keybd_event(byte bVk, byte bScan, uint dwFlags, UIntPtr extra);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
}
"@

$p = Get-Process digimon-player -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $p) { throw "digimon-player is not running" }
$h = $p.MainWindowHandle
[Uix]::SetForegroundWindow($h) | Out-Null
Start-Sleep -Milliseconds 500

$steps = @(
  @{ name = "00_boot"; key = 0 },
  @{ name = "01_j";    key = 0x4A },
  @{ name = "02_k";    key = 0x4B },
  @{ name = "03_k";    key = 0x4B },
  @{ name = "04_l";    key = 0x4C },
  @{ name = "05_j";    key = 0x4A },
  @{ name = "06_k";    key = 0x4B },
  @{ name = "07_j";    key = 0x4A }
)

$i = 0
foreach ($s in $steps) {
  if ($s.key -ne 0) {
    [Uix]::keybd_event([byte]$s.key, 0, 0, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds 350
    [Uix]::keybd_event([byte]$s.key, 0, 2, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds 1200
  }
  $r = New-Object Uix+RECT
  [Uix]::GetWindowRect($h, [ref]$r) | Out-Null
  $w = $r.Right - $r.Left; $hh = $r.Bottom - $r.Top
  $bmp = New-Object System.Drawing.Bitmap $w, $hh
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $g.CopyFromScreen($r.Left, $r.Top, 0, 0, $bmp.Size)
  $g.Dispose()
  $path = Join-Path $OutDir ("{0}.png" -f $s.name)
  $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
  $bmp.Dispose()
  "captured $path"
  $i++
}
