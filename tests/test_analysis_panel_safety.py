#!/usr/bin/env python3
"""Focused source contracts for analysis-panel recovery and safe actions."""

from __future__ import annotations

import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
MAIN_CPP = (ROOT / "Editor" / "MainWindow.cpp").read_text(encoding="utf-8")
MAIN_H = (ROOT / "Editor" / "MainWindow.h").read_text(encoding="utf-8")
SCENE_CPP = (ROOT / "Editor" / "AnalysisPlotScene.cpp").read_text(
    encoding="utf-8"
)
THREAD_CPP = (ROOT / "Editor" / "AnalysisThread.cpp").read_text(
    encoding="utf-8"
)
THREAD_H = (ROOT / "Editor" / "AnalysisThread.h").read_text(encoding="utf-8")
TABLE_CPP = (ROOT / "Editor" / "FilterTable.cpp").read_text(encoding="utf-8")
ENGINE_CPP = (ROOT / "FilterEngine.cpp").read_text(encoding="utf-8")
FILTER_INTERFACE = (ROOT / "IFilter.h").read_text(encoding="utf-8")
LOUDNESS_CPP = (
    ROOT / "filters" / "loudnessCorrection" / "LoudnessCorrectionFilter.cpp"
).read_text(encoding="utf-8")


class AnalysisPanelSafetyTests(unittest.TestCase):
    def test_analysis_loading_uses_compact_accessible_status_without_a_bar(self) -> None:
        self.assertNotIn("QProgressBar", MAIN_CPP)
        self.assertNotIn("QProgressBar", MAIN_H)
        self.assertNotIn("headroomMeter", MAIN_CPP)
        self.assertNotIn("headroomMeter", MAIN_H)

        accessibility = MAIN_CPP[
            MAIN_CPP.index("class AnalysisStatusAccessible") : MAIN_CPP.index(
                "static QByteArray serializeConfigurationLines"
            )
        ]
        for token in (
            'property("analysisBusy").toBool()',
            "accessibleState.busy",
            "QAccessibleStateChangeEvent",
            "QAccessible::updateAccessibility",
            "label->setAccessibleDescription(text)",
            'label->setProperty("analysisBusy", busy)',
        ):
            self.assertIn(token, accessibility)

        constructor = MAIN_CPP[
            MAIN_CPP.index("MainWindow::MainWindow") : MAIN_CPP.index(
                "MainWindow::~MainWindow"
            )
        ]
        self.assertIn('setAccessibleName(tr("Analysis status"))', constructor)
        self.assertIn("ensureAnalysisStatusAccessibility();", constructor)

        start = MAIN_CPP[
            MAIN_CPP.index("void MainWindow::startAnalysis") : MAIN_CPP.index(
                "void MainWindow::loadPreferences"
            )
        ]
        analyzing = start.index('tr("Analyzing the current signal path…")')
        self.assertLess(start.rfind("setAnalysisStatus(", 0, analyzing), analyzing)
        self.assertIn('"normal",\n\t\ttrue);', start[analyzing:])
        self.assertIn('"warning",\n\t\t\tfalse);', start[analyzing:])

    def test_floating_analysis_panel_has_explicit_bottom_dock_recovery(self) -> None:
        constructor = MAIN_CPP[
            MAIN_CPP.index("MainWindow::MainWindow") : MAIN_CPP.index(
                "MainWindow::~MainWindow"
            )
        ]
        for token in (
            "setDockNestingEnabled(true)",
            "setAllowedAreas(Qt::AllDockWidgetAreas)",
            "QDockWidget::DockWidgetClosable",
            "QDockWidget::DockWidgetMovable",
            "QDockWidget::DockWidgetFloatable",
            'QStringLiteral("actionDockAnalysisPanel")',
            "QDockWidget::topLevelChanged",
            "QDockWidget::dockLocationChanged",
        ):
            self.assertIn(token, constructor)

        recovery = MAIN_CPP[
            MAIN_CPP.index("void MainWindow::dockAnalysisPanel()") : MAIN_CPP.index(
                "void MainWindow::lowerPreampToPreventClipping()"
            )
        ]
        self.assertIn("addDockWidget(Qt::BottomDockWidgetArea", recovery)
        self.assertIn("setFloating(false)", recovery)
        self.assertIn("->show()", recovery)
        self.assertIn("->raise()", recovery)
        self.assertNotIn("Qt::RightDockWidgetArea", recovery)

    def test_response_animation_is_latest_wins_and_display_only(self) -> None:
        update = SCENE_CPP[
            SCENE_CPP.index("void AnalysisPlotScene::setFreqData") : SCENE_CPP.index(
                "const vector<FilterNode>& AnalysisPlotScene::getNodes"
            )
        ]
        self.assertIn("QVariantAnimation", SCENE_CPP)
        self.assertIn("setDuration(180)", SCENE_CPP)
        self.assertIn("QEasingCurve::OutCubic", SCENE_CPP)
        self.assertIn("responseAnimation->currentValue()", update)
        handoff = update[
            update.index("if (responseAnimation->state()") :
            update.index("if (nodes.empty()")
        ]
        self.assertLess(
            handoff.index("setResponseAnimationProgress(responseAnimation->currentValue()"),
            handoff.index("responseAnimation->stop()"),
        )
        self.assertLess(
            handoff.index("responseAnimation->stop()"),
            update.index("responseAnimation->start()"),
        )
        self.assertNotIn("QQueue", SCENE_CPP)
        self.assertNotIn("AnalysisThread", SCENE_CPP)
        self.assertNotIn("FilterEngine", SCENE_CPP)

    def test_response_animation_respects_motion_and_snapshot_policies(self) -> None:
        policy = SCENE_CPP[
            SCENE_CPP.index("bool AnalysisPlotScene::responseAnimationAllowed") :
            SCENE_CPP.index("bool AnalysisPlotScene::responseShapesMatch")
        ]
        for token in (
            "UiSnapshot::requested()",
            'property("eqapoDisableAnimations")',
            "QStyle::SH_Widget_Animate",
            "SPI_GETCLIENTAREAANIMATION",
        ):
            self.assertIn(token, policy)

    def test_analysis_results_are_generation_and_identity_bound(self) -> None:
        self.assertIn("quint64 setParameters", THREAD_H)
        self.assertIn("quint64 getResultGeneration() const", THREAD_H)
        self.assertIn("++requestGeneration", THREAD_CPP)
        self.assertIn("this->resultGeneration = generation", THREAD_CPP)

        update = MAIN_CPP[
            MAIN_CPP.index("void MainWindow::updateAnalysisPanel") : MAIN_CPP.index(
                "void MainWindow::on_mainToolBar_visibilityChanged"
            )
        ]
        self.assertIn("resultGeneration != requestedAnalysisGeneration", update)
        self.assertLess(
            update.index("resultGeneration != requestedAnalysisGeneration"),
            update.index("analysisPlotScene->setFreqData"),
        )

        eligibility = MAIN_CPP[
            MAIN_CPP.index("bool MainWindow::analysisResultCanAdjustPreamp") :
            MAIN_CPP.index("void MainWindow::refreshAutoPreampActionState")
        ]
        for token in (
            "requestedAnalysisConfigPath",
            "requestedAnalysisLinesHash",
            "requestedAnalysisDeviceId",
            "requestedAnalysisChannelMask",
            "requestedAnalysisChannelIndex",
            "requestedAnalysisStartFrom",
            "acceptedAnalysisGeneration != requestedAnalysisGeneration",
            "isFilterTableDirty(filterTable)",
            "showingComparisonA",
            "!bypassTable.isNull()",
            "restoringTemporaryState",
            "analysisFilesStillMatch",
            "analysisRootMatchesEditor",
            "analysisVolumesStillMatch",
            "analysisTopologySupportsAutoPreamp",
        ):
            self.assertIn(token, eligibility)

    def test_analysis_provenance_covers_root_includes_and_automatic_volume(self) -> None:
        for token in (
            "loadedConfigurationFiles.clear()",
            "loadedConfigurationFiles.push_back({path, string(), false})",
            "loadedConfigurationFiles.push_back({path, inputStream.str(), false})",
            "loadedConfigurationFiles.push_back({path, inputStream.str(), true})",
            "runtimeContext.volumeObservations = &runtimeVolumeObservations",
        ):
            self.assertIn(token, ENGINE_CPP)
        for token in (
            "observation.requestedEndpointId = getVolumeControllerEndpointId()",
            "observation.resolvedEndpointId = volumeController.getEndpointId()",
            "observation.volumeDb =",
            "observation.volumeScalar =",
            "observation.muted =",
            "observation.available = false",
            "observation.available = SUCCEEDED(volumeResult)",
        ):
            self.assertIn(token, LOUDNESS_CPP)

        for source in (FILTER_INTERFACE, THREAD_H):
            self.assertIn("double volumeScalar", source)
            self.assertIn("bool muted", source)
        self.assertIn(
            "snapshot.volumeScalar = observation.volumeScalar", THREAD_CPP
        )
        self.assertIn("snapshot.muted = observation.muted", THREAD_CPP)

        file_check = MAIN_CPP[
            MAIN_CPP.index("static bool analysisFilesStillMatch") : MAIN_CPP.index(
                "static bool analysisRootMatchesEditor"
            )
        ]
        self.assertIn("contents == snapshot.contents", file_check)
        root_check = MAIN_CPP[
            MAIN_CPP.index("static bool analysisRootMatchesEditor") : MAIN_CPP.index(
                "static bool analysisVolumesStillMatch"
            )
        ]
        self.assertIn("configurationPathKey(snapshot.path)", root_check)
        self.assertIn("deserializeConfigurationLines(snapshot.contents) != editorLines", root_check)
        self.assertIn("matchingSnapshots == 1", root_check)
        volume_check = MAIN_CPP[
            MAIN_CPP.index("static bool analysisVolumesStillMatch") : MAIN_CPP.index(
                "static bool analysisTopologySupportsAutoPreamp"
            )
        ]
        self.assertIn("!snapshot.available", volume_check)
        self.assertIn("snapshot.requestedEndpointId", volume_check)
        self.assertIn("snapshot.resolvedEndpointId", volume_check)
        self.assertIn("getVolumeState(", volume_check)
        self.assertIn("snapshot.volumeDb", volume_check)
        self.assertIn("snapshot.volumeScalar", volume_check)
        self.assertIn("snapshot.muted", volume_check)

    def test_auto_preamp_topology_is_an_explicit_fail_closed_allowlist(self) -> None:
        topology = MAIN_CPP[
            MAIN_CPP.index("static bool analysisTopologySupportsAutoPreamp") : MAIN_CPP.index(
                "namespace", MAIN_CPP.index("static bool analysisTopologySupportsAutoPreamp")
            )
        ]
        for command in (
            "device",
            "include",
            "channel",
            "preamp",
            "parametriceq",
            "graphiceq",
            "delay",
            "loudnesscorrection",
            "vumeter",
            "headphonecalibration",
        ):
            self.assertIn(f'QStringLiteral("{command}")', topology)
        self.assertIn("snapshot.contents.contains('`')", topology)
        self.assertIn("!supportedCommands.contains(normalizedCommand)", topology)
        self.assertIn('startsWith(QStringLiteral("filter"))', topology)
        for unsafe_command in (
            "copy",
            "pan",
            "crossfeed",
            "chorus",
            "reverb",
            "tonegenerator",
            "convolution",
            "vstplugin",
            "outprocvstplugin",
            "outputguard",
            "stage",
            "if",
            "eval",
        ):
            self.assertNotIn(f'QStringLiteral("{unsafe_command}")', topology)

    def test_preamp_plan_edits_only_active_root_lines_and_compares_before_apply(self) -> None:
        parser = TABLE_CPP[
            TABLE_CPP.index("const QSet<QString>& preampScopeBoundaries") :
            TABLE_CPP.index("bool FilterTable::planPreampReduction")
        ]
        plan = TABLE_CPP[
            TABLE_CPP.index("bool FilterTable::planPreampReduction") : TABLE_CPP.index(
                "bool FilterTable::applyPreampReduction"
            )
        ]
        apply = TABLE_CPP[
            TABLE_CPP.index("bool FilterTable::applyPreampReduction") : TABLE_CPP.index(
                "bool FilterTable::setLines"
            )
        ]
        self.assertIn("trimmed.startsWith('#')", plan)
        for boundary in (
            "channel",
            "device",
            "if",
            "elseif",
            "else",
            "endif",
            "include",
            "stage",
        ):
            self.assertIn(f'QStringLiteral("{boundary}")', parser)
        self.assertNotIn("qobject_cast<PreampFilterGUI", plan + apply)
        self.assertIn("*plan = PreampAdjustmentPlan()", plan)
        self.assertIn("line.contains('`')", parser)
        self.assertIn("parseEditablePreampLine", plan)
        self.assertIn("parseEditablePreampLine", apply)
        self.assertIn("-0x1p2", parser)
        self.assertIn(r"(?:dB)?\s*(?:#.*)?", parser)
        self.assertIn("std::floor(", plan)
        self.assertIn("existing value with finer precision", plan)
        self.assertIn("oldDbGain - targetDbGain < reductionDb", plan)
        self.assertIn("oldDbGain - targetDbGain >= reductionDb", plan)
        self.assertIn("hasScopeBoundary", plan)
        self.assertIn("Keep searching", plan)
        self.assertIn("items[plan.itemIndex]->text != plan.originalLine", apply)
        self.assertIn("!std::isfinite(plan.oldDbGain)", apply)
        self.assertIn("serializedTargetGain != plan.targetDbGain", apply)
        self.assertIn("serializedTargetGain >= parsedOldGain", apply)
        self.assertIn("serializedTargetGain >= 0.0", apply)
        self.assertIn("isPreampScopeBoundary(items[index]->text)", apply)
        self.assertIn("index <= plan.itemIndex", apply)
        self.assertIn("items.first()", apply)
        self.assertIn("updateGuis()", apply)
        self.assertIn("updateModel()", apply)

    def test_one_shot_cut_is_conservative_confirmed_and_never_boosts(self) -> None:
        quantizer = MAIN_CPP[
            MAIN_CPP.index("static double conservativePreampReduction") :
            MAIN_CPP.index("namespace", MAIN_CPP.index("static double conservativePreampReduction"))
        ]
        self.assertIn("std::ceil(peakGain * 100.0", quantizer)
        self.assertIn("(std::max)(", quantizer)
        self.assertIn("0.01, std::ceil(peakGain * 100.0", quantizer)
        self.assertIn("reduction < peakGain", quantizer)
        self.assertIn("reduction >= peakGain ? reduction : 0.0", quantizer)
        self.assertNotIn("1e-9", quantizer)

        action = MAIN_CPP[
            MAIN_CPP.index("void MainWindow::lowerPreampToPreventClipping") :
            MAIN_CPP.index("void MainWindow::languageSelected")
        ]
        self.assertIn("analysisResultCanAdjustPreamp()", action)
        self.assertIn("QMessageBox::Apply | QMessageBox::Cancel", action)
        self.assertIn("plan.oldDbGain", action)
        self.assertIn("plan.targetDbGain", action)
        self.assertIn("appliedReductionDb", action)
        self.assertIn("selected channel", action)
        self.assertIn("intersample peaks", action)
        self.assertGreaterEqual(action.count("planPreampReduction"), 2)
        self.assertIn("applyPreampReduction(currentPlan)", action)
        self.assertIn("save the file to apply it", action)

        lines_changed = MAIN_CPP[
            MAIN_CPP.index("void MainWindow::linesChanged") : MAIN_CPP.index(
                "bool MainWindow::on_tabWidget_tabCloseRequested"
            )
        ]
        self.assertIn(
            "&& !applyingAutoPreampAdjustment", " ".join(lines_changed.split())
        )
        self.assertIn(
            "QScopedValueRollback<bool> adjustmentGuard(applyingAutoPreampAdjustment, true)",
            action,
        )
        self.assertIn("leaves the editor dirty in every mode", action)

        self.assertIn("invalidateAnalysisResult();", MAIN_CPP)
        self.assertIn("autoPreampButton->setEnabled(false)", MAIN_CPP)
        self.assertIn("QPointer<FilterTable> requestedAnalysisTable", MAIN_H)
        snapshot_validator = MAIN_CPP[
            MAIN_CPP.index("bool MainWindow::snapshotLayoutIsValid") :
            MAIN_CPP.index("void MainWindow::getDeviceAndChannelMask")
        ]
        self.assertIn("autoPreampButton->parentWidget()", snapshot_validator)
        self.assertIn("fitsInsideContainer", snapshot_validator)


if __name__ == "__main__":
    unittest.main()
