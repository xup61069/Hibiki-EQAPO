param()

$ErrorActionPreference = "Stop"

$installRoot = [Environment]::GetEnvironmentVariable(
	"EQAPO_INSTALL_ROOT",
	[EnvironmentVariableTarget]::Process
)
$allowMissingRoot = [StringComparer]::Ordinal.Equals(
	[Environment]::GetEnvironmentVariable(
		"EQAPO_ALLOW_MISSING_INSTALL_ROOT",
		[EnvironmentVariableTarget]::Process
	),
	"1"
)
if ([string]::IsNullOrWhiteSpace($installRoot)) {
	Write-Error "The registered installation directory does not exist."
	exit 1
}

$root = [IO.Path]::GetFullPath($installRoot)
$volumeRoot = [IO.Path]::GetPathRoot($root)
if ([string]::IsNullOrWhiteSpace($volumeRoot) -or
	$volumeRoot -notmatch '^[A-Za-z]:\\$') {
	Write-Error "The installation directory must be on a local drive-letter volume."
	exit 1
}
$drive = [IO.DriveInfo]::new($volumeRoot)
if (-not $drive.IsReady -or
	$drive.DriveType -ne [IO.DriveType]::Fixed -or
	($drive.DriveFormat -ne "NTFS" -and $drive.DriveFormat -ne "ReFS")) {
	Write-Error "The installation directory must be on a ready local NTFS or ReFS fixed volume."
	exit 1
}
$systemSid = "S-1-5-18"
$administratorsSid = "S-1-5-32-544"
$trustedInstallerSid =
	"S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464"
$creatorOwnerSid = "S-1-3-0"
$safeOwners = [Collections.Generic.HashSet[string]]::new(
	[StringComparer]::OrdinalIgnoreCase
)
[void]$safeOwners.Add($systemSid)
[void]$safeOwners.Add($administratorsSid)
[void]$safeOwners.Add($trustedInstallerSid)

$safeWriters = [Collections.Generic.HashSet[string]]::new(
	[StringComparer]::OrdinalIgnoreCase
)
foreach ($sid in @(
	$systemSid,
	$administratorsSid,
	$trustedInstallerSid,
	$creatorOwnerSid
)) {
	[void]$safeWriters.Add($sid)
}

# Use only .NET ACL APIs. An elevated process must not auto-load a PowerShell
# module from a caller-controlled PSModulePath merely to validate its trust root.
$aclSections =
	[Security.AccessControl.AccessControlSections]::Owner -bor
	[Security.AccessControl.AccessControlSections]::Access
# Test only write-capable bits. Aggregate enum values such as FullControl also
# contain read bits and would incorrectly classify ReadAndExecute as writable.
$dangerousRootRights = [uint64]0x500D0156
# An ancestor need not be read-only, but an untrusted principal must not be able
# to replace the next path component (DELETE_CHILD), take ownership, or rewrite
# the DACL. Checking every ancestor also closes junction/symlink swap paths.
$dangerousAncestorRights = [uint64]0x000D0040
$current = [IO.DirectoryInfo]::new($root)
$current.Refresh()
if (-not $current.Exists) {
	if (-not $allowMissingRoot) {
		Write-Error "The registered installation directory does not exist."
		exit 1
	}
	do {
		$current = $current.Parent
		if ($null -eq $current) {
			Write-Error "No existing installation-path ancestor could be validated."
			exit 1
		}
		$current.Refresh()
	} while (-not $current.Exists)
}
$isInstallRoot = $true
while ($null -ne $current) {
	$current.Refresh()
	if (-not $current.Exists) {
		Write-Error "An installation path component does not exist: $($current.FullName)"
		exit 1
	}
	if (($current.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
		Write-Error "An installation path component is a reparse point: $($current.FullName)"
		exit 1
	}

	$acl = [IO.Directory]::GetAccessControl($current.FullName, $aclSections)
	$ownerSid = $acl.GetOwner(
		[Security.Principal.SecurityIdentifier]
	).Value
	if (-not $safeOwners.Contains($ownerSid)) {
		Write-Error "An installation path component has an untrusted owner: $($current.FullName) ($ownerSid)"
		exit 1
	}

	$dangerousRights = if ($isInstallRoot) {
		$dangerousRootRights
	} else {
		$dangerousAncestorRights
	}
	$rules = $acl.GetAccessRules(
		$true,
		$true,
		[Security.Principal.SecurityIdentifier]
	)
	foreach ($rule in $rules) {
		if ($rule.AccessControlType -ne
			[Security.AccessControl.AccessControlType]::Allow -or
			($rule.PropagationFlags -band
				[Security.AccessControl.PropagationFlags]::InheritOnly) -ne 0) {
			continue
		}
		$rawRights = [uint64][BitConverter]::ToUInt32(
			[BitConverter]::GetBytes([int]$rule.FileSystemRights),
			0
		)
		if (($rawRights -band $dangerousRights) -eq 0) {
			continue
		}
		$sid = $rule.IdentityReference.Value
		if (-not $safeWriters.Contains($sid)) {
			Write-Error "An installation path component grants unsafe access to ${sid}: $($current.FullName)"
			exit 1
		}
	}

	$isInstallRoot = $false
	$current = $current.Parent
}

exit 0
