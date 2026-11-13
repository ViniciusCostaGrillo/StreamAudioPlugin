[CmdletBinding()]
param (
    [ValidateSet("smoke", "tests", "plugin", "install")]
    [string]$Mode = "smoke"
)

$ErrorActionPreference = "Stop"

$RootDir = $PSScriptRoot
$BuildDir = Join-Path $RootDir "build"
$SrcDir = Join-Path $RootDir "src"
$VendorDir = Join-Path $RootDir "vendor\vst3sdk"
$TestsDir = Join-Path $RootDir "tests"

if (-not (Test-Path $BuildDir)) {
    New-Item -ItemType Directory -Path $BuildDir | Out-Null
}

$ClangCommonFlags = @(
    "-std=c++17",
    "-O2",
    "-DRELEASE=1",
    "-D_UNICODE",
    "-DUNICODE",
    "-DNOMINMAX",
    "-DWIN32_LEAN_AND_MEAN",
    "-Wno-pragma-pack",
    "-I$VendorDir",
    "-I$SrcDir"
)

switch ($Mode) {
    "smoke" {
        Write-Host "=== [build.ps1] Building smoke test ===" -ForegroundColor Cyan
        $SmokeSrc = Join-Path $TestsDir "test_sdk_smoke.cpp"
        $FUnknownSrc = Join-Path $VendorDir "pluginterfaces\base\funknown.cpp"
        $SmokeExe = Join-Path $BuildDir "test_sdk_smoke.exe"

        Write-Host "Compiling $SmokeExe..."
        & clang++ @ClangCommonFlags $SmokeSrc $FUnknownSrc -lole32 -o $SmokeExe
        if ($LASTEXITCODE -ne 0) {
            Write-Error "Smoke test compilation failed with exit code $LASTEXITCODE"
            exit $LASTEXITCODE
        }

        Write-Host "=== [build.ps1] Running smoke test ===" -ForegroundColor Cyan
        & $SmokeExe
        if ($LASTEXITCODE -ne 0) {
            Write-Error "Smoke test execution failed with exit code $LASTEXITCODE"
            exit $LASTEXITCODE
        }
        Write-Host "=== [build.ps1] Smoke test passed successfully! ===" -ForegroundColor Green
    }
    "tests" {
        Write-Host "=== [build.ps1] Mode: tests (stub for Tasks 2-6) ===" -ForegroundColor Yellow
    }
    "plugin" {
        Write-Host "=== [build.ps1] Mode: plugin (stub for Tasks 5-6) ===" -ForegroundColor Yellow
    }
    "install" {
        Write-Host "=== [build.ps1] Mode: install (stub for Task 6) ===" -ForegroundColor Yellow
    }
}
