#!/usr/bin/env python3
"""Regression contracts for Mixomo build wiring and installer payloads."""

from __future__ import annotations

import pathlib
import re
import shutil
import subprocess
import tempfile
import unittest
import xml.etree.ElementTree as ET


ROOT = pathlib.Path(__file__).resolve().parents[1]
MSBUILD = {"m": "http://schemas.microsoft.com/developer/msbuild/2003"}
COMMON_PROJECT_GUID = "{6758B50E-B9F0-4389-A61D-A842F0545E1B}"


def read(relative_path: str) -> str:
    return (ROOT / relative_path).read_text(encoding="utf-8")


def project_items(relative_path: str, kind: str) -> set[str]:
    root = ET.parse(ROOT / relative_path).getroot()
    return {
        node.attrib["Include"]
        for node in root.findall(f".//m:{kind}", MSBUILD)
        if "Include" in node.attrib
    }


class BuildPackageWiringTests(unittest.TestCase):
    def test_outproc_host_depends_on_common_in_solution(self) -> None:
        solution = read("HibikiEQAPO.sln")
        project_start = solution.index(
            'Project("{8BC9CEB8-8B4A-11D0-8D11-00A0C91BC942}") = '
            '"EqApoOutProcHost"'
        )
        project_end = re.search(
            r"^EndProject\s*$", solution[project_start:], flags=re.MULTILINE
        )
        self.assertIsNotNone(project_end)
        project_block = solution[
            project_start : project_start + project_end.end()
        ]
        self.assertIn("ProjectSection(ProjectDependencies) = postProject", project_block)
        self.assertIn(
            f"{COMMON_PROJECT_GUID} = {COMMON_PROJECT_GUID}", project_block
        )

    def test_muparserx_paths_and_library_name_match_bootstrap_output(self) -> None:
        bootstrap = read("scripts/bootstrap-third-party.ps1")
        self.assertIn('"build\\muparserx-$Triplet"', bootstrap)
        self.assertIn('"$Configuration\\muparserx.lib"', bootstrap)

        consumers = (
            "Common.vcxproj",
            "Benchmark/Benchmark.vcxproj",
            "EqualizerAPO/EqualizerAPO.vcxproj",
            "VoicemeeterClient/VoicemeeterClient.vcxproj",
            "EqApoOutProcHost/EqApoOutProcHost.vcxproj",
            "Editor/Editor.pro",
        )
        for relative_path in consumers:
            content = read(relative_path)
            self.assertNotIn("muparserxd.lib", content, relative_path)
            self.assertNotIn("third_party\\muparserx\\build", content, relative_path)

        for relative_path in consumers[1:]:
            self.assertIn("muparserx.lib", read(relative_path), relative_path)

        editor = read("Editor/Editor.pro")
        self.assertIn("muparserx-x64-windows/Debug", editor)
        self.assertIn("muparserx-x64-windows/Release", editor)

    def test_common_filters_cover_all_compiled_sources_and_headers(self) -> None:
        for kind in ("ClInclude", "ClCompile"):
            project = project_items("Common.vcxproj", kind)
            filters = project_items("Common.vcxproj.filters", kind)
            self.assertEqual(set(), project - filters, f"missing {kind} filter entries")

        host_headers = project_items(
            "EqApoOutProcHost/EqApoOutProcHost.vcxproj", "ClInclude"
        )
        self.assertIn("..\\outproc\\OutProcAudioProtocol.h", host_headers)
        self.assertIn("..\\outproc\\OutProcVSTConfig.h", host_headers)

    def test_public_staging_excludes_unreviewed_datasets(self) -> None:
        staging = read("scripts/stage-installer-x64.ps1")
        for token in (
            "$headphoneCalSrc",
            "$stagedHeadphoneCalCatalog",
            "$irSrc",
            "$expectedImpulseResponseCount",
            "$stagedIrMetadata",
        ):
            self.assertNotIn(token, staging)

        setup = read("Setup/Setup.nsi")
        self.assertNotIn('File /r "${LIBPATH}\\IRs\\*"', setup)
        self.assertNotIn(
            'File /r "${LIBPATH}\\HeadphoneCalibrations\\*"', setup
        )
        self.assertNotIn('RequireInstalledAsset "$INSTDIR\\IRs', setup)
        self.assertNotIn(
            'RequireInstalledAsset "$INSTDIR\\HeadphoneCalibrations', setup
        )

    def test_release_history_guard_is_wired_into_full_checkouts(self) -> None:
        guard = read("scripts/test-public-history.ps1")
        self.assertIn("90148767f6363c3b7de5cd22082bb450876d3798", guard)
        self.assertNotIn("git merge-base", guard)
        self.assertIn("$reachableCommits = @(& git rev-list $Revision)", guard)
        self.assertIn("git -c core.quotePath=false rev-list --objects", guard)
        self.assertIn('path -ne "IRs/README.md"', guard)
        self.assertIn(
            'path -ne "resources/HeadphoneCalibrations/README.md"', guard
        )

        for workflow in (
            ".github/workflows/build.yml",
            ".github/workflows/release.yml",
        ):
            content = read(workflow)
            self.assertIn("fetch-depth: 0", content, workflow)
            self.assertIn("test-public-history.ps1 -Revision HEAD", content, workflow)

    def test_public_history_guard_works_without_rejected_object(self) -> None:
        powershell = shutil.which("powershell")
        git = shutil.which("git")
        if powershell is None or git is None:
            self.skipTest("PowerShell and Git are required for the history guard")

        with tempfile.TemporaryDirectory() as directory:
            repository = pathlib.Path(directory)
            scripts = repository / "scripts"
            scripts.mkdir()
            shutil.copy2(ROOT / "scripts/test-public-history.ps1", scripts)
            (repository / "README.md").write_text("fixture\n", encoding="utf-8")
            subprocess.run([git, "init", "-q"], cwd=repository, check=True)
            subprocess.run([git, "add", "README.md"], cwd=repository, check=True)
            subprocess.run(
                [
                    git,
                    "-c",
                    "user.name=History Guard Test",
                    "-c",
                    "user.email=history-guard@example.invalid",
                    "commit",
                    "-q",
                    "-m",
                    "fixture",
                ],
                cwd=repository,
                check=True,
            )
            result = subprocess.run(
                [
                    powershell,
                    "-NoProfile",
                    "-ExecutionPolicy",
                    "Bypass",
                    "-File",
                    str(scripts / "test-public-history.ps1"),
                    "-Revision",
                    "HEAD",
                ],
                cwd=repository,
                capture_output=True,
                text=True,
            )
            self.assertEqual(
                0,
                result.returncode,
                result.stdout + result.stderr,
            )

    def test_public_history_guard_rejects_unicode_asset_paths(self) -> None:
        powershell = shutil.which("powershell")
        git = shutil.which("git")
        if powershell is None or git is None:
            self.skipTest("PowerShell and Git are required for the history guard")

        with tempfile.TemporaryDirectory() as directory:
            repository = pathlib.Path(directory)
            scripts = repository / "scripts"
            scripts.mkdir()
            shutil.copy2(ROOT / "scripts/test-public-history.ps1", scripts)
            ir_directory = repository / "IRs"
            ir_directory.mkdir()
            (ir_directory / "測試.wav").write_bytes(b"not an impulse response")
            subprocess.run([git, "init", "-q"], cwd=repository, check=True)
            subprocess.run([git, "add", "IRs/測試.wav"], cwd=repository, check=True)
            subprocess.run(
                [
                    git,
                    "-c",
                    "user.name=History Guard Test",
                    "-c",
                    "user.email=history-guard@example.invalid",
                    "commit",
                    "-q",
                    "-m",
                    "forbidden unicode fixture",
                ],
                cwd=repository,
                check=True,
            )
            result = subprocess.run(
                [
                    powershell,
                    "-NoProfile",
                    "-ExecutionPolicy",
                    "Bypass",
                    "-File",
                    str(scripts / "test-public-history.ps1"),
                    "-Revision",
                    "HEAD",
                ],
                cwd=repository,
                capture_output=True,
                text=True,
            )
            self.assertNotEqual(0, result.returncode, result.stdout + result.stderr)
            self.assertIn("Third-party calibration", result.stdout + result.stderr)

    def test_uninstall_never_deletes_user_supplied_audio_data(self) -> None:
        setup = read("Setup/Setup.nsi")
        self.assertNotIn("RemoveBundledAssetTreeSafely", setup)
        self.assertNotIn(
            'DeleteTransactionFile "$INSTDIR\\HeadphoneCalibrations', setup
        )
        self.assertNotIn('RMDir "$INSTDIR\\HeadphoneCalibrations"', setup)
        self.assertNotIn('RMDir "$INSTDIR\\IRs"', setup)


if __name__ == "__main__":
    unittest.main()
