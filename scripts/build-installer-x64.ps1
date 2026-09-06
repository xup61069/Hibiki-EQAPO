param(
	[string]$Configuration = "Release",
	[string]$VisualStudioEdition = "",
	[string]$Makensis = ""
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot

& (Join-Path $root "scripts\bootstrap-third-party.ps1") -Configuration $Configuration -WithQt -WithNsis
& python -B (Join-Path $root "tests\test_asio_installer_discovery_runtime.py") -v
if ($LASTEXITCODE -ne 0) {
	throw "ASIO installer discovery regression failed with exit code $LASTEXITCODE"
}
& python -B (Join-Path $root "tests\test_installer_process_stopper_nsis_runtime.py") -v
if ($LASTEXITCODE -ne 0) {
	throw "process-stopper NSIS quote-boundary regression failed with exit code $LASTEXITCODE"
}
& (Join-Path $root "build-local-x64.ps1") `
	-Configuration $Configuration `
	-VisualStudioEdition $VisualStudioEdition `
	-Rebuild
if ($LASTEXITCODE -ne 0) {
	throw "native build failed with exit code $LASTEXITCODE"
}
& (Join-Path $root "scripts\build-qt-apps-x64.ps1") -Configuration $Configuration -VisualStudioEdition $VisualStudioEdition
if ($LASTEXITCODE -ne 0) {
	throw "Qt app build failed with exit code $LASTEXITCODE"
}
& (Join-Path $root "scripts\stage-installer-x64.ps1") `
	-Configuration $Configuration `
	-VisualStudioEdition $VisualStudioEdition
if ($LASTEXITCODE -ne 0) {
	throw "staging failed with exit code $LASTEXITCODE"
}

if ($Makensis -eq "") {
	$localMakensis = Join-Path $root "third_party\nsis-3.11\makensis.exe"
	if (Test-Path -LiteralPath $localMakensis) {
		$Makensis = $localMakensis
	}
}

if ($Makensis -eq "") {
	$makensisCommand = Get-Command makensis.exe -ErrorAction SilentlyContinue
	if ($null -ne $makensisCommand) {
		$Makensis = $makensisCommand.Source
	}
}

if ($Makensis -eq "") {
	$candidates = @(
		Join-Path ${env:ProgramFiles} "NSIS\makensis.exe",
		Join-Path ${env:ProgramFiles(x86)} "NSIS\makensis.exe"
	)
	$found = $candidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
	if ($null -ne $found) {
		$Makensis = $found
	}
}

if ($Makensis -eq "") {
	throw "makensis.exe was not found. Install NSIS or pass -Makensis <path>."
}

& (Join-Path $root "scripts\test-installer-recovery-manifest.ps1") -Makensis $Makensis

Push-Location (Join-Path $root "Setup")
try {
	& $Makensis "/WX" "/INPUTCHARSET" "UTF8" "/DCONFIGURATION=$Configuration" ".\Setup64.nsi"
	if ($LASTEXITCODE -ne 0) {
		throw "makensis failed with exit code $LASTEXITCODE"
	}
}
finally {
	Pop-Location
}

$version = & (Join-Path $root "scripts\get-project-version.ps1")
$installer = Join-Path $root "Setup\Hibiki-EQAPO-x64-$version.exe"
if (!(Test-Path -LiteralPath $installer)) {
	throw "Expected installer was not produced: $installer"
}

$checksum = Get-FileHash -LiteralPath $installer -Algorithm SHA256
$checksumPath = "$installer.sha256"
"$($checksum.Hash.ToLowerInvariant())  $([System.IO.Path]::GetFileName($installer))" |
	Set-Content -LiteralPath $checksumPath -Encoding ascii

Write-Host "Installer build finished: $installer"
Write-Host "SHA-256: $($checksum.Hash.ToLowerInvariant())"
