param(
	[switch]$CompileOnly
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$version = & (Join-Path $PSScriptRoot "get-project-version.ps1")
$setupDirectory = Join-Path $root "Setup"
$outputDirectory = Join-Path $root "_build\installer-release-gate"
$makensis = Join-Path $root "third_party\nsis-3.11\makensis.exe"
$installer = Join-Path $setupDirectory "Hibiki-EQAPO-x64-$version.exe"
$reportPath = Join-Path $outputDirectory "results.json"

# Installation mutates machine-wide APO registration. Only disposable GitHub
# hosted Windows runners may execute this gate; local use only compiles probes.
if (!$CompileOnly) {
	if ($env:GITHUB_ACTIONS -ne "true" -or $env:RUNNER_ENVIRONMENT -ne "github-hosted" -or
		$env:RUNNER_OS -ne "Windows") {
		throw "Installer execution is restricted to disposable GitHub-hosted Windows runners."
	}
	$principal = [Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
	if (!$principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
		throw "The disposable runner must have administrator rights."
	}
	if (Test-Path -LiteralPath "HKLM:\Software\EqualizerAPO") {
		throw "The disposable runner already has an Equalizer APO installation; refusing to modify it."
	}
}

New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null
$body = [IO.File]::ReadAllText((Join-Path $setupDirectory "Setup.nsi")).Replace("`r`n", "`n")
$entry = [IO.File]::ReadAllText((Join-Path $setupDirectory "Setup64.nsi")).Replace("`r`n", "`n")
$utf8 = [Text.UTF8Encoding]::new($false)
$markerKey = "Software\HibikiEQAPOInstallerGate"
$variants = @{}

function Replace-Unique([string]$Source, [string]$Needle, [string]$Replacement) {
	if ([regex]::Matches($Source, [regex]::Escape($Needle)).Count -ne 1) {
		throw "The installer probe insertion point is not unique: $Needle"
	}
	return $Source.Replace($Needle, $Replacement)
}

# Fault injection lives only in private copies compiled here, never in the
# shipped installer. Exercise rollback after extraction, and recovery after an
# actual process termination with a durable active journal.
foreach ($mode in @("rollback", "interrupted", "recover")) {
	$probeBody = $body
	if ($mode -eq "rollback") {
		$probeBody = Replace-Unique $probeBody '  Call VerifyRequiredAssets' `
			('  Delete "$INSTDIR\Editor.exe"' + "`n" + '  Call VerifyRequiredAssets')
	}
	elseif ($mode -eq "interrupted") {
		$needle = '  File "${BINPATH_EDITOR}\Editor.exe"'
		$probeBody = Replace-Unique $probeBody $needle `
			($needle + "`n" + '  WriteRegDWORD HKLM "' + $markerKey + '" "Ready" 1' + "`n  Sleep 120000")
	}
	else {
		$needle = '  !insertmacro MUI_LANGDLL_DISPLAY'
		$probeBody = Replace-Unique $probeBody $needle ("  SetErrorLevel 0`n  Quit`n" + $needle)
	}
	$id = [Guid]::NewGuid().ToString("N")
	$bodyName = "release-gate-$id-body.nsi"
	$bodyPath = Join-Path $setupDirectory $bodyName
	$entryPath = Join-Path $setupDirectory "release-gate-$id.nsi"
	$output = Join-Path $outputDirectory "$mode.exe"
	$probeEntry = Replace-Unique $entry '!include "Setup.nsi"' ('!include "' + $bodyName + '"')
	$probeEntry = Replace-Unique $probeEntry 'OutFile "Hibiki-EQAPO-x64-${VERSION}.exe"' ('OutFile "' + $output + '"')
	try {
		[IO.File]::WriteAllText($bodyPath, $probeBody, $utf8)
		[IO.File]::WriteAllText($entryPath, $probeEntry, $utf8)
		Push-Location $setupDirectory
		try {
			& $makensis /WX /INPUTCHARSET UTF8 /DENABLE_ASIO_PROXY=0 $entryPath
			if ($LASTEXITCODE -ne 0) { throw "Could not compile the $mode installer probe." }
		}
		finally { Pop-Location }
		$variants[$mode] = $output
	}
	finally {
		foreach ($path in @($bodyPath, $entryPath)) {
			if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path -Force }
		}
	}
}

if ($CompileOnly) {
	Write-Host "Installer release gate probes compiled; no installation was executed."
	return
}

function Invoke-Installer([string]$Path, [string]$Arguments, [bool]$ExpectSuccess = $true) {
	$process = Start-Process -FilePath $Path -ArgumentList $Arguments -PassThru -WindowStyle Hidden
	if (!$process.WaitForExit(180000)) {
		Stop-Process -Id $process.Id -Force
		throw "Installer operation timed out: $Path"
	}
	$process.Refresh()
	if ($ExpectSuccess -and $process.ExitCode -ne 0) { throw "Installer failed: $Path ($($process.ExitCode))" }
	if (!$ExpectSuccess -and $process.ExitCode -eq 0) { throw "Fault-injected installer unexpectedly succeeded." }
}

function Assert-NoRecoveryJournal {
	foreach ($key in @("InstallerAppRecovery", "InstallerRecovery")) {
		if (Test-Path -LiteralPath "HKLM:\Software\EqualizerAPO\$key") {
			throw "A recovery journal remains: $key"
		}
	}
	if (Test-Path -LiteralPath "HKLM:\Software\EqualizerAPOUninstallRecovery") {
		throw "An uninstall recovery journal remains."
	}
}

function Get-Hash([string]$Path) { return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash }

$installDirectory = Join-Path $env:ProgramFiles ("Hibiki-EQAPO-release-gate-" + [Guid]::NewGuid().ToString("N"))
$previousDirectory = Join-Path $outputDirectory "previous"
New-Item -ItemType Directory -Force -Path $previousDirectory | Out-Null
& gh release download v3.1.7 --repo xup61069/Hibiki-EQAPO `
	--pattern "Hibiki-EQAPO-x64-3.1.7.exe*" --dir $previousDirectory --clobber
if ($LASTEXITCODE -ne 0) { throw "Could not download the previous published installer." }
$previousInstaller = Join-Path $previousDirectory "Hibiki-EQAPO-x64-3.1.7.exe"
foreach ($path in @($previousInstaller, $installer)) {
	$line = [IO.File]::ReadAllText("$path.sha256").Trim()
	if ($line -notmatch '^([0-9a-fA-F]{64})  (.+)$') { throw "Invalid installer checksum file: $path" }
	if ($Matches[2] -ne [IO.Path]::GetFileName($path) -or (Get-Hash $path) -ne $Matches[1]) {
		throw "Installer checksum mismatch: $path"
	}
}

# Hosted Windows Server images can disable audio services by default. Enable
# them only inside this disposable runner to exercise the installer's restart.
foreach ($serviceName in @("AudioEndpointBuilder", "AudioSrv")) {
	Set-Service -Name $serviceName -StartupType Manual
	Start-Service -Name $serviceName
}
Invoke-Installer $previousInstaller "/S /NOASIOPROXY /D=$installDirectory"
Assert-NoRecoveryJournal
$userConfig = Join-Path $installDirectory "config\release-gate-user.txt"
$userPlugin = Join-Path $installDirectory "VSTPlugins\release-gate-user.bin"
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $userPlugin) | Out-Null
[IO.File]::WriteAllText($userConfig, "Preamp: -7 dB`r`n", $utf8)
[IO.File]::WriteAllBytes($userPlugin, [byte[]]@(1, 3, 7, 255))
$userConfigHash = Get-Hash $userConfig
$userPluginHash = Get-Hash $userPlugin
$coreFiles = @("Editor.exe", "EqualizerAPO.dll", "DeviceSelector.exe", "UpdateChecker.exe", "Uninstall.exe")
$oldHashes = @{}
foreach ($name in $coreFiles) { $oldHashes[$name] = Get-Hash (Join-Path $installDirectory $name) }
$oldProxyHash = Get-Hash (Join-Path $installDirectory "HibikiEQAPODriver.dll")

function Assert-UserData {
	if ((Get-Hash $userConfig) -ne $userConfigHash -or (Get-Hash $userPlugin) -ne $userPluginHash) {
		throw "The installer changed user configuration or plug-in data."
	}
}

function Assert-PreviousInstall {
	foreach ($name in $coreFiles) {
		if ((Get-Hash (Join-Path $installDirectory $name)) -ne $oldHashes[$name]) {
			throw "Rollback did not restore the previous $name."
		}
	}
	$metadata = Get-ItemProperty -LiteralPath "HKLM:\Software\Microsoft\Windows\CurrentVersion\Uninstall\EqualizerAPO"
	if ($metadata.DisplayVersion -ne "3.1.7") { throw "Rollback did not restore the previous version metadata." }
	Assert-UserData
	Assert-NoRecoveryJournal
}

Invoke-Installer $variants.rollback "/S /D=$installDirectory" $false
Assert-PreviousInstall
Write-Host "PASS: extraction failure rolled back to the previous published installation."

$interruptedProcess = Start-Process -FilePath $variants.interrupted `
	-ArgumentList "/S /D=$installDirectory" -PassThru -WindowStyle Hidden
try {
	$deadline = [DateTime]::UtcNow.AddSeconds(180)
	while (!(Test-Path -LiteralPath "HKLM:\$markerKey")) {
		if ($interruptedProcess.HasExited -or [DateTime]::UtcNow -gt $deadline) {
			throw "The interrupted installer did not reach the extraction checkpoint."
		}
		Start-Sleep -Milliseconds 100
	}
	$journal = Get-ItemProperty -LiteralPath "HKLM:\Software\EqualizerAPO\InstallerAppRecovery"
	if ($journal.Pending -ne 1 -or $journal.Phase -ne "active") { throw "No durable active journal at interruption." }
	Stop-Process -Id $interruptedProcess.Id -Force
	$interruptedProcess.WaitForExit()
}
finally {
	if (!$interruptedProcess.HasExited) { Stop-Process -Id $interruptedProcess.Id -Force }
	if (Test-Path -LiteralPath "HKLM:\$markerKey") { Remove-Item -LiteralPath "HKLM:\$markerKey" }
}
Invoke-Installer $variants.recover "/S /D=$installDirectory"
Assert-PreviousInstall
Write-Host "PASS: the next installer recovered a process-terminated upgrade."

Invoke-Installer $installer "/S /D=$installDirectory"
Assert-UserData
Assert-NoRecoveryJournal
$metadata = Get-ItemProperty -LiteralPath "HKLM:\Software\Microsoft\Windows\CurrentVersion\Uninstall\EqualizerAPO"
if ($metadata.DisplayVersion -ne $version) { throw "Upgrade did not publish the release version metadata." }
if ((Get-Hash (Join-Path $installDirectory "Editor.exe")) -ne (Get-Hash (Join-Path $root "x64\Release\Editor.exe"))) {
	throw "The upgraded Editor does not match the release build."
}
if ((Get-Hash (Join-Path $installDirectory "NOTICE.md")) -ne (Get-Hash (Join-Path $root "NOTICE.md"))) {
	throw "The installer did not include the release NOTICE.md."
}
if ((Get-Hash (Join-Path $installDirectory "HibikiEQAPODriver.dll")) -ne $oldProxyHash) {
	throw "The official installer changed an existing experimental proxy."
}
Write-Host "PASS: normal in-place upgrade preserved user data and the existing experimental proxy."

Invoke-Installer (Join-Path $installDirectory "Uninstall.exe") "/S _?=$installDirectory"
Assert-UserData
Assert-NoRecoveryJournal
foreach ($name in @("Editor.exe", "EqualizerAPO.dll", "DeviceSelector.exe", "UpdateChecker.exe")) {
	if (Test-Path -LiteralPath (Join-Path $installDirectory $name)) { throw "Uninstall left $name installed." }
}
if (Test-Path -LiteralPath "HKLM:\Software\Microsoft\Windows\CurrentVersion\Uninstall\EqualizerAPO") {
	throw "Uninstall left the product enumeration key."
}
# The previous test installer deliberately excluded registration. Remove only
# its exact, hash-verified proxy payload so the next install checks fresh output.
$oldProxyPath = Join-Path $installDirectory "HibikiEQAPODriver.dll"
if ((Get-Hash $oldProxyPath) -ne $oldProxyHash) { throw "Unexpected previous proxy payload." }
Remove-Item -LiteralPath $oldProxyPath -Force
Invoke-Installer $installer "/S /D=$installDirectory"
if (Test-Path -LiteralPath $oldProxyPath) { throw "The official installer contains the excluded proxy." }
if (Test-Path -LiteralPath "HKLM:\Software\ASIO\Hibiki EQAPO") { throw "The official installer registered the excluded proxy." }
Invoke-Installer (Join-Path $installDirectory "Uninstall.exe") "/S _?=$installDirectory"
Assert-UserData
Assert-NoRecoveryJournal
Write-Host "PASS: fresh official install excludes the proxy; uninstall preserves user data."

$report = [ordered]@{
	version = $version
	previousVersion = "3.1.7"
	runner = $env:RUNNER_OS
	installerSha256 = (Get-Hash $installer).ToLowerInvariant()
	rollback = "passed"
	interruptedUpgradeRecovery = "passed"
	inPlaceUpgrade = "passed"
	freshInstall = "passed"
	uninstall = "passed"
	userDataPreserved = $true
	proxyExcluded = $true
}
[IO.File]::WriteAllText($reportPath, ($report | ConvertTo-Json) + "`n", $utf8)
Write-Host "Installer release gate passed: $reportPath"
