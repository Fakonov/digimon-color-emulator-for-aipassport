# r6b_scene.ps1 - run one non-perturbing scenario and summarise LCD activity.
param(
  [string]$Press = "10:700:706",
  [int]$Frames = 1800,
  [string]$Tag = "scene",
  [switch]$Dump,
  [int]$DumpFrom = 0,
  [int]$DumpTo = 0
)
$ErrorActionPreference = "Stop"
Set-Location (Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)))
$d = "tmp\r6b\" + $Tag
New-Item -ItemType Directory -Force $d | Out-Null
Get-ChildItem $d -Filter *.bmp -ErrorAction SilentlyContinue | Remove-Item -Force
$args = @("--frames", $Frames, "--press", $Press, "--nohooks",
          "--csv", (Join-Path $d "frames.csv"), "--windows", (Join-Path $d "wins.csv"),
          "--events", (Join-Path $d "events.txt"))
if ($Dump) {
  $args += @("--dump-dir", $d, "--dump-from", $DumpFrom, "--dump-to", $DumpTo, "--dump-every", 1)
}
& .\tools\r6b\probe.exe @args 2>&1 | Select-String -Pattern 'final PC'
Write-Host "--- screen changes ---"
Get-Content (Join-Path $d "events.txt")
Write-Host "--- frames with LCD pushes (frame,px) ---"
Get-Content (Join-Path $d "frames.csv") | Select-Object -Skip 1 |
  Where-Object { ($_ -split ',')[4] -ne '0' } |
  ForEach-Object { $p = $_ -split ','; "  f=" + $p[0] + " px=" + $p[4] + " cmds=" + $p[5] + " wins=" + $p[8] }
