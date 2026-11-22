# Capture the running player window to a PNG (proof shot for docs).
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools/capture_window.ps1 -Out dbgtest/player.png
param([string]$Out = "dbgtest/player.png", [int]$Wait = 6, [string]$Title = "Digimon Color")

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class Win {
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
}
"@

$p = Get-Process digimon-player -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $p) { throw "digimon-player is not running" }
[Win]::SetForegroundWindow($p.MainWindowHandle) | Out-Null
Start-Sleep -Seconds $Wait
$r = New-Object Win+RECT
[Win]::GetWindowRect($p.MainWindowHandle, [ref]$r) | Out-Null
$w = $r.Right - $r.Left
$h = $r.Bottom - $r.Top
$bmp = New-Object System.Drawing.Bitmap $w, $h
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($r.Left, $r.Top, 0, 0, $bmp.Size)
$g.Dispose()
New-Item -ItemType Directory -Force (Split-Path -Parent $Out) | Out-Null
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()
"saved $Out ($w x $h)"
