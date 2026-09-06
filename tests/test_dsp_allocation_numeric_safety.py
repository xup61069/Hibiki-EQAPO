#!/usr/bin/env python3
"""Contracts for DSP allocation failure and non-finite numeric inputs."""

from __future__ import annotations

import pathlib
import re
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]


def read(relative_path: str) -> str:
    return (ROOT / relative_path).read_text(encoding="utf-8")


class DspAllocationNumericSafetyTests(unittest.TestCase):
    def test_every_filter_factory_guards_placement_new_allocation(self) -> None:
        factory_base = read("IFilterFactory.h")
        construct_start = factory_base.index("static FilterType* constructFilter(")
        adopt_start = factory_base.index("static std::vector<IFilter*> adoptFilter(")
        construct_body = factory_base[construct_start:adopt_start]
        adopt_body = factory_base[adopt_start:]
        self.assertIn("if (memory == NULL)", construct_body)
        self.assertLess(
            construct_body.index("if (memory == NULL)"),
            construct_body.index("return new(memory) FilterType("),
        )
        self.assertIn("catch (...)", construct_body)
        self.assertIn("MemoryHelper::free(memory);", construct_body)
        self.assertIn("return std::vector<IFilter*>(1, filter);", adopt_body)
        self.assertIn("filter->~IFilter();", adopt_body)
        self.assertIn("MemoryHelper::free(filter);", adopt_body)

        helper_users = 0
        for path in sorted((ROOT / "filters").rglob("*FilterFactory.cpp")):
            source = path.read_text(encoding="utf-8")
            with self.subTest(path=path.relative_to(ROOT)):
                self.assertNotIn("MemoryHelper::alloc(", source)
                self.assertNotRegex(source, r"\bnew\s*\(")
                self.assertNotIn("vector<IFilter*>(1", source)
                self.assertNotIn("push_back(new", source)
            helper_users += source.count("constructFilter<")
        self.assertGreaterEqual(helper_users, 21)

    def test_memory_helper_and_engine_use_checked_transactional_allocation(self) -> None:
        memory_header = read("helpers/MemoryHelper.h")
        memory_source = read("helpers/MemoryHelper.cpp")
        configuration_header = read("FilterConfiguration.h")
        configuration = read("FilterConfiguration.cpp")
        engine = read("FilterEngine.cpp")

        self.assertIn("allocArray(size_t count, size_t elementSize)", memory_header)
        self.assertIn("clearAllocationFailure() noexcept", memory_header)
        self.assertIn("markAllocationFailure() noexcept", memory_header)
        self.assertIn("consumeAllocationFailure() noexcept", memory_header)
        self.assertIn("thread_local bool allocationFailedOnCurrentThread", memory_source)
        self.assertIn("size > maximumSize - 32", memory_source)
        self.assertIn("count > (maximumSize - 32) / elementSize", memory_source)
        self.assertIn("if (ptr == NULL)", memory_source)

        self.assertIn("bool isValid() const", configuration_header)
        self.assertIn("allocatedSampleChannelCount", configuration_header)
        self.assertIn("allocatedSample2ChannelCount", configuration_header)
        self.assertGreaterEqual(configuration.count("MemoryHelper::allocArray("), 7)
        self.assertIn("if (this->filterInfos == NULL)", configuration)
        self.assertIn("if (allSamples == NULL)", configuration)
        self.assertIn("if (allSamples2 == NULL)", configuration)
        self.assertIn("if (currentSamples == NULL)", configuration)
        self.assertIn("if (currentSamples2 == NULL)", configuration)

        self.assertIn("configStorage = MemoryHelper::alloc", engine)
        self.assertIn("if (configStorage == NULL)", engine)
        self.assertIn("if (allocationFailed || !config->isValid())", engine)
        self.assertLess(
            engine.index("MemoryHelper::clearAllocationFailure();"),
            engine.index("void* configStorage = NULL;"),
        )
        self.assertIn("catch (const std::bad_alloc&)", engine)
        self.assertIn("destroyFilterInfos(filterInfos);", engine)
        self.assertIn("if (filterInfo == NULL)", engine)
        self.assertGreaterEqual(engine.count("MemoryHelper::allocArray("), 2)

    def test_stateful_filters_fail_dry_after_partial_allocation_failure(self) -> None:
        copy_filter = read("filters/CopyFilter.cpp")
        delay_filter = read("filters/DelayFilter.cpp")
        iir_filter = read("filters/IIRFilter.cpp")
        vst_filter = read("filters/VSTPluginFilter.cpp")

        self.assertIn("if (allocationFailed)", copy_filter)
        self.assertIn("if (allocationFailed || !stateReady)", iir_filter)
        iir_initialize = iir_filter.split(
            "vector<wstring> IIRFilter::initialize", maxsplit=1
        )[1].split("void IIRFilter::process", maxsplit=1)[0]
        self.assertNotIn("allocationFailed = true", iir_initialize)
        self.assertIn("stateReady = false;", iir_initialize)
        self.assertIn("stateReady = true;", iir_initialize)
        self.assertIn("coefficientsAreStable", iir_filter)
        self.assertIn("abs(reflection) >= 1.0", iir_filter)
        self.assertIn("if (!std::isfinite(sum))", iir_filter)
        self.assertIn(
            "stateReady = false;",
            iir_filter.split("void IIRFilter::process", 1)[1],
        )
        self.assertIn("if (bufferLength == 0)", delay_filter)
        self.assertIn("bufferLength = 0;", delay_filter)
        self.assertIn("bufferOffset = 0;", delay_filter)
        self.assertIn("if (effects == NULL)", vst_filter)
        self.assertIn("if (effect == NULL)\n\t\t\t\tcontinue;", vst_filter)
        self.assertIn("if (skipProcessing)", vst_filter)
        self.assertIn("initialDelay > 0", vst_filter)
        self.assertIn("static_cast<size_t>(i) * maxFrameCount", vst_filter)
        self.assertIn("effects[i]->numInputs() != pluginInputCount", vst_filter)
        self.assertIn("effects[i]->numOutputs() != pluginOutputCount", vst_filter)
        self.assertIn("for (int j = 0; j < pluginInputCount; j++)", vst_filter)
        self.assertIn("for (int j = 0; j < pluginOutputCount; j++)", vst_filter)
        self.assertIn("VSTPluginInstance* firstEffect = NULL;", vst_filter)
        self.assertIn("if (firstEffect != NULL)", vst_filter)
        self.assertIn("MemoryHelper::free(mem);\n\t\tthrow;", vst_filter)
        self.assertIn("effects[0] = firstEffect;\n\tfirstEffect = NULL;", vst_filter)
        ownership_transfer = vst_filter.index("effects[0] = firstEffect;")
        self.assertNotIn("firstEffect->", vst_filter[ownership_transfer:])
        self.assertIn("effects[0]->getInitialDelay()", vst_filter[ownership_transfer:])

    def test_nonfinite_factory_inputs_are_rejected_or_ignored(self) -> None:
        sources = {
            name: read(f"filters/{name}FilterFactory.cpp")
            for name in (
                "BiQuad",
                "Copy",
                "Delay",
                "IIR",
                "OutProcBiquad",
                "OutProcGain",
                "OutputGuard",
                "ParametricEQ",
                "Preamp",
                "ToneGenerator",
            )
        }

        self.assertIn("std::isfinite(preamp_dB)", sources["Preamp"])
        self.assertIn("std::isfinite(gainDb)", sources["OutProcGain"])
        self.assertIn("std::isfinite(ceilingDb)", sources["OutputGuard"])
        self.assertIn("std::isfinite(delay)", sources["Delay"])
        self.assertIn("std::isfinite(summand.factor)", sources["Copy"])
        self.assertIn("std::isfinite(coefficient / a0)", sources["IIR"])
        self.assertIn("BiQuad::isConfigurationValid(", sources["BiQuad"])
        self.assertIn("BiQuad::isConfigurationValid(", sources["OutProcBiquad"])
        self.assertIn("bandwidthOrQOrS <= 0.0", sources["BiQuad"])
        self.assertIn("bandwidthOrQOrS <= 0.0", sources["OutProcBiquad"])
        self.assertIn("std::isfinite(band.gain)", sources["ParametricEQ"])
        self.assertIn("std::isfinite(frequency * twoPi)", sources["ToneGenerator"])
        self.assertIn("std::isfinite(AudioTools::dbToGain(level) * 1.5)", sources["ToneGenerator"])

    def test_native_benchmark_covers_numeric_and_reinitialization_edges(self) -> None:
        benchmark = read("Benchmark/Benchmark.cpp")
        for case in (
            "Preamp NaN rejected",
            "Preamp finite dB overflow rejected",
            "OutProcGain infinity rejected",
            "OutputGuard finite dB overflow rejected",
            "BiQuad finite gain overflow rejected",
            "BiQuad zero frequency rejected",
            "BiQuad negative Q rejected",
            "BiQuad zero bandwidth rejected",
            "BiQuad zero shelf slope rejected",
            "OutProcBiquad finite gain overflow rejected",
            "OutProcBiquad negative frequency rejected",
            "OutProcBiquad zero Q rejected",
            "OutProcBiquad negative bandwidth rejected",
            "OutProcBiquad zero shelf slope rejected",
            "IIR non-finite coefficient rejected",
            "IIR normalized coefficient overflow rejected",
            "IIR unstable denominator rejected",
            "Copy non-finite factor rejected",
            "Copy decibel conversion overflow rejected",
            "Delay infinity rejected",
            "ToneGenerator NaN rejected",
            "ToneGenerator level overflow rejected",
            "ToneGenerator phase overflow rejected",
            "ParametricEQ numeric overflow ignored",
            "Delay failed reinitialization remains dry",
            "OutputGuard sanitizes non-finite samples",
            "ToneGenerator invalid phase increment remains dry",
            "BiQuad negative-Q identity stability",
            "BiQuad Nyquist identity stability",
            "BiQuad valid impulse stability",
            "OutProcBiquad Nyquist remains dry",
            "MemoryHelper allocation-failure checkpoint",
            "FilterEngine failed-reload transaction",
            "FilterEngine locked-file reload timeout",
            "FilterEngine zero-length transition",
            "FilterEngine sample-rate transition bounds",
        ):
            with self.subTest(case=case):
                self.assertIn(case, benchmark)

        output_guard = read("filters/OutputGuardFilter.cpp")
        self.assertIn("if (std::isfinite(sample))", output_guard)
        self.assertIn("sample * currentGain : 0.0", output_guard)
        tone = read("filters/ToneGeneratorFilter.cpp")
        self.assertIn("const double phaseIncrement", tone)
        self.assertIn("!std::isfinite(phaseIncrement)", tone)
        self.assertIn("!std::isfinite(nextPhase)", tone)
        self.assertIn("state = false;", tone)

    def test_unsigned_channel_boundaries_are_checked_before_cast(self) -> None:
        for relative_path in (
            "filters/ConvolutionFilter.cpp",
            "filters/IIRFilter.cpp",
            "filters/OutProcGainFilter.cpp",
            "filters/OutProcBiquadFilter.cpp",
            "filters/OutProcVSTPluginFilter.cpp",
        ):
            source = read(relative_path)
            with self.subTest(path=relative_path):
                guard = source.index(
                    "channelNames.size() > (std::numeric_limits<unsigned>::max)()"
                )
                cast = source.index(
                    "static_cast<unsigned>(channelNames.size())", guard
                )
                self.assertLess(guard, cast)

    def test_biquad_common_layer_checks_nyquist_and_pole_stability(self) -> None:
        header = read("filters/BiQuad.h")
        source = read("filters/BiQuad.cpp")
        outproc = read("filters/OutProcBiquadFilter.cpp")
        self.assertIn("bool isValid() const noexcept", header)
        self.assertIn("freq >= srate * 0.5", source)
        self.assertIn("stabilityMargin", source)
        self.assertIn("1.0 + normalizedB1 + normalizedB2", source)
        self.assertIn("1.0 - normalizedB1 + normalizedB2", source)
        self.assertIn("1.0 - normalizedB2", source)
        self.assertIn("if (!masterBiquad.isValid())", outproc)


if __name__ == "__main__":
    unittest.main()
