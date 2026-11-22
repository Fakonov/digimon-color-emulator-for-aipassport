# Throughput A/B for the audio model (docs/AUDIO.md §4).
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File dbgtest\audio\measure_speed.ps1
#
# Why CPU time and not just the emulator's own "speed=": on a loaded machine the
# wall clock reports < 1.0x for reasons that have nothing to do with the model
# (four concurrent DSH agents were enough to push an 8-core box to 100 %).  CPU
# seconds per emulated second is what the SoC actually spends, so the A/B runs
# interleaved and compares that; the wall clock figure is printed for reference.
param([int]$Seconds = 5, [int]$Repeat = 3)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path))
Set-Location $root

function Run-Once([bool]$AudioOn) {
    $env:GPCE4_AUDIO_OFF = if ($AudioOn) { $null } else { "1" }
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = (Resolve-Path ".\emu\digicore.exe").Path
    $psi.Arguments = "--seconds $Seconds --autoplay"
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.CreateNoWindow = $true
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $p = [System.Diagnostics.Process]::Start($psi)
    $stdout = $p.StandardOutput.ReadToEnd()
    $p.WaitForExit()
    $sw.Stop()
    $cpu = $p.TotalProcessorTime.TotalSeconds
    $reported = if ($stdout -match "speed=([0-9.]+)x") { [double]$Matches[1] } else { 0 }
    [pscustomobject]@{
        audio  = if ($AudioOn) { "on" } else { "off" }
        wall_s = [math]::Round($sw.Elapsed.TotalSeconds, 2)
        cpu_s  = [math]::Round($cpu, 3)
        cpu_x  = if ($cpu -gt 0) { [math]::Round($Seconds / $cpu, 2) } else { 0 }
        wall_x = $reported
    }
}

$rows = @()
for ($i = 0; $i -lt $Repeat; $i++) {
    $rows += Run-Once $true          # interleaved: the two variants see the same load
    $rows += Run-Once $false
}
$env:GPCE4_AUDIO_OFF = $null

$rows | Format-Table -AutoSize

foreach ($grp in ($rows | Group-Object audio)) {
    $cpu = ($grp.Group | Measure-Object cpu_x -Average).Average
    $wall = ($grp.Group | Measure-Object wall_x -Maximum).Maximum
    "audio {0,-3} : cpu_x avg {1:N2}x, best cpu_x {2:N2}x, best wall_x {3:N2}x" -f $grp.Name, $cpu,
        ($grp.Group | Measure-Object cpu_x -Maximum).Maximum, $wall
}
