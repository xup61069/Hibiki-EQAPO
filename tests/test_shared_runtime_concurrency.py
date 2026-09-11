#!/usr/bin/env python3
"""Contracts for process-wide helpers used by parallel audio-engine instances."""

from __future__ import annotations

import pathlib
import re
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]


def read(relative_path: str) -> str:
    return (ROOT / relative_path).read_text(encoding="utf-8-sig")


class SharedRuntimeConcurrencyTests(unittest.TestCase):
    def test_library_initialization_is_serialized_and_reentrant_load_fails_fast(self) -> None:
        header = read("helpers/AbstractLibrary.h")
        source = read("helpers/AbstractLibrary.cpp")

        self.assertIn("RECURSIVE_LOADING", header)
        self.assertIn("std::recursive_mutex initializationMutex", header)
        self.assertIn("std::lock_guard<std::recursive_mutex>", source)
        self.assertRegex(
            source,
            re.compile(r"if\s*\(initializing\).*?RECURSIVE_LOADING", re.S),
        )
        self.assertIn("customUninitialize", header + source)
        self.assertIn("unloadAfterInitializationFailure", header + source)

        vst_header = read("helpers/VSTPluginLibrary.h")
        vst_source = read("helpers/VSTPluginLibrary.cpp")
        self.assertIn("isCurrentModule", vst_header + vst_source)
        self.assertIn("GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS", vst_source)
        self.assertRegex(
            vst_source,
            re.compile(r"VSTPluginLibrary::initialize\(\).*?selfModule.*?RECURSIVE_LOADING", re.S),
        )

    def test_vst_library_cache_and_default_path_are_thread_safe(self) -> None:
        header = read("helpers/VSTPluginLibrary.h")
        source = read("helpers/VSTPluginLibrary.cpp")

        for token in (
            "getInstanceKey",
            "GetFullPathNameW",
            "CharLowerBuffW",
            "instanceMapMutex",
            "std::lock_guard<std::mutex>",
            "std::once_flag defaultPluginPathOnce",
            "std::call_once",
            "instance.lock()",
            "InstanceSlot",
            "hasInstance",
            "destructorThread",
            "condition.wait",
            "weakSlot",
        ):
            with self.subTest(token=token):
                self.assertIn(token, header + source)

        get_instance = source[
            source.index("VSTPluginLibrary::getInstance(") : source.index(
                "VSTPluginLibrary::getDefaultPluginPath()"
            )
        ]
        self.assertNotIn("return it->second", get_instance)

    def test_vst_cache_and_self_load_guard_use_physical_file_identity(self) -> None:
        header = read("helpers/VSTPluginLibrary.h")
        source = read("helpers/VSTPluginLibrary.cpp")

        for token in (
            "tryGetFileIdentityKey",
            "CreateFileW",
            "GetFileInformationByHandleEx",
            "FileIdInfo",
            "FILE_SHARE_DELETE",
            "VolumeSerialNumber",
            "FileId.Identifier",
        ):
            with self.subTest(token=token):
                self.assertIn(token, header + source)

        outproc_factory = read("filters/OutProcVSTPluginFilterFactory.cpp")
        self.assertIn("isCurrentModulePath", outproc_factory)
        reject = outproc_factory.index("VSTPluginLibrary::isCurrentModulePath")
        launch = outproc_factory.index("constructFilter<OutProcVSTPluginFilter>")
        self.assertLess(reject, launch)
        self.assertIn("Recursive out-of-process VST library load rejected", outproc_factory)

    def test_vst3_module_entry_factory_and_exit_are_balanced(self) -> None:
        header = read("helpers/VSTPluginLibrary.h")
        source = read("helpers/VSTPluginLibrary.cpp")

        for token in (
            "~VSTPluginLibrary() override",
            'GetProcAddress(module, "InitDll")',
            'GetProcAddress(module, "ExitDll")',
            "factory->release()",
            "customUninitialize",
            "vst3ModuleEntered",
        ):
            with self.subTest(token=token):
                self.assertIn(token, header + source)
        self.assertLess(
            source.index("vst3ModuleEntered = true"),
            source.index("!InitModule()"),
        )

    def test_logger_publishes_a_locked_snapshot_and_reset_clears_all_modes(self) -> None:
        header = read("helpers/LogHelper.h")
        source = read("helpers/LogHelper.cpp")

        for token in (
            "stateMutex",
            "outputMutex",
            "std::lock_guard<std::mutex>",
            "activeLogPath",
            "activePresetFP",
            "activeEnableTrace",
            "catch (const RegistryException& e)",
            "initializationError.c_str()",
        ):
            with self.subTest(token=token):
                self.assertIn(token, header + source)

        reset = source[source.index("void LogHelper::reset()") : source.index("void LogHelper::set(")]
        for assignment in (
            "initialized = false",
            "logPath = L\"\"",
            "enableTrace = false",
            "presetFP = NULL",
            "compact = false",
            "useConsoleColors = false",
        ):
            with self.subTest(reset_assignment=assignment):
                self.assertIn(assignment, reset)

    def test_vst2_time_info_and_window_registration_are_not_racy_globals(self) -> None:
        header = read("helpers/VSTPluginInstance.h")
        source = read("helpers/VSTPluginInstance.cpp")

        self.assertIn("vst_time_info vstTime", header)
        self.assertIn("getVST2TimeInfo", header + source)
        self.assertNotRegex(source, re.compile(r"^vst_time_info vstTime", re.M))
        self.assertIn("std::once_flag registered", source)
        self.assertIn("std::call_once", source)
        self.assertNotIn("static bool registered", source)

    def test_vst_load_diagnostics_pass_string_buffers_to_varargs(self) -> None:
        factory = read("filters/VSTPluginFilterFactory.cpp")
        self.assertNotRegex(
            factory,
            re.compile(r"(?:LogF|TraceF|VSTDiag)\([^\n]*getLibPath\(\)(?!\.c_str\(\))"),
        )

    def test_native_self_load_guard_exercises_direct_and_hardlink_paths(self) -> None:
        benchmark = read("Benchmark/Benchmark.cpp")
        benchmark_project = read("Benchmark/Benchmark.vcxproj")
        script = read("scripts/test-vst-self-load-guard.ps1")

        for token in (
            "runVSTSelfLoadGuardTest",
            "GetFullPathNameW",
            "VSTPluginLibrary::isCurrentModulePath",
            "AbstractLibrary::RECURSIVE_LOADING",
            "OutProcVSTPluginFilterFactory",
            '"vst-self-identity-test"',
        ):
            with self.subTest(benchmark_token=token):
                self.assertIn(token, benchmark)
        for token in (
            "--vst-self-identity-test",
            "third_party\\vcpkg_installed\\x64-windows\\bin",
            "-ItemType HardLink",
            "Benchmark-hardlink.exe",
            "foreach ($candidate in @($benchmark, $aliasPath))",
            "[System.IO.File]::Delete",
            "[System.IO.Directory]::Delete",
        ):
            with self.subTest(script_token=token):
                self.assertIn(token, script)
        self.assertIn("VST3_SDK_ROOT", benchmark_project)
        self.assertIn("$(VST3_SDK_ROOT)", benchmark_project)


if __name__ == "__main__":
    unittest.main()
