param([string]$Project = "C:\Users\DuWeisi\digimon-color-passport\firmware\digicore-c3")
# Build the digicore-c3 firmware with the ESP-IDF 5.5.3 environment.
# passport-toolkit's env.ps1 already exports IDF_PATH and the toolchain; the old
# esp-idf-setup\esp-idf-5.5.3\export.ps1 path no longer exists (it made this
# script exit 1 even when the build was fine).
. "$env:USERPROFILE\passport-toolkit\env.ps1" | Out-Null
Push-Location $Project
try {
    & idf.py build 2>&1 | Select-String -Pattern "error|Project build complete|binary size" | ForEach-Object { $_.Line }
    exit $LASTEXITCODE
} finally {
    Pop-Location
}
