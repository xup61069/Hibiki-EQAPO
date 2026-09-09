"""Contracts for the Configuration Editor ASIO target control."""
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


class AsioEditorControlTests(unittest.TestCase):
    def setUp(self):
        self.window = (ROOT / "Editor/MainWindow.cpp").read_text(encoding="utf-8")
        self.project = (ROOT / "Editor/Editor.pro").read_text(encoding="utf-8")
        self.store = (ROOT / "HibikiEQAPODriver/AsioTargetStore.cpp").read_text(
            encoding="utf-8"
        )

    def test_editor_reuses_proxy_discovery_and_is_x64_gated(self):
        self.assertIn("contains(QT_ARCH, x86_64)", self.project)
        self.assertIn("../HibikiEQAPODriver/AsioTargetStore.cpp", self.project)
        self.assertIn("../HibikiEQAPODriver/AsioTargetSelection.cpp", self.project)
        self.assertIn("#if defined(_M_AMD64)", self.window)
        self.assertIn("HibikiAsio::inspectAsioTargets", self.window)

    def test_control_is_visible_and_explains_reopen_semantics(self):
        self.assertIn('QStringLiteral("asioDeviceComboBox")', self.window)
        self.assertIn('tr("ASIO device:")', self.window)
        self.assertIn(
            'tr("ASIO device saved. Reopen the DAW audio device to apply.")',
            self.window,
        )
        self.assertNotIn("new StudioSignature(studioHeader)", self.window)

    def test_candidate_validation_does_not_load_vendor_code(self):
        discovery = self.store.split(
            "std::vector<AsioDriverCandidate> controllableCandidates(", 2
        )[2].split("std::wstring modulePath", 1)[0]
        self.assertIn("candidate.is64Bit", discovery)
        self.assertIn("candidate.isSamePhysicalFile", discovery)
        self.assertIn("HKEY_CLASSES_ROOT", discovery)
        self.assertIn("samePhysicalFile(candidate.dllPath", discovery)
        for forbidden in ("LoadLibrary", "CoCreateInstance"):
            self.assertNotIn(forbidden, discovery)

    def test_editor_writes_only_current_user_override(self):
        setter = self.store.split("AsioTargetUpdateStatus setUserAsioTarget(", 1)[1]
        setter = setter.split("AsioTargetUpdateStatus clearUserAsioTarget(", 1)[0]
        self.assertIn("HKEY_CURRENT_USER", setter)
        self.assertIn("kTargetRegistryKey", setter)
        self.assertIn("kTargetClsidValue", setter)
        self.assertIn("RegSetValueExW", setter)
        self.assertNotIn("HKEY_LOCAL_MACHINE", setter)
        self.assertIn("state.candidates", setter)

        clearer = self.store.split("AsioTargetUpdateStatus clearUserAsioTarget(", 1)[1]
        self.assertIn("HKEY_CURRENT_USER", clearer)
        self.assertIn("RegDeleteValueW", clearer)
        self.assertNotIn("RegDeleteKey", clearer)
        self.assertNotIn("HKEY_LOCAL_MACHINE", clearer)


if __name__ == "__main__":
    unittest.main()
