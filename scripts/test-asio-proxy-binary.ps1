param(
	[Parameter(Mandatory = $true)]
	[string]$Path
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

function Test-ByteSequence {
	param(
		[Parameter(Mandatory = $true)][byte[]]$Bytes,
		[Parameter(Mandatory = $true)][byte[]]$Sequence
	)

	if ($Sequence.Length -eq 0 -or $Bytes.Length -lt $Sequence.Length) {
		return $false
	}
	$lastStart = $Bytes.Length - $Sequence.Length
	for ($offset = 0; $offset -le $lastStart; $offset++) {
		$matches = $true
		for ($index = 0; $index -lt $Sequence.Length; $index++) {
			if ($Bytes[$offset + $index] -ne $Sequence[$index]) {
				$matches = $false
				break
			}
		}
		if ($matches) {
			return $true
		}
	}
	return $false
}

$resolvedPath = [IO.Path]::GetFullPath($Path)
Import-Module (Join-Path $PSScriptRoot "MonitorVST3Binary.psm1") -Force
$metadata = Read-Amd64PeMetadata -Path $resolvedPath
if (($metadata.Characteristics -band 0x2000) -eq 0) {
	throw "Hibiki EQAPO driver is not marked IMAGE_FILE_DLL: $resolvedPath"
}

$requiredExports = @(
	"DllCanUnloadNow",
	"DllGetClassObject",
	"DllRegisterServer",
	"DllUnregisterServer"
)
$actualExports = [Collections.Generic.HashSet[string]]::new(
	[StringComparer]::Ordinal
)
foreach ($export in @($metadata.Exports)) {
	if (!$actualExports.Add([string]$export)) {
		throw "Hibiki EQAPO driver has a duplicate export '$export'."
	}
}
if ($actualExports.Count -ne $requiredExports.Count) {
	throw "Hibiki EQAPO driver must export exactly the four COM entry points."
}
foreach ($requiredExport in $requiredExports) {
	if (!$actualExports.Contains($requiredExport)) {
		throw "Hibiki EQAPO driver is missing COM export '$requiredExport'."
	}
}

$allowedImports = [Collections.Generic.HashSet[string]]::new(
	[StringComparer]::OrdinalIgnoreCase
)
foreach ($dependency in @(
	"ADVAPI32.dll",
	"CRYPT32.dll",
	"dbghelp.dll",
	"KERNEL32.dll",
	"MSVCP140.dll",
	"MSVCP140_1.dll",
	"MSVCP140_2.dll",
	"ole32.dll",
	"SHLWAPI.dll",
	"ucrtbase.dll",
	"USER32.dll",
	"VCRUNTIME140.dll",
	"VCRUNTIME140_1.dll",
	"WINMM.dll"
)) {
	[void]$allowedImports.Add($dependency)
}
foreach ($kind in @("NormalImports", "DelayImports")) {
	foreach ($dependency in @($metadata.$kind)) {
		$name = [string]$dependency
		if ($name.Equals("HibikiEQAPODriver.dll", [StringComparison]::OrdinalIgnoreCase)) {
			throw "Hibiki EQAPO driver must not import itself."
		}
		$isSystemApiSet =
			$name.StartsWith("api-ms-win-", [StringComparison]::OrdinalIgnoreCase) -or
			$name.StartsWith("ext-ms-win-", [StringComparison]::OrdinalIgnoreCase)
		if (!$isSystemApiSet -and !$allowedImports.Contains($name)) {
			$label = if ($kind -eq "DelayImports") { "delay-import" } else { "import" }
			throw "Hibiki EQAPO driver has disallowed $label dependency '$name'. The DAW-process proxy must use the isolated static dependency graph."
		}
	}
}

# Keep the stable proxy identity and canonical target-store contract in the
# shipped image. Together with source-level tests for samePhysicalFile(), these
# markers guard against accidentally building a pass-through DLL that can load
# its own CLSID/path recursively.
$image = [IO.File]::ReadAllBytes($resolvedPath)
$driverClsidBytes = [Text.Encoding]::Unicode.GetBytes(
	"{D47C55C9-3F7D-422F-86E9-32E170815D53}"
)
$targetStoreBytes = [Text.Encoding]::Unicode.GetBytes(
	"Software\EqualizerAPO\ASIOProxy"
)
if (!(Test-ByteSequence -Bytes $image -Sequence $driverClsidBytes)) {
	throw "Hibiki EQAPO driver image does not contain the stable proxy CLSID."
}
if (!(Test-ByteSequence -Bytes $image -Sequence $targetStoreBytes)) {
	throw "Hibiki EQAPO driver image does not contain the canonical ASIO target store."
}

$successMessage = (
	"Hibiki EQAPO proxy-driver binary validation passed: {0} " +
	"(exports={1}, normal-imports={2}, delay-imports={3})"
) -f
	$resolvedPath,
	$actualExports.Count,
	@($metadata.NormalImports).Count,
	@($metadata.DelayImports).Count
Write-Host $successMessage
