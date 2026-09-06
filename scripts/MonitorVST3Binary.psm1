Set-StrictMode -Version Latest

$script:MonitorVST3ModuleName = "HibikiEQAPOMonitor.vst3"
$script:MonitorVST3RequiredPayloadNames = @(
	$script:MonitorVST3ModuleName,
	"msvcp140.dll",
	"msvcp140_1.dll",
	"vcruntime140.dll",
	"vcruntime140_1.dll"
)
$script:MonitorVST3ForbiddenDynamicDependencies = @(
	"fftw3.dll",
	"libfftw3.dll",
	"sndfile.dll",
	"FLAC.dll",
	"libmp3lame.dll",
	"libmpghip.dll",
	"mpg123.dll",
	"ogg.dll",
	"opus.dll",
	"vorbis.dll",
	"vorbisenc.dll",
	"vorbisfile.dll"
)

function Assert-PeRange {
	param(
		[Parameter(Mandatory = $true)][long]$Offset,
		[Parameter(Mandatory = $true)][long]$Length,
		[Parameter(Mandatory = $true)][long]$FileLength,
		[Parameter(Mandatory = $true)][string]$Description
	)

	if ($Offset -lt 0 -or $Length -lt 0 -or
		$Offset -gt $FileLength -or $Length -gt ($FileLength - $Offset)) {
		throw "Invalid PE $Description range (offset=$Offset, length=$Length)."
	}
}

function Read-PeUInt16 {
	param(
		[Parameter(Mandatory = $true)][System.IO.BinaryReader]$Reader,
		[Parameter(Mandatory = $true)][long]$Offset,
		[Parameter(Mandatory = $true)][long]$FileLength,
		[Parameter(Mandatory = $true)][string]$Description
	)

	Assert-PeRange -Offset $Offset -Length 2 -FileLength $FileLength -Description $Description
	$Reader.BaseStream.Position = $Offset
	return $Reader.ReadUInt16()
}

function Read-PeUInt32 {
	param(
		[Parameter(Mandatory = $true)][System.IO.BinaryReader]$Reader,
		[Parameter(Mandatory = $true)][long]$Offset,
		[Parameter(Mandatory = $true)][long]$FileLength,
		[Parameter(Mandatory = $true)][string]$Description
	)

	Assert-PeRange -Offset $Offset -Length 4 -FileLength $FileLength -Description $Description
	$Reader.BaseStream.Position = $Offset
	return $Reader.ReadUInt32()
}

function Read-PeUInt64 {
	param(
		[Parameter(Mandatory = $true)][System.IO.BinaryReader]$Reader,
		[Parameter(Mandatory = $true)][long]$Offset,
		[Parameter(Mandatory = $true)][long]$FileLength,
		[Parameter(Mandatory = $true)][string]$Description
	)

	Assert-PeRange -Offset $Offset -Length 8 -FileLength $FileLength -Description $Description
	$Reader.BaseStream.Position = $Offset
	return $Reader.ReadUInt64()
}

function Convert-PeRvaToFileOffset {
	param(
		[Parameter(Mandatory = $true)][uint32]$Rva,
		[Parameter(Mandatory = $true)][long]$FileLength,
		[Parameter(Mandatory = $true)][uint32]$SizeOfHeaders,
		[Parameter(Mandatory = $true)][object[]]$Sections,
		[Parameter(Mandatory = $true)][string]$Description
	)

	if ([uint64]$Rva -lt [uint64]$SizeOfHeaders) {
		Assert-PeRange -Offset ([long]$Rva) -Length 1 -FileLength $FileLength -Description $Description
		return [long]$Rva
	}

	foreach ($section in $Sections) {
		$sectionStart = [uint64]$section.VirtualAddress
		$sectionSpan = [Math]::Max(
			[uint64]$section.VirtualSize,
			[uint64]$section.SizeOfRawData
		)
		$sectionEnd = $sectionStart + $sectionSpan
		if ([uint64]$Rva -ge $sectionStart -and [uint64]$Rva -lt $sectionEnd) {
			$delta = [uint64]$Rva - $sectionStart
			if ($delta -ge [uint64]$section.SizeOfRawData) {
				throw "PE $Description points into zero-filled section data."
			}
			$fileOffset = [uint64]$section.PointerToRawData + $delta
			if ($fileOffset -gt [uint64][long]::MaxValue) {
				throw "PE $Description file offset is too large."
			}
			Assert-PeRange `
				-Offset ([long]$fileOffset) `
				-Length 1 `
				-FileLength $FileLength `
				-Description $Description
			return [long]$fileOffset
		}
	}

	throw ("PE {0} RVA 0x{1:X8} is not backed by file data." -f $Description, $Rva)
}

