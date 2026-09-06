#!/usr/bin/env python3
"""Source contracts for realtime-safe FilterEngine configuration handoff."""

from __future__ import annotations

import pathlib
import re
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
HEADER = (ROOT / "FilterEngine.h").read_text(encoding="utf-8")
SOURCE = (ROOT / "FilterEngine.cpp").read_text(encoding="utf-8")
CONFIGURATION_SOURCE = (ROOT / "FilterConfiguration.cpp").read_text(
    encoding="utf-8"
)


def function_body(source: str, signature: str) -> str:
    """Return a C++ function body, accounting for nested braces."""
    signature_index = source.index(signature)
    opening_brace = source.index("{", signature_index)
    depth = 0
    for index in range(opening_brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[opening_brace + 1 : index]
    raise AssertionError(f"unterminated function: {signature}")


class FilterEngineHandoffTests(unittest.TestCase):
    PROCESS_SIGNATURES = (
        "void FilterEngine::process(float* output",
        "void FilterEngine::process(float** output",
        "void FilterEngine::process(double* output",
        "void FilterEngine::process(double** output",
    )
    REALTIME_HELPER_SIGNATURES = {
        "getSafeSampleCount": "bool getSafeSampleCount(",
        "bypassInterleaved": "void bypassInterleaved(",
        "bypassPlanar": "void bypassPlanar(",
        "convertFloatToDouble": "void convertFloatToDouble(",
        "convertDoubleToFloat": "void convertDoubleToFloat(",
        "commitCompletedTransition": (
            "void FilterEngine::commitCompletedTransition("
        ),
    }

    def reachable_realtime_bodies(self, root_signature: str) -> dict[str, str]:
        """Follow source-local helpers called by one audio entry point."""
        reachable = {root_signature: function_body(SOURCE, root_signature)}
        pending = [root_signature]
        while pending:
            caller = pending.pop()
            caller_body = reachable[caller]
            for name, signature in self.REALTIME_HELPER_SIGNATURES.items():
                helper_call = rf"\b{re.escape(name)}(?:<[^>]+>)?\s*\("
                if re.search(helper_call, caller_body) and name not in reachable:
                    reachable[name] = function_body(SOURCE, signature)
                    pending.append(name)
        return reachable

    def test_pending_and_retired_slots_are_lock_free_atomics(self) -> None:
        self.assertIn("#include <atomic>", HEADER)
        self.assertIn(
            "std::atomic<FilterConfiguration*> pendingConfig;", HEADER
        )
        self.assertIn(
            "std::atomic<FilterConfiguration*> retiredConfig;", HEADER
        )
        self.assertIn(
            "std::atomic<FilterConfiguration*> currentConfig;", HEADER
        )
        self.assertIn("is_always_lock_free", SOURCE)
        self.assertNotIn("FilterConfiguration* nextConfig;", HEADER)
        self.assertNotIn("FilterConfiguration* previousConfig;", HEADER)

    def test_writer_release_publishes_and_audio_acquire_snapshots(self) -> None:
        load_body = function_body(SOURCE, "bool FilterEngine::loadConfig(")
        self.assertIn("pendingConfig.compare_exchange_strong(", load_body)
        self.assertIn("std::memory_order_release", load_body)
        self.assertIn("if (!hasInitialConfiguration)", load_body)
        self.assertIn(
            "currentConfig.store(config, std::memory_order_release);", load_body
        )

    def test_failed_reload_restores_watch_set_and_does_not_wait_for_retirement(self) -> None:
        load_body = function_body(SOURCE, "bool FilterEngine::loadConfig(")
        self.assertIn(
            'bool loadConfig(const std::wstring& customPath = L"");', HEADER
        )
        self.assertIn(
            "RegistryWatchSetTransaction watchTransaction(watchRegistryKeys);",
            load_body,
        )
        self.assertGreaterEqual(load_body.count("watchTransaction.rollback();"), 2)
        self.assertIn("watchTransaction.commit();", load_body)
        self.assertIn("return false;", load_body)
        self.assertIn("return true;", load_body)

        thread_body = function_body(
            SOURCE, "unsigned long __stdcall FilterEngine::notificationThread("
        )
        self.assertIn(
            "const bool waitForRetirement = engine->loadConfig();", thread_body
        )
        rejection_start = thread_body.index("if (!waitForRetirement)")
        retirement_start = thread_body.index("DWORD retirementWait")
        rejection_body = thread_body[rejection_start:retirement_start]
        self.assertIn("ReleaseSemaphore(engine->loadSemaphore", rejection_body)
        self.assertIn("continue;", rejection_body)
        self.assertLess(rejection_start, retirement_start)

    def test_load_boundary_releases_lock_and_rejects_stl_allocation_failure(self) -> None:
        load_body = function_body(SOURCE, "bool FilterEngine::loadConfig(")
        self.assertIn("CriticalSectionGuard loadGuard(loadSection);", load_body)
        self.assertIn("catch (const std::bad_alloc&)", load_body)
        self.assertIn("discardUnpublishedBuild();", load_body)
        self.assertIn("MemoryHelper::markAllocationFailure();", load_body)
        self.assertNotIn("LeaveCriticalSection(&loadSection);", load_body)

        for signature in self.PROCESS_SIGNATURES:
            process_body = function_body(SOURCE, signature)
            self.assertEqual(process_body.count("pendingConfig.load("), 1)
            self.assertIn("std::memory_order_acquire", process_body)
            self.assertIn("FilterConfiguration* const active = currentConfig.load(", process_body)

    def test_initialize_and_filter_build_release_owned_state_on_exceptions(self) -> None:
        initialize_body = function_body(SOURCE, "void FilterEngine::initialize(")
        self.assertIn(
            "CriticalSectionGuard initializeGuard(loadSection);", initialize_body
        )
        self.assertNotIn("EnterCriticalSection(&loadSection);", initialize_body)
        self.assertNotIn("LeaveCriticalSection(&loadSection);", initialize_body)
        self.assertIn("catch (const std::bad_alloc&)", initialize_body)
        self.assertIn("catch (...)", initialize_body)
        self.assertIn("stopNotificationThread();", initialize_body)
        self.assertIn("cleanupConfigurations();", initialize_body)
        self.assertIn("destroyFilterInfos(filterInfos);", initialize_body)
        self.assertIn("this->maxFrameCount = 0;", initialize_body)
        self.assertIn("allocatedFrameCount = 0;", initialize_body)

        add_filters_body = function_body(SOURCE, "void FilterEngine::addFilters(")
        self.assertIn("void addFilters(std::vector<IFilter*>& filters);", HEADER)
        self.assertIn(
            "void FilterEngine::addFilters(vector<IFilter*>& filters)", SOURCE
        )
        generic_catch = add_filters_body[add_filters_body.rindex("catch (...)") :]
        self.assertIn("restoreBuildState();", generic_catch)
        self.assertIn("discardCurrentFilter();", generic_catch)
        self.assertIn("MemoryHelper::free(*it);", generic_catch)
        self.assertIn("throw;", generic_catch)
        allocation = add_filters_body.index("MemoryHelper::alloc(sizeof(FilterInfo))")
        in_channels = add_filters_body.index("filterInfo->inChannels = NULL;", allocation)
        in_place_call = add_filters_body.index("filter->getInPlace();", allocation)
        self.assertLess(in_channels, in_place_call)

        constructor_body = function_body(SOURCE, "FilterEngine::FilterEngine()")
        self.assertIn("factories.reserve(28);", constructor_body)
        self.assertIn("std::unique_ptr<IFilterFactory> owner(factory);", constructor_body)
        self.assertIn("catch (...)", constructor_body)
        self.assertIn("delete factory;", constructor_body)
        self.assertIn("delete parser;", constructor_body)
        self.assertIn("CloseHandle(loadSemaphore);", constructor_body)
        self.assertIn("DeleteCriticalSection(&loadSection);", constructor_body)

    def test_file_and_notification_handles_are_exception_safe(self) -> None:
        load_file_body = function_body(SOURCE, "void FilterEngine::loadConfigFile(")
        self.assertIn("SCOPE_EXIT", load_file_body)
        self.assertIn("CloseHandle(hFile);", load_file_body)
        self.assertNotIn("CloseHandle(hFile);\n\tif (offlineAnalysis)", load_file_body)
        self.assertIn("const ULONGLONG retryDeadline", load_file_body)
        self.assertIn("GetTickCount64() >= retryDeadline", load_file_body)
        self.assertIn("if (readSucceeded == FALSE)", load_file_body)
        self.assertIn("throw ConfigurationFileLoadError();", load_file_body)
        self.assertLess(
            load_file_body.index("if (readSucceeded == FALSE)"),
            load_file_body.index("inputStream.seekg(0)"),
        )

        thread_body = function_body(
            SOURCE, "unsigned long __stdcall FilterEngine::notificationThread("
        )
        self.assertIn("catch (const std::bad_alloc&)", thread_body)
        self.assertIn("FindCloseChangeNotification(notificationHandle);", thread_body)
        self.assertIn("CloseHandle(registryEvent);", thread_body)
        self.assertIn("RegCloseKey(keyHandle);", thread_body)
        self.assertIn("RegCloseKey(*it);", thread_body)
        self.assertIn("if (reloadTokenHeld)", thread_body)
        self.assertIn("ReleaseSemaphore(engine->loadSemaphore", thread_body)

    def test_all_process_overloads_share_one_noexcept_commit_helper(self) -> None:
        self.assertIn(
            "commitCompletedTransition(FilterConfiguration* pending) noexcept;",
            HEADER,
        )
        helper_start = SOURCE.index("void FilterEngine::commitCompletedTransition(")
        helper_open = SOURCE.index("{", helper_start)
        helper_signature = SOURCE[helper_start:helper_open]
        helper_body = function_body(
            SOURCE, "void FilterEngine::commitCompletedTransition("
        )
        self.assertIn("noexcept", helper_signature)
        self.assertIn("pendingConfig.compare_exchange_strong(", helper_body)
        self.assertIn("retiredConfig.store(retired, std::memory_order_release);", helper_body)
        self.assertNotIn("ReleaseSemaphore(", helper_body)
        self.assertEqual(SOURCE.count("commitCompletedTransition(pending);"), 4)

    def test_audio_callbacks_and_reachable_helpers_remain_realtime_safe(self) -> None:
        forbidden = (
            ".resize(",
            "vector<",
            "std::vector<",
            "new ",
            "delete ",
            "MemoryHelper::alloc(",
            "MemoryHelper::free(",
            "WaitFor",
            "Sleep(",
            "EnterCriticalSection(",
            "LeaveCriticalSection(",
            "ReleaseSemaphore(",
            "SetEvent(",
            "CloseHandle(",
            "CreateEvent",
            "CreateThread(",
            "GetTickCount64(",
            "ReadFile(",
            "WriteFile(",
            "RegCloseKey(",
            "LogF(",
            "LogFStatic(",
            "TraceF(",
        )
        for signature in self.PROCESS_SIGNATURES:
            reachable = self.reachable_realtime_bodies(signature)
            self.assertIn("commitCompletedTransition", reachable)
            for function_name, body in reachable.items():
                for token in forbidden:
                    self.assertNotIn(token, body, f"{function_name}: {token}")
                self.assertNotIn("resizeBuffers(", body, function_name)

        pointer_setup = function_body(SOURCE, "void FilterEngine::resizeBuffers(")
        self.assertIn("inputBufPointers.resize(inputChannelCount);", pointer_setup)
        self.assertIn("outputBufPointers.resize(outputChannelCount);", pointer_setup)

    def test_notification_thread_reclaims_only_after_audio_release(self) -> None:
        thread_body = function_body(
            SOURCE, "unsigned long __stdcall FilterEngine::notificationThread("
        )
        retirement_poll = "engine->retiredConfig.load("
        self.assertIn(retirement_poll, thread_body)
        self.assertIn("std::memory_order_acquire) == nullptr", thread_body)
        self.assertIn("WaitForSingleObject(", thread_body)
        self.assertIn("engine->shutdownEvent, 1", thread_body)
        self.assertLess(
            thread_body.index(retirement_poll),
            thread_body.index("engine->reclaimRetiredConfiguration();"),
        )
        self.assertLess(
            thread_body.index("engine->reclaimRetiredConfiguration();"),
            thread_body.index(
                "ReleaseSemaphore(engine->loadSemaphore",
                thread_body.index("engine->reclaimRetiredConfiguration();"),
            ),
        )

    def test_cleanup_clears_all_slots_without_double_free(self) -> None:
        cleanup_body = function_body(
            SOURCE, "void FilterEngine::cleanupConfigurations()"
        )
        self.assertIn("currentConfig.exchange(", cleanup_body)
        self.assertIn("pendingConfig.exchange(", cleanup_body)
        self.assertIn("retiredConfig.exchange(", cleanup_body)
        self.assertIn("if (pending != active)", cleanup_body)
        self.assertIn("if (retired != active && retired != pending)", cleanup_body)

    def test_reinitialize_stops_waiter_and_resets_reload_token(self) -> None:
        initialize_body = function_body(SOURCE, "void FilterEngine::initialize(")
        self.assertLess(
            initialize_body.index("stopNotificationThread();"),
            initialize_body.index("cleanupConfigurations();"),
        )
        self.assertIn("CloseHandle(loadSemaphore);", initialize_body)
        self.assertIn(
            "loadSemaphore = CreateSemaphore(NULL, 1, 1, NULL);",
            initialize_body,
        )

        stop_body = function_body(
            SOURCE, "void FilterEngine::stopNotificationThread()"
        )
        self.assertIn("SetEvent(shutdownEvent);", stop_body)
        self.assertIn("WaitForSingleObject(threadHandle, INFINITE)", stop_body)
        self.assertIn("shutdownEvent = NULL;", stop_body)

        load_file_body = function_body(SOURCE, "void FilterEngine::loadConfigFile(")
        self.assertIn("WaitForSingleObject(shutdownEvent, 0)", load_file_body)
        self.assertIn("Configuration reload cancelled during shutdown", load_file_body)

    def test_transition_length_avoids_invalid_cast_and_zero_division(self) -> None:
        initialize_body = function_body(SOURCE, "void FilterEngine::initialize(")
        self.assertIn("!std::isfinite(sampleRate)", initialize_body)
        self.assertIn("sampleRate <= 0.0f", initialize_body)
        self.assertIn("std::numeric_limits<int>::max", initialize_body)
        self.assertIn("this->maxFrameCount = 0;", initialize_body)
        self.assertIn("allocatedFrameCount != maxFrameCount", initialize_body)
        self.assertIn("(std::max)(", initialize_body)
        self.assertIn("1u, static_cast<unsigned>(transitionSampleCount)", initialize_body)

        transition_body = function_body(
            CONFIGURATION_SOURCE, "unsigned FilterConfiguration::doTransition("
        )
        branch = "transitionLength != 0 && transitionCounter < transitionLength"
        self.assertIn(branch, transition_body)
        self.assertLess(
            transition_body.index(branch),
            transition_body.index("/\n\t\t\t\ttransitionLength"),
        )
        self.assertIn("if (transitionCounter < transitionLength)", transition_body)

        benchmark = (ROOT / "Benchmark" / "Benchmark.cpp").read_text(
            encoding="utf-8"
        )
        self.assertIn("FilterEngine zero-length transition", benchmark)
        self.assertIn("FilterEngine sample-rate transition bounds", benchmark)
        self.assertIn("FilterEngine locked-file reload timeout", benchmark)


if __name__ == "__main__":
    unittest.main()
