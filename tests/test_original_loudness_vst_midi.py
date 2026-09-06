"""Contracts for the standalone original loudness filter and VST MIDI control."""

from __future__ import annotations

import re
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(relative: str) -> str:
    return (ROOT / relative).read_text(encoding="utf-8")


class OriginalLoudnessComponentTests(unittest.TestCase):
    def test_formula_gui_store_uses_valid_qt_numeric_placeholders(self) -> None:
        gui = read("Editor/guis/LoudnessCorrectionFilterGUI.cpp")
        store_start = gui.index("void LoudnessCorrectionFilterGUI::store")
        store = gui[
            store_start :
            gui.index("LoudnessCorrectionFilterGUI::getBindingMode", store_start)
        ]

        self.assertNotIn("%0", store)
        self.assertGreaterEqual(store.count('QString("%1").arg(att'), 2)
        self.assertIn('QString(" Volume %1").arg(', store)

    def test_original_component_is_not_a_mode_of_the_formula_component(self) -> None:
        required = (
            "filters/loudnessCorrection/OriginalLoudnessCorrectionFilter.h",
            "filters/loudnessCorrection/OriginalLoudnessCorrectionFilter.cpp",
            "filters/loudnessCorrection/OriginalLoudnessCorrectionFilterFactory.h",
            "filters/loudnessCorrection/OriginalLoudnessCorrectionFilterFactory.cpp",
            "Editor/guis/OriginalLoudnessCorrectionFilterGUI.h",
            "Editor/guis/OriginalLoudnessCorrectionFilterGUI.cpp",
            "Editor/guis/OriginalLoudnessCorrectionFilterGUIFactory.h",
            "Editor/guis/OriginalLoudnessCorrectionFilterGUIFactory.cpp",
        )
        for relative in required:
            with self.subTest(path=relative):
                self.assertTrue((ROOT / relative).is_file())

        runtime_factory = read(
            "filters/loudnessCorrection/OriginalLoudnessCorrectionFilterFactory.cpp"
        )
        gui_factory = read(
            "Editor/guis/OriginalLoudnessCorrectionFilterGUIFactory.cpp"
        )
        self.assertIn('L"LoudnessCorrectionOriginal"', runtime_factory)
        self.assertRegex(
            gui_factory,
            r'command\s*[!=]=\s*"LoudnessCorrectionOriginal"',
        )
        self.assertIn("Schema 1 Model MixomoShelfV1", gui_factory)
        self.assertIn("Loudness correction (original)", gui_factory)

        modern_header = read(
            "filters/loudnessCorrection/LoudnessCorrectionFilter.h"
        )
        self.assertNotIn("MixomoShelfV1", modern_header)
        self.assertNotIn("Original", modern_header)

        filter_table = read("Editor/FilterTable.cpp")
        self.assertIn("OriginalLoudnessCorrectionFilterGUIFactory", filter_table)
        modern_factory = read(
            "Editor/guis/LoudnessCorrectionFilterGUIFactory.cpp"
        )
        self.assertNotIn('command == "LoudnessCorrectionOriginal"', modern_factory)

    def test_original_calibration_is_separate_and_journaled(self) -> None:
        gui = read("Editor/guis/OriginalLoudnessCorrectionFilterGUI.cpp")
        self.assertIn("OriginalLoudnessCorrectionCalibrationDialog", gui)
        self.assertIn("beginTemporaryFilterConfiguration", gui)
        self.assertIn("restoreTemporaryFilterConfiguration", gui)
        self.assertIn("serializedParameters(false)", gui)
        self.assertIn("75.0 - dialog.getMeasuredLevel()", gui)

    def test_calibrations_do_not_overwrite_kept_external_edits(self) -> None:
        calibrations = (
            (
                read("Editor/guis/LoudnessCorrectionFilterGUI.cpp"),
                "void LoudnessCorrectionFilterGUI::on_calibrateButton_clicked()",
                "void LoudnessCorrectionFilterGUI::updateVolume()",
            ),
            (
                read("Editor/guis/OriginalLoudnessCorrectionFilterGUI.cpp"),
                "void OriginalLoudnessCorrectionFilterGUI::calibrate()",
                "QSize OriginalLoudnessCorrectionFilterGUI::sizeHint() const",
            ),
        )
        for source, start_marker, end_marker in calibrations:
            with self.subTest(calibration=start_marker):
                body = source[
                    source.index(start_marker) : source.index(end_marker)
                ]
                self.assertIn("bool keptExternal = false;", body)
                self.assertIn(
                    "restoreTemporaryFilterConfiguration(&keptExternal)", body
                )
                kept_index = body.index("if (keptExternal)")
                self.assertLess(kept_index, body.index("getMeasuredLevel()"))

    def test_both_calibration_dialogs_stop_noise_before_closing(self) -> None:
        dialogs = (
            (
                "OriginalLoudnessCorrectionCalibrationDialog",
                "Editor/guis/OriginalLoudnessCorrectionCalibrationDialog.h",
                "Editor/guis/OriginalLoudnessCorrectionCalibrationDialog.cpp",
            ),
            (
                "LoudnessCorrectionFilterGUIDialog",
                "Editor/guis/LoudnessCorrectionFilterGUIDialog.h",
                "Editor/guis/LoudnessCorrectionFilterGUIDialog.cpp",
            ),
        )
        for class_name, header_path, source_path in dialogs:
            with self.subTest(dialog=class_name):
                header = read(header_path)
                source = read(source_path)
                self.assertIn("void accept() override;", header)
                self.assertIn("void reject() override;", header)
                for method in ("accept", "reject"):
                    start = source.index(f"void {class_name}::{method}()")
                    base_call = source.index(f"QDialog::{method}()", start)
                    self.assertLess(source.index("stopPlayback();", start), base_call)

    def test_calibration_override_resolves_decorated_gui_descendants(self) -> None:
        table_header = read("Editor/FilterTable.h")
        table_source = read("Editor/FilterTable.cpp")
        main_window = read("Editor/MainWindow.cpp")

        self.assertIn("or one of its descendants as the target", table_header)
        self.assertIn("item->gui->isAncestorOf(target)", table_source)
        snapshot_probe = main_window[
            main_window.index("const QString overrideMarker") :
            main_window.index("refreshWorkspaceActionState();", main_window.index("const QString overrideMarker"))
        ]
        self.assertIn("makeLinesWithGuiOverride", snapshot_probe)
        self.assertIn('QStringLiteral("LoudnessCorrectionFilterGUI")', snapshot_probe)
        self.assertIn(
            'QStringLiteral("OriginalLoudnessCorrectionFilterGUI")', snapshot_probe
        )
        self.assertIn("overriddenLines.count", snapshot_probe)

    def test_original_small_corrections_are_not_discarded_as_identity(self) -> None:
        runtime = read(
            "filters/loudnessCorrection/OriginalLoudnessCorrectionFilter.cpp"
        )
        benchmark = read("Benchmark/Benchmark.cpp")

        self.assertIn("std::numeric_limits<double>::epsilon()", runtime)
        self.assertIn(
            "std::abs(result.outputGainLinear - 1.0) <= identityEpsilon",
            runtime,
        )
        self.assertNotIn(") < 0.2;", runtime)
        self.assertIn("minus-0.1db-has-no-identity-dead-zone", benchmark)
        self.assertIn("plus-0.1db-has-no-identity-dead-zone", benchmark)

    def test_original_failed_initial_observation_is_deterministic(self) -> None:
        runtime = read(
            "filters/loudnessCorrection/OriginalLoudnessCorrectionFilter.cpp"
        )
        initialize = runtime[
            runtime.index("OriginalLoudnessCorrectionFilter::initialize(") :
            runtime.index("void OriginalLoudnessCorrectionFilter::publishSnapshot")
        ]
        self.assertIn("EndpointVolumeState volumeState = {};", initialize)
        self.assertLess(
            initialize.index("EndpointVolumeState volumeState = {};"),
            initialize.index("observation.volumeScalar = volumeState.scalar"),
        )

    def test_original_controls_reflow_and_calibration_can_shrink(self) -> None:
        gui_header = read("Editor/guis/OriginalLoudnessCorrectionFilterGUI.h")
        gui_source = read("Editor/guis/OriginalLoudnessCorrectionFilterGUI.cpp")
        calibration = read(
            "Editor/guis/OriginalLoudnessCorrectionCalibrationDialog.cpp"
        )
        formula_gui = read("Editor/guis/LoudnessCorrectionFilterGUI.cpp")

        self.assertIn("void resizeEvent(QResizeEvent* event) override;", gui_header)
        self.assertIn("updateResponsiveLayout(event->size().width())", gui_source)
        self.assertIn("availableWidth >= GUIHelper::scale(620)", gui_source)
        self.assertIn("index / columns, index % columns", gui_source)
        self.assertGreaterEqual(gui_source.count("GUIHelper::scale(184)"), 2)
        for object_name in (
            "originalReferenceLevelControl",
            "originalReferenceOffsetControl",
            "originalAttenuationControl",
            "originalVolumeControl",
        ):
            self.assertIn(object_name, gui_source)
        self.assertIn("setMinimumWidth(0);", calibration)
        self.assertNotIn("setMinimumWidth(420);", calibration)
        self.assertIn("GUIHelper::scale(520)", calibration)
        minimum_hint = formula_gui[
            formula_gui.index("QSize LoudnessCorrectionFilterGUI::minimumSizeHint") :
        ]
        self.assertIn("size.setWidth(0);", minimum_hint)

    def test_original_audio_callback_has_no_blocking_or_io(self) -> None:
        source = read(
            "filters/loudnessCorrection/OriginalLoudnessCorrectionFilter.cpp"
        )
        match = re.search(
            r"#pragma AVRT_CODE_BEGIN(.*?)#pragma AVRT_CODE_END",
            source,
            re.S,
        )
        self.assertIsNotNone(match)
        body = match.group(1)
        forbidden = (
            "WaitForSingleObject",
            "Sleep(",
            "EnterCriticalSection",
            "LeaveCriticalSection",
            "ResetEvent",
            "LogF(",
            "new ",
            "delete ",
            "CreateThread",
            "CreateEvent",
            "VolumeController",
            ".assign(",
            ".resize(",
            ".push_back(",
        )
        for token in forbidden:
            with self.subTest(token=token):
                self.assertNotIn(token, body)

    def test_docs_preserve_the_two_component_boundary(self) -> None:
        readme = read("README.md")
        agents = read("AGENTS.md")
        decision = read(
            "docs/decisions/0003-separate-original-loudness-component.md"
        )
        for text in (readme, agents, decision):
            with self.subTest(document=text[:40]):
                self.assertIn("LoudnessCorrectionOriginal:", text)
                self.assertIn("MixomoShelfV1", text)
                self.assertIn("LoudnessCorrection:", text)
                self.assertIn("FormulaLoudnessV1", text)


