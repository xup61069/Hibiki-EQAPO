#!/usr/bin/env python3
"""Exercise the production process-stopper command across the NSIS boundary."""

from __future__ import annotations

import base64
import gzip
import os
import re
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SETUP_SOURCE = (ROOT / "Setup" / "Setup.nsi").read_text(encoding="utf-8")
BUILD_SCRIPT = (ROOT / "scripts" / "build-installer-x64.ps1").read_text(
    encoding="utf-8"
)
MAKENSIS = ROOT / "third_party" / "nsis-3.11" / "makensis.exe"
WINDOWS_POWERSHELL = (
    Path(os.environ.get("WINDIR", "C:/Windows"))
    / "System32"
    / "WindowsPowerShell"
    / "v1.0"
    / "powershell.exe"
)


def extract_process_stopper_command(source: str) -> str:
    macro = re.search(
        r"^!macro RunEmbeddedProcessStopper protectInteractive\s*$"
        r"(?P<body>.*?)"
        r"^!macroend\s*$",
        source,
        flags=re.MULTILINE | re.DOTALL,
    )
    if macro is None:
        raise AssertionError("RunEmbeddedProcessStopper macro was not found")
    commands = [
        line.strip()
        for line in macro.group("body").splitlines()
        if line.strip().startswith("nsExec::ExecToLog")
    ]
    if len(commands) != 1:
        raise AssertionError(
            "RunEmbeddedProcessStopper must contain exactly one nsExec::ExecToLog "
            f"command, found {len(commands)}"
        )
    return commands[0]


def split_encoded_payload(payload: bytes, count: int = 4) -> list[str]:
    encoded = base64.b64encode(gzip.compress(payload)).decode("ascii")
    boundaries = [len(encoded) * index // count for index in range(count + 1)]
    chunks = [
        encoded[boundaries[index] : boundaries[index + 1]]
        for index in range(count)
    ]
    if not all(chunks) or "".join(chunks) != encoded:
        raise AssertionError("failed to split the embedded PowerShell payload")
    return chunks


class ProcessStopperNsisBuildContractTests(unittest.TestCase):
    def test_installer_build_runs_quote_boundary_regression_before_native_build(self):
        bootstrap = BUILD_SCRIPT.index("-WithQt -WithNsis")
        discovery = BUILD_SCRIPT.index("test_asio_installer_discovery_runtime.py")
        quote_boundary = BUILD_SCRIPT.index(
            "test_installer_process_stopper_nsis_runtime.py"
        )
        native_build = BUILD_SCRIPT.index('"build-local-x64.ps1"')
        failure = BUILD_SCRIPT.index(
            "process-stopper NSIS quote-boundary regression failed",
            quote_boundary,
        )
        self.assertLess(bootstrap, discovery)
        self.assertLess(discovery, quote_boundary)
        self.assertLess(quote_boundary, failure)
        self.assertLess(failure, native_build)


@unittest.skipUnless(
    os.name == "nt" and MAKENSIS.is_file() and WINDOWS_POWERSHELL.is_file(),
    "Windows, Windows PowerShell 5.1, and bootstrapped NSIS are required",
)
class ProcessStopperNsisRuntimeTests(unittest.TestCase):
    def test_production_command_preserves_process_stopper_arguments(self):
        command = extract_process_stopper_command(SETUP_SOURCE)
        self.assertIn("EQAPO_STOP_CODE_0", command)
        self.assertIn("EQAPO_PROCESS_PROTECT_INTERACTIVE", command)

        payload = b"""param(
    [Parameter(Mandatory=$true)]
    [string]$InstallRoot,
    [switch]$ProtectInteractiveApplications
)
$lines = @(
    \"InstallRoot=$InstallRoot\",
    \"ProtectInteractiveApplications=$($ProtectInteractiveApplications.IsPresent)\"
)
[IO.File]::WriteAllLines(
    $env:EQAPO_RUNTIME_RESULT_PATH,
    $lines,
    [Text.UTF8Encoding]::new($false)
)
"""
        chunks = split_encoded_payload(payload)

        with tempfile.TemporaryDirectory(
            prefix="hibiki-process-stopper-nsis-"
        ) as directory:
            temp = Path(directory)
            harness_source = temp / "process-stopper-quote-harness.nsi"
            harness = temp / "process-stopper-quote-harness.exe"
            harness_source.write_text(
                "\n".join(
                    (
                        "Unicode true",
                        "SilentInstall silent",
                        "AutoCloseWindow true",
                        "RequestExecutionLevel user",
                        "!ifndef HARNESS_OUTPUT",
                        '  !error "HARNESS_OUTPUT must point to the harness executable"',
                        "!endif",
                        'OutFile "${HARNESS_OUTPUT}"',
                        "Section",
                        '  StrCpy $0 "$SYSDIR\\WindowsPowerShell\\v1.0\\powershell.exe"',
                        f"  {command}",
                        "  Pop $1",
                        "  SetErrorLevel $1",
                        "SectionEnd",
                        "",
                    )
                ),
                encoding="utf-8",
                newline="\n",
            )

            compile_result = subprocess.run(
                [
                    str(MAKENSIS),
                    "/WX",
                    "/INPUTCHARSET",
                    "UTF8",
                    f"/DHARNESS_OUTPUT={harness}",
                    str(harness_source),
                ],
                cwd=ROOT,
                capture_output=True,
                text=True,
                encoding="utf-8",
                errors="replace",
                check=False,
                timeout=10,
            )
            self.assertEqual(
                compile_result.returncode,
                0,
                compile_result.stderr + compile_result.stdout,
            )

            install_root = temp / "install root with spaces"
            install_root.mkdir()
            for protect_interactive in (False, True):
                with self.subTest(protect_interactive=protect_interactive):
                    result_path = temp / f"result-{int(protect_interactive)}.txt"
                    environment = os.environ.copy()
                    environment["EQAPO_PROCESS_INSTALL_ROOT"] = str(install_root)
                    environment["EQAPO_PROCESS_PROTECT_INTERACTIVE"] = (
                        "1" if protect_interactive else "0"
                    )
                    environment["EQAPO_RUNTIME_RESULT_PATH"] = str(result_path)
                    for index, chunk in enumerate(chunks):
                        environment[f"EQAPO_STOP_CODE_{index}"] = chunk

                    result = subprocess.run(
                        [str(harness), "/S"],
                        cwd=ROOT,
                        capture_output=True,
                        text=True,
                        encoding="utf-8",
                        errors="replace",
                        check=False,
                        timeout=5,
                        env=environment,
                    )
                    self.assertEqual(
                        result.returncode,
                        0,
                        f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}",
                    )
                    self.assertTrue(result_path.is_file(), "payload did not run")
                    self.assertEqual(
                        result_path.read_text(encoding="utf-8").splitlines(),
                        [
                            f"InstallRoot={install_root}",
                            "ProtectInteractiveApplications="
                            + str(protect_interactive),
                        ],
                    )


if __name__ == "__main__":
    unittest.main()
