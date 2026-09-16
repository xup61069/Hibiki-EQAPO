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
                         ["Off", "Linear amplitude", "Squared amplitude", "Cubic taper (s³)",
                          "Perceptual (-60 dB)", "Follow dB"])
        gui_cpp = (ROOT / "Editor/guis/LoudnessCorrectionFilterGUI.cpp").read_text(encoding="utf-8")
        self.assertIn("ui->volumeFollowComboBox->view()->setMouseTracking(true)", gui_cpp)
        self.assertIn("ui->volumeFollowComboBox->setItemData(", gui_cpp)
        self.assertIn("Qt::ToolTipRole", gui_cpp)
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

    def test_volume_takeover_integration_in_loudness_gui(self):
        ui = ET.parse(ROOT / "Editor/guis/LoudnessCorrectionFilterGUI.ui").getroot()
        takeover_box = ui.find(".//widget[@name='takeoverVolumeCheckBox']")
        self.assertIsNotNone(takeover_box)
        self.assertIn("Take over Windows volume", takeover_box.findtext("property[@name='text']/string"))
        gui_source = (ROOT / "Editor/guis/LoudnessCorrectionFilterGUI.cpp").read_text(encoding="utf-8")
        self.assertIn("&VolumeTakeoverManager::takeoverToggled", gui_source)
        self.assertIn("VolumeTakeoverManager::instance()->setManualMode", gui_source)
        self.assertIn("void LoudnessCorrectionFilterGUI::on_takeoverVolumeCheckBox_toggled", gui_source)
        mgr_header = (ROOT / "Editor/helpers/VolumeTakeoverManager.h").read_text(encoding="utf-8")
        self.assertIn("void takeoverToggled(bool enabled);", mgr_header)
        self.assertIn("void setManualMode(bool manual", mgr_header)

    def test_volume_and_ui_synchronization_contract(self):
        gui_source = (ROOT / "Editor/guis/LoudnessCorrectionFilterGUI.cpp").read_text(encoding="utf-8")
        mgr_source = (ROOT / "Editor/helpers/VolumeTakeoverManager.cpp").read_text(encoding="utf-8")
        mgr_header = (ROOT / "Editor/helpers/VolumeTakeoverManager.h").read_text(encoding="utf-8")
        vc_header = (ROOT / "filters/loudnessCorrection/VolumeController.h").read_text(encoding="utf-8")

        # 1. VolumeController exposes getRequestedEndpointId
        self.assertIn("getRequestedEndpointId()", vc_header)

        # 2. VolumeTakeoverManager compares against requested endpoint ID
        self.assertIn("getRequestedEndpointId() == endpointId", mgr_source)

        # 3. VolumeTakeoverManager supports volumeFollowMode and uses calculateListeningVolumeDb for phon
        self.assertIn("void setVolumeFollowMode(", mgr_header)
        self.assertIn("calculateListeningVolumeDb(", mgr_source)

        # 4. LoudnessCorrectionFilterGUI updates lastEndpointState completely on external volume change
        external_lambda = gui_source.split("&VolumeTakeoverManager::volumeChangedExternal", 1)[1].split(
            "updateAutomaticVolumeUi();", 1
        )[0]
        self.assertIn("lastEndpointState.levelDb = levelDb;", external_lambda)
        self.assertIn("lastEndpointState.scalar = scalar;", external_lambda)
        self.assertIn("lastEndpointState.muted = muted;", external_lambda)

        # 5. Manual mode in on_volumeSpinBox_valueChanged does NOT set Windows endpoint volume
        manual_spin = gui_source.split("void LoudnessCorrectionFilterGUI::on_volumeSpinBox_valueChanged", 1)[1].split(
            "void LoudnessCorrectionFilterGUI::on_takeoverVolumeCheckBox_toggled", 1
        )[0]
        self.assertNotIn("volumeController->setVolume(", manual_spin)

        # 6. Volume step in VolumeTakeoverManager quantizes to integer percentage to match Windows steps
        self.assertIn("std::round(state.scalar * 100.0)", mgr_source)

        # 7. VolumeTakeoverManager lifecycle uses qApp as parent for clean destruction
        self.assertIn("s_instance = new VolumeTakeoverManager(qApp);", mgr_source)

        # 8. Rapid step and mute fallback in VolumeTakeoverManager
        self.assertIn("state.scalar = newScalar;", mgr_source)
        self.assertIn("state.muted = newMute;", mgr_source)

        # 9. VolumeTakeoverManager calculates APO follow target dB for OSD
        self.assertIn("double calculateApoFollowTargetDb(", mgr_header)
        self.assertIn("calculateApoFollowTargetDb(", mgr_source)
        self.assertIn("const double apoDb = calculateApoFollowTargetDb(", mgr_source)

    def test_original_loudness_ui_and_osd_contract(self):
        orig_header = (ROOT / "Editor/guis/OriginalLoudnessCorrectionFilterGUI.h").read_text(encoding="utf-8")
        orig_source = (ROOT / "Editor/guis/OriginalLoudnessCorrectionFilterGUI.cpp").read_text(encoding="utf-8")
        osd_source = (ROOT / "Editor/widgets/VolumeOsdWidget.cpp").read_text(encoding="utf-8")

        # 1. Original loudness GUI tracks muted state
        self.assertIn("bool volumeMuted = false;", orig_header)
        self.assertIn("volumeMuted = muted;", orig_source)
        self.assertIn('tr("Muted")', orig_source)
        self.assertIn("calibrateButton->setEnabled(!volumeMuted);", orig_source)

        # 2. OSD targets cursor screen with primary fallback
        self.assertIn("QScreen* screen = QGuiApplication::screenAt(QCursor::pos());", osd_source)
        self.assertIn("screen = QGuiApplication::primaryScreen();", osd_source)

        # 3. OSD uses 2-decimal dB readout matching GUI precision and static geometry anchoring without per-tick move()
        self.assertIn('QString::asprintf("%.2f dB  (%d%%)", targetDb, percent);', osd_source)
        fade_changed = osd_source.split("void VolumeOsdWidget::onFadeAnimationChanged", 1)[1].split(
            "void VolumeOsdWidget::onFadeAnimationFinished", 1
        )[0]
        self.assertNotIn("updateGeometryPosition()", fade_changed)
        self.assertNotIn("move(", fade_changed)


if __name__ == "__main__":
    unittest.main()
