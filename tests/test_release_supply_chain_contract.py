#!/usr/bin/env python3
"""Regression contracts for the release privilege and download trust boundaries."""

from __future__ import annotations

import http.server
import json
import os
import pathlib
import re
import shutil
import socket
import subprocess
import tempfile
import threading
import unittest
import urllib.parse


ROOT = pathlib.Path(__file__).resolve().parents[1]
WORKFLOW_PATH = ROOT / ".github" / "workflows" / "release.yml"
WORKFLOW = WORKFLOW_PATH.read_text(encoding="utf-8") if WORKFLOW_PATH.is_file() else ""
BUILD_WORKFLOW_PATH = ROOT / ".github" / "workflows" / "build.yml"
BUILD_WORKFLOW = BUILD_WORKFLOW_PATH.read_text(encoding="utf-8")
BOOTSTRAP = (ROOT / "scripts" / "bootstrap-third-party.ps1").read_text(
    encoding="utf-8"
)
QT_EXTRACTOR_LOCK_PATH = (
    ROOT / "scripts" / "qt-extractor-requirements-win-py313.txt"
)
QT_EXTRACTOR_LOCK = QT_EXTRACTOR_LOCK_PATH.read_text(encoding="utf-8")
HEADPHONE_BUNDLE_UPDATE = (
    ROOT / "scripts" / "update-headphone-calibration-bundle.ps1"
).read_text(encoding="utf-8")
# GitHub Actions runs this release script under PowerShell 7. Prefer that same
# host for network transaction tests; Windows PowerShell syntax is checked
# separately and is also exercised by the NSIS helper contracts.
POWERSHELL = shutil.which("pwsh") or shutil.which("powershell")
_SERVER_PORT_LOCK = threading.Lock()
_NEXT_SERVER_PORT = 43000


class IPv6ThreadingHTTPServer(http.server.ThreadingHTTPServer):
    address_family = socket.AF_INET6


def bind_loopback_test_server(
    handler: type[http.server.BaseHTTPRequestHandler],
) -> http.server.ThreadingHTTPServer:
    """Bind outside this host's unusually low dynamic/proxy-intercepted range."""
    global _NEXT_SERVER_PORT
    with _SERVER_PORT_LOCK:
        for _ in range(1000):
            port = _NEXT_SERVER_PORT
            _NEXT_SERVER_PORT += 1
            try:
                return IPv6ThreadingHTTPServer(("::1", port), handler)
            except OSError:
                continue
    raise OSError("No loopback port was available for the calibration test server")


def tree_snapshot(root: pathlib.Path) -> dict[str, bytes]:
    if not root.is_dir():
        return {}
    return {
        path.relative_to(root).as_posix(): path.read_bytes()
        for path in sorted(root.rglob("*"))
        if path.is_file()
    }


def seed_calibration_output(output: pathlib.Path) -> dict[str, bytes]:
    output.mkdir(parents=True)
    (output / "index.json").write_text(
        '{"version":1,"targets":[{"file":"old-target.txt"}]}',
        encoding="utf-8",
    )
    (output / "old-target.txt").write_bytes(b"old managed payload\x00")
    (output / "README.md").write_text("local documentation\n", encoding="utf-8")
    (output / "local-notes.bin").write_bytes(b"unmanaged\xffdata")
    return tree_snapshot(output)


