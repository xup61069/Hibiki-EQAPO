"""Contract tests for startup autostart and fullscreen volume takeover controls."""

from pathlib import Path
import unittest
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]


class VolumeTakeoverFullscreenAutostartTests(unittest.TestCase):
    def test_main_supports_tray_startup_without_immediate_show(self):
        source = (ROOT / "Editor/main.cpp").read_text(encoding="utf-8")
        self.assertIn('arg == QStringLiteral("--tray")', source)
        self.assertIn('arg == QStringLiteral("--minimized")', source)
        self.assertIn('if (!startInTray || snapshotMode)', source)
        self.assertIn('w.show();', source)

    def test_mainwindow_start_with_windows_contract(self):
        header = (ROOT / "Editor/MainWindow.h").read_text(encoding="utf-8")
        self.assertIn("void startWithWindowsToggled(bool enabled);", header)
        self.assertIn("bool isStartWithWindowsEnabled() const;", header)
        self.assertIn("void setStartWithWindows(bool enabled);", header)
        self.assertIn("QAction* startWithWindowsAction = NULL;", header)
        self.assertIn("bool startWithWindows = false;", header)

        source = (ROOT / "Editor/MainWindow.cpp").read_text(encoding="utf-8")
        self.assertIn('startWithWindowsAction = ui->menuSettings->addAction(tr("Start with Windows"));', source)
        self.assertIn('trayMenu->addAction(startWithWindowsAction);', source)
        self.assertIn('QApplication::setQuitOnLastWindowClosed(false);', source)
        self.assertIn('HKEY_CURRENT_USER', source)
        self.assertIn('CurrentVersion', source)
        self.assertIn('settings.setValue("startWithWindows", startWithWindows);', source)
        self.assertIn('if (!startWithWindows)', source)
        self.assertIn('setStartWithWindows(true);', source)

    def test_low_level_keyboard_hook_is_non_blocking_async(self):
        source = (ROOT / "Editor/helpers/VolumeTakeoverManager.cpp").read_text(encoding="utf-8")
        hook_proc = source.split("LRESULT CALLBACK VolumeTakeoverManager::LowLevelKeyboardProc", 1)[1].split(
            "double VolumeTakeoverManager::calculateApoFollowTargetDb", 1
        )[0]
        self.assertIn("QMetaObject::invokeMethod", hook_proc)
        self.assertIn("Qt::QueuedConnection", hook_proc)
        self.assertIn("mgr->stepVolume(true);", hook_proc)
        self.assertIn("mgr->stepVolume(false);", hook_proc)
        self.assertIn("mgr->toggleMute();", hook_proc)
        self.assertNotIn("s_instance->stepVolume(true);", hook_proc)
        self.assertNotIn("s_instance->stepVolume(false);", hook_proc)

    def test_fullscreen_bidirectional_endpoint_sync_and_watchdog(self):
        source = (ROOT / "Editor/helpers/VolumeTakeoverManager.cpp").read_text(encoding="utf-8")
        check_proc = source.split("void VolumeTakeoverManager::checkVolumeChange()", 1)[1].split(
            "if (manualMode)", 1
        )[0]
        self.assertIn("if (s_keyboardHook == NULL)", check_proc)
        self.assertIn("installHook();", check_proc)
        self.assertIn("currentRealState.scalar < 0.999", check_proc)
        self.assertIn("currentRealState.muted != takeoverMuted", check_proc)
        self.assertIn("publishTakeoverSharedData();", check_proc)
        self.assertIn("enforceWindowsVolume100();", check_proc)

    def test_osd_uses_noactivate_for_fullscreen_safety(self):
        source = (ROOT / "Editor/widgets/VolumeOsdWidget.cpp").read_text(encoding="utf-8")
        self.assertIn("WS_EX_NOACTIVATE", source)
        self.assertIn("SWP_NOACTIVATE", source)

    def test_translations_contain_start_with_windows(self):
        tw_ts = (ROOT / "Editor/translations/Editor_zh_TW.ts").read_text(encoding="utf-8")
        self.assertIn("<source>Start with Windows</source>", tw_ts)
        self.assertIn("<translation>隨 Windows 開機啟動</translation>", tw_ts)
        self.assertNotIn("<translation type=\"unfinished\">隨 Windows 開機啟動</translation>", tw_ts)

        cn_ts = (ROOT / "Editor/translations/Editor_zh_CN.ts").read_text(encoding="utf-8")
        self.assertIn("<source>Start with Windows</source>", cn_ts)
        self.assertIn("<translation>随 Windows 开机启动</translation>", cn_ts)


if __name__ == "__main__":
    unittest.main()