function Read-PeAsciiString {
	param(
		[Parameter(Mandatory = $true)][System.IO.BinaryReader]$Reader,
		[Parameter(Mandatory = $true)][long]$Offset,
		[Parameter(Mandatory = $true)][long]$FileLength,
		[Parameter(Mandatory = $true)][string]$Description
	)

	Assert-PeRange -Offset $Offset -Length 1 -FileLength $FileLength -Description $Description
	$Reader.BaseStream.Position = $Offset
	$builder = New-Object System.Text.StringBuilder
	for ($index = 0; $index -lt 512; $index++) {
		if ($Reader.BaseStream.Position -ge $FileLength) {
			throw "PE $Description is not null-terminated."
		}
		$value = $Reader.ReadByte()
		if ($value -eq 0) {
			if ($builder.Length -eq 0) {
				throw "PE $Description is empty."
			}
			return $builder.ToString()
		}
		if ($value -lt 0x20 -or $value -gt 0x7E) {
			throw "PE $Description contains a non-ASCII byte."
		}
		[void]$builder.Append([char]$value)
	}
	throw "PE $Description exceeds 511 bytes."
}

function Get-PeDataDirectory {
	param(
		[Parameter(Mandatory = $true)][System.IO.BinaryReader]$Reader,
		[Parameter(Mandatory = $true)][long]$OptionalHeaderOffset,
		[Parameter(Mandatory = $true)][uint16]$OptionalHeaderSize,
		[Parameter(Mandatory = $true)][uint32]$DirectoryCount,
		[Parameter(Mandatory = $true)][int]$Index,
		[Parameter(Mandatory = $true)][long]$FileLength
	)

	if ($DirectoryCount -le $Index) {
		return [pscustomobject]@{ Rva = [uint32]0; Size = [uint32]0 }
	}
	$directoryOffset = $OptionalHeaderOffset + 112 + ($Index * 8)
	$optionalHeaderEnd = $OptionalHeaderOffset + [long]$OptionalHeaderSize
	if ($directoryOffset -gt $optionalHeaderEnd -or
		8 -gt ($optionalHeaderEnd - $directoryOffset)) {
		throw "PE data directory $Index is outside the optional header."
	}
	return [pscustomobject]@{
		Rva = Read-PeUInt32 `
			-Reader $Reader `
			-Offset $directoryOffset `
			-FileLength $FileLength `
			-Description "data directory $Index RVA"
		Size = Read-PeUInt32 `
			-Reader $Reader `
			-Offset ($directoryOffset + 4) `
			-FileLength $FileLength `
			-Description "data directory $Index size"
	}
}

