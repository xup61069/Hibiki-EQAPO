#!/usr/bin/env python3
"""Regression contracts for unattended installer behavior."""

from __future__ import annotations

import base64
import gzip
import os
import pathlib
import re
import shutil
import subprocess
import tempfile
import time
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
SETUP_SOURCE = (ROOT / "Setup" / "Setup.nsi").read_text(encoding="utf-8")
SETUP64_SOURCE = (ROOT / "Setup" / "Setup64.nsi").read_text(encoding="utf-8")
MANIFEST_HELPER_SOURCE = (
    ROOT / "Setup" / "InstallerRecoveryManifest.nsh"
).read_text(encoding="utf-8")
MANIFEST_HARNESS_SOURCE = (
    ROOT / "tests" / "installer-recovery-manifest-harness.nsi"
).read_text(encoding="utf-8")
INSTALLER_BUILD_SCRIPT = (
    ROOT / "scripts" / "build-installer-x64.ps1"
).read_text(encoding="utf-8")
INSTALL_ROOT_ACL_HELPER_PATH = ROOT / "Setup" / "check-install-root-acl.ps1"
INSTALL_ROOT_ACL_HELPER = INSTALL_ROOT_ACL_HELPER_PATH.read_bytes()
INSTALL_ROOT_ACL_DEFINES = (
    ROOT / "Setup" / "InstallRootAclCheck.nsh"
).read_text(encoding="utf-8")
EMBEDDED_POWERSHELL_DEFINES_PATH = (
    ROOT / "Setup" / "EmbeddedPowerShellHelpers.nsh"
)
EMBEDDED_POWERSHELL_DEFINES = (
    EMBEDDED_POWERSHELL_DEFINES_PATH.read_text(encoding="utf-8")
    if EMBEDDED_POWERSHELL_DEFINES_PATH.is_file()
    else ""
)
STOP_PRODUCT_PROCESSES_HELPER = (
    ROOT / "Setup" / "stop-product-processes.ps1"
).read_bytes()
X64_LOAD_CHECK_HELPER = (ROOT / "Setup" / "x64-load-check.ps1").read_bytes()
QT_CONFIG = (ROOT / "Setup" / "qt.conf").read_text(encoding="utf-8")
DEVICE_SELECTOR_SOURCE = (ROOT / "DeviceSelector" / "main.cpp").read_text(
    encoding="utf-8"
)
UPDATE_CHECKER_SOURCE = (ROOT / "UpdateChecker" / "main.cpp").read_text(
    encoding="utf-8"
)
WINDOWS_POWERSHELL = (
    pathlib.Path(os.environ.get("WINDIR", "C:/Windows"))
    / "System32"
    / "WindowsPowerShell"
    / "v1.0"
    / "powershell.exe"
)
PING = (
    pathlib.Path(os.environ.get("WINDIR", "C:/Windows"))
    / "System32"
    / "ping.exe"
)
CMD = pathlib.Path(os.environ.get("WINDIR", "C:/Windows")) / "System32" / "cmd.exe"
ICACLS = (
    pathlib.Path(os.environ.get("WINDIR", "C:/Windows"))
    / "System32"
    / "icacls.exe"
)


def embedded_script_chunks(prefix: str) -> list[str]:
    chunks = {
        int(index): value
        for index, value in re.findall(
            rf'{prefix}_CHUNK_(\d+) "([^"]*)"',
            EMBEDDED_POWERSHELL_DEFINES,
        )
    }
    if not chunks:
        return []
    if set(chunks) != set(range(max(chunks) + 1)):
        raise AssertionError(f"{prefix} chunks are not contiguous")
    return [chunks[index] for index in range(max(chunks) + 1)]


def embedded_script_decoder(environment_prefix: str, chunk_count: int) -> str:
    encoded = "+".join(
        f"$env:{environment_prefix}_{index}" for index in range(chunk_count)
    )
    return (
        f"$z={encoded};"
        "$b=[Convert]::FromBase64String($z);"
        "$m=[IO.MemoryStream]::new($b);"
        "$g=[IO.Compression.GzipStream]::new("
        "$m,[IO.Compression.CompressionMode]::Decompress);"
        "$r=[IO.StreamReader]::new($g,[Text.Encoding]::UTF8);"
    )