class VSTMidiControlTests(unittest.TestCase):
    def test_existing_midi_mapping_is_temporarily_released_while_learning(self) -> None:
        gui_header = read("Editor/guis/VSTPluginFilterGUI.h")
        gui = read("Editor/guis/VSTPluginFilterGUI.cpp")
        main_header = read("Editor/MainWindow.h")
        main = read("Editor/MainWindow.cpp")

        self.assertIn("storeWithMidiConfig", gui_header)
        store = gui[
            gui.index("void VSTPluginFilterGUI::store") :
            gui.index("void VSTPluginFilterGUI::loadPreferences")
        ]
        self.assertIn("storeWithMidiConfig(command, parameters, midiConfig)", store)

        handler = gui[
            gui.index("void VSTPluginFilterGUI::on_midiButton_clicked") :
            gui.index("void VSTPluginFilterGUI::on_vst3ClassComboBox_currentIndexChanged")
        ]
        ensure_stopped = gui[
            gui.index("bool VSTPluginFilterGUI::ensureOutProcPanelStopped") :
            gui.index("void VSTPluginFilterGUI::applyDialog")
        ]
        self.assertIn("beginTemporaryFilterConfiguration", handler)
        self.assertIn("restoreTemporaryFilterConfiguration", handler)
        self.assertIn("storeWithMidiConfig", handler)
        self.assertIn("std::wstring()", handler)
        self.assertIn("keptExternal", handler)
        self.assertIn("if (outProcMode && !midiConfig.empty())", handler)
        self.assertNotIn("outProcGuiRunning && !midiConfig.empty()", handler)
        self.assertIn("if (!ensureOutProcPanelStopped(true))", handler)
        self.assertLess(
            handler.index("ensureOutProcPanelStopped(true)"),
            handler.index("beginTemporaryFilterConfiguration"),
        )
        self.assertIn("synchronizeRecoveredState && stateChanged", ensure_stopped)
        self.assertIn("emit updateModel()", ensure_stopped)
        self.assertLess(
            handler.index("beginTemporaryFilterConfiguration"),
            handler.index("VSTMidiMappingDialog dialog"),
        )
        self.assertLess(
            handler.index("dialog.exec()"),
            handler.index("restoreTemporaryFilterConfiguration"),
        )
        self.assertIn(
            "close its WinMM handle) before restoring the\n"
            "\t\t// runtime row",
            handler,
        )
        self.assertLess(
            handler.index("restoreTemporaryFilterConfiguration"),
            handler.index("if (dialogResult != QDialog::Accepted)"),
        )
        self.assertLess(
            handler.index("restoreTemporaryFilterConfiguration"),
            handler.index("midiConfig = updatedConfiguration"),
        )

        self.assertIn(
            "bool restoreTemporaryFilterConfiguration(bool* keptExternal = NULL);",
            main_header,
        )
        restore = main[
            main.index("bool MainWindow::restoreTemporaryFilterConfiguration") :
            main.index("bool MainWindow::setTemporaryLines")
        ]
        self.assertIn("*keptExternal = false", restore)
        self.assertIn("*keptExternal = keepExternal", restore)

        begin = main[
            main.index("bool MainWindow::beginTemporaryFilterConfiguration") :
            main.index("bool MainWindow::restoreTemporaryFilterConfiguration")
        ]
        self.assertIn("isFilterTableDirty(filterTable)", begin)
        self.assertIn("writeTemporaryRecoveryJournal", begin)
        self.assertIn('mode == QStringLiteral("vst-midi-learn")', begin)
        self.assertIn("This row temporarily released its MIDI input", begin)
        self.assertLess(
            begin.index("isFilterTableDirty(filterTable)"),
            begin.index("writeTemporaryRecoveryJournal"),
        )

    def test_only_visible_writable_parameters_are_offered_for_midi(self) -> None:
        gui = read("Editor/guis/VSTPluginFilterGUI.cpp")
        available = gui[
            gui.index(
                "std::vector<VSTParameterDescriptor> "
                "VSTPluginFilterGUI::availableMidiParameters"
            ) : gui.index("void VSTPluginFilterGUI::updateMidiButton")
        ]
        self.assertIn("!parameter.readOnly && !parameter.hidden", available)
        self.assertIn(
            "eligibleParameters(effect->getParameterDescriptors())", available
        )
        self.assertIn("eligibleParameters(outProcParameterDescriptors)", available)

    def test_runtime_owns_versioned_midi_configuration(self) -> None:
        codec = read("helpers/VSTMidiBindingCodec.h")
        self.assertIn("VSTMidiBinding", codec)
        self.assertIn("serialize", codec)
        self.assertIn("deserialize", codec)
        self.assertRegex(codec, r"\bversion\b")

        midi_input = read("helpers/WinMidiInput.cpp")
        self.assertIn("midiInGetNumDevs", midi_input)
        self.assertIn("midiInOpen", midi_input)
        self.assertIn("midiInStart", midi_input)
        self.assertIn("midiInReset", midi_input)
        self.assertIn("midiInClose", midi_input)
        self.assertIn("CALLBACK_FUNCTION", midi_input)

        for factory in (
            "filters/VSTPluginFilterFactory.cpp",
            "filters/OutProcVSTPluginFilterFactory.cpp",
        ):
            with self.subTest(factory=factory):
                self.assertIn("MidiConfig", read(factory))

        config = read("outproc/OutProcVSTConfig.h")
        self.assertIn("midiConfig", config)
        self.assertRegex(
            config,
            r"OUTPROC_VST_CONFIG_VERSION\s*=\s*[34]",
        )
        self.assertRegex(
            config,
            re.compile(r"version\s*==\s*1.*version\s*==\s*2", re.S),
        )

        editor_factory = read("Editor/guis/VSTPluginFilterGUIFactory.cpp")
        self.assertIn("MidiConfig", editor_factory)
        self.assertIn("getMidiConfig", editor_factory)

    def test_midi_targets_stable_vst_parameters(self) -> None:
        header = read("helpers/VSTPluginInstance.h")
        source = read("helpers/VSTPluginInstance.cpp")
        self.assertIn("VSTParameterDescriptor", header)
        self.assertIn("getParameterDescriptors", header)
        self.assertIn("setParameterNormalized", header)
        self.assertIn("ParamID id", source)
        self.assertIn("instance->onAutomate(id, value)", source)
        self.assertRegex(source, r"VST_HOST_OPCODE_AUTOMATE[\s\S]{0,200}onAutomate\(.*index.*opt")

    def test_midi_ui_is_accessible_and_does_not_write_per_message(self) -> None:
        gui = read("Editor/guis/VSTPluginFilterGUI.cpp")
        header = read("Editor/guis/VSTPluginFilterGUI.h")
        ui = ET.parse(ROOT / "Editor/guis/VSTPluginFilterGUI.ui").getroot()
        names = {node.attrib.get("name") for node in ui.findall(".//widget")}

        self.assertIn("midiButton", names)
        self.assertIn("on_midiButton_clicked", header)
        self.assertIn("setAccessibleName", gui)
        self.assertIn("VSTMidiMappingDialog", gui)
        self.assertIn("Qt::CoarseTimer", gui)

        automate = re.search(
            r"void\s+VSTPluginFilterGUI::onAutomate\s*\([^)]*\)\s*\{(.*?)\n\}",
            gui,
            re.S,
        )
        self.assertIsNotNone(automate)
        self.assertNotIn("updatePermissionWarning", automate.group(1))
        self.assertNotIn("readFromEffect", automate.group(1))
        self.assertIn("automationDirty", automate.group(1))

    def test_vst_path_comparison_uses_resolved_absolute_paths(self) -> None:
        gui = read("Editor/guis/VSTPluginFilterGUI.cpp")
        handler = gui[
            gui.index("void VSTPluginFilterGUI::on_pathLineEdit_editingFinished") :
            gui.index("void VSTPluginFilterGUI::refreshVST3ClassComboBox")
        ]
        self.assertIn("canonical", handler.lower())
        self.assertNotIn(
            "QString::fromStdWString(library->getLibPath()) != ui->pathLineEdit->text()",
            handler,
        )

    def test_editor_releases_vst3_instances_instead_of_leaking_on_reload(self) -> None:
        gui = read("Editor/guis/VSTPluginFilterGUI.cpp")
        release = gui[
            gui.index("void VSTPluginFilterGUI::releasePluginInstance") :
            gui.index("void VSTPluginFilterGUI::on_pathLineEdit_editingFinished")
        ]
        self.assertIn("delete instance", release)
        self.assertNotRegex(
            release,
            r"if \(library->isVST3\(\)\)[\s\S]*?effect\s*=\s*NULL;[\s\S]*?return;",
        )

    def test_traditional_chinese_names_are_complete(self) -> None:
        translations = read("Editor/translations/Editor_zh_TW.ts")
        self.assertIn("響度校正（原版）", translations)
        self.assertIn("MIDI 控制", translations)
        self.assertNotIn("type=\"unfinished\"", translations)


if __name__ == "__main__":
    unittest.main()
