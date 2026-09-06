#!/usr/bin/env python3
"""Contracts that keep the feature-complete Mixomo integration intact."""

from __future__ import annotations

import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]


def read(relative_path: str) -> str:
    return (ROOT / relative_path).read_text(encoding="utf-8")


class MixomoFeatureRetentionTests(unittest.TestCase):
    RUNTIME_FEATURES = (
        "ParametricEQ",
        "OutputGuard",
        "Pan",
        "Crossfeed",
        "Chorus",
        "Reverb",
        "ToneGenerator",
        "VUMeter",
        "HeadphoneCalibration",
        "OutProcGain",
        "OutProcBiquad",
        "OutProcVSTPlugin",
    )

    def test_runtime_factories_remain_registered(self) -> None:
        source = read("FilterEngine.cpp")
        self.assertIn("std::unique_ptr<IFilterFactory> owner(factory);", source)
        self.assertIn("owner.release();", source)
        for feature in self.RUNTIME_FEATURES:
            self.assertIn(f'#include "filters/{feature}FilterFactory.h"', source)
            self.assertRegex(
                source,
                rf"addFactory\(new {feature}FilterFactory\(\)(?:, false)?\);",
            )

    def test_feature_sources_and_editor_factories_remain_present(self) -> None:
        required_paths = (
            "EqApoOutProcHost/EqApoOutProcHost.cpp",
            "EqApoOutProcHost/EqApoOutProcHost.vcxproj",
            "outproc/OutProcAudioProtocol.h",
            "outproc/OutProcVSTConfig.h",
            "filters/AudioToolsHelper.h",
            "filters/ParametricEQFilter.cpp",
            "filters/HeadphoneCalibrationFilter.cpp",
            "filters/OutProcVSTPluginFilter.cpp",
            "Editor/guis/AudioToolFilterGUIFactory.cpp",
            "Editor/guis/ParametricEQFilterGUIFactory.cpp",
            "Editor/guis/HeadphoneCalibrationFilterGUIFactory.cpp",
        )
        for relative_path in required_paths:
            self.assertTrue((ROOT / relative_path).is_file(), relative_path)

    def test_feature_complete_editor_entries_remain_registered(self) -> None:
        filter_table = read("Editor/FilterTable.cpp")
        for factory in (
            "PanFilterGUIFactory",
            "CrossfeedFilterGUIFactory",
            "ChorusFilterGUIFactory",
            "ReverbFilterGUIFactory",
            "ToneGeneratorFilterGUIFactory",
            "VUMeterFilterGUIFactory",
            "HeadphoneCalibrationFilterGUIFactory",
            "ParametricEQFilterGUIFactory",
        ):
            self.assertIn(f"factories.append(new {factory});", filter_table)

        vst_gui_factory = read("Editor/guis/VSTPluginFilterGUIFactory.cpp")
        self.assertIn("Out-of-process VST plugin", vst_gui_factory)
        self.assertIn('command == "OutProcVSTPlugin"', vst_gui_factory)

    def test_visual_studio_and_qt_projects_keep_feature_wiring(self) -> None:
        common_project = read("Common.vcxproj")
        editor_project = read("Editor/Editor.pro")
        solution = read("HibikiEQAPO.sln")

        for feature in self.RUNTIME_FEATURES:
            self.assertIn(f"filters\\{feature}FilterFactory.cpp", common_project)

        for token in (
            "guis/AudioToolFilterGUIFactory.cpp",
            "guis/ParametricEQFilterGUIFactory.cpp",
            "guis/HeadphoneCalibrationFilterGUIFactory.cpp",
        ):
            self.assertIn(token, editor_project)

        self.assertIn(
            '"EqApoOutProcHost", "EqApoOutProcHost\\EqApoOutProcHost.vcxproj"',
            solution,
        )

    def test_installer_stages_host_without_third_party_datasets(self) -> None:
        staging = read("scripts/stage-installer-x64.ps1")
        setup = read("Setup/Setup.nsi")
        self.assertIn("EqApoOutProcHost.exe", staging)
        self.assertIn("EqApoOutProcHost.exe", setup)
        self.assertNotIn('$headphoneCalSrc =', staging)
        self.assertNotIn('$irSrc =', staging)
        self.assertNotIn(
            'File /r "${LIBPATH}\\HeadphoneCalibrations\\*"', setup
        )
        self.assertNotIn('File /r "${LIBPATH}\\IRs\\*"', setup)
        self.assertNotIn(
            'RequireInstalledAsset "$INSTDIR\\HeadphoneCalibrations', setup
        )
        self.assertNotIn('RequireInstalledAsset "$INSTDIR\\IRs', setup)

    def test_user_supplied_calibration_and_ir_paths_remain_supported(self) -> None:
        calibration = read("Editor/guis/HeadphoneCalibrationFilterGUIFactory.cpp")
        convolution = read("Editor/guis/ConvolutionFilterGUI.cpp")
        self.assertIn(
            '"HeadphoneCalibrations/ash_hpcf_catalog.json"', calibration
        )
        self.assertIn("filterTable->getConfigPath()", calibration)
        self.assertIn('absoluteFilePath("IRs")', convolution)
        self.assertIn("QFileInfo(configPath).absoluteDir()", convolution)
        self.assertNotIn("QCoreApplication::applicationDirPath()", calibration)
        self.assertNotIn("QCoreApplication::applicationDirPath()", convolution)
        self.assertIn("No headphone-measurement dataset is bundled", calibration)
        self.assertIn("Use only files that you are licensed to use", convolution)

        ignore = read(".gitignore")
        self.assertIn("/IRs/*", ignore)
        self.assertIn("/resources/HeadphoneCalibrations/*", ignore)
        self.assertTrue((ROOT / "IRs/README.md").is_file())
        self.assertTrue(
            (ROOT / "resources/HeadphoneCalibrations/README.md").is_file()
        )

    def test_readme_identifies_feature_complete_line_and_requested_title(self) -> None:
        expected_titles = {
            "README.md": "# Hibiki EQAPO",
            "README.en.md": "# Hibiki EQAPO",
        }
        for relative_path, title in expected_titles.items():
            content = read(relative_path)
            self.assertEqual(content.splitlines()[0], title)
            self.assertIn("Mixomo", content)
            self.assertIn("`exp`", content)

        legacy_chinese_link = read("README_zh-TW.md")
        self.assertIn("[README.md](README.md)", legacy_chinese_link)


if __name__ == "__main__":
    unittest.main()
