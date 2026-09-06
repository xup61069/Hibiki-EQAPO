import subprocess
import struct
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
VALIDATOR = ROOT / "scripts" / "test-asio-proxy-binary.ps1"
MAKENSIS = ROOT / "third_party" / "nsis-3.11" / "makensis.exe"
NSIS_HARNESS = ROOT / "tests" / "asio-installer-pe-harness.nsi"
RELEASE_DRIVER = ROOT / "x64" / "Release" / "HibikiEQAPODriver.dll"
REQUIRED_EXPORTS = (
    "DllCanUnloadNow",
    "DllGetClassObject",
    "DllRegisterServer",
    "DllUnregisterServer",
)
DRIVER_CLSID = "{D47C55C9-3F7D-422F-86E9-32E170815D53}"
TARGET_STORE = r"Software\EqualizerAPO\ASIOProxy"


def write_minimal_pe(
    path: Path,
    machine: int,
    characteristics: int,
    *,
    section_count: int = 1,
    optional_size: int = 0xF0,
    size_of_headers: int = 0x200,
) -> None:
    image = bytearray(512)
    image[0:2] = b"MZ"
    image[0x3C:0x40] = (0x80).to_bytes(4, "little")
    image[0x80:0x84] = b"PE\0\0"
    image[0x84:0x86] = machine.to_bytes(2, "little")
    image[0x86:0x88] = section_count.to_bytes(2, "little")
    image[0x94:0x96] = optional_size.to_bytes(2, "little")
    image[0x96:0x98] = characteristics.to_bytes(2, "little")
    image[0x98:0x9A] = (0x20B).to_bytes(2, "little")
    image[0x98 + 60 : 0x98 + 64] = size_of_headers.to_bytes(4, "little")
    path.write_bytes(image)


def write_validated_proxy_pe(
    path: Path,
    *,
    machine: int = 0x8664,
    characteristics: int = 0x2022,
    size_of_headers: int = 0x200,
    exports: tuple[str, ...] = REQUIRED_EXPORTS,
    normal_imports: tuple[str, ...] = ("KERNEL32.dll",),
    delay_imports: tuple[str, ...] = (),
    include_clsid: bool = True,
    include_target_store: bool = True,
) -> None:
    """Write a deterministic PE32+ fixture exercising all validator gates."""
    image = bytearray(0x1000)
    image[0:2] = b"MZ"
    struct.pack_into("<I", image, 0x3C, 0x80)

    pe_offset = 0x80
    image[pe_offset : pe_offset + 4] = b"PE\0\0"
    coff_offset = pe_offset + 4
    optional_size = 0xF0
    struct.pack_into(
        "<HHIIIHH",
        image,
        coff_offset,
        machine,
        1,
        0,
        0,
        0,
        optional_size,
        characteristics,
    )

    optional_offset = coff_offset + 20
    struct.pack_into("<H", image, optional_offset, 0x20B)
    struct.pack_into("<I", image, optional_offset + 16, 0x1000)
    struct.pack_into("<I", image, optional_offset + 20, 0x1000)
    struct.pack_into("<Q", image, optional_offset + 24, 0x180000000)
    struct.pack_into("<I", image, optional_offset + 32, 0x1000)
    struct.pack_into("<I", image, optional_offset + 36, 0x200)
    struct.pack_into("<H", image, optional_offset + 40, 6)
    struct.pack_into("<H", image, optional_offset + 48, 6)
    struct.pack_into("<I", image, optional_offset + 56, 0x2000)
    struct.pack_into("<I", image, optional_offset + 60, size_of_headers)
    struct.pack_into("<H", image, optional_offset + 68, 3)
    struct.pack_into("<H", image, optional_offset + 70, 0x8160)
    struct.pack_into("<Q", image, optional_offset + 72, 0x100000)
    struct.pack_into("<Q", image, optional_offset + 80, 0x1000)
    struct.pack_into("<Q", image, optional_offset + 88, 0x100000)
    struct.pack_into("<Q", image, optional_offset + 96, 0x1000)
    struct.pack_into("<I", image, optional_offset + 108, 16)

    section_header = optional_offset + optional_size
    image[section_header : section_header + 8] = b".rdata\0\0"
    struct.pack_into("<I", image, section_header + 8, 0xE00)
    struct.pack_into("<I", image, section_header + 12, 0x1000)
    struct.pack_into("<I", image, section_header + 16, 0xE00)
    struct.pack_into("<I", image, section_header + 20, 0x200)
    struct.pack_into("<I", image, section_header + 36, 0x40000040)

    section_offset = 0x200
    export_directory_offset = section_offset
    export_name_table_offset = section_offset + 0x40
    export_string_offset = section_offset + 0x80
    struct.pack_into("<I", image, export_directory_offset + 20, len(exports))
    struct.pack_into("<I", image, export_directory_offset + 24, len(exports))
    struct.pack_into("<I", image, export_directory_offset + 32, 0x1040)
    for index, export in enumerate(exports):
        encoded = export.encode("ascii") + b"\0"
        name_rva = 0x1000 + export_string_offset - section_offset
        struct.pack_into("<I", image, export_name_table_offset + index * 4, name_rva)
        image[export_string_offset : export_string_offset + len(encoded)] = encoded
        export_string_offset += len(encoded)
    struct.pack_into("<II", image, optional_offset + 112, 0x1000, 40)

    dependency_string_offset = section_offset + 0x320
    if normal_imports:
        descriptor_offset = section_offset + 0x180
        for index, dependency in enumerate(normal_imports):
            encoded = dependency.encode("ascii") + b"\0"
            name_rva = 0x1000 + dependency_string_offset - section_offset
            struct.pack_into(
                "<IIIII",
                image,
                descriptor_offset + index * 20,
                0,
                0,
                0,
                name_rva,
                0,
            )
            image[
                dependency_string_offset : dependency_string_offset + len(encoded)
            ] = encoded
            dependency_string_offset += len(encoded)
        struct.pack_into(
            "<II",
            image,
            optional_offset + 112 + 8,
            0x1180,
            (len(normal_imports) + 1) * 20,
        )

    if delay_imports:
        descriptor_offset = section_offset + 0x240
        for index, dependency in enumerate(delay_imports):
            encoded = dependency.encode("ascii") + b"\0"
            name_rva = 0x1000 + dependency_string_offset - section_offset
            struct.pack_into(
                "<IIIIIIII",
                image,
                descriptor_offset + index * 32,
                1,
                name_rva,
                0,
                0,
                0,
                0,
                0,
                0,
            )
            image[
                dependency_string_offset : dependency_string_offset + len(encoded)
            ] = encoded
            dependency_string_offset += len(encoded)
        struct.pack_into(
            "<II",
            image,
            optional_offset + 112 + 13 * 8,
            0x1240,
            (len(delay_imports) + 1) * 32,
        )

    if include_clsid:
        marker = DRIVER_CLSID.encode("utf-16-le")
        image[section_offset + 0x500 : section_offset + 0x500 + len(marker)] = marker
    if include_target_store:
        marker = TARGET_STORE.encode("utf-16-le")
        image[section_offset + 0x600 : section_offset + 0x600 + len(marker)] = marker
    path.write_bytes(image)


