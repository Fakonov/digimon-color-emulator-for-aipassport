param([string]$Press="", [int]$Frames=900, [string]$Tag="save", [switch]$Dump, [int]$DumpFrom=0, [int]$DumpTo=0)
$ErrorActionPreference="Stop"
Set-Location (Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)))
$d = "tmp\r6b\" + $Tag
New-Item -ItemType Directory -Force $d | Out-Null
Get-ChildItem $d -Filter *.bmp -ErrorAction SilentlyContinue | Remove-Item -Force
$a = @("--frames",$Frames,"--nohooks","--load-flash","saves\flash.bin",
       "--csv",(Join-Path $d "frames.csv"),"--windows",(Join-Path $d "wins.csv"),"--events",(Join-Path $d "events.txt"))
if ($Press -ne "") { $a += @("--press",$Press) }
if ($Dump) { $a += @("--dump-dir",$d,"--dump-from",$DumpFrom,"--dump-to",$DumpTo,"--dump-every",1) }
& .\tools\r6b\probe.exe @a 2>&1 | Select-String 'final PC'
Write-Host "--- screen changes ---"
Get-Content (Join-Path $d "events.txt") | Select-Object -First 25
