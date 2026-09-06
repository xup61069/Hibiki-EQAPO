#!/usr/bin/env python3
"""Regression contracts for the supported native Windows build toolchain."""

from __future__ import annotations

import json
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
BUILD_SCRIPT_PATH = ROOT / "build-local-x64.ps1"
BUILD_SCRIPT = BUILD_SCRIPT_PATH.read_text(encoding="utf-8")
VS_TOOLS_MODULE = (ROOT / "scripts" / "VisualStudioTools.psm1").read_text(
    encoding="utf-8"
)
README_ZH = (ROOT / "README.md").read_text(encoding="utf-8")
README_EN = (ROOT / "README.en.md").read_text(encoding="utf-8")
POWERSHELL = shutil.which("powershell") or shutil.which("pwsh")
CMD = shutil.which("cmd")
POISONED_MSBUILD_ENVIRONMENT_VARIABLES = (
    "CustomBeforeMicrosoftCommonProps",
    "CustomAfterMicrosoftCommonProps",
    "CustomBeforeMicrosoftCommonTargets",
    "CustomAfterMicrosoftCommonTargets",
    "DirectoryBuildPropsPath",
    "DirectoryBuildTargetsPath",
    "ForceImportBeforeCppProps",
    "ForceImportAfterCppProps",
    "ForceImportAfterCppDefaultProps",
    "ForceImportBeforeCppTargets",
    "ForceImportAfterCppTargets",
    "MSBuildProjectExtensionsPath",
    "MSBuildUserExtensionsPath",
    "VCTargetsPath",
    "VCTargetsPath18",
    "UserRootDir",
    "VCLibPackagePath",
    "VcpkgManifestDirectory",
    "VcpkgRoot",
    "ImportBeforeCppProps",
    "ImportAfterCppProps",
    "ImportBeforeCppTargets",
    "ImportAfterCppTargets",
    "CRTDefinitionalFile",
    "MetaGenTargets",
    "_CppCommonExtensionTargets",
    "UniversalCRT_PropsPath",
    "WindowsSDKProps",
    "PoisonLoaded",
    "TotallyArbitraryMsBuildProperty",
)
PROJECT_PATHS = (
    ROOT / "Common.vcxproj",
    ROOT / "Benchmark" / "Benchmark.vcxproj",
    ROOT / "DeviceSelector" / "DeviceSelector.vcxproj",
    ROOT / "EqApoOutProcHost" / "EqApoOutProcHost.vcxproj",
    ROOT / "EqualizerAPO" / "EqualizerAPO.vcxproj",
    ROOT / "MonitorVST3" / "MonitorVST3.vcxproj",
    ROOT / "UpdateChecker" / "UpdateChecker.vcxproj",
    ROOT / "VoicemeeterClient" / "VoicemeeterClient.vcxproj",
)


