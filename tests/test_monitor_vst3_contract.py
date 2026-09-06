#!/usr/bin/env python3
"""Static design contracts for the monitor-only VST3 implementation slice."""

from __future__ import annotations

import pathlib
import re
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
DECISION_PATH = ROOT / "docs" / "decisions" / "0006-monitor-vst3.md"
DECISION = DECISION_PATH.read_text(encoding="utf-8")
PLUGIN_ROOT = ROOT / "MonitorVST3"
SOLUTION = (ROOT / "HibikiEQAPO.sln").read_text(encoding="utf-8-sig")
LOCAL_BUILD = (ROOT / "build-local-x64.ps1").read_text(encoding="utf-8")
INSTALLER_STAGE = (ROOT / "scripts" / "stage-installer-x64.ps1").read_text(
    encoding="utf-8"
)
SAFE_GENERATED_TREE = (ROOT / "scripts" / "SafeGeneratedTree.psm1").read_text(
    encoding="utf-8-sig"
)
BINARY_AUDIT_PATH = ROOT / "scripts" / "test-monitor-vst3-binary.ps1"
BINARY_AUDIT = BINARY_AUDIT_PATH.read_text(encoding="utf-8")
BINARY_AUDIT_MODULE_PATH = ROOT / "scripts" / "MonitorVST3Binary.psm1"
BINARY_AUDIT_MODULE = BINARY_AUDIT_MODULE_PATH.read_text(encoding="utf-8-sig")
MONITOR_MANAGER = (ROOT / "scripts" / "manage-monitor-vst3.ps1").read_text(
    encoding="utf-8-sig"
)
VALIDATOR_GATE_PATH = ROOT / "scripts" / "test-monitor-vst3-validator.ps1"
VALIDATOR_GATE = VALIDATOR_GATE_PATH.read_text(encoding="utf-8")
INSTALLER = (ROOT / "Setup" / "Setup.nsi").read_text(encoding="utf-8")
RELEASE_WORKFLOW = (ROOT / ".github" / "workflows" / "release.yml").read_text(
    encoding="utf-8"
)
BUILD_WORKFLOW = (ROOT / ".github" / "workflows" / "build.yml").read_text(
    encoding="utf-8"
)

EXPECTED_IMPLEMENTATION_PATHS = (
    "MonitorVST3/MonitorVST3.vcxproj",
    "MonitorVST3/MonitorVST3Factory.cpp",
    "MonitorVST3/MonitorVST3Identity.h",
    "MonitorVST3/MonitorVST3Processor.h",
    "MonitorVST3/MonitorVST3Processor.cpp",
    "MonitorVST3/MonitorVST3Controller.h",
    "MonitorVST3/MonitorVST3Controller.cpp",
    "MonitorVST3/MonitorVST3State.h",
    "MonitorVST3/README.md",
)

EXPORT_SAFETY_WARNING = (
    "Export safety limit: the plug-in cannot identify every real-time export "
    "and cannot prevent a user from placing it on a renderable master bus."
)


