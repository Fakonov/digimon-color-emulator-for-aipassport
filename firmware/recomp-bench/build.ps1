param(
    [switch]$Sim
)
$ErrorActionPreference = 'Stop'

$IDF = 'C:/Users/DuWeisi/idf-setup/esp-idf-5.5.3'
$TOOLS = 'C:/Users/DuWeisi/.espressif'
$PY = "$TOOLS/python_env/idf5.5_py3.11_env/Scripts/python.exe"
$NODE = 'C:/Users/DuWeisi/.dsh/dsh-runtimes/dsh-primary-runtime/dependencies/node/bin/node.exe'

$env:IDF_PATH = $IDF
$env:IDF_TOOLS_PATH = $TOOLS
$env:IDF_PYTHON_ENV_PATH = "$TOOLS/python_env/idf5.5_py3.11_env"
$toolPath = @(
    "$TOOLS/tools/riscv32-esp-elf/esp-14.2.0_20251107/riscv32-esp-elf/bin",
    "$TOOLS/tools/ninja/1.12.1",
    "$TOOLS/tools/cmake/3.30.2/bin",
    "$TOOLS/tools/ccache/4.12.1",
    "$TOOLS/tools/idf-exe/1.0.3",
    'C:/Users/DuWeisi/py311',
    'C:/Users/DuWeisi/py311/Scripts'
) -join ';'
$env:Path = $toolPath + ';' + $env:Path

$full = Join-Path $PSScriptRoot 'recomp-bench-full.bin'
Push-Location $PSScriptRoot
try {
    & $PY "$IDF/tools/idf.py" -B build -DIDF_TARGET=esp32c3 build
    if ($LASTEXITCODE -ne 0) { throw "idf.py build failed: $LASTEXITCODE" }

    Push-Location build
    try {
        & $PY -m esptool --chip esp32c3 merge_bin -o $full `
            --flash_mode dio --flash_freq 80m --flash_size 8MB `
            0x0 bootloader/bootloader.bin `
            0x8000 partition_table/partition-table.bin `
            0x10000 recomp-bench.bin
        if ($LASTEXITCODE -ne 0) { throw "merge_bin failed: $LASTEXITCODE" }
    } finally {
        Pop-Location
    }

    if ($Sim) {
        & $NODE (Join-Path $PSScriptRoot '../../tools/sim_uart_probe.mjs') $full 3
        if ($LASTEXITCODE -ne 0) { throw "simulator failed: $LASTEXITCODE" }
    }
} finally {
    Pop-Location
}
