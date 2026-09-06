import os
import subprocess
import tempfile
import unittest
import uuid
from pathlib import Path

if os.name == "nt":
    import winreg


ROOT = Path(__file__).resolve().parents[1]
MAKENSIS = ROOT / "third_party" / "nsis-3.11" / "makensis.exe"
HARNESS_SOURCE = ROOT / "tests" / "asio-installer-discovery-harness.nsi"
BUILD_SCRIPT = ROOT / "scripts" / "build-installer-x64.ps1"
FIXTURE_A_CLSID = "{11111111-1111-4111-8111-111111111111}"
MISSING_CLSID = "{33333333-3333-4333-8333-333333333333}"
REGISTRY_TEST_PARENT = r"Software\HibikiEQAPO\Tests\AsioDiscovery"


def write_minimal_amd64_dll(path: Path) -> None:
    """Write the smallest PE32+ DLL accepted by the production NSIS parser."""
    image = bytearray(512)
    image[0:2] = b"MZ"
    image[0x3C:0x40] = (0x80).to_bytes(4, "little")
    image[0x80:0x84] = b"PE\0\0"
    image[0x84:0x86] = (0x8664).to_bytes(2, "little")
    image[0x86:0x88] = (1).to_bytes(2, "little")
    image[0x94:0x96] = (0xF0).to_bytes(2, "little")
    image[0x96:0x98] = (0x2000).to_bytes(2, "little")
    image[0x98:0x9A] = (0x20B).to_bytes(2, "little")
    image[0x98 + 60 : 0x98 + 64] = (0x200).to_bytes(4, "little")
    path.write_bytes(image)


def parse_result(path: Path) -> dict[str, str]:
    values: dict[str, str] = {}
    for line in path.read_text(encoding="utf-8-sig").splitlines():
        key, separator, value = line.partition("=")
        if not separator:
            raise AssertionError(f"Malformed harness result line: {line!r}")
        values[key] = value
    return values


def registry_key_exists(path: str) -> bool:
    try:
        with winreg.OpenKey(
            winreg.HKEY_CURRENT_USER,
            path,
            0,
            winreg.KEY_READ | winreg.KEY_WOW64_64KEY,
        ):
            return True
    except FileNotFoundError:
        return False


def delete_registry_tree(path: str) -> None:
    try:
        with winreg.OpenKey(
            winreg.HKEY_CURRENT_USER,
            path,
            0,
            winreg.KEY_READ | winreg.KEY_WRITE | winreg.KEY_WOW64_64KEY,
        ) as key:
            children: list[str] = []
            index = 0
            while True:
                try:
                    children.append(winreg.EnumKey(key, index))
                except OSError:
                    break
                index += 1
    except FileNotFoundError:
        return

    for child in children:
        delete_registry_tree(path + "\\" + child)
    winreg.DeleteKeyEx(
        winreg.HKEY_CURRENT_USER,
        path,
        access=winreg.KEY_WOW64_64KEY,
    )


class AsioInstallerDiscoveryBuildContractTests(unittest.TestCase):
    def test_installer_build_runs_discovery_regression_after_nsis_bootstrap(self):
        source = BUILD_SCRIPT.read_text(encoding="utf-8")
        bootstrap = source.index("-WithQt -WithNsis")
        regression = source.index("test_asio_installer_discovery_runtime.py")
        native_build = source.index('"build-local-x64.ps1"')
        self.assertLess(bootstrap, regression)
        self.assertLess(regression, native_build)


@unittest.skipUnless(
    os.name == "nt" and MAKENSIS.is_file(),
    "Windows and bootstrapped NSIS are required",
)
class AsioInstallerDiscoveryRuntimeTests(unittest.TestCase):
    def test_two_isolated_amd64_drivers_are_resolved_without_silent_guessing(self):
        with tempfile.TemporaryDirectory(prefix="hibiki-asio-discovery-") as directory:
            temp = Path(directory)
            run_id = uuid.uuid4().hex
            registry_path = REGISTRY_TEST_PARENT + "\\" + run_id
            self.addCleanup(delete_registry_tree, registry_path)
            fixture_a = temp / "fixture-alpha.dll"
            fixture_b = temp / "fixture-beta.dll"
            harness = temp / "asio-installer-discovery-harness.exe"
            write_minimal_amd64_dll(fixture_a)
            write_minimal_amd64_dll(fixture_b)

            compile_result = subprocess.run(
                [
                    str(MAKENSIS),
                    "/WX",
                    "/INPUTCHARSET",
                    "UTF8",
                    f"/DHARNESS_OUTPUT={harness}",
                    f"/DHARNESS_FIXTURE_A={fixture_a}",
                    f"/DHARNESS_FIXTURE_B={fixture_b}",
                    f"/DHARNESS_RUN_ID={run_id}",
                    str(HARNESS_SOURCE),
                ],
                cwd=ROOT,
                capture_output=True,
                text=True,
                check=False,
                timeout=10,
            )
            self.assertEqual(
                compile_result.returncode,
                0,
                compile_result.stderr + compile_result.stdout,
            )

            cases = (
                (
                    "ambiguous",
                    [],
                    {
                        "requested": "0",
                        "selected": "",
                        "selected_name": "",
                        "target_valid": "",
                        "recovery_failed": "0",
                        "failure_reason": "",
                    },
                ),
                (
                    "valid",
                    [f"/ASIOCLSID={FIXTURE_A_CLSID}"],
                    {
                        "requested": "1",
                        "selected": FIXTURE_A_CLSID,
                        "selected_name": "Fixture ASIO Alpha",
                        "target_valid": "1",
                        "recovery_failed": "0",
                        "failure_reason": "",
                    },
                ),
                (
                    "missing",
                    [f"/ASIOCLSID={MISSING_CLSID}"],
                    {
                        "requested": "0",
                        "selected": MISSING_CLSID,
                        "selected_name": "",
                        "target_valid": "0",
                        "recovery_failed": "1",
                        "failure_reason": "validating the explicit /ASIOCLSID target",
                    },
                ),
            )

            for case_name, arguments, expected in cases:
                with self.subTest(case=case_name):
                    result_path = temp / f"{case_name}-result.txt"
                    result = subprocess.run(
                        [str(harness), f"/RESULT={result_path}", *arguments],
                        cwd=ROOT,
                        capture_output=True,
                        text=True,
                        check=False,
                        timeout=5,
                    )
                    self.assertEqual(
                        result.returncode,
                        0,
                        result.stderr + result.stdout,
                    )
                    values = parse_result(result_path)
                    self.assertEqual(values["discover_count"], "2")
                    self.assertEqual(values["discover_single"], "")
                    self.assertEqual(values["resolve_count"], "2")
                    self.assertEqual(values["resolve_single"], "")
                    for key, value in expected.items():
                        self.assertEqual(values[key], value, key)
                    self.assertFalse(registry_key_exists(registry_path))


if __name__ == "__main__":
    unittest.main()
