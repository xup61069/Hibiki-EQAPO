param(
	[string]$BaseUrl = "https://raw.githubusercontent.com/IJustItay/Neon-Equalizer/main/public/targets",
	[string]$OutputDir = (Join-Path (Split-Path $PSScriptRoot -Parent) "resources\HeadphoneCalibrations")
)

$ErrorActionPreference = "Stop"

function Get-ValidatedTargetNames {
	param(
		[Parameter(Mandatory = $true)]$Index,
		[Parameter(Mandatory = $true)][string]$Purpose
	)

	if (-not ($Index -is [pscustomobject]) -or
		-not ($Index.PSObject.Properties.Name -contains "version") -or
		[int]$Index.version -ne 1 -or
		-not ($Index.PSObject.Properties.Name -contains "targets") -or
		-not ($Index.targets -is [Array]) -or
		$Index.targets.Count -lt 1 -or
		$Index.targets.Count -gt 256) {
		throw "Invalid $Purpose calibration index schema."
	}

	$names = [Collections.Generic.HashSet[string]]::new(
		[StringComparer]::OrdinalIgnoreCase
	)
	foreach ($target in $Index.targets) {
		if ($null -eq $target -or
			-not ($target -is [pscustomobject]) -or
			-not ($target.PSObject.Properties.Name -contains "file") -or
			-not ($target.file -is [string])) {
			throw "Invalid target entry in $Purpose calibration index."
		}
		$fileName = [string]$target.file
		$win32NormalizedName = $fileName.TrimEnd([char[]]@('.', ' '))
		$deviceStem = [IO.Path]::GetFileNameWithoutExtension(
			$fileName
		).TrimEnd([char[]]@('.', ' '))
		if ([string]::IsNullOrWhiteSpace($fileName) -or
			-not [StringComparer]::Ordinal.Equals(
				$fileName,
				$win32NormalizedName
			) -or
			[IO.Path]::IsPathRooted($fileName) -or
			$fileName.Contains("/") -or
			$fileName.Contains("\") -or
			[IO.Path]::GetFileName($fileName) -ne $fileName -or
			$fileName.IndexOfAny([IO.Path]::GetInvalidFileNameChars()) -ge 0 -or
			-not [StringComparer]::OrdinalIgnoreCase.Equals(
				[IO.Path]::GetExtension($fileName),
				".txt"
			) -or
			$deviceStem -match '^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])$' -or
			-not $names.Add($fileName)) {
			throw "Unsafe or duplicate target file name in $Purpose calibration index: $fileName"
		}
	}
	return [string[]]$names
}

function Assert-RegularDirectory {
	param([Parameter(Mandatory = $true)][string]$Path)
	$item = Get-Item -LiteralPath $Path -Force
	if (-not $item.PSIsContainer -or
		($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
		throw "Expected a regular directory: $Path"
	}
}

function Save-BoundedHttpResource {
	param(
		[Parameter(Mandatory = $true)][uri]$Uri,
		[Parameter(Mandatory = $true)][string]$Destination,
		[Parameter(Mandatory = $true)][long]$MaximumBytes
	)

	$destinationStream = $null
	try {
		# Hold a non-shareable CreateNew handle for the complete download. The
		# response can neither overwrite an existing Win32 alias nor be redirected
		# through a path swapped between reservation and the HTTP stream.
		$destinationStream = [IO.File]::Open(
			$Destination,
			[IO.FileMode]::CreateNew,
			[IO.FileAccess]::Write,
			[IO.FileShare]::None
		)
		for ($attempt = 1; $attempt -le 3; $attempt++) {
			$request = $null
			$response = $null
			$sourceStream = $null
			try {
				# A retry reuses the already-reserved handle. No path lookup or
				# replacement window is reintroduced between attempts.
				$destinationStream.Position = 0
				$destinationStream.SetLength(0)
				$request = [Net.HttpWebRequest]::Create($Uri)
				$request.Method = "GET"
				$request.KeepAlive = $false
				$request.AllowAutoRedirect = $true
				$request.MaximumAutomaticRedirections = 5
				$request.Timeout = 60000
				$request.ReadWriteTimeout = 60000
				if ($Uri.IsLoopback) {
					$request.Proxy = $null
				}
				$response = $request.GetResponse()
				$contentLength = [long]$response.ContentLength
				if ($contentLength -gt $MaximumBytes) {
					throw "Remote resource exceeds the $MaximumBytes-byte limit: $Uri"
				}

				$sourceStream = $response.GetResponseStream()
				$buffer = [byte[]]::new(65536)
				$totalBytes = [long]0
				while (($bytesRead = $sourceStream.Read(
					$buffer,
					0,
					$buffer.Length
				)) -gt 0) {
					if ($totalBytes -gt ($MaximumBytes - $bytesRead)) {
						throw "Remote resource exceeds the $MaximumBytes-byte limit: $Uri"
					}
					$destinationStream.Write($buffer, 0, $bytesRead)
					$totalBytes += $bytesRead
				}
				if ($totalBytes -le 0) {
					throw "Remote resource is empty: $Uri"
				}
				$destinationStream.Flush($true)
				return $totalBytes
			}
			catch {
				$failure = $_.Exception
				$transientTransportFailure = $false
				$currentFailure = $failure
				while ($null -ne $currentFailure) {
					if ($currentFailure -is [Net.Sockets.SocketException]) {
						$transientTransportFailure = $true
						break
					}
					if ($currentFailure -is [Net.WebException] -and
						$currentFailure.Status -ne [Net.WebExceptionStatus]::ProtocolError -and
						$currentFailure.Status -ne [Net.WebExceptionStatus]::TrustFailure) {
						$transientTransportFailure = $true
						break
					}
					$currentFailure = $currentFailure.InnerException
				}
				if (-not $transientTransportFailure -or $attempt -ge 3) {
					throw
				}
			}
			finally {
				if ($null -ne $sourceStream) {
					$sourceStream.Dispose()
				}
				if ($null -ne $response) {
					$response.Dispose()
				}
				if ($null -ne $request -and $null -eq $response) {
					$request.Abort()
				}
			}
		}
	} catch {
		throw "Bounded HTTP download failed for ${Uri}: $($_.Exception.ToString())"
	} finally {
		if ($null -ne $destinationStream) {
			$destinationStream.Dispose()
		}
	}
}

$outputRoot = [IO.Path]::GetFullPath($OutputDir)
$outputLeaf = [IO.Path]::GetFileName($outputRoot.TrimEnd([char[]]@(
	[IO.Path]::DirectorySeparatorChar,
	[IO.Path]::AltDirectorySeparatorChar
)))
$outputParent = [IO.Path]::GetDirectoryName($outputRoot)
if ([string]::IsNullOrWhiteSpace($outputLeaf) -or
	[string]::IsNullOrWhiteSpace($outputParent)) {
	throw "The calibration output must not be a filesystem root: $outputRoot"
}
New-Item -ItemType Directory -Force -Path $outputParent | Out-Null
Assert-RegularDirectory $outputParent

$outputExists = Test-Path -LiteralPath $outputRoot
$oldAcl = $null
$oldManagedNames = [Collections.Generic.HashSet[string]]::new(
	[StringComparer]::OrdinalIgnoreCase
)
$unmanagedFiles = [Collections.Generic.List[IO.FileInfo]]::new()
if ($outputExists) {
	Assert-RegularDirectory $outputRoot
	$oldAcl = Get-Acl -LiteralPath $outputRoot
	$oldIndexPath = Join-Path $outputRoot "index.json"
	if (Test-Path -LiteralPath $oldIndexPath) {
		$oldIndexItem = Get-Item -LiteralPath $oldIndexPath -Force
		if (($oldIndexItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
			throw "Refusing a reparse-point calibration index: $oldIndexPath"
		}
		$oldIndex = Get-Content -LiteralPath $oldIndexPath -Raw | ConvertFrom-Json
		foreach ($name in (Get-ValidatedTargetNames $oldIndex "existing")) {
			[void]$oldManagedNames.Add($name)
		}
	}

	foreach ($item in (Get-ChildItem -LiteralPath $outputRoot -Force)) {
		if ($item.PSIsContainer -or
			($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
			throw "The calibration output contains an unmanaged directory or reparse point: $($item.FullName)"
		}
		if ([StringComparer]::OrdinalIgnoreCase.Equals($item.Name, "index.json") -or
			$oldManagedNames.Contains($item.Name)) {
			continue
		}
		$unmanagedFiles.Add($item)
	}
}

$transactionRoot = Join-Path $outputParent (
	".$outputLeaf.update." + [Guid]::NewGuid().ToString("N")
)
$manifestRoot = Join-Path $transactionRoot "manifest"
$payloadRoot = Join-Path $transactionRoot "payload"
$nextRoot = Join-Path $transactionRoot "next"
$previousRoot = Join-Path $transactionRoot "previous"
[void][IO.Directory]::CreateDirectory($manifestRoot)
[void][IO.Directory]::CreateDirectory($payloadRoot)
[void][IO.Directory]::CreateDirectory($nextRoot)
$oldMoved = $false
$committed = $false

try {
	$BaseUrl = $BaseUrl.TrimEnd("/")
	if ([string]::IsNullOrWhiteSpace($BaseUrl)) {
		throw "BaseUrl must not be empty."
	}
	$indexPath = Join-Path $manifestRoot "index.json"
	[void](Save-BoundedHttpResource `
		-Uri ([uri]"$BaseUrl/index.json") `
		-Destination $indexPath `
		-MaximumBytes 1048576
	)
	$indexItem = Get-Item -LiteralPath $indexPath -Force
	if (($indexItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0 -or
		$indexItem.Length -le 0 -or $indexItem.Length -gt 1048576) {
		throw "Downloaded calibration index is unsafe."
	}
	$index = Get-Content -LiteralPath $indexPath -Raw | ConvertFrom-Json
	$newTargetNames = @(Get-ValidatedTargetNames $index "downloaded")
	$newTargetSet = [Collections.Generic.HashSet[string]]::new(
		[StringComparer]::OrdinalIgnoreCase
	)
	foreach ($fileName in $newTargetNames) {
		[void]$newTargetSet.Add($fileName)
	}
	foreach ($unmanaged in $unmanagedFiles) {
		if ($newTargetSet.Contains($unmanaged.Name)) {
			throw "Remote target would replace an unmanaged local file: $($unmanaged.Name)"
		}
	}

	$payloadPrefix = $payloadRoot.TrimEnd([char[]]@(
		[IO.Path]::DirectorySeparatorChar,
		[IO.Path]::AltDirectorySeparatorChar
	)) + [IO.Path]::DirectorySeparatorChar
	foreach ($fileName in $newTargetNames) {
		$destination = [IO.Path]::GetFullPath((Join-Path $payloadRoot $fileName))
		if (-not $destination.StartsWith(
			$payloadPrefix,
			[StringComparison]::OrdinalIgnoreCase
		) -or -not [StringComparer]::Ordinal.Equals(
			[IO.Path]::GetFileName($destination),
			$fileName
		)) {
			throw "Target file escapes or changes in the staging directory: $fileName"
		}

		$escaped = [uri]::EscapeDataString($fileName)
		[void](Save-BoundedHttpResource `
			-Uri ([uri]"$BaseUrl/$escaped") `
			-Destination $destination `
			-MaximumBytes 16777216
		)
		$download = Get-Item -LiteralPath $destination -Force
		if (($download.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0 -or
			$download.Length -le 0 -or $download.Length -gt 16777216) {
			throw "Downloaded calibration target is unsafe: $fileName"
		}
	}

	foreach ($unmanaged in $unmanagedFiles) {
		Copy-Item -LiteralPath $unmanaged.FullName -Destination (
			Join-Path $nextRoot $unmanaged.Name
		)
	}
	foreach ($fileName in $newTargetNames) {
		Move-Item -LiteralPath (Join-Path $payloadRoot $fileName) -Destination (
			Join-Path $nextRoot $fileName
		)
	}
	Copy-Item -LiteralPath $indexPath -Destination (Join-Path $nextRoot "index.json")
	if ($null -ne $oldAcl) {
		Set-Acl -LiteralPath $nextRoot -AclObject $oldAcl
	}

	if ($outputExists) {
		Move-Item -LiteralPath $outputRoot -Destination $previousRoot
		$oldMoved = $true
	}
	try {
		Move-Item -LiteralPath $nextRoot -Destination $outputRoot
		$committed = $true
	} catch {
		if ($oldMoved -and -not (Test-Path -LiteralPath $outputRoot)) {
			Move-Item -LiteralPath $previousRoot -Destination $outputRoot
			$oldMoved = $false
		}
		throw
	}

	Write-Host "Updated headphone calibration bundle: $($newTargetNames.Count) managed targets in $outputRoot"
} finally {
	if (Test-Path -LiteralPath $transactionRoot) {
		if ($committed) {
			try {
				[IO.Directory]::Delete($transactionRoot, $true)
			} catch {
				Write-Warning "The update committed, but its private backup could not be removed: $transactionRoot"
			}
		} elseif (-not $oldMoved) {
			[IO.Directory]::Delete($transactionRoot, $true)
		}
	}
}
