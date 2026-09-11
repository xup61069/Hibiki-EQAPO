param(
	[string]$Configuration = "Release",
	[string]$PlatformToolset = "",
	[string]$VisualStudioEdition = "",
	[string]$MsBuildCommand = "msbuild",
	[switch]$Rebuild
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$thirdParty = Join-Path $root "third_party"
$triplet = "x64-windows"
$monitorTriplet = "x64-windows-static-md"

$callerEnvironment = @{}
foreach ($entry in [Environment]::GetEnvironmentVariables("Process").GetEnumerator()) {
	$callerEnvironment[[string]$entry.Key] = [string]$entry.Value
}

function Set-ExactProcessEnvironment {
	param([Parameter(Mandatory = $true)][hashtable]$Variables)

	foreach ($variableName in @([Environment]::GetEnvironmentVariables("Process").Keys)) {
		if (![string]::IsNullOrEmpty([string]$variableName)) {
			[Environment]::SetEnvironmentVariable([string]$variableName, $null, "Process")
		}
	}
	foreach ($entry in $Variables.GetEnumerator()) {
		if (![string]::IsNullOrEmpty([string]$entry.Key)) {
			[Environment]::SetEnvironmentVariable(
				[string]$entry.Key,
				[string]$entry.Value,
				"Process"
			)
		}
	}
}

function Get-CanonicalPathWithinRoot {
	param(
		[Parameter(Mandatory = $true)][string]$Path,
		[Parameter(Mandatory = $true)][string]$Root,
		[Parameter(Mandatory = $true)][string]$Description
	)

	$canonicalPath = [System.IO.Path]::GetFullPath($Path).TrimEnd('\')
	$canonicalRoot = [System.IO.Path]::GetFullPath($Root).TrimEnd('\')
	$rootPrefix = $canonicalRoot + '\'
	if (!$canonicalPath.Equals($canonicalRoot, [StringComparison]::OrdinalIgnoreCase) -and
		!$canonicalPath.StartsWith($rootPrefix, [StringComparison]::OrdinalIgnoreCase)) {
		throw "$Description is outside its trusted root: $canonicalPath"
	}
	return $canonicalPath
}

function Assert-NoReparsePoint {
	param(
		[Parameter(Mandatory = $true)][string]$Path,
		[Parameter(Mandatory = $true)][string]$Root,
		[Parameter(Mandatory = $true)][string]$Description
	)

	$canonicalPath = Get-CanonicalPathWithinRoot -Path $Path -Root $Root -Description $Description
	$canonicalRoot = [System.IO.Path]::GetFullPath($Root).TrimEnd('\')
	$current = $canonicalPath
	while ($true) {
		$item = Get-Item -LiteralPath $current -Force -ErrorAction Stop
		if (($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
			throw "$Description traverses a reparse point: $current"
		}
		if ($current.Equals($canonicalRoot, [StringComparison]::OrdinalIgnoreCase)) {
			break
		}
		$current = [System.IO.Directory]::GetParent($current).FullName.TrimEnd('\')
	}
	return $canonicalPath
}

# Resolve all bootstrap locations through Win32-backed .NET APIs before any
# Visual Studio discovery. The caller may control the inherited environment,
# so never use it to find cmd.exe, vswhere.exe, or the Windows SDK.
$systemDirectory = [Environment]::SystemDirectory
$windowsDirectory = if ([string]::IsNullOrWhiteSpace($systemDirectory)) {
	""
} else {
	[System.IO.Directory]::GetParent($systemDirectory).FullName
}
$programFilesX86 = [Environment]::GetFolderPath(
	[Environment+SpecialFolder]::ProgramFilesX86
)
$programFiles = [Environment]::GetFolderPath(
	[Environment+SpecialFolder]::ProgramFiles
)
$commonProgramFilesX86 = [Environment]::GetFolderPath(
	[Environment+SpecialFolder]::CommonProgramFilesX86
)
$commonProgramFiles = [Environment]::GetFolderPath(
	[Environment+SpecialFolder]::CommonProgramFiles
)
$programData = [Environment]::GetFolderPath(
	[Environment+SpecialFolder]::CommonApplicationData
)
$localApplicationData = [Environment]::GetFolderPath(
	[Environment+SpecialFolder]::LocalApplicationData
)
$applicationData = [Environment]::GetFolderPath(
	[Environment+SpecialFolder]::ApplicationData
)
$userProfile = [Environment]::GetFolderPath(
	[Environment+SpecialFolder]::UserProfile
)
$safeTemp = Join-Path $localApplicationData "Temp"
$cmdExe = Join-Path $systemDirectory "cmd.exe"
$wbemDirectory = Join-Path $systemDirectory "Wbem"

foreach ($trustedDirectory in @(
	$systemDirectory,
	$windowsDirectory,
	$programFilesX86,
	$programFiles,
	$commonProgramFilesX86,
	$commonProgramFiles,
	$programData,
	$applicationData,
	$localApplicationData,
	$userProfile,
	$safeTemp
)) {
	if ([string]::IsNullOrWhiteSpace($trustedDirectory) -or
		!(Test-Path -LiteralPath $trustedDirectory -PathType Container)) {
		throw "A trusted Windows bootstrap directory could not be resolved: $trustedDirectory"
	}
}
if (!(Test-Path -LiteralPath $cmdExe -PathType Leaf)) {
	throw "The trusted Windows command processor was not found: $cmdExe"
}

$systemDrive = [System.IO.Path]::GetPathRoot($windowsDirectory).TrimEnd('\')
$homeDrive = [System.IO.Path]::GetPathRoot($userProfile).TrimEnd('\')
$homePath = $userProfile.Substring([System.IO.Path]::GetPathRoot($userProfile).Length - 1)
$bootstrapEnvironment = @{
	"ALLUSERSPROFILE" = $programData
	"APPDATA" = $applicationData
	"CommonProgramFiles" = $commonProgramFiles
	"CommonProgramFiles(x86)" = $commonProgramFilesX86
	"CommonProgramW6432" = $commonProgramFiles
	"ComSpec" = $cmdExe
	"HOMEDRIVE" = $homeDrive
	"HOMEPATH" = $homePath
	"LOCALAPPDATA" = $localApplicationData
	"NUMBER_OF_PROCESSORS" = [Environment]::ProcessorCount.ToString(
		[Globalization.CultureInfo]::InvariantCulture
	)
	"OS" = "Windows_NT"
	"Path" = "$systemDirectory;$windowsDirectory;$wbemDirectory"
	"PATHEXT" = ".COM;.EXE;.BAT;.CMD"
	"PROCESSOR_ARCHITECTURE" = if ([Environment]::Is64BitOperatingSystem) { "AMD64" } else { "x86" }
	"ProgramData" = $programData
	"ProgramFiles" = $programFiles
	"ProgramFiles(x86)" = $programFilesX86
	"ProgramW6432" = $programFiles
	"SystemDrive" = $systemDrive
	"SystemRoot" = $windowsDirectory
	"TEMP" = $safeTemp
	"TMP" = $safeTemp
	"USERPROFILE" = $userProfile
	"windir" = $windowsDirectory
}

try {
	# Environment variables are initial MSBuild properties. Start both VsDevCmd
	# and MSBuild from an allowlist so an arbitrary caller variable cannot become
	# an import path before project policy is evaluated.
	Import-Module (Join-Path $root "scripts\VisualStudioTools.psm1") -Force
	Set-ExactProcessEnvironment -Variables $bootstrapEnvironment

function ConvertTo-CmdArgument {
	param([Parameter(Mandatory = $true)][string]$Value)

	# cmd expands percent variables even inside quotes, while quotes and newlines
	# can terminate an argument or command. Fail explicitly rather than execute a
	# command line different from the one represented by the PowerShell values.
	if ($Value.Contains('"') -or
		$Value.Contains('%') -or
		$Value.Contains("`r") -or
		$Value.Contains("`n")) {
		throw "Cannot safely pass value to cmd.exe: $Value"
	}
	return '"' + $Value + '"'
}

function Assert-CommandLength {
	param(
		[Parameter(Mandatory = $true)][string]$Executable,
		[Parameter(Mandatory = $true)][string[]]$Arguments
	)

	# CreateProcess supports a 32767-character command line. Batch files are
	# routed through cmd.exe and therefore retain its much smaller 8191 limit.
	$extension = [System.IO.Path]::GetExtension($Executable)
	$limit = if ($extension -in @(".cmd", ".bat")) { 7500 } else { 30000 }
	$estimatedLength = $Executable.Length + 3
	foreach ($argument in $Arguments) {
		$estimatedLength += $argument.Length + 3
	}
	if ($estimatedLength -gt $limit) {
		throw "Build command is too long ($estimatedLength characters; safe limit $limit). Move the repository closer to the drive root."
	}
}

if ($Configuration -notin @("Debug", "Release")) {
	throw "Unsupported configuration '$Configuration'. Use Debug or Release."
}
if ([string]::IsNullOrWhiteSpace($MsBuildCommand)) {
	throw "MsBuildCommand must not be empty."
}
$vcpkgLibSubdirectory = if ($Configuration -eq "Debug") { "debug\lib" } else { "lib" }

$paths = @{
	LIBSNDFILE_INCLUDE = Join-Path $thirdParty "vcpkg_installed\$triplet\include"
	LIBSNDFILE_LIB = Join-Path $thirdParty "vcpkg_installed\$triplet\$vcpkgLibSubdirectory"
	FFTW_INCLUDE = Join-Path $thirdParty "vcpkg_installed\$triplet\include"
	FFTW_LIB = Join-Path $thirdParty "vcpkg_installed\$triplet\$vcpkgLibSubdirectory"
	MUPARSERX_INCLUDE = Join-Path $thirdParty "muparserx\parser"
	MUPARSERX_LIB = Join-Path $thirdParty "build\muparserx-$triplet\$Configuration"
	TCLAP_ROOT = Join-Path $thirdParty "tclap"
	VST3_SDK_ROOT = Join-Path $thirdParty "vst3sdk"
	ASIO_SDK_INCLUDE = Join-Path $thirdParty "vcpkg_installed\$triplet\include\asiosdk\common"
	MONITOR_STATIC_LIB_DIR = Join-Path $thirdParty "monitor_vcpkg_installed\$monitorTriplet\$vcpkgLibSubdirectory"
}

foreach ($path in $paths.GetEnumerator()) {
	if (!(Test-Path -LiteralPath $path.Value)) {
		throw "Missing dependency path $($path.Key): $($path.Value)"
	}
}

$visualStudioRoot = [System.IO.Path]::GetFullPath(
	(Get-VisualStudioInstallation -Edition $VisualStudioEdition)
).TrimEnd('\')
if (!(Test-Path -LiteralPath $visualStudioRoot -PathType Container)) {
	throw "The selected Visual Studio installation does not exist: $visualStudioRoot"
}
$monitorVCRuntimeDlls = @(
	"msvcp140.dll",
	"msvcp140_1.dll",
	"vcruntime140.dll",
	"vcruntime140_1.dll"
)
$monitorVCRuntimeDir = Get-VisualStudioRedistDirectory `
	-Edition $VisualStudioEdition `
	-RequiredFiles $monitorVCRuntimeDlls
$monitorVCRuntimeDir = Assert-NoReparsePoint `
	-Path $monitorVCRuntimeDir `
	-Root $visualStudioRoot `
	-Description "Monitor VST3 Visual C++ runtime directory"
$vsDevCmd = Assert-NoReparsePoint `
	-Path (Join-Path $visualStudioRoot "Common7\Tools\VsDevCmd.bat") `
	-Root $visualStudioRoot `
	-Description "Visual Studio developer command script"
if (!(Test-Path -LiteralPath $vsDevCmd -PathType Leaf)) {
	throw "VsDevCmd.bat was not found: $vsDevCmd"
}

$windowsKitInclude = Join-Path $programFilesX86 "Windows Kits\10\Include"
$windowsSdkVersion = ""
if (Test-Path -LiteralPath $windowsKitInclude) {
	$windowsSdkVersion = Get-ChildItem -LiteralPath $windowsKitInclude -Directory |
		Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName "um\Windows.h") } |
		Sort-Object Name -Descending |
		Select-Object -First 1 -ExpandProperty Name
}
if ($windowsSdkVersion -eq "") {
	throw "Windows 10 SDK was not found under $windowsKitInclude"
}
if ($windowsSdkVersion -notmatch '^\d+\.\d+\.\d+\.\d+$') {
	throw "Windows SDK directory has an unsafe version name: $windowsSdkVersion"
}

$projects = @(
	"Common.vcxproj",
	"HibikiEQAPODriver\HibikiEQAPODriver.vcxproj",
	"HibikiEQAPODriver\Tests\AsioProxyCoreTests.vcxproj",
	"HibikiEQAPODriver\Tests\AsioProxyDriverTests.vcxproj",
	"MonitorVST3\MonitorVST3.vcxproj",
	"EqualizerAPO\EqualizerAPO.vcxproj",
	"EqApoOutProcHost\EqApoOutProcHost.vcxproj",
	"Benchmark\Benchmark.vcxproj",
	"VoicemeeterClient\VoicemeeterClient.vcxproj"
)

$nativeTestExecutables = @(
	"HibikiEQAPODriver\Tests\x64\$Configuration\AsioProxyCoreTests.exe",
	"HibikiEQAPODriver\Tests\x64\$Configuration\AsioProxyDriverTests.exe"
)

$quotedTargetSdk = ConvertTo-CmdArgument "WindowsTargetPlatformVersion=$windowsSdkVersion"
$quotedSdk = ConvertTo-CmdArgument "WindowsSDKVersion=$windowsSdkVersion\"
$quotedVsDevCmd = ConvertTo-CmdArgument $vsDevCmd
$devCmd = "set $quotedTargetSdk && set $quotedSdk && call $quotedVsDevCmd -no_logo -winsdk=$windowsSdkVersion >nul && set"
$devEnvironment = & $cmdExe /D /V:OFF /S /C $devCmd
if ($LASTEXITCODE -ne 0) {
	throw "Visual Studio developer environment initialization failed with exit code $LASTEXITCODE"
}
$devEnvironmentValues = @{}
foreach ($line in $devEnvironment) {
	if ($line -match '^([^=][^=]*)=(.*)$') {
		$devEnvironmentValues[$Matches[1]] = $Matches[2]
	}
}

$allowedDeveloperEnvironmentNames = @(
	"CommandPromptType",
	"DevEnvDir",
	"ExtensionSdkDir",
	"EXTERNAL_INCLUDE",
	"Framework40Version",
	"FrameworkDir",
	"FrameworkDir32",
	"FrameworkVersion",
	"FrameworkVersion32",
	"INCLUDE",
	"LIB",
	"LIBPATH",
	"NETFXSDKDir",
	"Path",
	"UCRTVersion",
	"UniversalCRTSdkDir",
	"VCIDEInstallDir",
	"VCINSTALLDIR",
	"VCToolsInstallDir",
	"VCToolsRedistDir",
	"VCToolsVersion",
	"VisualStudioVersion",
	"VSINSTALLDIR",
	"VSCMD_VER",
	"WindowsLibPath",
	"WindowsSDK_ExecutablePath_x64",
	"WindowsSDK_ExecutablePath_x86",
	"WindowsSdkBinPath",
	"WindowsSdkDir",
	"WindowsSDKLibVersion",
	"WindowsSdkVerBinPath",
	"WindowsSDKVersion"
)
$buildEnvironment = $bootstrapEnvironment.Clone()
foreach ($name in $allowedDeveloperEnvironmentNames) {
	if ($devEnvironmentValues.ContainsKey($name)) {
		$buildEnvironment[$name] = $devEnvironmentValues[$name]
	}
}
foreach ($entry in $devEnvironmentValues.GetEnumerator()) {
	if ($entry.Key -match '^VSCMD_ARG_[A-Za-z0-9_]+$' -or
		$entry.Key -match '^VS\d+COMNTOOLS$') {
		$buildEnvironment[$entry.Key] = $entry.Value
	}
}

if (!$buildEnvironment.ContainsKey("VisualStudioVersion") -or
	$buildEnvironment["VisualStudioVersion"] -notmatch '^(\d+)\.0$') {
	throw "VsDevCmd returned an unsafe VisualStudioVersion."
}
$vcTargetsVersion = "v$($Matches[1])0"
$trustedVCTargetsPath = Assert-NoReparsePoint `
	-Path (Join-Path $visualStudioRoot "MSBuild\Microsoft\VC\$vcTargetsVersion") `
	-Root $visualStudioRoot `
	-Description "Visual C++ targets directory"
foreach ($requiredTarget in @(
	"Microsoft.Cpp.Default.props",
	"Microsoft.Cpp.props",
	"Microsoft.Cpp.targets"
)) {
	if (!(Test-Path -LiteralPath (Join-Path $trustedVCTargetsPath $requiredTarget) -PathType Leaf)) {
		throw "The trusted Visual C++ targets directory is incomplete: $requiredTarget"
	}
}
# Keep the separator expected by several VC targets, but use a forward slash so
# a quoted native argument cannot end in a backslash and swallow the next token
# when an explicit .cmd MSBuild shim is used by the regression harness.
$trustedVCTargetsPath += '/'

foreach ($devPathCheck in @(
	@("VSINSTALLDIR", $visualStudioRoot),
	@("VCINSTALLDIR", $visualStudioRoot),
	@("VCToolsInstallDir", $visualStudioRoot),
	@("WindowsSdkDir", (Join-Path $programFilesX86 "Windows Kits")),
	@("UniversalCRTSdkDir", (Join-Path $programFilesX86 "Windows Kits"))
)) {
	$name = $devPathCheck[0]
	if (!$buildEnvironment.ContainsKey($name) -or
		[string]::IsNullOrWhiteSpace($buildEnvironment[$name])) {
		throw "VsDevCmd did not provide required environment path $name."
	}
	$null = Get-CanonicalPathWithinRoot `
		-Path $buildEnvironment[$name] `
		-Root $devPathCheck[1] `
		-Description "VsDevCmd environment path $name"
}

$disabledUserRoot = Join-Path $systemDirectory "EqApoBuild.NoUserProps"
if (Test-Path -LiteralPath $disabledUserRoot) {
	throw "The protected disabled-user-props path unexpectedly exists: $disabledUserRoot"
}

Set-ExactProcessEnvironment -Variables $buildEnvironment

$resolvedMsBuild = Get-Command $MsBuildCommand -CommandType Application -ErrorAction Stop |
	Select-Object -First 1
if ($MsBuildCommand -in @("msbuild", "msbuild.exe")) {
	$null = Assert-NoReparsePoint `
		-Path $resolvedMsBuild.Source `
		-Root $visualStudioRoot `
		-Description "MSBuild executable"
}

$props = @(
	"/p:Configuration=$Configuration",
	"/p:Platform=x64",
	"/p:LIBSNDFILE_INCLUDE=$($paths.LIBSNDFILE_INCLUDE)",
	"/p:LIBSNDFILE_LIB=$($paths.LIBSNDFILE_LIB)",
	"/p:FFTW_INCLUDE=$($paths.FFTW_INCLUDE)",
	"/p:FFTW_LIB=$($paths.FFTW_LIB)",
	"/p:MUPARSERX_INCLUDE=$($paths.MUPARSERX_INCLUDE)",
	"/p:MUPARSERX_LIB=$($paths.MUPARSERX_LIB)",
	"/p:TCLAP_ROOT=$($paths.TCLAP_ROOT)",
	"/p:VST3_SDK_ROOT=$($paths.VST3_SDK_ROOT)",
	"/p:ASIO_SDK_INCLUDE=$($paths.ASIO_SDK_INCLUDE)",
	"/p:MONITOR_STATIC_LIB_DIR=$($paths.MONITOR_STATIC_LIB_DIR)",
	"/p:MONITOR_VC_RUNTIME_DIR=$monitorVCRuntimeDir",
	"/p:TreatWarningAsError=true",
	"/p:WindowsTargetPlatformVersion=$windowsSdkVersion",
	"/p:VCTargetsPath=$trustedVCTargetsPath",
	"/p:UserRootDir=$disabledUserRoot",
	"/p:MSBuildUserExtensionsPath=$disabledUserRoot",
	"/p:ImportDirectoryBuildProps=false",
	"/p:ImportDirectoryBuildTargets=false",
	"/p:ImportUserLocationsByWildcardBeforeMicrosoftCommonProps=false",
	"/p:ImportUserLocationsByWildcardAfterMicrosoftCommonProps=false",
	"/p:ImportUserLocationsByWildcardBeforeMicrosoftCommonTargets=false",
	"/p:ImportUserLocationsByWildcardAfterMicrosoftCommonTargets=false",
	"/p:CustomBeforeMicrosoftCommonProps=",
	"/p:CustomAfterMicrosoftCommonProps=",
	"/p:CustomBeforeMicrosoftCommonTargets=",
	"/p:CustomAfterMicrosoftCommonTargets=",
	"/p:ForceImportBeforeCppProps=",
	"/p:ForceImportAfterCppProps=",
	"/p:ForceImportAfterCppDefaultProps=",
	"/p:ForceImportBeforeCppTargets=",
	"/p:ForceImportAfterCppTargets=",
	"/p:VcpkgManifestDirectory=",
	"/p:VcpkgRoot=",
	"/p:VcpkgActivationOptions=",
	"/p:VCLibPackagePath=",
	"/p:ImportBeforeCppProps=",
	"/p:ImportAfterCppProps=",
	"/p:ImportBeforeCppTargets=",
	"/p:ImportAfterCppTargets=",
	"/noAutoResponse",
	"/nodeReuse:false",
	"/m"
)
if ($PlatformToolset -ne "") {
	$props += "/p:PlatformToolset=$PlatformToolset"
}
Push-Location $root
try {
	foreach ($project in $projects) {
		[string[]]$targetArguments = @()
		if ($Rebuild) {
			$targetArguments = @("/t:Rebuild")
		}
		$projectProperties = if ($Rebuild -and $project -ne "Common.vcxproj") {
			@("/p:BuildProjectReferences=false") + $props
		} else {
			$props
		}
		# Invoke each project directly so repository paths are not multiplied into
		# one cmd.exe command line (whose hard limit is only 8191 characters).
		Assert-CommandLength `
			-Executable $resolvedMsBuild.Source `
			-Arguments (@($project) + $targetArguments + $projectProperties)
		& $resolvedMsBuild.Source $project @targetArguments @projectProperties
		if ($LASTEXITCODE -ne 0) {
			throw "Native x64 build failed for $project with exit code $LASTEXITCODE"
		}
	}
	foreach ($testExecutable in $nativeTestExecutables) {
		$testPath = Join-Path $root $testExecutable
		if (!(Test-Path -LiteralPath $testPath -PathType Leaf)) {
			throw "Native x64 test executable was not produced: $testExecutable"
		}
		& $testPath
		if ($LASTEXITCODE -ne 0) {
			throw "Native x64 test failed: $testExecutable (exit code $LASTEXITCODE)"
		}
	}
}
finally {
	Pop-Location
}
}
finally {
	# The installer orchestrator invokes this script in-process before its Qt and
	# packaging steps, so do not leak the hermetic MSBuild environment outward.
	Set-ExactProcessEnvironment -Variables $callerEnvironment
}
