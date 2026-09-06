import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MAKENSIS = ROOT / "third_party" / "nsis-3.11" / "makensis.exe"
HARNESS_SOURCE = ROOT / "tests" / "asio-installer-guid-harness.nsi"


@unittest.skipUnless(MAKENSIS.is_file(), "bootstrapped NSIS is required")
class AsioInstallerGuidValidatorTests(unittest.TestCase):
    def test_accepts_only_parseable_braced_clsids(self):
        with tempfile.TemporaryDirectory() as directory:
            harness = Path(directory) / "asio-installer-guid-harness.exe"
            compile_result = subprocess.run(
                [
                    str(MAKENSIS),
                    "/WX",
                    "/INPUTCHARSET",
                    "UTF8",
                    f"/DHARNESS_OUTPUT={harness}",
                    str(HARNESS_SOURCE),
                ],
                cwd=ROOT,
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(
                compile_result.returncode,
                0,
                compile_result.stderr + compile_result.stdout,
            )

            def run(value: str) -> subprocess.CompletedProcess[str]:
                return subprocess.run(
                    [str(harness), f"/CLSID={value}"],
                    cwd=ROOT,
                    capture_output=True,
                    text=True,
                    check=False,
                )

            self.assertEqual(
                run("{D47C55C9-3F7D-422F-86E9-32E170815D53}").returncode,
                0,
            )
            for malformed in (
                "",
                "D47C55C9-3F7D-422F-86E9-32E170815D53",
                "{ZZZZZZZZ-ZZZZ-ZZZZ-ZZZZ-ZZZZZZZZZZZZ}",
                "{D47C55C9_3F7D_422F_86E9_32E170815D53}",
            ):
                self.assertNotEqual(run(malformed).returncode, 0, malformed)


if __name__ == "__main__":
    unittest.main()