function Read-PeImportNames {
	param(
		[Parameter(Mandatory = $true)][System.IO.BinaryReader]$Reader,
		[Parameter(Mandatory = $true)][long]$FileLength,
		[Parameter(Mandatory = $true)][uint32]$SizeOfHeaders,
		[Parameter(Mandatory = $true)][object[]]$Sections,
		[Parameter(Mandatory = $true)][object]$Directory,
		[Parameter(Mandatory = $true)][uint64]$ImageBase,
		[Parameter(Mandatory = $true)][bool]$Delay
	)

	if ($Directory.Rva -eq 0 -and $Directory.Size -eq 0) {
		return @()
	}
	if ($Directory.Rva -eq 0 -or $Directory.Size -eq 0) {
		throw "PE import directory has an incomplete RVA/size pair."
	}

	$descriptorSize = if ($Delay) { 32 } else { 20 }
	if ($Directory.Size -lt $descriptorSize -or
		($Directory.Size % $descriptorSize) -ne 0) {
		throw "PE import directory has an invalid descriptor span."
	}
	$directoryOffset = Convert-PeRvaToFileOffset `
		-Rva $Directory.Rva `
		-FileLength $FileLength `
		-SizeOfHeaders $SizeOfHeaders `
		-Sections $Sections `
		-Description "import directory"
	$descriptorCount = [int]([uint32]$Directory.Size / $descriptorSize)
	$names = New-Object 'System.Collections.Generic.List[string]'
	$terminated = $false
	for ($index = 0; $index -lt $descriptorCount; $index++) {
		$descriptorOffset = $directoryOffset + ($index * $descriptorSize)
		Assert-PeRange `
			-Offset $descriptorOffset `
			-Length $descriptorSize `
			-FileLength $FileLength `
			-Description "import descriptor"
		$values = @()
		for ($field = 0; $field -lt ($descriptorSize / 4); $field++) {
			$values += Read-PeUInt32 `
				-Reader $Reader `
				-Offset ($descriptorOffset + ($field * 4)) `
				-FileLength $FileLength `
				-Description "import descriptor field"
		}
		if (@($values | Where-Object { $_ -ne 0 }).Count -eq 0) {
			$terminated = $true
			break
		}

		if ($Delay) {
			$attributes = [uint32]$values[0]
			if (($attributes -band 0xFFFFFFFE) -ne 0) {
				throw "PE delay-import descriptor has unsupported attributes."
			}
			$nameValue = [uint64][uint32]$values[1]
			if (($attributes -band 1) -ne 0) {
				$nameRva = [uint32]$nameValue
			} else {
				if ($nameValue -lt $ImageBase -or
					($nameValue - $ImageBase) -gt [uint32]::MaxValue) {
					throw "PE delay-import name VA is outside the image."
				}
				$nameRva = [uint32]($nameValue - $ImageBase)
			}
		} else {
			$nameRva = [uint32]$values[3]
		}
		if ($nameRva -eq 0) {
			throw "PE import descriptor has no dependency name."
		}
		$nameOffset = Convert-PeRvaToFileOffset `
			-Rva $nameRva `
			-FileLength $FileLength `
			-SizeOfHeaders $SizeOfHeaders `
			-Sections $Sections `
			-Description "import dependency name"
		$names.Add((Read-PeAsciiString `
			-Reader $Reader `
			-Offset $nameOffset `
			-FileLength $FileLength `
			-Description "import dependency name"))
	}
	if (!$terminated) {
		throw "PE import directory has no terminating descriptor."
	}
	return $names.ToArray()
}

