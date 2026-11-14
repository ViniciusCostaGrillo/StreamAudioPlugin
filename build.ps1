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

function Build-Plugin {
    Write-Host "=== [build.ps1] Building StreamAudioPlugin.vst3 bundle ===" -ForegroundColor Cyan
    $BundleDir = Join-Path $BuildDir "StreamAudioPlugin.vst3\Contents\x86_64-win"
    if (-not (Test-Path $BundleDir)) {
        New-Item -ItemType Directory -Force -Path $BundleDir | Out-Null
    }

    $SdkSources = @(
        "public.sdk\source\main\dllmain.cpp",
        "public.sdk\source\main\pluginfactory.cpp",
        "public.sdk\source\vst\vstsinglecomponenteffect.cpp",
        "public.sdk\source\vst\vstbus.cpp",
        "public.sdk\source\vst\vstparameters.cpp",
        "public.sdk\source\vst\vstcomponentbase.cpp",
        "public.sdk\source\vst\vstinitiids.cpp",
        "public.sdk\source\common\pluginview.cpp",
        "public.sdk\source\common\commoniids.cpp",
        "pluginterfaces\base\funknown.cpp",
        "pluginterfaces\base\ustring.cpp",
        "pluginterfaces\base\coreiids.cpp",
        "base\source\baseiids.cpp",
        "base\source\fstring.cpp",
        "base\source\fbuffer.cpp",
        "base\source\fdebug.cpp",
        "base\source\fobject.cpp",
        "base\source\updatehandler.cpp",
        "base\thread\source\flock.cpp"
    )

    $SdkObjs = @()
    foreach ($rel in $SdkSources) {
        $srcPath = Join-Path $VendorDir $rel
        $baseName = [System.IO.Path]::GetFileNameWithoutExtension($srcPath)
        $objPath = Join-Path $BuildDir "sdk_$baseName.o"
        $SdkObjs += $objPath

        $needCompile = $true
        if (Test-Path $objPath) {
            $srcTime = (Get-Item $srcPath).LastWriteTimeUtc
            $objTime = (Get-Item $objPath).LastWriteTimeUtc
            if ($objTime -ge $srcTime) {
                $needCompile = $false
            }
        }

        if ($needCompile) {
            Write-Host "Compiling SDK source: $rel"
            & clang++ @ClangCommonFlags -Wno-format -c $srcPath -o $objPath
            if ($LASTEXITCODE -ne 0) {
                Write-Error "Failed to compile SDK source $rel (exit code $LASTEXITCODE)"
                exit $LASTEXITCODE
            }
        }
    }

    $PluginSources = @(
        "StreamAudioPlugin.cpp",
        "PluginFactory.cpp",
        "UdpService.cpp"
    )

    $PluginObjs = @()
    foreach ($rel in $PluginSources) {
        $srcPath = Join-Path $SrcDir $rel
        $baseName = [System.IO.Path]::GetFileNameWithoutExtension($srcPath)
        $objPath = Join-Path $BuildDir "$baseName.o"
        $PluginObjs += $objPath

        Write-Host "Compiling plugin source: $rel"
        & clang++ @ClangCommonFlags -Wall -Wextra -Werror -c $srcPath -o $objPath
        if ($LASTEXITCODE -ne 0) {
            Write-Error "Failed to compile plugin source $rel (exit code $LASTEXITCODE)"
            exit $LASTEXITCODE
        }
    }

    $PluginDll = Join-Path $BundleDir "StreamAudioPlugin.vst3"
    $DefFile = Join-Path $SrcDir "exports.def"
    $AllObjs = $SdkObjs + $PluginObjs

    Write-Host "Linking $PluginDll..."
    & clang++ -shared @AllObjs $DefFile -lws2_32 -liphlpapi -lole32 -luuid -o $PluginDll
    if ($LASTEXITCODE -ne 0) {
        Write-Error "Plugin linking failed with exit code $LASTEXITCODE"
        exit $LASTEXITCODE
    }

    Write-Host "=== [build.ps1] StreamAudioPlugin.vst3 built successfully! ===" -ForegroundColor Green
}

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
        Build-Plugin

        Write-Host "=== [build.ps1] Running test_protocol ===" -ForegroundColor Cyan
        $TestProtocolSrc = Join-Path $TestsDir "test_protocol.cpp"
        $TestProtocolExe = Join-Path $BuildDir "test_protocol.exe"
        & clang++ @ClangCommonFlags -Wall -Wextra -Werror $TestProtocolSrc -o $TestProtocolExe
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
        & $TestProtocolExe
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

        Write-Host "=== [build.ps1] Running test_dsp_buffers ===" -ForegroundColor Cyan
        $TestDspSrc = Join-Path $TestsDir "test_dsp_buffers.cpp"
        $TestDspExe = Join-Path $BuildDir "test_dsp_buffers.exe"
        & clang++ @ClangCommonFlags -Wall -Wextra -Werror $TestDspSrc -o $TestDspExe
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
        & $TestDspExe
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

        Write-Host "=== [build.ps1] Running test_udp_service ===" -ForegroundColor Cyan
        $TestUdpSrc = Join-Path $TestsDir "test_udp_service.cpp"
        $UdpServiceSrc = Join-Path $SrcDir "UdpService.cpp"
        $TestUdpExe = Join-Path $BuildDir "test_udp_service.exe"
        & clang++ @ClangCommonFlags -Wall -Wextra -Werror $TestUdpSrc $UdpServiceSrc -lws2_32 -liphlpapi -o $TestUdpExe
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
        & $TestUdpExe
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

        Write-Host "=== [build.ps1] Running test_vst3_host ===" -ForegroundColor Cyan
        $TestHostSrc = Join-Path $TestsDir "test_vst3_host.cpp"
        $TestHostExe = Join-Path $BuildDir "test_vst3_host.exe"
        & clang++ @ClangCommonFlags -Wno-format $TestHostSrc -lole32 -luuid -o $TestHostExe
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
        & $TestHostExe
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

        Write-Host "=== [build.ps1] ALL 4 TEST SUITES PASSED SUCCESSFULLY! ===" -ForegroundColor Green
    }
    "plugin" {
        Build-Plugin
    }
    "install" {
        Write-Host "=== [build.ps1] Mode: install (stub for Task 6) ===" -ForegroundColor Yellow
    }
}
