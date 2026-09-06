#!/usr/bin/env python3
"""Ownership and transaction contracts for the Monitor VST3 manager."""

from __future__ import annotations

import json
import pathlib
import shutil
import struct
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
MANAGER = ROOT / "scripts" / "manage-monitor-vst3.ps1"
POWERSHELL = shutil.which("pwsh") or shutil.which("powershell")

PAYLOAD_DIRECTORY = pathlib.Path("Contents") / "x86_64-win"
REQUIRED_FILES = (
    "HibikiEQAPOMonitor.vst3",
    "msvcp140.dll",
    "msvcp140_1.dll",
    "vcruntime140.dll",
    "vcruntime140_1.dll",
)


def write_minimal_amd64_pe(
    path: pathlib.Path,
    *,
    machine: int = 0x8664,
    marker: int = 1,
    normal_imports: tuple[str, ...] = (),
    delay_imports: tuple[str, ...] = (),
) -> None:
    """Write a minimal PE32+ image with deterministic import descriptors."""
    image = bytearray(0x400)
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
        marker,
        0,
        0,
        optional_size,
        0x2022,
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
    struct.pack_into("<I", image, optional_offset + 60, 0x200)
    struct.pack_into("<H", image, optional_offset + 68, 3)
    struct.pack_into("<H", image, optional_offset + 70, 0x8160)
    struct.pack_into("<Q", image, optional_offset + 72, 0x100000)
    struct.pack_into("<Q", image, optional_offset + 80, 0x1000)
    struct.pack_into("<Q", image, optional_offset + 88, 0x100000)
    struct.pack_into("<Q", image, optional_offset + 96, 0x1000)
    struct.pack_into("<I", image, optional_offset + 108, 16)

    section_header = optional_offset + optional_size
    image[section_header : section_header + 8] = b".rdata\0\0"
    struct.pack_into("<I", image, section_header + 8, 0x200)
    struct.pack_into("<I", image, section_header + 12, 0x1000)
    struct.pack_into("<I", image, section_header + 16, 0x200)
    struct.pack_into("<I", image, section_header + 20, 0x200)
    struct.pack_into("<I", image, section_header + 36, 0x40000040)

    section_offset = 0x200
    string_offset = 0x100
    if normal_imports:
        descriptor_offset = 0
        for index, dependency in enumerate(normal_imports):
            encoded = dependency.encode("ascii") + b"\0"
            image[section_offset + string_offset : section_offset + string_offset + len(encoded)] = encoded
            name_rva = 0x1000 + string_offset
            struct.pack_into(
                "<IIIII",
                image,
                section_offset + descriptor_offset + index * 20,
                0,
                0,
                0,
                name_rva,
                0,
            )
            string_offset += len(encoded)
        struct.pack_into(
            "<II",
            image,
            optional_offset + 112 + 8,
            0x1000 + descriptor_offset,
            (len(normal_imports) + 1) * 20,
        )

    if delay_imports:
        descriptor_offset = 0x80
        for index, dependency in enumerate(delay_imports):
            encoded = dependency.encode("ascii") + b"\0"
            image[section_offset + string_offset : section_offset + string_offset + len(encoded)] = encoded
            name_rva = 0x1000 + string_offset
            struct.pack_into(
                "<IIIIIIII",
                image,
                section_offset + descriptor_offset + index * 32,
                1,
                name_rva,
                0,
                0,
                0,
                0,
                0,
                0,
            )
            string_offset += len(encoded)
        struct.pack_into(
            "<II",
            image,
            optional_offset + 112 + 13 * 8,
            0x1000 + descriptor_offset,
            (len(delay_imports) + 1) * 32,
        )

    path.write_bytes(image)


