"""Cross-layer contracts for configuration-owned signal precision."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]


class ProcessingPrecisionTests(unittest.TestCase):
    def test_single_callback_has_no_allocation_or_platform_io(self):
        source = (ROOT / "FilterConfiguration.cpp").read_text(encoding="utf-8")
        callback = source.split("void FilterConfiguration::processSingle(", 1)[1].split(
            "unsigned FilterConfiguration::doTransition", 1
        )[0]
        for forbidden in ("MemoryHelper::alloc", "MemoryHelper::free", "new ",
                          "resize(", "push_back(", "LogF(", "WaitFor", "CreateFile"):
            self.assertNotIn(forbidden, callback)
        self.assertIn("->processSingle(", callback)
        self.assertIn("->process(currentSamples2, currentSamples, frameCount)", callback)

    def test_precision_is_configuration_owned_and_defaults_to_double(self):
        engine = (ROOT / "FilterEngine.cpp").read_text(encoding="utf-8")
        header = (ROOT / "FilterEngine.h").read_text(encoding="utf-8")
        config = (ROOT / "FilterConfiguration.cpp").read_text(encoding="utf-8")
        self.assertIn("bool loadingSinglePrecision = false;", header)
        self.assertIn("loadingSinglePrecision = false;", engine)
        self.assertIn("singlePrecision(engine->getLoadingSinglePrecision())", config)
        self.assertIn("precisionDirectiveSeen ||", engine)
        self.assertIn('precision != L"32" && precision != L"64"', engine)
        self.assertIn("throw ConfigurationFileLoadError();", engine)

    def test_editor_uses_serialized_model_and_guards_ambiguous_scopes(self):
        table = (ROOT / "Editor/FilterTable.cpp").read_text(encoding="utf-8")
        window = (ROOT / "Editor/MainWindow.cpp").read_text(encoding="utf-8")
        self.assertIn('QStringLiteral("ProcessingPrecision: %1")', table)
        self.assertIn("if (!processingPrecisionEditable()) return false;", table)
        self.assertIn("filterTable->processingPrecisionEditable()", window)
        self.assertIn("table->setDoublePrecision(enabled)", window)
        self.assertIn('QStringLiteral("doublePrecisionCheckBox")', window)
        self.assertIn(
            "doublePrecisionCheckBox->setToolTip(doublePrecisionAction->toolTip())",
            window,
        )
        self.assertIn(
            "doublePrecisionCheckBox, &QCheckBox::clicked,\n"
            "\t\tdoublePrecisionAction, &QAction::setChecked",
            window,
        )
        method = table.split("bool FilterTable::setDoublePrecision(", 1)[1].split(
            "bool FilterTable::setLines(", 1
        )[0]
        self.assertIn("updateModel();", method)
        self.assertNotIn("QSettings", method)

    def test_header_switch_tracks_text_edits(self):
        window = (ROOT / "Editor/MainWindow.cpp").read_text(encoding="utf-8")
        slot = window.split("void MainWindow::linesChanged()", 1)[1].split(
            "bool MainWindow::on_tabWidget_tabCloseRequested(", 1
        )[0]
        self.assertIn("refreshWorkspaceActionState()", slot)
        self.assertIn("currentFilterTable()", slot)

    def test_header_switch_reflects_precision_state(self):
        window = (ROOT / "Editor/MainWindow.cpp").read_text(encoding="utf-8")
        table = (ROOT / "Editor/FilterTable.cpp").read_text(encoding="utf-8")
        refresh = window.split("void MainWindow::refreshWorkspaceActionState()", 1)[1].split(
            "void MainWindow::invalidateAnalysisResult()", 1
        )[0]
        self.assertIn("doublePrecisionCheckBox->setChecked(doublePrecision);", refresh)
        self.assertIn("doublePrecisionAction->setChecked(doublePrecision);", refresh)
        self.assertIn('doublePrecisionCheckBox->setText(tr("Double precision"));', refresh)
        self.assertIn('tr("Processing precision (see configuration)")', refresh)

        editable = table.split("bool FilterTable::processingPrecisionEditable() const", 1)[1].split(
            "bool FilterTable::setDoublePrecision(", 1
        )[0]
        self.assertIn('"Include"', editable)
        self.assertIn('"If"', editable)
        self.assertNotIn('"Device"', editable)
        self.assertNotIn('"Stage"', editable)


if __name__ == "__main__":
    unittest.main()
