param(
	[ValidateSet("Release")]
	[string]$Configuration = "Release",
	[string]$BundlePath = "",
	[string]$VisualStudioEdition = ""
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$root = Split-Path -Parent $PSScriptRoot
$bundleName = "HibikiEQAPOMonitor.vst3"
if ([string]::IsNullOrWhiteSpace($BundlePath)) {
	$BundlePath = Join-Path $root "build\VST3\$Configuration\HibikiEQAPO\$bundleName"
}
$modulePath = Join-Path $BundlePath "Contents\x86_64-win\$bundleName"
if (!(Test-Path -LiteralPath $modulePath -PathType Leaf)) {
	throw "Monitor VST3 module not found: $modulePath"
}
$module = Get-Item -LiteralPath $modulePath -Force
if (($module.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0 -or
	$module.Length -le 0) {
	throw "Monitor VST3 module must be a regular non-empty file: $modulePath"
}

Import-Module (Join-Path $PSScriptRoot "MonitorVST3Binary.psm1") -Force
$null = $VisualStudioEdition # Kept for compatibility with existing build callers.
$metadata = Assert-MonitorVST3Binary -ModulePath $module.FullName

$successMessage = (
	"Monitor VST3 AMD64 PE normal/delay import audit passed: {0} " +
	"(normal={1}, delay={2})"
) -f
	$module.FullName,
	@($metadata.NormalImports).Count,
	@($metadata.DelayImports).Count
Write-Host $successMessage
