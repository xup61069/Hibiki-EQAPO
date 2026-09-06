import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SETUP = (ROOT / "Setup" / "Setup.nsi").read_text(encoding="utf-8")
ASIO_HELPERS = (ROOT / "Setup" / "AsioProxy.nsh").read_text(encoding="utf-8")
ASIO_BINARY_HELPER = (ROOT / "Setup" / "AsioProxyBinaryCheck.nsh").read_text(
    encoding="utf-8"
)
SETUP32 = (ROOT / "Setup" / "Setup32.nsi").read_text(encoding="utf-8")
SETUP64 = (ROOT / "Setup" / "Setup64.nsi").read_text(encoding="utf-8")
SETUP_ARM64 = (ROOT / "Setup" / "SetupARM64.nsi").read_text(encoding="utf-8")
STAGE_X64 = (ROOT / "scripts" / "stage-installer-x64.ps1").read_text(
    encoding="utf-8"
)


def function_body(source: str, name: str) -> str:
    match = re.search(
        rf"^Function {re.escape(name)}\s*$\n(?P<body>.*?)^FunctionEnd\s*$",
        source,
        flags=re.MULTILINE | re.DOTALL,
    )
    if match is None:
        raise AssertionError(f"NSIS function not found: {name}")
    return match.group("body")


def assert_enum_reg_key_eof_contracts(source: str, expected_count: int) -> None:
    significant_lines = [
        (line_number, line.strip())
        for line_number, line in enumerate(source.splitlines(), start=1)
        if line.strip() and not line.lstrip().startswith(";")
    ]
    enumerations = [
        (index, match.group("output"))
        for index, (_, line) in enumerate(significant_lines)
        if (
            match := re.fullmatch(
                r"EnumRegKey\s+(?P<output>\$\w+|\$\d+)\s+HKLM\s+.+",
                line,
            )
        )
    ]
    if len(enumerations) != expected_count:
        raise AssertionError(
            f"expected {expected_count} HKLM EnumRegKey sites, found {len(enumerations)}"
        )

    for index, output in enumerations:
        line_number = significant_lines[index][0]
        if index < 2 or significant_lines[index - 2][1] != f'StrCpy {output} ""':
            raise AssertionError(
                f"EnumRegKey at line {line_number} must initialize its own output "
                f'register {output} with StrCpy {output} ""'
            )
        if significant_lines[index - 1][1] != "ClearErrors":
            raise AssertionError(
                f"EnumRegKey at line {line_number} must immediately follow ClearErrors"
            )

        required_tail = (
            "${If} ${Errors}",
            f'${{OrIf}} {output} == ""',
        )
        actual_tail = tuple(
            line for _, line in significant_lines[index + 1 : index + 3]
        )
        if actual_tail != required_tail:
            raise AssertionError(
                f"EnumRegKey at line {line_number} must terminate on Errors or an "
                f"empty {output} result"
            )

        try:
            _, termination = significant_lines[index + 3]
            _, end_if = significant_lines[index + 4]
        except IndexError as error:
            raise AssertionError(
                f"EnumRegKey at line {line_number} has no complete termination guard"
            ) from error
        if end_if != "${EndIf}":
            raise AssertionError(
                f"EnumRegKey at line {line_number} termination branch is not closed"
            )
        if termination == "Return":
            continue

        goto = re.fullmatch(r"Goto\s+(?P<label>[A-Za-z_]\w*)", termination)
        if goto is None:
            raise AssertionError(
                f"EnumRegKey at line {line_number} termination branch must Goto or Return"
            )
        label = goto.group("label")
        target_indices = [
            target_index
            for target_index, (_, line) in enumerate(significant_lines)
            if line == f"{label}:"
        ]
        if len(target_indices) != 1 or target_indices[0] <= index + 4:
            raise AssertionError(
                f"EnumRegKey at line {line_number} must jump to one forward label "
                "outside its termination guard"
            )
        loop_labels = [
            line[:-1]
            for _, line in significant_lines[:index]
            if re.fullmatch(r"[A-Za-z_]\w*:", line)
        ]
        if not loop_labels:
            raise AssertionError(
                f"EnumRegKey at line {line_number} has no enclosing loop label"
            )
        loop_label = loop_labels[-1]
        back_edges = [
            branch_index
            for branch_index, (_, line) in enumerate(
                significant_lines[index + 5 :], start=index + 5
            )
            if line == f"Goto {loop_label}"
        ]
        if not back_edges or target_indices[0] <= back_edges[0]:
            raise AssertionError(
                f"EnumRegKey at line {line_number} termination target must cross "
                f"the {loop_label} loop back edge"
            )