class BuildToolchainContractTests(unittest.TestCase):
    def test_parallel_common_build_embeds_debug_information_per_object(self) -> None:
        common_project = (ROOT / "Common.vcxproj").read_text(encoding="utf-8-sig")
        parallel_blocks = common_project.count(
            "<MultiProcessorCompilation>true</MultiProcessorCompilation>"
        )
        embedded_debug_blocks = common_project.count(
            "<DebugInformationFormat>OldStyle</DebugInformationFormat>"
        )
        self.assertGreater(parallel_blocks, 0)
        self.assertEqual(embedded_debug_blocks, parallel_blocks)
        self.assertNotIn("ForceSynchronousPDBWrites", common_project)

    def test_release_rebuild_cleans_consumers_without_rebuilding_common_references(self) -> None:
        self.assertIn("[switch]$Rebuild", BUILD_SCRIPT)
        self.assertIn('[string[]]$targetArguments = @()', BUILD_SCRIPT)
        self.assertIn('$targetArguments = @("/t:Rebuild")', BUILD_SCRIPT)
        self.assertIn('$project -ne "Common.vcxproj"', BUILD_SCRIPT)
        self.assertIn('@("/p:BuildProjectReferences=false") + $props', BUILD_SCRIPT)
        installer_build = (
            ROOT / "scripts" / "build-installer-x64.ps1"
        ).read_text(encoding="utf-8")
        self.assertIn("-Rebuild", installer_build)

    def test_documented_msvc_toolset_matches_all_native_projects(self) -> None:
        toolsets: set[str] = set()
        for project in PROJECT_PATHS:
            toolsets.update(
                re.findall(
                    r"<PlatformToolset>([^<]+)</PlatformToolset>",
                    project.read_text(encoding="utf-8-sig"),
                )
            )

        self.assertEqual(toolsets, {"v145"})
        for readme in (README_ZH, README_EN):
            self.assertIn("MSVC v145 toolset", readme)
            self.assertIn("Desktop development with C++", readme)
            self.assertIn("Windows SDK", readme)
        self.assertNotIn("Visual Studio 2022，並安裝", README_ZH)
        self.assertNotIn("Visual Studio 2022 with", README_EN)

    def test_bootstrap_paths_are_trusted_and_projects_are_invoked_individually(
        self,
    ) -> None:
        self.assertIn("function ConvertTo-CmdArgument", BUILD_SCRIPT)
        self.assertIn("function Assert-CommandLength", BUILD_SCRIPT)
        self.assertIn('$limit = if ($extension -in @(".cmd", ".bat"))', BUILD_SCRIPT)
        self.assertIn("$Value.Contains('%')", BUILD_SCRIPT)
        self.assertIn("foreach ($project in $projects)", BUILD_SCRIPT)
        self.assertIn("$systemDirectory = [Environment]::SystemDirectory", BUILD_SCRIPT)
        self.assertIn("[Environment]::GetFolderPath(", BUILD_SCRIPT)
        self.assertIn('$cmdExe = Join-Path $systemDirectory "cmd.exe"', BUILD_SCRIPT)
        self.assertIn("Test-Path -LiteralPath $cmdExe -PathType Leaf", BUILD_SCRIPT)
        self.assertIn('"SystemRoot" = $windowsDirectory', BUILD_SCRIPT)
        self.assertIn("& $cmdExe /D /V:OFF /S /C $devCmd", BUILD_SCRIPT)
        self.assertIn(
            "& $resolvedMsBuild.Source $project @targetArguments @projectProperties",
            BUILD_SCRIPT,
        )
        self.assertIn('"/p:ImportDirectoryBuildProps=false"', BUILD_SCRIPT)
        self.assertIn('"/p:ImportDirectoryBuildTargets=false"', BUILD_SCRIPT)
        self.assertIn('"/p:VCTargetsPath=$trustedVCTargetsPath"', BUILD_SCRIPT)
        self.assertIn('"/p:UserRootDir=$disabledUserRoot"', BUILD_SCRIPT)
        self.assertIn('"/noAutoResponse"', BUILD_SCRIPT)
        self.assertIn("Set-ExactProcessEnvironment -Variables $bootstrapEnvironment", BUILD_SCRIPT)
        self.assertIn("Set-ExactProcessEnvironment -Variables $buildEnvironment", BUILD_SCRIPT)
        self.assertIn("Set-ExactProcessEnvironment -Variables $callerEnvironment", BUILD_SCRIPT)
        self.assertIn("Assert-NoReparsePoint", BUILD_SCRIPT)
        self.assertIn('"/nodeReuse:false"', BUILD_SCRIPT)
        self.assertNotIn("Clear-UntrustedMsBuildEnvironment", BUILD_SCRIPT)
        for property_name in (
            "CustomBeforeMicrosoftCommonProps",
            "CustomAfterMicrosoftCommonTargets",
            "ForceImportBeforeCppProps",
            "ForceImportAfterCppTargets",
            "VcpkgManifestDirectory",
            "ImportBeforeCppProps",
            "ImportAfterCppTargets",
        ):
            with self.subTest(locked_msbuild_property=property_name):
                self.assertIn(f'"/p:{property_name}=', BUILD_SCRIPT)
        self.assertLess(
            BUILD_SCRIPT.index("Import-Module"),
            BUILD_SCRIPT.index("$devEnvironment ="),
        )
        self.assertLess(BUILD_SCRIPT.index("$devEnvironment ="), BUILD_SCRIPT.index("foreach ($project in $projects)"))
        self.assertNotIn("$env:SystemRoot", BUILD_SCRIPT)
        self.assertNotIn("${env:ProgramFiles", BUILD_SCRIPT)
        self.assertNotRegex(BUILD_SCRIPT, r"(?m)^\s*cmd(?:\.exe)?\s+/")
        self.assertNotIn("$props -join ' '", BUILD_SCRIPT)
        self.assertNotIn("Get-Command vswhere", VS_TOOLS_MODULE)
        self.assertNotIn("${env:ProgramFiles", VS_TOOLS_MODULE)
        self.assertIn("[Environment]::GetFolderPath(", VS_TOOLS_MODULE)

    @unittest.skipUnless(
        os.name == "nt" and POWERSHELL and CMD,
        "Windows, PowerShell, and cmd.exe are required for the junction test",
    )
    def test_msbuild_receives_single_properties_with_poisoned_environment(self) -> None:
        temp = pathlib.Path(tempfile.mkdtemp(prefix="eqapo-build-args-"))
        deep_parent = temp
        for index in range(2):
            deep_parent /= f"deep repository parent {index} " + ("x" * 24)
        deep_parent.mkdir(parents=True)
        junction = deep_parent / "repository path with spaces & metachar"
        capture_script = temp / "capture_args.py"
        build_harness = temp / "invoke-build.ps1"
        fake_msbuild = temp / "fake msbuild.cmd"
        argument_log = temp / "arguments.jsonl"
        probe_project = temp / "import-probe.vcxproj"
        poison_hook = temp / "malicious-before.targets"
        poisoned_vc_targets = temp / "caller-controlled-vc-targets"
        poisoned_user_root = temp / "caller-controlled-user-root"
        poisoned_windows = temp / "caller-controlled-windows"
        poisoned_system32 = poisoned_windows / "System32"
        poisoned_program_files = temp / "caller-controlled-program-files"
        poisoned_program_files_x86 = temp / "caller-controlled-program-files-x86"
        junction_created = False
        created_dependency_directories: set[pathlib.Path] = set()

        # The release workflow runs this contract on a fresh checkout before
        # dependency bootstrap. The build script intentionally validates its
        # generated include/lib roots before invoking MSBuild, so create only
        # those empty generated directories for the fake-MSBuild exercise and
        # remove exactly the directories this test created in ``finally``.
        generated_dependency_directories = (
            ROOT / "third_party" / "vcpkg_installed" / "x64-windows" / "include",
            ROOT / "third_party" / "vcpkg_installed" / "x64-windows" / "lib",
            ROOT
            / "third_party"
            / "monitor_vcpkg_installed"
            / "x64-windows-static-md"
            / "lib",
            ROOT
            / "third_party"
            / "build"
            / "muparserx-x64-windows"
            / "Release",
        )
        for required_directory in generated_dependency_directories:
            missing: list[pathlib.Path] = []
            cursor = required_directory
            while not cursor.exists() and cursor != ROOT:
                missing.append(cursor)
                cursor = cursor.parent
            required_directory.mkdir(parents=True, exist_ok=True)
            created_dependency_directories.update(missing)

        poisoned_system32.mkdir(parents=True)
        shutil.copy2(sys.executable, poisoned_system32 / "cmd.exe")
        for poisoned_program_root in (
            poisoned_program_files,
            poisoned_program_files_x86,
        ):
            fake_vswhere = (
                poisoned_program_root
                / "Microsoft Visual Studio"
                / "Installer"
                / "vswhere.exe"
            )
            fake_vswhere.parent.mkdir(parents=True)
            shutil.copy2(sys.executable, fake_vswhere)

        poison_property_group = """<Project xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <PropertyGroup><PoisonLoaded>true</PoisonLoaded></PropertyGroup>
</Project>
"""
        poisoned_vc_targets.mkdir()
        (poisoned_vc_targets / "Microsoft.Cpp.Default.props").write_text(
            poison_property_group, encoding="utf-8"
        )
        poisoned_user_root.mkdir()
        (poisoned_user_root / "Microsoft.Cpp.x64.user.props").write_text(
            poison_property_group, encoding="utf-8"
        )
        poison_hook.write_text(poison_property_group, encoding="utf-8")
        (temp / "Directory.Build.props").write_text(
            poison_property_group, encoding="utf-8"
        )
        (temp / "Directory.Build.rsp").write_text(
            "/p:PoisonLoaded=true\n", encoding="utf-8"
        )
        probe_project.write_text(
            """<Project DefaultTargets="PoisonProbe" xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <Import Project="$(VCTargetsPath)\\Microsoft.Cpp.Default.props" />
  <PropertyGroup Label="Configuration">
    <ConfigurationType>StaticLibrary</ConfigurationType>
    <PlatformToolset>v145</PlatformToolset>
  </PropertyGroup>
  <Import Project="$(VCTargetsPath)\\Microsoft.Cpp.props" />
  <ImportGroup Label="PropertySheets">
    <Import Project="$(UserRootDir)\\Microsoft.Cpp.$(Platform).user.props"
            Condition="exists('$(UserRootDir)\\Microsoft.Cpp.$(Platform).user.props')" />
  </ImportGroup>
  <Import Project="$(VCTargetsPath)\\Microsoft.Cpp.targets" />
  <Target Name="PoisonProbe">
    <Error Condition="'$(PoisonLoaded)' == 'true'" Text="Untrusted MSBuild import executed." />
    <Error Condition="'$(VCLibPackagePath)' != ''" Text="The per-user vcpkg import root was not disabled." />
  </Target>
</Project>
""",
            encoding="utf-8",
        )

        capture_script.write_text(
            f"""import json
import os
import shutil
import subprocess
import sys

blocked_names = {POISONED_MSBUILD_ENVIRONMENT_VARIABLES!r}
record = {{
    "args": sys.argv[1:],
    "blocked_environment": {{
        name: os.environ.get(name) for name in blocked_names
    }},
}}
with open({str(argument_log)!r}, "a", encoding="utf-8") as log:
    log.write(json.dumps(record) + "\\n")

real_msbuild = shutil.which("msbuild.exe") or shutil.which("msbuild")
if not real_msbuild:
    raise SystemExit("trusted MSBuild was not available in the clean child Path")

probe_property_names = {{
    "configuration", "platform", "windowstargetplatformversion",
    "vctargetspath", "userrootdir", "msbuilduserextensionspath",
    "importdirectorybuildprops", "importdirectorybuildtargets",
    "importuserlocationsbywildcardbeforemicrosoftcommonprops",
    "importuserlocationsbywildcardaftermicrosoftcommonprops",
    "importuserlocationsbywildcardbeforemicrosoftcommontargets",
    "importuserlocationsbywildcardaftermicrosoftcommontargets",
    "custombeforemicrosoftcommonprops", "customaftermicrosoftcommonprops",
    "custombeforemicrosoftcommontargets", "customaftermicrosoftcommontargets",
    "forceimportbeforecppprops", "forceimportaftercppprops",
    "forceimportaftercppdefaultprops", "forceimportbeforecpptargets",
    "forceimportaftercpptargets", "vcpkgmanifestdirectory", "vcpkgroot",
    "vcpkgactivationoptions", "vclibpackagepath", "importbeforecppprops", "importaftercppprops",
    "importbeforecpptargets", "importaftercpptargets",
}}
probe_args = []
for argument in sys.argv[1:]:
    if argument.lower() in {{"/noautoresponse", "-noautoresponse"}}:
        probe_args.append(argument)
        continue
    if not argument.lower().startswith("/p:"):
        continue
    name = argument[3:].split("=", 1)[0].lower()
    if name in probe_property_names:
        probe_args.append(argument)

probe = subprocess.run(
    [
        real_msbuild,
        {str(probe_project)!r},
        "/nologo",
        "/verbosity:quiet",
        "/nodeReuse:false",
        "/t:PoisonProbe",
        *probe_args,
    ],
    capture_output=True,
    check=False,
)
if probe.returncode != 0:
    sys.stderr.buffer.write(probe.stdout)
    sys.stderr.buffer.write(probe.stderr)
    raise SystemExit(probe.returncode)
""",
            encoding="utf-8",
        )
        fake_msbuild.write_text(
            f'@echo off\n"{sys.executable}" "{capture_script}" %*\nexit /b %ERRORLEVEL%\n',
            encoding="ascii",
        )
        build_harness.write_text(
            """param(
    [string]$BuildScript,
    [string]$MsBuildCommand,
    [string]$PoisonedWindows,
    [string]$PoisonedProgramFiles,
    [string]$PoisonedProgramFilesX86,
    [string]$PoisonedVCTargetsPath,
    [string]$PoisonedUserRoot
)
$ErrorActionPreference = "Stop"
$env:SystemRoot = $PoisonedWindows
$env:windir = $PoisonedWindows
$env:ProgramFiles = $PoisonedProgramFiles
$env:ProgramW6432 = $PoisonedProgramFiles
[Environment]::SetEnvironmentVariable(
    "ProgramFiles(x86)",
    $PoisonedProgramFilesX86,
    "Process"
)
[Environment]::SetEnvironmentVariable("PATH", $null, "Process")
[Environment]::SetEnvironmentVariable(
    "Path",
    (Join-Path $PoisonedWindows "System32"),
    "Process"
)
foreach ($variableName in $env:EQAPO_POISONED_MSBUILD_ENV_NAMES.Split(";")) {
    [Environment]::SetEnvironmentVariable(
        $variableName,
        (Join-Path $PoisonedProgramFiles "malicious-before.targets"),
        "Process"
    )
}
[Environment]::SetEnvironmentVariable(
    "VCTargetsPath", $PoisonedVCTargetsPath, "Process"
)
[Environment]::SetEnvironmentVariable(
    "UserRootDir", $PoisonedUserRoot, "Process"
)
[Environment]::SetEnvironmentVariable("PoisonLoaded", "true", "Process")
& $BuildScript -Configuration Release -MsBuildCommand $MsBuildCommand -Rebuild
if ($env:SystemRoot -ne $PoisonedWindows -or
    $env:VCTargetsPath -ne $PoisonedVCTargetsPath -or
    $env:UserRootDir -ne $PoisonedUserRoot -or
    $env:PoisonLoaded -ne "true") {
    throw "build-local-x64.ps1 did not restore the caller environment"
}
""",
            encoding="utf-8-sig",
        )

        try:
            junction_result = subprocess.run(
                [
                    CMD,
                    "/D",
                    "/C",
                    "mklink",
                    "/J",
                    str(junction),
                    str(ROOT),
                ],
                capture_output=True,
                text=True,
                timeout=30,
                check=False,
            )
            if junction_result.returncode != 0:
                self.skipTest(
                    "directory junction creation is unavailable: "
                    + junction_result.stderr.strip()
                )
            junction_created = True

            environment = os.environ.copy()
            environment["EQAPO_POISONED_MSBUILD_ENV_NAMES"] = ";".join(
                POISONED_MSBUILD_ENVIRONMENT_VARIABLES
            )
            result = subprocess.run(
                [
                    POWERSHELL,
                    "-NoProfile",
                    "-ExecutionPolicy",
                    "Bypass",
                    "-File",
                    str(build_harness),
                    "-BuildScript",
                    str(junction / BUILD_SCRIPT_PATH.name),
                    "-MsBuildCommand",
                    str(fake_msbuild),
                    "-PoisonedWindows",
                    str(poisoned_windows),
                    "-PoisonedProgramFiles",
                    str(poisoned_program_files),
                    "-PoisonedProgramFilesX86",
                    str(poisoned_program_files_x86),
                    "-PoisonedVCTargetsPath",
                    str(poisoned_vc_targets),
                    "-PoisonedUserRoot",
                    str(poisoned_user_root),
                ],
                cwd=temp,
                env=environment,
                capture_output=True,
                text=True,
                encoding="utf-8",
                errors="replace",
                timeout=120,
                check=False,
            )
            self.assertEqual(
                result.returncode,
                0,
                msg=f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}",
            )

            invocations = [
                json.loads(line)
                for line in argument_log.read_text(encoding="utf-8").splitlines()
            ]
            expected_projects = (
                "Common.vcxproj",
                "HibikiEQAPODriver\\HibikiEQAPODriver.vcxproj",
                "HibikiEQAPODriver\\Tests\\AsioProxyCoreTests.vcxproj",
                "HibikiEQAPODriver\\Tests\\AsioProxyDriverTests.vcxproj",
                "MonitorVST3\\MonitorVST3.vcxproj",
                "EqualizerAPO\\EqualizerAPO.vcxproj",
                "EqApoOutProcHost\\EqApoOutProcHost.vcxproj",
                "Benchmark\\Benchmark.vcxproj",
                "VoicemeeterClient\\VoicemeeterClient.vcxproj",
            )
            self.assertEqual(
                tuple(record["args"][0] for record in invocations),
                expected_projects,
            )
            for index, record in enumerate(invocations):
                invocation = record["args"]
                with self.subTest(project=invocation[0] if invocation else None):
                    self.assertGreaterEqual(len(invocation), 35)
                    self.assertIn("/t:Rebuild", invocation)
                    if index == 0:
                        self.assertEqual(invocation[0], "Common.vcxproj")
                        self.assertNotIn(
                            "/p:BuildProjectReferences=false", invocation
                        )
                    else:
                        self.assertIn(
                            "/p:BuildProjectReferences=false", invocation
                        )
                    self.assertTrue(
                        all(
                            value is None
                            for value in record["blocked_environment"].values()
                        ),
                        record["blocked_environment"],
                    )
                    include_argument = next(
                        argument
                        for argument in invocation
                        if argument.startswith("/p:LIBSNDFILE_INCLUDE=")
                    )
                    tclap_argument = next(
                        argument
                        for argument in invocation
                        if argument.startswith("/p:TCLAP_ROOT=")
                    )
                    vst3_sdk_argument = next(
                        argument
                        for argument in invocation
                        if argument.startswith("/p:VST3_SDK_ROOT=")
                    )
                    monitor_static_argument = next(
                        argument
                        for argument in invocation
                        if argument.startswith("/p:MONITOR_STATIC_LIB_DIR=")
                    )
                    monitor_vc_runtime_argument = next(
                        argument
                        for argument in invocation
                        if argument.startswith("/p:MONITOR_VC_RUNTIME_DIR=")
                    )
                    self.assertIn(
                        "repository path with spaces & metachar", include_argument
                    )
                    self.assertGreater(len(include_argument), 200)
                    self.assertTrue(
                        include_argument.endswith(
                            "\\third_party\\vcpkg_installed"
                            "\\x64-windows\\include"
                        )
                    )
                    self.assertIn(
                        "repository path with spaces & metachar", tclap_argument
                    )
                    self.assertTrue(
                        tclap_argument.endswith("\\third_party\\tclap")
                    )
                    self.assertIn(
                        "repository path with spaces & metachar", vst3_sdk_argument
                    )
                    self.assertTrue(
                        vst3_sdk_argument.endswith("\\third_party\\vst3sdk")
                    )
                    self.assertTrue(
                        monitor_static_argument.endswith(
                            "\\third_party\\monitor_vcpkg_installed"
                            "\\x64-windows-static-md\\lib"
                        )
                    )
                    self.assertIn("Microsoft.VC", monitor_vc_runtime_argument)
                    self.assertIn("/p:ImportDirectoryBuildProps=false", invocation)
                    self.assertIn("/p:ImportDirectoryBuildTargets=false", invocation)
                    self.assertTrue(
                        any(
                            argument.startswith("/p:VCTargetsPath=")
                            and str(poisoned_vc_targets) not in argument
                            for argument in invocation
                        )
                    )
                    self.assertTrue(
                        any(
                            argument.startswith("/p:UserRootDir=")
                            and str(poisoned_user_root) not in argument
                            for argument in invocation
                        )
                    )
                    self.assertIn("/p:VCLibPackagePath=", invocation)
                    self.assertIn("/noAutoResponse", invocation)
                    self.assertIn("/nodeReuse:false", invocation)
                    self.assertNotIn("path", invocation)
                    self.assertNotIn("with", invocation)
                    self.assertNotIn("spaces", invocation)
        finally:
            # RemoveDirectory on a junction removes only the junction itself; do
            # this before cleaning its parent so cleanup can never traverse ROOT.
            if junction_created and os.path.lexists(junction):
                os.rmdir(junction)
            if not os.path.lexists(junction):
                shutil.rmtree(temp, ignore_errors=True)
            for directory in sorted(
                created_dependency_directories,
                key=lambda path: len(path.parts),
                reverse=True,
            ):
                try:
                    directory.rmdir()
                except OSError:
                    # Preserve anything another process populated concurrently.
                    pass


if __name__ == "__main__":
    unittest.main()
