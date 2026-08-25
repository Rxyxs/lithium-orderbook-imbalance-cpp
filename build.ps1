# Compiles the engine, tests, and sample-data generator with MSVC
# (cl.exe), with zero external dependencies. Loads vcvars64.bat if
# `cl` is not already on PATH.
#
# Usage:
#   .\build.ps1            # build only
#   .\build.ps1 -RunTests  # build and run the hand-rolled test suite
#   .\build.ps1 -GenData   # build and (re)generate data/lithium_ticks_sample.csv

param(
    [switch]$RunTests,
    [switch]$GenData
)

$ErrorActionPreference = "Stop"

if (-not (Get-Command cl -ErrorAction SilentlyContinue)) {
    $vcvars = "C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars64.bat"
    if (-not (Test-Path $vcvars)) {
        throw "No se encontro vcvars64.bat en la ruta esperada: $vcvars"
    }
    Write-Host "Cargando entorno MSVC desde $vcvars..."
    $envDump = cmd /c "`"$vcvars`" >nul 2>&1 && set"
    foreach ($line in $envDump) {
        if ($line -match "^(.*?)=(.*)$") {
            Set-Item -Path "env:$($Matches[1])" -Value $Matches[2]
        }
    }
}

New-Item -ItemType Directory -Force -Path bin | Out-Null

$commonFlags = @("/std:c++17", "/EHsc", "/O2", "/W4", "/nologo", "/I", "include")

Write-Host "Compilando loi_engine.exe..."
& cl @commonFlags src\main.cpp /Fe:bin\loi_engine.exe /Fo:bin\ | Out-Host
if ($LASTEXITCODE -ne 0) { throw "Fallo la compilacion de loi_engine.exe" }

Write-Host "Compilando generate_sample_data.exe..."
& cl @commonFlags tools\generate_sample_data.cpp /Fe:bin\generate_sample_data.exe /Fo:bin\ | Out-Host
if ($LASTEXITCODE -ne 0) { throw "Fallo la compilacion de generate_sample_data.exe" }

Write-Host "Compilando test_engine.exe..."
& cl @commonFlags tests\test_engine.cpp /Fe:bin\test_engine.exe /Fo:bin\ | Out-Host
if ($LASTEXITCODE -ne 0) { throw "Fallo la compilacion de test_engine.exe" }

Write-Host "`nBuild completo. Binarios en .\bin\"

if ($GenData) {
    Write-Host "`nGenerando datos sinteticos de ejemplo..."
    New-Item -ItemType Directory -Force -Path data | Out-Null
    & .\bin\generate_sample_data.exe data\lithium_ticks_sample.csv
    if ($LASTEXITCODE -ne 0) { throw "Fallo la generacion de datos" }
}

if ($RunTests) {
    Write-Host "`nEjecutando suite de tests..."
    & .\bin\test_engine.exe
    if ($LASTEXITCODE -ne 0) { throw "Fallaron los tests" }
}
