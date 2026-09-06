#!/usr/bin/env python3
"""Regression contracts for the Windows-native Qt product experience."""

from __future__ import annotations

import pathlib
import re
import unittest
import xml.etree.ElementTree as ET


ROOT = pathlib.Path(__file__).resolve().parents[1]
EDITOR_MAIN = (ROOT / "Editor" / "main.cpp").read_text(encoding="utf-8")
MAIN_WINDOW = (ROOT / "Editor" / "MainWindow.cpp").read_text(encoding="utf-8")
MAIN_WINDOW_UI = (ROOT / "Editor" / "MainWindow.ui").read_text(encoding="utf-8")
MODERN_THEME = (ROOT / "Editor" / "ModernTheme.cpp").read_text(encoding="utf-8")
DEVICE_SELECTOR = (ROOT / "DeviceSelector" / "DeviceSelector.cpp").read_text(
    encoding="utf-8"
)
DEVICE_TEST_DIALOG = (
    ROOT / "DeviceSelector" / "DeviceTestDialog.cpp"
).read_text(encoding="utf-8")
UPDATE_CHECKER = (ROOT / "UpdateChecker" / "UpdateChecker.cpp").read_text(
    encoding="utf-8"
)
FREQUENCY_PLOT_VRULER = (
    ROOT / "Editor" / "widgets" / "FrequencyPlotVRuler.cpp"
).read_text(encoding="utf-8")
FREQUENCY_PLOT_HRULER = (
    ROOT / "Editor" / "widgets" / "FrequencyPlotHRuler.cpp"
).read_text(encoding="utf-8")
FREQUENCY_PLOT_VIEW = (
    ROOT / "Editor" / "widgets" / "FrequencyPlotView.cpp"
).read_text(encoding="utf-8")
FREQUENCY_PLOT_VIEW_HEADER = (
    ROOT / "Editor" / "widgets" / "FrequencyPlotView.h"
).read_text(encoding="utf-8")


