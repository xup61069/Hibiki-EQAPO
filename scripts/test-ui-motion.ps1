param([string]$QtRoot = "")
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Import-Module (Join-Path $PSScriptRoot "VisualStudioTools.psm1") -Force
if ($QtRoot -eq "") {
    $qmake = Get-ChildItem (Join-Path $root "third_party\Qt") -Filter qmake.exe -Recurse |
        Where-Object { $_.FullName -match "\\msvc\d*_64\\bin\\qmake.exe$" } |
        Sort-Object FullName | Select-Object -First 1
    if (!$qmake) { throw "Qt qmake.exe not found." }
    $QtRoot = Split-Path -Parent (Split-Path -Parent $qmake.FullName)
}
$buildDir = Join-Path $root "_build\ui-motion"
New-Item -ItemType Directory -Force -Path $buildDir | Out-Null
$vsDevCmd = Get-VisualStudioDevCmd
$project = Join-Path $root "tests\ui-motion\ui-motion.pro"
$qmakePath = Join-Path $QtRoot "bin\qmake.exe"
$commands = @(
    "call `"$vsDevCmd`" -arch=x64 -host_arch=x64",
    "set `"CL=/external:anglebrackets /external:W0`"",
    "cd /d `"$buildDir`"",
    "`"$qmakePath`" `"$project`" -spec win32-msvc CONFIG+=release CONFIG-=debug",
    "nmake /nologo"
)
cmd /c ($commands -join " && ")
if ($LASTEXITCODE -ne 0) { throw "UI motion test build failed." }
$previousPath = $env:Path
$previousPlatform = $env:QT_QPA_PLATFORM
try {
    $env:Path = (Join-Path $QtRoot "bin") + ";" + $previousPath
    $env:QT_QPA_PLATFORM = "offscreen"
    $report = Join-Path $buildDir "results.txt"
    & (Join-Path $buildDir "release\ui-motion.exe") -o "$report,txt"
    if ($LASTEXITCODE -ne 0) { throw "UI motion runtime tests failed." }
    if (!(Test-Path -LiteralPath $report)) { throw "UI motion test report was not produced." }
    Get-Content -LiteralPath $report
} finally {
    $env:Path = $previousPath
    $env:QT_QPA_PLATFORM = $previousPlatform
}
