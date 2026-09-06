param(
	[Parameter(Mandatory = $true)]
	[ValidateSet("Install", "Uninstall")]
	[string]$Action,
	[ValidateSet("Release")]
	[string]$Configuration = "Release",
	[string]$SourceBundle = "",
	[string]$DestinationRoot = ""
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

Import-Module (Join-Path $PSScriptRoot "MonitorVST3Binary.psm1") -Force

$repoRoot = Split-Path -Parent $PSScriptRoot
$bundleName = "HibikiEQAPOMonitor.vst3"
$architecture = "x86_64-win"
$manifestName = "hibiki-eqapo-monitor.json"
$requiredPayloadNames = @(Get-MonitorVST3RequiredPayloadNames)
$allPayloadNames = @($requiredPayloadNames)

if (![Environment]::Is64BitProcess) {
	throw "The x64 Monitor VST3 manager must run in a 64-bit PowerShell process."
}

function Get-CanonicalPath {
	param([Parameter(Mandatory = $true)][string]$Path)

	if ([string]::IsNullOrWhiteSpace($Path)) {
		throw "Path must not be empty."
	}
	$fullPath = [System.IO.Path]::GetFullPath($Path)
	$pathRoot = [System.IO.Path]::GetPathRoot($fullPath)
	if ($fullPath.Equals($pathRoot, [StringComparison]::OrdinalIgnoreCase)) {
		return $pathRoot
	}
	return $fullPath.TrimEnd('\')
}

function Assert-DirectoryNoReparse {
	param(
		[Parameter(Mandatory = $true)][string]$Path,
		[Parameter(Mandatory = $true)][string]$Description
	)

	if (!(Test-Path -LiteralPath $Path -PathType Container)) {
		throw "$Description not found: $Path"
	}
	$item = Get-Item -LiteralPath $Path -Force
	if (($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
		throw "$Description must not be a reparse point: $Path"
	}
}

function Assert-RegularNonEmptyFile {
	param(
		[Parameter(Mandatory = $true)][string]$Path,
		[Parameter(Mandatory = $true)][string]$Description
	)

	if (!(Test-Path -LiteralPath $Path -PathType Leaf)) {
		throw "$Description not found: $Path"
	}
	$item = Get-Item -LiteralPath $Path -Force
	if (($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
		throw "$Description must not be a reparse point: $Path"
	}
	if ($item.Length -le 0) {
		throw "$Description is empty: $Path"
	}
}

function Assert-ExistingDirectoryChainNoReparse {
	param(
		[Parameter(Mandatory = $true)][string]$Path,
		[Parameter(Mandatory = $true)][string]$Description
	)

	$current = Get-CanonicalPath $Path
	while (![string]::IsNullOrWhiteSpace($current)) {
		if (Test-Path -LiteralPath $current) {
			if (!(Test-Path -LiteralPath $current -PathType Container)) {
				throw "$Description contains a non-directory path component: $current"
			}
			Assert-DirectoryNoReparse -Path $current -Description $Description
		}
		$parent = [System.IO.Directory]::GetParent($current)
		if ($null -eq $parent -or
			$parent.FullName.Equals($current, [StringComparison]::OrdinalIgnoreCase)) {
			break
		}
		$current = $parent.FullName
	}
}

function Assert-ExactDirectoryMembers {
	param(
		[Parameter(Mandatory = $true)][string]$Path,
		[string[]]$FileNames = @(),
		[string[]]$DirectoryNames = @(),
		[Parameter(Mandatory = $true)][string]$Description
	)

	Assert-DirectoryNoReparse -Path $Path -Description $Description
	$expectedFiles = New-Object 'System.Collections.Generic.HashSet[string]' `
		([StringComparer]::OrdinalIgnoreCase)
	$expectedDirectories = New-Object 'System.Collections.Generic.HashSet[string]' `
		([StringComparer]::OrdinalIgnoreCase)
	foreach ($name in $FileNames) {
		[void]$expectedFiles.Add($name)
	}
	foreach ($name in $DirectoryNames) {
		[void]$expectedDirectories.Add($name)
	}

	$seenFiles = New-Object 'System.Collections.Generic.HashSet[string]' `
		([StringComparer]::OrdinalIgnoreCase)
	$seenDirectories = New-Object 'System.Collections.Generic.HashSet[string]' `
		([StringComparer]::OrdinalIgnoreCase)
	foreach ($item in @(Get-ChildItem -LiteralPath $Path -Force)) {
		if (($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
			throw "$Description contains a reparse point: $($item.FullName)"
		}
		if ($item.PSIsContainer) {
			if (!$expectedDirectories.Contains($item.Name)) {
				throw "$Description contains an unexpected directory: $($item.FullName)"
			}
			[void]$seenDirectories.Add($item.Name)
		} else {
			if (!$expectedFiles.Contains($item.Name)) {
				throw "$Description contains an unexpected file: $($item.FullName)"
			}
			[void]$seenFiles.Add($item.Name)
		}
	}

	foreach ($name in $FileNames) {
		if (!$seenFiles.Contains($name)) {
			throw "$Description is missing required file '$name'."
		}
	}
	foreach ($name in $DirectoryNames) {
		if (!$seenDirectories.Contains($name)) {
			throw "$Description is missing required directory '$name'."
		}
	}
}

function Get-PayloadDirectory {
	param([Parameter(Mandatory = $true)][string]$BundlePath)

	return Join-Path $BundlePath "Contents\$architecture"
}

function Get-PresentSourcePayloadNames {
	param([Parameter(Mandatory = $true)][string]$BundlePath)

	$bundlePath = Get-CanonicalPath $BundlePath
	Assert-ExistingDirectoryChainNoReparse -Path $bundlePath -Description "Source bundle path"
	Assert-ExactDirectoryMembers `
		-Path $bundlePath `
		-DirectoryNames @("Contents") `
		-Description "Source bundle root"
	$contentsPath = Join-Path $bundlePath "Contents"
	Assert-ExactDirectoryMembers `
		-Path $contentsPath `
		-DirectoryNames @($architecture) `
		-Description "Source bundle Contents"
	$payloadPath = Get-PayloadDirectory $bundlePath

	$presentNames = @()
	foreach ($item in @(Get-ChildItem -LiteralPath $payloadPath -Force)) {
		if ($item.PSIsContainer -or
			($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0 -or
			$allPayloadNames -notcontains $item.Name) {
			throw "Source bundle contains an unexpected payload entry: $($item.FullName)"
		}
		Assert-RegularNonEmptyFile -Path $item.FullName -Description "Source payload file"
		$presentNames += $item.Name
	}
	foreach ($name in $requiredPayloadNames) {
		if ($presentNames -notcontains $name) {
			throw "Source bundle is missing required payload file '$name'."
		}
	}
	return $presentNames
}

function Write-BundleManifest {
	param(
		[Parameter(Mandatory = $true)][string]$BundlePath,
		[Parameter(Mandatory = $true)][string[]]$PayloadNames
	)

	$resourcesPath = Join-Path $BundlePath "Contents\Resources"
	[void][System.IO.Directory]::CreateDirectory($resourcesPath)
	Assert-DirectoryNoReparse -Path $resourcesPath -Description "Bundle Resources directory"
	$payloadPath = Get-PayloadDirectory $BundlePath
	$records = @()
	foreach ($name in $PayloadNames) {
		$filePath = Join-Path $payloadPath $name
		Assert-RegularNonEmptyFile -Path $filePath -Description "Staged payload file"
		$item = Get-Item -LiteralPath $filePath -Force
		$records += [ordered]@{
			path = "Contents/$architecture/$name"
			length = [long]$item.Length
			sha256 = (Get-FileHash -LiteralPath $filePath -Algorithm SHA256).Hash.ToLowerInvariant()
		}
	}
	$manifest = [ordered]@{
		schema = 1
		product = "HibikiEQAPOMonitor"
		architecture = $architecture
		files = $records
	}
	$json = $manifest | ConvertTo-Json -Depth 4
	$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
	[System.IO.File]::WriteAllText(
		(Join-Path $resourcesPath $manifestName),
		$json + [Environment]::NewLine,
		$utf8NoBom
	)
}

function Assert-OwnedBundle {
	param([Parameter(Mandatory = $true)][string]$BundlePath)

	$bundlePath = Get-CanonicalPath $BundlePath
	Assert-ExistingDirectoryChainNoReparse -Path $bundlePath -Description "Installed bundle path"
	Assert-ExactDirectoryMembers `
		-Path $bundlePath `
		-DirectoryNames @("Contents") `
		-Description "Installed bundle root"
	$contentsPath = Join-Path $bundlePath "Contents"
	Assert-ExactDirectoryMembers `
		-Path $contentsPath `
		-DirectoryNames @($architecture, "Resources") `
		-Description "Installed bundle Contents"
	$resourcesPath = Join-Path $contentsPath "Resources"
	Assert-ExactDirectoryMembers `
		-Path $resourcesPath `
		-FileNames @($manifestName) `
		-Description "Installed bundle Resources"
	$manifestPath = Join-Path $resourcesPath $manifestName
	Assert-RegularNonEmptyFile -Path $manifestPath -Description "Installed bundle manifest"

	try {
		$manifest = Get-Content -LiteralPath $manifestPath -Raw -Encoding UTF8 |
			ConvertFrom-Json -ErrorAction Stop
	} catch {
		throw "Installed bundle manifest is invalid JSON: $($_.Exception.Message)"
	}
	if ($manifest.schema -ne 1 -or
		$manifest.product -cne "HibikiEQAPOMonitor" -or
		$manifest.architecture -cne $architecture) {
		throw "Installed bundle manifest identity is not owned by this manager."
	}

	$records = @($manifest.files)
	if ($records.Count -ne $requiredPayloadNames.Count) {
		throw "Installed bundle manifest has an unexpected payload count."
	}
	$seenNames = New-Object 'System.Collections.Generic.HashSet[string]' `
		([StringComparer]::OrdinalIgnoreCase)
	$manifestPayloadNames = @()
	$payloadPath = Get-PayloadDirectory $bundlePath
	$requiredPrefix = "Contents/$architecture/"
	foreach ($record in $records) {
		if ($null -eq $record -or
			$record.path -isnot [string] -or
			!$record.path.StartsWith($requiredPrefix, [StringComparison]::Ordinal)) {
			throw "Installed bundle manifest contains an invalid payload path."
		}
		$name = $record.path.Substring($requiredPrefix.Length)
		if ($name.Contains('/') -or $name.Contains('\') -or
			$allPayloadNames -notcontains $name -or !$seenNames.Add($name)) {
			throw "Installed bundle manifest contains an unexpected or duplicate file '$name'."
		}
		$filePath = Join-Path $payloadPath $name
		Assert-RegularNonEmptyFile -Path $filePath -Description "Installed payload file"
		$item = Get-Item -LiteralPath $filePath -Force
		$declaredLength = 0L
		if (![long]::TryParse(
			[string]$record.length,
			[Globalization.NumberStyles]::Integer,
			[Globalization.CultureInfo]::InvariantCulture,
			[ref]$declaredLength) -or
			$declaredLength -ne [long]$item.Length) {
			throw "Installed payload length does not match its manifest: $filePath"
		}
		$declaredHash = [string]$record.sha256
		if ($declaredHash -notmatch '^[0-9a-fA-F]{64}$') {
			throw "Installed payload hash is invalid in the manifest: $filePath"
		}
		$actualHash = (Get-FileHash -LiteralPath $filePath -Algorithm SHA256).Hash
		if (!$actualHash.Equals($declaredHash, [StringComparison]::OrdinalIgnoreCase)) {
			throw "Installed payload hash does not match its manifest: $filePath"
		}
		$manifestPayloadNames += $name
	}
	foreach ($name in $requiredPayloadNames) {
		if (!$seenNames.Contains($name)) {
			throw "Installed bundle manifest is missing required file '$name'."
		}
	}
	Assert-ExactDirectoryMembers `
		-Path $payloadPath `
		-FileNames $manifestPayloadNames `
		-Description "Installed bundle payload"
	return $manifest
}

function Assert-BundleFilesUnlocked {
	param([Parameter(Mandatory = $true)][string]$BundlePath)

	$manifest = Assert-OwnedBundle $BundlePath
	foreach ($record in @($manifest.files)) {
		$name = [string]$record.path
		$filePath = Join-Path $BundlePath ($name.Replace('/', '\'))
		try {
			$stream = [System.IO.File]::Open(
				$filePath,
				[System.IO.FileMode]::Open,
				[System.IO.FileAccess]::ReadWrite,
				[System.IO.FileShare]::None
			)
			$stream.Dispose()
		} catch {
			throw "Monitor VST3 is in use or cannot be updated. Close every DAW and try again: $filePath"
		}
	}
}

function Remove-ConstructedBundleExact {
	param([Parameter(Mandatory = $true)][string]$BundlePath)

	if (!(Test-Path -LiteralPath $BundlePath)) {
		return
	}
	Assert-DirectoryNoReparse -Path $BundlePath -Description "Owned transaction bundle"
	$payloadPath = Get-PayloadDirectory $BundlePath
	foreach ($name in $allPayloadNames) {
		$filePath = Join-Path $payloadPath $name
		if (Test-Path -LiteralPath $filePath -PathType Leaf) {
			Assert-RegularNonEmptyFile -Path $filePath -Description "Owned transaction payload"
			Remove-Item -LiteralPath $filePath -Force
		} elseif (Test-Path -LiteralPath $filePath) {
			throw "Refusing to remove unexpected transaction entry: $filePath"
		}
	}
	$manifestPath = Join-Path $BundlePath "Contents\Resources\$manifestName"
	if (Test-Path -LiteralPath $manifestPath -PathType Leaf) {
		Assert-RegularNonEmptyFile -Path $manifestPath -Description "Owned transaction manifest"
		Remove-Item -LiteralPath $manifestPath -Force
	} elseif (Test-Path -LiteralPath $manifestPath) {
		throw "Refusing to remove unexpected transaction manifest entry: $manifestPath"
	}

	foreach ($directory in @(
		(Join-Path $BundlePath "Contents\Resources"),
		$payloadPath,
		(Join-Path $BundlePath "Contents"),
		$BundlePath
	)) {
		if (Test-Path -LiteralPath $directory -PathType Container) {
			Assert-DirectoryNoReparse -Path $directory -Description "Owned transaction directory"
			if (@(Get-ChildItem -LiteralPath $directory -Force).Count -ne 0) {
				throw "Refusing to remove non-empty transaction directory: $directory"
			}
			Remove-Item -LiteralPath $directory -Force
		} elseif (Test-Path -LiteralPath $directory) {
			throw "Refusing to remove non-directory transaction path: $directory"
		}
	}
}

function Remove-OwnedBundleExact {
	param([Parameter(Mandatory = $true)][string]$BundlePath)

	$null = Assert-OwnedBundle $BundlePath
	Remove-ConstructedBundleExact $BundlePath
}

function New-OwnedBundle {
	param(
		[Parameter(Mandatory = $true)][string]$SourcePath,
		[Parameter(Mandatory = $true)][string]$BundlePath,
		[Parameter(Mandatory = $true)][string[]]$PayloadNames
	)

	if (Test-Path -LiteralPath $BundlePath) {
		throw "Transaction staging path already exists: $BundlePath"
	}
	$sourcePayload = Get-PayloadDirectory $SourcePath
	$destinationPayload = Get-PayloadDirectory $BundlePath
	[void][System.IO.Directory]::CreateDirectory($destinationPayload)
	Assert-DirectoryNoReparse -Path $BundlePath -Description "Transaction bundle root"
	Assert-DirectoryNoReparse `
		-Path (Join-Path $BundlePath "Contents") `
		-Description "Transaction bundle Contents"
	Assert-DirectoryNoReparse -Path $destinationPayload -Description "Transaction payload"
	foreach ($name in $PayloadNames) {
		Copy-Item `
			-LiteralPath (Join-Path $sourcePayload $name) `
			-Destination (Join-Path $destinationPayload $name)
	}
	$null = Assert-MonitorVST3PayloadBinaries -PayloadPath $destinationPayload
	Write-BundleManifest -BundlePath $BundlePath -PayloadNames $PayloadNames
	$null = Assert-OwnedBundle $BundlePath
}

function Complete-InterruptedTransactions {
	param(
		[Parameter(Mandatory = $true)][string]$DestinationBundle,
		[Parameter(Mandatory = $true)][string]$NewBundle,
		[Parameter(Mandatory = $true)][string]$BackupBundle,
		[Parameter(Mandatory = $true)][string]$FailedBundle,
		[Parameter(Mandatory = $true)][string]$RemovedBundle
	)

	# Directory renames are the commit markers.  The fixed sibling names let a
	# later invocation recover after process termination without enumerating or
	# deleting any unrelated path.
	if (!(Test-Path -LiteralPath $DestinationBundle) -and
		(Test-Path -LiteralPath $BackupBundle -PathType Container)) {
		$null = Assert-OwnedBundle $BackupBundle
		Move-Item -LiteralPath $BackupBundle -Destination $DestinationBundle -ErrorAction Stop
	}

	if (Test-Path -LiteralPath $NewBundle -PathType Container) {
		$null = Assert-OwnedBundle $NewBundle
		Remove-OwnedBundleExact $NewBundle
	} elseif (Test-Path -LiteralPath $NewBundle) {
		throw "Refusing an unexpected Monitor VST3 transaction path: $NewBundle"
	}

	if ((Test-Path -LiteralPath $DestinationBundle -PathType Container) -and
		(Test-Path -LiteralPath $BackupBundle -PathType Container)) {
		$null = Assert-OwnedBundle $DestinationBundle
		$null = Assert-OwnedBundle $BackupBundle
		Remove-OwnedBundleExact $BackupBundle
	} elseif (Test-Path -LiteralPath $BackupBundle) {
		throw "Refusing an unexpected Monitor VST3 backup path: $BackupBundle"
	}

	foreach ($transactionPath in @($FailedBundle, $RemovedBundle)) {
		if (Test-Path -LiteralPath $transactionPath -PathType Container) {
			try {
				Remove-OwnedBundleExact $transactionPath
			} catch {
				throw "A prior Monitor VST3 operation committed but exact cleanup is incomplete. Preserve this path for manual recovery: $transactionPath. $($_.Exception.Message)"
			}
		} elseif (Test-Path -LiteralPath $transactionPath) {
			throw "Refusing an unexpected Monitor VST3 transaction path: $transactionPath"
		}
	}
}

if ([string]::IsNullOrWhiteSpace($SourceBundle)) {
	$SourceBundle = Join-Path $repoRoot (
		"build\VST3\$Configuration\HibikiEQAPO\$bundleName"
	)
}
if ([string]::IsNullOrWhiteSpace($DestinationRoot)) {
	$commonProgramFiles = [Environment]::GetFolderPath(
		[Environment+SpecialFolder]::CommonProgramFiles
	)
	if ([string]::IsNullOrWhiteSpace($commonProgramFiles)) {
		throw "Windows Common Program Files directory could not be resolved."
	}
	$DestinationRoot = Join-Path $commonProgramFiles "VST3\HibikiEQAPO"
}

$sourceBundlePath = Get-CanonicalPath $SourceBundle
$destinationRootPath = Get-CanonicalPath $DestinationRoot
$destinationBundle = Join-Path $destinationRootPath $bundleName
$newBundle = "$destinationBundle.new"
$backupBundle = "$destinationBundle.backup"
$failedBundle = "$destinationBundle.failed"
$removedBundle = "$destinationBundle.removed"

$managerMutex = [System.Threading.Mutex]::new(
	$false,
	"Global\HibikiEQAPOMonitorVST3Manager"
)
$ownsManagerMutex = $false
try {
	try {
		$ownsManagerMutex = $managerMutex.WaitOne(0, $false)
	} catch [System.Threading.AbandonedMutexException] {
		$ownsManagerMutex = $true
	}
	if (!$ownsManagerMutex) {
		throw "Another Monitor VST3 install or removal is already running."
	}

	if ($Action -eq "Install") {
		$payloadNames = @(Get-PresentSourcePayloadNames $sourceBundlePath)
		$null = Assert-MonitorVST3PayloadBinaries `
			-PayloadPath (Get-PayloadDirectory $sourceBundlePath)
		$destinationParent = [System.IO.Directory]::GetParent($destinationRootPath)
		if ($null -eq $destinationParent) {
			throw "DestinationRoot must not be a filesystem root: $destinationRootPath"
		}
		Assert-ExistingDirectoryChainNoReparse `
			-Path $destinationParent.FullName `
			-Description "Destination parent path"
		[void][System.IO.Directory]::CreateDirectory($destinationRootPath)
		Assert-ExistingDirectoryChainNoReparse `
			-Path $destinationRootPath `
			-Description "Destination root"
	} elseif (!(Test-Path -LiteralPath $destinationRootPath)) {
		Write-Host "Monitor VST3 is not installed: $destinationBundle"
		return
	} else {
		Assert-ExistingDirectoryChainNoReparse `
			-Path $destinationRootPath `
			-Description "Destination root"
	}

	Complete-InterruptedTransactions `
		-DestinationBundle $destinationBundle `
		-NewBundle $newBundle `
		-BackupBundle $backupBundle `
		-FailedBundle $failedBundle `
		-RemovedBundle $removedBundle

	if ($Action -eq "Install") {
		$sourcePrefix = $sourceBundlePath + '\'
		$destinationPrefix = $destinationBundle + '\'
		if ($sourceBundlePath.Equals($destinationBundle, [StringComparison]::OrdinalIgnoreCase) -or
			$sourceBundlePath.StartsWith($destinationPrefix, [StringComparison]::OrdinalIgnoreCase) -or
			$destinationBundle.StartsWith($sourcePrefix, [StringComparison]::OrdinalIgnoreCase)) {
			throw "Source and destination bundle paths must not overlap."
		}

		$hadExistingBundle = Test-Path -LiteralPath $destinationBundle
		if ($hadExistingBundle) {
			$null = Assert-OwnedBundle $destinationBundle
			Assert-BundleFilesUnlocked $destinationBundle
		}

		try {
			New-OwnedBundle `
				-SourcePath $sourceBundlePath `
				-BundlePath $newBundle `
				-PayloadNames $payloadNames
			if ($hadExistingBundle) {
				Move-Item `
					-LiteralPath $destinationBundle `
					-Destination $backupBundle `
					-ErrorAction Stop
			}
			Move-Item `
				-LiteralPath $newBundle `
				-Destination $destinationBundle `
				-ErrorAction Stop

			try {
				$null = Assert-OwnedBundle $destinationBundle
				$null = Assert-MonitorVST3PayloadBinaries `
					-PayloadPath (Get-PayloadDirectory $destinationBundle)
			} catch {
				$validationFailure = $_
				if (Test-Path -LiteralPath $failedBundle) {
					throw "Cannot quarantine the failed new bundle because the fixed recovery path already exists: $failedBundle"
				}
				Move-Item `
					-LiteralPath $destinationBundle `
					-Destination $failedBundle `
					-ErrorAction Stop
				if ($hadExistingBundle -and
					(Test-Path -LiteralPath $backupBundle -PathType Container)) {
					Move-Item `
						-LiteralPath $backupBundle `
						-Destination $destinationBundle `
						-ErrorAction Stop
				}
				throw $validationFailure
			}
		} catch {
			$failure = $_
			if (Test-Path -LiteralPath $newBundle -PathType Container) {
				try {
					Remove-ConstructedBundleExact $newBundle
				} catch {
					Write-Warning "Staged Monitor VST3 cleanup is incomplete: $newBundle. $($_.Exception.Message)"
				}
			}
			if ($hadExistingBundle -and
				!(Test-Path -LiteralPath $destinationBundle) -and
				(Test-Path -LiteralPath $backupBundle -PathType Container)) {
				Move-Item `
					-LiteralPath $backupBundle `
					-Destination $destinationBundle `
					-ErrorAction Stop
			}
			throw $failure
		}

		if (Test-Path -LiteralPath $backupBundle -PathType Container) {
			try {
				Remove-OwnedBundleExact $backupBundle
			} catch {
				Write-Warning "The new Monitor VST3 is installed and verified, but backup cleanup is incomplete: $backupBundle. $($_.Exception.Message)"
			}
		}
		Write-Host "Monitor VST3 installed: $destinationBundle"
	} else {
		if (!(Test-Path -LiteralPath $destinationBundle)) {
			Write-Host "Monitor VST3 is not installed: $destinationBundle"
			return
		}
		$null = Assert-OwnedBundle $destinationBundle
		Assert-BundleFilesUnlocked $destinationBundle
		Move-Item `
			-LiteralPath $destinationBundle `
			-Destination $removedBundle `
			-ErrorAction Stop
		try {
			Remove-OwnedBundleExact $removedBundle
		} catch {
			throw "Monitor VST3 removal is committed, but exact tombstone cleanup is incomplete. The inactive recovery path is: $removedBundle. $($_.Exception.Message)"
		}
		Write-Host "Monitor VST3 uninstalled: $destinationBundle"
	}
} finally {
	if ($ownsManagerMutex) {
		[void]$managerMutex.ReleaseMutex()
	}
	$managerMutex.Dispose()
}