function Read-PeExportNames {
	param(
		[Parameter(Mandatory = $true)][System.IO.BinaryReader]$Reader,
		[Parameter(Mandatory = $true)][long]$FileLength,
		[Parameter(Mandatory = $true)][uint32]$SizeOfHeaders,
		[Parameter(Mandatory = $true)][object[]]$Sections,
		[Parameter(Mandatory = $true)][object]$Directory
	)

	if ($Directory.Rva -eq 0 -and $Directory.Size -eq 0) {
		return @()
	}
	if ($Directory.Rva -eq 0 -or $Directory.Size -lt 40) {
		throw "PE export directory has an incomplete or invalid RVA/size pair."
	}
	$directoryOffset = Convert-PeRvaToFileOffset `
		-Rva $Directory.Rva `
		-FileLength $FileLength `
		-SizeOfHeaders $SizeOfHeaders `
		-Sections $Sections `
		-Description "export directory"
	Assert-PeRange `
		-Offset $directoryOffset `
		-Length 40 `
		-FileLength $FileLength `
		-Description "export directory"
	$functionCount = Read-PeUInt32 `
		-Reader $Reader `
		-Offset ($directoryOffset + 20) `
		-FileLength $FileLength `
		-Description "export function count"
	$nameCount = Read-PeUInt32 `
		-Reader $Reader `
		-Offset ($directoryOffset + 24) `
		-FileLength $FileLength `
		-Description "export name count"
	if ($nameCount -gt $functionCount -or $nameCount -gt 4096) {
		throw "PE export directory has an invalid name count."
	}
	if ($nameCount -eq 0) {
		return @()
	}
	$nameTableRva = Read-PeUInt32 `
		-Reader $Reader `
		-Offset ($directoryOffset + 32) `
		-FileLength $FileLength `
		-Description "export name table RVA"
	if ($nameTableRva -eq 0) {
		throw "PE export directory has no name table."
	}
	$nameTableOffset = Convert-PeRvaToFileOffset `
		-Rva $nameTableRva `
		-FileLength $FileLength `
		-SizeOfHeaders $SizeOfHeaders `
		-Sections $Sections `
		-Description "export name table"
	Assert-PeRange `
		-Offset $nameTableOffset `
		-Length ([long]$nameCount * 4) `
		-FileLength $FileLength `
		-Description "export name table"
	$names = New-Object 'System.Collections.Generic.List[string]'
	for ($index = 0; $index -lt $nameCount; $index++) {
		$nameRva = Read-PeUInt32 `
			-Reader $Reader `
			-Offset ($nameTableOffset + ($index * 4)) `
			-FileLength $FileLength `
			-Description "export name RVA"
		$nameOffset = Convert-PeRvaToFileOffset `
			-Rva $nameRva `
			-FileLength $FileLength `
			-SizeOfHeaders $SizeOfHeaders `
			-Sections $Sections `
			-Description "export name"
		$names.Add((Read-PeAsciiString `
			-Reader $Reader `
			-Offset $nameOffset `
			-FileLength $FileLength `
			-Description "export name"))
	}
	return $names.ToArray()
}

function Read-Amd64PeMetadata {
	param([Parameter(Mandatory = $true)][string]$Path)

	if (!(Test-Path -LiteralPath $Path -PathType Leaf)) {
		throw "PE file not found: $Path"
	}
	$item = Get-Item -LiteralPath $Path -Force
	if (($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0 -or
		$item.Length -le 0) {
		throw "PE input must be a regular non-empty file: $Path"
	}

	$stream = $null
	$reader = $null
	try {
		$stream = [System.IO.File]::Open(
			$item.FullName,
			[System.IO.FileMode]::Open,
			[System.IO.FileAccess]::Read,
			[System.IO.FileShare]::Read
		)
		$reader = New-Object System.IO.BinaryReader($stream)
		$fileLength = [long]$stream.Length
		if ($fileLength -lt 64) {
			throw "File is too short to be a PE image: $($item.FullName)"
		}
		if ((Read-PeUInt16 -Reader $reader -Offset 0 -FileLength $fileLength -Description "DOS signature") -ne 0x5A4D) {
			throw "File has no PE DOS signature: $($item.FullName)"
		}
		$peOffset = [long](Read-PeUInt32 `
			-Reader $reader `
			-Offset 0x3C `
			-FileLength $fileLength `
			-Description "PE header offset")
		if ((Read-PeUInt32 `
				-Reader $reader `
				-Offset $peOffset `
				-FileLength $fileLength `
				-Description "PE signature") -ne 0x00004550) {
			throw "File has no PE signature: $($item.FullName)"
		}

		$coffOffset = $peOffset + 4
		$machine = Read-PeUInt16 `
			-Reader $reader `
			-Offset $coffOffset `
			-FileLength $fileLength `
			-Description "COFF machine"
		if ($machine -ne 0x8664) {
			throw ("PE image is not AMD64 (machine=0x{0:X4}): {1}" -f $machine, $item.FullName)
		}
		$sectionCount = Read-PeUInt16 `
			-Reader $reader `
			-Offset ($coffOffset + 2) `
			-FileLength $fileLength `
			-Description "COFF section count"
		if ($sectionCount -eq 0 -or $sectionCount -gt 96) {
			throw "PE image has an invalid section count: $($item.FullName)"
		}
		$characteristics = Read-PeUInt16 `
			-Reader $reader `
			-Offset ($coffOffset + 18) `
			-FileLength $fileLength `
			-Description "COFF characteristics"
		$optionalHeaderSize = Read-PeUInt16 `
			-Reader $reader `
			-Offset ($coffOffset + 16) `
			-FileLength $fileLength `
			-Description "COFF optional-header size"
		$optionalHeaderOffset = $coffOffset + 20
		if ($optionalHeaderSize -lt 112) {
			throw "PE32+ optional header is too short: $($item.FullName)"
		}
		Assert-PeRange `
			-Offset $optionalHeaderOffset `
			-Length $optionalHeaderSize `
			-FileLength $fileLength `
			-Description "optional header"
		if ((Read-PeUInt16 `
				-Reader $reader `
				-Offset $optionalHeaderOffset `
				-FileLength $fileLength `
				-Description "optional-header magic") -ne 0x20B) {
			throw "AMD64 image is not PE32+: $($item.FullName)"
		}
		$imageBase = Read-PeUInt64 `
			-Reader $reader `
			-Offset ($optionalHeaderOffset + 24) `
			-FileLength $fileLength `
			-Description "image base"
		$sizeOfHeaders = Read-PeUInt32 `
			-Reader $reader `
			-Offset ($optionalHeaderOffset + 60) `
			-FileLength $fileLength `
			-Description "size of headers"
		$directoryCount = Read-PeUInt32 `
			-Reader $reader `
			-Offset ($optionalHeaderOffset + 108) `
			-FileLength $fileLength `
			-Description "data-directory count"
		if ($directoryCount -gt 16) {
			$directoryCount = 16
		}

		$sectionTableOffset = $optionalHeaderOffset + [long]$optionalHeaderSize
		Assert-PeRange `
			-Offset $sectionTableOffset `
			-Length ([long]$sectionCount * 40) `
			-FileLength $fileLength `
			-Description "section table"
		$headerEnd = [uint64]$sectionTableOffset + ([uint64]$sectionCount * 40)
		if ([uint64]$sizeOfHeaders -lt $headerEnd -or
			[uint64]$sizeOfHeaders -gt [uint64]$fileLength) {
			throw "PE SizeOfHeaders does not cover the parsed headers: $($item.FullName)"
		}
		$sections = @()
		for ($index = 0; $index -lt $sectionCount; $index++) {
			$sectionOffset = $sectionTableOffset + ($index * 40)
			$sections += [pscustomobject]@{
				VirtualSize = Read-PeUInt32 `
					-Reader $reader `
					-Offset ($sectionOffset + 8) `
					-FileLength $fileLength `
					-Description "section virtual size"
				VirtualAddress = Read-PeUInt32 `
					-Reader $reader `
					-Offset ($sectionOffset + 12) `
					-FileLength $fileLength `
					-Description "section virtual address"
				SizeOfRawData = Read-PeUInt32 `
					-Reader $reader `
					-Offset ($sectionOffset + 16) `
					-FileLength $fileLength `
					-Description "section raw size"
				PointerToRawData = Read-PeUInt32 `
					-Reader $reader `
					-Offset ($sectionOffset + 20) `
					-FileLength $fileLength `
					-Description "section raw offset"
			}
		}

		$exportDirectory = Get-PeDataDirectory `
			-Reader $reader `
			-OptionalHeaderOffset $optionalHeaderOffset `
			-OptionalHeaderSize $optionalHeaderSize `
			-DirectoryCount $directoryCount `
			-Index 0 `
			-FileLength $fileLength
		$importDirectory = Get-PeDataDirectory `
			-Reader $reader `
			-OptionalHeaderOffset $optionalHeaderOffset `
			-OptionalHeaderSize $optionalHeaderSize `
			-DirectoryCount $directoryCount `
			-Index 1 `
			-FileLength $fileLength
		$delayImportDirectory = Get-PeDataDirectory `
			-Reader $reader `
			-OptionalHeaderOffset $optionalHeaderOffset `
			-OptionalHeaderSize $optionalHeaderSize `
			-DirectoryCount $directoryCount `
			-Index 13 `
			-FileLength $fileLength
		$exports = @(Read-PeExportNames `
			-Reader $reader `
			-FileLength $fileLength `
			-SizeOfHeaders $sizeOfHeaders `
			-Sections $sections `
			-Directory $exportDirectory)
		$normalImports = @(Read-PeImportNames `
			-Reader $reader `
			-FileLength $fileLength `
			-SizeOfHeaders $sizeOfHeaders `
			-Sections $sections `
			-Directory $importDirectory `
			-ImageBase $imageBase `
			-Delay $false)
		$delayImports = @(Read-PeImportNames `
			-Reader $reader `
			-FileLength $fileLength `
			-SizeOfHeaders $sizeOfHeaders `
			-Sections $sections `
			-Directory $delayImportDirectory `
			-ImageBase $imageBase `
			-Delay $true)

		return [pscustomobject]@{
			Path = $item.FullName
			Machine = [uint16]$machine
			Characteristics = [uint16]$characteristics
			Exports = $exports
			NormalImports = $normalImports
			DelayImports = $delayImports
		}
	} finally {
		if ($null -ne $reader) {
			$reader.Dispose()
		} elseif ($null -ne $stream) {
			$stream.Dispose()
		}
	}
}

