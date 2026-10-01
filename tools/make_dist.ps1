# Crea i due pacchetti portable (macchina BASIC e Archimedes) in dist\
# Compila con il runtime C statico, cosi' non serve il Visual C++ Redistributable.
# Uso: powershell -File tools\make_dist.ps1 [-Version v1.0]
param([string]$Version = "v1.0")
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

cmake -S . -B build-dist -G "Visual Studio 17 2022" -A x64 -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded | Out-Null
cmake --build build-dist --config Release --target armwin archie
if ($LASTEXITCODE) { throw "build fallita (archie.exe o armwin.exe ancora aperti?)" }
$bin = "build-dist\Release"

$stage = "dist\stage"
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }

# macchina BBC BASIC: non serve nessuna ROM
$b = "$stage\ArchieEmu-BASIC"
New-Item -ItemType Directory -Force "$b\third_party\riscos", "$b\disc" | Out-Null
Copy-Item "$bin\armwin.exe", LICENSE $b
Copy-Item tools\dist\README-BASIC.txt "$b\README.txt"
Copy-Item third_party\riscos\BASIC, third_party\riscos\LICENSE, third_party\riscos\README.md "$b\third_party\riscos"
Copy-Item disc\* "$b\disc"

# Archimedes: ROM e dischetti li mette l'utente
$a = "$stage\ArchieEmu-Archimedes"
New-Item -ItemType Directory -Force "$a\roms\1. Major", "$a\ADF" | Out-Null
Copy-Item "$bin\archie.exe", LICENSE $a
Copy-Item tools\dist\README-Archimedes.txt "$a\README.txt"
Set-Content -Encoding ascii "$a\roms\1. Major\PUT_ROM311_HERE.txt" "Put your RISC OS 3.11 ROM image here as a file named ROM311 (not included: copyrighted)."
Set-Content -Encoding ascii "$a\ADF\PUT_ADF_IMAGES_HERE.txt" "Put your .adf floppy images here (Ctrl+F9 in the emulator opens this folder)."

foreach ($p in "BASIC", "Archimedes") {
    $zip = "dist\ArchieEmu-$p-$Version-win64.zip"
    if (Test-Path $zip) { Remove-Item $zip }
    Compress-Archive -Path "$stage\ArchieEmu-$p" -DestinationPath $zip
    Get-Item $zip | Select-Object Name, Length
}