@unittest.skipUnless(POWERSHELL, "PowerShell is required")
class MonitorVST3InstallTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = pathlib.Path(tempfile.mkdtemp(prefix="eqapo-monitor-vst3-"))
        self.source_bundle = self.temp / "source" / "HibikiEQAPOMonitor.vst3"
        self.destination_root = self.temp / "installed-vst3" / "HibikiEQAPO"
        payload = self.source_bundle / PAYLOAD_DIRECTORY
        payload.mkdir(parents=True)
        for index, file_name in enumerate(REQUIRED_FILES, start=1):
            write_minimal_amd64_pe(payload / file_name, marker=index)

    def tearDown(self) -> None:
        shutil.rmtree(self.temp, ignore_errors=True)

    def run_manager(self, action: str, *, expect_success: bool = True) -> subprocess.CompletedProcess[str]:
        result = subprocess.run(
            [
                POWERSHELL,
                "-NoProfile",
                "-ExecutionPolicy",
                "Bypass",
                "-File",
                str(MANAGER),
                "-Action",
                action,
                "-SourceBundle",
                str(self.source_bundle),
                "-DestinationRoot",
                str(self.destination_root),
            ],
            cwd=ROOT,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=60,
            check=False,
        )
        if expect_success:
            self.assertEqual(
                result.returncode,
                0,
                msg=f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}",
            )
        else:
            self.assertNotEqual(
                result.returncode,
                0,
                msg=f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}",
            )
        return result

    def test_script_uses_exact_owned_paths_without_recursive_delete(self) -> None:
        source = MANAGER.read_text(encoding="utf-8-sig")
        for required in (
            'ValidateSet("Install", "Uninstall")',
            'ValidateSet("Release")',
            "[Environment]::Is64BitProcess",
            '"VST3\\HibikiEQAPO"',
            'product = "HibikiEQAPOMonitor"',
            "Get-FileHash",
            "Assert-MonitorVST3PayloadBinaries",
            "GetPathRoot",
            "$fullPath.Equals($pathRoot",
            "Assert-OwnedBundle",
            "-LiteralPath $destinationBundle",
        ):
            with self.subTest(required=required):
                self.assertIn(required, source)
        for forbidden in ("Remove-Item -Recurse", "Remove-Item -Path", "*.*"):
            with self.subTest(forbidden=forbidden):
                self.assertNotIn(forbidden, source)

    def test_install_update_and_uninstall_round_trip(self) -> None:
        self.run_manager("Install")
        installed = self.destination_root / "HibikiEQAPOMonitor.vst3"
        manifest_path = (
            installed / "Contents" / "Resources" / "hibiki-eqapo-monitor.json"
        )
        self.assertTrue(manifest_path.is_file())
        manifest = json.loads(manifest_path.read_text(encoding="utf-8-sig"))
        self.assertEqual(manifest["schema"], 1)
        self.assertEqual(manifest["product"], "HibikiEQAPOMonitor")
        self.assertEqual(manifest["architecture"], "x86_64-win")
        self.assertEqual(len(manifest["files"]), len(REQUIRED_FILES))

        updated_module = self.source_bundle / PAYLOAD_DIRECTORY / REQUIRED_FILES[0]
        write_minimal_amd64_pe(updated_module, marker=100)
        updated_bytes = updated_module.read_bytes()
        self.run_manager("Install")
        self.assertEqual(
            (installed / PAYLOAD_DIRECTORY / REQUIRED_FILES[0]).read_bytes(),
            updated_bytes,
        )
        self.assertFalse(any(self.destination_root.glob("*.backup.*")))
        self.assertFalse(any(self.destination_root.glob("*.new.*")))
        self.assertFalse((self.destination_root / "HibikiEQAPOMonitor.vst3.backup").exists())
        self.assertFalse((self.destination_root / "HibikiEQAPOMonitor.vst3.new").exists())

        self.run_manager("Uninstall")
        self.assertFalse(installed.exists())
        self.assertTrue(self.destination_root.is_dir())
        self.assertFalse(any(self.destination_root.glob("*.removed.*")))
        self.assertFalse((self.destination_root / "HibikiEQAPOMonitor.vst3.removed").exists())

    def test_interrupted_fixed_transaction_slots_are_recovered(self) -> None:
        self.run_manager("Install")
        installed = self.destination_root / "HibikiEQAPOMonitor.vst3"
        backup = self.destination_root / "HibikiEQAPOMonitor.vst3.backup"
        removed = self.destination_root / "HibikiEQAPOMonitor.vst3.removed"

        installed.rename(backup)
        self.run_manager("Install")
        self.assertTrue(installed.is_dir())
        self.assertFalse(backup.exists())

        installed.rename(removed)
        self.run_manager("Uninstall")
        self.assertFalse(installed.exists())
        self.assertFalse(removed.exists())

    def test_unexpected_extra_file_is_preserved_and_refused(self) -> None:
        self.run_manager("Install")
        installed = self.destination_root / "HibikiEQAPOMonitor.vst3"
        unexpected = installed / PAYLOAD_DIRECTORY / "user-owned.txt"
        unexpected.write_text("preserve me\n", encoding="utf-8")

        self.run_manager("Uninstall", expect_success=False)
        self.assertTrue(installed.is_dir())
        self.assertEqual(unexpected.read_text(encoding="utf-8"), "preserve me\n")

    def test_tampered_install_is_not_removed_or_replaced(self) -> None:
        self.run_manager("Install")
        installed = self.destination_root / "HibikiEQAPOMonitor.vst3"
        installed_module = installed / PAYLOAD_DIRECTORY / REQUIRED_FILES[0]
        installed_module.write_bytes(b"tampered\n")

        self.run_manager("Uninstall", expect_success=False)
        self.assertTrue(installed.is_dir())
        self.assertEqual(installed_module.read_bytes(), b"tampered\n")

        self.run_manager("Install", expect_success=False)
        self.assertTrue(installed.is_dir())
        self.assertEqual(installed_module.read_bytes(), b"tampered\n")

    def test_text_payload_is_rejected_before_install(self) -> None:
        module = self.source_bundle / PAYLOAD_DIRECTORY / REQUIRED_FILES[0]
        module.write_bytes(b"not-a-portable-executable\n")

        result = self.run_manager("Install", expect_success=False)

        self.assertIn("PE", result.stdout + result.stderr)
        self.assertFalse((self.destination_root / "HibikiEQAPOMonitor.vst3").exists())

    def test_non_amd64_payload_is_rejected_before_install(self) -> None:
        runtime = self.source_bundle / PAYLOAD_DIRECTORY / REQUIRED_FILES[1]
        write_minimal_amd64_pe(runtime, machine=0x14C)

        result = self.run_manager("Install", expect_success=False)

        self.assertIn("AMD64", result.stdout + result.stderr)
        self.assertFalse((self.destination_root / "HibikiEQAPOMonitor.vst3").exists())

    def test_forbidden_normal_import_is_rejected_before_install(self) -> None:
        module = self.source_bundle / PAYLOAD_DIRECTORY / REQUIRED_FILES[0]
        write_minimal_amd64_pe(module, normal_imports=("sndfile.dll",))

        result = self.run_manager("Install", expect_success=False)

        self.assertIn("sndfile.dll", result.stdout + result.stderr)
        self.assertFalse((self.destination_root / "HibikiEQAPOMonitor.vst3").exists())

    def test_forbidden_delay_import_is_rejected_before_install(self) -> None:
        module = self.source_bundle / PAYLOAD_DIRECTORY / REQUIRED_FILES[0]
        write_minimal_amd64_pe(module, delay_imports=("fftw3.dll",))

        result = self.run_manager("Install", expect_success=False)

        self.assertIn("fftw3.dll", result.stdout + result.stderr)
        self.assertFalse((self.destination_root / "HibikiEQAPOMonitor.vst3").exists())


if __name__ == "__main__":
    unittest.main()