class AsioInstallerBinaryValidatorTests(unittest.TestCase):
    def run_validator(self, path: Path) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [
                "powershell.exe",
                "-NoProfile",
                "-NonInteractive",
                "-ExecutionPolicy",
                "Bypass",
                "-File",
                str(VALIDATOR),
                "-Path",
                str(path),
            ],
            cwd=ROOT,
            capture_output=True,
            text=True,
            check=False,
        )

    def test_accepts_an_amd64_dll(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "driver.dll"
            write_validated_proxy_pe(path)
            result = self.run_validator(path)
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)

    def test_rejects_x86_and_non_dll_images(self):
        with tempfile.TemporaryDirectory() as directory:
            x86 = Path(directory) / "x86.dll"
            exe = Path(directory) / "x64.exe"
            write_validated_proxy_pe(x86, machine=0x014C)
            write_validated_proxy_pe(exe, characteristics=0x22)
            x86_result = self.run_validator(x86)
            exe_result = self.run_validator(exe)
        self.assertNotEqual(x86_result.returncode, 0)
        self.assertNotEqual(exe_result.returncode, 0)

    def test_rejects_missing_exports_or_identity_markers(self):
        with tempfile.TemporaryDirectory() as directory:
            temp = Path(directory)
            missing_export = temp / "missing-export.dll"
            missing_clsid = temp / "missing-clsid.dll"
            missing_store = temp / "missing-store.dll"
            write_validated_proxy_pe(missing_export, exports=REQUIRED_EXPORTS[:-1])
            write_validated_proxy_pe(missing_clsid, include_clsid=False)
            write_validated_proxy_pe(missing_store, include_target_store=False)
            results = [
                self.run_validator(missing_export),
                self.run_validator(missing_clsid),
                self.run_validator(missing_store),
            ]
        self.assertTrue(all(result.returncode != 0 for result in results))

    def test_rejects_forbidden_normal_delay_and_self_imports(self):
        cases = (
            {"normal_imports": ("sndfile.dll",)},
            {"delay_imports": ("fftw3.dll",)},
            {"normal_imports": ("HibikiEQAPODriver.dll",)},
        )
        with tempfile.TemporaryDirectory() as directory:
            results = []
            for index, options in enumerate(cases):
                path = Path(directory) / f"bad-import-{index}.dll"
                write_validated_proxy_pe(path, **options)
                results.append(self.run_validator(path))
        self.assertTrue(all(result.returncode != 0 for result in results))

    def test_rejects_truncated_or_malformed_images(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "bad.dll"
            path.write_bytes(b"MZ")
            result = self.run_validator(path)
        self.assertNotEqual(result.returncode, 0)

    def test_shipping_validator_rejects_undersized_or_oversized_headers(self):
        with tempfile.TemporaryDirectory() as directory:
            undersized = Path(directory) / "undersized-headers.dll"
            oversized = Path(directory) / "oversized-headers.dll"
            write_validated_proxy_pe(undersized, size_of_headers=1)
            write_validated_proxy_pe(oversized, size_of_headers=0x2000)
            undersized_result = self.run_validator(undersized)
            oversized_result = self.run_validator(oversized)
        self.assertNotEqual(undersized_result.returncode, 0)
        self.assertNotEqual(oversized_result.returncode, 0)

    @unittest.skipUnless(RELEASE_DRIVER.is_file(), "Release driver has not been built")
    def test_built_release_driver_passes_the_shipping_validator(self):
        result = self.run_validator(RELEASE_DRIVER)
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)

    @unittest.skipUnless(MAKENSIS.is_file(), "bootstrapped NSIS is required")
    def test_nsis_discovery_parser_accepts_only_amd64_dlls(self):
        with tempfile.TemporaryDirectory() as directory:
            temp = Path(directory)
            harness = temp / "asio-installer-pe-harness.exe"
            valid = temp / "valid.dll"
            x86 = temp / "x86.dll"
            executable = temp / "executable.dll"
            truncated = temp / "truncated.dll"
            truncated_sections = temp / "truncated-sections.dll"
            tiny_optional = temp / "tiny-optional-header.dll"
            zero_headers = temp / "zero-size-of-headers.dll"
            undersized_headers = temp / "undersized-size-of-headers.dll"
            oversized_headers = temp / "oversized-size-of-headers.dll"
            write_minimal_pe(valid, machine=0x8664, characteristics=0x2000)
            write_minimal_pe(x86, machine=0x014C, characteristics=0x2000)
            write_minimal_pe(executable, machine=0x8664, characteristics=0)
            write_minimal_pe(
                truncated_sections,
                machine=0x8664,
                characteristics=0x2000,
                section_count=4,
            )
            write_minimal_pe(
                tiny_optional,
                machine=0x8664,
                characteristics=0x2000,
                optional_size=2,
            )
            write_minimal_pe(
                zero_headers,
                machine=0x8664,
                characteristics=0x2000,
                size_of_headers=0,
            )
            write_minimal_pe(
                undersized_headers,
                machine=0x8664,
                characteristics=0x2000,
                size_of_headers=1,
            )
            write_minimal_pe(
                oversized_headers,
                machine=0x8664,
                characteristics=0x2000,
                size_of_headers=0x400,
            )
            truncated.write_bytes(b"MZ")

            compile_result = subprocess.run(
                [
                    str(MAKENSIS),
                    "/WX",
                    "/INPUTCHARSET",
                    "UTF8",
                    f"/DHARNESS_OUTPUT={harness}",
                    str(NSIS_HARNESS),
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

            def run(path: Path) -> subprocess.CompletedProcess[str]:
                return subprocess.run(
                    [str(harness), f"/INPUT={path}"],
                    cwd=ROOT,
                    capture_output=True,
                    text=True,
                    check=False,
                )

            self.assertEqual(run(valid).returncode, 0)
            self.assertNotEqual(run(x86).returncode, 0)
            self.assertNotEqual(run(executable).returncode, 0)
            self.assertNotEqual(run(truncated).returncode, 0)
            self.assertNotEqual(run(truncated_sections).returncode, 0)
            self.assertNotEqual(run(tiny_optional).returncode, 0)
            self.assertNotEqual(run(zero_headers).returncode, 0)
            self.assertNotEqual(run(undersized_headers).returncode, 0)
            self.assertNotEqual(run(oversized_headers).returncode, 0)


if __name__ == "__main__":
    unittest.main()
