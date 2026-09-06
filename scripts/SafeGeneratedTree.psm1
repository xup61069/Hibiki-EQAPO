Set-StrictMode -Version Latest

function Get-NormalizedFullPath {
	param([Parameter(Mandatory = $true)][string]$Path)

	if ([string]::IsNullOrWhiteSpace($Path)) {
		throw "Path must not be empty."
	}
	$fullPath = [System.IO.Path]::GetFullPath($Path)
	$pathRoot = [System.IO.Path]::GetPathRoot($fullPath)
	if ($fullPath.Equals($pathRoot, [StringComparison]::OrdinalIgnoreCase)) {
		return $pathRoot
	}
	return $fullPath.TrimEnd([char[]]@('\', '/'))
}

function Get-ContainedFullPath {
	param(
		[Parameter(Mandatory = $true)][string]$AllowedRoot,
		[Parameter(Mandatory = $true)][string]$Path,
		[switch]$AllowEqual
	)

	$rootPath = Get-NormalizedFullPath $AllowedRoot
	$fullPath = Get-NormalizedFullPath $Path
	if ($fullPath.Equals($rootPath, [StringComparison]::OrdinalIgnoreCase)) {
		if (!$AllowEqual) {
			throw "Generated path must be below its allowed root: $fullPath"
		}
		return $fullPath
	}
	$rootPrefix = $rootPath
	if (!$rootPrefix.EndsWith('\') -and !$rootPrefix.EndsWith('/')) {
		$rootPrefix += [System.IO.Path]::DirectorySeparatorChar
	}
	if (!$fullPath.StartsWith($rootPrefix, [StringComparison]::OrdinalIgnoreCase)) {
		throw "Path escapes its allowed root: $fullPath"
	}
	return $fullPath
}

function Assert-DirectoryChainNoReparse {
	param(
		[Parameter(Mandatory = $true)][string]$Path,
		[Parameter(Mandatory = $true)][string]$Description
	)

	$current = Get-NormalizedFullPath $Path
	while (![string]::IsNullOrWhiteSpace($current)) {
		if (Test-Path -LiteralPath $current -ErrorAction Stop) {
			$item = Get-Item -LiteralPath $current -Force -ErrorAction Stop
			if (!$item.PSIsContainer) {
				throw "$Description contains a non-directory path component: $current"
			}
			if (($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
				throw "$Description contains a reparse point: $current"
			}
		}
		$parent = [System.IO.Directory]::GetParent($current)
		if ($null -eq $parent -or
			$parent.FullName.Equals($current, [StringComparison]::OrdinalIgnoreCase)) {
			break
		}
		$current = $parent.FullName
	}
}

function Assert-DirectoryNoReparse {
	param(
		[Parameter(Mandatory = $true)][string]$Path,
		[Parameter(Mandatory = $true)][string]$Description
	)

	if (!(Test-Path -LiteralPath $Path -PathType Container -ErrorAction Stop)) {
		throw "$Description not found: $Path"
	}
	$item = Get-Item -LiteralPath $Path -Force -ErrorAction Stop
	if (($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
		throw "$Description must not be a reparse point: $Path"
	}
}

function Assert-SafeDirectoryChainNoReparse {
	[CmdletBinding()]
	param(
		[Parameter(Mandatory = $true)][string]$AllowedRoot,
		[Parameter(Mandatory = $true)][string]$Path,
		[Parameter(Mandatory = $true)][string]$Description
	)

	$allowedRootPath = Get-NormalizedFullPath $AllowedRoot
	$containedPath = Get-ContainedFullPath `
		-AllowedRoot $allowedRootPath `
		-Path $Path `
		-AllowEqual
	Assert-DirectoryChainNoReparse `
		-Path $allowedRootPath `
		-Description "Allowed root path"
	Assert-DirectoryNoReparse -Path $allowedRootPath -Description "Allowed root"
	Assert-DirectoryChainNoReparse -Path $containedPath -Description $Description
}

function Get-SafeGeneratedTreeInventory {
	param(
		[Parameter(Mandatory = $true)][string]$AllowedRoot,
		[Parameter(Mandatory = $true)][string]$GeneratedPath
	)

	$allowedRootPath = Get-NormalizedFullPath $AllowedRoot
	$generatedRootPath = Get-ContainedFullPath `
		-AllowedRoot $allowedRootPath `
		-Path $GeneratedPath
	Assert-DirectoryChainNoReparse `
		-Path $allowedRootPath `
		-Description "Allowed root path"
	Assert-DirectoryNoReparse -Path $allowedRootPath -Description "Allowed root"
	Assert-DirectoryChainNoReparse `
		-Path $generatedRootPath `
		-Description "Generated tree path"

	$directories = New-Object 'System.Collections.Generic.List[string]'
	$files = New-Object 'System.Collections.Generic.List[string]'
	if (!(Test-Path -LiteralPath $generatedRootPath -ErrorAction Stop)) {
		return [pscustomobject]@{
			AllowedRoot = $allowedRootPath
			GeneratedRoot = $generatedRootPath
			Directories = $directories.ToArray()
			Files = $files.ToArray()
		}
	}
	Assert-DirectoryNoReparse -Path $generatedRootPath -Description "Generated tree"

	$pending = New-Object 'System.Collections.Generic.Stack[string]'
	$pending.Push($generatedRootPath)
	while ($pending.Count -ne 0) {
		$directoryPath = Get-ContainedFullPath `
			-AllowedRoot $generatedRootPath `
			-Path $pending.Pop() `
			-AllowEqual
		Assert-DirectoryChainNoReparse `
			-Path $directoryPath `
			-Description "Generated tree directory"
		Assert-DirectoryNoReparse `
			-Path $directoryPath `
			-Description "Generated tree directory"
		$directories.Add($directoryPath)

		foreach ($entry in @(Get-ChildItem -LiteralPath $directoryPath -Force -ErrorAction Stop)) {
			$entryPath = Get-ContainedFullPath `
				-AllowedRoot $generatedRootPath `
				-Path $entry.FullName
			if (($entry.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
				throw "Generated tree contains a reparse point: $entryPath"
			}
			if ($entry.PSIsContainer) {
				$pending.Push($entryPath)
			} else {
				$files.Add($entryPath)
			}
		}
	}

	return [pscustomobject]@{
		AllowedRoot = $allowedRootPath
		GeneratedRoot = $generatedRootPath
		Directories = $directories.ToArray()
		Files = $files.ToArray()
	}
}

function Remove-SafeGeneratedFile {
	param(
		[Parameter(Mandatory = $true)][string]$AllowedRoot,
		[Parameter(Mandatory = $true)][string]$GeneratedRoot,
		[Parameter(Mandatory = $true)][string]$Path
	)

	$generatedRootPath = Get-ContainedFullPath `
		-AllowedRoot $AllowedRoot `
		-Path $GeneratedRoot
	$filePath = Get-ContainedFullPath `
		-AllowedRoot $generatedRootPath `
		-Path $Path
	$parentPath = Split-Path -Parent $filePath
	Assert-DirectoryChainNoReparse `
		-Path $parentPath `
		-Description "Generated file parent path"
	if (!(Test-Path -LiteralPath $filePath -PathType Leaf -ErrorAction Stop)) {
		throw "Generated file changed or disappeared before removal: $filePath"
	}
	$item = Get-Item -LiteralPath $filePath -Force -ErrorAction Stop
	if (($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
		throw "Generated file became a reparse point before removal: $filePath"
	}
	Remove-Item -LiteralPath $filePath -Force -ErrorAction Stop
	if (Test-Path -LiteralPath $filePath -ErrorAction Stop) {
		throw "Generated file still exists after removal: $filePath"
	}
}

function Remove-SafeGeneratedDirectory {
	param(
		[Parameter(Mandatory = $true)][string]$AllowedRoot,
		[Parameter(Mandatory = $true)][string]$GeneratedRoot,
		[Parameter(Mandatory = $true)][string]$Path
	)

	$generatedRootPath = Get-ContainedFullPath `
		-AllowedRoot $AllowedRoot `
		-Path $GeneratedRoot
	$directoryPath = Get-ContainedFullPath `
		-AllowedRoot $generatedRootPath `
		-Path $Path `
		-AllowEqual
	Assert-DirectoryChainNoReparse `
		-Path $directoryPath `
		-Description "Generated directory path"
	Assert-DirectoryNoReparse `
		-Path $directoryPath `
		-Description "Generated directory"
	if (@(Get-ChildItem -LiteralPath $directoryPath -Force -ErrorAction Stop).Count -ne 0) {
		throw "Generated directory changed or is not empty before removal: $directoryPath"
	}
	Remove-Item -LiteralPath $directoryPath -Force -ErrorAction Stop
	if (Test-Path -LiteralPath $directoryPath -ErrorAction Stop) {
		throw "Generated directory still exists after removal: $directoryPath"
	}
}

function Reset-SafeGeneratedTree {
	[CmdletBinding()]
	param(
		[Parameter(Mandatory = $true)][string]$AllowedRoot,
		[Parameter(Mandatory = $true)][string]$GeneratedPath
	)

	$inventory = Get-SafeGeneratedTreeInventory `
		-AllowedRoot $AllowedRoot `
		-GeneratedPath $GeneratedPath
	foreach ($filePath in @($inventory.Files)) {
		Remove-SafeGeneratedFile `
			-AllowedRoot $inventory.AllowedRoot `
			-GeneratedRoot $inventory.GeneratedRoot `
			-Path $filePath
	}
	$directories = @($inventory.Directories | Sort-Object Length -Descending)
	foreach ($directoryPath in $directories) {
		Remove-SafeGeneratedDirectory `
			-AllowedRoot $inventory.AllowedRoot `
			-GeneratedRoot $inventory.GeneratedRoot `
			-Path $directoryPath
	}

	$rebuildPath = Get-ContainedFullPath `
		-AllowedRoot $inventory.AllowedRoot `
		-Path $inventory.GeneratedRoot
	Assert-DirectoryChainNoReparse `
		-Path $rebuildPath `
		-Description "Generated tree rebuild path"
	[void][System.IO.Directory]::CreateDirectory($rebuildPath)
	Assert-DirectoryChainNoReparse `
		-Path $rebuildPath `
		-Description "Rebuilt generated tree path"
	Assert-DirectoryNoReparse `
		-Path $rebuildPath `
		-Description "Rebuilt generated tree"
	if (@(Get-ChildItem -LiteralPath $rebuildPath -Force -ErrorAction Stop).Count -ne 0) {
		throw "Rebuilt generated tree is unexpectedly non-empty: $rebuildPath"
	}
	return $rebuildPath
}

Export-ModuleMember -Function @(
	"Assert-SafeDirectoryChainNoReparse",
	"Reset-SafeGeneratedTree"
)
