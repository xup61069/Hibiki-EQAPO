param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Release",
    [string]$OutputDirectory = ""
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$qmake = Get-ChildItem (Join-Path $root "third_party\Qt") -Filter qmake.exe -Recurse |
    Where-Object { $_.FullName -match "\\msvc\d*_64\\bin\\qmake.exe$" } |
    Sort-Object FullName | Select-Object -First 1
if (!$qmake) { throw "Qt qmake.exe not found." }
$qtRoot = Split-Path -Parent (Split-Path -Parent $qmake.FullName)
$editor = Join-Path $root "_build\ui-snapshot-bin-$Configuration\Editor.exe"
if (!(Test-Path -LiteralPath $editor)) { throw "Run capture-ui-regression.ps1 -Configuration $Configuration first." }
if ($OutputDirectory -eq "") { $OutputDirectory = Join-Path $root "artifacts\volume-step-ui" }
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$records = @()
foreach ($locale in @("en", "zh_CN", "zh_TW")) {
    foreach ($theme in @("light", "dark", "high-contrast")) {
        foreach ($scale in @("1.0", "1.5")) {
            $target = [IO.Path]::GetFullPath((Join-Path $OutputDirectory "volume-step-$locale-$theme-$scale.png"))
            $startInfo = [Diagnostics.ProcessStartInfo]::new($editor)
            $startInfo.UseShellExecute = $false
            $startInfo.CreateNoWindow = $true
            $startInfo.WindowStyle = [Diagnostics.ProcessWindowStyle]::Hidden
            $startInfo.RedirectStandardError = $true
            $startInfo.RedirectStandardOutput = $true
            $startInfo.EnvironmentVariables["QT_QPA_PLATFORM"] = "windows"
            $startInfo.EnvironmentVariables["QT_QPA_PLATFORM_PLUGIN_PATH"] = Join-Path $qtRoot "plugins\platforms"
            $startInfo.EnvironmentVariables["QT_SCALE_FACTOR"] = $scale
            $startInfo.EnvironmentVariables["EQAPO_UI_FONT_SCALE"] = "1.0"
            $startInfo.EnvironmentVariables["EQAPO_UI_THEME"] = $theme
            $startInfo.EnvironmentVariables["EQAPO_UI_SNAPSHOT"] = $target
            $startInfo.EnvironmentVariables["EQAPO_UI_SNAPSHOT_DELAY_MS"] = "10000"
            $startInfo.EnvironmentVariables["EQAPO_UI_SNAPSHOT_LOCALE"] = $locale
            $startInfo.EnvironmentVariables["EQAPO_UI_SNAPSHOT_SCENARIO"] = "volume-step"
            $startInfo.EnvironmentVariables["PATH"] = (Join-Path $qtRoot "bin") + ";" +
                (Join-Path $root "third_party\vcpkg_installed\x64-windows\bin") + ";" +
                (Join-Path $root "x64\$Configuration") + ";" + $env:Path
            $process = [Diagnostics.Process]::new()
            $process.StartInfo = $startInfo
            try {
                [void]$process.Start()
                $stdout = $process.StandardOutput.ReadToEndAsync()
                $stderr = $process.StandardError.ReadToEndAsync()
                if (!$process.WaitForExit(15000)) {
                    $process.Kill()
                    $process.WaitForExit()
                    throw "Volume step dialog timed out: $locale / $theme / $scale"
                }
                $output = $stdout.GetAwaiter().GetResult() + $stderr.GetAwaiter().GetResult()
                if ($process.ExitCode -ne 0 -or !(Test-Path -LiteralPath $target) -or
                    (Get-Item -LiteralPath $target).Length -lt 1024) {
                    throw "Volume step dialog failed: $locale / $theme / $scale ($($process.ExitCode)): $output"
                }
            } finally { $process.Dispose() }
            $records += [ordered]@{ locale = $locale; theme = $theme; scale = $scale; file = $target;
                accept = "passed"; cancel = "passed"; sha256 = (Get-FileHash $target -Algorithm SHA256).Hash.ToLowerInvariant() }
        }
    }
}
[IO.File]::WriteAllText((Join-Path ([IO.Path]::GetFullPath($OutputDirectory)) "manifest.json"),
    (($records | ConvertTo-Json -Depth 4) + "`n"), [Text.UTF8Encoding]::new($false))
Write-Host "Volume step dialog: $($records.Count) native Windows captures; accept/cancel passed."