function Assert-MonitorVST3Binary {
	[CmdletBinding()]
	param([Parameter(Mandatory = $true)][string]$ModulePath)

	$metadata = Read-Amd64PeMetadata -Path $ModulePath
	foreach ($kind in @("NormalImports", "DelayImports")) {
		foreach ($dependency in @($metadata.$kind)) {
			foreach ($forbidden in $script:MonitorVST3ForbiddenDynamicDependencies) {
				if ($dependency.Equals($forbidden, [StringComparison]::OrdinalIgnoreCase)) {
					$label = if ($kind -eq "DelayImports") { "delay-imports" } else { "imports" }
					throw "Monitor VST3 $label forbidden shared-process dependency '$dependency'. Rebuild it from the dedicated x64-windows-static-md tree."
				}
			}
		}
	}
	return $metadata
}

function Get-MonitorVST3RequiredPayloadNames {
	return [string[]]@($script:MonitorVST3RequiredPayloadNames)
}

function Assert-MonitorVST3PayloadBinaries {
	[CmdletBinding()]
	param([Parameter(Mandatory = $true)][string]$PayloadPath)

	$moduleMetadata = $null
	foreach ($name in $script:MonitorVST3RequiredPayloadNames) {
		$filePath = Join-Path $PayloadPath $name
		if ($name -ceq $script:MonitorVST3ModuleName) {
			$moduleMetadata = Assert-MonitorVST3Binary -ModulePath $filePath
		} else {
			$null = Read-Amd64PeMetadata -Path $filePath
		}
	}
	return $moduleMetadata
}

Export-ModuleMember -Function @(
	"Read-Amd64PeMetadata",
	"Assert-MonitorVST3Binary",
	"Assert-MonitorVST3PayloadBinaries",
	"Get-MonitorVST3RequiredPayloadNames"
)
