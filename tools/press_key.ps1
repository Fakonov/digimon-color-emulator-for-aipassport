# Press a key into the focused window and hold it for N milliseconds.
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools/press_key.ps1 -Key 0x4A -Hold 300
# Uses keybd_event so the target's GetKeyState() polling sees a real key-down.
param([int]$Key = 0x4A, [int]$Hold = 300, [string]$Title = "Digimon Color")

$ErrorActionPreference = "Stop"
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class Kbd {
  [DllImport("user32.dll")] public static extern void keybd_event(byte bVk, byte bScan, uint dwFlags, UIntPtr extra);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
}
"@

$p = Get-Process digimon-player -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $p) { throw "digimon-player is not running" }
[Kbd]::SetForegroundWindow($p.MainWindowHandle) | Out-Null
Start-Sleep -Milliseconds 400

[Kbd]::keybd_event([byte]$Key, 0, 0, [UIntPtr]::Zero)
Start-Sleep -Milliseconds $Hold
[Kbd]::keybd_event([byte]$Key, 0, 2, [UIntPtr]::Zero)
"pressed 0x{0:X2} for {1} ms" -f $Key, $Hold
