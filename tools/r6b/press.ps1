# r6b probe runner: press one button at a chosen frame, capture shots every N frames.
param([string]$Key = "10", [int]$At = 500, [int]$Frames = 1100, [int]$Every = 20, [string]$Tag = "")
$ErrorActionPreference = "Stop"
Set-Location (Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)))
$py = "C:\Users\DuWeisi\.dsh\dsh-runtimes\dsh-primary-runtime\dependencies\python\python.exe"
$d = "tmp\r6b\" + $Key + $Tag
New-Item -ItemType Directory -Force $d | Out-Null
Get-ChildItem $d -Filter *.bmp | Remove-Item -Force
$press = "" + $Key + ":" + $At + ":" + ($At + 6)
& .\emu\digicore.exe --frames $Frames --press $press --shot-dir $d --shot-every $Every 2>&1 |
    Select-String -Pattern 'PC=|^frames=' | ForEach-Object { $_.Line }
& $py tools\r6b\contact.py $d (Join-Path $d "sheet.png")