def method_body(source: str, marker: str) -> str:
    """Return one C++ method body, including its outer braces."""
    start = source.find(marker)
    if start < 0:
        raise AssertionError(f"Missing C++ method marker: {marker}")
    opening = source.find("{", start)
    if opening < 0:
        raise AssertionError(f"Missing opening brace after: {marker}")

    depth = 0
    for index in range(opening, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[opening : index + 1]
    raise AssertionError(f"Unterminated C++ method after: {marker}")


class MonitorVST3ContractTests(unittest.TestCase):
    def read_required(self, relative_path: str) -> str:
        path = ROOT / relative_path
        self.assertTrue(
            path.is_file(),
            f"Missing monitor VST3 implementation file: {relative_path}",
        )
        return path.read_text(encoding="utf-8-sig")

    def test_decision_pins_export_boundary_identity_and_bundle_location(self) -> None:
        for token in (
            "ProcessModes::kOffline",
            "Vst::PlugType::kFx",
            "ParameterInfo::kIsBypass",
            "kSample32",
            "kSample64",
            'kMonitorDeviceGuid = L""',
            EXPORT_SAFETY_WARNING,
            "%CommonProgramFiles%\\VST3\\HibikiEQAPO\\"
            "HibikiEQAPOMonitor.vst3\\Contents\\x86_64-win\\"
            "HibikiEQAPOMonitor.vst3",
        ):
            with self.subTest(token=token):
                self.assertIn(token, DECISION)

        self.assertIn("不得宣稱外掛會自動避免誤放 master", DECISION)
        self.assertIn("realtime export 仍可能把校正燒入", DECISION)

    def test_expected_implementation_files_exist(self) -> None:
        missing = [
            relative_path
            for relative_path in EXPECTED_IMPLEMENTATION_PATHS
            if not (ROOT / relative_path).is_file()
        ]
        self.assertFalse(
            missing,
            "Missing monitor VST3 implementation slice:\n- " + "\n- ".join(missing),
        )

    def test_factory_registers_a_local_fx_and_controller(self) -> None:
        factory = self.read_required("MonitorVST3/MonitorVST3Factory.cpp")
        identity = self.read_required("MonitorVST3/MonitorVST3Identity.h")

        for token in (
            "BEGIN_FACTORY_DEF",
            "DEF_CLASS2",
            "END_FACTORY",
            "kVstAudioEffectClass",
            "Vst::PlugType::kFx",
            "kVstComponentControllerClass",
            "kMonitorProcessorUid",
            "kMonitorControllerUid",
        ):
            with self.subTest(token=token):
                self.assertIn(token, factory)
        self.assertGreaterEqual(factory.count("DEF_CLASS2"), 2)
        self.assertNotIn(
            "Vst::kDistributable",
            factory,
            "The processor reads local Hibiki EQAPO configuration and cannot run remotely",
        )

        for token in (
            "0x7C8A3D91",
            "0x0E6F4B27",
            "0xB1D8A45C",
            "0x92F36710",
            "0x3F2B86E4",
            "0xC95047AD",
            "0xA67E19D2",
            "0x548CB301",
        ):
            with self.subTest(uid_word=token):
                self.assertIn(token, identity)

    def test_bypass_is_a_stable_host_parameter_and_has_a_dry_path(self) -> None:
        identity = self.read_required("MonitorVST3/MonitorVST3Identity.h")
        controller = self.read_required("MonitorVST3/MonitorVST3Controller.cpp")
        processor = self.read_required("MonitorVST3/MonitorVST3Processor.cpp")

        self.assertRegex(
            identity,
            r"kMonitorBypassParamId\s*=\s*0x00010000u?",
        )
        self.assertIn("kMonitorBypassParamId", controller)
        self.assertIn("ParameterInfo::kIsBypass", controller)
        self.assertIn("Bypass", controller)
        self.assertIn("kMonitorBypassParamId", processor)
        self.assertIn("inputParameterChanges", processor)
        self.assertIn("copyDry", processor)

    def test_processor_supports_float32_float64_and_offline_dry_bypass(self) -> None:
        header = self.read_required("MonitorVST3/MonitorVST3Processor.h")
        processor = self.read_required("MonitorVST3/MonitorVST3Processor.cpp")

        self.assertIn("canProcessSampleSize", header + processor)
        for token in (
            "kSample32",
            "kSample64",
            "channelBuffers32",
            "channelBuffers64",
            "ProcessModes::kOffline",
        ):
            with self.subTest(token=token):
                self.assertIn(token, processor)
        self.assertGreaterEqual(processor.count("engine->process("), 2)

        process = method_body(processor, "MonitorVST3Processor::process(")
        self.assertIn("offlineMode", process)
        self.assertIn("copyDry", process)
        self.assertIn("engine->process(", process)
        self.assertIn("data.processMode == Steinberg::Vst::ProcessModes::kOffline", process)

    def test_filter_engine_uses_the_non_endpoint_monitor_identity(self) -> None:
        identity = self.read_required("MonitorVST3/MonitorVST3Identity.h")
        processor = self.read_required("MonitorVST3/MonitorVST3Processor.cpp")

        for token in (
            'L"Hibiki EQAPO Monitor VST3"',
            'L"DAW Monitor Insert"',
            'kMonitorDeviceGuid[] = L""',
            'L"DAW Monitor Insert Hibiki EQAPO Monitor VST3"',
        ):
            with self.subTest(identity=token):
                self.assertIn(token, identity)

        set_device = processor.find("newEngine->setDeviceInfo(")
        initialize = processor.find("newEngine->initialize(")
        self.assertGreaterEqual(set_device, 0)
        self.assertGreater(initialize, set_device)
        for token in (
            "false",
            "true",
            "kMonitorDeviceName",
            "kMonitorConnectionName",
            "kMonitorDeviceGuid",
            "kMonitorDeviceString",
        ):
            self.assertIn(token, processor[set_device:initialize])

    def test_missing_or_unreadable_configuration_cannot_report_ready(self) -> None:
        engine_header = self.read_required("FilterEngine.h")
        processor = self.read_required("MonitorVST3/MonitorVST3Processor.cpp")

        self.assertIn("bool hasActiveConfiguration() const noexcept", engine_header)
        self.assertIn("currentConfig.load(std::memory_order_acquire)", engine_header)
        setup = method_body(processor, "MonitorVST3Processor::setupProcessing(")
        self.assertIn("newEngine->hasActiveConfiguration()", setup)
        self.assertLess(
            setup.index("newEngine->hasActiveConfiguration()"),
            setup.index("engine = std::move(newEngine)"),
        )

        process = method_body(processor, "MonitorVST3Processor::process(")
        missing_engine = process.index(
            "!engineReady.load(std::memory_order_acquire) || engine == nullptr"
        )
        inactive = process.index("!active.load(std::memory_order_acquire)")
        self.assertLess(missing_engine, inactive)
        self.assertNotIn(
            "setStatus(MonitorProcessStatus::Inactive)",
            process[missing_engine:inactive],
        )

    def test_audio_callback_contains_no_allocation_blocking_or_io(self) -> None:
        processor = self.read_required("MonitorVST3/MonitorVST3Processor.cpp")
        process = method_body(processor, "MonitorVST3Processor::process(")

        forbidden_patterns = (
            r"\b(?:new|delete|malloc|calloc|realloc|free)\b",
            r"\.(?:resize|reserve|push_back|emplace_back)\s*\(",
            r"\b(?:mutex|condition_variable|lock_guard|unique_lock)\b",
            r"\b(?:Sleep|WaitFor\w*|CreateFile\w*|ReadFile|WriteFile)\s*\(",
            r"\b(?:RegOpen\w*|RegQuery\w*|CoCreateInstance|LoadLibrary\w*)\s*\(",
            r"\b(?:fopen|ifstream|ofstream|filesystem|CreateThread)\b",
            r"\b(?:LogF|TraceF)\s*\(",
            r"engine\.(?:initialize|loadConfig|loadConfigFile|setDeviceInfo)\s*\(",
        )
        for pattern in forbidden_patterns:
            with self.subTest(pattern=pattern):
                self.assertNotRegex(process, re.compile(pattern))

    def test_dsp_dependencies_are_statically_linked_into_the_module(self) -> None:
        project = self.read_required("MonitorVST3/MonitorVST3.vcxproj")
        header = self.read_required("MonitorVST3/MonitorVST3Processor.h")
        processor = self.read_required("MonitorVST3/MonitorVST3Processor.cpp")

        for token in (
            "MONITOR_STATIC_LIB_DIR",
            "fftw3.lib",
            "sndfile.lib",
            "FLAC.lib",
            "ogg.lib",
            "vorbis.lib",
            "vorbisenc.lib",
            "opus.lib",
            "mpg123.lib",
        ):
            with self.subTest(static_dependency=token):
                self.assertIn(token, project)

        for forbidden in (
            "DelayLoadDLLs",
            "delayimp.lib",
            "MonitorVST3Runtime",
            "sndfile.dll",
            "fftw3.dll",
        ):
            with self.subTest(dynamic_dependency=forbidden):
                self.assertNotIn(forbidden, project + header + processor)
        self.assertFalse((PLUGIN_ROOT / "MonitorVST3Runtime.cpp").exists())
        self.assertFalse((PLUGIN_ROOT / "MonitorVST3Runtime.h").exists())

        setup = method_body(processor, "MonitorVST3Processor::setupProcessing(")
        process = method_body(processor, "MonitorVST3Processor::process(")
        initialize = setup.find("newEngine->initialize(")
        self.assertGreaterEqual(initialize, 0)
        for forbidden in ("runtimeDependencies", "ensureLoaded", "LoadLibrary"):
            with self.subTest(process_hot_path_forbidden=forbidden):
                self.assertNotIn(forbidden, process)

        self.assertIn("engine.reset()", setup)
        offline_return = setup.find("setStatus(MonitorProcessStatus::OfflineDry)")
        self.assertGreaterEqual(offline_return, 0)
        self.assertGreater(initialize, offline_return)

    def test_terminate_releases_the_monitor_engine_off_the_audio_thread(self) -> None:
        header = self.read_required("MonitorVST3/MonitorVST3Processor.h")
        processor = self.read_required("MonitorVST3/MonitorVST3Processor.cpp")
        self.assertIn("PLUGIN_API terminate() override", header)
        terminate = method_body(processor, "MonitorVST3Processor::terminate(")
        for token in (
            "processing.store(false",
            "active.store(false",
            "engineReady.store(false",
            "engine.reset()",
            "AudioEffect::terminate()",
        ):
            with self.subTest(token=token):
                self.assertIn(token, terminate)

    def test_release_binary_has_a_normal_and_delay_import_audit(self) -> None:
        self.assertTrue(BINARY_AUDIT_PATH.is_file())
        self.assertTrue(BINARY_AUDIT_MODULE_PATH.is_file())
        import_gate = (
            'Import-Module (Join-Path $PSScriptRoot "MonitorVST3Binary.psm1") -Force'
        )
        self.assertIn(import_gate, BINARY_AUDIT)
        self.assertIn("Assert-MonitorVST3Binary", BINARY_AUDIT)
        self.assertIn(import_gate, MONITOR_MANAGER)
        self.assertIn("Assert-MonitorVST3PayloadBinaries", MONITOR_MANAGER)

        for token in (
            "0x8664",
            "not AMD64",
            "NormalImports",
            "DelayImports",
            "-Index 1",
            "-Index 13",
            "-Delay $false",
            "-Delay $true",
            "fftw3.dll",
            "sndfile.dll",
            "libmp3lame.dll",
            "vorbisfile.dll",
            "forbidden shared-process dependency",
        ):
            with self.subTest(token=token):
                self.assertIn(token, BINARY_AUDIT_MODULE)

    def test_ci_runs_reproducible_binary_gates_but_not_an_unbootstrapped_validator(
        self,
    ) -> None:
        for workflow_name, workflow in (
            ("build", BUILD_WORKFLOW),
            ("release", RELEASE_WORKFLOW),
        ):
            with self.subTest(workflow=workflow_name):
                self.assertIn("test-monitor-vst3-binary.ps1", workflow)
                self.assertIn("test-vst-self-load-guard.ps1", workflow)
                self.assertNotIn("test-monitor-vst3-validator.ps1", workflow)
                self.assertNotIn("validator.exe", workflow)
                self.assertNotIn("vst3-validator", workflow)

        release_configuration_guard = (
            "if: ${{ (inputs.configuration || 'Release') == 'Release' }}"
        )
        self.assertEqual(BUILD_WORKFLOW.count(release_configuration_guard), 2)
        self.assertIn("人工 release gate", DECISION)
        self.assertIn("尚無受專案維護、可重現的 validator", DECISION)

    def test_manual_validator_gate_has_a_stable_repo_entry_point(self) -> None:
        self.assertTrue(VALIDATOR_GATE_PATH.is_file())
        for token in (
            'ValidateSet("Release")',
            "_build\\vst3-validator-x64\\bin\\validator.exe",
            "build\\VST3\\$Configuration\\HibikiEQAPO\\$bundleName",
            "Contents\\x86_64-win\\$bundleName",
            "& $resolvedValidator -e $resolvedBundle",
            "$LASTEXITCODE",
            "VST3 validator failed",
        ):
            with self.subTest(token=token):
                self.assertIn(token, VALIDATOR_GATE)

    def test_only_windows_compatible_vst_speaker_layouts_reach_filter_engine(self) -> None:
        header = self.read_required("MonitorVST3/MonitorVST3Processor.h")
        processor = self.read_required("MonitorVST3/MonitorVST3Processor.cpp")
        readme = self.read_required("MonitorVST3/README.md")

        for token in (
            "configuredChannelMask",
            "toFilterEngineChannelMask",
            "isSupportedSpeakerArrangement",
            "SpeakerArr::kMono",
            "windowsSpeakerBits",
            "arrangement & ~windowsSpeakerBits",
        ):
            with self.subTest(token=token):
                self.assertIn(token, header + processor)
        self.assertIn(
            "configuredChannelMask.load(std::memory_order_acquire)",
            processor,
        )

        set_bus = method_body(processor, "MonitorVST3Processor::setBusArrangements(")
        reject_unsupported = set_bus.find(
            "!isSupportedSpeakerArrangement(inputs[0])"
        )
        delegate_to_sdk = set_bus.find("AudioEffect::setBusArrangements")
        self.assertGreaterEqual(reject_unsupported, 0)
        self.assertGreater(
            delegate_to_sdk,
            reject_unsupported,
            "Unsupported layouts must be rejected before the SDK accepts the bus",
        )
        self.assertIn("Windows speaker positions", readme)
        self.assertNotIn("positions outside the Windows speaker mask retain", readme)

    def test_unknown_sample_type_is_rejected_before_any_dry_copy(self) -> None:
        processor = self.read_required("MonitorVST3/MonitorVST3Processor.cpp")
        process = method_body(processor, "MonitorVST3Processor::process(")
        rejected = process.find("MonitorProcessStatus::UnsupportedSampleSize")
        offline = process.find("const bool offlineMode")
        self.assertGreaterEqual(rejected, 0)
        self.assertGreater(offline, rejected)

    def test_project_builds_the_standard_x64_vst3_bundle(self) -> None:
        project = self.read_required("MonitorVST3/MonitorVST3.vcxproj")

        for token in (
            "MonitorVST3Factory.cpp",
            "MonitorVST3Processor.cpp",
            "MonitorVST3Controller.cpp",
            "..\\Common.vcxproj",
            "VST3_SDK_ROOT",
            "MONITOR_STATIC_LIB_DIR",
            "<TargetName>HibikiEQAPOMonitor</TargetName>",
            "<TargetExt>.vst3</TargetExt>",
            "VST3\\$(Configuration)\\HibikiEQAPO\\HibikiEQAPOMonitor.vst3",
        ):
            with self.subTest(token=token):
                self.assertIn(token, project)
        self.assertIn("x64", project)

    def test_decision_accepts_the_monitor_vst3_build_and_owned_install_path(self) -> None:
        self.assertIn("## 狀態\n\nAccepted", DECISION)
        self.assertNotIn("## 狀態\n\nProposed", DECISION)
        self.assertIn("manage-monitor-vst3.ps1", DECISION)
        self.assertIn("主 NSIS", DECISION)
        self.assertIn("不會自動安裝", DECISION)
        self.assertIn("真實 DAW", DECISION)

    def test_solution_local_build_and_staging_include_the_x64_bundle(self) -> None:
        self.assertIn(
            '"MonitorVST3", "MonitorVST3\\MonitorVST3.vcxproj"',
            SOLUTION,
        )
        self.assertIn("{E89AB845-3184-43F0-8654-1AB9625B9D08}", SOLUTION)
        self.assertIn("{6758B50E-B9F0-4389-A61D-A842F0545E1B}", SOLUTION)
        self.assertIn('"MonitorVST3\\MonitorVST3.vcxproj"', LOCAL_BUILD)
        self.assertIn("VST3_SDK_ROOT", LOCAL_BUILD)
        self.assertIn("/p:VST3_SDK_ROOT=", LOCAL_BUILD)
        self.assertIn("MONITOR_STATIC_LIB_DIR", LOCAL_BUILD)
        self.assertIn("HibikiEQAPOMonitor.vst3", INSTALLER_STAGE)
        self.assertIn("Contents\\x86_64-win", INSTALLER_STAGE)
        self.assertIn("Assert-RegularNonEmptyFile", INSTALLER_STAGE)
        self.assertIn("test-monitor-vst3-binary.ps1", INSTALLER_STAGE)
        self.assertIn('build\\VST3\\$Configuration', INSTALLER_STAGE)
        self.assertIn("Reset-SafeGeneratedTree", INSTALLER_STAGE)
        self.assertIn("Get-ContainedFullPath", SAFE_GENERATED_TREE)
        self.assertIn("contains a reparse point", SAFE_GENERATED_TREE)
        self.assertIn("Assert-DirectoryChainNoReparse", SAFE_GENERATED_TREE)
        self.assertIn(
            "System.Collections.Generic.Stack[string]", SAFE_GENERATED_TREE
        )
        self.assertNotIn("-Recurse", SAFE_GENERATED_TREE)
        self.assertNotIn(
            "Remove-Item -LiteralPath $libDir -Recurse -Force -ErrorAction SilentlyContinue",
            INSTALLER_STAGE,
        )

    def test_owned_manual_manager_and_readme_are_part_of_the_supported_path(self) -> None:
        manager_path = ROOT / "scripts" / "manage-monitor-vst3.ps1"
        self.assertTrue(manager_path.is_file())
        manager = manager_path.read_text(encoding="utf-8-sig")
        for required in (
            'ValidateSet("Install", "Uninstall")',
            '"VST3\\HibikiEQAPO"',
            'product = "HibikiEQAPOMonitor"',
            "Get-FileHash",
            "Assert-OwnedBundle",
            "-LiteralPath $destinationBundle",
        ):
            with self.subTest(manager_contract=required):
                self.assertIn(required, manager)
        for forbidden in ("Remove-Item -Recurse", "Remove-Item -Path", "*.*"):
            with self.subTest(manager_forbidden=forbidden):
                self.assertNotIn(forbidden, manager)

        plugin_readme = self.read_required("MonitorVST3/README.md")
        for required in (
            "normal local x64 build creates",
            "stage-installer-x64.ps1` first audits both normal and delay-load PE imports",
            "then validates the statically linked module and copies it",
            "main NSIS script does not consume that VST3 staging subtree",
            "manage-monitor-vst3.ps1",
            "The default destination is",
            "main NSIS installer does not install",
        ):
            with self.subTest(required=required):
                self.assertIn(required, plugin_readme)

        self.assertNotIn("source-only prototype", plugin_readme)
        self.assertNotIn("Do not copy a locally built bundle", plugin_readme)

    def test_setup_nsi_does_not_install_the_external_monitor_vst3_bundle(self) -> None:
        for forbidden in (
            "MonitorVST3",
            "HibikiEQAPOMonitor",
            "manage-monitor-vst3.ps1",
        ):
            with self.subTest(setup_forbidden=forbidden):
                self.assertNotIn(forbidden, INSTALLER)

        external_vst3_paths = (
            r"\$(?:COMMONFILES|COMMONFILES32|COMMONFILES64)[^\r\n]*\\VST3",
            r"CommonProgramFiles[^\r\n]*\\VST3",
            r"Common Files\\VST3",
        )
        for pattern in external_vst3_paths:
            with self.subTest(external_vst3_path=pattern):
                self.assertNotRegex(
                    INSTALLER,
                    re.compile(pattern, re.IGNORECASE),
                )

    def test_plugin_documentation_discloses_placement_and_realtime_export_risk(self) -> None:
        plugin_readme = self.read_required("MonitorVST3/README.md")
        self.assertIn(EXPORT_SAFETY_WARNING, plugin_readme)
        self.assertRegex(plugin_readme, re.compile(r"monitor", re.IGNORECASE))
        self.assertRegex(plugin_readme, re.compile(r"control.?room", re.IGNORECASE))
        self.assertRegex(plugin_readme, re.compile(r"offline", re.IGNORECASE))

        forbidden_claims = (
            r"automatically prevents? (?:all )?(?:export|render)",
            r"cannot be (?:exported|rendered|printed)",
            r"safe (?:in|on) (?:any|every) (?:insert|bus)",
            r"自動避免.{0,24}(?:master|匯出|渲染)",
            r"絕不會.{0,24}(?:匯出|渲染|燒入)",
        )
        for pattern in forbidden_claims:
            with self.subTest(pattern=pattern):
                self.assertNotRegex(
                    plugin_readme,
                    re.compile(pattern, re.IGNORECASE),
                )


if __name__ == "__main__":
    unittest.main()
