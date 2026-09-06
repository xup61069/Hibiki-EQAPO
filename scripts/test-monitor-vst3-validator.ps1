param(
	[ValidateSet("Release")]
	[string]$Configuration = "Release",
	[string]$ValidatorPath = "",
	[string]$BundlePath = ""
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$root = Split-Path -Parent $PSScriptRoot
$bundleName = "HibikiEQAPOMonitor.vst3"
if ([string]::IsNullOrWhiteSpace($ValidatorPath)) {
	$ValidatorPath = Join-Path $root "_build\vst3-validator-x64\bin\validator.exe"
}
if ([string]::IsNullOrWhiteSpace($BundlePath)) {
	$BundlePath = Join-Path $root "build\VST3\$Configuration\HibikiEQAPO\$bundleName"
}

function Resolve-RegularNonEmptyFile {
	param(
		[Parameter(Mandatory = $true)][string]$Path,
		[Parameter(Mandatory = $true)][string]$Description
	)

	if (!(Test-Path -LiteralPath $Path -PathType Leaf)) {
		throw "$Description not found: $Path"
	}
	$item = Get-Item -LiteralPath $Path -Force
	if (($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0 -or
		$item.Length -le 0) {
		throw "$Description must be a regular non-empty file: $Path"
	}
	return $item.FullName
}

$resolvedValidator = Resolve-RegularNonEmptyFile `
	-Path $ValidatorPath `
	-Description "VST3 validator"
if (!(Test-Path -LiteralPath $BundlePath -PathType Container)) {
	throw "Monitor VST3 bundle not found: $BundlePath"
}
$bundle = Get-Item -LiteralPath $BundlePath -Force
if (($bundle.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
	throw "Monitor VST3 bundle must not be a reparse point: $BundlePath"
}
$resolvedBundle = $bundle.FullName
$modulePath = Join-Path $resolvedBundle "Contents\x86_64-win\$bundleName"
$null = Resolve-RegularNonEmptyFile `
	-Path $modulePath `
	-Description "Monitor VST3 module"

# The validator writes some diagnostics to stderr even during a successful
# run. Capture both streams under non-terminating native-command handling, then
# decide success exclusively from the validator process exit code.
$previousErrorActionPreference = $ErrorActionPreference
try {
	$ErrorActionPreference = "Continue"
	$validationOutput = @(& $resolvedValidator -e $resolvedBundle 2>&1)
	$exitCode = $LASTEXITCODE
}
finally {
	$ErrorActionPreference = $previousErrorActionPreference
}
$validationOutput | ForEach-Object { Write-Host ([string]$_) }
if ($exitCode -ne 0) {
	throw "VST3 validator failed for '$resolvedBundle' with exit code $exitCode."
}

Write-Host "Official VST3 validator passed: $resolvedBundle"
