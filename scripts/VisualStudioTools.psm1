function Get-VsWherePath {
	$programFilesX86 = [Environment]::GetFolderPath(
		[Environment+SpecialFolder]::ProgramFilesX86
	)
	$programFiles = [Environment]::GetFolderPath(
		[Environment+SpecialFolder]::ProgramFiles
	)
	if ([string]::IsNullOrWhiteSpace($programFilesX86) -or
		[string]::IsNullOrWhiteSpace($programFiles)) {
		throw "The trusted Program Files directories could not be resolved."
	}

	$candidates = @(
		(Join-Path $programFilesX86 "Microsoft Visual Studio\Installer\vswhere.exe"),
		(Join-Path $programFiles "Microsoft Visual Studio\Installer\vswhere.exe")
	)
	foreach ($candidate in $candidates) {
		if (Test-Path -LiteralPath $candidate -PathType Leaf) {
			return $candidate
		}
	}
	throw "vswhere.exe was not found. Install Visual Studio Installer or Visual Studio Build Tools."
}

function Get-VisualStudioInstallation {
	param([string]$Edition = "")

	$vswhere = Get-VsWherePath
	$arguments = @(
		"-latest",
		"-products",
		"*",
		"-requires",
		"Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
		"-property",
		"installationPath"
	)
	if ($Edition -ne "") {
		$arguments[2] = "Microsoft.VisualStudio.Product.$Edition"
	}

	$installation = & $vswhere @arguments | Select-Object -First 1
	if ($null -ne $installation) {
		$installation = $installation.Trim()
	}
	if ($null -eq $installation -or $installation -eq "") {
		$description = if ($Edition -eq "") { "a Visual Studio installation with C++ tools" } else { "Visual Studio $Edition with C++ tools" }
		throw "$description was not found."
	}
	return $installation
}

function Get-VisualStudioDevCmd {
	param([string]$Edition = "")

	$installation = Get-VisualStudioInstallation -Edition $Edition
	$devCmd = Join-Path $installation "Common7\Tools\VsDevCmd.bat"
	if (!(Test-Path -LiteralPath $devCmd)) {
		throw "VsDevCmd.bat was not found: $devCmd"
	}
	return $devCmd
}

function Get-VisualStudioRedistDirectory {
	param(
		[string]$Edition = "",
		[string[]]$RequiredFiles = @()
	)

	$installation = Get-VisualStudioInstallation -Edition $Edition
	$redistRoot = Join-Path $installation "VC\Redist\MSVC"
	$candidates = Get-ChildItem -LiteralPath $redistRoot -Directory -ErrorAction SilentlyContinue |
		Sort-Object Name -Descending |
		ForEach-Object {
			Get-ChildItem -LiteralPath (Join-Path $_.FullName "x64") -Directory -ErrorAction SilentlyContinue |
				Where-Object { $_.Name -match '^Microsoft\.VC\d+\.CRT$' }
		}

	foreach ($candidate in $candidates) {
		$complete = $true
		foreach ($file in $RequiredFiles) {
			if (!(Test-Path -LiteralPath (Join-Path $candidate.FullName $file))) {
				$complete = $false
				break
			}
		}
		if ($complete) {
			return $candidate.FullName
		}
	}

	throw "A complete Visual C++ x64 runtime directory was not found under $redistRoot."
}

function Get-VisualStudioDumpbin {
	param([string]$Edition = "")

	$installation = Get-VisualStudioInstallation -Edition $Edition
	$toolsRoot = Join-Path $installation "VC\Tools\MSVC"
	$candidates = Get-ChildItem -LiteralPath $toolsRoot -Directory -ErrorAction SilentlyContinue |
		Sort-Object Name -Descending |
		ForEach-Object { Join-Path $_.FullName "bin\Hostx64\x64\dumpbin.exe" }
	foreach ($candidate in $candidates) {
		if (Test-Path -LiteralPath $candidate -PathType Leaf) {
			$item = Get-Item -LiteralPath $candidate -Force
			if (($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -eq 0) {
				return $item.FullName
			}
		}
	}

	throw "The x64 dumpbin.exe tool was not found under $toolsRoot."
}

Export-ModuleMember -Function Get-VisualStudioInstallation, Get-VisualStudioDevCmd, Get-VisualStudioRedistDirectory, Get-VisualStudioDumpbin
