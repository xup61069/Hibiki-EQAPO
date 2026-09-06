param(
	[Parameter(Mandatory = $true)]
	[string] $InstallRoot,
	[switch] $ProtectInteractiveApplications
)

$ErrorActionPreference = "Stop"

$nativeSource = @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Text;

public static class EqualizerApoProcessStopper
{
    private const uint PROCESS_TERMINATE = 0x0001;
    private const uint PROCESS_QUERY_LIMITED_INFORMATION = 0x1000;
    private const uint SYNCHRONIZE = 0x00100000;
    private const uint WAIT_OBJECT_0 = 0;

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern IntPtr OpenProcess(
        uint desiredAccess,
        bool inheritHandle,
        uint processId);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool QueryFullProcessImageName(
        IntPtr process,
        uint flags,
        StringBuilder executablePath,
        ref uint size);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool TerminateProcess(IntPtr process, uint exitCode);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern uint WaitForSingleObject(IntPtr handle, uint milliseconds);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool CloseHandle(IntPtr handle);

    public static string QueryPathAndTerminate(
        uint processId,
        string expectedPath,
        bool terminate)
    {
        uint desiredAccess = PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE;
        if (terminate)
            desiredAccess |= PROCESS_TERMINATE;
        IntPtr process = OpenProcess(
            desiredAccess,
            false,
            processId);
        if (process == IntPtr.Zero)
        {
            int error = Marshal.GetLastWin32Error();
            // ERROR_INVALID_PARAMETER means the enumerated process exited before
            // OpenProcess. Other failures (notably access denied) are ambiguous
            // and must stop installation/uninstallation rather than be ignored.
            if (error == 87)
                return null;
            throw new Win32Exception(error);
        }

        try
        {
            var path = new StringBuilder(32768);
            uint length = (uint)path.Capacity;
            if (!QueryFullProcessImageName(process, 0, path, ref length))
            {
                int error = Marshal.GetLastWin32Error();
                if (WaitForSingleObject(process, 0) == WAIT_OBJECT_0)
                    return null;
                throw new Win32Exception(error);
            }

            string actualPath = System.IO.Path.GetFullPath(path.ToString());
            if (!string.Equals(
                actualPath,
                expectedPath,
                StringComparison.OrdinalIgnoreCase))
            {
                return null;
            }

            if (!terminate)
                return actualPath;

            // The same handle is used for the identity check and termination,
            // so PID reuse cannot redirect the operation to another process.
            if (!TerminateProcess(process, 1))
                throw new Win32Exception(Marshal.GetLastWin32Error());
            if (WaitForSingleObject(process, 5000) != WAIT_OBJECT_0)
                throw new TimeoutException("The process did not stop within five seconds.");
            return actualPath;
        }
        finally
        {
            CloseHandle(process);
        }
    }
}
'@

if (-not ("EqualizerApoProcessStopper" -as [type])) {
	Add-Type -TypeDefinition $nativeSource -Language CSharp
}

$root = [IO.Path]::GetFullPath($InstallRoot)
if (-not [IO.Directory]::Exists($root)) {
	throw "The installed product directory does not exist: $root"
}
$rootPath = [IO.Path]::GetPathRoot($root)
if ([StringComparer]::OrdinalIgnoreCase.Equals(
	$root.TrimEnd('\', '/'),
	$rootPath.TrimEnd('\', '/')
)) {
	throw "Refusing to inspect a filesystem root as an installed product directory: $root"
}
$leafNames = @(
	"Editor.exe",
	"DeviceSelector.exe",
	"UpdateChecker.exe",
	"Benchmark.exe",
	"VoicemeeterClient.exe",
	"EqApoOutProcHost.exe"
)
$interactiveLeafNames = [Collections.Generic.HashSet[string]]::new(
	[StringComparer]::OrdinalIgnoreCase
)
foreach ($leafName in $leafNames) {
	if ($leafName -ne "EqApoOutProcHost.exe") {
		[void]$interactiveLeafNames.Add($leafName)
	}
}
$failures = [Collections.Generic.List[string]]::new()
$runningInteractiveProcesses = [Collections.Generic.List[string]]::new()
foreach ($leafName in $leafNames) {
	$processName = [IO.Path]::GetFileNameWithoutExtension($leafName)
	$expectedPath = [IO.Path]::GetFullPath((Join-Path $root $leafName))
	$requiresConsent = (
		$ProtectInteractiveApplications -and
		$interactiveLeafNames.Contains($leafName)
	)
	# EqApoOutProcHost has no document UI and can normally be stopped without a
	# prompt. If an interactive application was found first, defer the host too so
	# declining the prompt leaves every product process untouched.
	$deferredForConsent = (
		$ProtectInteractiveApplications -and
		$leafName -eq "EqApoOutProcHost.exe" -and
		$runningInteractiveProcesses.Count -ne 0
	)
	$terminate = -not ($requiresConsent -or $deferredForConsent)
	foreach ($process in [Diagnostics.Process]::GetProcessesByName($processName)) {
		try {
			try {
				$terminatedPath = [EqualizerApoProcessStopper]::QueryPathAndTerminate(
					[uint32]$process.Id,
					$expectedPath,
					$terminate
				)
				if (-not [string]::IsNullOrEmpty($terminatedPath)) {
					if ($terminate) {
						Write-Host "Stopped installed process: $terminatedPath (PID $($process.Id))"
					} elseif ($requiresConsent) {
						$runningInteractiveProcesses.Add(
							"$terminatedPath (PID $($process.Id))"
						)
					} else {
						Write-Host "Deferred installed host until consent: $terminatedPath (PID $($process.Id))"
					}
				}
			} catch {
				$failures.Add("PID $($process.Id): $($_.Exception.Message)")
			}
		} finally {
			$process.Dispose()
		}
	}
}

if ($failures.Count -ne 0) {
	$failures | ForEach-Object { Write-Error $_ }
	exit 1
}
if ($runningInteractiveProcesses.Count -ne 0) {
	$runningInteractiveProcesses | ForEach-Object {
		Write-Host "Running installed application requires consent before termination: $_"
	}
	exit 2
}

exit 0