class AsioInstallerContractTests(unittest.TestCase):
    def test_proxy_has_stable_public_identity_and_is_x64_only(self):
        self.assertIn('!define ASIO_PROXY_LABEL "Hibiki EQAPO"', SETUP)
        self.assertIn(
            '!define ASIO_PROXY_CLSID "{D47C55C9-3F7D-422F-86E9-32E170815D53}"',
            SETUP,
        )
        self.assertIn('!define ASIO_PROXY_DLL "HibikiEQAPODriver.dll"', SETUP)
        self.assertIn('!define ENABLE_ASIO_PROXY 1', SETUP64)
        self.assertNotIn('!define ENABLE_ASIO_PROXY 1', SETUP32)
        self.assertNotIn('!define ENABLE_ASIO_PROXY 1', SETUP_ARM64)

    def test_x64_staging_requires_and_copies_the_proxy_driver(self):
        self.assertIn(
            'Join-Path $root "x64\\$Configuration\\HibikiEQAPODriver.dll"',
            STAGE_X64,
        )
        self.assertIn(
            'Join-Path $libDir "HibikiEQAPODriver.dll"', STAGE_X64
        )
        self.assertIn("Hibiki EQAPO driver", STAGE_X64)
        self.assertIn(
            'Join-Path $libDir "HibikiEQAPODriver.dll"',
            STAGE_X64[STAGE_X64.index("$requiredInstallerAssets = @(") :],
        )

    def test_proxy_payload_is_transactional_and_removed_on_uninstall(self):
        for contract in (
            '!insertmacro RequireInstalledAsset "$INSTDIR\\${ASIO_PROXY_DLL}"',
            '!insertmacro RenameAndDelete "$INSTDIR\\${ASIO_PROXY_DLL}"',
            'File "${LIBPATH}\\${ASIO_PROXY_DLL}"',
            '!insertmacro DeleteTransactionFile "$INSTDIR\\${ASIO_PROXY_DLL}"',
            '!insertmacro RemoveUninstallPayloadFile "$INSTDIR\\${ASIO_PROXY_DLL}"',
        ):
            self.assertIn(contract, SETUP)

    def test_target_selection_is_machine_default_with_no_elevated_hkcu_write(self):
        self.assertIn(
            '!define ASIO_PROXY_TARGET_REGPATH "Software\\EqualizerAPO\\ASIOProxy"',
            SETUP,
        )
        self.assertIn(
            'WriteRegStr HKLM "${ASIO_PROXY_TARGET_REGPATH}" "TargetCLSID"',
            ASIO_HELPERS,
        )
        self.assertNotRegex(
            ASIO_HELPERS,
            r"WriteReg(?:Str|DWORD)\s+HKCU\s+\"?\$\{ASIO_PROXY_TARGET_REGPATH\}",
        )
        self.assertIn("over-the-shoulder UAC", ASIO_HELPERS)

    def test_selection_never_targets_itself_or_guesses_silent_ambiguity(self):
        selection = function_body(ASIO_HELPERS, "ResolveInitialAsioSelection")
        discovery = function_body(ASIO_HELPERS, "LoadEligibleAsioEntry")
        self.assertIn(
            '${OrIf} $AsioEntryClsid == "${ASIO_PROXY_CLSID}"', discovery
        )
        self.assertIn("Call ValidateAsioGuidText", discovery)
        self.assertIn('"/NOASIOPROXY"', selection)
        self.assertIn('"/ASIOCLSID="', selection)
        conflict = selection.index('$AsioNoProxyOption == "1"')
        explicit = selection.index('$AsioCommandLineTargetClsid != ""', conflict)
        failure = selection.index('StrCpy $InstallRecoveryFailed "1"', explicit)
        self.assertLess(conflict, explicit)
        self.assertLess(explicit, failure)
        unique = selection[selection.index("$AsioCandidateCount == 1") :]
        self.assertIn(
            'StrCpy $AsioSelectedTargetClsid "$AsioSingleCandidateClsid"', unique
        )
        self.assertIn('StrCpy $AsioProxyRequested "1"', unique)
        ambiguous = selection[selection.index("$AsioCandidateCount > 1") :]
        self.assertIn("${IfNot} ${Silent}", ambiguous)
        self.assertNotIn('StrCpy $AsioSelectedTargetClsid "$AsioSingleCandidateClsid"', ambiguous)

    def test_x64_registry_view_is_selected_before_driver_discovery(self):
        on_init = function_body(SETUP, ".onInit")
        self.assertLess(on_init.index("SetRegView 64"), on_init.index("ResolveInitialAsioSelection"))

    def test_registry_enumeration_uses_an_empty_name_termination_sentinel(self):
        assert_enum_reg_key_eof_contracts(ASIO_HELPERS, expected_count=5)

    def test_registry_enumeration_contract_rejects_incomplete_guards(self):
        mutations = {
            "wrong initialization register": (
                '  StrCpy $AsioEntryKey ""\n  ClearErrors\n  EnumRegKey $AsioEntryKey',
                '  StrCpy $5 ""\n  ClearErrors\n  EnumRegKey $AsioEntryKey',
                "initialize its own output register",
            ),
            "missing ClearErrors": (
                '  ClearErrors\n  EnumRegKey $AsioEntryKey',
                '  StrCpy $0 "not-clear"\n  EnumRegKey $AsioEntryKey',
                "immediately follow ClearErrors",
            ),
            "wrong empty-name register": (
                '${OrIf} $AsioEntryKey == ""',
                '${OrIf} $5 == ""',
                "terminate on Errors or an empty",
            ),
            "missing Errors guard": (
                '${If} ${Errors}\n  ${OrIf} $AsioEntryKey == ""',
                '${If} $0 == "unexpected"\n  ${OrIf} $AsioEntryKey == ""',
                "terminate on Errors or an empty",
            ),
            "non-exiting guard": (
                "    Goto asioDiscoverDone",
                '    StrCpy $0 "not-an-exit"',
                "termination branch",
            ),
            "backward jump": (
                "    Goto asioDiscoverDone",
                "    Goto asioDiscoverNext",
                "forward label",
            ),
            "non-exiting forward jump": (
                "    Goto asioDiscoverDone",
                "    Goto asioDiscoverAdvance",
                "loop back edge",
            ),
        }
        for name, (original, replacement, expected_error) in mutations.items():
            with self.subTest(name=name):
                self.assertIn(original, ASIO_HELPERS)
                broken = ASIO_HELPERS.replace(original, replacement, 1)
                with self.assertRaisesRegex(AssertionError, expected_error):
                    assert_enum_reg_key_eof_contracts(broken, expected_count=5)

    def test_vendor_registration_is_discovery_only_and_never_rewritten(self):
        mutations = re.findall(
            r"(?m)^\s*(?:WriteRegStr|WriteRegDWORD|DeleteRegValue|DeleteRegKey)[^\n]*",
            ASIO_HELPERS,
        )
        self.assertTrue(mutations)
        for mutation in mutations:
            self.assertNotIn('${ASIO_ENUM_ROOT}\\$AsioEntryKey', mutation)
            self.assertNotRegex(
                mutation,
                r'Software\\Classes\\CLSID\\\$Asio(?:Entry|SelectedTarget)Clsid',
            )

    def test_discovery_parses_amd64_dll_headers_without_loading_vendor_code(self):
        validator = function_body(
            ASIO_BINARY_HELPER, "ValidateAmd64AsioDriverBinary"
        )
        discovery = function_body(ASIO_HELPERS, "LoadEligibleAsioEntry")
        self.assertIn("FileOpen", validator)
        self.assertIn("FileReadByte", validator)
        self.assertIn("${IMAGE_FILE_MACHINE_AMD64}", validator)
        self.assertIn("${IMAGE_FILE_DLL}", validator)
        self.assertNotRegex(validator, r"(?m)^\s*System::Call.*LoadLibrary")
        for register in range(10):
            self.assertIn(f"Push ${register}", validator)
            self.assertIn(f"Pop ${register}", validator)
        self.assertIn('!include "AsioProxyBinaryCheck.nsh"', ASIO_HELPERS)
        self.assertIn("Call ValidateAmd64AsioDriverBinary", discovery)
        self.assertLess(
            discovery.index("PathIsRelativeW"),
            discovery.index("Call ValidateAmd64AsioDriverBinary"),
        )

    def test_apply_publishes_enum_clsid_only_after_complete_registration(self):
        apply = function_body(ASIO_HELPERS, "ApplyAsioProxyRegistration")
        target = apply.index(
            'WriteRegStr HKLM "${ASIO_PROXY_TARGET_REGPATH}" "TargetCLSID"'
        )
        inproc = apply.index(
            'WriteRegStr HKLM "${ASIO_PROXY_COM_REGPATH}\\InprocServer32" ""'
        )
        description = apply.index(
            'WriteRegStr HKLM "${ASIO_PROXY_ENUM_REGPATH}" "Description"'
        )
        publish = apply.index(
            'WriteRegStr HKLM "${ASIO_PROXY_ENUM_REGPATH}" "CLSID"'
        )
        self.assertLess(target, inproc)
        self.assertLess(inproc, description)
        self.assertLess(description, publish)

    def test_install_journal_covers_asio_before_mutation_and_precise_restore(self):
        save = function_body(SETUP, "SaveInstallMetadataJournal")
        apply = function_body(ASIO_HELPERS, "ApplyAsioProxyRegistration")
        restore = function_body(ASIO_HELPERS, "RestoreAsioProxyRegistration")
        for value in (
            "PreviousAsioEnumClsid",
            "PreviousAsioEnumDescription",
            "PreviousAsioComDescription",
            "PreviousAsioInprocServer",
            "PreviousAsioThreadingModel",
            "PreviousAsioTargetClsid",
        ):
            self.assertIn(value, save)
            self.assertIn(value, restore)
        attempted = apply.index('"NewAsioRegistrationAttempted" 1')
        first_registry_mutation = apply.index(
            'WriteRegStr HKLM "${ASIO_PROXY_TARGET_REGPATH}"'
        )
        self.assertLess(attempted, first_registry_mutation)
        self.assertIn("Call FlushRenameCleanupJournal", apply[:first_registry_mutation])

    def test_rollback_unpublishes_before_files_then_restores_after_files(self):
        rollback = function_body(SETUP, "RollbackInstallTransaction")
        hide = rollback.index("Call HideAsioProxyRegistrationForRollback")
        remove_files = rollback.index("Call RemoveInstalledProductFiles")
        restore_files = rollback.index('nsExec::ExecToLog \'"$SYSDIR\\robocopy.exe"')
        restore_registry = rollback.index("Call RestoreAsioProxyRegistration")
        self.assertLess(hide, remove_files)
        self.assertLess(remove_files, restore_files)
        self.assertLess(restore_files, restore_registry)

    def test_uninstall_is_idempotent_owned_and_driver_lock_safe(self):
        prepare = function_body(SETUP, "un.PrepareUninstallTransaction")
        unregister = function_body(ASIO_HELPERS, "un.UnregisterAsioProxy")
        self.assertIn("Call un.CheckAsioProxyDllUnlocked", prepare)
        self.assertIn("close every DAW", prepare)
        for owned_value in (
            'ReadRegStr $0 HKLM "${ASIO_PROXY_ENUM_REGPATH}" "CLSID"',
            '$0 != "${ASIO_PROXY_CLSID}"',
            'ReadRegStr $0 HKLM "${ASIO_PROXY_ENUM_REGPATH}" "Description"',
            '$0 != "${ASIO_PROXY_LABEL}"',
            'ReadRegStr $0 HKLM "${ASIO_PROXY_COM_REGPATH}\\InprocServer32" "ThreadingModel"',
            '$0 != "Apartment"',
            'GetFullPathName $1 "$INSTDIR\\${ASIO_PROXY_DLL}"',
        ):
            self.assertIn(owned_value, unregister)
        first_delete = unregister.index("DeleteRegValue")
        self.assertLess(unregister.index('$0 != "${ASIO_PROXY_CLSID}"'), first_delete)
        self.assertLess(unregister.index('$0 != "${ASIO_PROXY_LABEL}"'), first_delete)
        self.assertLess(unregister.index('$0 != "Apartment"'), first_delete)
        self.assertLess(
            unregister.index('GetFullPathName $1 "$INSTDIR\\${ASIO_PROXY_DLL}"'),
            first_delete,
        )
        self.assertIn(
            'DeleteRegValue HKLM "${ASIO_PROXY_TARGET_REGPATH}" "TargetCLSID"',
            unregister,
        )
        unregister_call = SETUP.index("Call un.UnregisterAsioProxy")
        payload_phase = SETUP.index("Call un.MarkUninstallPayloadPhase", unregister_call)
        driver_delete = SETUP.index(
            '!insertmacro RemoveUninstallPayloadFile "$INSTDIR\\${ASIO_PROXY_DLL}"',
            payload_phase,
        )
        self.assertLess(unregister_call, payload_phase)
        self.assertLess(payload_phase, driver_delete)

    def test_install_refuses_foreign_or_orphaned_fixed_registration(self):
        validate = function_body(
            ASIO_HELPERS, "ValidateExistingAsioProxyRegistration"
        )
        collision = validate.index("asioExistingCollision:")
        self.assertIn('${If} $2 != $3', validate[:collision])
        self.assertIn('$0 != "${ASIO_PROXY_CLSID}"', validate[:collision])
        self.assertIn('$0 != "${ASIO_PROXY_LABEL}"', validate[:collision])
        self.assertIn('$0 != "Apartment"', validate[:collision])
        self.assertIn('StrCpy $InstallRecoveryFailed "1"', validate[collision:])

    def test_non_reg_sz_values_are_rejected_before_snapshot_or_removal(self):
        install_types = function_body(
            ASIO_HELPERS, "ValidateAsioSnapshotRegistryTypes"
        )
        uninstall_types = function_body(
            ASIO_HELPERS, "un.ValidateAsioRegistryTypesForRemoval"
        )
        for body in (install_types, uninstall_types):
            self.assertIn("RegQueryValueExW", body)
            self.assertIn("${REG_SZ}", body)
            self.assertIn('"TargetCLSID"', body)
            self.assertIn('"ThreadingModel"', body)
        install_validate = function_body(
            ASIO_HELPERS, "ValidateExistingAsioProxyRegistration"
        )
        self.assertIn("Call ValidateAsioSnapshotRegistryTypes", install_validate)
        prepare = function_body(SETUP, "PrepareInstallTransaction")
        self.assertLess(
            prepare.index("Call ValidateExistingAsioProxyRegistration"),
            prepare.index("Call SaveInstallMetadataJournal"),
        )
        unregister = function_body(ASIO_HELPERS, "un.UnregisterAsioProxy")
        self.assertLess(
            unregister.index("Call un.ValidateAsioRegistryTypesForRemoval"),
            unregister.index("DeleteRegValue"),
        )


if __name__ == "__main__":
    unittest.main()
