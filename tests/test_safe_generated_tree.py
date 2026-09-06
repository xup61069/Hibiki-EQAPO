#!/usr/bin/env python3
"""Filesystem safety contracts for generated build-tree replacement."""

from __future__ import annotations

import os
import pathlib
import shutil
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
MODULE = ROOT / "scripts" / "SafeGeneratedTree.psm1"
STAGE_SCRIPT = ROOT / "scripts" / "stage-installer-x64.ps1"


def available_powershell_hosts() -> tuple[pathlib.Path, ...]:
    hosts: list[pathlib.Path] = []
    seen: set[str] = set()
    for executable in ("powershell", "pwsh"):
        resolved = shutil.which(executable)
        if not resolved:
            continue
        path = pathlib.Path(resolved).resolve()
        key = str(path).casefold()
        if key not in seen:
            hosts.append(path)
            seen.add(key)
    return tuple(hosts)


POWERSHELL_HOSTS = available_powershell_hosts()


@unittest.skipUnless(os.name == "nt", "Windows junction semantics are required")
@unittest.skipUnless(POWERSHELL_HOSTS, "PowerShell is required")
class SafeGeneratedTreeTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = pathlib.Path(tempfile.mkdtemp(prefix="eqapo-safe-tree-"))
        self.junctions: list[pathlib.Path] = []

    def tearDown(self) -> None:
        # Never let recursive test cleanup traverse a surviving junction.
        for junction in reversed(self.junctions):
            if os.path.lexists(junction):
                os.rmdir(junction)
        shutil.rmtree(self.temp, ignore_errors=False)

    def create_junction(self, link: pathlib.Path, target: pathlib.Path) -> None:
        target.mkdir(parents=True, exist_ok=True)
        link.parent.mkdir(parents=True, exist_ok=True)
        result = subprocess.run(
            ["cmd.exe", "/d", "/c", "mklink", "/J", str(link), str(target)],
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=30,
            check=False,
        )
        self.assertEqual(
            result.returncode,
            0,
            msg=f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}",
        )
        self.junctions.append(link)

    def powershell_environment(self, host: pathlib.Path) -> dict[str, str]:
        environment = os.environ.copy()
        if host.name.casefold() == "powershell.exe":
            # A pwsh parent can export PS7 module paths into Windows PowerShell.
            system_root = pathlib.Path(environment.get("SystemRoot", r"C:\Windows"))
            program_files = pathlib.Path(
                environment.get("ProgramFiles", r"C:\Program Files")
            )
            environment["PSModulePath"] = os.pathsep.join(
                [
                    str(pathlib.Path.home() / "Documents" / "WindowsPowerShell" / "Modules"),
                    str(program_files / "WindowsPowerShell" / "Modules"),
                    str(
                        system_root
                        / "System32"
                        / "WindowsPowerShell"
                        / "v1.0"
                        / "Modules"
                    ),
                ]
            )
        return environment

    def reset_tree(
        self,
        host: pathlib.Path,
        *,
        allowed_root: pathlib.Path,
        generated_path: pathlib.Path,
        expect_success: bool,
    ) -> subprocess.CompletedProcess[str]:
        environment = self.powershell_environment(host)
        environment["EQAPO_SAFE_TREE_MODULE"] = str(MODULE)
        environment["EQAPO_SAFE_TREE_ALLOWED_ROOT"] = str(allowed_root)
        environment["EQAPO_SAFE_TREE_GENERATED_PATH"] = str(generated_path)
        command = (
            "Import-Module -Name $env:EQAPO_SAFE_TREE_MODULE -Force; "
            "Reset-SafeGeneratedTree "
            "-AllowedRoot $env:EQAPO_SAFE_TREE_ALLOWED_ROOT "
            "-GeneratedPath $env:EQAPO_SAFE_TREE_GENERATED_PATH"
        )
        result = subprocess.run(
            [
                str(host),
                "-NoLogo",
                "-NoProfile",
                "-NonInteractive",
                "-ExecutionPolicy",
                "Bypass",
                "-Command",
                command,
            ],
            cwd=ROOT,
            env=environment,
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
                msg=f"host={host}\nstdout:\n{result.stdout}\nstderr:\n{result.stderr}",
            )
        else:
            self.assertNotEqual(
                result.returncode,
                0,
                msg=f"host={host}\nstdout:\n{result.stdout}\nstderr:\n{result.stderr}",
            )
        return result

    def test_normal_generated_tree_is_cleared_and_recreated_by_each_host(self) -> None:
        for index, host in enumerate(POWERSHELL_HOSTS):
            with self.subTest(host=host.name):
                allowed_root = self.temp / f"normal-{index}" / "repo"
                generated = allowed_root / "Setup" / "lib64"
                nested = generated / "nested" / "deeper"
                nested.mkdir(parents=True)
                (generated / "root.bin").write_bytes(b"root")
                (nested / "payload.bin").write_bytes(b"payload")

                self.reset_tree(
                    host,
                    allowed_root=allowed_root,
                    generated_path=generated,
                    expect_success=True,
                )

                self.assertTrue(generated.is_dir())
                self.assertEqual(list(generated.iterdir()), [])
                self.assertFalse(generated.is_symlink())

    def test_nested_junction_fails_before_deleting_anything(self) -> None:
        for index, host in enumerate(POWERSHELL_HOSTS):
            with self.subTest(host=host.name):
                allowed_root = self.temp / f"nested-{index}" / "repo"
                generated = allowed_root / "Setup" / "lib64"
                generated.mkdir(parents=True)
                ordinary_file = generated / "ordinary.bin"
                ordinary_file.write_bytes(b"keep")
                external = self.temp / f"nested-{index}" / "external"
                sentinel = external / "sentinel.txt"
                external.mkdir(parents=True)
                sentinel.write_text("preserve", encoding="utf-8")
                self.create_junction(generated / "linked", external)

                result = self.reset_tree(
                    host,
                    allowed_root=allowed_root,
                    generated_path=generated,
                    expect_success=False,
                )

                self.assertIn("reparse point", (result.stdout + result.stderr).lower())
                self.assertEqual(sentinel.read_text(encoding="utf-8"), "preserve")
                self.assertEqual(ordinary_file.read_bytes(), b"keep")

    def test_ancestor_junction_fails_and_preserves_external_tree(self) -> None:
        for index, host in enumerate(POWERSHELL_HOSTS):
            with self.subTest(host=host.name):
                allowed_root = self.temp / f"ancestor-{index}" / "repo"
                allowed_root.mkdir(parents=True)
                external_setup = self.temp / f"ancestor-{index}" / "external-setup"
                external_generated = external_setup / "lib64"
                external_generated.mkdir(parents=True)
                sentinel = external_generated / "sentinel.txt"
                sentinel.write_text("preserve", encoding="utf-8")
                self.create_junction(allowed_root / "Setup", external_setup)
                generated = allowed_root / "Setup" / "lib64"

                result = self.reset_tree(
                    host,
                    allowed_root=allowed_root,
                    generated_path=generated,
                    expect_success=False,
                )

                self.assertIn("reparse point", (result.stdout + result.stderr).lower())
                self.assertEqual(sentinel.read_text(encoding="utf-8"), "preserve")

    def test_path_outside_allowed_root_is_rejected_without_deletion(self) -> None:
        for index, host in enumerate(POWERSHELL_HOSTS):
            with self.subTest(host=host.name):
                allowed_root = self.temp / f"containment-{index}" / "repo"
                allowed_root.mkdir(parents=True)
                generated = self.temp / f"containment-{index}" / "outside"
                generated.mkdir(parents=True)
                sentinel = generated / "sentinel.txt"
                sentinel.write_text("preserve", encoding="utf-8")

                result = self.reset_tree(
                    host,
                    allowed_root=allowed_root,
                    generated_path=generated,
                    expect_success=False,
                )

                self.assertIn("allowed root", (result.stdout + result.stderr).lower())
                self.assertEqual(sentinel.read_text(encoding="utf-8"), "preserve")

    def test_module_and_stage_script_forbid_recursive_generated_tree_removal(self) -> None:
        self.assertTrue(MODULE.is_file())
        module = MODULE.read_text(encoding="utf-8-sig")
        stage = STAGE_SCRIPT.read_text(encoding="utf-8-sig")

        self.assertNotIn("-Recurse", module)
        self.assertNotRegex(stage, r"Remove-Item\b[^\r\n]*-Recurse")
        for required in (
            "GetFullPath",
            "Get-ContainedFullPath",
            "StartsWith($rootPrefix",
            "ReparsePoint",
            "System.Collections.Generic.Stack[string]",
            "Get-ChildItem -LiteralPath $directoryPath -Force -ErrorAction Stop",
            "Remove-Item -LiteralPath",
            "-ErrorAction Stop",
            "Export-ModuleMember",
        ):
            with self.subTest(module_contract=required):
                self.assertIn(required, module)
        reset = module.index("function Reset-SafeGeneratedTree")
        inventory = module.index("$inventory = Get-SafeGeneratedTreeInventory", reset)
        first_remove = module.index("Remove-SafeGeneratedFile", inventory)
        self.assertLess(inventory, first_remove)
        self.assertIn("SafeGeneratedTree.psm1", stage)
        self.assertEqual(stage.count("Reset-SafeGeneratedTree"), 2)


if __name__ == "__main__":
    unittest.main()