class UiProductizationTests(unittest.TestCase):
    def test_qstring_arg_placeholders_start_at_one(self) -> None:
        # QString::arg() only recognizes %1 through %99. Keep this focused on
        # first-party Qt sources that format UI/config text so printf's %0 flag
        # elsewhere is not mistaken for a Qt placeholder.
        paths = (
            "DeviceSelector/DeviceSelector.cpp",
            "Editor/FilterTable.cpp",
            "Editor/FilterTableRow.cpp",
            "Editor/MainWindow.cpp",
            "Editor/widgets/FrequencyPlotHRuler.cpp",
            "Editor/widgets/FrequencyPlotVRuler.cpp",
            "Editor/guis/DelayFilterGUI.cpp",
            "Editor/guis/GraphicEQFilterGUI.cpp",
            "Editor/guis/LoudnessCorrectionFilterGUI.cpp",
            "Editor/guis/VSTPluginFilterGUI.cpp",
            "UpdateChecker/main.cpp",
            "UpdateChecker/UpdateChecker.cpp",
        )
        for relative_path in paths:
            with self.subTest(path=relative_path):
                source = (ROOT / relative_path).read_text(encoding="utf-8")
                self.assertNotIn("%0", source)

    def test_saved_window_state_cannot_merge_the_workspace_toolbar(self) -> None:
        header = (ROOT / "Editor" / "MainWindow.h").read_text(encoding="utf-8")
        self.assertIn("bool restoreWindowLayoutState(const QByteArray& state);", header)
        self.assertIn("QByteArray saveWindowLayoutState() const;", header)
        self.assertIn("static constexpr int WINDOW_LAYOUT_STATE_VERSION = 1;", MAIN_WINDOW)

        state_helpers = MAIN_WINDOW[
            MAIN_WINDOW.index("bool MainWindow::restoreWindowLayoutState") :
            MAIN_WINDOW.index("void MainWindow::setupTrayIcon")
        ]
        self.assertIn("restoreState(state, WINDOW_LAYOUT_STATE_VERSION)", state_helpers)
        self.assertIn("saveState(WINDOW_LAYOUT_STATE_VERSION)", state_helpers)
        self.assertNotIn("normalizeToolbarLayout", MAIN_WINDOW)

        load_preferences = MAIN_WINDOW[
            MAIN_WINDOW.index("void MainWindow::loadPreferences") :
            MAIN_WINDOW.index("void MainWindow::savePreferences")
        ]
        self.assertIn(
            "restoreWindowLayoutState(stateValue.toByteArray())", load_preferences
        )

        save_preferences = MAIN_WINDOW[
            MAIN_WINDOW.index("void MainWindow::savePreferences") :
            MAIN_WINDOW.index("void MainWindow::updateRecentFiles")
        ]
        self.assertIn('settings.setValue("windowState", saveWindowLayoutState())', save_preferences)

        snapshot_scenario = MAIN_WINDOW[
            MAIN_WINDOW.index("bool MainWindow::loadSnapshotScenario") :
            MAIN_WINDOW.index("bool MainWindow::snapshotLayoutIsValid")
        ]
        for token in (
            "const QByteArray currentWindowState = saveWindowLayoutState()",
            "const QByteArray legacyMergedToolbarState = saveState(0)",
            "restoreWindowLayoutState(currentWindowState)",
            "restoreWindowLayoutState(legacyMergedToolbarState)",
        ):
            with self.subTest(toolbar_round_trip=token):
                self.assertIn(token, snapshot_scenario)

    def test_compact_reset_buttons_keep_stretch_space_and_titles_at_top(self) -> None:
        cases = (
            ("PreampFilterGUI", "preampResetButton", "label", 6, 5),
            ("DelayFilterGUI", "delayResetButton", "delayLabel", 4, 3),
        )
        for gui_name, reset_name, title_name, reset_column, spacer_column in cases:
            cpp = (ROOT / "Editor" / "guis" / f"{gui_name}.cpp").read_text(
                encoding="utf-8"
            )
            ui = ET.parse(
                ROOT / "Editor" / "guis" / f"{gui_name}.ui"
            ).getroot()
            layout = ui.find(".//layout[@name='gridLayout']")
            self.assertIsNotNone(layout)

            spacer_item = next(
                item
                for item in layout.findall("item")
                if item.find("spacer[@name='horizontalSpacer']") is not None
            )
            title = ui.find(f".//widget[@name='{title_name}']")
            self.assertIsNotNone(title)

            with self.subTest(gui=gui_name):
                self.assertEqual(int(spacer_item.attrib["column"]), spacer_column)
                self.assertEqual(
                    layout.attrib["columnstretch"].split(",")[spacer_column], "1"
                )
                self.assertIn(
                    "Qt::AlignTop",
                    title.findtext("property[@name='alignment']/set", ""),
                )
                self.assertIn(f'QStringLiteral("{reset_name}")', cpp)
                self.assertIn(
                    "setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed)", cpp
                )
                self.assertNotIn("removeItem(spacer)", cpp)
                self.assertNotIn("delete spacer", cpp)
                self.assertRegex(
                    cpp,
                    rf"addWidget\(\s*resetButton,\s*0,\s*{reset_column},\s*2,\s*1,"
                    r"\s*Qt::AlignRight \| Qt::AlignVCenter\)",
                )

        copy_cpp = (
            ROOT / "Editor" / "guis" / "CopyFilterGUI.cpp"
        ).read_text(encoding="utf-8")
        copy_ui = ET.parse(
            ROOT / "Editor" / "guis" / "CopyFilterGUI.ui"
        ).getroot()
        copy_tab_item = next(
            item
            for item in copy_ui.findall(".//layout[@name='gridLayout']/item")
            if item.find("widget[@name='tabWidget']") is not None
        )
        self.assertEqual(copy_tab_item.attrib.get("colspan"), "2")
        self.assertIn('QStringLiteral("copyResetButton")', copy_cpp)
        self.assertIn(
            "setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed)", copy_cpp
        )

        snapshot_scenario = MAIN_WINDOW[
            MAIN_WINDOW.index("bool MainWindow::loadSnapshotScenario") :
            MAIN_WINDOW.index("bool MainWindow::snapshotLayoutIsValid")
        ]
        snapshot_validation = MAIN_WINDOW[
            MAIN_WINDOW.index("bool MainWindow::snapshotLayoutIsValid") :
            MAIN_WINDOW.index("void MainWindow::getDeviceAndChannelMask")
        ]
        self.assertIn('QStringLiteral("Delay: 250 ms")', snapshot_scenario)
        self.assertIn('QStringLiteral("Copy: L=L R=R")', snapshot_scenario)
        for token in (
            'QStringLiteral("preampResetButton")',
            'QStringLiteral("delayResetButton")',
            'QStringLiteral("copyResetButton")',
            "resetRect.intersects(editorRect)",
            "resetRect.intersects(titleRect)",
            "tabsRect.right() < resetRect.right()",
            "title->alignment() & Qt::AlignTop",
        ):
            with self.subTest(runtime_geometry_contract=token):
                self.assertIn(token, snapshot_validation)

        compact_sources = {
            "AudioToolFilterGUIFactory.cpp": (
                'commandName + QStringLiteral("ResetButton")',
                'QStringLiteral("VUMeterReadingResetButton")',
            ),
            "BiQuadFilterGUI.cpp": ('QStringLiteral("biQuadResetButton")',),
            "ConvolutionFilterGUI.cpp": (
                'QStringLiteral("convolutionResetButton")',
            ),
            "LoudnessCorrectionFilterGUI.cpp": (
                'QStringLiteral("loudnessResetButton")',
            ),
        }
        for file_name, object_names in compact_sources.items():
            source = (ROOT / "Editor" / "guis" / file_name).read_text(
                encoding="utf-8"
            )
            with self.subTest(compact_reset_source=file_name):
                for object_name in object_names:
                    self.assertIn(object_name, source)
                self.assertIn(
                    "setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed)",
                    source,
                )

        layout_contract = MAIN_WINDOW[
            MAIN_WINDOW.index("bool MainWindow::snapshotLayoutIsValid") :
            MAIN_WINDOW.index("void MainWindow::getDeviceAndChannelMask")
        ]
        self.assertIn("toolBarArea(ui->mainToolBar) != Qt::TopToolBarArea", layout_contract)
        self.assertIn("toolBarArea(workspaceToolBar) != Qt::TopToolBarArea", layout_contract)
        self.assertIn("!toolBarBreak(workspaceToolBar)", layout_contract)

    def test_all_qt_apps_install_the_shared_windows_theme(self) -> None:
        for relative_path in (
            "Editor/main.cpp",
            "DeviceSelector/main.cpp",
            "UpdateChecker/main.cpp",
        ):
            source = (ROOT / relative_path).read_text(encoding="utf-8")
            with self.subTest(app=relative_path):
                self.assertIn("ModernTheme::install", source)
                self.assertIn("UiSnapshot::schedule", source)

    def test_snapshot_hook_is_compiled_only_for_explicit_test_builds(self) -> None:
        snapshot = (ROOT / "helpers" / "UiSnapshot.h").read_text(encoding="utf-8")
        build_script = (
            ROOT / "scripts" / "build-qt-apps-x64.ps1"
        ).read_text(encoding="utf-8")
        capture_script = (
            ROOT / "scripts" / "capture-ui-regression.ps1"
        ).read_text(encoding="utf-8")

        self.assertIn("#ifdef EQAPO_ENABLE_UI_SNAPSHOTS", snapshot)
        test_build, production_build = snapshot.split("#else", 1)
        production_build = production_build.split("#endif", 1)[0]
        self.assertIn('qEnvironmentVariable("EQAPO_UI_SNAPSHOT")', test_build)
        self.assertIn('"EQAPO_UI_SNAPSHOT_SCENARIO"', test_build)
        self.assertIn('QStringLiteral("restored-tools")', test_build)
        self.assertIn('"EQAPO_UI_SNAPSHOT_LOCALE"', test_build)
        self.assertIn("widget.setAttribute(Qt::WA_DontShowOnScreen, true)", test_build)
        self.assertIn('QGuiApplication::platformName() != QStringLiteral("windows")', test_build)
        self.assertIn("metrics.inFontUcs4('A')", test_build)
        self.assertIn("metrics.inFontUcs4('0')", test_build)
        self.assertIn("metrics.inFontUcs4(0x6e2c)", test_build)
        self.assertIn("validator && !validator()", test_build)
        self.assertNotIn("EQAPO_UI_SNAPSHOT", production_build)
        self.assertNotIn("WA_DontShowOnScreen", production_build)
        self.assertRegex(
            production_build,
            r"inline QString outputPath\(\)\s*\{\s*return QString\(\);\s*\}",
        )
        self.assertRegex(
            production_build,
            r"inline bool requested\(\)\s*\{\s*return false;\s*\}",
        )

        self.assertIn("[switch]$EnableUiSnapshots", build_script)
        self.assertEqual(
            build_script.count("DEFINES+=EQAPO_ENABLE_UI_SNAPSHOTS"),
            1,
        )
        define_block = build_script[
            build_script.index("$snapshotDefine = if ($EnableUiSnapshots)") :
            build_script.index("$cmdParts = @(")
        ]
        self.assertIn('" DEFINES+=EQAPO_ENABLE_UI_SNAPSHOTS"', define_block)
        self.assertRegex(define_block, r"else\s*\{\s*\"\"\s*\}")
        self.assertIn("$snapshotDefine", build_script)
        self.assertRegex(
            capture_script,
            r"build-qt-apps-x64\.ps1[\s\S]*?-EnableUiSnapshots",
        )

        for project in (
            "Editor/Editor.pro",
            "DeviceSelector/DeviceSelector.pro",
            "UpdateChecker/UpdateChecker.pro",
        ):
            with self.subTest(project=project):
                self.assertNotIn(
                    "EQAPO_ENABLE_UI_SNAPSHOTS",
                    (ROOT / project).read_text(encoding="utf-8"),
                )

    def test_editor_does_not_disable_qt_per_monitor_dpi_scaling(self) -> None:
        self.assertNotIn('qputenv("QT_ENABLE_HIGHDPI_SCALING", "0")', EDITOR_MAIN)
        self.assertIn("QT_SCALE_FACTOR", (ROOT / "scripts" / "capture-ui-regression.ps1").read_text(encoding="utf-8"))

    def test_visual_regression_matrix_covers_three_apps_themes_and_scales(self) -> None:
        script = (ROOT / "scripts" / "capture-ui-regression.ps1").read_text(
            encoding="utf-8"
        )
        workflow = (ROOT / ".github" / "workflows" / "build.yml").read_text(
            encoding="utf-8"
        )
        for value in ("Editor", "DeviceSelector", "UpdateChecker"):
            self.assertIn(value, script)
        for value in ("light", "dark", "high-contrast"):
            self.assertIn(value, script)
        for value in ('"1.0"', '"1.25"', '"1.5"', '"1.75"', '"2.0"'):
            self.assertIn(value, script)
        self.assertIn('"text-150"', script)
        self.assertIn('"1.5"', script)
        self.assertIn("EQAPO_UI_FONT_SCALE", script)
        self.assertIn('EnvironmentVariables["QT_QPA_PLATFORM"] = "windows"', script)
        self.assertIn('"platforms\\qwindows$platformDebugSuffix.dll"', script)
        self.assertNotIn("qoffscreen", script)
        self.assertIn("function Invoke-UiSnapshotCapture", script)
        self.assertEqual(script.count("[System.Diagnostics.ProcessStartInfo]::new()"), 1)
        capture_function = script[
            script.index("function Invoke-UiSnapshotCapture") :
            script.index("$executables = @{}")
        ]
        self.assertEqual(capture_function.count("ReadToEndAsync()"), 2)
        self.assertNotIn(".ReadToEnd()", capture_function)
        self.assertLess(
            capture_function.index("ReadToEndAsync()"),
            capture_function.index("WaitForExit(25000)"),
        )
        self.assertIn("GetAwaiter().GetResult()", capture_function)
        self.assertNotIn("$process.Kill($true)", script)
        self.assertIn("$process.Kill()", script)
        self.assertIn("Expected exactly 90 UI snapshots", script)
        self.assertIn('FilePrefix = "editor-dense-zh-tw"', script)
        self.assertIn('SnapshotScenario = "dense"', script)
        self.assertIn('FilePrefix = "editor-restored-tools-zh-tw"', script)
        self.assertIn('SnapshotScenario = "restored-tools"', script)
        self.assertIn('Locale = "zh_TW"', script)
        self.assertIn("state.FilePrefix", script)
        self.assertIn(
            'EnvironmentVariables["EQAPO_UI_SNAPSHOT_SCENARIO"] = '
            "$State.SnapshotScenario",
            script,
        )
        self.assertNotIn('if ($state.SnapshotScenario -ne "")', script)
        self.assertIn("scenario.Label", script)
        self.assertIn("Remove-Item -LiteralPath $Target", script)
        self.assertIn('"manifest.json"', script)
        self.assertIn("sha256", script)
        for base_size in (
            "Editor = @{ Width = 1024; Height = 768 }",
            "DeviceSelector = @{ Width = 820; Height = 620 }",
            "UpdateChecker = @{ Width = 640; Height = 380 }",
        ):
            self.assertIn(base_size, script)
        self.assertIn('if ($scenario.FontScale -eq "1.0")', script)
        self.assertIn("[System.Globalization.CultureInfo]::InvariantCulture", script)
        self.assertIn("The snapshot window may have been constrained", script)

        self.assertNotIn("$windowsSmokeDirectory", script)
        self.assertIn("keep their top-level widgets off the physical desktop", script)
        self.assertIn("while still using qwindows", script)

        root_guard = script[
            script.index("$normalizedRoot =") :
            script.index("New-Item -ItemType Directory -Force -Path $OutputDirectory")
        ]
        self.assertIn("$normalizedOutput.Equals($normalizedRoot", root_guard)
        self.assertIn("[System.StringComparison]::OrdinalIgnoreCase", root_guard)
        self.assertIn(
            "must not be the repository root",
            root_guard,
        )

        cleanup_start = script.index("foreach ($expectedName in $expectedNames)")
        cleanup_end = script.index("$qtBin =", cleanup_start)
        cleanup = script[cleanup_start:cleanup_end]
        self.assertLess(cleanup_start, script.index("foreach ($appName in $apps)"))
        self.assertIn("Join-Path $OutputDirectory $expectedName", cleanup)
        self.assertIn("Remove-Item -LiteralPath $existingSnapshot -Force", cleanup)
        self.assertIn('Join-Path $OutputDirectory "manifest.json"', cleanup)
        self.assertIn("Remove-Item -LiteralPath $existingManifest -Force", cleanup)
        self.assertNotIn('Extension -ieq ".png"', cleanup)
        self.assertIn('Join-Path $OutputDirectory ".snapshot-bin"', cleanup)
        self.assertIn("$legacyPreviewDirectory.StartsWith($outputPrefix", cleanup)
        self.assertIn('Join-Path $snapshotProductBin ".as-invoker"', script)

        exact_set_check = script[
            script.index("$artifactFiles =") :
            script.index("$manifestPath =")
        ]
        self.assertIn("$artifactFiles = @(", exact_set_check)
        self.assertIn("-File -Recurse", exact_set_check)
        self.assertIn("$expectedFiles = @(", exact_set_check)
        self.assertIn("$unexpectedFiles = @(", exact_set_check)
        self.assertIn("$expectedNames -notcontains $relativeName", exact_set_check)
        self.assertIn("$expectedNames -contains $relativeName", exact_set_check)
        self.assertIn("-or $unexpectedFiles.Count -ne 0", exact_set_check)
        self.assertIn("Expected exactly 90 UI snapshots", exact_set_check)

        self.assertIn("capture-ui-regression.ps1", workflow)
        self.assertIn("ui-regression/*.png", workflow)
        self.assertIn("ui-regression/manifest.json", workflow)

    def test_dense_editor_snapshot_is_in_memory_isolated_and_self_validating(self) -> None:
        header = (ROOT / "Editor" / "MainWindow.h").read_text(encoding="utf-8")
        snapshot = (ROOT / "helpers" / "UiSnapshot.h").read_text(encoding="utf-8")
        capture = (ROOT / "scripts" / "capture-ui-regression.ps1").read_text(
            encoding="utf-8"
        )
        vst_library = (ROOT / "helpers" / "VSTPluginLibrary.cpp").read_text(
            encoding="utf-8"
        )
        vst_gui = (
            ROOT / "Editor" / "guis" / "VSTPluginFilterGUI.cpp"
        ).read_text(encoding="utf-8")
        loudness_factory = (
            ROOT / "Editor" / "guis" / "LoudnessCorrectionFilterGUIFactory.cpp"
        ).read_text(encoding="utf-8")

        self.assertIn("bool loadSnapshotScenario(const QString& scenario);", header)
        self.assertIn("bool snapshotLayoutIsValid() const;", header)
        self.assertIn("noSaveFilePreferences = snapshotMode;", MAIN_WINDOW)
        self.assertIn("QDir configDir(configPath);", EDITOR_MAIN)
        self.assertIn('QStringLiteral(":/snapshot")', EDITOR_MAIN)
        self.assertIn("if (!snapshotMode)", EDITOR_MAIN)
        self.assertIn("UiSnapshot::localeName()", EDITOR_MAIN)
        self.assertIn("w.loadSnapshotScenario(scenario)", EDITOR_MAIN)
        self.assertIn("w.snapshotLayoutIsValid()", EDITOR_MAIN)
        for relative_path, declaration in (
            ("Editor/main.cpp", "MainWindow w(configDir);"),
            ("DeviceSelector/main.cpp", "DeviceSelector dialog;"),
            ("UpdateChecker/main.cpp", "UpdateChecker dialog(nullptr, version);"),
        ):
            source = (ROOT / relative_path).read_text(encoding="utf-8")
            with self.subTest(snapshot_prepare=relative_path):
                self.assertLess(
                    source.index(declaration), source.index("UiSnapshot::prepareForCapture")
                )
                self.assertLess(
                    source.index("UiSnapshot::prepareForCapture"), source.index(".show();")
                )

        dense = MAIN_WINDOW[
            MAIN_WINDOW.index("bool MainWindow::loadSnapshotScenario") :
            MAIN_WINDOW.index("bool MainWindow::snapshotLayoutIsValid")
        ]
        self.assertIn("#ifdef EQAPO_ENABLE_UI_SNAPSHOTS", dense)
        self.assertIn("const QList<QString> lines", dense)
        self.assertIn("filterTable->addLine(line)", dense)
        self.assertNotIn("filterTable->setLines(", dense)
        self.assertNotIn("QSettings settings", dense)
        self.assertNotIn("QFile", dense)
        for command in (
            "LoudnessCorrection: Schema 1 Model FormulaLoudnessV1 Binding Single",
            "Volume -38.0",
            "LoudnessCorrectionOriginal: Schema 1 Model MixomoShelfV1",
            "Filter: ON PK Fc 1000 Hz Gain -3 dB Q 1",
            "UnsupportedSnapshotCommand: this-deliberately-long-unknown-command",
            "VSTPlugin: Library snapshot-memory",
            "Device: SNAPSHOT-MISSING-PLAYBACK-DEVICE",
            "# Preamp: -10.30 dB",
            "Convolution: snapshot-memory",
            "Include: snapshot-memory",
        ):
            with self.subTest(command=command):
                self.assertIn(command, dense)
        for object_name in (
            "FilterTableRow",
            "DeviceFilterGUI",
            "PreampFilterGUI",
            "ConvolutionFilterGUI",
            "IncludeFilterGUI",
            "BiQuadFilterGUI",
            "LoudnessCorrectionFilterGUI",
            "OriginalLoudnessCorrectionFilterGUI",
            "VSTPluginFilterGUI",
            "elidingCommandLabel",
        ):
            with self.subTest(object_name=object_name):
                self.assertIn(object_name, dense)

        for restored_command in (
            "Preamp: -100 dB",
            "Preamp: 100 dB",
            "Pan: Position 0 Width 100",
            "Crossfeed: Algorithm Natural",
            "Chorus: Rate 0.4 Hz",
            "Reverb: RoomSize 50 %",
            "ToneGenerator: State 0",
            "VUMeter: MeterId snapshot-meter",
            "ParametricEQ: ON PK",
            "HeadphoneCalibration:",
            "OutProcVSTPlugin: Library snapshot-memory",
        ):
            with self.subTest(restored_command=restored_command):
                self.assertIn(restored_command, dense)

        layout_contract = MAIN_WINDOW[
            MAIN_WINDOW.index("bool MainWindow::snapshotLayoutIsValid") :
            MAIN_WINDOW.index("void MainWindow::getDeviceAndChannelMask")
        ]
        self.assertIn('UiSnapshot::scenario() == QStringLiteral("dense")', layout_contract)
        self.assertIn('UiSnapshot::scenario() == QStringLiteral("restored-tools")', layout_contract)
        self.assertGreaterEqual(layout_contract.count("horizontalScrollBar()->maximum() != 0"), 2)
        self.assertIn('QStringLiteral("treeWidget")', layout_contract)
        self.assertIn("simpleGuiObjectNames", layout_contract)
        self.assertIn("widget->mapTo(container, QPoint(0, 0))", layout_contract)
        self.assertIn("container->rect().contains(geometryInContainer)", layout_contract)
        self.assertIn("gui->findChildren<QWidget*>()", layout_contract)
        self.assertIn('QStringLiteral("parametricEQScrollArea")', layout_contract)
        self.assertIn('QStringLiteral("headphoneCalibrationScrollArea")', layout_contract)
        self.assertIn("contentOverflows", layout_contract)
        self.assertIn(
            "snapshot failed for $($State.Label)/$Theme/$($Scenario.Label)", capture
        )

        test_snapshot, production_snapshot = snapshot.split("#else", 1)
        production_snapshot = production_snapshot.split("#endif", 1)[0]
        self.assertIn("std::function<bool()>", test_snapshot)
        self.assertNotIn("EQAPO_UI_SNAPSHOT", production_snapshot)
        self.assertNotIn("dense-real-world.txt", capture)
        self.assertNotIn("snapshot-memory", capture)

        vst_test_branch = vst_library[
            vst_library.index("#ifdef EQAPO_ENABLE_UI_SNAPSHOTS") :
            vst_library.index("#else", vst_library.index("#ifdef EQAPO_ENABLE_UI_SNAPSHOTS"))
        ]
        self.assertIn('return L"";', vst_test_branch)
        self.assertNotIn("RegistryHelper", vst_test_branch)

        vst_load_preferences = vst_gui[
            vst_gui.index("void VSTPluginFilterGUI::loadPreferences") :
            vst_gui.index("void VSTPluginFilterGUI::storePreferences")
        ]
        vst_snapshot_guard = vst_load_preferences[
            vst_load_preferences.index("#ifdef EQAPO_ENABLE_UI_SNAPSHOTS") :
            vst_load_preferences.index("#endif")
        ]
        self.assertIn("UiSnapshot::requested()", vst_snapshot_guard)
        self.assertIn("return;", vst_snapshot_guard)
        self.assertGreater(
            vst_load_preferences.index("initPlugin();"),
            vst_load_preferences.index("#endif"),
        )
        self.assertIn("timer == NULL && !UiSnapshot::requested()", loudness_factory)

    def test_palette_icons_render_on_a_transparent_surface(self) -> None:
        icon_engine = (ROOT / "Editor" / "helpers" / "GUIHelper.cpp").read_text(
            encoding="utf-8"
        )
        self.assertIn("QPixmap pixmap", icon_engine)
        self.assertIn("result.fill(Qt::transparent)", icon_engine)

    def test_large_text_can_expand_control_geometry(self) -> None:
        self.assertIn("QFontMetrics(QApplication::font()).height()", MODERN_THEME)
        self.assertIn("EQAPO_UI_FONT_SCALE", MODERN_THEME)
        self.assertNotIn("max-height: @controlHeight", MODERN_THEME)
        self.assertNotIn("max-height: @controlOuterHeight", MODERN_THEME)

    def test_frequency_plot_rulers_keep_large_text_inside_their_paint_devices(self) -> None:
        self.assertIn(
            "painter.fontMetrics().height() / 2.0",
            FREQUENCY_PLOT_VRULER,
        )
        self.assertIn("const auto clampTextCenter", FREQUENCY_PLOT_VRULER)
        self.assertIn(
            "return qBound(halfTextHeight, center, maxTextCenter);",
            FREQUENCY_PLOT_VRULER,
        )
        self.assertEqual(FREQUENCY_PLOT_VRULER.count("clampTextCenter("), 2)
        self.assertNotIn(
            "painter.drawText(0, y - topLeft.y() - 1",
            FREQUENCY_PLOT_VRULER,
        )
        self.assertIn("const auto clampTextCenter", FREQUENCY_PLOT_HRULER)
        self.assertIn(
            "return qBound(halfTextWidth, center, maxTextCenter);",
            FREQUENCY_PLOT_HRULER,
        )
        self.assertEqual(FREQUENCY_PLOT_HRULER.count("clampTextCenter("), 2)

        self.assertIn(
            'metrics.boundingRect(QStringLiteral("-100.0")).width()',
            FREQUENCY_PLOT_VIEW,
        )
        self.assertIn("metrics.height() + GUIHelper::scale(8)", FREQUENCY_PLOT_VIEW)
        self.assertIn("QEvent::FontChange", FREQUENCY_PLOT_VIEW)
        self.assertIn("QEvent::ApplicationFontChange", FREQUENCY_PLOT_VIEW)
        self.assertGreaterEqual(FREQUENCY_PLOT_VIEW.count("updateRulerMargins();"), 2)
        self.assertIn(
            "setViewportMargins(verticalRulerWidth, 0, 0, horizontalRulerHeight);\n"
            "\tupdateRulerGeometry();",
            FREQUENCY_PLOT_VIEW,
        )
        self.assertEqual(FREQUENCY_PLOT_VIEW.count("updateRulerGeometry();"), 2)

    def test_analysis_plot_bands_and_reset_remain_collision_safe_and_accessible(self) -> None:
        for token in (
            "struct TickLabel",
            "const qreal labelGap = GUIHelper::scale(6)",
            "const auto appendLabel",
            "labels.first().draw = true",
            "reserveRightBoundary",
            "candidate.occupiedRect.left() >= occupiedRight",
            "candidate.occupiedRect.right() <= rightBoundary",
        ):
            with self.subTest(collision_contract=token):
                self.assertIn(token, FREQUENCY_PLOT_HRULER)
        self.assertEqual(FREQUENCY_PLOT_HRULER.count("appendLabel("), 2)
        self.assertNotIn("metrics.size(0, text).width() + 2", FREQUENCY_PLOT_HRULER)

        self.assertIn("public slots:\n\tvoid resetView();", FREQUENCY_PLOT_VIEW_HEADER)
        reset_view = FREQUENCY_PLOT_VIEW[
            FREQUENCY_PLOT_VIEW.index("void FrequencyPlotView::resetView()") :
            FREQUENCY_PLOT_VIEW.index("void FrequencyPlotView::changeEvent")
        ]
        for token in (
            "GUIHelper::scaleZoom(1.0)",
            "s->setZoom(defaultZoom, defaultZoom)",
            "s->hzToX(20.0)",
            "s->dbToY(22.0)",
            "resetCachedContent()",
            "viewport()->update()",
            "hRuler->update()",
            "vRuler->update()",
        ):
            with self.subTest(reset_contract=token):
                self.assertIn(token, reset_view)

        ui_root = ET.fromstring(MAIN_WINDOW_UI)
        reset_action = ui_root.find(".//action[@name='actionResetAnalysisView']")
        self.assertIsNotNone(reset_action)
        self.assertEqual(
            reset_action.findtext("./property[@name='shortcut']/string"),
            "Ctrl+0",
        )
        shortcut_occurrences = sum(
            path.read_text(encoding="utf-8").count("Ctrl+0")
            for suffix in ("*.cpp", "*.h", "*.ui")
            for path in (ROOT / "Editor").rglob(suffix)
        )
        self.assertEqual(shortcut_occurrences, 1)
        reset_button = ui_root.find(".//widget[@name='resetAnalysisViewButton']")
        self.assertIsNotNone(reset_button)
        self.assertIn("GUIHelper::ThemeIcon::Restore", MAIN_WINDOW)
        self.assertIn("setDefaultAction(ui->actionResetAnalysisView)", MAIN_WINDOW)
        self.assertIn("setAccessibleName", MAIN_WINDOW)
        self.assertIn("setAccessibleDescription", MAIN_WINDOW)
        self.assertIn("SLOT(resetView())", MAIN_WINDOW)
        reset_button_style = MODERN_THEME[
            MODERN_THEME.index("QToolButton#resetAnalysisViewButton {") :
            MODERN_THEME.index("QScrollArea {")
        ]
        for token in (
            "background-color: @raised",
            "border: 1px solid @borderStrong",
            "min-height: @controlHeightpx",
            "QToolButton#resetAnalysisViewButton:hover",
            "QToolButton#resetAnalysisViewButton:focus",
            "QToolButton#resetAnalysisViewButton:pressed",
        ):
            with self.subTest(reset_style_contract=token):
                self.assertIn(token, reset_button_style)
        self.assertNotIn("border-radius", reset_button_style)

    def test_product_ui_has_no_decorative_rounded_corners(self) -> None:
        ui_sources = []
        for app in ("Editor", "DeviceSelector", "UpdateChecker"):
            for suffix in ("*.cpp", "*.h", "*.ui", "*.qss"):
                ui_sources.extend((ROOT / app).rglob(suffix))

        radius_pattern = re.compile(r"border-radius\s*:\s*([^;]+);", re.IGNORECASE)
        for path in ui_sources:
            text = path.read_text(encoding="utf-8", errors="strict")
            for match in radius_pattern.finditer(text):
                value = match.group(1).strip()
                with self.subTest(path=path.relative_to(ROOT), value=value):
                    self.assertIn(value, {"0", "0px", "@radioRadiuspx"})

    def test_every_designer_ui_file_is_valid_xml(self) -> None:
        ui_files = []
        for app in ("Editor", "DeviceSelector", "UpdateChecker"):
            ui_files.extend((ROOT / app).rglob("*.ui"))
        self.assertGreater(len(ui_files), 10)
        for path in ui_files:
            with self.subTest(path=path.relative_to(ROOT)):
                root = ET.parse(path).getroot()
                self.assertEqual(root.tag, "ui")

    def test_editor_workflows_are_real_and_recoverable(self) -> None:
        for token in (
            "captureComparisonA",
            "comparisonToggled",
            "bypassToggled",
            "linkedProfileForCurrentDevice",
            "QSystemTrayIcon",
            "temporaryProcessing",
            "conditionallyWriteConfiguration",
            "recoverInterruptedTemporaryProcessingState",
        ):
            with self.subTest(token=token):
                self.assertIn(token, MAIN_WINDOW)
        self.assertIn("findText", (ROOT / "Editor" / "FilterTable.cpp").read_text(encoding="utf-8"))
        self.assertIn("setAnalysisStatus", MAIN_WINDOW)
        self.assertIn("QAccessibleStateChangeEvent", MAIN_WINDOW)
        self.assertNotIn("Estimated headroom meter", MAIN_WINDOW)

    def test_temporary_audio_modes_preserve_dirty_and_external_edits(self) -> None:
        header = (ROOT / "Editor" / "MainWindow.h").read_text(encoding="utf-8")
        filter_table = (ROOT / "Editor" / "FilterTable.cpp").read_text(
            encoding="utf-8"
        )
        self.assertIn("isFilterTableDirty", MAIN_WINDOW)
        self.assertIn("temporaryHash", MAIN_WINDOW)
        self.assertIn("originalHash", MAIN_WINDOW)
        self.assertIn("OPEN_EXISTING", MAIN_WINDOW)
        self.assertIn("GENERIC_READ | GENERIC_WRITE", MAIN_WINDOW)
        self.assertIn("result.currentContent != expectedContent", MAIN_WINDOW)
        self.assertIn("temporaryContent", MAIN_WINDOW)
        self.assertIn("recordsByPath", MAIN_WINDOW)
        self.assertIn("resolvedHashes", MAIN_WINDOW)
        self.assertIn("matchingTemporary", MAIN_WINDOW)
        self.assertIn("JournalOwnerStatus::unknown", MAIN_WINDOW)
        self.assertIn("Global\\\\EqualizerAPO.Editor.TemporaryProcessing.v1", MAIN_WINDOW)
        self.assertGreaterEqual(MAIN_WINDOW.count("TemporaryProcessingMutexGuard processingLock"), 4)
        self.assertIn('QStringLiteral("journalVersion"), 2', MAIN_WINDOW)
        self.assertIn('QStringLiteral("committed"), false', MAIN_WINDOW)
        self.assertIn('QStringLiteral("committed"), true', MAIN_WINDOW)
        self.assertIn('QStringLiteral("originalContent"), originalContent', MAIN_WINDOW)
        journal = MAIN_WINDOW[
            MAIN_WINDOW.index("bool MainWindow::writeTemporaryRecoveryJournal") :
            MAIN_WINDOW.index("void MainWindow::clearTemporaryRecoveryJournal")
        ]
        self.assertLess(
            journal.index('QStringLiteral("committed"), false'),
            journal.index('QStringLiteral("path")'),
        )
        self.assertLess(
            journal.index('QStringLiteral("temporaryContent"), temporaryContent'),
            journal.index('QStringLiteral("committed"), true'),
        )
        self.assertIn("journalVersion >= 2 && !committed", MAIN_WINDOW)
        self.assertIn("serializeConfigurationLinesLike", MAIN_WINDOW)
        self.assertIn("MB_ERR_INVALID_CHARS", MAIN_WINDOW)
        self.assertIn("record.originalContent = hasOriginalContent", MAIN_WINDOW)
        self.assertIn("Keep external changes", MAIN_WINDOW)
        self.assertIn("if (restoringTemporaryState)", MAIN_WINDOW)
        self.assertIn("Restore saved profile", MAIN_WINDOW)
        self.assertIn("QMessageBox::Cancel", MAIN_WINDOW)
        self.assertIn("temporaryRecoveryOwner", MAIN_WINDOW)
        self.assertIn("ownerProcessStartedAt", MAIN_WINDOW)
        self.assertIn("journalOwnerStatus", MAIN_WINDOW)
        self.assertIn("quitRequested = false", MAIN_WINDOW)
        self.assertIn("bool restoreTemporaryProcessingState()", header)
        self.assertIn("bool load(QString path)", header)
        self.assertIn("selected.clear();", filter_table)
        self.assertIn("selectionStart = NULL;", filter_table)

    def test_failed_temporary_write_preserves_unverified_recovery_journal(self) -> None:
        conditional_write = MAIN_WINDOW[
            MAIN_WINDOW.index("static ConditionalWriteResult conditionallyWriteConfiguration") :
            MAIN_WINDOW.index("static quint64 processCreationToken")
        ]
        begin_temporary = MAIN_WINDOW[
            MAIN_WINDOW.index("bool MainWindow::beginTemporaryFilterConfiguration") :
            MAIN_WINDOW.index("bool MainWindow::restoreTemporaryFilterConfiguration")
        ]

        self.assertIn("bool rollbackVerified = false;", MAIN_WINDOW)
        self.assertIn("contentAfterRollback == originalContent", conditional_write)
        self.assertIn("readConfigurationHandle(", conditional_write)
        verified_branch = begin_temporary[
            begin_temporary.index("else if (result.rollbackVerified)") :
            begin_temporary.index("else\n\t\t{", begin_temporary.index("else if (result.rollbackVerified)"))
        ]
        self.assertIn("clearTemporaryRecoveryJournal();", verified_branch)
        unverified_start = begin_temporary.index(
            "else\n\t\t{", begin_temporary.index("else if (result.rollbackVerified)")
        )
        unverified_branch = begin_temporary[
            unverified_start : begin_temporary.index(
                "\t\treturn false;", unverified_start
            )
        ]
        self.assertNotIn("clearTemporaryRecoveryJournal();", unverified_branch)
        self.assertIn("QMessageBox::critical", unverified_branch)

    def test_temporary_audio_state_blocks_cross_session_and_destructive_actions(self) -> None:
        conflict_check = MAIN_WINDOW[
            MAIN_WINDOW.index("static bool hasConflictingTemporaryJournal") :
            MAIN_WINDOW.index("MainWindow::MainWindow")
        ]
        self.assertIn("configurationPathKey(path)", conflict_check)
        self.assertIn('beginGroup(QStringLiteral("temporaryProcessing"))', conflict_check)
        self.assertIn("if (owner == currentOwner)", conflict_check)
        self.assertIn(
            "configurationPathKey(journalPath) == requestedPathKey",
            conflict_check,
        )
        self.assertGreaterEqual(
            MAIN_WINDOW.count("hasConflictingTemporaryJournal("),
            3,
        )

        save_action = MAIN_WINDOW[
            MAIN_WINDOW.index("void MainWindow::on_actionSave_triggered()") :
            MAIN_WINDOW.index("void MainWindow::on_actionSaveAs_triggered()")
        ]
        save_as_action = MAIN_WINDOW[
            MAIN_WINDOW.index("void MainWindow::on_actionSaveAs_triggered()") :
            MAIN_WINDOW.index("void MainWindow::on_actionNew_triggered()")
        ]
        for action_name, action_source in (
            ("Save", save_action),
            ("Save As", save_as_action),
        ):
            with self.subTest(action=action_name):
                self.assertIn("restoreTemporaryProcessingState()", action_source)
                self.assertLess(
                    action_source.index("restoreTemporaryProcessingState()"),
                    action_source.index("save(filterTable"),
                )

        instant_mode = MAIN_WINDOW[
            MAIN_WINDOW.index("void MainWindow::instantModeEnabled(bool enabled)") :
            MAIN_WINDOW.index("void MainWindow::on_tabWidget_currentChanged")
        ]
        restore_index = instant_mode.index("restoreTemporaryProcessingState()")
        self.assertLess(instant_mode.index("if (enabled)"), restore_index)
        self.assertLess(restore_index, instant_mode.index("for (int i"))
        self.assertLess(restore_index, instant_mode.index("save(filterTable"))
        restore_failure = instant_mode[
            instant_mode.index("if (!restoreTemporaryProcessingState())") :
            instant_mode.index("for (int i")
        ]
        self.assertIn("instantModeCheckBox->setChecked(false);", restore_failure)
        self.assertLess(
            restore_failure.index("instantModeCheckBox->setChecked(false);"),
            restore_failure.index("return;"),
        )

        action_state = MAIN_WINDOW[
            MAIN_WINDOW.index("void MainWindow::refreshWorkspaceActionState()") :
            MAIN_WINDOW.index("void MainWindow::closeToTrayToggled")
        ]
        compact_action_state = re.sub(r"\s+", " ", action_state)
        for action_name in ("actionSave", "actionSaveAs"):
            with self.subTest(disabled_action=action_name):
                match = re.search(
                    rf"ui->{action_name}->setEnabled\(([^;]+)\);",
                    compact_action_state,
                )
                self.assertIsNotNone(match)
                condition = match.group(1)
                self.assertIn("!", condition)
                self.assertTrue(
                    "temporary" in condition.lower()
                    or (
                        "showingComparisonA" in condition
                        and "bypassTable.isNull()" in condition
                    ),
                    condition,
                )

        reset_global = MAIN_WINDOW[
            MAIN_WINDOW.index(
                "void MainWindow::on_actionResetAllGlobalPreferences_triggered()"
            ) :
            MAIN_WINDOW.index(
                "void MainWindow::on_actionResetAllFileSpecificPreferences_triggered()"
            )
        ]
        self.assertIn("restoreTemporaryProcessingState()", reset_global)
        self.assertLess(
            reset_global.index("restoreTemporaryProcessingState()"),
            reset_global.index("settings.remove("),
        )
        self.assertRegex(
            reset_global,
            r'key\s*!=\s*(?:QStringLiteral\()?"temporaryProcessing"\)?',
        )

    def test_windows_profile_names_reject_reserved_devices_and_controls(self) -> None:
        self.assertIn("character.unicode() < 0x20", MAIN_WINDOW)
        for reserved in ("CON", "PRN", "AUX", "NUL", "COM9", "LPT9"):
            with self.subTest(reserved=reserved):
                self.assertIn(f'QStringLiteral("{reserved}")', MAIN_WINDOW)

    def test_convolution_compacts_local_ir_controls_and_defers_manual_matching(self) -> None:
        source = (ROOT / "Editor" / "guis" / "ConvolutionFilterGUI.cpp").read_text(
            encoding="utf-8"
        )
        ui = ET.parse(ROOT / "Editor" / "guis" / "ConvolutionFilterGUI.ui").getroot()
        update_file_info = source[
            source.index("void ConvolutionFilterGUI::updateFileInfo()") :
        ]

        self.assertNotIn("bundledIrCreditLabel", source)
        self.assertIn("bundledIrButton->setToolTip(bundledIrDescription);", source)
        self.assertIn(
            "bundledIrButton->setAccessibleDescription(bundledIrDescription);", source
        )
        self.assertIn("const bool sampleRateMismatch = deviceSampleRate != 0", update_file_info)
        self.assertIn("sampleRate <= 0 ||", update_file_info)
        self.assertIn(
            "isSafeImpulseShape(info.frames, info.channels", update_file_info
        )
        self.assertIn(
            "matchedFirActionVisible = sampleRateMismatch;", update_file_info
        )
        self.assertNotIn("matchDeviceSampleRate(", update_file_info)
        self.assertIn(
            "ui->matchSampleRatePushButton->setVisible(matchedFirActionVisible);",
            update_file_info,
        )
        self.assertIn(
            "ui->matchSampleRatePushButton->setEnabled(matchedFirActionVisible);",
            update_file_info,
        )
        match_button = ui.find(".//widget[@name='matchSampleRatePushButton']")
        self.assertIsNotNone(match_button)
        self.assertEqual(
            match_button.findtext("property[@name='visible']/bool"), "false"
        )
        self.assertEqual(
            match_button.findtext("property[@name='text']/string"),
            "Rebuild matched FIR",
        )
        self.assertIn(
            "Original phase and delay are not preserved",
            match_button.findtext("property[@name='toolTip']/string"),
        )
        file_info_layout = ui.find(".//layout[@name='gridLayout_2']")
        self.assertIsNotNone(file_info_layout)
        self.assertEqual(file_info_layout.get("columnstretch"), "0,1")

    def test_calibration_exposes_safe_three_step_status_flow(self) -> None:
        dialog_source = (
            ROOT / "Editor" / "guis" / "LoudnessCorrectionFilterGUIDialog.cpp"
        ).read_text(encoding="utf-8")
        dialog_ui = ET.parse(
            ROOT / "Editor" / "guis" / "LoudnessCorrectionFilterGUIDialog.ui"
        ).getroot()
        titles = {
            node.text
            for node in dialog_ui.findall(".//property[@name='title']/string")
        }
        self.assertEqual(
            titles,
            {
                "1 · Choose one speaker",
                "2 · Play the calibration signal",
                "3 · Enter the measured level",
            },
        )
        self.assertIn("isPlaybackEndpointStillValid", dialog_source)
        self.assertIn("on_stopButton_clicked", dialog_source)
        dialog_text = ET.tostring(dialog_ui, encoding="unicode")
        self.assertIn("equal-loudness contour is temporarily disabled", dialog_text)
        self.assertIn("APO volume follow remains active", dialog_text)

    def test_companion_apps_expose_loading_empty_success_and_failure_states(self) -> None:
        for token in ("loadingPage", "emptyPage", "errorPage", "devicesPage"):
            with self.subTest(device_state=token):
                self.assertIn(token, DEVICE_SELECTOR)
        for token in ("showChecking", "showUpToDate", "showUpdateAvailable", "showFailure"):
            with self.subTest(update_state=token):
                self.assertIn(token, UPDATE_CHECKER)
        self.assertIn("retryRequested", UPDATE_CHECKER)
        self.assertIn("layout()->activate()", UPDATE_CHECKER)
        self.assertIn("layout()->minimumSize().height()", UPDATE_CHECKER)
        self.assertIn("qMax(preferredHeight, contentMinimumHeight)", UPDATE_CHECKER)
        self.assertIn("bool UpdateChecker::snapshotLayoutIsValid() const", UPDATE_CHECKER)
        self.assertIn("widget->height() < widget->sizeHint().height()", UPDATE_CHECKER)
        update_main = (ROOT / "UpdateChecker" / "main.cpp").read_text(encoding="utf-8")
        self.assertIn("dialog.snapshotLayoutIsValid()", update_main)

    def test_device_test_shutdown_is_cooperative_and_preserves_errors(self) -> None:
        worker = (ROOT / "DeviceSelector" / "DeviceTestThread.cpp").read_text(
            encoding="utf-8"
        )
        dialog_header = (ROOT / "DeviceSelector" / "DeviceTestDialog.h").read_text(
            encoding="utf-8"
        )
        service_helper_header = (ROOT / "helpers" / "ServiceHelper.h").read_text(
            encoding="utf-8"
        )
        service_helper = (ROOT / "helpers" / "ServiceHelper.cpp").read_text(
            encoding="utf-8"
        )
        self.assertNotIn("thread->terminate()", DEVICE_TEST_DIALOG)
        self.assertIn("thread->requestInterruption()", DEVICE_TEST_DIALOG)
        self.assertIn("connect(thread, &QThread::finished", DEVICE_TEST_DIALOG)
        self.assertIn("__override void reject();", dialog_header)
        self.assertIn("__override void done(int resultCode);", dialog_header)
        self.assertIn("bool requestCooperativeShutdown(int resultCode);", dialog_header)
        self.assertIn("void DeviceTestDialog::reject()", DEVICE_TEST_DIALOG)
        self.assertIn("void DeviceTestDialog::done(int resultCode)", DEVICE_TEST_DIALOG)
        self.assertIn(
            "if (requestCooperativeShutdown(QDialog::Rejected))",
            DEVICE_TEST_DIALOG,
        )
        self.assertIn(
            "if (requestCooperativeShutdown(resultCode))",
            DEVICE_TEST_DIALOG,
        )
        self.assertIn("hasErrors = hasErrors || hasProblems", DEVICE_TEST_DIALOG)
        self.assertIn("!initializationFailed && !devices.isEmpty()", DEVICE_TEST_DIALOG)
        self.assertIn("setAccessibleDescription(message)", DEVICE_TEST_DIALOG)
        self.assertIn("isInterruptionRequested()", worker)
        self.assertIn("using CancellationCheck = std::function<bool()>;", service_helper_header)
        self.assertIn("static bool restartService", service_helper_header)
        self.assertEqual(
            worker.count(
                'if (!ServiceHelper::restartService(L"AudioSrv", interruptionRequested))'
            ),
            1,
        )
        fallback_start = worker.index("if (!remainingDevices.isEmpty())")
        fallback_guard = worker.rfind(
            "if (isInterruptionRequested())", 0, fallback_start
        )
        fallback_end = worker.index("catch (ReceiveException", fallback_start)
        fallback = worker[fallback_start:fallback_end]
        self.assertGreater(fallback_guard, worker.index("thread.waitUntil(timeout)"))
        self.assertLess(fallback_guard, fallback_start)
        self.assertIn('ServiceHelper::restartService(L"AudioSrv");', fallback)
        self.assertNotIn(
            'restartService(L"AudioSrv", interruptionRequested)', fallback
        )
        self.assertLess(
            fallback.index("testInfo->deviceInfo->reinstall();"),
            fallback.index('ServiceHelper::restartService(L"AudioSrv");'),
        )
        self.assertIn("return isInterruptionRequested();", worker)
        self.assertGreaterEqual(
            service_helper.count("cancellationRequested(isCancellationRequested)"),
            4,
        )
        self.assertIn("stoppedServices.push_back(service);", service_helper)
        self.assertIn(
            "servicesToStart = cancelled ? stoppedServices : services",
            service_helper,
        )
        restore_phase = service_helper.index("servicesToStart =")
        start_request = service_helper.index("service->start();", restore_phase)
        cancelled_exit = service_helper.index(
            "if (cancelled && index == 0 && state == SERVICE_START_PENDING)",
            start_request,
        )
        self.assertLess(start_request, cancelled_exit)

    def test_theme_reacts_to_windows_palette_and_accessibility_changes(self) -> None:
        for token in (
            "DwmGetColorizationColor",
            "WM_THEMECHANGED",
            "WM_SYSCOLORCHANGE",
            "WM_SETTINGCHANGE",
            "colorSchemeChanged",
            "contrastPreferenceChanged",
            "createHighContrastPalette",
        ):
            with self.subTest(token=token):
                self.assertIn(token, MODERN_THEME)

    def test_high_contrast_keeps_full_styles_with_system_selection_colors(self) -> None:
        signature = "QString createHighContrastStyleSheet()"
        declaration = MODERN_THEME.index(signature)
        definition = MODERN_THEME.index(signature, declaration + len(signature))
        high_contrast = MODERN_THEME[
            definition :
            MODERN_THEME.index("class ThemeMonitor final")
        ]
        self.assertIn("const QPalette palette = QApplication::palette();", high_contrast)
        self.assertIn("const ThemeColors colors = {", high_contrast)
        colors = high_contrast[
            high_contrast.index("const ThemeColors colors = {") :
            high_contrast.index(
                "};", high_contrast.index("const ThemeColors colors = {")
            )
        ]
        for role in (
            "QPalette::Window",
            "QPalette::Base",
            "QPalette::Button",
            "QPalette::WindowText",
            "QPalette::ButtonText",
            "QPalette::Text",
            "QPalette::Highlight",
            "QPalette::HighlightedText",
        ):
            with self.subTest(system_role=role):
                self.assertIn(role, colors)
        self.assertNotRegex(colors, r"#[0-9A-Fa-f]{3,8}")
        self.assertIn("QString style = createStyleSheet(colors);", high_contrast)
        self.assertIn("background-color: palette(highlight);", high_contrast)
        self.assertIn("color: palette(highlighted-text);", high_contrast)
        self.assertIn("QGroupBox::indicator", MODERN_THEME)
        self.assertIn("QToolBar#mainToolBar QToolButton,", high_contrast)
        self.assertIn("border: 1px solid palette(window-text);", high_contrast)
        update_ui = (ROOT / "UpdateChecker" / "UpdateChecker.ui").read_text(
            encoding="utf-8"
        )
        self.assertNotIn("QPushButton#skipButton", update_ui)


if __name__ == "__main__":
    unittest.main()
