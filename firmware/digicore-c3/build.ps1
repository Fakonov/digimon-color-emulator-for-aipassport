# Build, merge and report the digicore-c3 image.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File firmware\digicore-c3\build.ps1
#   powershell ... -NoProfile ... -File ...\build.ps1 -Clean          # full rebuild
#   powershell ... -NoProfile ... -File ...\build.ps1 -NoSparse       # keep the sparse image
#
# Produces
#   build/digicore-c3.bin                the app image (factory partition only)
#   digicore-c3-full.bin                 merged at offset 0: bootloader,
#                                        partition table, app, sparse ROM at 0x210000
#   sparse/digicore-sparse.bin           tool output, flashed at 0x210000
# and writes the whole idf.py transcript to build.log (UTF-8).
#
# Nothing here touches a serial port: the owner flashes.
param(
    [switch]$Clean,
    [switch]$NoSparse
)
$ErrorActionPreference = 'Stop'

$IDF = '<home>/idf-setup/esp-idf-5.5.3'
$TOOLS = '<home>/.espressif'
$PY = "$TOOLS/python_env/idf5.5_py3.11_env/Scripts/python.exe"
$HOSTPY = '<home>/py311/python.exe'

$env:IDF_PATH = $IDF
$env:IDF_TOOLS_PATH = $TOOLS
$env:IDF_PYTHON_ENV_PATH = "$TOOLS/python_env/idf5.5_py3.11_env"
$toolPath = @(
    "$TOOLS/tools/riscv32-esp-elf/esp-14.2.0_20251107/riscv32-esp-elf/bin",
    "$TOOLS/tools/ninja/1.12.1",
    "$TOOLS/tools/cmake/3.30.2/bin",
    "$TOOLS/tools/ccache/4.12.1",
    "$TOOLS/tools/idf-exe/1.0.3",
    '<home>/py311',
    '<home>/py311/Scripts'
) -join ';'
$env:Path = $toolPath + ';' + $env:Path

$root = $PSScriptRoot
$repo = (Resolve-Path (Join-Path $root '..\..')).Path
$log = Join-Path $root 'build.log'
$full = Join-Path $root 'digicore-c3-full.bin'
$sparse = Join-Path $root 'sparse\digicore-sparse.bin'
$romSrc = Join-Path $repo 'roms\dmc_v1_base.bin'

# Run a native command, echo it, and append its combined output to build.log.
function Invoke-Logged {
    param(
        [string]$Exe,
        [string[]]$ArgList
    )
    # idf.py prints warnings on stderr; with EA=Stop PowerShell 5.1 turns the
    # first stderr line into a terminating NativeCommandError.  $LASTEXITCODE
    # stays authoritative.
    $ErrorActionPreference = 'Continue'
    $out = & $Exe @ArgList 2>&1 | Out-String
    $rc = $LASTEXITCODE
    Write-Host $out
    $out | Out-File -FilePath $log -Encoding utf8 -Append
    return $rc
}

Push-Location $root
try {
    # ---- sparse SPI flash image ------------------------------------------
    $needSparse = $true
    if ($NoSparse -and (Test-Path $sparse)) {
        $needSparse = $false
    } elseif ((Test-Path $sparse) -and (Test-Path $romSrc)) {
        $needSparse = (Get-Item $sparse).LastWriteTime -lt (Get-Item $romSrc).LastWriteTime
    }
    if ($needSparse) {
        $rc = Invoke-Logged $HOSTPY @(
            (Join-Path $repo 'tools\make_sparse_rom.py'), '--verify')
        if ($rc -ne 0) { throw "make_sparse_rom.py failed: $rc" }
    }

    if ($Clean) {
        Remove-Item -Recurse -Force (Join-Path $root 'build') -ErrorAction SilentlyContinue
    }

    "=== digicore-c3 build $(Get-Date -Format s) ===" |
        Out-File -FilePath $log -Encoding utf8

    # ---- build ------------------------------------------------------------
    $rc = Invoke-Logged $PY @(
        "$IDF/tools/idf.py", '-B', 'build', '-DIDF_TARGET=esp32c3', 'build')
    if ($rc -ne 0) { throw "idf.py build failed: $rc" }

    # ---- size report ------------------------------------------------------
    $rc = Invoke-Logged $PY @("$IDF/tools/idf.py", '-B', 'build', 'size')
    if ($rc -ne 0) { throw "idf.py size failed: $rc" }

    # ---- merge ------------------------------------------------------------
    $app = Join-Path $root 'build\digicore-c3.bin'
    if (-not (Test-Path $app)) { throw "app image missing: $app" }
    if (-not (Test-Path $sparse)) { throw "sparse image missing: $sparse" }

    Push-Location (Join-Path $root 'build')
    try {
        $rc = Invoke-Logged $PY @(
            '-m', 'esptool', '--chip', 'esp32c3', 'merge_bin',
            '-o', $full, '--flash_mode', 'dio', '--flash_freq', '80m',
            '--flash_size', '8MB',
            '0x0', 'bootloader/bootloader.bin',
            '0x8000', 'partition_table/partition-table.bin',
            '0x10000', 'digicore-c3.bin',
            '0x210000', '..\sparse\digicore-sparse.bin')
        if ($rc -ne 0) { throw "merge_bin failed: $rc" }
    } finally {
        Pop-Location
    }

    # ---- report -----------------------------------------------------------
    $rows = foreach ($f in @($app, $full, $sparse)) {
        $item = Get-Item $f
        [pscustomobject]@{
            file = $item.Name
            bytes = $item.Length
            sha256 = (Get-FileHash $f -Algorithm SHA256).Hash
        }
    }
    $report = $rows | Format-Table -AutoSize | Out-String
    Write-Host $report
    $report | Out-File -FilePath $log -Encoding utf8 -Append

    # The merged image is written at offset 0, so one write_flash covers
    # bootloader, partition table, app and the sparse ROM partition.
    $flashCmd = 'esptool --chip esp32c3 -p COMx -b 460800 --before default_reset ' +
                '--after hard_reset write_flash --flash_mode dio --flash_freq 80m ' +
                '--flash_size 8MB 0x0 digicore-c3-full.bin'
    Write-Host ''
    Write-Host "flash (owner): $flashCmd"
    '' | Out-File -FilePath $log -Encoding utf8 -Append
    "flash (owner): $flashCmd" | Out-File -FilePath $log -Encoding utf8 -Append
} finally {
    Pop-Location
}
