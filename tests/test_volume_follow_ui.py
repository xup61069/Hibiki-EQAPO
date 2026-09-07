"""Keep source/target volume readouts and contour previews on one contract."""

from pathlib import Path
import unittest
import xml.etree.ElementTree as ET


ROOT = Path(__file__).resolve().parents[1]


class VolumeFollowUiTests(unittest.TestCase):
    def test_target_readout_uses_complete_snapshot_and_shared_gain(self):
        source = (ROOT / "Editor/guis/LoudnessCorrectionFilterGUI.cpp").read_text(encoding="utf-8")
        readout = source.split("void LoudnessCorrectionFilterGUI::updateVolumeReadout()", 1)[1].split(
            "void LoudnessCorrectionFilterGUI::on_refLevel", 1
        )[0]
        for token in ("calculateVolumeFollowGain", "lastEndpointState.levelDb",
                      "lastEndpointState.scalar", "lastEndpointState.muted",
                      "unknown (source unavailable)", "not a measurement", "if (!state)"):
            self.assertIn(token, readout)
        timer = source.split("void LoudnessCorrectionFilterGUI::updateVolume()", 1)[1]
        self.assertIn("updateVolumeReadout();", timer)

    def test_manual_to_automatic_reuses_single_endpoint_guards(self):
        source = (ROOT / "Editor/guis/LoudnessCorrectionFilterGUI.cpp").read_text(encoding="utf-8")
        toggle = source.split("void LoudnessCorrectionFilterGUI::on_manualVolumeCheckBox_toggled", 1)[1].split(
            "void LoudnessCorrectionFilterGUI::on_volumeSpinBox", 1
        )[0]
        self.assertIn("refreshVolumeController();", toggle)
        self.assertNotIn("new VolumeController", toggle)
        self.assertIn("if (!automaticVolumeAvailable)", toggle)

    def test_widgets_and_studio_use_explicit_target_semantics(self):
        ui = ET.parse(ROOT / "Editor/guis/LoudnessCorrectionFilterGUI.ui").getroot()
        combo = ui.find(".//widget[@name='volumeFollowComboBox']")
        self.assertEqual([node.text for node in combo.findall("item/property/string")],
                         ["Off", "Linear amplitude", "Squared amplitude", "Follow dB"])
        for name in ("volumeFollowStatusLabel", "volumeSourceLabel"):
            label = ui.find(f".//widget[@name='{name}']")
            self.assertEqual(label.findtext("property[@name='wordWrap']/bool"), "true")
        studio = (ROOT / "Editor/guis/LoudnessCorrectionStudioDialog.cpp").read_text(encoding="utf-8")
        self.assertIn("calculateListeningVolumeDb(", studio)
        self.assertIn("setValue(automaticVolumeDb)", studio)
        self.assertIn("Preview uses the volume snapshot", studio)
        self.assertNotIn("referenceLevel + ui->volumeSpinBox->value()", studio)
        snapshots = (ROOT / "Editor/MainWindow.cpp").read_text(encoding="utf-8")
        snapshot_entry = (ROOT / "helpers/UiSnapshot.h").read_text(encoding="utf-8")
        self.assertIn('value == QStringLiteral("volume-follow")', snapshot_entry)
        self.assertIn('scenario == QStringLiteral("volume-follow")', snapshots)
        for expected in ("-6.02", "-12.04", "-50.00", "-20.00"):
            self.assertIn(f'target->text().contains("{expected}")', snapshots)


if __name__ == "__main__":
    unittest.main()