class InstallerContractTests(unittest.TestCase):
    def test_silent_update_uses_headless_audio_restart(self) -> None:
        self.assertIn('${If} ${Silent}', SETUP_SOURCE)
        self.assertIn('DeviceSelector.exe" /r /s', SETUP_SOURCE)
        self.assertIn('arguments().contains("/r", Qt::CaseInsensitive)', DEVICE_SELECTOR_SOURCE)
        self.assertIn('ServiceHelper::restartService(L"AudioSrv")', DEVICE_SELECTOR_SOURCE)

    def test_silent_task_updates_do_not_show_error_dialogs(self) -> None:
        self.assertIn('UpdateChecker.exe" -i -s', SETUP_SOURCE)
        self.assertIn('UpdateChecker.exe" -u -s', SETUP_SOURCE)
        self.assertIn('QCommandLineOption silentOption("s"', UPDATE_CHECKER_SOURCE)
        self.assertGreaterEqual(UPDATE_CHECKER_SOURCE.count("if (!silentMode)"), 2)

    def test_installer_message_boxes_are_silent_guarded(self) -> None:
        # Fatal and warning messages stay available interactively, but every
        # installer MessageBox must be nested in an explicit silent guard.
        self.assertGreaterEqual(SETUP_SOURCE.count("MessageBox "), 7)
        self.assertGreaterEqual(SETUP_SOURCE.count('${IfNot} ${Silent}'), 7)

    def test_default_start_menu_folder_is_migrated_on_upgrade(self) -> None:
        self.assertIn('StrCpy $0 "$OldStartMenuFolder" 14', SETUP_SOURCE)
        self.assertIn(
            'StrCpy $StartMenuFolder "${PRODUCT_LABEL} ${VERSION}"',
            SETUP_SOURCE,
        )

    def test_user_facing_identity_is_hibiki_eqapo_while_compatibility_ids_remain(self) -> None:
        self.assertIn(
            '!define PRODUCT_LABEL "Hibiki EQAPO"',
            SETUP_SOURCE,
        )
        self.assertIn(
            '!define PRODUCT_FULL_LABEL '
            '"${PRODUCT_LABEL} (unofficial Equalizer APO fork)"',
            SETUP_SOURCE,
        )
        self.assertIn('Name "${PRODUCT_FULL_LABEL} ${VERSION}"', SETUP_SOURCE)
        self.assertIn(
            'VIAddVersionKey /LANG=1033 "ProductName" '
            '"${PRODUCT_FULL_LABEL}"',
            SETUP_SOURCE,
        )
        self.assertIn(
            '"DisplayName" "${PRODUCT_FULL_LABEL}"', SETUP_SOURCE
        )
        self.assertNotIn('Name "Equalizer APO ${VERSION}"', SETUP_SOURCE)

        # These identifiers and tool names are compatibility contracts, not a
        # claim that this fork is the upstream product.
        self.assertIn('!define REGPATH "Software\\EqualizerAPO"', SETUP_SOURCE)
        self.assertIn(
            '!define UNINST_REGPATH '
            '"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\EqualizerAPO"',
            SETUP_SOURCE,
        )
        self.assertIn(
            'StrCpy $INSTDIR "$PROGRAMFILES64\\EqualizerAPO"', SETUP_SOURCE
        )
        self.assertIn("EqualizerAPOUpdateChecker", SETUP_SOURCE)
        self.assertIn("Equalizer APO Configuration Editor.lnk", SETUP_SOURCE)
        self.assertIn("Equalizer APO Device Selector.lnk", SETUP_SOURCE)
        self.assertIn(
            'OutFile "Hibiki-EQAPO-x64-${VERSION}.exe"', SETUP64_SOURCE
        )

    def test_qt_plugins_are_found_outside_the_install_working_directory(self) -> None:
        self.assertIn('File "qt.conf"', SETUP_SOURCE)
        self.assertIn(
            '!insertmacro RemoveUninstallPayloadFile "$INSTDIR\\qt.conf"',
            SETUP_SOURCE,
        )
        self.assertEqual(QT_CONFIG.strip(), "[Paths]\nPlugins = qt")

    def test_install_root_is_trusted_before_elevated_payload_use(self) -> None:
        self.assertIn('!include "InstallRootAclCheck.nsh"', SETUP_SOURCE)
        section_start = SETUP_SOURCE.index('Section "-Install"')
        section_end = SETUP_SOURCE.index("SectionEnd", section_start)
        section = SETUP_SOURCE[section_start:section_end]
        first_gate = section.index('StrCpy $InstallRootAllowMissing "1"')
        create_root = section.index('CreateDirectory "$INSTDIR"')
        second_gate = section.index(
            'StrCpy $InstallRootAllowMissing "0"', create_root
        )
        final_validation = section.index("Call ValidateInstallRootAcl", second_gate)
        install_outdir = section.index('SetOutPath "$INSTDIR"', final_validation)
        restore_point = section.index("Call CreateRestorePoint", install_outdir)
        self.assertLess(first_gate, create_root)
        self.assertLess(create_root, second_gate)
        self.assertLess(second_gate, final_validation)
        self.assertLess(final_validation, install_outdir)
        self.assertLess(install_outdir, restore_point)

        recovery_start = SETUP_SOURCE.index(
            "Function LoadInstallRecoveryTargetFromJournal"
        )
        recovery_end = SETUP_SOURCE.index("FunctionEnd", recovery_start)
        recovery = SETUP_SOURCE[recovery_start:recovery_end]
        self.assertIn('StrCpy $InstallRootAllowMissing "0"', recovery)
        self.assertIn("Call ValidateInstallRootAcl", recovery)

        uninstall_start = SETUP_SOURCE.index("Function un.onInit")
        uninstall_end = SETUP_SOURCE.index("FunctionEnd", uninstall_start)
        uninstall_init = SETUP_SOURCE[uninstall_start:uninstall_end]
        self.assertIn('ReadRegStr $0 HKLM ${REGPATH} "InstallPath"', uninstall_init)
        self.assertIn('GetFullPathName $2 "$EXEDIR"', uninstall_init)
        self.assertIn("Call un.ValidateInstallRootAcl", uninstall_init)
        self.assertIn("Abort", uninstall_init)

    @unittest.skipUnless(
        os.name == "nt" and WINDOWS_POWERSHELL.is_file(),
        "Windows PowerShell 5.1 is required for the embedded ACL helper test",
    )
    def test_embedded_install_root_acl_helper_executes_exact_payload(self) -> None:
        chunks = {
            int(index): value
            for index, value in re.findall(
                r'INSTALL_ROOT_ACL_CHECK_CHUNK_(\d+) "([^"]*)"',
                INSTALL_ROOT_ACL_DEFINES,
            )
        }
        self.assertEqual(set(chunks), set(range(8)))
        encoded = "".join(chunks[index] for index in range(8))
        self.assertEqual(gzip.decompress(base64.b64decode(encoded)), INSTALL_ROOT_ACL_HELPER)

        decoder = (
            "$z=$env:EQAPO_ACL_CODE_0+$env:EQAPO_ACL_CODE_1+"
            "$env:EQAPO_ACL_CODE_2+$env:EQAPO_ACL_CODE_3+"
            "$env:EQAPO_ACL_CODE_4+$env:EQAPO_ACL_CODE_5+"
            "$env:EQAPO_ACL_CODE_6+$env:EQAPO_ACL_CODE_7;"
            "$b=[Convert]::FromBase64String($z);"
            "$m=[IO.MemoryStream]::new($b);"
            "$g=[IO.Compression.GzipStream]::new("
            "$m,[IO.Compression.CompressionMode]::Decompress);"
            "$r=[IO.StreamReader]::new($g,[Text.Encoding]::UTF8);"
            "&([ScriptBlock]::Create($r.ReadToEnd()))"
        )
        self.assertIn(decoder.replace("$", "$$"), SETUP_SOURCE)

        def run_helper(install_root: pathlib.Path) -> subprocess.CompletedProcess[str]:
            environment = os.environ.copy()
            environment["EQAPO_INSTALL_ROOT"] = str(install_root)
            environment["EQAPO_ALLOW_MISSING_INSTALL_ROOT"] = "0"
            for index in range(8):
                environment[f"EQAPO_ACL_CODE_{index}"] = chunks[index]
            return subprocess.run(
                [
                    str(WINDOWS_POWERSHELL),
                    "-NoProfile",
                    "-NonInteractive",
                    "-ExecutionPolicy",
                    "Bypass",
                    "-Command",
                    decoder,
                ],
                capture_output=True,
                text=True,
                encoding="utf-8",
                errors="replace",
                timeout=15,
                check=False,
                env=environment,
            )

        safe_root = pathlib.Path(os.environ.get("WINDIR", "C:/Windows"))
        safe_result = run_helper(safe_root)
        self.assertEqual(
            safe_result.returncode,
            0,
            msg=f"stdout:\n{safe_result.stdout}\nstderr:\n{safe_result.stderr}",
        )
        with tempfile.TemporaryDirectory(prefix="eqapo-weak-install-root-") as temp_dir:
            weak_result = run_helper(pathlib.Path(temp_dir))
        self.assertNotEqual(
            weak_result.returncode,
            0,
            msg="a current-user-owned temporary directory must be rejected",
        )

    def test_installer_validates_payload_before_persistent_changes(self) -> None:
        self.assertIn("CRCCheck force", SETUP_SOURCE)
        self.assertIn('File /oname=NOTICE.md "..\\NOTICE.md"', SETUP_SOURCE)
        self.assertIn('!insertmacro RequireInstalledAsset "$INSTDIR\\NOTICE.md"', SETUP_SOURCE)
        self.assertLess(
            SETUP_SOURCE.index("Call VerifyRequiredAssets"),
            SETUP_SOURCE.index(
                '!insertmacro WriteRequiredRegStr HKLM "${REGPATH}" '
                '"InstallPath"'
            ),
        )
        self.assertIn("Function SaveProtectedAudioSetting", SETUP_SOURCE)
        self.assertIn("Function RestoreProtectedAudioSetting", SETUP_SOURCE)

    def test_failed_registration_rolls_back_the_application_tree(self) -> None:
        prepare_call = "Call PrepareInstallTransaction"
        install_new_dll = 'File "${BINPATH}\\EqualizerAPO.dll"'
        restore_call = "Call RollbackInstallTransaction"
        commit_call = "Call CommitInstallTransaction"

        self.assertNotIn("GetTempFileName $InstallRollbackDirectory", SETUP_SOURCE)
        self.assertNotIn("$PLUGINSDIR\\install-rollback", SETUP_SOURCE)
        self.assertIn(
            'StrCpy $InstallRecoveryProductRoot '
            '"$InstallRecoveryCommonAppData\\EqualizerAPO"',
            SETUP_SOURCE,
        )
        self.assertIn(
            'StrCpy $InstallRollbackDirectory '
            '"$InstallRecoveryProductRoot\\InstallerRecovery"',
            SETUP_SOURCE,
        )
        self.assertIn(
            '"$SYSDIR\\robocopy.exe" "$INSTDIR" '
            '"$InstallRollbackFiles" /E /COPY:DATS',
            SETUP_SOURCE,
        )
        self.assertIn(
            '/XD "$INSTDIR\\config" "$INSTDIR\\VSTPlugins"', SETUP_SOURCE
        )
        self.assertIn('$InstallRollbackCopyCode >= 8', SETUP_SOURCE)
        self.assertLess(SETUP_SOURCE.index(prepare_call), SETUP_SOURCE.index(install_new_dll))

        install_start = SETUP_SOURCE.index('Section "-Install"')
        install_end = SETUP_SOURCE.index("SectionEnd", install_start)
        install_body = SETUP_SOURCE[install_start:install_end]
        registration_index = install_body.index(
            'ExecWait \'"$SYSDIR\\regsvr32.exe" /s '
            '"$INSTDIR\\EqualizerAPO.dll"\' $1'
        )
        failure_index = install_body.index("${If} $1 != 0", registration_index)
        restore_index = install_body.index(restore_call, failure_index)
        abort_index = install_body.index("Abort", restore_index)
        commit_index = install_body.index(commit_call, abort_index)
        self.assertLess(failure_index, restore_index)
        self.assertLess(restore_index, abort_index)
        self.assertLess(abort_index, commit_index)

        self.assertIn("Function RemoveInstalledProductFiles", SETUP_SOURCE)
        self.assertIn(
            '!insertmacro DeleteTransactionFile '
            '"$INSTDIR\\EqualizerAPO.dll"',
            SETUP_SOURCE,
        )
        self.assertIn(
            '!insertmacro DeleteTransactionFile '
            '"$INSTDIR\\qt\\platforms\\qwindows.dll"',
            SETUP_SOURCE,
        )
        remove_start = SETUP_SOURCE.index("Function RemoveInstalledProductFiles")
        remove_end = SETUP_SOURCE.index("FunctionEnd", remove_start)
        remove_body = SETUP_SOURCE[remove_start:remove_end]
        self.assertNotIn('RMDir /r "$INSTDIR\\qt"', remove_body)
        self.assertIn(
            '"$SYSDIR\\robocopy.exe" "$InstallRollbackFiles" '
            '"$INSTDIR" /E /COPY:DATS',
            SETUP_SOURCE,
        )
        self.assertGreaterEqual(
            SETUP_SOURCE.count(
                'ExecWait \'"$SYSDIR\\regsvr32.exe" /s '
                '"$INSTDIR\\EqualizerAPO.dll"\''
            ),
            2,
        )
        self.assertIn(
            'ExecWait \'"$SYSDIR\\regsvr32.exe" /u /s '
            '"$INSTDIR\\EqualizerAPO.dll"\'',
            SETUP_SOURCE,
        )
        self.assertIn("Function .onInstFailed", SETUP_SOURCE)
        self.assertIn('$NewApoRegistrationAttempted == "1"', SETUP_SOURCE)
        self.assertIn(
            'StrCpy $NewApoRegistrationAttempted "1"', SETUP_SOURCE
        )

        rollback_start = SETUP_SOURCE.index("Function RollbackInstallTransaction")
        rollback_end = SETUP_SOURCE.index("FunctionEnd", rollback_start)
        rollback_body = SETUP_SOURCE[rollback_start:rollback_end]
        unregister = rollback_body.index(
            'ExecWait \'"$SYSDIR\\regsvr32.exe" /u /s '
            '"$INSTDIR\\EqualizerAPO.dll"\''
        )
        phase_guard = rollback_body.index('$NewApoRegistrationAttempted == "1"')
        self.assertLess(phase_guard, unregister)
        self.assertIn("ClearErrors", rollback_body[:unregister])
        self.assertIn("${Errors}", rollback_body[unregister:])
        reregister = rollback_body.index(
            'ExecWait \'"$SYSDIR\\regsvr32.exe" /s '
            '"$INSTDIR\\EqualizerAPO.dll"\' '
            "$ApoRollbackRegistrationCode"
        )
        self.assertLess(
            rollback_body.rindex("ClearErrors", 0, reregister), reregister
        )
        self.assertLess(
            rollback_body.rindex(
                'StrCpy $ApoRollbackRegistrationCode '
                '"process did not start"',
                0,
                reregister,
            ),
            reregister,
        )
        self.assertGreater(
            rollback_body.index("${If} ${Errors}", reregister), reregister
        )
        self.assertIn(
            "Recovery files remain at $InstallRollbackDirectory", rollback_body
        )
        # The rollback decision is made durable first. Rename/snapshot cleanup is
        # restartable, and the journal is always the final deletion.
        self.assertEqual(
            rollback_body.count('StrCpy $InstallRollbackState "0"'), 2
        )
        rollback_phase = rollback_body.rindex('"Phase" "rollback-cleanup"')
        rollback_state = rollback_body.rindex('StrCpy $InstallRollbackState "0"')
        renamed_cleanup = rollback_body.rindex("Call DiscardRenamedProductFiles")
        snapshot_cleanup = rollback_body.rindex("Call DiscardInstallRecoverySnapshot")
        journal_cleanup = rollback_body.rindex("Call ClearInstallRecoveryJournal")
        self.assertLess(rollback_phase, rollback_state)
        self.assertLess(rollback_state, renamed_cleanup)
        self.assertLess(renamed_cleanup, snapshot_cleanup)
        self.assertLess(snapshot_cleanup, journal_cleanup)

        self.assertNotIn("$INSTDIR\\config", remove_body)
        self.assertNotIn("$INSTDIR\\VSTPlugins", remove_body)

    def test_commit_follows_every_fallible_persistent_operation(self) -> None:
        install_start = SETUP_SOURCE.index('Section "-Install"')
        install_end = SETUP_SOURCE.index("SectionEnd", install_start)
        install_body = SETUP_SOURCE[install_start:install_end]
        registration = install_body.index(
            'ExecWait \'"$SYSDIR\\regsvr32.exe" /s '
            '"$INSTDIR\\EqualizerAPO.dll"\' $1'
        )
        commit = install_body.index("Call CommitInstallTransaction", registration)

        persistent_operations = (
            'nsExec::ExecToLog \'"$SYSDIR\\icacls.exe"',
            '!insertmacro WriteRequiredRegStr HKLM "${REGPATH}" "InstallPath"',
            'WriteUninstaller "$INSTDIR\\Uninstall.exe"',
            "!insertmacro DeleteProductShortcuts $OldStartMenuFolder",
            '!insertmacro MUI_STARTMENU_WRITE_BEGIN Application',
            '!insertmacro WriteRequiredRegStr HKLM "${UNINST_REGPATH}" "DisplayName"',
            'DeviceSelector.exe" /r /s',
            'UpdateChecker.exe" -i -s',
            'UpdateChecker.exe" -u -s',
        )
        for operation in persistent_operations:
            with self.subTest(operation=operation):
                self.assertLess(install_body.index(operation), commit)

        failure_label = install_body.index("installTransactionFailed:")
        rollback = install_body.index("Call RollbackInstallTransaction", failure_label)
        abort = install_body.index("Abort", rollback)
        self.assertLess(commit, failure_label)
        self.assertLess(failure_label, rollback)
        self.assertLess(rollback, abort)

        commit_start = SETUP_SOURCE.index("Function CommitInstallTransaction")
        commit_end = SETUP_SOURCE.index("FunctionEnd", commit_start)
        commit_body = SETUP_SOURCE[commit_start:commit_end]
        self.assertIn(
            'WriteRegStr HKLM ${INSTALLER_APP_RECOVERY_REGPATH} '
            '"Phase" "committed"',
            commit_body,
        )
        self.assertIn(
            'ReadRegStr $InstallRecoveryPhase HKLM '
            '${INSTALLER_APP_RECOVERY_REGPATH} "Phase"',
            commit_body,
        )

        verification = install_body.index("Call VerifyRequiredAssets")
        self.assertLess(verification, registration)
        missing_asset = SETUP_SOURCE.index("missingRequiredAsset:")
        rollback_after_missing = SETUP_SOURCE.index(
            "Call RollbackInstallTransaction", missing_asset
        )
        abort_after_missing = SETUP_SOURCE.index("Abort", rollback_after_missing)
        self.assertLess(rollback_after_missing, abort_after_missing)

    def test_old_file_cleanup_is_scoped_to_product_files(self) -> None:
        self.assertIn("Function DiscardRenamedProductFiles", SETUP_SOURCE)
        self.assertIn("Call DiscardRenamedProductFiles", SETUP_SOURCE)
        self.assertIn('!include "InstallerRecoveryManifest.nsh"', SETUP_SOURCE)

        append_open = MANIFEST_HELPER_SOURCE.index(
            'FileOpen ${handle} "${path}" a'
        )
        append_seek = MANIFEST_HELPER_SOURCE.index(
            "FileSeek ${handle} 0 END", append_open
        )
        append_write = MANIFEST_HELPER_SOURCE.index(
            'FileWriteUTF16LE ${handle} "${line}$\\r$\\n"', append_seek
        )
        append_close = MANIFEST_HELPER_SOURCE.index(
            "FileClose ${handle}", append_write
        )
        self.assertLess(append_open, append_seek)
        self.assertLess(append_seek, append_write)
        self.assertLess(append_write, append_close)
        self.assertIn('StrCpy ${result} "1"', MANIFEST_HELPER_SOURCE)
        self.assertIn('StrCpy ${handle} ""', MANIFEST_HELPER_SOURCE)

        macro_start = SETUP_SOURCE.index("!macro RenameAndDelete path")
        macro_end = SETUP_SOURCE.index("!macroend", macro_start)
        macro_body = SETUP_SOURCE[macro_start:macro_end]
        identity_query = macro_body.index("Call QueryRenameFileIdentityAndHold")
        rename = macro_body.index('Rename "${path}" "$renamePath"')
        rename_failure = macro_body.index("${If} ${Errors}", rename)
        rollback_after_rename = macro_body.index(
            "Call RollbackInstallTransaction", rename_failure
        )
        abort_after_rename = macro_body.index("Abort", rollback_after_rename)
        append = macro_body.index(
            "AppendInstallerRecoveryManifestLine", abort_after_rename
        )
        append_failure = macro_body.index(
            '$RenameManifestWriteFailed == "1"', append
        )
        close_after_append = macro_body.rindex("Call CloseRenameIdentityHandle")
        rollback = macro_body.index("Call RollbackInstallTransaction", append_failure)
        abort = macro_body.index("Abort", rollback)
        self.assertLess(identity_query, rename)
        self.assertLess(rename, rename_failure)
        self.assertLess(rename_failure, rollback_after_rename)
        self.assertLess(rollback_after_rename, abort_after_rename)
        self.assertLess(abort_after_rename, append)
        self.assertLess(append, append_failure)
        self.assertLess(append_failure, rollback)
        self.assertLess(rollback, abort)
        self.assertLess(abort, close_after_append)
        self.assertIn("$RenameIdentityVolumeSerial", macro_body)
        self.assertIn("$RenameIdentityFileIndexHigh", macro_body)
        self.assertIn("$RenameIdentityFileIndexLow", macro_body)
        self.assertNotIn("FileOpen", macro_body)
        self.assertNotIn("FileWrite", macro_body)

        cleanup_start = SETUP_SOURCE.index("Function DiscardRenamedProductFiles")
        cleanup_end = SETUP_SOURCE.index("FunctionEnd", cleanup_start)
        cleanup_body = SETUP_SOURCE[cleanup_start:cleanup_end]
        self.assertIn('GetFullPathName $5 "$INSTDIR"', cleanup_body)
        self.assertIn('${GetParent} "$RenameIdentityPath" $4', cleanup_body)
        self.assertIn('${GetFileName} "$RenameIdentityPath" $9', cleanup_body)
        self.assertIn('GetFullPathName $4 "$4"', cleanup_body)
        self.assertIn(
            'StrCpy $RenameCleanupInstallPrefix "$5\\"', cleanup_body
        )
        self.assertIn('StrCpy $8 "$4" $7', cleanup_body)
        self.assertIn(
            '${If} $8 == "$RenameCleanupInstallPrefix"', cleanup_body
        )
        self.assertIn("ERROR_FILE_NOT_FOUND", cleanup_body)
        self.assertIn("ERROR_PATH_NOT_FOUND", cleanup_body)
        self.assertIn("Call DeleteRenameFileByIdentity", cleanup_body)
        self.assertNotIn("/REBOOTOK", cleanup_body)
        self.assertIn("FILE_ATTRIBUTE_REPARSE_POINT", cleanup_body)
        self.assertNotIn('Delete /REBOOTOK "$INSTDIR\\*.old"', SETUP_SOURCE)
        self.assertNotIn('Delete /REBOOTOK "$INSTDIR\\*.old.*"', SETUP_SOURCE)
        self.assertNotIn('Delete "${path}.old', SETUP_SOURCE)

    def test_rename_cleanup_uses_stable_identity_and_a_flushed_one_shot_gate(
        self,
    ) -> None:
        query_start = SETUP_SOURCE.index("Function QueryRenameFileIdentityAndHold")
        query_end = SETUP_SOURCE.index("FunctionEnd", query_start)
        query_body = SETUP_SOURCE[query_start:query_end]
        query_open = query_body.index(
            'CreateFileW(w "$RenameIdentityPath", i ${FILE_READ_ATTRIBUTES}, '
            'i ${FILE_SHARE_READ_WRITE_DELETE}'
        )
        query_type = query_body.index("GetFileType(p r0)", query_open)
        query_info = query_body.index(
            "GetFileInformationByHandle(p r0, p r1)", query_type
        )
        query_parse = query_body.index(
            '*$1(i .r2, &v24, i .r3, &v12, i .r4, i .r5)', query_info
        )
        self.assertIn(
            "${FILE_FLAG_OPEN_REPARSE_POINT}|${FILE_FLAG_BACKUP_SEMANTICS}",
            query_body,
        )
        self.assertIn("$6 != ${FILE_TYPE_DISK}", query_body)
        self.assertIn("${AndIf} $5 == 0", query_body)
        self.assertLess(query_open, query_type)
        self.assertLess(query_type, query_info)
        self.assertLess(query_info, query_parse)

        delete_start = SETUP_SOURCE.index("Function DeleteRenameFileByIdentity")
        delete_end = SETUP_SOURCE.index("FunctionEnd", delete_start)
        delete_body = SETUP_SOURCE[delete_start:delete_end]
        delete_open = delete_body.index(
            'CreateFileW(w "$RenameIdentityPath", '
            'i ${DELETE_ACCESS}|${FILE_READ_ATTRIBUTES}, '
            'i ${FILE_SHARE_READ_WRITE}'
        )
        delete_type = delete_body.index("GetFileType(p r0)", delete_open)
        delete_info = delete_body.index(
            "GetFileInformationByHandle(p r0, p r1)", delete_type
        )
        compare_volume = delete_body.index(
            '$3 != "$RenameExpectedVolumeSerial"', delete_info
        )
        compare_high = delete_body.index(
            '$4 != "$RenameExpectedFileIndexHigh"', compare_volume
        )
        compare_low = delete_body.index(
            '$5 != "$RenameExpectedFileIndexLow"', compare_high
        )
        disposition = delete_body.index(
            "SetFileInformationByHandle(p r0, "
            "i ${FILE_DISPOSITION_INFO_CLASS}, p r6, i 1)",
            compare_low,
        )
        self.assertIn(
            "${FILE_FLAG_OPEN_REPARSE_POINT}|${FILE_FLAG_BACKUP_SEMANTICS}",
            delete_body,
        )
        self.assertIn("$7 != ${FILE_TYPE_DISK}", delete_body)
        self.assertIn("${AndIf} $5 == 0", delete_body)
        self.assertLess(delete_open, delete_type)
        self.assertLess(delete_type, delete_info)
        self.assertLess(delete_info, compare_volume)
        self.assertLess(compare_volume, compare_high)
        self.assertLess(compare_high, compare_low)
        self.assertLess(compare_low, disposition)
        self.assertNotIn('Delete "$RenameIdentityPath"', delete_body)
        self.assertNotIn("/REBOOTOK", delete_body)

        flush_start = SETUP_SOURCE.index("Function FlushRenameCleanupJournal")
        flush_end = SETUP_SOURCE.index("FunctionEnd", flush_start)
        flush_body = SETUP_SOURCE[flush_start:flush_end]
        registry_open = flush_body.index("RegOpenKeyExW")
        registry_flush = flush_body.index("RegFlushKey", registry_open)
        registry_close = flush_body.index("RegCloseKey", registry_flush)
        self.assertIn("${KEY_QUERY_VALUE_64}", flush_body)
        self.assertIn("${KEY_QUERY_VALUE}", flush_body)
        self.assertLess(registry_open, registry_flush)
        self.assertLess(registry_flush, registry_close)

        cleanup_start = SETUP_SOURCE.index("Function DiscardRenamedProductFiles")
        cleanup_end = SETUP_SOURCE.index("FunctionEnd", cleanup_start)
        cleanup_body = SETUP_SOURCE[cleanup_start:cleanup_end]
        marker_write = cleanup_body.index('"RenameCleanupStarted" 1')
        marker_readback = cleanup_body.index(
            '"RenameCleanupStarted"', marker_write + 1
        )
        marker_flush = cleanup_body.index(
            "Call FlushRenameCleanupJournal", marker_readback
        )
        manifest_open = cleanup_body.index(
            'FileOpen $RenameManifestHandle "$RenameManifestPath" r', marker_flush
        )
        identity_delete = cleanup_body.index(
            "Call DeleteRenameFileByIdentity", manifest_open
        )
        self.assertLess(marker_write, marker_readback)
        self.assertLess(marker_readback, marker_flush)
        self.assertLess(marker_flush, manifest_open)
        self.assertLess(manifest_open, identity_delete)
        self.assertIn(
            'FileReadUTF16LE $RenameManifestHandle $1', cleanup_body
        )
        manifest_close = cleanup_body.index(
            'Call CloseRenameManifest', identity_delete
        )
        self.assertLess(identity_delete, manifest_close)
        self.assertNotIn('FileOpen $0 "$RenameManifestPath" r', cleanup_body)
        self.assertNotIn('FileReadUTF16LE $0 $1', cleanup_body)
        self.assertNotIn('FileClose $0', cleanup_body)
        self.assertIn(
            'StrCpy $RenameCleanupInstallPrefix "$5\\"', cleanup_body
        )
        self.assertIn(
            'StrLen $7 "$RenameCleanupInstallPrefix"', cleanup_body
        )
        self.assertIn(
            '${If} $8 == "$RenameCleanupInstallPrefix"', cleanup_body
        )
        self.assertNotIn('StrCpy $6 "$5\\"', cleanup_body)
        self.assertNotIn('StrLen $7 "$6"', cleanup_body)
        self.assertNotIn('${If} $8 == "$6"', cleanup_body)

    def test_legacy_v302_rename_manifest_is_retired_without_guessing_paths(self) -> None:
        self.assertIn("!define INSTALL_RECOVERY_JOURNAL_VERSION 2", SETUP_SOURCE)

        metadata_start = SETUP_SOURCE.index("Function SaveInstallMetadataJournal")
        metadata_end = SETUP_SOURCE.index("FunctionEnd", metadata_start)
        metadata_body = SETUP_SOURCE[metadata_start:metadata_end]
        self.assertIn("Call PersistInstallRecoveryJournalVersion", metadata_body)
        self.assertIn('"RenameCleanupStarted" 0', metadata_body)

        prepare_start = SETUP_SOURCE.index(
            "Function PrepareRenameManifestForCleanup"
        )
        prepare_end = SETUP_SOURCE.index("FunctionEnd", prepare_start)
        prepare_body = SETUP_SOURCE[prepare_start:prepare_end]
        self.assertIn('ReadRegDWORD $0 HKLM', prepare_body)
        self.assertIn('"JournalVersion"', prepare_body)
        self.assertIn("Call RetireLegacyRenameManifest", prepare_body)
        self.assertIn("$0 != ${INSTALL_RECOVERY_JOURNAL_VERSION}", prepare_body)

        retire_start = SETUP_SOURCE.index("Function RetireLegacyRenameManifest")
        retire_end = SETUP_SOURCE.index("FunctionEnd", retire_start)
        retire_body = SETUP_SOURCE[retire_start:retire_end]
        self.assertIn('$InstallRecoveryPhase != "committed"', retire_body)
        self.assertIn('$InstallRecoveryPhase != "rollback-cleanup"', retire_body)
        self.assertIn("Call SecureExistingInstallRecoveryTree", retire_body)
        self.assertIn("RetireLegacyRenameRecoveryArtifact", retire_body)
        self.assertIn("Call PersistInstallRecoveryJournalVersion", retire_body)
        self.assertIn('"RenameCleanupStarted" 1', retire_body)
        self.assertNotIn("$INSTDIR", retire_body)
        self.assertNotIn("RecoverLegacyRenamedFile", SETUP_SOURCE)
        self.assertNotIn("RebuildLegacyRenameManifest", SETUP_SOURCE)

        cleanup_start = SETUP_SOURCE.index("Function DiscardRenamedProductFiles")
        cleanup_end = SETUP_SOURCE.index("FunctionEnd", cleanup_start)
        cleanup_records = SETUP_SOURCE[cleanup_start:cleanup_end]
        self.assertIn('"RenameCleanupStarted"', cleanup_records)
        self.assertIn('${StrTok} $RenameExpectedVolumeSerial', cleanup_records)
        self.assertIn('${StrTok} $RenameIdentityPath', cleanup_records)
        self.assertIn('$3 != "C"', cleanup_records)
        self.assertIn('$7 != "C"', cleanup_records)
        self.assertNotIn("/REBOOTOK", cleanup_records)

        cleanup_start = SETUP_SOURCE.index(
            "Function CleanupCompletedInstallTransaction"
        )
        cleanup_end = SETUP_SOURCE.index("FunctionEnd", cleanup_start)
        cleanup_body = SETUP_SOURCE[cleanup_start:cleanup_end]
        self.assertIn("Call PrepareRenameManifestForCleanup", cleanup_body)

        rollback_start = SETUP_SOURCE.index("Function RollbackInstallTransaction")
        rollback_end = SETUP_SOURCE.index("FunctionEnd", rollback_start)
        rollback_body = SETUP_SOURCE[rollback_start:rollback_end]
        rollback_decision = rollback_body.rindex('"Phase" "rollback-cleanup"')
        migration = rollback_body.index(
            "Call PrepareRenameManifestForCleanup", rollback_decision
        )
        renamed_cleanup = rollback_body.index(
            "Call DiscardRenamedProductFiles", migration
        )
        self.assertLess(rollback_decision, migration)
        self.assertLess(migration, renamed_cleanup)

    def test_manifest_append_runtime_harness_is_part_of_installer_build(self) -> None:
        self.assertIn(
            '!include "..\\Setup\\InstallerRecoveryManifest.nsh"',
            MANIFEST_HARNESS_SOURCE,
        )
        self.assertEqual(
            MANIFEST_HARNESS_SOURCE.count("AppendHarnessLine"),
            4,
        )
        self.assertIn("second-line-is-deliberately-longer", MANIFEST_HARNESS_SOURCE)
        self.assertIn("路徑-🔊-z", MANIFEST_HARNESS_SOURCE)
        self.assertIn(
            "FileReadUTF16LE $RenameManifestHandle $ReadLine",
            MANIFEST_HARNESS_SOURCE,
        )
        first_read = MANIFEST_HARNESS_SOURCE.index(
            '!insertmacro ReadHarnessLine "first" 21'
        )
        clobber = MANIFEST_HARNESS_SOURCE.index(
            "Call ClobberIdentityScratchRegisters", first_read
        )
        second_read = MANIFEST_HARNESS_SOURCE.index(
            '!insertmacro ReadHarnessLine "second-line-is-deliberately-longer" 22',
            clobber,
        )
        self.assertLess(first_read, clobber)
        self.assertLess(clobber, second_read)
        self.assertIn(
            'StrCpy $RenameCleanupInstallPrefix "$PLUGINSDIR\\"',
            MANIFEST_HARNESS_SOURCE,
        )
        self.assertIn('Delete "$ManifestPath"', MANIFEST_HARNESS_SOURCE)

        harness_call = INSTALLER_BUILD_SCRIPT.index(
            "scripts\\test-installer-recovery-manifest.ps1"
        )
        installer_compile = INSTALLER_BUILD_SCRIPT.index(
            '".\\Setup64.nsi"', harness_call
        )
        self.assertLess(harness_call, installer_compile)

    def test_regsvr32_process_creation_errors_cannot_commit(self) -> None:
        install_start = SETUP_SOURCE.index('Section "-Install"')
        install_end = SETUP_SOURCE.index("SectionEnd", install_start)
        install_body = SETUP_SOURCE[install_start:install_end]
        registration = install_body.index(
            'ExecWait \'"$SYSDIR\\regsvr32.exe" /s '
            '"$INSTDIR\\EqualizerAPO.dll"\' $1'
        )
        clear_errors = install_body.rindex("ClearErrors", 0, registration)
        sentinel = install_body.rindex(
            'StrCpy $1 "process did not start"', 0, registration
        )
        error_check = install_body.index("${If} ${Errors}", registration)
        failure_jump = install_body.index(
            "Goto apoRegistrationFailed", error_check
        )
        commit = install_body.index("Call CommitInstallTransaction", registration)
        persistent_attempt = install_body.index(
            'WriteRegDWORD HKLM ${INSTALLER_APP_RECOVERY_REGPATH} '
            '"NewApoRegistrationAttempted" 1'
        )
        self.assertLess(persistent_attempt, registration)
        self.assertLess(clear_errors, registration)
        self.assertLess(sentinel, registration)
        self.assertLess(registration, error_check)
        self.assertLess(error_check, failure_jump)
        self.assertLess(failure_jump, commit)

    def test_app_tree_recovery_journal_survives_process_and_power_loss(self) -> None:
        self.assertIn(
            '!define INSTALLER_APP_RECOVERY_REGPATH '
            '"${REGPATH}\\InstallerAppRecovery"',
            SETUP_SOURCE,
        )
        self.assertIn(
            "!define CSIDL_COMMON_APPDATA 0x23",
            SETUP_SOURCE,
        )
        self.assertIn("shell32::SHGetFolderPathW", SETUP_SOURCE)
        self.assertIn(
            'StrCpy $InstallRollbackDirectory '
            '"$InstallRecoveryProductRoot\\InstallerRecovery"',
            SETUP_SOURCE,
        )
        self.assertNotIn("GetTempFileName", SETUP_SOURCE)
        self.assertNotIn('RMDir /r "$INSTDIR"', SETUP_SOURCE)
        self.assertNotIn(
            'ReadRegStr $InstallRollbackDirectory HKLM', SETUP_SOURCE
        )

        init_start = SETUP_SOURCE.index("Function .onInit")
        init_end = SETUP_SOURCE.index("FunctionEnd", init_start)
        init_body = SETUP_SOURCE[init_start:init_end]
        protected = init_body.index("Call RecoverProtectedAudioSetting")
        app_tree = init_body.index("Call RecoverInstallTransaction")
        normal_state = init_body.index("!insertmacro MUI_LANGDLL_DISPLAY")
        self.assertLess(protected, app_tree)
        self.assertLess(app_tree, normal_state)

        prepare_start = SETUP_SOURCE.index("Function PrepareInstallTransaction")
        prepare_end = SETUP_SOURCE.index("FunctionEnd", prepare_start)
        prepare_body = SETUP_SOURCE[prepare_start:prepare_end]
        initializing = prepare_body.index('"Phase" "initializing"')
        pending = prepare_body.index('"Pending" 1', initializing)
        secure_tree = prepare_body.index("Call CreateSecureInstallRecoveryTree", pending)
        preparing = prepare_body.index('"Phase" "preparing"')
        snapshot = prepare_body.index('"$SYSDIR\\robocopy.exe"', pending)
        marker = prepare_body.index("EqualizerAPO installer app-tree recovery v1")
        prepared = prepare_body.index('"Phase" "prepared"', marker)
        active = prepare_body.index('"Phase" "active"', prepared)
        self.assertLess(initializing, pending)
        self.assertLess(pending, secure_tree)
        self.assertLess(secure_tree, preparing)
        self.assertLess(preparing, snapshot)
        self.assertLess(snapshot, marker)
        self.assertLess(marker, prepared)
        self.assertLess(prepared, active)

        recover_start = SETUP_SOURCE.index("Function RecoverInstallTransaction")
        recover_end = SETUP_SOURCE.index("FunctionEnd", recover_start)
        recover_body = SETUP_SOURCE[recover_start:recover_end]
        for phase in (
            "initializing",
            "preparing",
            "prepared",
            "committed",
            "rollback-cleanup",
        ):
            with self.subTest(phase=phase):
                self.assertIn(f'$InstallRecoveryPhase == "{phase}"', recover_body)
        self.assertIn('$InstallRecoveryPhase != "active"', recover_body)
        self.assertIn("Call ValidateActiveInstallRecoverySnapshot", recover_body)
        self.assertIn("Call LoadInstallRecoveryTargetFromJournal", recover_body)
        self.assertIn("Call RollbackInstallTransaction", recover_body)
        discard_start = SETUP_SOURCE.index("Function DiscardInstallRecoverySnapshot")
        discard_end = SETUP_SOURCE.index("FunctionEnd", discard_start)
        discard_body = SETUP_SOURCE[discard_start:discard_end]
        self.assertLess(
            discard_body.index("Call InitializeInstallRecoveryPaths"),
            discard_body.index("Call SecureExistingInstallRecoveryTree"),
        )
        self.assertLess(
            discard_body.index("Call SecureExistingInstallRecoveryTree"),
            discard_body.index('RMDir /r "$InstallRollbackDirectory"'),
        )

    def test_recovery_tree_rejects_reparse_points_and_is_created_with_a_secure_acl(
        self,
    ) -> None:
        self.assertIn("!define FILE_ATTRIBUTE_DIRECTORY 0x10", SETUP_SOURCE)
        self.assertIn("!define FILE_ATTRIBUTE_REPARSE_POINT 0x400", SETUP_SOURCE)

        validate_start = SETUP_SOURCE.index(
            "Function ValidateInstallRecoveryComponent"
        )
        validate_end = SETUP_SOURCE.index("FunctionEnd", validate_start)
        validate_body = SETUP_SOURCE[validate_start:validate_end]
        self.assertIn("kernel32::GetFileAttributesW", validate_body)
        self.assertIn("ERROR_FILE_NOT_FOUND", validate_body)
        self.assertIn("ERROR_PATH_NOT_FOUND", validate_body)
        self.assertIn("FILE_ATTRIBUTE_DIRECTORY", validate_body)
        self.assertIn("FILE_ATTRIBUTE_REPARSE_POINT", validate_body)

        components_start = SETUP_SOURCE.index(
            "Function ValidateInstallRecoveryComponents"
        )
        components_end = SETUP_SOURCE.index("FunctionEnd", components_start)
        components_body = SETUP_SOURCE[components_start:components_end]
        component_paths = (
            "$InstallRecoveryCommonAppData",
            "$InstallRecoveryProductRoot",
            "$InstallRollbackDirectory",
            "$InstallRollbackFiles",
        )
        last_path = -1
        for path in component_paths:
            with self.subTest(path=path):
                path_index = components_body.index(
                    f'StrCpy $InstallRecoveryPathToCheck "{path}"'
                )
                self.assertGreater(path_index, last_path)
                last_path = path_index

        atomic_start = SETUP_SOURCE.index(
            "Function CreateInstallRecoveryDirectoryAtomically"
        )
        atomic_end = SETUP_SOURCE.index("FunctionEnd", atomic_start)
        atomic_body = SETUP_SOURCE[atomic_start:atomic_end]
        protected_sddl = "O:BAG:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)"
        self.assertIn(protected_sddl, atomic_body)
        self.assertIn("*(i 12, p r0, i 0) p .r2", atomic_body)
        self.assertIn('${If} $2 == 0', atomic_body)
        self.assertIn("kernel32::CreateDirectoryW", atomic_body)
        self.assertLess(
            atomic_body.index("ConvertStringSecurityDescriptorToSecurityDescriptorW"),
            atomic_body.index("kernel32::CreateDirectoryW"),
        )
        self.assertLess(
            atomic_body.index("kernel32::CreateDirectoryW"),
            atomic_body.index("Call ValidateInstallRecoveryComponent"),
        )

        create_start = SETUP_SOURCE.index("Function CreateSecureInstallRecoveryTree")
        create_end = SETUP_SOURCE.index("FunctionEnd", create_start)
        create_body = SETUP_SOURCE[create_start:create_end]
        root_create = create_body.index(
            'StrCpy $InstallRecoveryAclTarget "$InstallRecoveryProductRoot"'
        )
        ownership_write = create_body.index('"ProductRootCreated" 1', root_create)
        ownership_read = create_body.index('"ProductRootCreated"', ownership_write + 1)
        child_create = create_body.index(
            'StrCpy $InstallRecoveryAclTarget "$InstallRollbackDirectory"',
            ownership_read,
        )
        files_create = create_body.index(
            'StrCpy $InstallRecoveryAclTarget "$InstallRollbackFiles"', child_create
        )
        self.assertLess(root_create, ownership_write)
        self.assertLess(ownership_write, ownership_read)
        self.assertLess(ownership_read, child_create)
        self.assertLess(child_create, files_create)

        secure_start = SETUP_SOURCE.index(
            "Function SecureExistingInstallRecoveryTree"
        )
        secure_end = SETUP_SOURCE.index("FunctionEnd", secure_start)
        secure_body = SETUP_SOURCE[secure_start:secure_end]
        self.assertIn('ReadRegDWORD $0 HKLM ${INSTALLER_APP_RECOVERY_REGPATH} "ProductRootCreated"', secure_body)
        self.assertIn('${If} $0 == 0', secure_body)
        self.assertIn('${ElseIf} $InstallRecoveryPathExists == "0"', secure_body)
        self.assertIn("without a verified ownership marker", secure_body)
        self.assertIn("Call HardenInstallRecoveryDirectory", secure_body)

        discard_start = SETUP_SOURCE.index("Function DiscardInstallRecoverySnapshot")
        discard_end = SETUP_SOURCE.index("FunctionEnd", discard_start)
        discard_body = SETUP_SOURCE[discard_start:discard_end]
        recursive_delete = discard_body.index(
            'RMDir /r "$InstallRollbackDirectory"'
        )
        self.assertLess(
            discard_body.index("Call SecureExistingInstallRecoveryTree"),
            recursive_delete,
        )
        self.assertLess(
            discard_body.rindex("Call ValidateInstallRecoveryComponents", 0, recursive_delete),
            recursive_delete,
        )
        self.assertEqual(
            SETUP_SOURCE.count('RMDir /r "$InstallRollbackDirectory"'), 1
        )

        active_start = SETUP_SOURCE.index(
            "Function ValidateActiveInstallRecoverySnapshot"
        )
        active_end = SETUP_SOURCE.index("FunctionEnd", active_start)
        active_body = SETUP_SOURCE[active_start:active_end]
        self.assertIn('"ProductRootCreated"', active_body)
        for required_path in component_paths:
            with self.subTest(required_active_path=required_path):
                path_index = active_body.index(
                    f'StrCpy $InstallRecoveryPathToCheck "{required_path}"'
                )
                required_index = active_body.index(
                    'StrCpy $InstallRecoveryPathRequired "1"', path_index
                )
                self.assertLess(path_index, required_index)
        self.assertIn(
            'GetFileAttributesW(w "$InstallRecoveryMarkerPath")', active_body
        )
        self.assertIn("FILE_ATTRIBUTE_REPARSE_POINT", active_body)
        self.assertIn("EqualizerAPO installer app-tree recovery v1", active_body)

    def test_recovery_path_overlap_and_manifest_containment_are_canonical(self) -> None:
        resolver_start = SETUP_SOURCE.index(
            "Function ResolveInstallRecoveryPhysicalPath"
        )
        resolver_end = SETUP_SOURCE.index("FunctionEnd", resolver_start)
        resolver_body = SETUP_SOURCE[resolver_start:resolver_end]
        self.assertIn("kernel32::CreateFileW", resolver_body)
        self.assertIn("FILE_FLAG_BACKUP_SEMANTICS", resolver_body)
        self.assertIn("FILE_SHARE_READ_WRITE_DELETE", resolver_body)
        self.assertIn("kernel32::GetFinalPathNameByHandleW", resolver_body)
        self.assertIn("kernel32::CloseHandle", resolver_body)
        self.assertIn('\\\\?\\UNC\\', resolver_body)
        self.assertIn('\\\\?\\', resolver_body)

        target_start = SETUP_SOURCE.index("Function ValidateInstallRecoveryTarget")
        target_end = SETUP_SOURCE.index("FunctionEnd", target_start)
        target_body = SETUP_SOURCE[target_start:target_end]
        self.assertGreaterEqual(target_body.count("GetFullPathName"), 2)
        self.assertIn("normalizeInstallPathTail:", target_body)
        self.assertIn("normalizeCommonAppDataTail:", target_body)
        self.assertIn(
            'GetFullPathName $2 "$InstallRecoveryCommonAppData"', target_body
        )
        self.assertNotIn(
            'GetFullPathName $2 "$InstallRollbackDirectory"', target_body
        )
        common_data = target_body.index(
            'GetFullPathName $2 "$InstallRecoveryCommonAppData"'
        )
        product_root = target_body.index(
            'StrCpy $InstallRecoveryProductRoot '
            '"$InstallRecoveryCommonAppData\\EqualizerAPO"',
            common_data,
        )
        recovery_directory = target_body.index(
            'StrCpy $InstallRollbackDirectory '
            '"$InstallRecoveryProductRoot\\InstallerRecovery"',
            product_root,
        )
        self.assertLess(common_data, product_root)
        self.assertLess(product_root, recovery_directory)
        self.assertIn('StrCpy $2 "$INSTDIR\\"', target_body)
        self.assertIn('StrCpy $3 "$InstallRollbackDirectory\\"', target_body)
        self.assertIn('StrCpy $5 "$3" $4', target_body)
        self.assertIn('StrCpy $5 "$2" $4', target_body)
        self.assertGreaterEqual(
            target_body.count("Call ResolveInstallRecoveryPhysicalPath"), 2
        )
        self.assertIn("$InstallRecoveryPhysicalInstallPath", target_body)
        self.assertIn(
            "$InstallRecoveryPhysicalPath\\EqualizerAPO\\InstallerRecovery\\",
            target_body,
        )
        self.assertIn('"PhysicalInstallPath"', target_body)

        metadata_start = SETUP_SOURCE.index("Function SaveInstallMetadataJournal")
        metadata_end = SETUP_SOURCE.index("FunctionEnd", metadata_start)
        metadata_body = SETUP_SOURCE[metadata_start:metadata_end]
        self.assertIn(
            '"PhysicalInstallPath" "$InstallRecoveryPhysicalInstallPath"',
            metadata_body,
        )

        load_start = SETUP_SOURCE.index(
            "Function LoadInstallRecoveryTargetFromJournal"
        )
        load_end = SETUP_SOURCE.index("FunctionEnd", load_start)
        load_body = SETUP_SOURCE[load_start:load_end]
        self.assertIn('ReadRegStr $0 HKLM', load_body)
        self.assertIn('"PhysicalInstallPath"', load_body)
        self.assertIn('${OrIf} $0 == ""', load_body)
        self.assertIn(
            '${OrIf} $0 != "$InstallRecoveryPhysicalInstallPath"', load_body
        )

        def overlaps(left: str, right: str) -> bool:
            left_delimited = left.rstrip("\\").casefold() + "\\"
            right_delimited = right.rstrip("\\").casefold() + "\\"
            return left_delimited.startswith(right_delimited) or right_delimited.startswith(
                left_delimited
            )

        recovery = r"C:\ProgramData\EqualizerAPO\InstallerRecovery"
        self.assertTrue(overlaps(recovery, recovery))
        self.assertTrue(overlaps(r"C:\ProgramData\EqualizerAPO", recovery))
        self.assertTrue(overlaps(recovery + r"\files", recovery))
        self.assertFalse(
            overlaps(r"C:\ProgramData\EqualizerAPO\InstallerRecovery2", recovery)
        )

        cleanup_start = SETUP_SOURCE.index("Function DiscardRenamedProductFiles")
        cleanup_end = SETUP_SOURCE.index("FunctionEnd", cleanup_start)
        cleanup_body = SETUP_SOURCE[cleanup_start:cleanup_end]
        parent = cleanup_body.index('${GetParent} "$RenameIdentityPath" $4')
        filename = cleanup_body.index('${GetFileName} "$RenameIdentityPath" $9', parent)
        canonical_entry = cleanup_body.index('GetFullPathName $4 "$4"', filename)
        containment = cleanup_body.index(
            '${If} $8 == "$RenameCleanupInstallPrefix"', canonical_entry
        )
        deletion = cleanup_body.index("Call DeleteRenameFileByIdentity", containment)
        self.assertLess(parent, filename)
        self.assertLess(filename, canonical_entry)
        self.assertLess(canonical_entry, containment)
        self.assertLess(containment, deletion)

    def test_updater_task_is_snapshotted_and_restored_from_exact_xml(self) -> None:
        self.assertIn(
            'StrCpy $InstallRecoveryTaskXmlPath '
            '"$InstallRollbackDirectory\\update-task.xml"',
            SETUP_SOURCE,
        )
        prepare_start = SETUP_SOURCE.index("Function PrepareInstallTransaction")
        prepare_end = SETUP_SOURCE.index("FunctionEnd", prepare_start)
        prepare_body = SETUP_SOURCE[prepare_start:prepare_end]
        task_query = prepare_body.index(
            'schtasks.exe" /Query /TN "EqualizerAPOUpdateChecker" /FO LIST'
        )
        xml_export = prepare_body.index(
            r'/Query /TN $\"EqualizerAPOUpdateChecker$\" /XML > '
            r'$\"$InstallRecoveryTaskXmlPath$\"',
            task_query,
        )
        xml_saved = prepare_body.index('"PreviousUpdateTaskXmlSaved" 1', xml_export)
        self.assertLess(task_query, xml_export)
        self.assertLess(xml_export, xml_saved)
        self.assertIn("FILE_ATTRIBUTE_REPARSE_POINT", prepare_body[xml_export:xml_saved])
        self.assertIn('FileOpen $0 "$InstallRecoveryTaskXmlPath" r', prepare_body)
        self.assertIn('${If} $1 == ""', prepare_body)
        self.assertIn('${ElseIf} $InstallOperationCode == 1', prepare_body)
        self.assertIn(
            '$WINDIR\\System32\\Tasks\\EqualizerAPOUpdateChecker', prepare_body
        )
        self.assertIn('"PreviousUpdateTaskPresent" 0', prepare_body)

        rollback_start = SETUP_SOURCE.index("Function RollbackInstallTransaction")
        rollback_end = SETUP_SOURCE.index("FunctionEnd", rollback_start)
        rollback_body = SETUP_SOURCE[rollback_start:rollback_end]
        self.assertIn(
            'schtasks.exe" /Create /TN "EqualizerAPOUpdateChecker" '
            '/XML "$InstallRecoveryTaskXmlPath" /F',
            rollback_body,
        )
        self.assertIn(
            'schtasks.exe" /Delete /TN "EqualizerAPOUpdateChecker" /F',
            rollback_body,
        )
        self.assertIn('"PreviousUpdateTaskXmlSaved"', rollback_body)
        self.assertIn("FILE_ATTRIBUTE_REPARSE_POINT", rollback_body)
        self.assertIn(
            '$WINDIR\\System32\\Tasks\\EqualizerAPOUpdateChecker', rollback_body
        )
        self.assertIn('${If} $InstallOperationCode == "error"', rollback_body)
        self.assertIn('${ElseIf} $InstallOperationCode != 0', rollback_body)
        self.assertNotIn("UpdateChecker.exe", rollback_body)
        self.assertLess(
            rollback_body.index('/Create /TN "EqualizerAPOUpdateChecker"'),
            rollback_body.index('"UpdaterOperationStarted" 0'),
        )

    def test_interactive_endpoint_selection_is_post_commit_only(self) -> None:
        install_start = SETUP_SOURCE.index('Section "-Install"')
        install_end = SETUP_SOURCE.index("SectionEnd", install_start)
        install_body = SETUP_SOURCE[install_start:install_end]
        silent_restart = install_body.index('DeviceSelector.exe" /r /s')
        commit = install_body.index("Call CommitInstallTransaction", silent_restart)
        interactive = install_body.index('DeviceSelector.exe" /i', commit)
        complete = install_body.index("Goto installTransactionComplete", interactive)
        self.assertLess(silent_restart, commit)
        self.assertLess(commit, interactive)
        self.assertLess(interactive, complete)
        self.assertNotIn('DeviceSelector.exe" /i', install_body[:commit])
        post_commit_action = install_body[interactive:complete]
        self.assertNotIn("Call RollbackInstallTransaction", post_commit_action)
        self.assertNotIn("Goto installTransactionFailed", post_commit_action)
        self.assertIn("No committed files were rolled back", post_commit_action)
        self.assertNotIn("SetRebootFlag false", SETUP_SOURCE)

    def test_transaction_cleanup_is_restartable_and_journal_last(self) -> None:
        cleanup_start = SETUP_SOURCE.index(
            "Function CleanupCompletedInstallTransaction"
        )
        cleanup_end = SETUP_SOURCE.index("FunctionEnd", cleanup_start)
        cleanup_body = SETUP_SOURCE[cleanup_start:cleanup_end]
        target = cleanup_body.index("Call LoadInstallRecoveryTargetFromJournal")
        migration = cleanup_body.index("Call PrepareRenameManifestForCleanup", target)
        renamed = cleanup_body.index("Call DiscardRenamedProductFiles", migration)
        snapshot = cleanup_body.index("Call DiscardInstallRecoverySnapshot", renamed)
        journal = cleanup_body.index("Call ClearInstallRecoveryJournal", snapshot)
        self.assertLess(target, migration)
        self.assertLess(migration, renamed)
        self.assertLess(renamed, snapshot)
        self.assertLess(snapshot, journal)

        recover_start = SETUP_SOURCE.index("Function RecoverInstallTransaction")
        recover_end = SETUP_SOURCE.index("FunctionEnd", recover_start)
        recover_body = SETUP_SOURCE[recover_start:recover_end]
        no_pending_start = recover_body.index(
            "; A fixed name is not proof of ownership."
        )
        no_pending_end = recover_body.index(
            'ReadRegStr $InstallRecoveryPhase', no_pending_start
        )
        no_pending_body = recover_body[no_pending_start:no_pending_end]
        self.assertIn("unjournaled installer recovery tree", no_pending_body)
        self.assertNotIn("Call DiscardInstallRecoverySnapshot", no_pending_body)
        self.assertIn(
            '${ElseIf} $InstallRecoveryPhase == "committed"', recover_body
        )
        self.assertIn(
            '${ElseIf} $InstallRecoveryPhase == "rollback-cleanup"', recover_body
        )

        prepare_start = SETUP_SOURCE.index("Function PrepareInstallTransaction")
        prepare_end = SETUP_SOURCE.index("FunctionEnd", prepare_start)
        prepare_body = SETUP_SOURCE[prepare_start:prepare_end]
        failed_tail = prepare_body[prepare_body.index("installBackupFailed:") :]
        self.assertLess(
            failed_tail.index("Call DiscardInstallRecoverySnapshot"),
            failed_tail.index("Call ClearInstallRecoveryJournal"),
        )

        commit_start = SETUP_SOURCE.index("Function CommitInstallTransaction")
        commit_end = SETUP_SOURCE.index("FunctionEnd", commit_start)
        commit_body = SETUP_SOURCE[commit_start:commit_end]
        committed = commit_body.index('"Phase" "committed"')
        cleanup = commit_body.index("Call CleanupCompletedInstallTransaction", committed)
        defer_without_rollback = commit_body.index(
            'StrCpy $InstallRecoveryFailed "0"', cleanup
        )
        self.assertLess(committed, cleanup)
        self.assertLess(cleanup, defer_without_rollback)

        rollback_start = SETUP_SOURCE.index("Function RollbackInstallTransaction")
        rollback_end = SETUP_SOURCE.index("FunctionEnd", rollback_start)
        rollback_body = SETUP_SOURCE[rollback_start:rollback_end]
        durable_phase = rollback_body.index(
            'ReadRegStr $InstallRecoveryPhase HKLM '
            '${INSTALLER_APP_RECOVERY_REGPATH} "Phase"'
        )
        committed_guard = rollback_body.index(
            '$InstallRecoveryPhase == "committed"', durable_phase
        )
        active_guard = rollback_body.index(
            '$InstallRecoveryPhase != "active"', committed_guard
        )
        active_snapshot = rollback_body.index(
            "Call ValidateActiveInstallRecoverySnapshot", active_guard
        )
        first_mutation = rollback_body.index(
            'ReadRegDWORD $0 HKLM ${INSTALLER_APP_RECOVERY_REGPATH} '
            '"UpdaterOperationStarted"',
            active_snapshot,
        )
        self.assertLess(durable_phase, committed_guard)
        self.assertLess(committed_guard, active_guard)
        self.assertLess(active_guard, active_snapshot)
        self.assertLess(active_snapshot, first_mutation)
        committed_slice = rollback_body[committed_guard:active_guard]
        self.assertIn("Call CleanupCompletedInstallTransaction", committed_slice)
        self.assertIn('StrCpy $InstallRollbackState "0"', committed_slice)
        self.assertNotIn("UpdaterOperationStarted", committed_slice)

    def test_rollback_requires_durable_active_phase_and_complete_snapshot(
        self,
    ) -> None:
        strict_start = SETUP_SOURCE.index(
            "Function ValidateActiveInstallRecoverySnapshot"
        )
        strict_end = SETUP_SOURCE.index("FunctionEnd", strict_start)
        strict_body = SETUP_SOURCE[strict_start:strict_end]
        for required_path in (
            "$InstallRecoveryProductRoot",
            "$InstallRollbackDirectory",
            "$InstallRollbackFiles",
        ):
            path = strict_body.index(
                f'StrCpy $InstallRecoveryPathToCheck "{required_path}"'
            )
            required = strict_body.index(
                'StrCpy $InstallRecoveryPathRequired "1"', path
            )
            validation = strict_body.index(
                "Call ValidateInstallRecoveryComponent", required
            )
            self.assertLess(path, required)
            self.assertLess(required, validation)
        self.assertIn(
            'GetFileAttributesW(w "$InstallRecoveryMarkerPath")', strict_body
        )
        self.assertIn("FILE_ATTRIBUTE_REPARSE_POINT", strict_body)
        self.assertIn("EqualizerAPO installer app-tree recovery v1", strict_body)

        rollback_start = SETUP_SOURCE.index("Function RollbackInstallTransaction")
        rollback_end = SETUP_SOURCE.index("FunctionEnd", rollback_start)
        rollback_body = SETUP_SOURCE[rollback_start:rollback_end]
        phase = rollback_body.index('"Phase"')
        committed = rollback_body.index('$InstallRecoveryPhase == "committed"', phase)
        active = rollback_body.index('$InstallRecoveryPhase != "active"', committed)
        snapshot = rollback_body.index(
            "Call ValidateActiveInstallRecoverySnapshot", active
        )
        task_mutation = rollback_body.index('"UpdaterOperationStarted"', snapshot)
        self.assertLess(phase, committed)
        self.assertLess(committed, active)
        self.assertLess(active, snapshot)
        self.assertLess(snapshot, task_mutation)
        self.assertNotIn('RMDir "$INSTDIR"', rollback_body)

    def test_uninstaller_avoids_unguarded_recursive_product_tree_deletion(
        self,
    ) -> None:
        recursive_commands = [
            line.strip()
            for line in SETUP_SOURCE.splitlines()
            if line.strip().startswith("RMDir ") and " /r " in line
        ]
        self.assertEqual(
            recursive_commands, ['RMDir /r "$InstallRollbackDirectory"']
        )

        validator_start = SETUP_SOURCE.index(
            "Function un.ValidateProductChildDirectory"
        )
        validator_end = SETUP_SOURCE.index("FunctionEnd", validator_start)
        validator_body = SETUP_SOURCE[validator_start:validator_end]
        self.assertGreaterEqual(validator_body.count("GetFullPathName"), 2)
        self.assertIn('StrCpy $2 "$0\\"', validator_body)
        self.assertIn("kernel32::GetFileAttributesW", validator_body)
        self.assertIn("FILE_ATTRIBUTE_REPARSE_POINT", validator_body)

        config_start = SETUP_SOURCE.index(
            "Function un.RemoveRequestedUserConfiguration"
        )
        config_end = SETUP_SOURCE.index("FunctionEnd", config_start)
        config_body = SETUP_SOURCE[config_start:config_end]
        self.assertIn("Call un.ValidateProductChildDirectory", config_body)
        self.assertIn('Delete "$InstallRecoveryPathToCheck\\*.*"', config_body)
        self.assertIn('RMDir /REBOOTOK "$InstallRecoveryPathToCheck"', config_body)
        self.assertNotIn("/r", config_body)

        qt_start = SETUP_SOURCE.index("Function un.RemoveQtPluginTreeSafely")
        qt_end = SETUP_SOURCE.index("FunctionEnd", qt_start)
        qt_body = SETUP_SOURCE[qt_start:qt_end]
        self.assertGreaterEqual(
            qt_body.count("Call un.ValidateProductChildDirectory"), 8
        )
        self.assertIn(
            '!insertmacro RemoveUninstallPayloadFile '
            '"$INSTDIR\\qt\\platforms\\qwindows.dll"',
            qt_body,
        )
        self.assertIn(
            '!insertmacro RemoveUninstallPayloadDirectory "$INSTDIR\\qt"', qt_body
        )
        self.assertNotIn("RMDir /r", qt_body)

    def test_post_registration_failures_are_checked_and_rolled_back(self) -> None:
        install_start = SETUP_SOURCE.index('Section "-Install"')
        install_end = SETUP_SOURCE.index("SectionEnd", install_start)
        install_body = SETUP_SOURCE[install_start:install_end]
        registration = install_body.index(
            'ExecWait \'"$SYSDIR\\regsvr32.exe" /s '
            '"$INSTDIR\\EqualizerAPO.dll"\' $1'
        )
        post_registration = install_body[registration:]
        self.assertIn('${ElseIf} $InstallOperationCode != 0', post_registration)
        self.assertIn('WriteUninstaller "$INSTDIR\\Uninstall.exe"', post_registration)
        self.assertIn('${If} ${Errors}', post_registration)
        self.assertIn(
            'ExecWait \'"$INSTDIR\\DeviceSelector.exe" /r /s\' '
            "$DeviceSelectorResult",
            post_registration,
        )
        self.assertIn(
            'ExecWait \'"$INSTDIR\\UpdateChecker.exe" -i -s\' '
            "$InstallOperationCode",
            post_registration,
        )
        self.assertIn(
            'WriteRegDWORD HKLM ${INSTALLER_APP_RECOVERY_REGPATH} '
            '"UpdaterOperationStarted" 1',
            post_registration,
        )
        self.assertIn("installTransactionFailed:", post_registration)
        self.assertIn("Call RollbackInstallTransaction", post_registration)

    def test_config_acl_operations_never_walk_the_user_writable_tree(self) -> None:
        install_start = SETUP_SOURCE.index('Section "-Install"')
        install_end = SETUP_SOURCE.index("SectionEnd", install_start)
        install_body = SETUP_SOURCE[install_start:install_end]
        prepare_start = SETUP_SOURCE.index("Function PrepareInstallTransaction")
        prepare_end = SETUP_SOURCE.index("FunctionEnd", prepare_start)
        prepare_body = SETUP_SOURCE[prepare_start:prepare_end]
        rollback_start = SETUP_SOURCE.index("Function RollbackInstallTransaction")
        rollback_end = SETUP_SOURCE.index("FunctionEnd", rollback_start)
        rollback_body = SETUP_SOURCE[rollback_start:rollback_end]

        config_acl_commands = [
            line.strip()
            for line in SETUP_SOURCE.splitlines()
            if "icacls.exe" in line
            and (
                "$INSTDIR\\config" in line
                or "$InstallRecoveryAclPath" in line
            )
        ]
        self.assertEqual(len(config_acl_commands), 4)
        for command in config_acl_commands:
            with self.subTest(command=command):
                self.assertIn(" /L", command)
                self.assertNotIn(" /T", command)
                self.assertNotIn(" /C", command)

        validator_start = SETUP_SOURCE.index("Function ValidateAndHoldConfigRoot")
        validator_end = SETUP_SOURCE.index("FunctionEnd", validator_start)
        validator = SETUP_SOURCE[validator_start:validator_end]
        self.assertIn("FILE_FLAG_OPEN_REPARSE_POINT", validator)
        self.assertIn("FILE_FLAG_BACKUP_SEMANTICS", validator)
        self.assertIn("FILE_SHARE_READ_WRITE", validator)
        self.assertNotIn("FILE_SHARE_READ_WRITE_DELETE", validator)
        self.assertIn("GetFileInformationByHandle", validator)
        self.assertIn("FILE_ATTRIBUTE_DIRECTORY", validator)
        self.assertIn("FILE_ATTRIBUTE_REPARSE_POINT", validator)

        create_root = install_body.index('CreateDirectory "$INSTDIR\\config"')
        validate_root = install_body.index("Call ValidateAndHoldConfigRoot", create_root)
        fresh_only = install_body.index(
            '${If} $ConfigRootCreated == "1"', validate_root
        )
        create_child = install_body.index(
            'CreateDirectory "$INSTDIR\\config\\HeadphoneCalibrations"',
            fresh_only,
        )
        first_config_file = install_body.index(
            "File /oname=config\\config.txt", create_child
        )
        last_config_file = install_body.index(
            "File /oname=config\\selective_delay.txt", first_config_file
        )
        fresh_only_end = install_body.index("${EndIf}", last_config_file)
        grant_root = install_body.index("icacls.exe", fresh_only_end)
        close_root = install_body.index(
            "Call CloseConfigRootIdentityHandle", grant_root
        )
        self.assertLess(create_root, validate_root)
        self.assertLess(validate_root, fresh_only)
        self.assertLess(fresh_only, create_child)
        self.assertLess(create_child, first_config_file)
        self.assertLess(first_config_file, last_config_file)
        self.assertLess(last_config_file, fresh_only_end)
        self.assertLess(fresh_only_end, grant_root)
        self.assertLess(grant_root, close_root)
        self.assertNotIn(
            "CreateDirectory \"$INSTDIR\\config\\",
            install_body[grant_root:close_root],
        )
        self.assertNotIn("File /oname=config\\", install_body[grant_root:close_root])
        verify_start = SETUP_SOURCE.index("Function VerifyRequiredAssets")
        verify_end = SETUP_SOURCE.index("FunctionEnd", verify_start)
        verify_body = SETUP_SOURCE[verify_start:verify_end]
        self.assertIn('${If} $ConfigRootCreated == "1"', verify_body)
        self.assertIn(
            '!insertmacro RequireInstalledAsset "$INSTDIR\\config\\config.txt"',
            verify_body,
        )
        self.assertIn(" /save ", prepare_body)
        self.assertIn(" /L /Q", prepare_body)
        self.assertIn(" /restore ", rollback_body)
        self.assertIn(" /reset /L /Q", rollback_body)

        failed_start = SETUP_SOURCE.index("Function .onInstFailed")
        failed_end = SETUP_SOURCE.index("FunctionEnd", failed_start)
        failed_body = SETUP_SOURCE[failed_start:failed_end]
        self.assertLess(
            failed_body.index("Call CloseConfigRootIdentityHandle"),
            failed_body.index("Call RollbackInstallTransaction"),
        )

    @unittest.skipUnless(
        os.name == "nt" and CMD.is_file() and ICACLS.is_file(),
        "Windows cmd.exe and icacls.exe are required for the junction test",
    )
    def test_root_only_config_acl_update_does_not_follow_a_child_junction(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory(prefix="eqapo-config-acl-") as temp_dir:
            root = pathlib.Path(temp_dir)
            config = root / "config"
            outside = root / "outside"
            child_junction = config / "user-junction"
            config.mkdir()
            outside.mkdir()

            junction = subprocess.run(
                [str(CMD), "/D", "/C", "mklink", "/J", str(child_junction), str(outside)],
                capture_output=True,
                text=True,
                encoding="utf-8",
                errors="replace",
                check=False,
            )
            if junction.returncode != 0:
                self.skipTest(
                    "directory junction creation is unavailable: "
                    + junction.stderr.strip()
                )

            def outside_acl() -> str:
                result = subprocess.run(
                    [str(ICACLS), str(outside)],
                    capture_output=True,
                    text=True,
                    encoding="utf-8",
                    errors="replace",
                    timeout=15,
                    check=False,
                )
                self.assertEqual(result.returncode, 0, msg=result.stderr)
                return result.stdout.strip()

            original = outside_acl()
            try:
                for arguments in (
                    [
                        str(config),
                        "/grant",
                        "*S-1-5-32-545:(OI)(CI)F",
                        "/L",
                        "/Q",
                    ],
                    [str(config), "/reset", "/L", "/Q"],
                ):
                    with self.subTest(arguments=arguments):
                        result = subprocess.run(
                            [str(ICACLS), *arguments],
                            capture_output=True,
                            text=True,
                            encoding="utf-8",
                            errors="replace",
                            timeout=15,
                            check=False,
                        )
                        self.assertEqual(result.returncode, 0, msg=result.stderr)
                        self.assertEqual(outside_acl(), original)
            finally:
                if os.path.lexists(child_junction):
                    os.rmdir(child_junction)

    def test_protected_audio_override_has_a_next_run_recovery_journal(self) -> None:
        self.assertIn(
            '!define INSTALLER_RECOVERY_REGPATH '
            '"${REGPATH}\\InstallerRecovery"',
            SETUP_SOURCE,
        )
        init_start = SETUP_SOURCE.index("Function .onInit")
        init_end = SETUP_SOURCE.index("FunctionEnd", init_start)
        init_body = SETUP_SOURCE[init_start:init_end]
        self.assertIn("Call RecoverProtectedAudioSetting", init_body)

        begin_start = SETUP_SOURCE.index("Function BeginProtectedAudioOverride")
        begin_end = SETUP_SOURCE.index("FunctionEnd", begin_start)
        begin_body = SETUP_SOURCE[begin_start:begin_end]
        pending = begin_body.index(
            'WriteRegDWORD HKLM ${INSTALLER_RECOVERY_REGPATH} "Pending" 1'
        )
        override = begin_body.index(
            'WriteRegDWORD HKLM '
            '"Software\\Microsoft\\Windows\\CurrentVersion\\Audio" '
            '"DisableProtectedAudioDG" 1'
        )
        self.assertLess(pending, override)

        recover_start = SETUP_SOURCE.index(
            "Function RecoverProtectedAudioSetting"
        )
        recover_end = SETUP_SOURCE.index("FunctionEnd", recover_start)
        recover_body = SETUP_SOURCE[recover_start:recover_end]
        self.assertIn(
            'ReadRegDWORD $0 HKLM ${INSTALLER_RECOVERY_REGPATH} "Pending"',
            recover_body,
        )
        self.assertIn("Call RestoreProtectedAudioSetting", recover_body)
        restore_start = SETUP_SOURCE.index("Function RestoreProtectedAudioSetting")
        restore_end = SETUP_SOURCE.index("FunctionEnd", restore_start)
        restore_body = SETUP_SOURCE[restore_start:restore_end]
        self.assertIn(
            'DeleteRegValue HKLM '
            '"Software\\Microsoft\\Windows\\CurrentVersion\\Audio" '
            '"DisableProtectedAudioDG"',
            restore_body,
        )
        self.assertIn(
            'StrCpy $ProtectedAudioOverrideActive "1"', restore_body
        )
        self.assertIn(
            'StrCpy $ProtectedAudioOverrideActive "0"', restore_body
        )
        self.assertLess(
            restore_body.index(
                'ReadRegDWORD $0 HKLM ${INSTALLER_RECOVERY_REGPATH} "Pending"'
            ),
            restore_body.index('StrCpy $ProtectedAudioOverrideActive "0"'),
        )

    def test_start_menu_cleanup_preserves_unrelated_shortcuts(self) -> None:
        self.assertIn("!macro DeleteProductShortcuts folder", SETUP_SOURCE)
        self.assertIn(
            '!insertmacro CreateRequiredShortcut '
            '"$SMPROGRAMS\\$StartMenuFolder\\Check for updates.lnk"',
            SETUP_SOURCE,
        )
        self.assertNotIn('RMDir /r "$SMPROGRAMS\\$OldStartMenuFolder"', SETUP_SOURCE)
        self.assertNotIn('RMDir /r "$SMPROGRAMS\\$StartMenuFolder"', SETUP_SOURCE)

    def test_rollback_removes_optional_directx_payloads_before_restore(self) -> None:
        remove_start = SETUP_SOURCE.index("Function RemoveInstalledProductFiles")
        remove_end = SETUP_SOURCE.index("FunctionEnd", remove_start)
        remove_body = SETUP_SOURCE[remove_start:remove_end]

        self.assertIn(
            '!insertmacro DeleteTransactionFile "$INSTDIR\\dxcompiler.dll"',
            remove_body,
        )
        self.assertIn(
            '!insertmacro DeleteTransactionFile "$INSTDIR\\dxil.dll"',
            remove_body,
        )

    def test_every_installed_optional_qt_imageformat_is_removed_by_rollback(
        self,
    ) -> None:
        installed = set(
            re.findall(
                r'File /nonfatal /oname=qt\\imageformats\\([^" ]+)',
                SETUP_SOURCE,
            )
        )
        remove_start = SETUP_SOURCE.index("Function RemoveInstalledProductFiles")
        remove_end = SETUP_SOURCE.index("FunctionEnd", remove_start)
        rollback = set(
            re.findall(
                r'DeleteTransactionFile "\$INSTDIR\\qt\\imageformats\\([^" ]+)',
                SETUP_SOURCE[remove_start:remove_end],
            )
        )

        self.assertEqual(installed, {"qgif.dll", "qjpeg.dll"})
        self.assertTrue(installed <= rollback)

    def test_uninstaller_blocks_all_installer_recovery_journals_before_trust(
        self,
    ) -> None:
        init_start = SETUP_SOURCE.index("Function un.onInit")
        init_end = SETUP_SOURCE.index("FunctionEnd", init_start)
        init = SETUP_SOURCE[init_start:init_end]
        guard_call = init.index("Call un.BlockIfInstallRecoveryJournalExists")
        install_path_read = init.index(
            'ReadRegStr $0 HKLM ${REGPATH} "InstallPath"'
        )
        self.assertLess(guard_call, install_path_read)
        self.assertIn('StrCpy $InstallRecoveryFailed "0"', init[:install_path_read])
        self.assertIn("Abort", init)

        guard_start = SETUP_SOURCE.index(
            "Function un.BlockIfInstallRecoveryJournalExists"
        )
        guard_end = SETUP_SOURCE.index("FunctionEnd", guard_start)
        guard = SETUP_SOURCE[guard_start:guard_end]
        self.assertIn("RegOpenKeyExW", guard)
        self.assertIn("${ERROR_FILE_NOT_FOUND}", guard)
        self.assertIn(
            'ReadRegDWORD $0 HKLM ${INSTALLER_APP_RECOVERY_REGPATH} "Pending"',
            guard,
        )
        self.assertIn(
            'ReadRegStr $InstallRecoveryPhase HKLM '
            '${INSTALLER_APP_RECOVERY_REGPATH} "Phase"',
            guard,
        )
        self.assertIn(
            'ReadRegDWORD $1 HKLM ${INSTALLER_APP_RECOVERY_REGPATH} '
            '"JournalVersion"',
            guard,
        )
        for phase in (
            "initializing",
            "preparing",
            "prepared",
            "active",
            "committed",
            "rollback-cleanup",
        ):
            self.assertIn(f'$InstallRecoveryPhase == "{phase}"', guard)
        self.assertIn('$0 != 1', guard)
        self.assertNotIn("DeleteReg", guard)

    def test_setup_blocks_partial_uninstall_until_the_same_journal_is_retried(
        self,
    ) -> None:
        init_start = SETUP_SOURCE.index("Function .onInit")
        init_end = SETUP_SOURCE.index("FunctionEnd", init_start)
        init = SETUP_SOURCE[init_start:init_end]
        guard_call = init.index("Call BlockIfUninstallRecoveryJournalExists")
        protected_audio_recovery = init.index("Call RecoverProtectedAudioSetting")
        app_tree_recovery = init.index("Call RecoverInstallTransaction")
        self.assertLess(guard_call, protected_audio_recovery)
        self.assertLess(guard_call, app_tree_recovery)
        self.assertIn("Abort", init[guard_call:protected_audio_recovery])
        self.assertIn("complete the existing uninstall", init)

        guard_start = SETUP_SOURCE.index(
            "Function BlockIfUninstallRecoveryJournalExists"
        )
        guard_end = SETUP_SOURCE.index("FunctionEnd", guard_start)
        guard = SETUP_SOURCE[guard_start:guard_end]
        self.assertIn("RegOpenKeyExW", guard)
        for value in (
            "JournalVersion",
            "InstallPath",
            "Pending",
            "Phase",
            "UpdateCheckerDone",
            "DeviceSelectorDone",
            "ApoUnregistered",
        ):
            self.assertIn(f'"{value}"', guard)
        self.assertIn('$UninstallRecoveryPhase == "critical"', guard)
        self.assertIn('$UninstallRecoveryPhase == "payload"', guard)
        self.assertNotIn("DeleteReg", guard)

        section_start = SETUP_SOURCE.index('Section "-un.Uninstall"')
        section_end = SETUP_SOURCE.index("SectionEnd", section_start)
        section = SETUP_SOURCE[section_start:section_end]
        completed_side_effects = section.index("Call un.MarkUninstallPayloadPhase")
        partial_payload = section.index("RemoveUninstallPayloadFile")
        retry_failure = section.index("uninstallPayloadCleanupFailed:")
        journal_clear = section.index("Call un.ClearUninstallTransaction")
        self.assertLess(completed_side_effects, partial_payload)
        self.assertLess(partial_payload, retry_failure)
        self.assertLess(retry_failure, journal_clear)

        prepare_start = SETUP_SOURCE.index("Function un.PrepareUninstallTransaction")
        prepare_end = SETUP_SOURCE.index("FunctionEnd", prepare_start)
        prepare = SETUP_SOURCE[prepare_start:prepare_end]
        self.assertIn('$UninstallRecoveryPhase == "payload"', prepare)
        self.assertIn('$UninstallUpdateCheckerDone != 1', prepare)
        self.assertIn('$UninstallDeviceSelectorDone != 1', prepare)
        self.assertIn('$UninstallApoUnregistered != 1', prepare)

    def test_partial_uninstall_uses_a_durable_idempotent_journal(self) -> None:
        self.assertIn(
            '!define UNINSTALLER_RECOVERY_REGPATH '
            '"Software\\EqualizerAPOUninstallRecovery"',
            SETUP_SOURCE,
        )
        prepare_start = SETUP_SOURCE.index("Function un.PrepareUninstallTransaction")
        prepare_end = SETUP_SOURCE.index("FunctionEnd", prepare_start)
        prepare = SETUP_SOURCE[prepare_start:prepare_end]
        for value in (
            "JournalVersion",
            "InstallPath",
            "Pending",
            "Phase",
            "UpdateCheckerDone",
            "DeviceSelectorDone",
            "ApoUnregistered",
        ):
            self.assertIn(f'"{value}"', prepare)
        self.assertIn("RegOpenKeyExW", prepare)
        self.assertIn('${UNINSTALLER_RECOVERY_JOURNAL_VERSION}', prepare)
        self.assertIn('$UninstallRecoveryPhase == "critical"', prepare)
        self.assertIn('$UninstallRecoveryPhase == "payload"', prepare)
        self.assertIn('$UninstallRecoveryInstallPath != "$INSTDIR"', prepare)

        init_start = SETUP_SOURCE.index("Function un.onInit")
        init_end = SETUP_SOURCE.index("FunctionEnd", init_start)
        init = SETUP_SOURCE[init_start:init_end]
        self.assertIn("Call un.LoadUninstallRecoveryTargetForInit", init)
        fallback_start = SETUP_SOURCE.index(
            "Function un.LoadUninstallRecoveryTargetForInit"
        )
        fallback_end = SETUP_SOURCE.index("FunctionEnd", fallback_start)
        fallback = SETUP_SOURCE[fallback_start:fallback_end]
        self.assertIn('${UNINSTALLER_RECOVERY_JOURNAL_VERSION}', fallback)
        self.assertIn('"Pending"', fallback)
        self.assertIn('$UninstallRecoveryPhase != "critical"', fallback)
        self.assertIn('$UninstallRecoveryPhase != "payload"', fallback)
        self.assertIn('"InstallPath"', fallback)

        # A missing helper is accepted only after a valid journal proves that
        # the corresponding side effect already completed.
        first_write = prepare.index(
            'WriteRegDWORD HKLM ${UNINSTALLER_RECOVERY_REGPATH}'
        )
        for payload in (
            "UpdateChecker.exe",
            "DeviceSelector.exe",
            "EqualizerAPO.dll",
        ):
            self.assertIn(f'${{FileExists}} "$INSTDIR\\{payload}"', prepare[:first_write])

        section_start = SETUP_SOURCE.index('Section "-un.Uninstall"')
        section_end = SETUP_SOURCE.index("SectionEnd", section_start)
        section = SETUP_SOURCE[section_start:section_end]
        prepare_call = section.index("Call un.PrepareUninstallTransaction")
        stop_call = section.index("Call un.CheckInstalledProductProcesses")
        self.assertLess(prepare_call, stop_call)
        for flag, command in (
            ("$UninstallUpdateCheckerDone", 'UpdateChecker.exe" -u'),
            ("$UninstallDeviceSelectorDone", 'DeviceSelector.exe" /u'),
            ("$UninstallApoUnregistered", 'regsvr32.exe" /u /s'),
        ):
            condition = section.index(f'${{If}} {flag} == "0"')
            operation = section.index(command, condition)
            persisted = section.index(
                "Call un.PersistUninstallStep", operation
            )
            self.assertLess(condition, operation)
            self.assertLess(operation, persisted)

        persist_start = SETUP_SOURCE.index("Function un.PersistUninstallStep")
        persist_end = SETUP_SOURCE.index("FunctionEnd", persist_start)
        persist = SETUP_SOURCE[persist_start:persist_end]
        persisted_write = persist.index(
            'WriteRegDWORD HKLM ${UNINSTALLER_RECOVERY_REGPATH}'
        )
        persisted_readback = persist.index(
            'ReadRegDWORD $0 HKLM ${UNINSTALLER_RECOVERY_REGPATH}',
            persisted_write,
        )
        persisted_flush = persist.index(
            "Call un.FlushUninstallTransaction", persisted_readback
        )
        self.assertLess(persisted_write, persisted_readback)
        self.assertLess(persisted_readback, persisted_flush)

        payload_phase = section.index("Call un.MarkUninstallPayloadPhase")
        first_delete = section.index("RemoveUninstallPayloadFile")
        self.assertLess(payload_phase, first_delete)
        mark_start = SETUP_SOURCE.index("Function un.MarkUninstallPayloadPhase")
        mark_end = SETUP_SOURCE.index("FunctionEnd", mark_start)
        mark = SETUP_SOURCE[mark_start:mark_end]
        phase_write = mark.index('"Phase" "payload"')
        phase_readback = mark.index('"Phase"', phase_write + 1)
        phase_flush = mark.index(
            "Call un.FlushUninstallTransaction", phase_readback
        )
        self.assertLess(phase_write, phase_readback)
        self.assertLess(phase_readback, phase_flush)
        failure = section.index("uninstallPayloadCleanupFailed:")
        clear = section.index("Call un.ClearUninstallTransaction")
        self.assertLess(failure, clear)
        self.assertNotIn(
            "${UNINSTALLER_RECOVERY_REGPATH}",
            section[:clear],
        )

    def test_powershell_helpers_are_embedded_and_never_executed_from_pluginsdir(
        self,
    ) -> None:
        self.assertIn('!include "EmbeddedPowerShellHelpers.nsh"', SETUP_SOURCE)
        self.assertNotIn('File "stop-product-processes.ps1"', SETUP_SOURCE)
        self.assertNotIn('$PLUGINSDIR\\stop-product-processes.ps1', SETUP_SOURCE)
        self.assertNotIn('File "x64-load-check.ps1"', SETUP_SOURCE)
        self.assertNotIn('$PLUGINSDIR\\x64-load-check.ps1', SETUP_SOURCE)
        self.assertNotRegex(
            SETUP_SOURCE,
            r'-File\s+"\$PLUGINSDIR\\(?:stop-product-processes|x64-load-check)\.ps1"',
        )

        stop_chunks = embedded_script_chunks("STOP_PRODUCT_PROCESSES")
        x64_chunks = embedded_script_chunks("X64_LOAD_CHECK")
        self.assertTrue(stop_chunks)
        self.assertTrue(x64_chunks)
        self.assertEqual(
            gzip.decompress(base64.b64decode("".join(stop_chunks))),
            STOP_PRODUCT_PROCESSES_HELPER,
        )
        self.assertEqual(
            gzip.decompress(base64.b64decode("".join(x64_chunks))),
            X64_LOAD_CHECK_HELPER,
        )

        stop_decoder = embedded_script_decoder(
            "EQAPO_STOP_CODE", len(stop_chunks)
        )
        stop_decoder += (
            "$p=@{InstallRoot=$env:EQAPO_PROCESS_INSTALL_ROOT};"
            "if([int]$env:EQAPO_PROCESS_PROTECT_INTERACTIVE -eq 1){"
            "$p.ProtectInteractiveApplications=$true};"
            "&([ScriptBlock]::Create($r.ReadToEnd())) @p"
        )
        x64_decoder = embedded_script_decoder(
            "EQAPO_X64_CODE", len(x64_chunks)
        )
        x64_decoder += (
            "&([ScriptBlock]::Create($r.ReadToEnd())) "
            "$env:EQAPO_X64_DLL_PATH $env:EQAPO_X64_LOG_PATH"
        )
        self.assertIn(stop_decoder.replace("$", "$$"), SETUP_SOURCE)
        self.assertIn(x64_decoder.replace("$", "$$"), SETUP_SOURCE)

    @unittest.skipUnless(
        os.name == "nt" and WINDOWS_POWERSHELL.is_file(),
        "Windows PowerShell 5.1 is required for embedded helper runtime tests",
    )
    def test_embedded_x64_load_check_executes_the_exact_source_payload(self) -> None:
        chunks = embedded_script_chunks("X64_LOAD_CHECK")
        self.assertTrue(chunks)
        decoder = embedded_script_decoder("EQAPO_X64_CODE", len(chunks))
        decoder += (
            "&([ScriptBlock]::Create($r.ReadToEnd())) "
            "$env:EQAPO_X64_DLL_PATH $env:EQAPO_X64_LOG_PATH"
        )
        with tempfile.TemporaryDirectory(prefix="eqapo-x64-embedded-") as temp_dir:
            log_path = pathlib.Path(temp_dir) / "diagnostic.log"
            environment = os.environ.copy()
            for index, chunk in enumerate(chunks):
                environment[f"EQAPO_X64_CODE_{index}"] = chunk
            environment["EQAPO_X64_DLL_PATH"] = str(
                pathlib.Path(os.environ.get("WINDIR", "C:/Windows"))
                / "System32"
                / "kernel32.dll"
            )
            environment["EQAPO_X64_LOG_PATH"] = str(log_path)
            result = subprocess.run(
                [
                    str(WINDOWS_POWERSHELL),
                    "-NoProfile",
                    "-NonInteractive",
                    "-ExecutionPolicy",
                    "Bypass",
                    "-Command",
                    decoder,
                ],
                capture_output=True,
                text=True,
                encoding="utf-8",
                errors="replace",
                timeout=15,
                check=False,
                env=environment,
            )
            self.assertEqual(
                result.returncode,
                0,
                msg=f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}",
            )
            self.assertIn("LoadLibrary OK.", log_path.read_text(encoding="utf-8"))

    def test_every_nsexec_result_is_popped_from_the_nsis_stack(self) -> None:
        lines = [line.strip() for line in SETUP_SOURCE.splitlines()]
        for index, line in enumerate(lines):
            if not line.startswith("nsExec::ExecToLog"):
                continue
            with self.subTest(command=line):
                self.assertLess(index + 1, len(lines))
                self.assertTrue(lines[index + 1].startswith("Pop $"))

    def test_process_shutdown_is_bound_to_the_installed_executable_path(self) -> None:
        helper = (ROOT / "Setup" / "stop-product-processes.ps1").read_text(
            encoding="utf-8"
        )

        self.assertNotIn("taskkill.exe", SETUP_SOURCE)
        self.assertNotIn(" /IM ", SETUP_SOURCE)
        self.assertIn("QueryFullProcessImageName", helper)
        self.assertIn("StringComparison.OrdinalIgnoreCase", helper)
        self.assertIn("expectedPath", helper)
        self.assertIn("TerminateProcess(process, 1)", helper)
        self.assertIn("throw new Win32Exception(error)", helper)
        self.assertIn("GetProcessesByName($processName)", helper)
        self.assertNotIn("[Diagnostics.Process]::GetProcesses()", helper)
        self.assertIn("$ProtectInteractiveApplications", helper)
        self.assertIn("$runningInteractiveProcesses", helper)
        self.assertIn("exit 2", helper)
        self.assertIn("The same handle is used", helper)
        self.assertIn("Call StopInstalledProductProcesses", SETUP_SOURCE)
        self.assertIn("Call un.StopInstalledProductProcesses", SETUP_SOURCE)

        prepare_start = SETUP_SOURCE.index("Function PrepareInstallTransaction")
        prepare_end = SETUP_SOURCE.index("FunctionEnd", prepare_start)
        prepare = SETUP_SOURCE[prepare_start:prepare_end]
        install_start = SETUP_SOURCE.index('Section "-Install"')
        install_end = SETUP_SOURCE.index("SectionEnd", install_start)
        install = SETUP_SOURCE[install_start:install_end]
        self.assertIn('ReadRegStr $0 HKLM ${REGPATH} "InstallPath"', prepare)
        self.assertIn('StrCpy $VerifiedPreviousInstall "1"', prepare)
        self.assertIn('${If} $VerifiedPreviousInstall == "1"', prepare)
        self.assertLess(
            prepare.index("Call ValidateInstallRecoveryTarget"),
            prepare.index("Call CloseRunningApplications"),
        )
        self.assertLess(
            prepare.index("Call CloseRunningApplications"),
            prepare.index("Call SaveInstallMetadataJournal"),
        )
        self.assertNotIn("Call CloseRunningApplications", install)

    @unittest.skipUnless(
        os.name == "nt" and WINDOWS_POWERSHELL.is_file() and PING.is_file(),
        "Windows PowerShell 5.1 and ping.exe are required for the process test",
    )
    def test_process_shutdown_runtime_requires_consent_and_preserves_same_name_apps(
        self,
    ) -> None:
        chunks = embedded_script_chunks("STOP_PRODUCT_PROCESSES")
        self.assertTrue(chunks)
        self.assertEqual(
            gzip.decompress(base64.b64decode("".join(chunks))),
            STOP_PRODUCT_PROCESSES_HELPER,
        )
        decoder = embedded_script_decoder("EQAPO_STOP_CODE", len(chunks))
        decoder += (
            "$p=@{InstallRoot=$env:EQAPO_PROCESS_INSTALL_ROOT};"
            "if([int]$env:EQAPO_PROCESS_PROTECT_INTERACTIVE -eq 1){"
            "$p.ProtectInteractiveApplications=$true};"
            "&([ScriptBlock]::Create($r.ReadToEnd())) @p"
        )

        def run_process_helper(
            install_root: pathlib.Path, protect_interactive: bool
        ) -> subprocess.CompletedProcess[str]:
            environment = os.environ.copy()
            environment["EQAPO_PROCESS_INSTALL_ROOT"] = str(install_root)
            environment["EQAPO_PROCESS_PROTECT_INTERACTIVE"] = (
                "1" if protect_interactive else "0"
            )
            for index, chunk in enumerate(chunks):
                environment[f"EQAPO_STOP_CODE_{index}"] = chunk
            return subprocess.run(
                [
                    str(WINDOWS_POWERSHELL),
                    "-NoProfile",
                    "-NonInteractive",
                    "-ExecutionPolicy",
                    "Bypass",
                    "-Command",
                    decoder,
                ],
                capture_output=True,
                text=True,
                encoding="utf-8",
                errors="replace",
                timeout=15,
                check=False,
                env=environment,
            )

        creation_flags = getattr(subprocess, "CREATE_NO_WINDOW", 0)
        with tempfile.TemporaryDirectory(prefix="eqapo-process-stop-") as temp_dir:
            temp = pathlib.Path(temp_dir)
            exact_root = temp / "installed"
            unrelated_root = temp / "unrelated"
            exact_root.mkdir()
            unrelated_root.mkdir()
            exact_executable = exact_root / "Editor.exe"
            outproc_executable = exact_root / "EqApoOutProcHost.exe"
            unrelated_executable = unrelated_root / "Editor.exe"
            shutil.copy2(PING, exact_executable)
            shutil.copy2(PING, outproc_executable)
            shutil.copy2(PING, unrelated_executable)
            exact_process = subprocess.Popen(
                [str(exact_executable), "-t", "127.0.0.1"],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                creationflags=creation_flags,
            )
            unrelated_process = subprocess.Popen(
                [str(unrelated_executable), "-t", "127.0.0.1"],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                creationflags=creation_flags,
            )
            outproc_process = subprocess.Popen(
                [str(outproc_executable), "-t", "127.0.0.1"],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                creationflags=creation_flags,
            )
            try:
                time.sleep(0.3)
                check = run_process_helper(
                    exact_root,
                    protect_interactive=True,
                )
                self.assertEqual(
                    check.returncode,
                    2,
                    msg=f"stdout:\n{check.stdout}\nstderr:\n{check.stderr}",
                )
                self.assertIsNone(exact_process.poll())
                self.assertIsNone(unrelated_process.poll())
                self.assertIsNone(outproc_process.poll())

                stop = run_process_helper(
                    exact_root,
                    protect_interactive=False,
                )
                self.assertEqual(
                    stop.returncode,
                    0,
                    msg=f"stdout:\n{stop.stdout}\nstderr:\n{stop.stderr}",
                )
                exact_process.wait(timeout=5)
                outproc_process.wait(timeout=5)
                self.assertIsNone(unrelated_process.poll())
            finally:
                for process in (exact_process, unrelated_process, outproc_process):
                    if process.poll() is None:
                        process.kill()
                    process.wait(timeout=5)

    def test_uninstaller_stops_before_deleting_files_on_critical_cleanup_failure(
        self,
    ) -> None:
        section_start = SETUP_SOURCE.index('Section "-un.Uninstall"')
        section_end = SETUP_SOURCE.index("SectionEnd", section_start)
        section = SETUP_SOURCE[section_start:section_end]

        stop_offset = section.index("Call un.StopInstalledProductProcesses")
        check_offset = section.index("Call un.CheckInstalledProductProcesses")
        consent_offset = section.index("$(UninstallCloseAppsPrompt)")
        self.assertLess(check_offset, consent_offset)
        self.assertLess(consent_offset, stop_offset)
        self.assertLess(stop_offset, section.index('UpdateChecker.exe" -u'))
        self.assertLess(stop_offset, section.index('DeviceSelector.exe" /u'))
        self.assertIn("close them before silent uninstall", section)

        for command in (
            'UpdateChecker.exe" -u',
            'DeviceSelector.exe" /u',
            'regsvr32.exe" /u /s',
        ):
            with self.subTest(command=command):
                command_offset = section.index(command)
                command_line = section[command_offset:].splitlines()[0]
                self.assertIn("$InstallOperationCode", command_line)

        failure = section.index("uninstallCriticalCleanupFailed:")
        success = section.index("uninstallCriticalCleanupSucceeded:")
        first_file_delete = section.index(
            '!insertmacro RemoveUninstallPayloadFile '
            '"$INSTDIR\\Configuration reference'
        )
        payload_failure_guard = section.index(
            '${If} $InstallRecoveryFailed == "1"', first_file_delete
        )
        uninstall_metadata_delete = section.index(
            "DeleteRegKey HKLM ${UNINST_REGPATH}"
        )
        self.assertIn("${If} ${Errors}", section)
        self.assertGreaterEqual(section.count("$InstallOperationCode != 0"), 3)
        self.assertIn("SetErrorLevel 1", section[failure:success])
        self.assertIn("Abort", section[failure:success])
        self.assertLess(failure, success)
        self.assertLess(success, first_file_delete)
        self.assertLess(first_file_delete, payload_failure_guard)
        self.assertLess(success, uninstall_metadata_delete)

        optional_start = SETUP_SOURCE.index("Section /o un.$(SecRemoveName)")
        optional_end = SETUP_SOURCE.index("SectionEnd", optional_start)
        optional_section = SETUP_SOURCE[optional_start:optional_end]
        user_data_function_start = SETUP_SOURCE.index(
            "Function un.RemoveRequestedUserConfiguration"
        )
        user_data_function_end = SETUP_SOURCE.index(
            "FunctionEnd", user_data_function_start
        )
        user_data_function = SETUP_SOURCE[
            user_data_function_start:user_data_function_end
        ]
        user_data_call = section.index("Call un.RemoveRequestedUserConfiguration")
        self.assertIn(
            'StrCpy $RemoveUserConfigurationRequested "1"', optional_section
        )
        self.assertNotIn('Delete "$INSTDIR\\*.reg"', optional_section)
        self.assertNotIn("DeleteRegKey HKCU ${REGPATH}", optional_section)
        self.assertIn('Delete "$INSTDIR\\*.reg"', user_data_function)
        self.assertIn("DeleteRegKey HKCU ${REGPATH}", user_data_function)
        self.assertLess(success, user_data_call)
        self.assertLess(payload_failure_guard, user_data_call)
        self.assertLess(user_data_call, uninstall_metadata_delete)


if __name__ == "__main__":
    unittest.main()
