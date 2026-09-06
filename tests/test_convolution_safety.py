#!/usr/bin/env python3
"""Safety and UI contracts for the impulse-response convolution feature."""

from __future__ import annotations

import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]


def read(relative_path: str) -> str:
    return (ROOT / relative_path).read_text(encoding="utf-8")


class ConvolutionSafetyTests(unittest.TestCase):
    def test_hybridconv_single_initialization_is_transactional(self) -> None:
        header = read("libHybridConv-0.1.1/libHybridConv_eapo.h")
        source = read("libHybridConv-0.1.1/libHybridConv_eapo.cpp")
        initialize = source[
            source.index("bool hcInitSingle") : source.index("void hcCloseSingle")
        ]
        close = source[
            source.index("void hcCloseSingle") : source.index("void hcBenchmarkDual")
        ]

        self.assertIn("bool hcInitSingle", header)
        self.assertNotIn("void hcInitSingle", header)
        self.assertIn("memset(filter, 0, sizeof(HConvSingle));", initialize)
        self.assertIn("hcCloseSingle(filter);", initialize)
        self.assertIn("return false;", initialize)
        self.assertIn("return true;", initialize)

        for member in (
            "dft_time",
            "dft_freq",
            "in_freq_real",
            "in_freq_imag",
            "steptask",
            "filterbuf_freq_real",
            "filterbuf_freq_imag",
            "mixbuf_freq_real",
            "mixbuf_freq_imag",
            "history_time",
            "fft",
            "ifft",
        ):
            with self.subTest(member=member):
                self.assertIn(f"filter->{member} == NULL", initialize)

        self.assertIn("filter->filterbuf_freq_real[i] == NULL", initialize)
        self.assertIn("filter->filterbuf_freq_imag[i] == NULL", initialize)
        self.assertIn("filter->mixbuf_freq_real[i] == NULL", initialize)
        self.assertIn("filter->mixbuf_freq_imag[i] == NULL", initialize)

        self.assertIn("if (filter == NULL)", close)
        self.assertIn("if (filter->ifft != NULL)", close)
        self.assertIn("if (filter->fft != NULL)", close)
        self.assertIn("if (filter->mixbuf_freq_real != NULL)", close)
        self.assertIn("if (filter->mixbuf_freq_imag != NULL)", close)
        self.assertIn("if (filter->filterbuf_freq_real != NULL)", close)
        self.assertIn("if (filter->filterbuf_freq_imag != NULL)", close)

    def test_hybridconv_rejects_excessive_partition_counts_before_allocating(self) -> None:
        header = read("libHybridConv-0.1.1/libHybridConv_eapo.h")
        source = read("libHybridConv-0.1.1/libHybridConv_eapo.cpp")
        initialize = source[
            source.index("bool hcInitSingle") : source.index("void hcCloseSingle")
        ]
        benchmark = read("Benchmark/Benchmark.cpp")

        self.assertIn("constexpr int HC_MAX_SINGLE_PARTITIONS = 4096;", header)
        self.assertIn(
            "filterBufferCount > HC_MAX_SINGLE_PARTITIONS", initialize
        )
        self.assertLess(
            initialize.index("filterBufferCount > HC_MAX_SINGLE_PARTITIONS"),
            initialize.index("fftw_malloc"),
        )
        self.assertIn("runHybridConvPartitionLimitTests", benchmark)
        self.assertIn("HC_MAX_SINGLE_PARTITIONS + 1", benchmark)
        self.assertIn("hcCloseSingle(&rejectedFilter);", benchmark)

    def test_hybridconv_callers_fail_closed_when_initialization_fails(self) -> None:
        convolution = read("filters/ConvolutionFilter.cpp")
        convolution_initialize = convolution[
            convolution.index("ConvolutionFilter::ConvolutionBank* ConvolutionFilter::createBank") :
            convolution.index("void ConvolutionFilter::destroyBank")
        ]
        graphic = read("filters/GraphicEQFilter.cpp")
        graphic_initialize = graphic[
            graphic.index("bool GraphicEQFilter::prepareImpulseResponse") :
            graphic.index("// Minimum phase spectrum")
        ]
        benchmark = read("Benchmark/Benchmark.cpp")
        benchmark_case = benchmark[
            benchmark.index("HConvSingle filter") :
            benchmark.index("double maxAbsError", benchmark.index("HConvSingle filter"))
        ]

        self.assertIn("!hcInitSingle(&bank->filters[i]", convolution_initialize)
        self.assertIn("destroyBank(bank);", convolution_initialize)
        self.assertNotIn("hcInitSingle", graphic_initialize)
        self.assertIn("impulseResponses.push_back", graphic_initialize)
        self.assertIn("if (!hcInitSingle(", benchmark_case)
        self.assertIn("&filter, impulse.data()", benchmark_case)

    def test_legacy_hybridconv_wrappers_propagate_initialization_failure(self) -> None:
        header = read("libHybridConv-0.1.1/libHybridConv_eapo.h")
        source = read("libHybridConv-0.1.1/libHybridConv_eapo.cpp")
        benchmark = read("Benchmark/Benchmark.cpp")
        get_proc_time = source[
            source.index("double getProcTime") : source.index("void hcPutSingle")
        ]
        initialize_dual = source[
            source.index("bool hcInitDual") : source.index("void hcCloseDual")
        ]
        close_dual = source[
            source.index("void hcCloseDual") : source.index("void hcBenchmarkTripple")
        ]
        initialize_tripple = source[
            source.index("bool hcInitTripple") : source.index("void hcCloseTripple")
        ]
        close_tripple = source[source.index("void hcCloseTripple") :]

        self.assertIn("bool hcInitDual", header)
        self.assertIn("bool hcInitTripple", header)
        self.assertIn("if (!hcInitSingle(&filter", get_proc_time)
        self.assertIn("return -1.0;", get_proc_time)
        self.assertIn("num > HC_MAX_SINGLE_PARTITIONS", get_proc_time)
        self.assertIn("if (pos > xlen - flen)", get_proc_time)
        self.assertIn("memset(filter, 0, sizeof(HConvDual));", initialize_dual)
        self.assertEqual(initialize_dual.count("if (!hcInitSingle("), 2)
        self.assertIn("hcCloseDual(filter);", initialize_dual)
        self.assertIn("if (filter == NULL)", close_dual)
        self.assertIn("if (filter->f_short != NULL)", close_dual)
        self.assertIn("if (filter->f_long != NULL)", close_dual)
        self.assertIn("memset(filter, 0, sizeof(HConvTripple));", initialize_tripple)
        self.assertIn("if (!hcInitSingle(", initialize_tripple)
        self.assertIn("if (!hcInitDual(", initialize_tripple)
        self.assertIn("hcCloseTripple(filter);", initialize_tripple)
        self.assertIn("if (filter == NULL)", close_tripple)
        self.assertIn("if (filter->f_short != NULL)", close_tripple)
        self.assertIn("if (filter->f_medium != NULL)", close_tripple)
        self.assertIn("runHybridConvLegacyWrapperFailureTests", benchmark)
        self.assertIn("dualRejectedCleanly", benchmark)
        self.assertIn("trippleRejectedCleanly", benchmark)
        self.assertIn("procTimeRejectedCleanly", benchmark)

    def test_factory_rejects_empty_paths_without_max_path_truncation(self) -> None:
        source = read("filters/ConvolutionFilterFactory.cpp")

        self.assertIn("StringHelper::trim(parameters)", source)
        self.assertIn("if (value.empty())", source)
        self.assertIn("std::filesystem::path", source)
        self.assertNotIn("MAX_PATH", source)
        self.assertNotIn("PathAppendW", source)

    def test_runtime_validates_file_shape_and_allocation_before_initializing(self) -> None:
        source = read("filters/ConvolutionFilter.cpp")
        initialize = source[
            source.index("vector<wstring> ConvolutionFilter::initialize") :
            source.index("void ConvolutionFilter::copyDry")
        ]
        prepare = source[
            source.index("bool ConvolutionFilter::prepareImpulseResponse") :
            source.index("ConvolutionFilter::ConvolutionBank* ConvolutionFilter::createBank")
        ]
        create_bank = source[
            source.index("ConvolutionFilter::ConvolutionBank* ConvolutionFilter::createBank") :
            source.index("void ConvolutionFilter::destroyBank")
        ]

        self.assertIn("isSafeImpulseShape", prepare)
        self.assertIn("std::numeric_limits<int>::max()", prepare)
        self.assertIn("std::numeric_limits<size_t>::max()", source)
        self.assertIn("std::any_of(", initialize)
        self.assertIn("!std::isfinite(sample)", initialize)
        self.assertLess(
            initialize.index("!std::isfinite(sample)"),
            initialize.index("impulseResponses.swap(preparedImpulseResponses)"),
        )
        self.assertIn("if (bank->filters == NULL)", create_bank)
        self.assertLess(
            create_bank.index("if (bank->filters == NULL)"),
            create_bank.index("hcInitSingle(&bank->filters[i]"),
        )

        graphic = read("filters/GraphicEQFilter.cpp")
        graphic_prepare = graphic[
            graphic.index("bool GraphicEQFilter::prepareImpulseResponse") :
        ]
        self.assertIn("createImpulseResponse", graphic_prepare)
        self.assertIn("!std::isfinite(node.freq)", graphic_prepare)
        self.assertIn("node.freq <= 0.0", graphic_prepare)
        self.assertIn("!std::isfinite(node.dbGain)", graphic_prepare)
        self.assertIn("node.freq <= previousFrequency", graphic_prepare)
        self.assertIn("previousFrequency = node.freq", graphic_prepare)
        self.assertIn("impulseResponses.push_back", graphic_prepare)
        self.assertNotIn("hcInitSingle", graphic_prepare)

        benchmark = read("Benchmark/Benchmark.cpp")
        self.assertIn("runInvalidConvolutionInputTests", benchmark)
        self.assertIn('"GraphicEQ zero-frequency fails dry out-of-place"', benchmark)
        self.assertIn('"GraphicEQ negative-frequency fails dry in-place"', benchmark)
        self.assertIn('"GraphicEQ duplicate-frequency fails dry out-of-place"', benchmark)
        self.assertIn('"GraphicEQ non-finite frequency fails dry out-of-place"', benchmark)
        self.assertIn('"GraphicEQ non-finite gain fails dry in-place"', benchmark)
        self.assertIn('"Convolution non-finite impulse fails dry"', benchmark)

    def test_native_benchmark_rejects_invalid_batch_and_nonfinite_output(self) -> None:
        benchmark = read("Benchmark/Benchmark.cpp")

        self.assertIn("if (batchsizeArg.getValue() == 0)", benchmark)
        self.assertIn('"Error: --batchsize must be greater than zero.', benchmark)
        self.assertIn("bool outputIsFinite = true;", benchmark)
        self.assertIn("if (!std::isfinite(buf2[i]))", benchmark)
        self.assertIn('"Processing produced non-finite output samples.', benchmark)
        self.assertIn("getSafeSampleCount<float>(", benchmark)
        self.assertIn("vector<float> buf;", benchmark)
        self.assertIn("vector<float> buf2(sampleCount, 0.0f);", benchmark)
        self.assertIn("info.frames <= 0", benchmark)
        self.assertIn("requestedFrames < 1.0", benchmark)
        self.assertIn("framesRead <= 0", benchmark)
        self.assertIn("framesWritten <= 0", benchmark)
        self.assertIn("while (processedFrames < frameCount)", benchmark)
        self.assertIn("finite &&", benchmark)
        self.assertIn("std::isfinite(actual[index])", benchmark)
        self.assertIn("std::numeric_limits<double>::infinity()", benchmark)

    def test_runtime_callback_remains_bounded_and_io_free(self) -> None:
        source = read("filters/ConvolutionFilter.cpp")
        process = source[
            source.index("void ConvolutionFilter::process") :
            source.index("unsigned long __stdcall ConvolutionFilter::bankWorkerEntry")
        ]
        copy_dry = source[
            source.index("void ConvolutionFilter::copyDry") :
            source.index("void ConvolutionFilter::process")
        ]
        callback = copy_dry + process

        for forbidden in (
            "new ",
            "malloc(",
            "sf_",
            "fftw_",
            "Path",
            "LogF(",
            "WaitFor",
            "CreateThread",
            "SetEvent",
            "Sleep(",
            "createBank(",
            "destroyBank(",
            "hcCloseSingle",
            ".resize(",
            ".push_back(",
            "MemoryHelper::",
            "EnterCriticalSection",
            "lock_guard",
            "condition_variable",
            "CloseHandle",
            ".join(",
        ):
            self.assertNotIn(forbidden, callback)

        self.assertIn(
            "requestedFrameCount.store(frameCount, std::memory_order_release)",
            process,
        )
        self.assertIn("pendingBank.compare_exchange_strong", process)
        self.assertIn("retiredBank.store", process)
        self.assertIn("copyDry", process)
        self.assertLess(
            process.index("requestedFrameCount.store"),
            process.index("hcPutSingle"),
        )

    def test_async_bank_handoff_has_worker_owned_reclamation(self) -> None:
        header = read("filters/ConvolutionFilter.h")
        source = read("filters/ConvolutionFilter.cpp")
        worker = source[
            source.index("void ConvolutionFilter::bankWorkerLoop") :
            source.index("bool ConvolutionFilter::prepareImpulseResponse")
        ]
        cleanup = source[
            source.index("void ConvolutionFilter::cleanup") :
            source.index("void ConvolutionFilter::stopWorker")
        ]

        self.assertIn("std::atomic<ConvolutionBank*> pendingBank", header)
        self.assertIn("std::atomic<ConvolutionBank*> retiredBank", header)
        self.assertIn("std::atomic<unsigned> requestedFrameCount", header)
        self.assertIn("static_assert(std::atomic<ConvolutionBank*>::is_always_lock_free", source)
        self.assertIn("static_assert(std::atomic<unsigned>::is_always_lock_free", source)
        self.assertIn("createBank(requested)", worker)
        self.assertIn("retiredBank.exchange", worker)
        self.assertIn("destroyBank(retired)", worker)
        self.assertIn("pendingBank.compare_exchange_strong", worker)
        self.assertNotIn("prepareImpulseResponse(", worker)
        self.assertIn("stopWorker();", cleanup)
        self.assertLess(cleanup.index("stopWorker();"), cleanup.index("destroyBank("))
        self.assertLess(cleanup.index("stopWorker();"), cleanup.index("impulseResponses.clear()"))

    def test_impulse_is_cached_before_worker_and_fade_scratch_is_preallocated(self) -> None:
        header = read("filters/ConvolutionFilter.h")
        source = read("filters/ConvolutionFilter.cpp")
        initialize = source[
            source.index("vector<wstring> ConvolutionFilter::initialize") :
            source.index("void ConvolutionFilter::copyDry")
        ]
        graphic_header = read("filters/GraphicEQFilter.h")

        self.assertIn("virtual bool prepareImpulseResponse", header)
        self.assertIn("std::vector<std::vector<double>> impulseResponses", header)
        self.assertIn("double* fadeScratch", header)
        self.assertIn("prepareImpulseResponse(preparedImpulseResponses)", initialize)
        self.assertIn("impulseResponses.swap(preparedImpulseResponses)", initialize)
        self.assertIn("fadeScratch = static_cast<double*>(MemoryHelper::alloc", initialize)
        self.assertIn("memset(fadeScratch, 0, scratchBytes)", initialize)
        self.assertLess(initialize.index("impulseResponses.swap"), initialize.index("startWorker()"))
        self.assertIn("bool asyncRebuildEnabled = false", initialize)
        self.assertIn("fadeScratch != NULL", initialize)
        self.assertIn("asyncRebuildEnabled && !startWorker()", initialize)
        self.assertIn("bool prepareImpulseResponse", graphic_header)

    def test_native_benchmark_covers_async_ir_and_graphic_eq_mismatch(self) -> None:
        benchmark = read("Benchmark/Benchmark.cpp")
        runtime_test = read("scripts/test-runtime-loudness.ps1")

        self.assertIn("runAsyncConvolutionFrameMismatchTests", benchmark)
        self.assertIn("maximumFrameCount = 1024", benchmark)
        self.assertIn("actualFrameCount = 480", benchmark)
        self.assertIn('"IR out-of-place"', benchmark)
        self.assertIn('"IR in-place"', benchmark)
        self.assertIn('"GraphicEQ out-of-place"', benchmark)
        self.assertIn('"GraphicEQ in-place"', benchmark)
        self.assertIn("FFTWPlannerGuard", benchmark)
        self.assertIn("callbackMilliseconds", benchmark)
        self.assertIn("firstCallbackMilliseconds", benchmark)
        self.assertIn("secondCallbackMilliseconds", benchmark)
        self.assertIn("sawValidTransition", benchmark)
        self.assertIn("--convselftest", runtime_test)

    def test_all_product_planners_share_one_lock(self) -> None:
        helper = read("helpers/FFTWHelper.h")
        self.assertIn("class FFTWPlannerGuard", helper)
        self.assertIn("static std::mutex plannerMutex", helper)

        for relative_path in (
            "libHybridConv-0.1.1/libHybridConv_eapo.cpp",
            "filters/GraphicEQFilter.cpp",
            "Editor/AnalysisThread.cpp",
            "Editor/guis/ConvolutionFilterGUI.cpp",
        ):
            with self.subTest(relative_path):
                source = read(relative_path)
                self.assertIn('#include "helpers/FFTWHelper.h"', source)
                self.assertIn("FFTWPlannerGuard", source)

        self.assertNotIn(
            "static mutex fftwPlannerMutex", read("filters/GraphicEQFilter.cpp")
        )
        self.assertNotIn(
            "static mutex fftwPlannerMutex", read("filters/ConvolutionFilter.cpp")
        )

    def test_ir_ui_uses_short_names_and_never_converts_while_typing(self) -> None:
        factory = read("Editor/guis/ConvolutionFilterGUIFactory.cpp")
        source = read("Editor/guis/ConvolutionFilterGUI.cpp")
        ui = read("Editor/guis/ConvolutionFilterGUI.ui")

        self.assertIn('tr("IR convolution")', factory)
        self.assertNotIn("Convolution (Convolution with impulse response)", factory)
        self.assertIn("<string>IR file:</string>", ui)
        self.assertNotIn("&QLineEdit::textChanged", source)
        self.assertIn('tr("%1 ms (%2 samples)")', source)
        self.assertIn('tr("%1 Hz")', source)
        self.assertNotIn('tr("%0 ms (%1 samples)")', source)
        self.assertNotIn('tr("%0 Hz")', source)
        self.assertNotIn("%0", source)

        select_local = source[
            source.index("void ConvolutionFilterGUI::selectBundledImpulseResponse") :
            source.index("void ConvolutionFilterGUI::selectBundledImpulseAt")
        ]
        update_info = source[source.index("void ConvolutionFilterGUI::updateFileInfo") :]
        self.assertNotIn("matchDeviceSampleRate(", select_local)
        self.assertNotIn("matchDeviceSampleRate(", update_info)
        self.assertIn("matchedFirActionVisible = sampleRateMismatch;", update_info)
        self.assertIn("isSafeImpulseShape(info.frames, info.channels", update_info)

    def test_regeneration_is_bounded_and_replaces_only_a_complete_temp_file(self) -> None:
        source = read("Editor/guis/ConvolutionFilterGUI.cpp")
        regenerate = source[
            source.index("static bool regenerateFirFromMagnitude") :
            source.index("static double firMagnitudePeak")
        ]
        match = source[
            source.index("bool ConvolutionFilterGUI::matchDeviceSampleRate") :
            source.index("void ConvolutionFilterGUI::updateFileInfo")
        ]

        self.assertIn("safeFftSize", regenerate)
        self.assertEqual(regenerate.count("fftw_plan_dft_r2c_1d"), 2)
        self.assertEqual(regenerate.count("fftw_plan_dft_c2r_1d"), 2)
        self.assertLess(
            regenerate.index("fftw_plan sourcePlan"),
            regenerate.index("for (int channel = 0; channel < channelCount; channel++)"),
        )
        self.assertIn("isSafeImpulseShape", match)
        self.assertIn("temporaryOutputPath", match)
        self.assertIn("MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH", match)
        self.assertIn("outputData.swap(inputData);", match)
        self.assertIn("std::isfinite(magnitudePeak)", match)
        self.assertIn("magnitudePeak <= 0.0", match)


if __name__ == "__main__":
    unittest.main()
