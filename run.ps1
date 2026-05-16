param(
    [ValidateSet("build", "rebuild", "install", "start", "stop", "restart", "status", "uninstall")]
    [string]$Action = "build",
    [ValidateSet("Debug", "Release")]
    [string]$Config = "Debug"
)

$ErrorActionPreference = "Stop"

$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$BuildDir = Join-Path $Root "out\build\x64-$Config"
$ServiceExe = Join-Path $BuildDir "WitcherTrayService.exe"
$VcVars = "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"

function Invoke-CMakeBuild {
    $cmd = "call ""$VcVars"" && cmake -S . -B ""$BuildDir"" -G Ninja -DCMAKE_BUILD_TYPE=$Config && cmake --build ""$BuildDir"" --config $Config"
    cmd /c $cmd
    if ($LASTEXITCODE -ne 0) {
        throw "Build failed with exit code $LASTEXITCODE"
    }
}

function Stop-RunningBinaries {
    try { taskkill /F /IM TrayWin32App.exe *> $null } catch {}
    try { taskkill /F /IM WitcherTrayService.exe *> $null } catch {}
}

switch ($Action) {
    "build" {
        Invoke-CMakeBuild
    }
    "rebuild" {
        Stop-RunningBinaries
        if (Test-Path $BuildDir) {
            Remove-Item -Recurse -Force $BuildDir
        }
        Invoke-CMakeBuild
    }
    "install" {
        if (!(Test-Path $ServiceExe)) {
            Invoke-CMakeBuild
        }
        & $ServiceExe --install
    }
    "start" {
        sc.exe start WitcherTrayService
    }
    "stop" {
        sc.exe stop WitcherTrayService
    }
    "restart" {
        sc.exe stop WitcherTrayService | Out-Null
        Start-Sleep -Seconds 1
        sc.exe start WitcherTrayService
    }
    "status" {
        Get-Service WitcherTrayService | Select-Object Name, Status, StartType
    }
    "uninstall" {
        if (Test-Path $ServiceExe) {
            & $ServiceExe --uninstall
        } else {
            throw "Service executable not found: $ServiceExe"
        }
    }
}