def run_calibration_update(
    temp: pathlib.Path,
    output: pathlib.Path,
    index: object,
    *,
    fail_file: str = "",
    oversized_index: bool = False,
    reset_once_file: str = "",
) -> subprocess.CompletedProcess[str]:
    index_json = json.dumps(index, ensure_ascii=False, separators=(",", ":"))
    index_bytes = index_json.encode("utf-8")
    reset_remaining = {reset_once_file.casefold(): 1} if reset_once_file else {}

    class CalibrationHandler(http.server.BaseHTTPRequestHandler):
        def log_message(self, _format: str, *args: object) -> None:
            pass

        def do_GET(self) -> None:  # noqa: N802 - stdlib handler API
            leaf = urllib.parse.unquote(self.path.rsplit("/", maxsplit=1)[-1])
            reset_key = leaf.casefold()
            if reset_remaining.get(reset_key, 0):
                reset_remaining[reset_key] = 0
                self.close_connection = True
                self.connection.shutdown(socket.SHUT_RDWR)
                self.connection.close()
                return
            if leaf == "index.json" and oversized_index:
                self.send_response(200)
                self.send_header("Content-Type", "application/octet-stream")
                self.send_header("Connection", "close")
                self.end_headers()
                remaining = 1048577
                block = b"x" * 65536
                try:
                    while remaining:
                        part = block[: min(remaining, len(block))]
                        self.wfile.write(part)
                        self.wfile.flush()
                        remaining -= len(part)
                except (BrokenPipeError, ConnectionResetError):
                    pass
                self.close_connection = True
                return
            if leaf == "index.json":
                body = index_bytes
                status = 200
            elif fail_file and leaf.casefold() == fail_file.casefold():
                body = b"simulated target download failure"
                status = 503
            else:
                body = f"payload:{leaf}".encode("utf-8")
                status = 200
            self.send_response(status)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(body)
            self.wfile.flush()
            self.close_connection = True

    server = bind_loopback_test_server(CalibrationHandler)
    server_thread = threading.Thread(target=server.serve_forever, daemon=True)
    server_thread.start()
    child_environment = os.environ.copy()
    # A PowerShell 7 parent can export its module path into Windows PowerShell
    # 5.1, which makes the inbox Security module collide with PS7 type data.
    if POWERSHELL and pathlib.Path(POWERSHELL).name.lower() == "powershell.exe":
        system_root = pathlib.Path(child_environment.get("SystemRoot", r"C:\Windows"))
        program_files = pathlib.Path(
            child_environment.get("ProgramFiles", r"C:\Program Files")
        )
        child_environment["PSModulePath"] = os.pathsep.join(
            [
                str(pathlib.Path.home() / "Documents" / "WindowsPowerShell" / "Modules"),
                str(program_files / "WindowsPowerShell" / "Modules"),
                str(system_root / "System32" / "WindowsPowerShell" / "v1.0" / "Modules"),
            ]
        )
    for proxy_name in ("HTTP_PROXY", "HTTPS_PROXY", "ALL_PROXY"):
        child_environment.pop(proxy_name, None)
        child_environment.pop(proxy_name.lower(), None)
    child_environment["NO_PROXY"] = "127.0.0.1,::1,localhost"
    try:
        return subprocess.run(
            [
                POWERSHELL or "powershell",
                "-NoProfile",
                "-ExecutionPolicy",
                "Bypass",
                "-File",
                str(ROOT / "scripts" / "update-headphone-calibration-bundle.ps1"),
                "-BaseUrl",
                f"http://[::1]:{server.server_port}",
                "-OutputDir",
                str(output),
            ],
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=30,
            check=False,
            env=child_environment,
        )
    finally:
        server.shutdown()
        server.server_close()
        server_thread.join(timeout=5)


def job_block(name: str, next_name: str | None = None) -> str:
    start = WORKFLOW.index(f"  {name}:\n")
    end = WORKFLOW.index(f"  {next_name}:\n", start) if next_name else len(WORKFLOW)
    return WORKFLOW[start:end]


