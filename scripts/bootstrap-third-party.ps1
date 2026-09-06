param(
    [string] $Triplet = "x64-windows",
    [string] $Configuration = "Release",
    [switch] $WithQt,
    [string] $QtVersion = "6.10.1",
    [string] $QtArch = "win64_msvc2022_64",
    [switch] $WithNsis,
    [string] $NsisVersion = "3.11",
    [string] $NsisSha256 = "C7D27F780DDB6CFFB4730138CD1591E841F4B7EDB155856901CDF5F214394FA1",
    [string] $VcpkgCommit = "30ef65cad98f08e7197c9a1656fbd871bcb72f2d"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$thirdParty = Join-Path $root "third_party"
New-Item -ItemType Directory -Force -Path $thirdParty | Out-Null

function Require-Command($Name) {
    if (-not (Get-Command $Name -ErrorAction SilentlyContinue)) {
        throw "$Name was not found in PATH"
    }
}

function Assert-Sha256 {
    param(
        [Parameter(Mandatory = $true)][string] $LiteralPath,
        [Parameter(Mandatory = $true)][string] $Expected,
        [Parameter(Mandatory = $true)][string] $Description
    )

    if (!(Test-Path -LiteralPath $LiteralPath -PathType Leaf)) {
        throw "$Description was not found: $LiteralPath"
    }
    $item = Get-Item -LiteralPath $LiteralPath -Force
    if (($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "$Description must not be a reparse point: $LiteralPath"
    }
    $actual = (Get-FileHash -LiteralPath $LiteralPath -Algorithm SHA256).Hash
    if (![StringComparer]::OrdinalIgnoreCase.Equals($actual, $Expected)) {
        throw "$Description SHA-256 mismatch. Expected $Expected but received $actual"
    }
}

function Assert-ExactFileSize {
    param(
        [Parameter(Mandatory = $true)][string] $LiteralPath,
        [Parameter(Mandatory = $true)][long] $Expected,
        [Parameter(Mandatory = $true)][string] $Description
    )

    if (!(Test-Path -LiteralPath $LiteralPath -PathType Leaf)) {
        throw "$Description was not found: $LiteralPath"
    }
    $actual = (Get-Item -LiteralPath $LiteralPath -Force).Length
    if ($actual -ne $Expected) {
        throw "$Description size mismatch. Expected $Expected bytes but received $actual"
    }
}

function Assert-GeneratedPathWithinThirdParty {
    param([Parameter(Mandatory = $true)][string] $LiteralPath)

    $thirdPartyPrefix = [System.IO.Path]::GetFullPath($thirdParty).TrimEnd('\', '/') + [System.IO.Path]::DirectorySeparatorChar
    $fullPath = [System.IO.Path]::GetFullPath($LiteralPath)
    if (!$fullPath.StartsWith($thirdPartyPrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Generated path escaped third_party: $fullPath"
    }
    return $fullPath
}

function Write-Utf8NoBom {
    param(
        [Parameter(Mandatory = $true)][string] $LiteralPath,
        [Parameter(Mandatory = $true)][string] $Contents
    )

    [System.IO.File]::WriteAllText(
        $LiteralPath,
        $Contents,
        (New-Object System.Text.UTF8Encoding($false))
    )
}

function Install-LockedQt {
    param(
        [Parameter(Mandatory = $true)][string] $QtRoot,
        [Parameter(Mandatory = $true)][string] $Version,
        [Parameter(Mandatory = $true)][string] $Architecture
    )

    if ($Version -ne "6.10.1" -or $Architecture -ne "win64_msvc2022_64") {
        throw "Qt archive lock only permits 6.10.1 win64_msvc2022_64. Update the pinned manifest to change it."
    }

    $pythonIdentity = python -c "import platform,sys; print(f'{sys.version_info.major}.{sys.version_info.minor}.{sys.version_info.micro}|{platform.machine()}')"
    if ($LASTEXITCODE -ne 0 -or $pythonIdentity.Trim() -ne "3.13.2|AMD64") {
        throw "Locked Qt extraction requires CPython 3.13.2 x64; received '$($pythonIdentity.Trim())'."
    }

    Require-Command curl.exe
    $lockFile = Join-Path $PSScriptRoot "qt-extractor-requirements-win-py313.txt"
    if (!(Test-Path -LiteralPath $lockFile -PathType Leaf)) {
        throw "Qt extractor lock file was not found: $lockFile"
    }

    $wheelRoot = Assert-GeneratedPathWithinThirdParty (Join-Path $thirdParty "downloads\qt-extractor-py313")
    New-Item -ItemType Directory -Force -Path $wheelRoot | Out-Null
    python -m pip download `
        --disable-pip-version-check `
        --require-hashes `
        --only-binary=:all: `
        --dest $wheelRoot `
        --requirement $lockFile
    if ($LASTEXITCODE -ne 0) {
        throw "Locked Qt extractor wheel download failed with exit code $LASTEXITCODE"
    }

    $archiveBase = "https://download.qt.io/online/qtsdkrepository/windows_x86/desktop/qt6_6101/qt6_6101/qt.qt6.6101.win64_msvc2022_64"
    $archives = @(
        @{
            File = "6.10.1-0-202511161843qtbase-Windows-Windows_11_24H2-MSVC2022-Windows-Windows_11_24H2-X86_64.7z"
            Sha256 = "4de2db56548aa6084479210843a0905d7a3ae3539ef6b918ac88d12a5eeb8178"
            Bytes = 42171066
        },
        @{
            File = "6.10.1-0-202511161843qtdeclarative-Windows-Windows_11_24H2-MSVC2022-Windows-Windows_11_24H2-X86_64.7z"
            Sha256 = "10f98479ca1aa50e53eee9c7d30143cb717da64447bc8a73f40094f9eff29cb3"
            Bytes = 151504709
        },
        @{
            File = "6.10.1-0-202511161843qtsvg-Windows-Windows_11_24H2-MSVC2022-Windows-Windows_11_24H2-X86_64.7z"
            Sha256 = "78949d2c42d9a35ef9d3bf376bec75de2321573de4c60cf0326f6f4b3d0f79c7"
            Bytes = 661125
        },
        @{
            File = "6.10.1-0-202511161843qttools-Windows-Windows_11_24H2-MSVC2022-Windows-Windows_11_24H2-X86_64.7z"
            Sha256 = "273d5fde0844cf68b025d66ba4a57faad23c8274fe3ec98e539d44d1b8f8fb1a"
            Bytes = 26891597
        },
        @{
            File = "6.10.1-0-202511161843qttranslations-Windows-Windows_11_24H2-MSVC2022-Windows-Windows_11_24H2-X86_64.7z"
            Sha256 = "bcd7a6d1e844dfb24122f7332f860ed2afa6e1fa62960e0aa232dfa0fd027da1"
            Bytes = 1870834
        }
    )

    $archiveRoot = Assert-GeneratedPathWithinThirdParty (Join-Path $thirdParty "downloads\qt\6.10.1\win64_msvc2022_64")
    New-Item -ItemType Directory -Force -Path $archiveRoot | Out-Null
    foreach ($archive in $archives) {
        $archivePath = Join-Path $archiveRoot $archive.File
        if (Test-Path -LiteralPath $archivePath) {
            Assert-ExactFileSize -LiteralPath $archivePath -Expected $archive.Bytes -Description "Cached Qt archive $($archive.File)"
            Assert-Sha256 -LiteralPath $archivePath -Expected $archive.Sha256 -Description "Cached Qt archive $($archive.File)"
            continue
        }

        $downloadPath = "$archivePath.download.$([Guid]::NewGuid().ToString('N'))"
        try {
            curl.exe -L --fail --retry 3 --retry-max-time 1800 `
                --connect-timeout 30 --max-time 900 --max-filesize $archive.Bytes `
                --proto "=https" --proto-redir "=https" --tlsv1.2 `
                --output $downloadPath "$archiveBase/$($archive.File)"
            if ($LASTEXITCODE -ne 0) {
                throw "Qt archive download failed with exit code ${LASTEXITCODE}: $($archive.File)"
            }
            Assert-ExactFileSize -LiteralPath $downloadPath -Expected $archive.Bytes -Description "Downloaded Qt archive $($archive.File)"
            Assert-Sha256 -LiteralPath $downloadPath -Expected $archive.Sha256 -Description "Downloaded Qt archive $($archive.File)"
            Move-Item -LiteralPath $downloadPath -Destination $archivePath
        }
        finally {
            if (Test-Path -LiteralPath $downloadPath) {
                Remove-Item -LiteralPath $downloadPath -Force
            }
        }
    }

    $stageRoot = Assert-GeneratedPathWithinThirdParty (Join-Path $thirdParty ("qt-stage-" + [Guid]::NewGuid().ToString("N")))
    $pythonStage = Assert-GeneratedPathWithinThirdParty (Join-Path $thirdParty ("qt-python-" + [Guid]::NewGuid().ToString("N")))
    $stageHost = Join-Path $stageRoot "6.10.1\msvc2022_64"
    try {
        New-Item -ItemType Directory -Path $stageHost -Force | Out-Null
        New-Item -ItemType Directory -Path $pythonStage | Out-Null
        python -m pip install `
            --disable-pip-version-check `
            --no-index `
            --no-deps `
            --require-hashes `
            --only-binary=:all: `
            --find-links $wheelRoot `
            --target $pythonStage `
            --requirement $lockFile
        if ($LASTEXITCODE -ne 0) {
            throw "Offline Qt extractor install failed with exit code $LASTEXITCODE"
        }

        $savedPythonPath = $env:PYTHONPATH
        try {
            $env:PYTHONPATH = $pythonStage
            foreach ($archive in $archives) {
                $archivePath = Join-Path $archiveRoot $archive.File
                Assert-Sha256 -LiteralPath $archivePath -Expected $archive.Sha256 -Description "Qt archive before extraction $($archive.File)"
                # The locked online-repository payloads contain bin/, lib/,
                # include/, and related host-relative paths at archive root.
                python -m py7zr x $archivePath $stageHost
                if ($LASTEXITCODE -ne 0) {
                    throw "Qt archive extraction failed with exit code ${LASTEXITCODE}: $($archive.File)"
                }
            }
        }
        finally {
            $env:PYTHONPATH = $savedPythonPath
        }

        $reparsePoint = Get-ChildItem -LiteralPath $stageRoot -Force -Recurse |
            Where-Object { ($_.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0 } |
            Select-Object -First 1
        if ($null -ne $reparsePoint) {
            throw "Extracted Qt tree contains a reparse point: $($reparsePoint.FullName)"
        }

        $qconfig = Join-Path $stageHost "mkspecs\qconfig.pri"
        $qconfigText = [System.IO.File]::ReadAllText($qconfig)
        $qconfigText = [regex]::Replace($qconfigText, '(?m)^QT_EDITION\s*=.*$', 'QT_EDITION = OpenSource')
        $qconfigText = [regex]::Replace($qconfigText, '(?m)^QT_LICHECK\s*=.*$', 'QT_LICHECK =')
        Write-Utf8NoBom -LiteralPath $qconfig -Contents $qconfigText

        Write-Utf8NoBom -LiteralPath (Join-Path $stageHost "bin\qt.conf") -Contents "[Paths]`nPrefix=..`n"
        $finalHost = Join-Path $QtRoot "6.10.1\msvc2022_64"
        $qtenv = "@echo off`r`necho Setting up environment for Qt usage...`r`nset PATH=$finalHost\bin;%PATH%`r`ncd /D $finalHost`r`necho Remember to call vcvarsall.bat to complete environment setup!`r`n"
        Write-Utf8NoBom -LiteralPath (Join-Path $stageHost "bin\qtenv2.bat") -Contents $qtenv

        foreach ($prl in Get-ChildItem -LiteralPath (Join-Path $stageHost "lib") -Filter "*.prl" -File -ErrorAction SilentlyContinue) {
            $text = [System.IO.File]::ReadAllText($prl.FullName).Replace("c:/Users/qt/work/install/lib", '$$[QT_INSTALL_LIBS]')
            Write-Utf8NoBom -LiteralPath $prl.FullName -Contents $text
        }
        foreach ($pc in Get-ChildItem -LiteralPath (Join-Path $stageHost "lib\pkgconfig") -Filter "*.pc" -File -ErrorAction SilentlyContinue) {
            $text = [System.IO.File]::ReadAllText($pc.FullName).Replace("prefix=c:/Users/qt/work/install", "prefix=$finalHost")
            Write-Utf8NoBom -LiteralPath $pc.FullName -Contents $text
        }

        foreach ($required in @(
            "bin\qmake.exe",
            "bin\windeployqt.exe",
            "bin\lrelease.exe",
            "bin\lupdate.exe",
            "bin\Qt6Qml.dll",
            "translations\catalogs.json"
        )) {
            $requiredPath = Join-Path $stageHost $required
            if (!(Test-Path -LiteralPath $requiredPath -PathType Leaf)) {
                throw "Locked Qt extraction did not produce required file: $requiredPath"
            }
        }

        if (Test-Path -LiteralPath $finalHost) {
            throw "Refusing to overwrite an incomplete Qt installation: $finalHost"
        }
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $finalHost) | Out-Null
        Move-Item -LiteralPath $stageHost -Destination $finalHost
    }
    finally {
        foreach ($generatedPath in @($pythonStage, $stageRoot)) {
            $validatedPath = Assert-GeneratedPathWithinThirdParty $generatedPath
            if (Test-Path -LiteralPath $validatedPath) {
                Remove-Item -LiteralPath $validatedPath -Recurse -Force
            }
        }
    }
}

Require-Command git
Require-Command cmake
Require-Command python

$vendoredDependencies = @(
    @{ Name = "VST3 SDK"; Path = Join-Path $thirdParty "vst3sdk" },
    @{ Name = "muparserx"; Path = Join-Path $thirdParty "muparserx" },
    @{ Name = "TCLAP"; Path = Join-Path $thirdParty "tclap" }
)

foreach ($dependency in $vendoredDependencies) {
    if (!(Test-Path -LiteralPath $dependency.Path)) {
        throw "$($dependency.Name) is a tracked dependency but is missing at $($dependency.Path). Restore it from Git."
    }
}

$vcpkgRoot = Join-Path $thirdParty "vcpkg"
if (!(Test-Path -LiteralPath $vcpkgRoot)) {
    git clone --filter=blob:none --no-checkout https://github.com/microsoft/vcpkg.git $vcpkgRoot
    if ($LASTEXITCODE -ne 0) {
        throw "vcpkg clone failed with exit code $LASTEXITCODE"
    }
}
if (!(Test-Path -LiteralPath (Join-Path $vcpkgRoot ".git"))) {
    throw "The generated vcpkg directory is not a Git checkout: $vcpkgRoot"
}
git -C $vcpkgRoot fetch --depth 1 origin $VcpkgCommit
if ($LASTEXITCODE -ne 0) {
    throw "Could not fetch pinned vcpkg commit $VcpkgCommit"
}
git -C $vcpkgRoot checkout --detach $VcpkgCommit
if ($LASTEXITCODE -ne 0) {
    throw "Could not check out pinned vcpkg commit $VcpkgCommit"
}

$vcpkgExe = Join-Path $thirdParty "vcpkg\vcpkg.exe"
& (Join-Path $thirdParty "vcpkg\bootstrap-vcpkg.bat") -disableMetrics
if ($LASTEXITCODE -ne 0 -or !(Test-Path -LiteralPath $vcpkgExe)) {
    throw "vcpkg bootstrap failed with exit code $LASTEXITCODE"
}

$vcpkgInstallRoot = Join-Path $thirdParty "vcpkg_installed"
& $vcpkgExe install --triplet $Triplet --x-install-root=$vcpkgInstallRoot
if ($LASTEXITCODE -ne 0) {
    throw "vcpkg install failed with exit code $LASTEXITCODE"
}

if ($Triplet -eq "x64-windows") {
    # The in-process DAW monitor plug-in must not expose generic codec DLL
    # basenames in a shared host process. Keep its static-md dependency graph
    # in a separate vcpkg root so manifest switching cannot remove the main
    # application's dynamic x64-windows packages.
    $monitorVcpkgInstallRoot = Assert-GeneratedPathWithinThirdParty (
        Join-Path $thirdParty "monitor_vcpkg_installed"
    )
    & $vcpkgExe install `
        --triplet "x64-windows-static-md" `
        --x-install-root=$monitorVcpkgInstallRoot
    if ($LASTEXITCODE -ne 0) {
        throw "Monitor VST3 static vcpkg install failed with exit code $LASTEXITCODE"
    }
}

$muparserBuild = Join-Path $thirdParty "build\muparserx-$Triplet"
$muparserSource = Join-Path $thirdParty "muparserx"
$muparserLib = Join-Path $muparserBuild "$Configuration\muparserx.lib"
if (Test-Path -LiteralPath $muparserLib) {
    Write-Host "muparserx already built: $muparserLib"
} else {
    cmake -S $muparserSource -B $muparserBuild -A x64 -DUSE_WIDE_STRING=ON -DCMAKE_BUILD_TYPE=$Configuration
    if ($LASTEXITCODE -ne 0) {
        throw "muparserx configure failed with exit code $LASTEXITCODE"
    }
    cmake --build $muparserBuild --config $Configuration
    if ($LASTEXITCODE -ne 0) {
        throw "muparserx build failed with exit code $LASTEXITCODE"
    }
}

if ($WithQt) {
    $qtRoot = Join-Path $thirdParty "Qt"
    $qtHost = Join-Path $qtRoot "$QtVersion\msvc2022_64"
    $qmake = Join-Path $qtHost "bin\qmake.exe"
    $windeployqt = Join-Path $qtHost "bin\windeployqt.exe"
    $lrelease = Join-Path $qtHost "bin\lrelease.exe"
	$lupdate = Join-Path $qtHost "bin\lupdate.exe"
	$qtQml = Join-Path $qtHost "bin\Qt6Qml.dll"
	$qtTranslationCatalog = Join-Path $qtHost "translations\catalogs.json"

    if ((Test-Path -LiteralPath $qmake) -and
        (Test-Path -LiteralPath $windeployqt) -and
		(Test-Path -LiteralPath $lrelease) -and
		(Test-Path -LiteralPath $lupdate) -and
		(Test-Path -LiteralPath $qtQml) -and
		(Test-Path -LiteralPath $qtTranslationCatalog)) {
        Write-Host "Qt already exists: $qtHost"
    } else {
        Install-LockedQt -QtRoot $qtRoot -Version $QtVersion -Architecture $QtArch
    }
}

if ($WithNsis) {
    $nsisRoot = Join-Path $thirdParty "nsis-$NsisVersion"
    $makensis = Join-Path $nsisRoot "makensis.exe"
    if (Test-Path -LiteralPath $makensis) {
        Write-Host "NSIS already exists: $nsisRoot"
    } else {
        Require-Command curl.exe
        $zip = Join-Path $thirdParty "nsis-$NsisVersion.zip"
        $downloadZip = "$zip.download"
        if ($NsisSha256 -notmatch '^[0-9a-fA-F]{64}$') {
            throw "NsisSha256 must be a 64-character SHA-256 digest"
        }

        try {
            curl.exe -L --fail --retry 3 --proto "=https" --proto-redir "=https" --tlsv1.2 --output $downloadZip "https://sourceforge.net/projects/nsis/files/NSIS%203/$NsisVersion/nsis-$NsisVersion.zip/download"
            if ($LASTEXITCODE -ne 0) {
                throw "NSIS download failed with exit code $LASTEXITCODE"
            }

            $actualNsisSha256 = (Get-FileHash -LiteralPath $downloadZip -Algorithm SHA256).Hash
            if (![StringComparer]::OrdinalIgnoreCase.Equals($actualNsisSha256, $NsisSha256)) {
                throw "NSIS SHA-256 mismatch. Expected $NsisSha256 but received $actualNsisSha256"
            }

            Move-Item -LiteralPath $downloadZip -Destination $zip -Force
        }
        finally {
            if (Test-Path -LiteralPath $downloadZip) {
                Remove-Item -LiteralPath $downloadZip -Force
            }
        }

        Expand-Archive -LiteralPath $zip -DestinationPath $thirdParty -Force
        if (!(Test-Path -LiteralPath $makensis)) {
            throw "NSIS was not extracted to the expected path: $makensis"
        }
    }
}

Write-Host "third_party is ready."
