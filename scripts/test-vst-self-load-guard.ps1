param(
	[ValidateSet("Release")]
	[string]$Configuration = "Release"
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$root = Split-Path -Parent $PSScriptRoot
$benchmark = Join-Path $root "Benchmark\x64\$Configuration\Benchmark.exe"
if (!(Test-Path -LiteralPath $benchmark -PathType Leaf)) {
	throw "Benchmark executable not found: $benchmark"
}

$runtimeDir = Join-Path $root "third_party\vcpkg_installed\x64-windows\bin"
foreach ($runtimeName in @("fftw3.dll", "sndfile.dll")) {
	$runtimePath = Join-Path $runtimeDir $runtimeName
	if (!(Test-Path -LiteralPath $runtimePath -PathType Leaf)) {
		throw "Benchmark runtime dependency not found: $runtimePath"
	}
}
$originalProcessPath = $env:PATH

$generatedRoot = Join-Path $root "_build"
$testRoot = Join-Path $generatedRoot "vst-self-load-guard"
$aliasPath = Join-Path $testRoot "Benchmark-hardlink.exe"
$canonicalGeneratedRoot = [System.IO.Path]::GetFullPath($generatedRoot).TrimEnd('\')
$canonicalTestRoot = [System.IO.Path]::GetFullPath($testRoot).TrimEnd('\')
if (!$canonicalTestRoot.StartsWith(
		$canonicalGeneratedRoot + '\',
		[StringComparison]::OrdinalIgnoreCase)) {
	throw "Self-load test path escapes the generated build root: $canonicalTestRoot"
}

if (Test-Path -LiteralPath $canonicalTestRoot) {
	$testRootItem = Get-Item -LiteralPath $canonicalTestRoot -Force
	if (!$testRootItem.PSIsContainer -or
		($testRootItem.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
		throw "Self-load test root must be a regular directory: $canonicalTestRoot"
	}
	if (@(Get-ChildItem -LiteralPath $canonicalTestRoot -Force).Count -ne 0) {
		throw "Self-load test root must be empty: $canonicalTestRoot"
	}
}
else {
	New-Item -ItemType Directory -Path $canonicalTestRoot | Out-Null
}

try {
	$env:PATH = "$runtimeDir;$originalProcessPath"
	New-Item -ItemType HardLink -Path $aliasPath -Target $benchmark | Out-Null
	foreach ($candidate in @($benchmark, $aliasPath)) {
		& $benchmark --vst-self-identity-test $candidate
		$exitCode = $LASTEXITCODE
		if ($exitCode -ne 0) {
			throw "VST self-load guard failed for '$candidate' with exit code $exitCode."
		}
	}
}
finally {
	$env:PATH = $originalProcessPath
	if (Test-Path -LiteralPath $aliasPath -PathType Leaf) {
		[System.IO.File]::Delete($aliasPath)
	}
	if ((Test-Path -LiteralPath $canonicalTestRoot -PathType Container) -and
		@(Get-ChildItem -LiteralPath $canonicalTestRoot -Force).Count -eq 0) {
		[System.IO.Directory]::Delete($canonicalTestRoot)
	}
}

Write-Host "Native VST direct-path and hardlink self-load guard test passed."