class ReleaseSupplyChainContractTests(unittest.TestCase):
    @unittest.skipUnless(WORKFLOW_PATH.is_file(), "release workflow has not been added yet")
    def test_build_is_read_only_and_publish_is_the_only_write_job(self) -> None:
        build = job_block("build", "publish")
        publish = job_block("publish")

        self.assertRegex(WORKFLOW, r"(?m)^permissions:\n  contents: read$")
        self.assertRegex(build, r"(?m)^    permissions:\n      contents: read$")
        self.assertNotIn("contents: write", build)
        self.assertRegex(publish, r"(?m)^    permissions:\n      contents: write$")
        self.assertIn("needs: build", publish)

    @unittest.skipUnless(WORKFLOW_PATH.is_file(), "release workflow has not been added yet")
    def test_publish_consumes_artifact_without_checkout_or_build_steps(self) -> None:
        publish = job_block("publish")

        self.assertIn("actions/download-artifact@", publish)
        self.assertIn("softprops/action-gh-release@", publish)
        self.assertNotIn("actions/checkout@", publish)
        self.assertNotIn("setup-msbuild@", publish)
        self.assertNotIn("build-installer-x64.ps1", publish)
        self.assertNotIn("test-runtime-loudness.ps1", publish)
        self.assertIn("Get-FileHash", publish)

    @unittest.skipUnless(WORKFLOW_PATH.is_file(), "release workflow has not been added yet")
    def test_manual_release_does_not_checkout_a_nonexistent_tag(self) -> None:
        build = job_block("build", "publish")
        checkout_step = build.split("- name: Checkout repository", maxsplit=1)[1].split(
            "- name:", maxsplit=1
        )[0]

        self.assertNotRegex(checkout_step, r"(?m)^\s+ref:")
        self.assertNotIn("inputs.tag_name", checkout_step)
        self.assertIn("target_commitish: ${{ needs.build.outputs.commit_sha }}", WORKFLOW)

    @unittest.skipUnless(WORKFLOW_PATH.is_file(), "release workflow has not been added yet")
    def test_existing_release_tag_must_match_the_built_commit(self) -> None:
        build = job_block("build", "publish")

        self.assertIn("git rev-parse HEAD", build)
        self.assertIn("git ls-remote --tags origin $tagRef $peeledTagRef", build)
        self.assertIn('$peeledTagRef = "$tagRef^{}"', build)
        self.assertIn("not checked-out commit $sourceCommit", build)
        self.assertIn('"commit_sha=$sourceCommit"', build)
        self.assertNotIn("SOURCE_COMMIT: ${{ github.sha }}", build)

    @unittest.skipUnless(WORKFLOW_PATH.is_file(), "release workflow has not been added yet")
    def test_same_tag_release_attempts_are_serialized(self) -> None:
        self.assertIn(
            "group: release-${{ github.event_name == 'workflow_dispatch' "
            "&& inputs.tag_name || github.ref_name }}",
            WORKFLOW,
        )
        self.assertIn("cancel-in-progress: false", WORKFLOW)

    @unittest.skipUnless(WORKFLOW_PATH.is_file(), "release workflow has not been added yet")
    def test_every_action_is_pinned_to_a_full_commit(self) -> None:
        for workflow_name, workflow in (
            ("build", BUILD_WORKFLOW),
            ("release", WORKFLOW),
        ):
            actions = re.findall(r"(?m)^\s*uses:\s*([^\s#]+)", workflow)
            self.assertGreaterEqual(len(actions), 3)
            for action in actions:
                with self.subTest(workflow=workflow_name, action=action):
                    self.assertRegex(action, r"^[^@]+@[0-9a-f]{40}$")

    def test_nsis_archive_is_verified_before_extraction(self) -> None:
        expected = "C7D27F780DDB6CFFB4730138CD1591E841F4B7EDB155856901CDF5F214394FA1"
        self.assertIn(f'[string] $NsisSha256 = "{expected}"', BOOTSTRAP)
        self.assertIn("Get-FileHash -LiteralPath $downloadZip -Algorithm SHA256", BOOTSTRAP)
        self.assertIn("[StringComparer]::OrdinalIgnoreCase.Equals", BOOTSTRAP)
        self.assertIn('curl.exe -L --fail --retry 3 --proto "=https"', BOOTSTRAP)
        self.assertIn('--proto-redir "=https"', BOOTSTRAP)
        self.assertLess(
            BOOTSTRAP.index("Get-FileHash -LiteralPath $downloadZip"),
            BOOTSTRAP.index("Expand-Archive -LiteralPath $zip"),
        )

    def test_qt_extractor_transitives_are_fully_hashed_and_installed_offline(self) -> None:
        self.assertTrue(QT_EXTRACTOR_LOCK_PATH.is_file())
        requirements = re.findall(r"(?m)^([A-Za-z0-9_.-]+)==([^ ]+)", QT_EXTRACTOR_LOCK)
        hashes = re.findall(r"--hash=sha256:([0-9a-f]{64})", QT_EXTRACTOR_LOCK)
        self.assertEqual(len(requirements), 10)
        self.assertEqual(len(hashes), len(requirements))
        self.assertEqual(len(set(hashes)), len(hashes))
        for package in (
            "py7zr",
            "backports.zstd",
            "brotli",
            "inflate64",
            "multivolumefile",
            "psutil",
            "pybcj",
            "pycryptodomex",
            "pyppmd",
            "texttable",
        ):
            self.assertIn(package, {name.lower() for name, _ in requirements})

        self.assertIn("--require-hashes", BOOTSTRAP)
        self.assertIn("--no-index", BOOTSTRAP)
        self.assertIn("--no-deps", BOOTSTRAP)
        self.assertIn("--find-links $wheelRoot", BOOTSTRAP)
        self.assertIn('"3.13.2|AMD64"', BOOTSTRAP)
        self.assertNotIn("python -m aqt", BOOTSTRAP)
        self.assertNotIn("AqtInstallSha256", BOOTSTRAP)

    @unittest.skipUnless(WORKFLOW_PATH.is_file(), "release workflow has not been added yet")
    def test_builds_pin_the_python_abi_used_by_the_qt_lock(self) -> None:
        for workflow_name, workflow in (
            ("build", BUILD_WORKFLOW),
            ("release", WORKFLOW),
        ):
            with self.subTest(workflow=workflow_name):
                self.assertIn(
                    "actions/setup-python@a309ff8b426b58ec0e2a45f0f869d46889d02405",
                    workflow,
                )
                self.assertIn('python-version: "3.13.2"', workflow)
                self.assertIn('architecture: "x64"', workflow)

    def test_qt_archives_are_repository_locked_before_extraction(self) -> None:
        expected_hashes = {
            "4de2db56548aa6084479210843a0905d7a3ae3539ef6b918ac88d12a5eeb8178",
            "10f98479ca1aa50e53eee9c7d30143cb717da64447bc8a73f40094f9eff29cb3",
            "78949d2c42d9a35ef9d3bf376bec75de2321573de4c60cf0326f6f4b3d0f79c7",
            "273d5fde0844cf68b025d66ba4a57faad23c8274fe3ec98e539d44d1b8f8fb1a",
            "bcd7a6d1e844dfb24122f7332f860ed2afa6e1fa62960e0aa232dfa0fd027da1",
        }
        for digest in expected_hashes:
            self.assertIn(digest, BOOTSTRAP)
        for byte_count in (42171066, 151504709, 661125, 26891597, 1870834):
            self.assertIn(f"Bytes = {byte_count}", BOOTSTRAP)
        self.assertEqual(BOOTSTRAP.count("202511161843"), 5)
        self.assertIn("curl.exe -L --fail --retry 3 --retry-max-time 1800", BOOTSTRAP)
        self.assertIn("--connect-timeout 30 --max-time 900", BOOTSTRAP)
        self.assertIn("--max-filesize $archive.Bytes", BOOTSTRAP)
        self.assertIn('--proto-redir "=https"', BOOTSTRAP)
        downloaded_size_check = BOOTSTRAP.index(
            "Assert-ExactFileSize -LiteralPath $downloadPath"
        )
        downloaded_hash_check = BOOTSTRAP.index(
            "Assert-Sha256 -LiteralPath $downloadPath"
        )
        self.assertLess(downloaded_size_check, downloaded_hash_check)
        verification = BOOTSTRAP.index(
            'Assert-Sha256 -LiteralPath $archivePath',
            BOOTSTRAP.index("foreach ($archive in $archives)", BOOTSTRAP.index("$stageHost =")),
        )
        extraction = BOOTSTRAP.index("python -m py7zr x $archivePath $stageHost")
        self.assertLess(verification, extraction)
        self.assertNotIn("python -m py7zr x $archivePath $stageRoot", BOOTSTRAP)
        self.assertLess(
            BOOTSTRAP.index('$stageHost = Join-Path $stageRoot "6.10.1\\msvc2022_64"'),
            extraction,
        )

    def test_remote_calibration_update_validates_before_staging_downloads(self) -> None:
        validation = HEADPHONE_BUNDLE_UPDATE[
            HEADPHONE_BUNDLE_UPDATE.index("function Get-ValidatedTargetNames") :
            HEADPHONE_BUNDLE_UPDATE.index("function Assert-RegularDirectory")
        ]
        payload_start = HEADPHONE_BUNDLE_UPDATE.index("$payloadPrefix =")
        payload = HEADPHONE_BUNDLE_UPDATE[
            payload_start : HEADPHONE_BUNDLE_UPDATE.index(
                "foreach ($unmanaged in $unmanagedFiles)", payload_start
            )
        ]
        bounded_download = HEADPHONE_BUNDLE_UPDATE[
            HEADPHONE_BUNDLE_UPDATE.index("function Save-BoundedHttpResource") :
            HEADPHONE_BUNDLE_UPDATE.index("$outputRoot =")
        ]

        self.assertIn('version")', validation)
        self.assertIn("[int]$Index.version -ne 1", validation)
        self.assertIn("$Index.targets -is [Array]", validation)
        self.assertIn("$Index.targets.Count -gt 256", validation)
        self.assertIn("[IO.Path]::IsPathRooted($fileName)", validation)
        self.assertIn('$fileName.Contains("/")', validation)
        self.assertIn('$fileName.Contains("\\")', validation)
        self.assertIn("GetInvalidFileNameChars", validation)
        self.assertIn("$fileName.TrimEnd", validation)
        self.assertIn('GetExtension($fileName)', validation)
        self.assertIn("CON|PRN|AUX|NUL", validation)
        self.assertIn("StringComparer]::OrdinalIgnoreCase", validation)
        self.assertIn("GetFullPath((Join-Path $payloadRoot $fileName))", payload)
        self.assertIn("StartsWith(", payload)
        self.assertIn("Save-BoundedHttpResource", payload)
        self.assertIn("[IO.FileMode]::CreateNew", bounded_download)
        self.assertIn("[Net.HttpWebRequest]::Create", bounded_download)
        self.assertIn("$response.GetResponseStream()", bounded_download)
        self.assertIn("$sourceStream.Read(", bounded_download)
        self.assertIn("$MaximumBytes - $bytesRead", bounded_download)
        self.assertIn("$attempt -le 3", bounded_download)
        self.assertIn("$destinationStream.Position = 0", bounded_download)
        self.assertIn("$destinationStream.SetLength(0)", bounded_download)
        self.assertIn("WebExceptionStatus]::ProtocolError", bounded_download)
        self.assertIn("WebExceptionStatus]::TrustFailure", bounded_download)
        self.assertLess(
            bounded_download.index("$MaximumBytes - $bytesRead"),
            bounded_download.index("$destinationStream.Write"),
        )
        self.assertIn("$destinationStream.Dispose()", bounded_download)
        self.assertIn("EscapeDataString($fileName)", payload)
        self.assertNotIn("Invoke-WebRequest", HEADPHONE_BUNDLE_UPDATE)

    def test_calibration_update_is_transactional_and_preserves_unmanaged_files(self) -> None:
        self.assertIn('$manifestRoot = Join-Path $transactionRoot "manifest"', HEADPHONE_BUNDLE_UPDATE)
        self.assertIn('$payloadRoot = Join-Path $transactionRoot "payload"', HEADPHONE_BUNDLE_UPDATE)
        self.assertIn('$nextRoot = Join-Path $transactionRoot "next"', HEADPHONE_BUNDLE_UPDATE)
        self.assertIn('$previousRoot = Join-Path $transactionRoot "previous"', HEADPHONE_BUNDLE_UPDATE)
        self.assertIn("Copy-Item -LiteralPath $unmanaged.FullName", HEADPHONE_BUNDLE_UPDATE)
        first_move = HEADPHONE_BUNDLE_UPDATE.index(
            "Move-Item -LiteralPath $outputRoot -Destination $previousRoot"
        )
        second_move = HEADPHONE_BUNDLE_UPDATE.index(
            "Move-Item -LiteralPath $nextRoot -Destination $outputRoot"
        )
        rollback = HEADPHONE_BUNDLE_UPDATE.index(
            "Move-Item -LiteralPath $previousRoot -Destination $outputRoot"
        )
        self.assertLess(first_move, second_move)
        self.assertLess(second_move, rollback)
        self.assertIn("finally {", HEADPHONE_BUNDLE_UPDATE)

    @unittest.skipUnless(POWERSHELL, "PowerShell is required for the download test")
    def test_remote_calibration_traversal_is_rejected_without_mutation(self) -> None:
        with tempfile.TemporaryDirectory() as temp_directory:
            temp = pathlib.Path(temp_directory)
            output = temp / "output"
            before = seed_calibration_output(output)
            result = run_calibration_update(
                temp,
                output,
                {"version": 1, "targets": [{"file": "../escaped.txt"}]},
            )

            self.assertNotEqual(result.returncode, 0)
            self.assertIn("Unsafe or duplicate target file name", result.stderr)
            self.assertEqual(tree_snapshot(output), before)
            self.assertFalse((temp / "escaped.txt").exists())
            self.assertEqual(list(temp.glob(".output.update.*")), [])

    @unittest.skipUnless(POWERSHELL, "PowerShell is required for the download test")
    def test_case_insensitive_target_alias_is_rejected_without_mutation(self) -> None:
        with tempfile.TemporaryDirectory() as temp_directory:
            temp = pathlib.Path(temp_directory)
            output = temp / "output"
            before = seed_calibration_output(output)
            result = run_calibration_update(
                temp,
                output,
                {
                    "version": 1,
                    "targets": [{"file": "model.txt"}, {"file": "MODEL.txt"}],
                },
            )

            self.assertNotEqual(result.returncode, 0)
            self.assertIn("Unsafe or duplicate target file name", result.stderr)
            self.assertEqual(tree_snapshot(output), before)
            self.assertEqual(list(temp.glob(".output.update.*")), [])

    @unittest.skipUnless(POWERSHELL, "PowerShell is required for the download test")
    def test_second_target_download_failure_preserves_previous_bundle(self) -> None:
        with tempfile.TemporaryDirectory() as temp_directory:
            temp = pathlib.Path(temp_directory)
            output = temp / "output"
            before = seed_calibration_output(output)
            result = run_calibration_update(
                temp,
                output,
                {
                    "version": 1,
                    "targets": [
                        {"file": "new-a.txt"},
                        {"file": "new-b.txt"},
                    ],
                },
                fail_file="new-b.txt",
            )

            self.assertNotEqual(result.returncode, 0)
            self.assertIn("503", result.stderr)
            self.assertEqual(tree_snapshot(output), before)
            self.assertEqual(list(temp.glob(".output.update.*")), [])

    @unittest.skipUnless(POWERSHELL, "PowerShell is required for the download test")
    def test_streaming_index_limit_aborts_before_disk_growth(self) -> None:
        with tempfile.TemporaryDirectory() as temp_directory:
            temp = pathlib.Path(temp_directory)
            output = temp / "output"
            before = seed_calibration_output(output)
            result = run_calibration_update(
                temp,
                output,
                {"version": 1, "targets": [{"file": "unused.txt"}]},
                oversized_index=True,
            )

            self.assertNotEqual(result.returncode, 0)
            self.assertIn("1048576-byte limit", result.stderr)
            self.assertEqual(tree_snapshot(output), before)
            self.assertEqual(list(temp.glob(".output.update.*")), [])

    @unittest.skipUnless(POWERSHELL, "PowerShell is required for the download test")
    def test_success_replaces_managed_set_and_preserves_unmanaged_files(self) -> None:
        with tempfile.TemporaryDirectory() as temp_directory:
            temp = pathlib.Path(temp_directory)
            output = temp / "output"
            before = seed_calibration_output(output)
            index = {
                "version": 1,
                "targets": [{"file": "new-a.txt"}, {"file": "new-b.txt"}],
            }
            result = run_calibration_update(temp, output, index)

            self.assertEqual(result.returncode, 0, msg=result.stderr)
            self.assertFalse((output / "old-target.txt").exists())
            self.assertEqual((output / "README.md").read_bytes(), before["README.md"])
            self.assertEqual(
                (output / "local-notes.bin").read_bytes(),
                before["local-notes.bin"],
            )
            self.assertEqual((output / "new-a.txt").read_text(), "payload:new-a.txt")
            self.assertEqual((output / "new-b.txt").read_text(), "payload:new-b.txt")
            self.assertEqual(
                json.loads((output / "index.json").read_text(encoding="utf-8")),
                index,
            )
            self.assertEqual(list(temp.glob(".output.update.*")), [])

    @unittest.skipUnless(POWERSHELL, "PowerShell is required for the download test")
    def test_transient_socket_reset_retries_without_reopening_destination(self) -> None:
        with tempfile.TemporaryDirectory() as temp_directory:
            temp = pathlib.Path(temp_directory)
            output = temp / "output"
            seed_calibration_output(output)
            index = {"version": 1, "targets": [{"file": "new-a.txt"}]}
            result = run_calibration_update(
                temp,
                output,
                index,
                reset_once_file="new-a.txt",
            )

            self.assertEqual(result.returncode, 0, msg=result.stderr)
            self.assertEqual((output / "new-a.txt").read_text(), "payload:new-a.txt")
            self.assertFalse((output / "old-target.txt").exists())

    @unittest.skipUnless(POWERSHELL, "PowerShell is required for the download test")
    def test_remote_readme_is_not_accepted_as_managed_payload(self) -> None:
        with tempfile.TemporaryDirectory() as temp_directory:
            temp = pathlib.Path(temp_directory)
            output = temp / "output"
            before = seed_calibration_output(output)
            result = run_calibration_update(
                temp,
                output,
                {"version": 1, "targets": [{"file": "README.md"}]},
            )

            self.assertNotEqual(result.returncode, 0)
            self.assertIn("Unsafe or duplicate target file name", result.stderr)
            self.assertEqual(tree_snapshot(output), before)

    @unittest.skipUnless(POWERSHELL, "PowerShell is required for the download test")
    def test_invalid_index_shapes_do_not_mutate_previous_bundle(self) -> None:
        invalid_indexes = [
            {},
            {"targets": [{"file": "new.txt"}]},
            {"version": 1},
            {"version": 1, "targets": None},
            {"version": 1, "targets": []},
            {"version": 1, "targets": {"file": "new.txt"}},
            {"version": 1, "targets": [{}]},
            {"version": 1, "targets": [{"file": "   "}]},
            {"version": 1, "targets": ["new.txt"]},
        ]
        for case_number, index in enumerate(invalid_indexes):
            with self.subTest(index=index), tempfile.TemporaryDirectory() as temp_directory:
                temp = pathlib.Path(temp_directory)
                output = temp / "output"
                before = seed_calibration_output(output)
                result = run_calibration_update(temp, output, index)

                self.assertNotEqual(result.returncode, 0, msg=f"case {case_number}")
                self.assertEqual(tree_snapshot(output), before)
                self.assertEqual(list(temp.glob(".output.update.*")), [])


if __name__ == "__main__":
    unittest.main()
