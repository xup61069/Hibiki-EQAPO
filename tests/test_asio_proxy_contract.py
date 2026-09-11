from pathlib import Path
import json
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]
DRIVER = ROOT / "HibikiEQAPODriver"
SOLUTION = ROOT / "HibikiEQAPO.sln"


def read(relative: str) -> str:
    return (ROOT / relative).read_text(encoding="utf-8")


class AsioProxyContractTests(unittest.TestCase):
    def test_official_asio_sdk_is_a_manifest_dependency(self) -> None:
        manifest = json.loads(read("vcpkg.json"))
        names = {
            dependency if isinstance(dependency, str) else dependency["name"]
            for dependency in manifest["dependencies"]
        }
        self.assertIn("asiosdk", names)

    def test_stable_driver_identity_and_machine_wide_discovery_are_explicit(self) -> None:
        identity = read("HibikiEQAPODriver/AsioProxyIdentity.h")
        self.assertIn("D47C55C9", identity)
        self.assertIn("3F7D", identity)
        self.assertIn("422F", identity)
        self.assertIn("86E9", identity)
        self.assertIn("32E170815D53", identity)
        self.assertIn('"Hibiki EQAPO"', identity)
        self.assertIn('L"Software\\\\EqualizerAPO\\\\ASIOProxy"', identity)
        self.assertIn('L"TargetCLSID"', identity)

    def test_target_lookup_prefers_user_override_then_machine_default(self) -> None:
        store = read("HibikiEQAPODriver/AsioTargetStore.cpp")
        user_read = store.index("HKEY_CURRENT_USER")
        machine_read = store.index("HKEY_LOCAL_MACHINE")
        self.assertLess(user_read, machine_read)
        self.assertIn("KEY_WOW64_64KEY", store)
        self.assertIn("selectTarget", store)
        self.assertIn("is64BitDll", store)
        self.assertIn("samePhysicalFile", store)
        self.assertIn("enum class RegistryStringStatus", store)
        self.assertIn("RegistryStringStatus::Invalid", store)
        self.assertIn("kTargetClsidValue, false", store)
        self.assertIn("user.present && !user.valid", store)
        self.assertIn("HKEY_CLASSES_ROOT", store)
        self.assertIn("samePhysicalFile(selected.dllPath, *effectivePath)", store)
        self.assertIn("IMAGE_NT_OPTIONAL_HDR64_MAGIC", store)
        self.assertIn("fileHeader.NumberOfSections != 0", store)

    def test_pcm_codec_supports_common_little_endian_formats_and_rejects_dsd(self) -> None:
        codec = read("HibikiEQAPODriver/AsioSampleCodec.cpp")
        for sample_type in (
            "ASIOSTInt16MSB",
            "ASIOSTInt24MSB",
            "ASIOSTInt32MSB",
            "ASIOSTFloat32MSB",
            "ASIOSTFloat64MSB",
            "ASIOSTInt32MSB16",
            "ASIOSTInt32MSB18",
            "ASIOSTInt32MSB20",
            "ASIOSTInt32MSB24",
            "ASIOSTInt16LSB",
            "ASIOSTInt24LSB",
            "ASIOSTInt32LSB",
            "ASIOSTFloat32LSB",
            "ASIOSTFloat64LSB",
            "ASIOSTInt32LSB16",
            "ASIOSTInt32LSB18",
            "ASIOSTInt32LSB20",
            "ASIOSTInt32LSB24",
        ):
            self.assertIn(sample_type, codec)
        for dsd_type in (
            "ASIOSTDSDInt8LSB1",
            "ASIOSTDSDInt8MSB1",
            "ASIOSTDSDInt8NER8",
        ):
            self.assertIn(dsd_type, codec)
        self.assertIn("SampleEncoding::Unsupported", codec)

    def test_realtime_callback_bridge_has_only_atomic_dispatch(self) -> None:
        bridge = read("HibikiEQAPODriver/AsioCallbackBridge.cpp")
        callback_region = bridge[
            bridge.index("void AsioCallbackBridge::bufferSwitchTrampoline") :
        ]
        for forbidden in (
            "new ",
            "delete ",
            "std::mutex",
            "lock_guard",
            "Sleep(",
            "WaitFor",
            "Log",
            "RegOpen",
            "CreateFile",
        ):
            self.assertNotIn(forbidden, callback_region)
        self.assertIn("compare_exchange_strong", bridge)
        self.assertIn("memory_order_acquire", callback_region)

    def test_output_is_processed_into_a_fixed_one_block_stage(self) -> None:
        driver = read("HibikiEQAPODriver/AsioProxyDriver.cpp")
        callback = driver[
            driver.index("void AsioProxyDriver::onBufferSwitch(") :
            driver.index("ASIOTime* AsioProxyDriver::onBufferSwitchTimeInfo(")
        ]
        staged_callback = callback[callback.index("if (lastVendorBufferIndex_") :]
        self.assertLess(staged_callback.index("commitNextStageToVendor"),
                        staged_callback.index("hostCallbacks_.bufferSwitch"))
        self.assertLess(staged_callback.index("hostCallbacks_.bufferSwitch"),
                        staged_callback.index("stageHostOutput"))
        output_ready = driver[
            driver.index("ASIOError AsioProxyDriver::outputReady(") :
        ]
        self.assertIn("deferOutputReady", output_ready)
        self.assertIn("completeNextAsyncBuffer", output_ready)
        self.assertIn("enqueueAsyncBuffer", driver)
        self.assertIn("announcedGenerations_", driver)
        self.assertIn("processingClaim_.compare_exchange_strong", driver)
        self.assertIn("asyncDesynchronized_.store(true", driver)
        header = read("HibikiEQAPODriver/AsioProxyDriver.h")
        self.assertIn("hostBuffers", header)
        self.assertIn("stageBuffers", header)
        self.assertIn("nextCommitSequence_", header)
        self.assertIn("State::Quarantined", driver)
        self.assertIn("restart the host", driver)
        self.assertNotIn("completionSignalQueue_", header)

    def test_pcm_only_future_contract_rejects_dsd_before_forwarding(self) -> None:
        driver = read("HibikiEQAPODriver/AsioProxyDriver.cpp")
        for selector in (
            "kAsioSetIoFormat",
            "kAsioGetIoFormat",
            "kAsioCanDoIoFormat",
        ):
            self.assertIn(selector, driver)
        self.assertIn("kASIOPCMFormat", driver)
        self.assertIn("kASIOFormatInvalid", driver)

    def test_filter_engine_uses_a_stable_explicit_device_scope(self) -> None:
        dsp = read("HibikiEQAPODriver/FilterEngineAsioDsp.cpp")
        call = dsp[dsp.index("engine_->setDeviceInfo(") : dsp.index("engine_->initialize(")]
        self.assertIn('L"Hibiki EQAPO"', call)
        self.assertIn('L"ASIO"', call)
        self.assertIn("ProcessingPolicy::AsioCallbackSafe", dsp)

    def test_asio_filter_policy_is_allowlisted_after_scope_controls(self) -> None:
        engine = read("FilterEngine.cpp")
        policy_check = engine.rindex("isKnownCallbackUnsafeCommand(key)")
        factory_call = engine.index("newFilters = factory->createFilter(path, key, value)")
        key_consumed = engine.index('if (key == L"")', factory_call)
        self.assertGreater(policy_check, factory_call)
        self.assertGreater(policy_check, key_consumed)
        for factory in (
            "VUMeterFilterFactory(), false",
            "OutProcGainFilterFactory(), false",
            "OutProcBiquadFilterFactory(), false",
            "OutProcVSTPluginFilterFactory(), false",
            "VSTPluginFilterFactory(), false",
        ):
            self.assertIn(factory, engine)
        native_tests = read("HibikiEQAPODriver/Tests/AsioProxyDriverTests.cpp")
        self.assertIn(
            "testAsioFilterPolicyHonorsInactiveScopesAndRejectsActiveUnsafeFilters",
            native_tests,
        )
        self.assertIn(
            "testUnsafeAsioReloadPreservesThePublishedSafeConfiguration",
            native_tests,
        )
        self.assertIn("isExplicitManualLoudness", engine)
        self.assertIn("isDisabledOriginalLoudness", engine)

        loudness = read(
            "filters/loudnessCorrection/LoudnessCorrectionFilter.cpp"
        )
        manual_follow = loudness[
            loudness.index("void LoudnessCorrectionFilter::installPendingVolumeFollow()") :
            loudness.index("#pragma AVRT_CODE_END")
        ]
        self.assertLess(
            manual_follow.index("if (_parameters.useManualVolume)"),
            manual_follow.index("TryEnterCriticalSection"),
        )
        self.assertIn(
            "(_parameters.useManualVolume ||\n\t\t TryEnterCriticalSection",
            loudness,
        )

    def test_driver_is_x64_only_and_wired_into_local_build(self) -> None:
        project = read("HibikiEQAPODriver/HibikiEQAPODriver.vcxproj")
        self.assertIn("Debug|x64", project)
        self.assertIn("Release|x64", project)
        self.assertNotRegex(project, r'ProjectConfiguration Include="(?:Debug|Release)\|Win32"')
        self.assertNotRegex(project, r"\$\(Platform\)'=='Win32")
        self.assertIn("<TargetName>HibikiEQAPODriver</TargetName>", project)
        self.assertIn("asiosdk\\common", project)
        self.assertIn("Common.lib", project)
        self.assertIn(
            "HibikiEQAPODriver\\HibikiEQAPODriver.vcxproj",
            SOLUTION.read_text(encoding="utf-8"),
        )
        solution = SOLUTION.read_text(encoding="utf-8")
        build = read("build-local-x64.ps1")
        self.assertIn(
            "HibikiEQAPODriver\\Tests\\AsioProxyDriverTests.vcxproj",
            solution,
        )
        self.assertIn("HibikiEQAPODriver\\HibikiEQAPODriver.vcxproj", build)
        self.assertIn(
            "HibikiEQAPODriver\\Tests\\AsioProxyDriverTests.vcxproj", build
        )
        self.assertIn("AsioProxyCoreTests.exe", build)
        self.assertIn("AsioProxyDriverTests.exe", build)

    def test_driver_binary_metadata_uses_product_and_stable_filename(self) -> None:
        resource = read("HibikiEQAPODriver/HibikiEQAPODriver.rc")
        self.assertIn('#include "../version.h"', resource)
        self.assertIn('VALUE "FileDescription", "Hibiki EQAPO"', resource)
        self.assertIn('VALUE "ProductName", "Hibiki EQAPO"', resource)
        self.assertIn('VALUE "InternalName", "HibikiEQAPODriver.dll"', resource)
        self.assertIn('VALUE "OriginalFilename", "HibikiEQAPODriver.dll"', resource)

    def test_native_tests_cover_codec_selection_and_callback_ownership(self) -> None:
        native_tests = read("HibikiEQAPODriver/Tests/AsioProxyCoreTests.cpp")
        for case_name in (
            "testCodecRoundTrips",
            "testCodecClipsAndSanitizes",
            "testTargetSelection",
            "testSingleActiveCallbackOwner",
            "testDeferredOutputReady",
        ):
            self.assertRegex(native_tests, rf"\b{re.escape(case_name)}\b")
        driver_tests = read("HibikiEQAPODriver/Tests/AsioProxyDriverTests.cpp")
        for case_name in (
            "testOneBlockDirectStagingAndLatencyAccounting",
            "testDeferredWorkerStagingAndVendorReadyCapability",
            "testInlineFalseCompletionStillAppliesCorrection",
            "testLateWorkerDeadlineAndHostHalfReuseAreSafe",
            "testRecursiveCallbackSilencesStaleVendorHalf",
            "testPreparedAndFailedDisposeCallbacksCannotEnterThePipeline",
            "testVendorMustPopulateEveryPrivateDescriptorBuffer",
            "testFailedDescriptorCleanupPinsVendorDescriptorArena",
            "testSuccessfulStopWithOutstandingWorkerQuarantinesPreparedArena",
            "testRecoverableCreateFailureDoesNotPoisonTheProcess",
            "testDsdIoFormatNegotiationIsRejectedBeforeVendorMutation",
            "testTimeInfoReturnPointerIsTransparent",
            "testFailedStopRemainsTerminalUntilSuccessfulBoundary",
            "testStartFailureDrainsCallbackBeforeResettingBuffers",
            "testStopDrainsEnteredCallbackBeforeCallingVendor",
            "testIndependentThreadsMayInitializeInParallel",
            "testUnsafeAsioReloadPreservesThePublishedSafeConfiguration",
            "testNonBufferCallbacksRejectLifecycleReentry",
            "testFinalReleaseInsideMessageIsDeferredPastTheTrampoline",
            "testFinalExternalReleaseCannotRaceAnInFlightCallback",
        ):
            self.assertRegex(driver_tests, rf"\b{re.escape(case_name)}\b")

        self.assertIn("kCallbackActivityClosed", read(
            "HibikiEQAPODriver/AsioProxyDriver.h"
        ))
        self.assertIn("compare_exchange_weak", read(
            "HibikiEQAPODriver/AsioProxyDriver.cpp"
        ))
        for case_name in (
            "testNestedDeferredOutputReadyDoesNotCrossCallbackDepth",
            "testDeferredOutputReadyIsThreadLocal",
            "testCallbackReleaseDrainsInFlight",
        ):
            self.assertRegex(native_tests, rf"\b{re.escape(case_name)}\b")
        bridge = read("HibikiEQAPODriver/AsioCallbackBridge.cpp")
        self.assertLess(
            bridge.index("sink->retainCallbackLifetime()"),
            bridge.index("activeSink_.compare_exchange_strong"),
        )

    def test_com_server_lifetime_and_registration_are_fail_closed(self) -> None:
        server = read("HibikiEQAPODriver/AsioComServer.cpp")
        native_tests = read("HibikiEQAPODriver/Tests/AsioProxyDriverTests.cpp")
        self.assertIn("factoryInstances", server)
        self.assertIn("compare_exchange_weak", server)
        self.assertEqual(server.count("return SELFREG_E_CLASS;"), 2)
        self.assertNotIn("RegCreateKey", server)
        self.assertIn(
            "testClassFactoryKeepsServerLoadedAndLockDoesNotUnderflow", native_tests
        )

    def test_runtime_device_scope_is_publicly_documented(self) -> None:
        readme = read("README.md")
        self.assertIn("Device: Hibiki EQAPO", readme)

    def test_experimental_proxy_has_an_explicit_release_gate(self) -> None:
        decision = read("docs/decisions/0007-transparent-asio-proxy.md")
        checklist = read("Release checklist.txt")
        readme = read("README.md")
        readme_en = read("README.en.md")

        self.assertIn("- 狀態：Proposed", decision)
        self.assertIn("真實音訊介面 ASIO driver 與 DAW 驗證尚未完成", readme)
        self.assertIn("The proxy is not production-ready", readme_en)
        self.assertIn("disable or exclude the proxy from the release installer", checklist)

    def test_sample_codec_fast_paths_and_benchmarking(self) -> None:
        codec = read("HibikiEQAPODriver/AsioSampleCodec.cpp")
        self.assertIn("Float32LSB", codec)
        self.assertIn("Float64LSB", codec)
        self.assertIn("Int32LSB", codec)
        self.assertIn("Int24LSB", codec)
        self.assertIn("Int16LSB", codec)
        core_tests = read("HibikiEQAPODriver/Tests/AsioProxyCoreTests.cpp")
        self.assertIn("runCodecPerformanceBenchmark", core_tests)
        self.assertIn("--asio-codec-performance", core_tests)


if __name__ == "__main__":
    unittest.main()
