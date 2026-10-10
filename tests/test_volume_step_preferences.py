"""Wiring and translation contracts; behavior is tested in the native Qt harness."""
from pathlib import Path
import unittest
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]


class VolumeStepPreferencesTests(unittest.TestCase):
    def test_startup_volume_is_restored_after_profiles_before_takeover(self):
        source = (ROOT / "Editor/MainWindow.cpp").read_text(encoding="utf-8")
        preferences = source.split("void MainWindow::loadPreferences()", 1)[1].split(
            "void MainWindow::savePreferences()", 1)[0]
        self.assertLess(preferences.index("load(fileList[i]);"),
                        preferences.index("restoreTakeoverVolume("))
        self.assertLess(preferences.index("restoreTakeoverVolume("),
                        preferences.index("setTakeoverEnabled(takeoverVolumeKeys)"))
        callback = source.split("&VolumeTakeoverManager::takeoverVolumeRemembered", 1)[1].split("});", 1)[0]
        self.assertIn("if (noSavePreferences) return;", callback)
        self.assertIn('settings.setValue("takeoverVolumeState", QString::fromUtf8(savedState));', callback)
        self.assertIn("settings.sync();", callback)

    def test_saved_step_is_loaded_before_takeover_and_saved_immediately(self):
        source = (ROOT / "Editor/MainWindow.cpp").read_text(encoding="utf-8")
        preferences = source.split("void MainWindow::loadPreferences()", 1)[1]
        self.assertLess(preferences.index('settings.value("volumeKeyStepPercent", 2)'),
                        preferences.index("setTakeoverEnabled(takeoverVolumeKeys)"))
        self.assertIn('settings.setValue("volumeKeyStepPercent", manager->getVolumeStepPercent());', source)
        self.assertIn("trayMenu->addAction(volumeKeyStepAction);", source)
        self.assertIn("if (dialog.exec() == QDialog::Accepted)", source)

    def test_volume_readout_does_not_write_back_through_spinbox_signal(self):
        source = (ROOT / "Editor/guis/LoudnessCorrectionFilterGUI.cpp").read_text(encoding="utf-8")
        callback = source.split("&VolumeTakeoverManager::volumeChangedExternal", 1)[1]
        self.assertLess(callback.index("QSignalBlocker volumeBlocker(ui->volumeSpinBox)"),
                        callback.index("ui->volumeSpinBox->setValue(levelDb)"))

    def test_step_strings_are_finished_in_all_editor_translations(self):
        for locale in ("de", "fr", "zh_CN", "zh_TW"):
            root = ET.parse(ROOT / f"Editor/translations/Editor_{locale}.ts").getroot()
            context = next(c for c in root.findall("context") if c.findtext("name") == "MainWindow")
            messages = {m.findtext("source"): m for m in context.findall("message")}
            for text in ("Volume key step…", "Volume key step", "Change per key press (%):"):
                with self.subTest(locale=locale, text=text):
                    translation = messages[text].find("translation")
                    self.assertTrue(translation.text)
                    self.assertNotEqual(translation.get("type"), "unfinished")

    def test_native_runtime_gate_runs_in_build_and_release(self):
        for filename in ("build.yml", "release.yml"):
            source = (ROOT / ".github/workflows" / filename).read_text(encoding="utf-8")
            self.assertIn("run: .\\scripts\\test-volume-takeover.ps1", source)


if __name__ == "__main__":
    unittest.main()
