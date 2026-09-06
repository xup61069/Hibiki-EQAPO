#!/usr/bin/env python3
"""Compatibility-safe user-facing branding contracts."""

from __future__ import annotations

import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]


def read(relative_path: str, encoding: str = "utf-8") -> str:
    return (ROOT / relative_path).read_text(encoding=encoding)


def old_brand_lines(relative_path: str, encoding: str = "utf-8") -> list[str]:
    return [
        line
        for line in read(relative_path, encoding).splitlines()
        if "Equalizer APO" in line
    ]


class BrandingContractTests(unittest.TestCase):
    def test_solution_and_public_surfaces_use_hibiki_name(self) -> None:
        self.assertTrue((ROOT / "HibikiEQAPO.sln").is_file())
        self.assertFalse((ROOT / "EqualizerAPO.sln").exists())

        for relative_path in (
            "README.md",
            "README.en.md",
            "README_zh-TW.md",
            "CONTRIBUTING.md",
            "SECURITY.md",
            ".github/ISSUE_TEMPLATE/bug_report.md",
            ".github/ISSUE_TEMPLATE/feature_request.md",
            ".github/PULL_REQUEST_TEMPLATE.md",
        ):
            with self.subTest(path=relative_path):
                self.assertIn("Hibiki EQAPO", read(relative_path))

    def test_windows_ui_and_version_metadata_use_hibiki_name(self) -> None:
        utf8_surfaces = (
            "Editor/MainWindow.cpp",
            "Editor/Editor.rc",
            "DeviceSelector/DeviceSelector.cpp",
            "DeviceSelector/DeviceSelector.ui",
            "DeviceSelector/DeviceTestDialog.ui",
            "UpdateChecker/UpdateChecker.cpp",
            "UpdateChecker/UpdateChecker.ui",
            "UpdateChecker/main.cpp",
            "VoicemeeterClient/VoicemeeterClient.cpp",
            "MonitorVST3/MonitorVST3Factory.cpp",
        )
        for relative_path in utf8_surfaces:
            with self.subTest(path=relative_path):
                self.assertIn("Hibiki EQAPO", read(relative_path))

        for relative_path in (
            "DeviceSelector/DeviceSelector.rc",
            "UpdateChecker/UpdateChecker.rc",
            "VoicemeeterClient/VoicemeeterClient.rc",
            "EqualizerAPO/EqualizerAPO.rc",
            "EqualizerAPO/EqualizerAPO64.rc",
            "EqualizerAPO/EqualizerAPO-ARM64.rc",
        ):
            resource = read(relative_path, "utf-16")
            with self.subTest(path=relative_path):
                self.assertIn('VALUE "ProductName", "Hibiki EQAPO', resource)
                self.assertIn('VALUE "FileDescription", "Hibiki EQAPO', resource)

        driver_resource = read("HibikiEQAPODriver/HibikiEQAPODriver.rc")
        for token in (
            'VALUE "ProductName", "Hibiki EQAPO"',
            'VALUE "FileDescription", "Hibiki EQAPO"',
            'VALUE "InternalName", "HibikiEQAPODriver.dll"',
            'VALUE "OriginalFilename", "HibikiEQAPODriver.dll"',
        ):
            self.assertIn(token, driver_resource)

    def test_release_artifacts_use_filesystem_safe_brand_slug(self) -> None:
        build = read(".github/workflows/build.yml")
        release = read(".github/workflows/release.yml")
        for workflow in (build, release):
            self.assertIn("Hibiki-EQAPO", workflow)
            self.assertNotIn("Setup/EqualizerAPO-x64-", workflow)

    def test_installer_current_user_copy_uses_hibiki_name(self) -> None:
        setup = read("Setup/Setup.nsi")
        self.assertIn("Run Hibiki EQAPO Device Selector from the Start Menu", setup)
        self.assertNotIn("Run Equalizer APO Device Selector from the Start Menu", setup)

        allowed_compatibility_lines = (
            "unofficial Equalizer APO fork",
            '"Equalizer APO contributors"',
            "Equalizer APO Configuration Editor.lnk",
            "Equalizer APO Device Selector.lnk",
            '$0 == "Equalizer APO "',
            '$0 == "Loudness Correction for Equalizer APO "',
            '$OldStartMenuFolder == "Equalizer APO"',
            '$OldStartMenuFolder == "Loudness Correction for Equalizer APO"',
        )
        for line in old_brand_lines("Setup/Setup.nsi"):
            with self.subTest(line=line):
                self.assertTrue(
                    any(token in line for token in allowed_compatibility_lines),
                    f"Non-allowlisted current installer branding: {line}",
                )

    def test_equalizer_apo_compatibility_identifiers_remain_stable(self) -> None:
        solution = read("HibikiEQAPO.sln", "utf-8-sig")
        monitor_identity = read("MonitorVST3/MonitorVST3Identity.h")
        setup = read("Setup/Setup.nsi")

        self.assertIn('"EqualizerAPO\\EqualizerAPO.vcxproj"', solution)
        self.assertIn('L"Hibiki EQAPO Monitor VST3"', monitor_identity)
        self.assertIn('L"DAW Monitor Insert Hibiki EQAPO Monitor VST3"', monitor_identity)
        self.assertIn('!define REGPATH "Software\\EqualizerAPO"', setup)
        self.assertIn('$PROGRAMFILES64\\EqualizerAPO', setup)

        for relative_path in (
            "EqualizerAPO/EqualizerAPO.rc",
            "EqualizerAPO/EqualizerAPO64.rc",
            "EqualizerAPO/EqualizerAPO-ARM64.rc",
        ):
            resource = read(relative_path, "utf-16")
            with self.subTest(path=relative_path):
                self.assertIn('VALUE "InternalName", "EqualizerAPO.dll"', resource)
                self.assertIn('VALUE "OriginalFilename", "EqualizerAPO.dll"', resource)

    def test_current_runtime_identity_has_no_old_user_facing_name(self) -> None:
        exact_contracts = {
            "EqualizerAPO/EqualizerAPO.cpp": (
                'EQUALIZERAPO_POST_MIX_GUID, L"Hibiki EQAPO"',
                'EQUALIZERAPO_PRE_MIX_GUID, L"Hibiki EQAPO"',
            ),
            "EqualizerAPO/DllMain.cpp": (
                'L"Hibiki EQAPO Post-Mix Class"',
                'L"Hibiki EQAPO Pre-Mix Class"',
            ),
            "DeviceAPOInfo.cpp": (
                'fxTitleValueName, L"Hibiki EQAPO"',
                "newer Hibiki EQAPO version",
            ),
            "helpers/VSTPluginInstance.cpp": (
                'L"Hibiki EQAPO", _TRUNCATE',
                '"Hibiki EQAPO");',
            ),
            "Benchmark/Benchmark.cpp": (
                "Hibiki EQAPO filter configuration",
            ),
            "vcpkg.json": ('"name": "hibiki-eqapo"',),
            "UpdateChecker/main.cpp": (
                'QString("Hibiki-EQAPO-UpdateChecker/%1")',
            ),
            "IRs/README.md": ("active Hibiki EQAPO configuration file",),
        }
        for relative_path, tokens in exact_contracts.items():
            source = read(relative_path)
            for token in tokens:
                with self.subTest(path=relative_path, token=token):
                    self.assertIn(token, source)

        no_old_brand_surfaces = (
            "Editor/MainWindow.cpp",
            "Editor/guis/LoudnessCorrectionFilterGUI.ui",
            "DeviceSelector/DeviceSelector.cpp",
            "DeviceSelector/DeviceSelector.ui",
            "DeviceSelector/DeviceTestDialog.cpp",
            "DeviceAPOInfo.cpp",
            "Benchmark/Benchmark.cpp",
        )
        for relative_path in no_old_brand_surfaces:
            with self.subTest(path=relative_path):
                self.assertFalse(old_brand_lines(relative_path))

        vst_host_lines = old_brand_lines("helpers/VSTPluginInstance.cpp")
        self.assertEqual(
            vst_host_lines,
            ["    This file is part of Equalizer APO, a system-wide equalizer."],
        )

    def test_voicemeeter_startup_link_migrates_to_hibiki_name(self) -> None:
        source = read("VoicemeeterAPOInfo.cpp")
        self.assertIn(
            'startupFilename = L"Hibiki EQAPO Voicemeeter Client.lnk"',
            source,
        )
        self.assertIn(
            'legacyStartupFilename = L"Equalizer APO Voicemeeter Client.lnk"',
            source,
        )
        self.assertIn("getStartupLinkArgs", source)
        self.assertIn("DeleteFileW(getLegacyStartupPath().c_str())", source)

        self.assertEqual(
            old_brand_lines("VoicemeeterAPOInfo.cpp"),
            [
                'static const wchar_t* legacyStartupFilename = L"Equalizer APO Voicemeeter Client.lnk";'
            ],
        )

    def test_unpublished_monitor_uses_one_hibiki_technical_identity(self) -> None:
        project = read("MonitorVST3/MonitorVST3.vcxproj")
        manager = read("scripts/manage-monitor-vst3.ps1", "utf-8-sig")
        stage = read("scripts/stage-installer-x64.ps1")
        binary_module = read("scripts/MonitorVST3Binary.psm1", "utf-8-sig")

        for source in (project, manager, stage, binary_module):
            self.assertNotIn("EqualizerAPOMonitor", source)
        self.assertIn("<TargetName>HibikiEQAPOMonitor</TargetName>", project)
        self.assertIn("HibikiEQAPOMonitor.vst3", manager)
        self.assertIn('product = "HibikiEQAPOMonitor"', manager)
        self.assertIn("hibiki-eqapo-monitor.json", manager)
        self.assertIn("HibikiEQAPOMonitor.vst3", stage)
        self.assertIn("HibikiEQAPOMonitor.vst3", binary_module)

        identity = read("MonitorVST3/MonitorVST3Identity.h")
        self.assertNotIn("Equalizer APO Monitor", identity)
        self.assertIn("Hibiki EQAPO Monitor VST3", identity)

    def test_asio_trademark_copy_is_precise_and_not_a_driver_name(self) -> None:
        for relative_path in ("README.md", "README.en.md", "NOTICE.md"):
            public_text = read(relative_path)
            with self.subTest(path=relative_path):
                self.assertIn(
                    "Hibiki EQAPO is compatible with ASIO® technology.", public_text
                )
                self.assertIn(
                    "ASIO is a registered trademark of Steinberg Media Technologies GmbH.",
                    public_text,
                )
                self.assertNotIn("Hibiki EQAPO ASIO", public_text)


if __name__ == "__main__":
    unittest.main()
