/*
    This file is part of EqualizerAPO, a system-wide equalizer.
    Copyright (C) 2012  Jonas Thedering

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with this program; if not, write to the Free Software Foundation, Inc.,
    51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/

#include "stdafx.h"
#ifdef DEBUG
#include <stdlib.h>
#include <crtdbg.h>
#endif
#include <cstdio>
#define _USE_MATH_DEFINES
#include <cmath>
#include <algorithm>
#include <atomic>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <sndfile.h>
#include <tclap/CmdLine.h>
#include <fftw3.h>

#include "../version.h"
#include "../FilterEngine.h"
#include "../libHybridConv-0.1.1/libHybridConv_eapo.h"
#include "../helpers/LogHelper.h"
#include "../helpers/StringHelper.h"
#include "../helpers/PrecisionTimer.h"
#include "../helpers/MemoryHelper.h"
#include "../helpers/FFTWHelper.h"
#include "../helpers/VSTMidiBindingCodec.h"
#include "../helpers/VSTPluginLibrary.h"
#include "../filters/ConvolutionFilter.h"
#include "../filters/GraphicEQFilter.h"
#include "../filters/BiQuadFilterFactory.h"
#include "../filters/CopyFilterFactory.h"
#include "../filters/DelayFilter.h"
#include "../filters/DelayFilterFactory.h"
#include "../filters/IIRFilterFactory.h"
#include "../filters/OutProcBiquadFilter.h"
#include "../filters/OutProcBiquadFilterFactory.h"
#include "../filters/OutProcGainFilterFactory.h"
#include "../filters/OutProcVSTPluginFilterFactory.h"
#include "../filters/OutputGuardFilter.h"
#include "../filters/OutputGuardFilterFactory.h"
#include "../filters/ParametricEQFilterFactory.h"
#include "../filters/PreampFilterFactory.h"
#include "../filters/ToneGeneratorFilterFactory.h"
#include "../filters/ToneGeneratorFilter.h"
#include "../filters/loudnessCorrection/LoudnessCorrectionFilter.h"
#include "../filters/loudnessCorrection/OriginalLoudnessCorrectionFilter.h"
#include "../filters/loudnessCorrection/OriginalLoudnessCorrectionFilterFactory.h"

using namespace std;

class FilterEngineTestAccess
{
public:
	static void addFactory(FilterEngine& engine, IFilterFactory* factory)
	{
		engine.factories.push_back(factory);
	}

	static bool watchesRegistryKey(
		const FilterEngine& engine,
		const std::wstring& key)
	{
		return engine.watchRegistryKeys.find(key) !=
			engine.watchRegistryKeys.end();
	}

	static bool hasPendingConfiguration(const FilterEngine& engine)
	{
		return engine.pendingConfig.load(std::memory_order_acquire) != nullptr;
	}

	static unsigned transitionLength(const FilterEngine& engine)
	{
		return engine.transitionLength;
	}

	static void forceTransitionLength(FilterEngine& engine, unsigned length)
	{
		engine.transitionLength = length;
	}
};

class AllocationFailureFilterFactory final : public IFilterFactory
{
public:
	vector<IFilter*> createFilter(
		const wstring& configPath,
		wstring& command,
		wstring& parameters) override
	{
		if (command == L"ForceAllocationFailure")
		{
			MemoryHelper::allocArray(
				(std::numeric_limits<size_t>::max)(), 2);
			command.clear();
		}
		return vector<IFilter*>();
	}
};

class LoudnessCorrectionFilterTestAccess
{
public:
	static void publishVolumeUpdate(
		LoudnessCorrectionFilter& filter,
		double volume)
	{
		vector<double> scratchGains;
		filter.publishVolumeUpdate(volume, scratchGains);
	}

	static void publishVolumeState(
		LoudnessCorrectionFilter& filter,
		double volumeDb,
		double volumeScalar,
		bool muted)
	{
		vector<double> scratchGains;
		filter.publishVolumeUpdate(
			volumeDb, volumeScalar, muted, scratchGains);
	}

	static double volumeFollowGain(
		LoudnessCorrectionFilter::FilterParameters::VolumeFollowMode mode,
		double volumeDb,
		double volumeScalar,
		bool muted)
	{
		return LoudnessCorrectionFilter::calculateVolumeFollowGain(
			mode, volumeDb, volumeScalar, muted);
	}

	static double currentVolumeFollowGain(
		const LoudnessCorrectionFilter& filter)
	{
		return filter._volumeFollowGainLinear;
	}

	static unsigned volumeFollowRampRemaining(
		const LoudnessCorrectionFilter& filter)
	{
		return filter._volumeFollowRampRemaining;
	}

	static void enableSyntheticAutomaticVolumePublisher(
		LoudnessCorrectionFilter& filter)
	{
		// initialize() uses a manual value to avoid depending on the machine's
		// real endpoint. The rest of this benchmark then emulates the automatic
		// publisher that owns runtime volume updates in production.
		filter._parameters.useManualVolume = false;
	}

	static void beginRuntimeBypass(LoudnessCorrectionFilter& filter)
	{
		filter._recoveryPending.store(false, std::memory_order_release);
		filter._runtimeBypass.store(true, std::memory_order_release);
	}

	static void publishRuntimeRecovery(
		LoudnessCorrectionFilter& filter,
		double volume)
	{
		publishVolumeUpdate(filter, volume);
		filter._recoveryPending.store(true, std::memory_order_release);
	}

	static void calculateTransfer(
		const LoudnessCorrectionFilter& filter,
		double volume,
		vector<double>& gains,
		double& outputGainLinear)
	{
		filter.calculateBandGains(volume, gains, outputGainLinear);
	}

	static void calculateConfiguredTransfer(
		const LoudnessCorrectionFilter& filter,
		double volume,
		vector<double>& gains,
		double& outputGainLinear)
	{
		if (filter._parameters.engine ==
			LoudnessCorrectionFilter::FilterParameters::ENGINE_FAST)
		{
			filter.calculateFastShelfGains(volume, gains, outputGainLinear);
		}
		else
		{
			filter.calculateBandGains(volume, gains, outputGainLinear);
		}
	}

	static double rawCorrectionResponseDb(
		const LoudnessCorrectionFilter& filter,
		const vector<double>& gains,
		double frequency)
	{
		double response = 0.0;
		for (size_t band = 0; band < filter._activeBandCount; ++band)
			response += filter.biquadResponseDb(band, gains[band], frequency);
		return response;
	}

	static double aggregateRawCorrectionResponseDb(
		const LoudnessCorrectionFilter& filter,
		const vector<double>& gains,
		double frequency)
	{
		LoudnessCorrectionFilter::BiquadCoeffs coefficients[
			LoudnessCorrectionFilter::NUM_BANDS];
		for (size_t band = 0; band < filter._activeBandCount; ++band)
			filter.computeBiquadCoeffs(band, gains[band], coefficients[band]);
		return filter.bandResponseDb(
			coefficients, filter._activeBandCount, frequency);
	}

	static double guardedResponseDb(
		const LoudnessCorrectionFilter& filter,
		const vector<double>& gains,
		double outputGainLinear,
		double frequency)
	{
		return filter.guardedResponseDb(gains, outputGainLinear, frequency);
	}

	static double maximumGuardedResponseDb(
		const LoudnessCorrectionFilter& filter,
		const vector<double>& gains,
		double outputGainLinear)
	{
		return filter.findMaximumResponseDb(gains, outputGainLinear, true);
	}

	static double maximumRawCorrectionResponseDb(
		const LoudnessCorrectionFilter& filter,
		const vector<double>& gains)
	{
		return filter.findMaximumResponseDb(gains, 1.0, false);
	}

	static size_t activeBandCount(const LoudnessCorrectionFilter& filter)
	{
		return filter._activeBandCount;
	}

	static unsigned warmupLength(const LoudnessCorrectionFilter& filter)
	{
		return filter._warmupLength;
	}

	static unsigned crossoverPrewarmLength(const LoudnessCorrectionFilter& filter)
	{
		return filter._crossoverPrewarmLength;
	}

	static bool crossoverPrewarmActive(const LoudnessCorrectionFilter& filter)
	{
		return filter._crossoverPrewarmActive;
	}

	static bool crossoverDomainReady(const LoudnessCorrectionFilter& filter)
	{
		return !filter._crossoverPrewarmActive &&
			!filter._crossoverHandoffActive &&
			filter._crossoverDomainChannelCount == filter._channelCount;
	}

	static bool runtimeBypass(const LoudnessCorrectionFilter& filter)
	{
		return filter._runtimeBypass.load(std::memory_order_acquire);
	}

	static double maximumCrossoverHandoffStep(
		const LoudnessCorrectionFilter& filter)
	{
		return filter._maximumCrossoverHandoffStep;
	}

	static bool isSafeCrossoverHandoff(
		double previousRaw,
		double currentRaw,
		double previousIdentity,
		double currentIdentity,
		double& handoffStep,
		double& naturalStep)
	{
		return LoudnessCorrectionFilter::isSafeCrossoverHandoff(
			previousRaw,
			currentRaw,
			previousIdentity,
			currentIdentity,
			handoffStep,
			naturalStep);
	}

	static bool crossoverChannelInDomain(
		const LoudnessCorrectionFilter& filter,
		size_t channel)
	{
		return channel < filter._crossoverDomainActive.size() &&
			filter._crossoverDomainActive[channel] != 0;
	}

	static complex<double> crossoverIdentityResponse(
		const LoudnessCorrectionFilter& filter,
		double frequency)
	{
		complex<double> lowpass(1.0, 0.0);
		complex<double> highpass(1.0, 0.0);
		for (size_t section = 0;
			section < LoudnessCorrectionFilter::CROSSOVER_SECTION_COUNT;
			++section)
		{
			lowpass *= filter.biquadResponse(
				filter._lowpassCoeffs[section], frequency);
			highpass *= filter.biquadResponse(
				filter._highpassCoeffs[section], frequency);
		}
		return lowpass + highpass;
	}

	static bool settledOnNonIdentityBank(
		const LoudnessCorrectionFilter& filter)
	{
		return crossoverDomainReady(filter) && !filter._warmupActive &&
			!filter._crossfadeActive &&
			!filter._runtimeBypass.load(std::memory_order_acquire) &&
			!filter._bankIdentity[filter._activeBankIndex];
	}

	static bool warmupActive(const LoudnessCorrectionFilter& filter)
	{
		return filter._warmupActive;
	}

	static unsigned warmupPosition(const LoudnessCorrectionFilter& filter)
	{
		return filter._warmupPosition;
	}

	static bool crossfadeActive(const LoudnessCorrectionFilter& filter)
	{
		return filter._crossfadeActive;
	}

	static unsigned crossfadePosition(const LoudnessCorrectionFilter& filter)
	{
		return filter._crossfadePosition;
	}

	static bool bypassFadeActive(const LoudnessCorrectionFilter& filter)
	{
		return filter._bypassFadeActive;
	}

	static unsigned bypassFadeLength(const LoudnessCorrectionFilter& filter)
	{
		return filter._bypassFadeLength;
	}

	static bool holdingBypassOnIdentityBank(
		const LoudnessCorrectionFilter& filter)
	{
		return crossoverDomainReady(filter) && !filter._warmupActive &&
			!filter._crossfadeActive && !filter._bypassFadeActive &&
			filter._runtimeBypass.load(std::memory_order_acquire) &&
			!filter._recoveryPending.load(std::memory_order_acquire);
	}

	static bool settledOnIdentityBank(
		const LoudnessCorrectionFilter& filter)
	{
		return crossoverDomainReady(filter) && !filter._warmupActive &&
			!filter._crossfadeActive &&
			!filter._runtimeBypass.load(std::memory_order_acquire) &&
			filter._bankIdentity[filter._activeBankIndex];
	}

	static unsigned crossfadeLength(const LoudnessCorrectionFilter& filter)
	{
		return filter._crossfadeLength;
	}

	static bool canTrackAutomaticVolume(
		const LoudnessCorrectionFilter& filter)
	{
		return filter.canTrackAutomaticVolume();
	}

	static bool inspectCrossover(
		const LoudnessCorrectionFilter& filter,
		double& maximumPoleRadius,
		double& maximumComplementError)
	{
		maximumPoleRadius = 0.0;
		maximumComplementError = 0.0;
		for (size_t section = 0;
			section < LoudnessCorrectionFilter::CROSSOVER_SECTION_COUNT;
			++section)
		{
			const LoudnessCorrectionFilter::BiquadCoeffs* coefficients[] = {
				&filter._lowpassCoeffs[section],
				&filter._highpassCoeffs[section]
			};
			for (const LoudnessCorrectionFilter::BiquadCoeffs* coeffs : coefficients)
			{
				if (!std::isfinite(coeffs->b0) || !std::isfinite(coeffs->b1) ||
					!std::isfinite(coeffs->b2) || !std::isfinite(coeffs->a1) ||
					!std::isfinite(coeffs->a2))
				{
					return false;
				}

				std::complex<double> discriminant(
					coeffs->a1 * coeffs->a1 - 4.0 * coeffs->a2,
					0.0);
				std::complex<double> squareRoot = std::sqrt(discriminant);
				std::complex<double> pole1 = (-coeffs->a1 + squareRoot) / 2.0;
				std::complex<double> pole2 = (-coeffs->a1 - squareRoot) / 2.0;
				maximumPoleRadius = (std::max)(maximumPoleRadius,
					(std::max)(std::abs(pole1), std::abs(pole2)));
			}
		}

		const double frequencies[] = { 1.0, 5.0, 10.0, 15.0, 19.0,
			20.0, 25.0, 31.5, 100.0 };
		for (double frequency : frequencies)
		{
			std::complex<double> lowpass(1.0, 0.0);
			std::complex<double> highpass(1.0, 0.0);
			for (size_t section = 0;
				section < LoudnessCorrectionFilter::CROSSOVER_SECTION_COUNT;
				++section)
			{
				lowpass *= filter.biquadResponse(
					filter._lowpassCoeffs[section], frequency);
				highpass *= filter.biquadResponse(
					filter._highpassCoeffs[section], frequency);
			}
			maximumComplementError = (std::max)(maximumComplementError,
				std::abs(std::abs(lowpass + highpass) - 1.0));
		}
		return true;
	}

	static void resetCrossoverHandoffRegistry()
	{
		LoudnessCorrectionFilter::resetCrossoverHandoffRegistry();
	}
};

class OriginalLoudnessCorrectionFilterTestAccess
{
public:
	static void calculateTransfer(
		const OriginalLoudnessCorrectionFilter::FilterParameters& parameters,
		double volumeDb,
		double& lowShelfGainDb,
		double& highShelfGainDb,
		double& outputGainLinear,
		bool& identity)
	{
		const OriginalLoudnessCorrectionFilter::Transfer transfer =
			OriginalLoudnessCorrectionFilter::calculateTransfer(
				parameters, volumeDb);
		lowShelfGainDb = transfer.lowShelfGainDb;
		highShelfGainDb = transfer.highShelfGainDb;
		outputGainLinear = transfer.outputGainLinear;
		identity = transfer.identity;
	}

	static void enableWithoutTracking(OriginalLoudnessCorrectionFilter& filter)
	{
		filter._parameters.state = true;
	}

	static void publishVolumeUpdate(
		OriginalLoudnessCorrectionFilter& filter,
		double volumeDb)
	{
		filter.publishVolumeUpdate(volumeDb);
	}

	static bool settledOnNonIdentityBank(
		const OriginalLoudnessCorrectionFilter& filter)
	{
		return !filter._warmupActive && !filter._crossfadeActive &&
			!filter._bankSnapshots[filter._activeBankIndex].identity;
	}

	static double maximumSnapshotPoleRadius(
		const OriginalLoudnessCorrectionFilter::FilterParameters& parameters,
		double volumeDb,
		double sampleRate,
		bool& highShelfIdentity)
	{
		const OriginalLoudnessCorrectionFilter::CoefficientSnapshot snapshot =
			OriginalLoudnessCorrectionFilter::makeSnapshot(
				OriginalLoudnessCorrectionFilter::calculateTransfer(
					parameters, volumeDb),
				sampleRate);
		auto maximumRadius = [](double a1, double a2)
		{
			const std::complex<double> root = std::sqrt(
				std::complex<double>(a1 * a1 - 4.0 * a2, 0.0));
			return (std::max)(
				std::abs((-a1 + root) / 2.0),
				std::abs((-a1 - root) / 2.0));
		};
		highShelfIdentity = snapshot.highShelfA0 == 1.0;
		for (size_t index = 0; index < 4; ++index)
			highShelfIdentity = highShelfIdentity &&
				snapshot.highShelf[index] == 0.0;
		return (std::max)(
			maximumRadius(snapshot.lowShelf[2], snapshot.lowShelf[3]),
			maximumRadius(snapshot.highShelf[2], snapshot.highShelf[3]));
	}

	static bool concurrentPublicationIsCoherent(
		OriginalLoudnessCorrectionFilter& filter)
	{
		OriginalLoudnessCorrectionFilter::CoefficientSnapshot signatures[2] = {};
		for (size_t signature = 0; signature < 2; ++signature)
		{
			const double base = signature == 0 ? 10.0 : 100.0;
			for (size_t index = 0; index < 4; ++index)
			{
				signatures[signature].lowShelf[index] = base + index;
				signatures[signature].highShelf[index] = base + 10.0 + index;
			}
			signatures[signature].lowShelfA0 = base + 20.0;
			signatures[signature].highShelfA0 = base + 21.0;
			signatures[signature].outputGainLinear = base + 22.0;
			signatures[signature].identity = signature != 0;
		}

		auto matches = [](
			const OriginalLoudnessCorrectionFilter::CoefficientSnapshot& actual,
			const OriginalLoudnessCorrectionFilter::CoefficientSnapshot& expected)
		{
			if (actual.lowShelfA0 != expected.lowShelfA0 ||
				actual.highShelfA0 != expected.highShelfA0 ||
				actual.outputGainLinear != expected.outputGainLinear ||
				actual.identity != expected.identity)
			{
				return false;
			}
			for (size_t index = 0; index < 4; ++index)
			{
				if (actual.lowShelf[index] != expected.lowShelf[index] ||
					actual.highShelf[index] != expected.highShelf[index])
				{
					return false;
				}
			}
			return true;
		};

		std::atomic<bool> start(false);
		std::atomic<bool> done(false);
		std::thread writer([&]()
		{
			while (!start.load(std::memory_order_acquire))
				std::this_thread::yield();
			for (size_t iteration = 0; iteration < 100000; ++iteration)
				filter.publishSnapshot(signatures[iteration & 1]);
			done.store(true, std::memory_order_release);
		});

		start.store(true, std::memory_order_release);
		bool coherent = true;
		size_t observed = 0;
		do
		{
			OriginalLoudnessCorrectionFilter::CoefficientSnapshot
				observedSnapshot = {};
			std::uint64_t sequence = 0;
			if (filter.readPublishedSnapshot(observedSnapshot, sequence))
			{
				coherent = coherent &&
					(matches(observedSnapshot, signatures[0]) ||
						matches(observedSnapshot, signatures[1]));
				++observed;
			}
		} while (!done.load(std::memory_order_acquire));
		writer.join();
		return coherent && observed != 0;
	}
};

namespace
{
	template<typename Sample>
	bool getSafeSampleCount(
		unsigned channelCount,
		unsigned frameCount,
		size_t& sampleCount) noexcept
	{
		const size_t maximumSize = (std::numeric_limits<size_t>::max)();
		if (frameCount != 0 &&
			static_cast<size_t>(channelCount) > maximumSize / frameCount)
		{
			return false;
		}
		sampleCount = static_cast<size_t>(channelCount) * frameCount;
		return sampleCount <= maximumSize / sizeof(Sample);
	}

	int nextPowerOfTwo(int value)
	{
		int result = 1;
		while (result < value)
			result <<= 1;
		return result;
	}

	void referenceConvolutionFft(
		const vector<double>& input,
		const vector<double>& impulse,
		vector<double>& output)
	{
		const int resultLength =
			static_cast<int>(input.size() + impulse.size() - 1);
		const int fftLength = nextPowerOfTwo(resultLength);

		double* xTime = static_cast<double*>(
			fftw_malloc(sizeof(double) * fftLength));
		double* hTime = static_cast<double*>(
			fftw_malloc(sizeof(double) * fftLength));
		fftw_complex* xFreq = static_cast<fftw_complex*>(
			fftw_malloc(sizeof(fftw_complex) * (fftLength / 2 + 1)));
		fftw_complex* hFreq = static_cast<fftw_complex*>(
			fftw_malloc(sizeof(fftw_complex) * (fftLength / 2 + 1)));

		memset(xTime, 0, sizeof(double) * fftLength);
		memset(hTime, 0, sizeof(double) * fftLength);
		memcpy(xTime, input.data(), sizeof(double) * input.size());
		memcpy(hTime, impulse.data(), sizeof(double) * impulse.size());

		fftw_plan xPlan = fftw_plan_dft_r2c_1d(
			fftLength, xTime, xFreq, FFTW_ESTIMATE);
		fftw_plan hPlan = fftw_plan_dft_r2c_1d(
			fftLength, hTime, hFreq, FFTW_ESTIMATE);
		fftw_execute(xPlan);
		fftw_execute(hPlan);

		for (int index = 0; index < fftLength / 2 + 1; ++index)
		{
			const double real =
				xFreq[index][0] * hFreq[index][0] -
				xFreq[index][1] * hFreq[index][1];
			const double imaginary =
				xFreq[index][0] * hFreq[index][1] +
				xFreq[index][1] * hFreq[index][0];
			xFreq[index][0] = real;
			xFreq[index][1] = imaginary;
		}

		fftw_plan yPlan = fftw_plan_dft_c2r_1d(
			fftLength, xFreq, xTime, FFTW_ESTIMATE);
		fftw_execute(yPlan);

		output.resize(input.size());
		for (size_t index = 0; index < output.size(); ++index)
			output[index] = xTime[index] / fftLength;

		fftw_destroy_plan(yPlan);
		fftw_destroy_plan(hPlan);
		fftw_destroy_plan(xPlan);
		fftw_free(hFreq);
		fftw_free(xFreq);
		fftw_free(hTime);
		fftw_free(xTime);
	}

	double deterministicNoise(unsigned& state)
	{
		state = state * 1664525u + 1013904223u;
		return ((state >> 8) / 16777216.0) * 2.0 - 1.0;
	}

	bool runConvolutionSelfTestCase(
		int sampleRate,
		int frameLength,
		int impulseLength,
		int blocks)
	{
		const int inputLength = frameLength * blocks;
		vector<double> input(inputLength);
		vector<double> impulse(impulseLength);
		vector<double> actual(inputLength);
		vector<double> reference;
		vector<double> block(frameLength);

		unsigned state =
			0x12345678u ^ static_cast<unsigned>(sampleRate) ^
			static_cast<unsigned>(frameLength);
		for (int index = 0; index < inputLength; ++index)
		{
			const double time = static_cast<double>(index) / sampleRate;
			input[index] = 0.17 * sin(2.0 * M_PI * 997.0 * time) +
				0.11 * sin(2.0 * M_PI * 1234.5 * time) +
				0.03 * deterministicNoise(state);
		}

		for (int index = 0; index < impulseLength; ++index)
		{
			const double decay =
				exp(-static_cast<double>(index) / (0.065 * sampleRate));
			impulse[index] = decay *
				(0.012 * sin(0.013 * index) +
					0.006 * deterministicNoise(state));
		}

		if (!impulse.empty())
		{
			impulse[0] += 0.55;
			for (int index = frameLength - 1;
				index < impulseLength;
				index += frameLength)
			{
				impulse[index] += 0.04 *
					((index / frameLength) % 2 == 0 ? 1.0 : -1.0);
			}
			for (int index = frameLength;
				index < impulseLength;
				index += frameLength)
			{
				impulse[index] += 0.035 *
					((index / frameLength) % 2 == 0 ? -1.0 : 1.0);
			}
		}

		referenceConvolutionFft(input, impulse, reference);

		HConvSingle filter = {};
		if (!hcInitSingle(
				&filter, impulse.data(), impulseLength, frameLength, 1))
		{
			fprintf(stderr,
				"HybridConv initialization failed for flen=%d hlen=%d.\n",
				frameLength, impulseLength);
			return false;
		}
		for (int blockIndex = 0; blockIndex < blocks; ++blockIndex)
		{
			hcPutSingle(
				&filter, input.data() + static_cast<size_t>(blockIndex) * frameLength);
			hcProcessSingle(&filter);
			hcGetSingle(&filter, block.data());
			memcpy(
				actual.data() + static_cast<size_t>(blockIndex) * frameLength,
				block.data(),
				sizeof(double) * frameLength);
		}
		hcCloseSingle(&filter);

		double maxAbsError = 0.0;
		double rmsError = 0.0;
		double maxReference = 0.0;
		bool finite = true;
		int maxIndex = 0;
		for (int index = 0; index < inputLength; ++index)
		{
			finite = finite && std::isfinite(actual[index]) &&
				std::isfinite(reference[index]);
			const double error = fabs(actual[index] - reference[index]);
			if (error > maxAbsError)
			{
				maxAbsError = error;
				maxIndex = index;
			}
			rmsError += error * error;
			maxReference = max(maxReference, fabs(reference[index]));
		}
		rmsError = sqrt(rmsError / inputLength);
		const double relativeError = maxReference > 0.0 ?
			maxAbsError / maxReference : maxAbsError;
		const bool passed = finite &&
			(maxAbsError < 1e-8 || relativeError < 1e-8);

		printf(
			"%s sr=%d flen=%d hlen=%d blocks=%d max=%0.12g rel=%0.12g rms=%0.12g idx=%d\n",
			passed ? "PASS" : "FAIL",
			sampleRate,
			frameLength,
			impulseLength,
			blocks,
			maxAbsError,
			relativeError,
			rmsError,
			maxIndex);

		return passed;
	}

	bool writeAsyncConvolutionTestImpulse(std::wstring& filename)
	{
		wchar_t tempDirectory[MAX_PATH] = {};
		wchar_t tempFilename[MAX_PATH] = {};
		if (GetTempPathW(MAX_PATH, tempDirectory) == 0 ||
			GetTempFileNameW(tempDirectory, L"EAP", 0, tempFilename) == 0)
		{
			return false;
		}

		SF_INFO info = {};
		info.samplerate = 48000;
		info.channels = 1;
		info.format = SF_FORMAT_WAV | SF_FORMAT_DOUBLE;
		SNDFILE* file = sf_wchar_open(tempFilename, SFM_WRITE, &info);
		if (file == NULL)
		{
			DeleteFileW(tempFilename);
			return false;
		}

		const double impulse[] = { 0.5 };
		const sf_count_t written = sf_writef_double(file, impulse, 1);
		const int closeResult = sf_close(file);
		if (written != 1 || closeResult != 0)
		{
			DeleteFileW(tempFilename);
			return false;
		}

		filename = tempFilename;
		return true;
	}

	bool runAsyncConvolutionFilterCase(
		const char* name,
		ConvolutionFilter& filter,
		double expectedScale,
		bool inPlace,
		bool verifyCallbackBound)
	{
		const unsigned sampleRate = 48000;
		const unsigned maximumFrameCount = 1024;
		const unsigned actualFrameCount = 480;
		const unsigned channelCount = 2;
		vector<wstring> channelNames = { L"L", L"R" };
		filter.initialize(
			static_cast<float>(sampleRate), maximumFrameCount, channelNames);

		vector<vector<double>> input(
			channelCount, vector<double>(actualFrameCount));
		vector<vector<double>> output(
			channelCount, vector<double>(actualFrameCount));
		vector<double*> inputChannels(channelCount);
		vector<double*> outputChannels(channelCount);
		auto prepareBlock = [&]()
		{
			for (unsigned channel = 0; channel < channelCount; ++channel)
			{
				const double value = channel == 0 ? 0.125 : -0.25;
				std::fill(input[channel].begin(), input[channel].end(), value);
				std::fill(output[channel].begin(), output[channel].end(), 123.0);
				inputChannels[channel] = input[channel].data();
				outputChannels[channel] = inPlace ?
					input[channel].data() : output[channel].data();
			}
		};
		auto maximumError = [&](double scale)
		{
			double error = 0.0;
			for (unsigned channel = 0; channel < channelCount; ++channel)
			{
				const double expected =
					(channel == 0 ? 0.125 : -0.25) * scale;
				for (double sample : inPlace ? input[channel] : output[channel])
				{
					if (!std::isfinite(sample))
						return std::numeric_limits<double>::infinity();
					error = (std::max)(error, std::abs(sample - expected));
				}
			}
			return error;
		};

		bool passed = true;
		if (verifyCallbackBound)
		{
			std::atomic<bool> plannerHeld(false);
			std::thread plannerBlocker([&]()
			{
				FFTWPlannerGuard plannerGuard;
				plannerHeld.store(true, std::memory_order_release);
				Sleep(750);
			});
			while (!plannerHeld.load(std::memory_order_acquire))
				std::this_thread::yield();

			prepareBlock();
			PrecisionTimer timer;
			timer.start();
			filter.process(
				outputChannels.data(), inputChannels.data(), actualFrameCount);
			const double firstCallbackMilliseconds = timer.stop() * 1000.0;
			passed = maximumError(1.0) < 1.0e-12 && passed;
			Sleep(25);
			prepareBlock();
			timer.start();
			filter.process(
				outputChannels.data(), inputChannels.data(), actualFrameCount);
			const double secondCallbackMilliseconds = timer.stop() * 1000.0;
			const double callbackMilliseconds = (std::max)(
				firstCallbackMilliseconds, secondCallbackMilliseconds);
			plannerBlocker.join();
			const bool bounded = callbackMilliseconds < 100.0;
			printf("%s callback while rebuild blocked: %.3f ms (%s)\n",
				name, callbackMilliseconds, bounded ? "bounded" : "BLOCKED");
			passed = bounded && maximumError(1.0) < 1.0e-12 && passed;
		}
		else
		{
			prepareBlock();
			filter.process(
				outputChannels.data(), inputChannels.data(), actualFrameCount);
			passed = maximumError(1.0) < 1.0e-12 && passed;
		}

		bool reachedWet = false;
		bool sawValidTransition = false;
		for (unsigned attempt = 0; attempt < 500 && !reachedWet; ++attempt)
		{
			prepareBlock();
			filter.process(
				outputChannels.data(), inputChannels.data(), actualFrameCount);
			const double dryError = maximumError(1.0);
			const double wetError = maximumError(expectedScale);
			reachedWet = wetError < 1.0e-5;
			if (!reachedWet && dryError > 1.0e-8 && !sawValidTransition)
			{
				double transitionError = 0.0;
				for (unsigned channel = 0; channel < channelCount; ++channel)
				{
					const double dry = channel == 0 ? 0.125 : -0.25;
					const vector<double>& rendered =
						inPlace ? input[channel] : output[channel];
					for (unsigned frame = 0; frame < actualFrameCount; ++frame)
					{
						const double wet = static_cast<double>(frame + 1) /
							actualFrameCount;
						const double expected = dry *
							(1.0 + (expectedScale - 1.0) * wet);
						transitionError = (std::max)(transitionError,
							std::abs(rendered[frame] - expected));
					}
				}
				sawValidTransition = transitionError < 1.0e-5;
			}
			if (!reachedWet)
				Sleep(2);
		}

		passed = reachedWet && sawValidTransition && passed;
		printf("%s: %s\n", name, passed ? "PASS" : "FAIL");
		return passed;
	}

	class NonFiniteImpulseConvolutionFilter : public ConvolutionFilter
	{
	public:
		NonFiniteImpulseConvolutionFilter()
			: ConvolutionFilter(L"")
		{
		}

	protected:
		bool prepareImpulseResponse(
			std::vector<std::vector<double>>& impulseResponses) override
		{
			impulseResponses.push_back(std::vector<double>(
				1, std::numeric_limits<double>::quiet_NaN()));
			return true;
		}
	};

	bool runFailClosedConvolutionCase(
		const char* name,
		ConvolutionFilter& filter,
		bool inPlace)
	{
		const unsigned frameCount = 64;
		const unsigned channelCount = 2;
		vector<wstring> channelNames = { L"L", L"R" };
		filter.initialize(48000.0f, frameCount, channelNames);

		vector<vector<double>> input(
			channelCount, vector<double>(frameCount));
		vector<vector<double>> output(
			channelCount, vector<double>(frameCount, 123.0));
		vector<double*> inputChannels(channelCount);
		vector<double*> outputChannels(channelCount);
		for (unsigned channel = 0; channel < channelCount; ++channel)
		{
			for (unsigned frame = 0; frame < frameCount; ++frame)
			{
				input[channel][frame] =
					(channel == 0 ? 0.125 : -0.25) + frame * 1.0e-4;
			}
			inputChannels[channel] = input[channel].data();
			outputChannels[channel] = inPlace ?
				input[channel].data() : output[channel].data();
		}

		filter.process(
			outputChannels.data(), inputChannels.data(), frameCount);
		bool passed = true;
		for (unsigned channel = 0; channel < channelCount; ++channel)
		{
			const vector<double>& rendered = inPlace ?
				input[channel] : output[channel];
			for (unsigned frame = 0; frame < frameCount; ++frame)
			{
				const double expected =
					(channel == 0 ? 0.125 : -0.25) + frame * 1.0e-4;
				passed = passed && std::isfinite(rendered[frame]) &&
					rendered[frame] == expected;
			}
		}

		printf("%s: %s\n", name, passed ? "PASS" : "FAIL");
		return passed;
	}

	bool runInvalidConvolutionInputTests()
	{
		bool passed = true;
		{
			const vector<FilterNode> nodes = {
				FilterNode(0.0, 0.0), FilterNode(100.0, 6.0)
			};
			GraphicEQFilter filter(nodes, 1024);
			passed = runFailClosedConvolutionCase(
				"GraphicEQ zero-frequency fails dry out-of-place",
				filter, false) && passed;
		}
		{
			const vector<FilterNode> nodes = { FilterNode(-1.0, 6.0) };
			GraphicEQFilter filter(nodes, 1024);
			passed = runFailClosedConvolutionCase(
				"GraphicEQ negative-frequency fails dry in-place",
				filter, true) && passed;
		}
		{
			const vector<FilterNode> nodes = {
				FilterNode(100.0, 0.0), FilterNode(100.0, 6.0)
			};
			GraphicEQFilter filter(nodes, 1024);
			passed = runFailClosedConvolutionCase(
				"GraphicEQ duplicate-frequency fails dry out-of-place",
				filter, false) && passed;
		}
		{
			const vector<FilterNode> nodes = { FilterNode(
				std::numeric_limits<double>::infinity(), 6.0) };
			GraphicEQFilter filter(nodes, 1024);
			passed = runFailClosedConvolutionCase(
				"GraphicEQ non-finite frequency fails dry out-of-place",
				filter, false) && passed;
		}
		{
			const vector<FilterNode> nodes = { FilterNode(
				100.0, std::numeric_limits<double>::infinity()) };
			GraphicEQFilter filter(nodes, 1024);
			passed = runFailClosedConvolutionCase(
				"GraphicEQ non-finite gain fails dry in-place",
				filter, true) && passed;
		}
		{
			NonFiniteImpulseConvolutionFilter filter;
			passed = runFailClosedConvolutionCase(
				"Convolution non-finite impulse fails dry",
				filter, false) && passed;
		}
		return passed;
	}

	bool expectNumericFactoryRejection(
		const char* name,
		IFilterFactory& factory,
		const wchar_t* commandValue,
		const wchar_t* parametersValue)
	{
		wstring command(commandValue);
		wstring parameters(parametersValue);
		vector<IFilter*> filters = factory.createFilter(L"", command, parameters);
		const bool passed = filters.empty();
		for (IFilter* filter : filters)
		{
			filter->~IFilter();
			MemoryHelper::free(filter);
		}
		printf("%s: %s\n", name, passed ? "PASS" : "FAIL");
		return passed;
	}

	bool runInvalidNumericFactoryTests()
	{
		bool passed = true;
		PreampFilterFactory preampFactory;
		passed = expectNumericFactoryRejection(
			"Preamp NaN rejected", preampFactory,
			L"Preamp", L"nan dB") && passed;
		passed = expectNumericFactoryRejection(
			"Preamp finite dB overflow rejected", preampFactory,
			L"Preamp", L"1e308 dB") && passed;

		OutProcGainFilterFactory outProcGainFactory;
		passed = expectNumericFactoryRejection(
			"OutProcGain infinity rejected", outProcGainFactory,
			L"OutProcGain", L"inf dB") && passed;

		OutputGuardFilterFactory outputGuardFactory;
		passed = expectNumericFactoryRejection(
			"OutputGuard finite dB overflow rejected", outputGuardFactory,
			L"OutputGuard", L"1e308 dB") && passed;

		BiQuadFilterFactory biquadFactory;
		passed = expectNumericFactoryRejection(
			"BiQuad finite gain overflow rejected", biquadFactory,
			L"Filter 1", L"ON PK Fc 1000 Hz Gain 1e308 dB Q 1") && passed;
		passed = expectNumericFactoryRejection(
			"BiQuad zero frequency rejected", biquadFactory,
			L"Filter 1", L"ON PK Fc 0 Hz Gain 0 dB Q 1") && passed;
		passed = expectNumericFactoryRejection(
			"BiQuad negative Q rejected", biquadFactory,
			L"Filter 1", L"ON LPQ Fc 1000 Hz Q -1") && passed;
		passed = expectNumericFactoryRejection(
			"BiQuad zero bandwidth rejected", biquadFactory,
			L"Filter 1", L"ON PK Fc 1000 Hz Gain 0 dB BW Oct 0") && passed;
		passed = expectNumericFactoryRejection(
			"BiQuad zero shelf slope rejected", biquadFactory,
			L"Filter 1", L"ON LS 0 dB Fc 1000 Hz Gain 1 dB") && passed;

		OutProcBiquadFilterFactory outProcBiquadFactory;
		passed = expectNumericFactoryRejection(
			"OutProcBiquad finite gain overflow rejected", outProcBiquadFactory,
			L"OutProcBiquad", L"ON PK Fc 1000 Hz Gain 1e308 dB Q 1") && passed;
		passed = expectNumericFactoryRejection(
			"OutProcBiquad negative frequency rejected", outProcBiquadFactory,
			L"OutProcBiquad", L"ON PK Fc -1 Hz Gain 0 dB Q 1") && passed;
		passed = expectNumericFactoryRejection(
			"OutProcBiquad zero Q rejected", outProcBiquadFactory,
			L"OutProcBiquad", L"ON LPQ Fc 1000 Hz Q 0") && passed;
		passed = expectNumericFactoryRejection(
			"OutProcBiquad negative bandwidth rejected", outProcBiquadFactory,
			L"OutProcBiquad", L"ON PK Fc 1000 Hz Gain 0 dB BW Oct -1") && passed;
		passed = expectNumericFactoryRejection(
			"OutProcBiquad zero shelf slope rejected", outProcBiquadFactory,
			L"OutProcBiquad", L"ON LS 0 dB Fc 1000 Hz Gain 1 dB") && passed;

		IIRFilterFactory iirFactory;
		passed = expectNumericFactoryRejection(
			"IIR non-finite coefficient rejected", iirFactory,
			L"Filter 1", L"ON IIR Order 1 Coefficients 1 0 1e999 0") && passed;
		passed = expectNumericFactoryRejection(
			"IIR normalized coefficient overflow rejected", iirFactory,
			L"Filter 1", L"ON IIR Order 1 Coefficients 1e308 0 1e-308 0") && passed;
		passed = expectNumericFactoryRejection(
			"IIR unstable denominator rejected", iirFactory,
			L"Filter 1", L"ON IIR Order 1 Coefficients 1 0 1 -2") && passed;

		CopyFilterFactory copyFactory;
		passed = expectNumericFactoryRejection(
			"Copy non-finite factor rejected", copyFactory,
			L"Copy", L"L=1e999*R") && passed;
		passed = expectNumericFactoryRejection(
			"Copy decibel conversion overflow rejected", copyFactory,
			L"Copy", L"L=1e308dB*R") && passed;

		DelayFilterFactory delayFactory;
		passed = expectNumericFactoryRejection(
			"Delay infinity rejected", delayFactory,
			L"Delay", L"inf samples") && passed;

		ToneGeneratorFilterFactory toneFactory;
		passed = expectNumericFactoryRejection(
			"ToneGenerator NaN rejected", toneFactory,
			L"ToneGenerator", L"Frequency nan Hz") && passed;
		passed = expectNumericFactoryRejection(
			"ToneGenerator level overflow rejected", toneFactory,
			L"ToneGenerator", L"Level 1e308 dB") && passed;
		passed = expectNumericFactoryRejection(
			"ToneGenerator phase overflow rejected", toneFactory,
			L"ToneGenerator", L"Frequency 1e308 Hz") && passed;

		const vector<ParametricEQFilter::Band> bands =
			ParametricEQFilterFactory::parseBands(
				L"ON PK Fc 1000 Hz Gain 1e999 dB Q 1");
		const bool parametricPassed = bands.empty();
		printf("ParametricEQ numeric overflow ignored: %s\n",
			parametricPassed ? "PASS" : "FAIL");
		passed = parametricPassed && passed;

		{
			DelayFilter delay(10.0, true);
			vector<wstring> channelNames = { L"L" };
			delay.initialize(48000.0f, 8, channelNames);
			delay.initialize(
				std::numeric_limits<float>::infinity(), 8, channelNames);
			vector<double> input(8);
			vector<double> output(8, 123.0);
			for (unsigned frame = 0; frame < input.size(); ++frame)
				input[frame] = 0.125 + frame * 1.0e-3;
			double* inputChannel = input.data();
			double* outputChannel = output.data();
			delay.process(&outputChannel, &inputChannel,
				static_cast<unsigned>(input.size()));
			const bool delayReinitializePassed = input == output;
			printf("Delay failed reinitialization remains dry: %s\n",
				delayReinitializePassed ? "PASS" : "FAIL");
			passed = delayReinitializePassed && passed;
		}

		{
			OutputGuardFilter guard(-1.0);
			vector<wstring> channelNames = { L"L" };
			guard.initialize(48000.0f, 4, channelNames);
			vector<double> input = {
				std::numeric_limits<double>::quiet_NaN(),
				std::numeric_limits<double>::infinity(),
				-std::numeric_limits<double>::infinity(),
				0.25
			};
			vector<double> output(4, 123.0);
			double* inputChannel = input.data();
			double* outputChannel = output.data();
			guard.process(&outputChannel, &inputChannel, 4);
			const bool guardPassed = output[0] == 0.0 &&
				output[1] == 0.0 && output[2] == 0.0 &&
				std::isfinite(output[3]);
			printf("OutputGuard sanitizes non-finite samples: %s\n",
				guardPassed ? "PASS" : "FAIL");
			passed = guardPassed && passed;
		}

		{
			ToneGeneratorFilter tone(
				true,
				ToneGeneratorFilter::SINE,
				1.0e307,
				20.0,
				20000.0,
				10.0,
				-20.0,
				L"all",
				ToneGeneratorFilter::REPLACE);
			vector<wstring> channelNames = { L"L" };
			tone.initialize((std::numeric_limits<float>::min)(), 4, channelNames);
			vector<double> input = { 0.1, 0.2, 0.3, 0.4 };
			vector<double> output(4, 123.0);
			double* inputChannel = input.data();
			double* outputChannel = output.data();
			tone.process(&outputChannel, &inputChannel, 4);
			const bool toneRuntimePassed = input == output;
			printf("ToneGenerator invalid phase increment remains dry: %s\n",
				toneRuntimePassed ? "PASS" : "FAIL");
			passed = toneRuntimePassed && passed;
		}

		return passed;
	}

	bool runBiQuadStabilityTests()
	{
		auto isIdentity = [](const BiQuad& filter)
		{
			double coefficients[4] = {};
			double a0 = 0.0;
			filter.getCoefficients(coefficients, a0);
			return a0 == 1.0 && coefficients[0] == 0.0 &&
				coefficients[1] == 0.0 && coefficients[2] == 0.0 &&
				coefficients[3] == 0.0;
		};
		auto finiteImpulse = [](BiQuad& filter, bool expectIdentity)
		{
			double tail = 0.0;
			for (unsigned frame = 0; frame < 8192; ++frame)
			{
				const double input = frame == 0 ? 1.0 : 0.0;
				const double output = filter.process(input);
				if (!std::isfinite(output) ||
					(expectIdentity && output != input) || std::abs(output) > 4.0)
				{
					return false;
				}
				tail = output;
			}
			return expectIdentity || std::abs(tail) < 1.0e-12;
		};

		BiQuad negativeQ(
			BiQuad::LOW_PASS, 0.0, 1000.0, 48000.0, -1.0, false);
		bool negativeQPassed = !negativeQ.isValid() && isIdentity(negativeQ) &&
			finiteImpulse(negativeQ, true);
		printf("BiQuad negative-Q identity stability: %s\n",
			negativeQPassed ? "PASS" : "FAIL");

		BiQuad nyquist(
			BiQuad::LOW_PASS, 0.0, 24000.0, 48000.0, 0.707, false);
		bool nyquistPassed = !nyquist.isValid() && isIdentity(nyquist) &&
			finiteImpulse(nyquist, true);
		printf("BiQuad Nyquist identity stability: %s\n",
			nyquistPassed ? "PASS" : "FAIL");

		BiQuad stable(
			BiQuad::LOW_PASS, 0.0, 1000.0, 48000.0, 0.707, false);
		bool stablePassed = stable.isValid() && !isIdentity(stable) &&
			finiteImpulse(stable, false);
		printf("BiQuad valid impulse stability: %s\n",
			stablePassed ? "PASS" : "FAIL");

		OutProcBiquadFilter outProcNyquist(
			BiQuad::LOW_PASS, 0.0, 24000.0, 0.707, false, false);
		vector<wstring> channels = { L"L" };
		outProcNyquist.initialize(48000.0f, 4, channels);
		double inputSamples[4] = { 0.25, -0.5, 0.75, -1.0 };
		double outputSamples[4] = {};
		double* input[] = { inputSamples };
		double* output[] = { outputSamples };
		outProcNyquist.process(output, input, 4);
		bool outProcPassed = std::equal(
			std::begin(inputSamples), std::end(inputSamples),
			std::begin(outputSamples));
		printf("OutProcBiquad Nyquist remains dry: %s\n",
			outProcPassed ? "PASS" : "FAIL");

		return negativeQPassed && nyquistPassed && stablePassed && outProcPassed;
	}

	bool runMemoryHelperAllocationCheckpointTests()
	{
		MemoryHelper::clearAllocationFailure();
		void* rejected = MemoryHelper::allocArray(
			(std::numeric_limits<size_t>::max)(), 2);
		const bool detected = rejected == NULL &&
			MemoryHelper::consumeAllocationFailure();
		const bool consumed = !MemoryHelper::consumeAllocationFailure();
		const bool passed = detected && consumed;
		printf("MemoryHelper allocation-failure checkpoint: %s\n",
			passed ? "PASS" : "FAIL");
		return passed;
	}

	bool runAsyncConvolutionFrameMismatchTests()
	{
		bool passed = true;
		std::wstring impulseFilename;
		if (!writeAsyncConvolutionTestImpulse(impulseFilename))
		{
			fprintf(stderr, "Could not create the async convolution test IR.\n");
			return false;
		}

		{
			ConvolutionFilter filter(impulseFilename);
			passed = runAsyncConvolutionFilterCase(
				"IR out-of-place", filter, 0.5, false, true) && passed;
		}
		{
			ConvolutionFilter filter(impulseFilename);
			passed = runAsyncConvolutionFilterCase(
				"IR in-place", filter, 0.5, true, false) && passed;
		}
		DeleteFileW(impulseFilename.c_str());

		const vector<FilterNode> nodes = {
			FilterNode(20.0, 6.0), FilterNode(20000.0, 6.0)
		};
		const double graphicEqScale = pow(10.0, 6.0 / 20.0);
		{
			GraphicEQFilter filter(nodes, 1024);
			passed = runAsyncConvolutionFilterCase(
				"GraphicEQ out-of-place", filter, graphicEqScale,
				false, false) && passed;
		}
		{
			GraphicEQFilter filter(nodes, 1024);
			passed = runAsyncConvolutionFilterCase(
				"GraphicEQ in-place", filter, graphicEqScale,
				true, false) && passed;
		}

		printf("Async convolution frame mismatch: %s\n",
			passed ? "PASS" : "FAIL");
		return passed;
	}

	bool runHybridConvPartitionLimitTests()
	{
		const int frameLength = 1;
		vector<double> allowedImpulse(HC_MAX_SINGLE_PARTITIONS, 0.0);
		allowedImpulse[0] = 1.0;
		HConvSingle allowedFilter = {};
		const bool allowedInitialized = hcInitSingle(
			&allowedFilter,
			allowedImpulse.data(),
			static_cast<int>(allowedImpulse.size()),
			frameLength,
			1);
		hcCloseSingle(&allowedFilter);

		vector<double> rejectedImpulse(
			static_cast<size_t>(HC_MAX_SINGLE_PARTITIONS) + 1, 0.0);
		rejectedImpulse[0] = 1.0;
		HConvSingle rejectedFilter = {};
		PrecisionTimer timer;
		timer.start();
		const bool rejectedInitialized = hcInitSingle(
			&rejectedFilter,
			rejectedImpulse.data(),
			HC_MAX_SINGLE_PARTITIONS + 1,
			frameLength,
			1);
		const double rejectionMilliseconds = timer.stop() * 1000.0;
		hcCloseSingle(&rejectedFilter);

		const bool passed = allowedInitialized && !rejectedInitialized &&
			rejectionMilliseconds < 100.0;
		printf(
			"HybridConv partition limit: %d accepted, %d rejected in %.3f ms (%s)\n",
			HC_MAX_SINGLE_PARTITIONS,
			HC_MAX_SINGLE_PARTITIONS + 1,
			rejectionMilliseconds,
			passed ? "PASS" : "FAIL");
		return passed;
	}

	bool runHybridConvLegacyWrapperFailureTests()
	{
		const int shortFrameLength = 1;
		const int dualLongFrameLength =
			HC_MAX_SINGLE_PARTITIONS / 2 + 1;
		vector<double> dualImpulse(
			static_cast<size_t>(2 * dualLongFrameLength) + 1, 0.0);
		dualImpulse[0] = 1.0;
		HConvDual dualFilter = {};
		const bool dualInitialized = hcInitDual(
			&dualFilter,
			dualImpulse.data(),
			static_cast<int>(dualImpulse.size()),
			shortFrameLength,
			dualLongFrameLength);
		const bool dualRejectedCleanly =
			!dualInitialized &&
			dualFilter.f_short == NULL &&
			dualFilter.f_long == NULL &&
			dualFilter.in_long == NULL &&
			dualFilter.out_long == NULL;
		hcCloseDual(&dualFilter);
		hcCloseDual(&dualFilter);

		const int mediumFrameLength = 1;
		const int trippleLongFrameLength =
			HC_MAX_SINGLE_PARTITIONS / 2 + 1;
		vector<double> trippleImpulse(
			static_cast<size_t>(mediumFrameLength) +
				2 * static_cast<size_t>(trippleLongFrameLength) + 1,
			0.0);
		trippleImpulse[0] = 1.0;
		HConvTripple trippleFilter = {};
		const bool trippleInitialized = hcInitTripple(
			&trippleFilter,
			trippleImpulse.data(),
			static_cast<int>(trippleImpulse.size()),
			shortFrameLength,
			mediumFrameLength,
			trippleLongFrameLength);
		const bool trippleRejectedCleanly =
			!trippleInitialized &&
			trippleFilter.f_short == NULL &&
			trippleFilter.f_medium == NULL &&
			trippleFilter.in_medium == NULL &&
			trippleFilter.out_medium == NULL;
		hcCloseTripple(&trippleFilter);
		hcCloseTripple(&trippleFilter);

		const double failedProcTime = getProcTime(
			1, HC_MAX_SINGLE_PARTITIONS + 1, 0.001);
		const bool procTimeRejectedCleanly =
			isfinite(failedProcTime) && failedProcTime < 0.0;
		const bool passed = dualRejectedCleanly &&
			trippleRejectedCleanly && procTimeRejectedCleanly;
		printf(
			"HybridConv legacy wrapper failure rollback: dual %s, tripple %s, timing %s (%s)\n",
			dualRejectedCleanly ? "PASS" : "FAIL",
			trippleRejectedCleanly ? "PASS" : "FAIL",
			procTimeRejectedCleanly ? "PASS" : "FAIL",
			passed ? "PASS" : "FAIL");
		return passed;
	}

	int runConvolutionSelfTest()
	{
		struct TestCase
		{
			int sampleRate;
			int frameLength;
			int impulseLength;
			int blocks;
		};

		const TestCase tests[] = {
			{ 44100, 256, 8192, 80 },
			{ 48000, 256, 8916, 80 },
			{ 96000, 512, 17832, 64 },
			{ 192000, 1024, 35664, 48 },
			{ 192000, 256, 35664, 80 },
		};

		bool passed = true;
		printf("Running internal HybridConv correctness benchmark...\n");
		for (const TestCase& test : tests)
		{
			passed = runConvolutionSelfTestCase(
				test.sampleRate,
				test.frameLength,
				test.impulseLength,
				test.blocks) && passed;
		}
		passed = runHybridConvPartitionLimitTests() && passed;
		passed = runHybridConvLegacyWrapperFailureTests() && passed;
		passed = runInvalidConvolutionInputTests() && passed;
		passed = runInvalidNumericFactoryTests() && passed;
		passed = runBiQuadStabilityTests() && passed;
		passed = runMemoryHelperAllocationCheckpointTests() && passed;
		passed = runAsyncConvolutionFrameMismatchTests() && passed;

		printf("HybridConv correctness benchmark: %s\n",
			passed ? "PASS" : "FAIL");
		return passed ? 0 : 2;
	}

	bool checkLoudnessFormulaValue(
		const char* name,
		size_t frequencyIndex,
		double phon,
		double expected)
	{
		double actual = LoudnessProfile::computeSPL(phon, frequencyIndex);
		double error = std::abs(actual - expected);
		printf("Loudness formula %s: %.12f dB\n", name, actual);
		if (!std::isfinite(actual) || error > 1.0e-9)
		{
			fprintf(stderr,
				"%s formula mismatch: expected %.12f dB, got %.12f dB.\n",
				name, expected, actual);
			return false;
		}
		return true;
	}

	bool runLoudnessFormulaTests()
	{
		bool passed = true;
		passed = checkLoudnessFormulaValue(
			"20Hz-20phon", 0, 20.0, 89.544478418546) && passed;
		passed = checkLoudnessFormulaValue(
			"100Hz-80phon", 7, 80.0, 92.460642396735) && passed;
		passed = checkLoudnessFormulaValue(
			"12500Hz-80phon", 28, 80.0, 85.605878244606) && passed;
		return passed;
	}

	bool runOriginalLoudnessTests()
	{
		auto checkCase = [](const char* name, bool result)
		{
			if (!result)
				fprintf(stderr, "Original loudness case '%s' failed.\n", name);
			return result;
		};
		auto nearlyEqual = [](double actual, double expected, double tolerance)
		{
			return std::isfinite(actual) &&
				std::abs(actual - expected) <= tolerance;
		};

		OriginalLoudnessCorrectionFilter::FilterParameters source;
		source.state = false;
		source.referenceLevel = 75.5f;
		source.referenceOffset = -2.25f;
		source.attenuation = 0.75f;
		const std::vector<char> serialized = source.serialize();
		const std::string serializedText(serialized.begin(), serialized.end());
		OriginalLoudnessCorrectionFilter::FilterParameters roundTrip(serialized);
		bool passed = checkCase("codec-round-trip",
			serializedText.rfind("Schema 1 Model MixomoShelfV1 ", 0) == 0 &&
			roundTrip.isInitialized() && !roundTrip.state &&
			std::abs(roundTrip.referenceLevel - 75.5f) < 1.0e-6f &&
			std::abs(roundTrip.referenceOffset + 2.25f) < 1.0e-6f &&
			std::abs(roundTrip.attenuation - 0.75f) < 1.0e-6f);

		OriginalLoudnessCorrectionFilter::FilterParameters commaDecimal(
			std::wstring(L"Schema 1 Model MixomoShelfV1 State 1 "
				L"ReferenceLevel 75,5 ReferenceOffset -2,25 Attenuation 0,75"));
		passed = checkCase("comma-decimal",
			commaDecimal.isInitialized() && commaDecimal.state &&
			std::abs(commaDecimal.referenceLevel - 75.5f) < 1.0e-6f &&
			std::abs(commaDecimal.referenceOffset + 2.25f) < 1.0e-6f &&
			std::abs(commaDecimal.attenuation - 0.75f) < 1.0e-6f) && passed;

		OriginalLoudnessCorrectionFilter::FilterParameters missingAttenuation(
			std::wstring(L"Schema 1 Model MixomoShelfV1 State 1 "
				L"ReferenceLevel 0 ReferenceOffset 0"));
		OriginalLoudnessCorrectionFilter::FilterParameters malformedAttenuation(
			std::wstring(L"Schema 1 Model MixomoShelfV1 State 1 "
				L"ReferenceLevel 0 ReferenceOffset 0 Attenuation broken"));
		passed = checkCase("strict-attenuation-required",
			!missingAttenuation.isInitialized() &&
			!malformedAttenuation.isInitialized()) && passed;

		const std::wstring invalidPayloads[] = {
			L"State 1 ReferenceLevel 0 ReferenceOffset 0 Attenuation 1",
			L"Schema 1 Model FormulaLoudnessV1 State 1 ReferenceLevel 0 ReferenceOffset 0 Attenuation 1",
			L"Schema 1 Model MixomoShelfV1 State 1 ReferenceLevel 0",
			L"Schema 1 Schema 1 Model MixomoShelfV1 State 1 ReferenceLevel 0 ReferenceOffset 0 Attenuation 1",
			L"Schema 1 Model MixomoShelfV1 Model MixomoShelfV1 State 1 ReferenceLevel 0 ReferenceOffset 0 Attenuation 1",
			L"Schema 1 Model MixomoShelfV1 State 1 State 0 ReferenceLevel 0 ReferenceOffset 0 Attenuation 1",
			L"Schema 1 Model MixomoShelfV1 State 1 ReferenceLevel 0 ReferenceLevel 1 ReferenceOffset 0 Attenuation 1",
			L"Schema 1 Model MixomoShelfV1 State 1 ReferenceLevel 0 ReferenceOffset 0 ReferenceOffset 1 Attenuation 1",
			L"Schema 1 Model MixomoShelfV1 State 1 ReferenceLevel 0 ReferenceOffset 0 Attenuation 1 Attenuation 0.5",
			L"Schema 1 Model MixomoShelfV1 State 1 ReferenceLevel 0 ReferenceOffset 0 Attenuation 1.5"
		};
		bool invalidsRejected = true;
		for (const std::wstring& payload : invalidPayloads)
		{
			OriginalLoudnessCorrectionFilter::FilterParameters parameters(payload);
			invalidsRejected = invalidsRejected && !parameters.isInitialized();
		}
		passed = checkCase("strict-parse-fail-closed", invalidsRejected) && passed;

		OriginalLoudnessCorrectionFilter::FilterParameters extension(
			std::wstring(L"Schema 1 Model MixomoShelfV1 State 1 ReferenceLevel 0 "
				L"ReferenceOffset 0 Attenuation 1 FutureToken value"));
		passed = checkCase("unknown-extension-is-ignored",
			extension.isInitialized()) && passed;

		OriginalLoudnessCorrectionFilterFactory factory;
		std::wstring originalCommand = L"LoudnessCorrectionOriginal";
		std::wstring originalParameters =
			L"Schema 1 Model MixomoShelfV1 State 0 ReferenceLevel 0 "
			L"ReferenceOffset 0 Attenuation 1";
		std::vector<IFilter*> factoryFilters = factory.createFilter(
			L"", originalCommand, originalParameters);
		const bool factoryCreatedStandalone = factoryFilters.size() == 1;
		for (IFilter* filter : factoryFilters)
		{
			static_cast<OriginalLoudnessCorrectionFilter*>(filter)->
				~OriginalLoudnessCorrectionFilter();
			MemoryHelper::free(filter);
		}
		std::wstring modernCommand = L"LoudnessCorrection";
		std::vector<IFilter*> modernFilters = factory.createFilter(
			L"", modernCommand, originalParameters);
		std::wstring invalidCommand = L"LoudnessCorrectionOriginal";
		std::wstring invalidParameters =
			L"Schema 1 Model MixomoShelfV1 State 0 State 1 "
			L"ReferenceLevel 0 ReferenceOffset 0 Attenuation 1";
		std::vector<IFilter*> invalidFilters = factory.createFilter(
			L"", invalidCommand, invalidParameters);
		passed = checkCase("standalone-factory-command",
			factoryCreatedStandalone && modernFilters.empty() &&
			invalidFilters.empty()) && passed;

		OriginalLoudnessCorrectionFilter::FilterParameters formula;
		formula.state = true;
		formula.referenceLevel = 0.0f;
		formula.referenceOffset = 0.0f;
		formula.attenuation = 1.0f;
		auto checkTransfer = [&](const char* name, double volumeDb,
			double expectedLow, double expectedHigh, double expectedOutput,
			bool expectedIdentity)
		{
			double low = 0.0;
			double high = 0.0;
			double output = 0.0;
			bool identity = false;
			OriginalLoudnessCorrectionFilterTestAccess::calculateTransfer(
				formula, volumeDb, low, high, output, identity);
			return checkCase(name,
				nearlyEqual(low, expectedLow, 1.0e-9) &&
				nearlyEqual(high, expectedHigh, 1.0e-9) &&
				nearlyEqual(output, expectedOutput, 1.0e-12) &&
				identity == expectedIdentity);
		};
		passed = checkTransfer("d-zero-is-unity", 0.0,
			0.0, 0.0, 1.0, true) && passed;
		passed = checkTransfer("minus-0.1db-has-no-identity-dead-zone", -0.1,
			0.12222222222222223, 0.0498890122542803,
			0.985979550168123, false) && passed;
		passed = checkTransfer("plus-0.1db-has-no-identity-dead-zone", 0.1,
			-0.0549389228269354, -0.0174781386661802,
			1.0, false) && passed;
		passed = checkTransfer("minus-10db-golden", -10.0,
			12.222222222222223, 4.00368701458404,
			0.24366365228060777, false) && passed;
		passed = checkTransfer("minus-20db-golden", -20.0,
			24.444444444444446, 6.411803884299546,
			0.05937197544272493, false) && passed;

		formula.referenceLevel = -20.0f;
		passed = checkTransfer("negative-difference-golden", -10.0,
			-4.9216162424790335, -1.544369579523042,
			1.0, false) && passed;

		formula.referenceLevel = 0.0f;
		const unsigned sampleRates[] = {
			8000, 11025, 12000, 16000, 22050, 48000
		};
		bool sampleRateCoefficientsSafe = true;
		for (unsigned sampleRate : sampleRates)
		{
			bool highShelfIdentity = false;
			const double maximumPoleRadius =
				OriginalLoudnessCorrectionFilterTestAccess::
					maximumSnapshotPoleRadius(
						formula, -10.0, sampleRate, highShelfIdentity);
			const bool shouldOmitHighShelf =
				10000.0 * 2.0 >= sampleRate;
			const bool safe = std::isfinite(maximumPoleRadius) &&
				maximumPoleRadius < 1.0 &&
				highShelfIdentity == shouldOmitHighShelf;
			if (!safe)
			{
				fprintf(stderr,
					"Original loudness %u Hz coefficients unsafe: "
					"pole %.12f, high identity %d (expected %d).\n",
					sampleRate,
					maximumPoleRadius,
					highShelfIdentity,
					shouldOmitHighShelf);
			}
			sampleRateCoefficientsSafe = sampleRateCoefficientsSafe && safe;
		}
		passed = checkCase("sample-rate-coefficient-safety",
			sampleRateCoefficientsSafe) && passed;

		OriginalLoudnessCorrectionFilter publicationProbe(formula);
		passed = checkCase("concurrent-snapshot-publication",
			OriginalLoudnessCorrectionFilterTestAccess::
				concurrentPublicationIsCoherent(publicationProbe)) && passed;

		bool lowRateCallbackBounded = true;
		for (unsigned sampleRate : sampleRates)
		{
			OriginalLoudnessCorrectionFilter::FilterParameters parameters = formula;
			parameters.state = false;
			OriginalLoudnessCorrectionFilter filter(parameters);
			filter.initialize(static_cast<float>(sampleRate), 64, { L"C" });
			OriginalLoudnessCorrectionFilterTestAccess::enableWithoutTracking(
				filter);
			OriginalLoudnessCorrectionFilterTestAccess::publishVolumeUpdate(
				filter, -10.0);

			const unsigned blockCount = (sampleRate / 4 + 63) / 64;
			for (unsigned block = 0; block < blockCount; ++block)
			{
				double inputSamples[64];
				double outputSamples[65] = {};
				for (unsigned frame = 0; frame < 64; ++frame)
				{
					const double time = static_cast<double>(block * 64 + frame) /
						static_cast<double>(sampleRate);
					inputSamples[frame] = 0.5 * std::sin(
						2.0 * M_PI * 997.0 * time);
				}
				outputSamples[64] = 67890.0;
				double* input[] = { inputSamples };
				double* output[] = { outputSamples };
				filter.process(output, input, 64);
				for (unsigned frame = 0; frame < 64; ++frame)
				{
					const double sample = outputSamples[frame];
					lowRateCallbackBounded = lowRateCallbackBounded &&
						std::isfinite(sample) && std::abs(sample) < 16.0;
				}
				lowRateCallbackBounded = lowRateCallbackBounded &&
					outputSamples[64] == 67890.0;
			}
			lowRateCallbackBounded = lowRateCallbackBounded &&
				OriginalLoudnessCorrectionFilterTestAccess::
					settledOnNonIdentityBank(filter);
		}
		passed = checkCase("low-rate-callback-finite-and-bounded",
			lowRateCallbackBounded) && passed;

		OriginalLoudnessCorrectionFilter::FilterParameters bypassParameters;
		bypassParameters.state = false;
		bypassParameters.referenceLevel = 0.0f;
		bypassParameters.referenceOffset = 0.0f;
		bypassParameters.attenuation = 1.0f;
		OriginalLoudnessCorrectionFilter bypass(bypassParameters);
		const std::vector<std::wstring> channels = { L"L", L"R" };
		bypass.initialize(48000.0f, 8, channels);
		double inputLeft[10] = { -1.0, -0.5, -0.25, 0.0, 0.25,
			0.5, 0.75, 1.0, 1234.0, 5678.0 };
		double inputRight[10] = { 1.0, 0.75, 0.5, 0.25, 0.0,
			-0.25, -0.5, -1.0, 4321.0, 8765.0 };
		double outputLeft[10] = {};
		double outputRight[10] = {};
		outputLeft[8] = 9001.0;
		outputLeft[9] = 9002.0;
		outputRight[8] = 9003.0;
		outputRight[9] = 9004.0;
		double* input[] = { inputLeft, inputRight };
		double* output[] = { outputLeft, outputRight };
		bypass.process(output, input, 8);
		bool stateZeroIdentity = bypass.getInPlace();
		for (size_t channel = 0; channel < 2; ++channel)
		{
			for (unsigned frame = 0; frame < 8; ++frame)
				stateZeroIdentity = stateZeroIdentity &&
					output[channel][frame] == input[channel][frame];
		}
		stateZeroIdentity = stateZeroIdentity &&
			outputLeft[8] == 9001.0 && outputLeft[9] == 9002.0 &&
			outputRight[8] == 9003.0 && outputRight[9] == 9004.0;
		passed = checkCase("state-zero-callback-identity",
			stateZeroIdentity) && passed;

		OriginalLoudnessCorrectionFilter outOfPlace(bypassParameters);
		OriginalLoudnessCorrectionFilter inPlace(bypassParameters);
		outOfPlace.initialize(48000.0f, 64, channels);
		inPlace.initialize(48000.0f, 64, channels);
		OriginalLoudnessCorrectionFilterTestAccess::enableWithoutTracking(
			outOfPlace);
		OriginalLoudnessCorrectionFilterTestAccess::enableWithoutTracking(
			inPlace);
		OriginalLoudnessCorrectionFilterTestAccess::publishVolumeUpdate(
			outOfPlace, -20.0);
		OriginalLoudnessCorrectionFilterTestAccess::publishVolumeUpdate(
			inPlace, -20.0);
		bool callbackContract = true;
		bool becameAudible = false;
		for (unsigned block = 0; block < 32; ++block)
		{
			double sourceLeft[65] = {};
			double sourceRight[65] = {};
			double processedLeft[65] = {};
			double processedRight[65] = {};
			double inPlaceLeft[65] = {};
			double inPlaceRight[65] = {};
			for (unsigned frame = 0; frame < 64; ++frame)
			{
				const double phase = static_cast<double>(block * 64 + frame);
				sourceLeft[frame] = std::sin(phase * 0.013);
				sourceRight[frame] = std::cos(phase * 0.017) * 0.5;
				inPlaceLeft[frame] = sourceLeft[frame];
				inPlaceRight[frame] = sourceRight[frame];
			}
			processedLeft[64] = 12345.0;
			processedRight[64] = 23456.0;
			inPlaceLeft[64] = 34567.0;
			inPlaceRight[64] = 45678.0;
			double* source[] = { sourceLeft, sourceRight };
			double* processed[] = { processedLeft, processedRight };
			double* aliased[] = { inPlaceLeft, inPlaceRight };
			outOfPlace.process(processed, source, 64);
			inPlace.process(aliased, aliased, 64);
			for (unsigned frame = 0; frame < 64; ++frame)
			{
				callbackContract = callbackContract &&
					nearlyEqual(processedLeft[frame], inPlaceLeft[frame], 1.0e-12) &&
					nearlyEqual(processedRight[frame], inPlaceRight[frame], 1.0e-12);
				becameAudible = becameAudible ||
					std::abs(processedLeft[frame] - sourceLeft[frame]) > 1.0e-6 ||
					std::abs(processedRight[frame] - sourceRight[frame]) > 1.0e-6;
			}
			callbackContract = callbackContract &&
				processedLeft[64] == 12345.0 &&
				processedRight[64] == 23456.0 &&
				inPlaceLeft[64] == 34567.0 &&
				inPlaceRight[64] == 45678.0;
		}
		callbackContract = callbackContract && becameAudible &&
			OriginalLoudnessCorrectionFilterTestAccess::settledOnNonIdentityBank(
				outOfPlace) &&
			OriginalLoudnessCorrectionFilterTestAccess::settledOnNonIdentityBank(
				inPlace);
		passed = checkCase("callback-in-place-out-of-place-contract",
			callbackContract) && passed;

		printf("Original loudness formula/codec/callback: %s\n",
			passed ? "passed" : "failed");
		return passed;
	}

	bool runVSTMidiBindingCodecTests()
	{
		auto checkCase = [](const char* name, bool result)
		{
			if (!result)
				fprintf(stderr, "VST MIDI codec case '%s' failed.\n", name);
			return result;
		};

		VSTMidiConfiguration source;
		source.device.manufacturerId = 0x1234;
		source.device.productId = 0xabcd;
		source.device.driverVersion = 0x01020304;
		source.device.nameOrdinal = 2;
		source.device.name = L"MIDI Controller";

		VSTMidiBinding cc;
		cc.messageType = VSTMidiMessageType::ControlChange;
		cc.channel = 2;
		cc.controlNumber = 74;
		cc.valueMode = VSTMidiValueMode::Absolute;
		cc.parameterType = VSTMidiParameterType::VST3ParamID;
		cc.parameterId = 0xf1234567;
		cc.parameterName = L"Cutoff";
		cc.stepCount = 0;
		source.bindings.push_back(cc);

		VSTMidiBinding note;
		note.messageType = VSTMidiMessageType::Note;
		note.channel = 15;
		note.controlNumber = 60;
		note.valueMode = VSTMidiValueMode::Toggle;
		note.parameterType = VSTMidiParameterType::VST2ParameterIndex;
		note.parameterId = 7;
		note.parameterName = L"Bypass";
		note.stepCount = 1;
		source.bindings.push_back(note);

		VSTMidiBinding pitch;
		pitch.messageType = VSTMidiMessageType::PitchBend;
		pitch.channel = 0xff;
		pitch.controlNumber = 0;
		pitch.valueMode = VSTMidiValueMode::Absolute;
		pitch.parameterType = VSTMidiParameterType::VST3ParamID;
		pitch.parameterId = 42;
		pitch.parameterName = L"Pitch";
		pitch.stepCount = 127;
		source.bindings.push_back(pitch);

		std::wstring encoded;
		VSTMidiConfiguration decoded;
		bool passed = checkCase("round-trip",
			VSTMidiBindingCodec::serialize(source, encoded) &&
			encoded.rfind(L"EAMIDI1:", 0) == 0 &&
			VSTMidiBindingCodec::deserialize(encoded, decoded) &&
			decoded.version == VST_MIDI_CONFIG_VERSION &&
			decoded.device.manufacturerId == source.device.manufacturerId &&
			decoded.device.productId == source.device.productId &&
			decoded.device.driverVersion == source.device.driverVersion &&
			decoded.device.nameOrdinal == source.device.nameOrdinal &&
			decoded.device.name == source.device.name &&
			decoded.bindings.size() == 3);
		if (decoded.bindings.size() == 3)
		{
			passed = checkCase("cc-vst3-id",
				decoded.bindings[0].messageType ==
					VSTMidiMessageType::ControlChange &&
				decoded.bindings[0].channel == 2 &&
				decoded.bindings[0].controlNumber == 74 &&
				decoded.bindings[0].valueMode == VSTMidiValueMode::Absolute &&
				decoded.bindings[0].parameterType ==
					VSTMidiParameterType::VST3ParamID &&
				decoded.bindings[0].parameterId == 0xf1234567 &&
				decoded.bindings[0].parameterName == L"Cutoff" &&
				decoded.bindings[0].stepCount == 0) && passed;
			passed = checkCase("note-vst2-toggle",
				decoded.bindings[1].messageType == VSTMidiMessageType::Note &&
				decoded.bindings[1].channel == 15 &&
				decoded.bindings[1].controlNumber == 60 &&
				decoded.bindings[1].valueMode == VSTMidiValueMode::Toggle &&
				decoded.bindings[1].parameterType ==
					VSTMidiParameterType::VST2ParameterIndex &&
				decoded.bindings[1].parameterId == 7 &&
				decoded.bindings[1].parameterName == L"Bypass" &&
				decoded.bindings[1].stepCount == 1) && passed;
			passed = checkCase("pitch-bend-omni-stepped",
				decoded.bindings[2].messageType == VSTMidiMessageType::PitchBend &&
				decoded.bindings[2].channel == 0xff &&
				decoded.bindings[2].parameterType ==
					VSTMidiParameterType::VST3ParamID &&
				decoded.bindings[2].parameterId == 42 &&
				decoded.bindings[2].stepCount == 127) && passed;
		}

		VSTMidiConfiguration ignored;
		std::wstring oddHex = encoded;
		oddHex.push_back(L'0');
		std::wstring nonHex = encoded;
		if (nonHex.size() > 8)
			nonHex[8] = L'Z';
		std::wstring truncated = encoded;
		if (truncated.size() >= 2)
			truncated.resize(truncated.size() - 2);
		std::wstring trailing = encoded + L"00";
		passed = checkCase("invalid-wire-tokens",
			!VSTMidiBindingCodec::deserialize(L"BAD:" + encoded, ignored) &&
			!VSTMidiBindingCodec::deserialize(oddHex, ignored) &&
			!VSTMidiBindingCodec::deserialize(nonHex, ignored) &&
			!VSTMidiBindingCodec::deserialize(truncated, ignored) &&
			!VSTMidiBindingCodec::deserialize(trailing, ignored)) && passed;

		VSTMidiConfiguration invalidVst2 = source;
		invalidVst2.bindings[1].parameterName.clear();
		std::wstring invalidVst2Encoded;
		const bool invalidVst2Serialized =
			VSTMidiBindingCodec::serialize(invalidVst2, invalidVst2Encoded);
		passed = checkCase("vst2-name-guard",
			!invalidVst2Serialized && invalidVst2Encoded.empty()) && passed;

		VSTMidiConfiguration tooMany;
		tooMany.bindings.resize(VST_MIDI_MAX_BINDINGS + 1);
		std::wstring tooManyEncoded;
		passed = checkCase("binding-limit",
			!VSTMidiBindingCodec::serialize(tooMany, tooManyEncoded)) && passed;

		printf("VST MIDI binding codec: %s\n",
			passed ? "passed" : "failed");
		return passed;
	}

	bool runLoudnessParameterCodecTests()
	{
		auto checkCase = [](const char* name, bool result)
		{
			if (!result)
				fprintf(stderr, "Loudness parameter codec case '%s' failed.\n", name);
			return result;
		};

		LoudnessCorrectionFilter::FilterParameters source;
		source.state = false;
		source.referenceLevel = 75.5f;
		source.referenceOffset = -2.25f;
		source.attenuation = 0.75f;
		source.useManualVolume = true;
		source.manualVolume = -24.5f;
		source.binding = LoudnessCorrectionFilter::FilterParameters::BINDING_ALL;

		std::vector<char> serialized = source.serialize();
		std::string serializedText(serialized.begin(), serialized.end());
		LoudnessCorrectionFilter::FilterParameters roundTrip(serialized);
		bool roundTripPassed = serializedText.rfind(
			"Schema 1 Model FormulaLoudnessV1 Binding All ", 0) == 0
			&& roundTrip.isInitialized()
			&& roundTrip.binding ==
				LoudnessCorrectionFilter::FilterParameters::BINDING_ALL
			&& !roundTrip.state
			&& std::abs(roundTrip.referenceLevel - 75.5f) < 1.0e-6f
			&& std::abs(roundTrip.referenceOffset + 2.25f) < 1.0e-6f
			&& std::abs(roundTrip.attenuation - 0.75f) < 1.0e-6f
			&& roundTrip.useManualVolume
			&& std::abs(roundTrip.manualVolume + 24.5f) < 1.0e-6f;
		bool passed = checkCase("marked-round-trip", roundTripPassed);
		if (!roundTripPassed)
		{
			fprintf(stderr,
				"Serialized codec payload: '%.*s'; decoded initialized=%d state=%d "
				"referenceLevel=%.9g referenceOffset=%.9g attenuation=%.9g "
				"useManualVolume=%d manualVolume=%.9g binding=%d.\n",
				static_cast<int>(serializedText.size()), serializedText.c_str(),
				roundTrip.isInitialized(), roundTrip.state,
				roundTrip.referenceLevel, roundTrip.referenceOffset,
				roundTrip.attenuation, roundTrip.useManualVolume,
				roundTrip.manualVolume, static_cast<int>(roundTrip.binding));
		}

		LoudnessCorrectionFilter::FilterParameters commaDecimal(std::wstring(
			L"Schema 1 Model FormulaLoudnessV1 State 0 ReferenceLevel 75,5 "
			L"ReferenceOffset -2,25 Attenuation 0,75 Volume -24,5"));
		passed = checkCase("marked-comma-decimal", commaDecimal.isInitialized()
			&& commaDecimal.binding ==
				LoudnessCorrectionFilter::FilterParameters::BINDING_SINGLE
			&& !commaDecimal.state
			&& std::abs(commaDecimal.referenceLevel - 75.5f) < 1.0e-6f
			&& std::abs(commaDecimal.referenceOffset + 2.25f) < 1.0e-6f
			&& std::abs(commaDecimal.attenuation - 0.75f) < 1.0e-6f
			&& commaDecimal.useManualVolume
			&& std::abs(commaDecimal.manualVolume + 24.5f) < 1.0e-6f) && passed;

		LoudnessCorrectionFilter::FilterParameters explicitSingle(std::wstring(
			L"Schema 1 Model FormulaLoudnessV1 Binding Single State 1 "
			L"ReferenceLevel 80 ReferenceOffset 0 Attenuation 1"));
		passed = checkCase("explicit-single-binding", explicitSingle.isInitialized()
			&& explicitSingle.binding ==
				LoudnessCorrectionFilter::FilterParameters::BINDING_SINGLE) && passed;

		LoudnessCorrectionFilter::FilterParameters releasedFormula(std::wstring(
			L"State 1 ReferenceLevel 80 ReferenceOffset 0 Attenuation 1"));
		passed = checkCase("unmarked-formula-fails-closed",
			!releasedFormula.isInitialized()) && passed;

		LoudnessCorrectionFilter::FilterParameters mixomoLegacy(std::wstring(
			L"State 1 ReferenceLevel 0 ReferenceOffset 0 Attenuation 1"));
		LoudnessCorrectionFilter::FilterParameters unknownModel(std::wstring(
			L"Schema 2 Model FutureLoudness State 1 ReferenceLevel 80 "
			L"ReferenceOffset 0 Attenuation 1"));
		LoudnessCorrectionFilter::FilterParameters truncated(std::wstring(
			L"Schema 1 Model FormulaLoudnessV1 State 1 ReferenceLevel 80"));
		LoudnessCorrectionFilter::FilterParameters duplicate(std::wstring(
			L"Schema 1 Model FormulaLoudnessV1 State 1 State 0 "
			L"ReferenceLevel 80 ReferenceOffset 0 Attenuation 1"));
		LoudnessCorrectionFilter::FilterParameters invalidBinding(std::wstring(
			L"Schema 1 Model FormulaLoudnessV1 Binding Nearby State 1 "
			L"ReferenceLevel 80 ReferenceOffset 0 Attenuation 1"));
		LoudnessCorrectionFilter::FilterParameters duplicateBinding(std::wstring(
			L"Schema 1 Model FormulaLoudnessV1 Binding All Binding Single State 1 "
			L"ReferenceLevel 80 ReferenceOffset 0 Attenuation 1"));
		LoudnessCorrectionFilter::FilterParameters invalidReferenceLevel(std::wstring(
			L"Schema 1 Model FormulaLoudnessV1 Binding Single State 1 "
			L"ReferenceLevel invalid ReferenceOffset 0 Attenuation 1"));
		LoudnessCorrectionFilter::FilterParameters invalidReferenceOffset(std::wstring(
			L"Schema 1 Model FormulaLoudnessV1 Binding Single State 1 "
			L"ReferenceLevel 80 ReferenceOffset invalid Attenuation 1"));
		passed = checkCase("unmarked-mixomo-legacy-fails-closed",
			!mixomoLegacy.isInitialized()) && passed;
		passed = checkCase("unknown-model-fails-closed",
			!unknownModel.isInitialized()) && passed;
		passed = checkCase("truncated-input-fails-closed",
			!truncated.isInitialized()) && passed;
		passed = checkCase("duplicate-field-fails-closed",
			!duplicate.isInitialized()) && passed;
		passed = checkCase("invalid-binding-fails-closed",
			!invalidBinding.isInitialized()) && passed;
		passed = checkCase("duplicate-binding-fails-closed",
			!duplicateBinding.isInitialized()) && passed;
		passed = checkCase("invalid-reference-level-fails-closed",
			!invalidReferenceLevel.isInitialized()) && passed;
		passed = checkCase("invalid-reference-offset-fails-closed",
			!invalidReferenceOffset.isInitialized()) && passed;

		const std::wstring overflowNumber(400, L'9');
		LoudnessCorrectionFilter::FilterParameters overflowReferenceLevel(
			std::wstring(
				L"Schema 1 Model FormulaLoudnessV1 Binding Single State 1 "
				L"ReferenceLevel ") + overflowNumber +
			L" ReferenceOffset 0 Attenuation 1");
		LoudnessCorrectionFilter::FilterParameters overflowReferenceOffset(
			std::wstring(
				L"Schema 1 Model FormulaLoudnessV1 Binding Single State 1 "
				L"ReferenceLevel 80 ReferenceOffset ") + overflowNumber +
			L" Attenuation 1");
		passed = checkCase("overflow-reference-level-fails-closed",
			!overflowReferenceLevel.isInitialized()) && passed;
		passed = checkCase("overflow-reference-offset-fails-closed",
			!overflowReferenceOffset.isInitialized()) && passed;

		LoudnessCorrectionFilter::FilterParameters invalidAttenuation(std::wstring(
			L"Schema 1 Model FormulaLoudnessV1 Binding Single State 1 "
			L"ReferenceLevel 80 ReferenceOffset 0 Attenuation invalid"));
		LoudnessCorrectionFilter::FilterParameters overflowAttenuation(
			std::wstring(
				L"Schema 1 Model FormulaLoudnessV1 Binding Single State 1 "
				L"ReferenceLevel 80 ReferenceOffset 0 Attenuation ") +
			overflowNumber);
		LoudnessCorrectionFilter::FilterParameters invalidVolume(std::wstring(
			L"Schema 1 Model FormulaLoudnessV1 Binding Single State 1 "
			L"ReferenceLevel 80 ReferenceOffset 0 Attenuation 1 Volume invalid"));
		LoudnessCorrectionFilter::FilterParameters overflowVolume(
			std::wstring(
				L"Schema 1 Model FormulaLoudnessV1 Binding Single State 1 "
				L"ReferenceLevel 80 ReferenceOffset 0 Attenuation 1 Volume ") +
			overflowNumber);
		passed = checkCase("invalid-attenuation-defaults-to-one",
			invalidAttenuation.isInitialized() &&
			invalidAttenuation.attenuation == 1.0f) && passed;
		passed = checkCase("overflow-attenuation-defaults-to-one",
			overflowAttenuation.isInitialized() &&
			overflowAttenuation.attenuation == 1.0f) && passed;
		passed = checkCase("invalid-volume-defaults-to-automatic",
			invalidVolume.isInitialized() && !invalidVolume.useManualVolume) && passed;
		passed = checkCase("overflow-volume-defaults-to-automatic",
			overflowVolume.isInitialized() && !overflowVolume.useManualVolume) && passed;

		LoudnessCorrectionFilter::FilterParameters fastEngine(std::wstring(
			L"Schema 1 Model FormulaLoudnessV1 Binding Single State 1 "
			L"ReferenceLevel 80 ReferenceOffset 0 Attenuation 1 Engine Fast"));
		passed = checkCase("fast-engine-parses",
			fastEngine.isInitialized() &&
			fastEngine.engine ==
				LoudnessCorrectionFilter::FilterParameters::ENGINE_FAST) && passed;

		LoudnessCorrectionFilter::FilterParameters defaultEngine(std::wstring(
			L"Schema 1 Model FormulaLoudnessV1 Binding Single State 1 "
			L"ReferenceLevel 80 ReferenceOffset 0 Attenuation 1"));
		passed = checkCase("absent-engine-defaults-to-full",
			defaultEngine.isInitialized() &&
			defaultEngine.engine ==
				LoudnessCorrectionFilter::FilterParameters::ENGINE_FULL) && passed;

		LoudnessCorrectionFilter::FilterParameters explicitFull(std::wstring(
			L"Schema 1 Model FormulaLoudnessV1 Binding Single State 1 "
			L"ReferenceLevel 80 ReferenceOffset 0 Attenuation 1 Engine Full"));
		passed = checkCase("explicit-full-engine-parses",
			explicitFull.isInitialized() &&
			explicitFull.engine ==
				LoudnessCorrectionFilter::FilterParameters::ENGINE_FULL) && passed;

		LoudnessCorrectionFilter::FilterParameters invalidEngine(std::wstring(
			L"Schema 1 Model FormulaLoudnessV1 Binding Single State 1 "
			L"ReferenceLevel 80 ReferenceOffset 0 Attenuation 1 Engine Turbo"));
		LoudnessCorrectionFilter::FilterParameters duplicateEngine(std::wstring(
			L"Schema 1 Model FormulaLoudnessV1 Binding Single State 1 "
			L"ReferenceLevel 80 ReferenceOffset 0 Attenuation 1 Engine Fast Engine Full"));
		passed = checkCase("invalid-engine-fails-closed",
			!invalidEngine.isInitialized()) && passed;
		passed = checkCase("duplicate-engine-fails-closed",
			!duplicateEngine.isInitialized()) && passed;

		// The default full engine omits the key so older profiles keep
		// their exact text; the fast engine round-trips explicitly.
		LoudnessCorrectionFilter::FilterParameters fastSource;
		fastSource.state = true;
		fastSource.referenceLevel = 80.0f;
		fastSource.referenceOffset = 0.0f;
		fastSource.attenuation = 1.0f;
		fastSource.useManualVolume = false;
		fastSource.binding = LoudnessCorrectionFilter::FilterParameters::BINDING_SINGLE;
		fastSource.engine = LoudnessCorrectionFilter::FilterParameters::ENGINE_FAST;
		std::vector<char> fastSerialized = fastSource.serialize();
		std::string fastText(fastSerialized.begin(), fastSerialized.end());
		LoudnessCorrectionFilter::FilterParameters fastRoundTrip(fastSerialized);
		passed = checkCase("fast-engine-round-trip",
			fastText.find("Engine Fast") != std::string::npos &&
			fastRoundTrip.isInitialized() &&
			fastRoundTrip.engine ==
				LoudnessCorrectionFilter::FilterParameters::ENGINE_FAST) && passed;
		std::vector<char> fullSerialized = source.serialize();
		std::string fullText(fullSerialized.begin(), fullSerialized.end());
		passed = checkCase("full-engine-omits-key",
			fullText.find("Engine") == std::string::npos) && passed;

		LoudnessCorrectionFilter::FilterParameters defaultFollow(std::wstring(
			L"Schema 1 Model FormulaLoudnessV1 Binding Single State 1 "
			L"ReferenceLevel 80 ReferenceOffset 0 Attenuation 1"));
		passed = checkCase("absent-volume-follow-defaults-off",
			defaultFollow.isInitialized() &&
			defaultFollow.volumeFollow ==
				LoudnessCorrectionFilter::FilterParameters::VOLUME_FOLLOW_OFF) && passed;
		LoudnessCorrectionFilter::FilterParameters explicitOff(std::wstring(
			L"Schema 1 Model FormulaLoudnessV1 Binding Single State 1 "
			L"ReferenceLevel 80 ReferenceOffset 0 Attenuation 1 VolumeFollow Off"));
		passed = checkCase("explicit-volume-follow-off-parses",
			explicitOff.isInitialized() &&
			explicitOff.volumeFollow ==
				LoudnessCorrectionFilter::FilterParameters::VOLUME_FOLLOW_OFF) && passed;

		struct FollowCodecCase
		{
			const wchar_t* token;
			LoudnessCorrectionFilter::FilterParameters::VolumeFollowMode mode;
		};
		const FollowCodecCase followCases[] = {
			{ L"Linear", LoudnessCorrectionFilter::FilterParameters::VOLUME_FOLLOW_LINEAR },
			{ L"Logarithmic", LoudnessCorrectionFilter::FilterParameters::VOLUME_FOLLOW_LOGARITHMIC },
			{ L"Windows", LoudnessCorrectionFilter::FilterParameters::VOLUME_FOLLOW_WINDOWS }
		};
		for (const FollowCodecCase& followCase : followCases)
		{
			std::wstring text =
				L"Schema 1 Model FormulaLoudnessV1 Binding Single State 1 "
				L"ReferenceLevel 80 ReferenceOffset 0 Attenuation 1 VolumeFollow ";
			text += followCase.token;
			LoudnessCorrectionFilter::FilterParameters decoded(text);
			decoded.useManualVolume = true;
			decoded.manualVolume = -20.0f;
			std::vector<char> encoded = decoded.serialize();
			std::string encodedText(encoded.begin(), encoded.end());
			LoudnessCorrectionFilter::FilterParameters roundTripped(encoded);
			passed = checkCase("volume-follow-round-trip",
				decoded.isInitialized() && decoded.volumeFollow == followCase.mode &&
				encodedText.find("VolumeFollow") != std::string::npos &&
				roundTripped.isInitialized() &&
				roundTripped.volumeFollow == followCase.mode) && passed;
		}

		LoudnessCorrectionFilter::FilterParameters invalidFollow(std::wstring(
			L"Schema 1 Model FormulaLoudnessV1 Binding Single State 1 "
			L"ReferenceLevel 80 ReferenceOffset 0 Attenuation 1 VolumeFollow Cubic"));
		LoudnessCorrectionFilter::FilterParameters missingFollowValue(std::wstring(
			L"Schema 1 Model FormulaLoudnessV1 Binding Single State 1 "
			L"ReferenceLevel 80 ReferenceOffset 0 Attenuation 1 VolumeFollow"));
		LoudnessCorrectionFilter::FilterParameters duplicateFollow(std::wstring(
			L"Schema 1 Model FormulaLoudnessV1 Binding Single State 1 "
			L"ReferenceLevel 80 ReferenceOffset 0 Attenuation 1 "
			L"VolumeFollow Linear VolumeFollow Windows"));
		passed = checkCase("invalid-volume-follow-fails-closed",
			!invalidFollow.isInitialized()) && passed;
		passed = checkCase("missing-volume-follow-value-fails-closed",
			!missingFollowValue.isInitialized()) && passed;
		passed = checkCase("duplicate-volume-follow-fails-closed",
			!duplicateFollow.isInitialized()) && passed;

		// ParameterArchive has historically ignored extension fields. Keep that
		// forward-compatible behavior while adding the optional VolumeFollow key.
		LoudnessCorrectionFilter::FilterParameters unknownExtension(std::wstring(
			L"Schema 1 Model FormulaLoudnessV1 Binding Single State 1 "
			L"ReferenceLevel 80 ReferenceOffset 0 Attenuation 1 "
			L"VolumeFollow Linear FutureHint KeepMe"));
		passed = checkCase("unknown-extension-field-remains-compatible",
			unknownExtension.isInitialized() &&
			unknownExtension.volumeFollow ==
				LoudnessCorrectionFilter::FilterParameters::VOLUME_FOLLOW_LINEAR) && passed;

		std::vector<char> offSerialized = explicitOff.serialize();
		std::string offText(offSerialized.begin(), offSerialized.end());
		LoudnessCorrectionFilter::FilterParameters offRoundTrip(offSerialized);
		passed = checkCase("off-volume-follow-omits-key",
			offText.find("VolumeFollow") == std::string::npos &&
			offRoundTrip.isInitialized() &&
			offRoundTrip.volumeFollow ==
				LoudnessCorrectionFilter::FilterParameters::VOLUME_FOLLOW_OFF) && passed;

		printf("Loudness parameter codec: %s\n", passed ? "passed" : "failed");
		return passed;
	}

	bool runLoudnessVolumeFollowTests()
	{
		using Parameters = LoudnessCorrectionFilter::FilterParameters;
		const double epsilon = 1.0e-12;
		bool passed = true;
		auto checkGain = [&](const char* name, Parameters::VolumeFollowMode mode,
			double volumeDb, double scalar, bool muted, double expected)
		{
			double actual = LoudnessCorrectionFilterTestAccess::volumeFollowGain(
				mode, volumeDb, scalar, muted);
			bool result = std::isfinite(actual) && std::abs(actual - expected) <= epsilon;
			if (!result)
				fprintf(stderr, "%s: expected %.12f, got %.12f.\n",
					name, expected, actual);
			return result;
		};

		passed = checkGain("volume-follow-off", Parameters::VOLUME_FOLLOW_OFF,
			-20.0, 0.5, true, 1.0) && passed;
		passed = checkGain("volume-follow-linear", Parameters::VOLUME_FOLLOW_LINEAR,
			-20.0, 0.5, false, 0.5) && passed;
		passed = checkGain("volume-follow-logarithmic",
			Parameters::VOLUME_FOLLOW_LOGARITHMIC,
			-20.0, 0.5, false, 0.25) && passed;
		passed = checkGain("volume-follow-windows", Parameters::VOLUME_FOLLOW_WINDOWS,
			-20.0, 0.5, false, 0.1) && passed;
		passed = checkGain("volume-follow-windows-uses-reported-db",
			Parameters::VOLUME_FOLLOW_WINDOWS,
			-6.020599913279624, 0.1, false, 0.5) && passed;
		passed = checkGain("volume-follow-linear-mute", Parameters::VOLUME_FOLLOW_LINEAR,
			-20.0, 0.5, true, 0.0) && passed;
		passed = checkGain("volume-follow-logarithmic-mute",
			Parameters::VOLUME_FOLLOW_LOGARITHMIC,
			-20.0, 0.5, true, 0.0) && passed;
		passed = checkGain("volume-follow-windows-mute", Parameters::VOLUME_FOLLOW_WINDOWS,
			-20.0, 0.5, true, 0.0) && passed;
		passed = checkGain("volume-follow-linear-cannot-amplify",
			Parameters::VOLUME_FOLLOW_LINEAR,
			0.0, 2.0, false, 1.0) && passed;
		passed = checkGain("volume-follow-windows-cannot-amplify",
			Parameters::VOLUME_FOLLOW_WINDOWS,
			6.0, 0.5, false, 1.0) && passed;
		passed = checkGain("volume-follow-windows-floor",
			Parameters::VOLUME_FOLLOW_WINDOWS,
			-200.0, 0.0, false, 1.0e-5) && passed;

		const unsigned sampleRate = 48000;
		const unsigned blockSize = 256;
		Parameters parameters;
		parameters.state = true;
		parameters.attenuation = 0.0f;
		parameters.useManualVolume = true;
		parameters.manualVolume = -20.0f;
		parameters.volumeFollow = Parameters::VOLUME_FOLLOW_WINDOWS;
		LoudnessCorrectionFilter filter(parameters);
		filter.initialize(
			static_cast<float>(sampleRate), blockSize, vector<wstring>(1, L"C"));
		LoudnessCorrectionFilterTestAccess::enableSyntheticAutomaticVolumePublisher(
			filter);

		double inputStorage[blockSize];
		double outputStorage[blockSize];
		for (unsigned frame = 0; frame < blockSize; ++frame)
			inputStorage[frame] = frame % 2 == 0 ? 0.8 : -0.4;
		double* inputChannels[] = { inputStorage };
		double* outputChannels[] = { outputStorage };
		filter.process(outputChannels, inputChannels, blockSize);
		bool initialGainPassed = true;
		for (unsigned frame = 0; frame < blockSize; ++frame)
		{
			if (std::abs(outputStorage[frame] - inputStorage[frame] * 0.1) > epsilon)
				initialGainPassed = false;
		}
		if (!initialGainPassed)
			fprintf(stderr, "volume-follow-initial-manual-gain failed.\n");
		passed = initialGainPassed && passed;

		LoudnessCorrectionFilterTestAccess::publishVolumeState(
			filter, -6.020599913279624, 0.5, false);
		filter.process(outputChannels, inputChannels, blockSize);
		bool rampPassed =
			LoudnessCorrectionFilterTestAccess::volumeFollowRampRemaining(filter) > 0 &&
			outputStorage[0] > inputStorage[0] * 0.1 &&
			outputStorage[0] < inputStorage[0] * 0.5;
		for (unsigned block = 0;
			block < 8 &&
			LoudnessCorrectionFilterTestAccess::volumeFollowRampRemaining(filter) > 0;
			++block)
		{
			filter.process(outputChannels, inputChannels, blockSize);
		}
		rampPassed = std::abs(
			LoudnessCorrectionFilterTestAccess::currentVolumeFollowGain(filter) - 0.5) <=
			epsilon &&
			LoudnessCorrectionFilterTestAccess::volumeFollowRampRemaining(filter) == 0 &&
			rampPassed;
		if (!rampPassed)
			fprintf(stderr, "volume-follow-target-ramp failed.\n");
		passed = rampPassed && passed;

		LoudnessCorrectionFilterTestAccess::beginRuntimeBypass(filter);
		filter.process(outputChannels, inputChannels, blockSize);
		bool recoveryPassed =
			std::abs(outputStorage[0] - inputStorage[0] * 0.5) <= epsilon;
		LoudnessCorrectionFilterTestAccess::publishRuntimeRecovery(
			filter, -6.020599913279624);
		filter.process(outputChannels, inputChannels, blockSize);
		recoveryPassed =
			!LoudnessCorrectionFilterTestAccess::runtimeBypass(filter) &&
			recoveryPassed;
		if (!recoveryPassed)
			fprintf(stderr, "volume-follow-runtime-recovery failed.\n");
		passed = recoveryPassed && passed;

		LoudnessCorrectionFilterTestAccess::publishVolumeState(
			filter, -6.020599913279624, 0.5, true);
		// The callback must first consume the published target before remaining
		// can become nonzero. Bound the drain so a broken ramp cannot hang CI.
		filter.process(outputChannels, inputChannels, blockSize);
		for (unsigned block = 0;
			block < 8 &&
			LoudnessCorrectionFilterTestAccess::volumeFollowRampRemaining(filter) > 0;
			++block)
		{
			filter.process(outputChannels, inputChannels, blockSize);
		}
		filter.process(outputChannels, inputChannels, blockSize);
		const bool mutePassed =
			LoudnessCorrectionFilterTestAccess::volumeFollowRampRemaining(filter) == 0 &&
			std::abs(outputStorage[0]) <= epsilon;
		if (!mutePassed)
			fprintf(stderr, "volume-follow-mute-ramp failed.\n");
		passed = mutePassed && passed;

		// Every channel must share one ramp position. Compare out-of-place and
		// in-place filters, then retarget halfway through a ramp and require the
		// first new sample to continue from the callback's current gain.
		Parameters rampParameters = parameters;
		rampParameters.manualVolume = -20.0f;
		LoudnessCorrectionFilter outOfPlaceFilter(rampParameters);
		LoudnessCorrectionFilter inPlaceFilter(rampParameters);
		vector<wstring> stereoChannels(2, L"C");
		outOfPlaceFilter.initialize(
			static_cast<float>(sampleRate), blockSize, stereoChannels);
		inPlaceFilter.initialize(
			static_cast<float>(sampleRate), blockSize, stereoChannels);
		LoudnessCorrectionFilterTestAccess::enableSyntheticAutomaticVolumePublisher(
			outOfPlaceFilter);
		LoudnessCorrectionFilterTestAccess::enableSyntheticAutomaticVolumePublisher(
			inPlaceFilter);
		double stereoInput[2][blockSize];
		double stereoOutput[2][blockSize];
		double inPlaceStorage[2][blockSize];
		double* stereoInputChannels[] = { stereoInput[0], stereoInput[1] };
		double* stereoOutputChannels[] = { stereoOutput[0], stereoOutput[1] };
		double* inPlaceChannels[] = { inPlaceStorage[0], inPlaceStorage[1] };
		bool stereoRampPassed = true;
		auto prepareStereoBlock = [&]()
		{
			for (unsigned channel = 0; channel < 2; ++channel)
			{
				for (unsigned frame = 0; frame < blockSize; ++frame)
				{
					stereoInput[channel][frame] = 1.0;
					stereoOutput[channel][frame] = 0.0;
					inPlaceStorage[channel][frame] = 1.0;
				}
			}
		};
		auto processStereoBlock = [&](unsigned frameCount)
		{
			prepareStereoBlock();
			outOfPlaceFilter.process(
				stereoOutputChannels, stereoInputChannels, frameCount);
			inPlaceFilter.process(inPlaceChannels, inPlaceChannels, frameCount);
			for (unsigned channel = 0; channel < 2; ++channel)
			{
				for (unsigned frame = 0; frame < frameCount; ++frame)
				{
					stereoRampPassed = std::abs(
						stereoOutput[channel][frame] -
						inPlaceStorage[channel][frame]) <= epsilon && stereoRampPassed;
					stereoRampPassed = std::abs(
						stereoOutput[channel][frame] -
						stereoOutput[0][frame]) <= epsilon && stereoRampPassed;
				}
			}
		};
		processStereoBlock(blockSize);
		stereoRampPassed =
			std::abs(stereoOutput[0][0] - 0.1) <= epsilon && stereoRampPassed;

		const double firstTargetDb = -6.020599913279624;
		LoudnessCorrectionFilterTestAccess::publishVolumeState(
			outOfPlaceFilter, firstTargetDb, 0.5, false);
		LoudnessCorrectionFilterTestAccess::publishVolumeState(
			inPlaceFilter, firstTargetDb, 0.5, false);
		const unsigned partialRampFrames = 128;
		processStereoBlock(partialRampFrames);
		double currentBeforeRetarget =
			LoudnessCorrectionFilterTestAccess::currentVolumeFollowGain(
				outOfPlaceFilter);
		double lastBeforeRetarget = stereoOutput[0][partialRampFrames - 1];
		const double oldStepBound = std::abs(0.5 - 0.1) / 480.0 + epsilon;
		stereoRampPassed = std::abs(
			lastBeforeRetarget - currentBeforeRetarget) <= oldStepBound &&
			stereoRampPassed;

		const double retargetGain = 0.05;
		const double retargetDb = 20.0 * std::log10(retargetGain);
		LoudnessCorrectionFilterTestAccess::publishVolumeState(
			outOfPlaceFilter, retargetDb, retargetGain, false);
		LoudnessCorrectionFilterTestAccess::publishVolumeState(
			inPlaceFilter, retargetDb, retargetGain, false);
		processStereoBlock(64);
		const double retargetStepBound =
			std::abs(currentBeforeRetarget - retargetGain) / 480.0 + epsilon;
		stereoRampPassed = std::abs(
			stereoOutput[0][0] - currentBeforeRetarget) <=
			retargetStepBound &&
			stereoOutput[0][0] <= currentBeforeRetarget + epsilon &&
			stereoOutput[0][0] >= retargetGain - epsilon && stereoRampPassed;
		if (!stereoRampPassed)
			fprintf(stderr, "volume-follow-stereo-retarget failed.\n");
		passed = stereoRampPassed && passed;

		// Keep volume follow as a final wideband stage while active Full/Fast
		// correction crosses raw -> common A, warms, fades, and reaches the
		// settled block path. Use in-place processing on the followed side so
		// this also guards its post-gain ordering.
		for (Parameters::EngineMode engine : {
			Parameters::ENGINE_FULL, Parameters::ENGINE_FAST })
		for (Parameters::VolumeFollowMode mode : {
			Parameters::VOLUME_FOLLOW_LINEAR,
			Parameters::VOLUME_FOLLOW_LOGARITHMIC,
			Parameters::VOLUME_FOLLOW_WINDOWS })
		{
			Parameters baselineParameters = parameters;
			baselineParameters.attenuation = 1.0f;
			baselineParameters.engine = engine;
			baselineParameters.volumeFollow = Parameters::VOLUME_FOLLOW_OFF;
			Parameters followedParameters = baselineParameters;
			followedParameters.volumeFollow = mode;
			const double expectedGain = mode == Parameters::VOLUME_FOLLOW_LINEAR ?
				0.8 : mode == Parameters::VOLUME_FOLLOW_LOGARITHMIC ? 0.64 : 0.1;
			// The contour must use the same attenuation as calibration and the
			// final wideband stage, not the unrelated source value (-20 dB).
			baselineParameters.manualVolume = 20.0 * std::log10(expectedGain);

			LoudnessCorrectionFilter baselineFilter(baselineParameters);
			LoudnessCorrectionFilter followedFilter(followedParameters);
			baselineFilter.initialize(
				static_cast<float>(sampleRate), blockSize, stereoChannels);
			followedFilter.initialize(
				static_cast<float>(sampleRate), blockSize, stereoChannels);
			LoudnessCorrectionFilterTestAccess::enableSyntheticAutomaticVolumePublisher(
				baselineFilter);
			LoudnessCorrectionFilterTestAccess::enableSyntheticAutomaticVolumePublisher(
				followedFilter);

			double baselineInput[2][blockSize];
			double baselineOutput[2][blockSize];
			double followedInPlace[2][blockSize];
			double* baselineInputChannels[] = {
				baselineInput[0], baselineInput[1] };
			double* baselineOutputChannels[] = {
				baselineOutput[0], baselineOutput[1] };
			double* followedInPlaceChannels[] = {
				followedInPlace[0], followedInPlace[1] };
			bool activeCorrectionPassed = true;
			const unsigned blockCount = 1152;
			for (unsigned block = 0; block < blockCount; ++block)
			{
				const double updatedGain = mode == Parameters::VOLUME_FOLLOW_LINEAR ?
					0.5 : mode == Parameters::VOLUME_FOLLOW_LOGARITHMIC ? 0.25 : 0.1;
				if (block == 384)
				{
					// The reported dB stays fixed while scalar changes independently.
					LoudnessCorrectionFilterTestAccess::publishVolumeState(
						baselineFilter, 20.0 * std::log10(updatedGain), 0.5, false);
					LoudnessCorrectionFilterTestAccess::publishVolumeState(
						followedFilter, -20.0, 0.5, false);
				}
				for (unsigned channel = 0; channel < 2; ++channel)
				{
					for (unsigned frame = 0; frame < blockSize; ++frame)
					{
						const double phase = 2.0 * 3.14159265358979323846 *
							(997.0 + channel * 151.0) *
							static_cast<double>(block * blockSize + frame) /
							static_cast<double>(sampleRate);
						const double sample = 0.6 * std::sin(phase);
						baselineInput[channel][frame] = sample;
						followedInPlace[channel][frame] = sample;
					}
				}

				baselineFilter.process(
					baselineOutputChannels, baselineInputChannels, blockSize);
				followedFilter.process(
					followedInPlaceChannels, followedInPlaceChannels, blockSize);
				for (unsigned channel = 0; channel < 2; ++channel)
				{
					for (unsigned frame = 0; frame < blockSize; ++frame)
					{
						// The initial manual parameter is float; allow its dB
						// quantization. During retarget, the gain ramps separately.
						if (block >= 384 && block < 768)
							continue;
						activeCorrectionPassed = std::abs(
							followedInPlace[channel][frame] -
							baselineOutput[channel][frame] *
							(block < 384 ? expectedGain : updatedGain)) <= 1.0e-7 &&
							activeCorrectionPassed;
					}
				}
			}
			activeCorrectionPassed =
				LoudnessCorrectionFilterTestAccess::settledOnNonIdentityBank(
					baselineFilter) &&
				LoudnessCorrectionFilterTestAccess::settledOnNonIdentityBank(
					followedFilter) &&
				activeCorrectionPassed;
			if (!activeCorrectionPassed)
				fprintf(stderr, "volume-follow-active-correction-post-gain failed (engine=%d, mode=%d).\n", engine, mode);
			passed = activeCorrectionPassed && passed;
		}

		// Automatic Single binding without endpoint identity cannot provide a
		// trustworthy first snapshot. An enabled APO-owned follow mode must stay
		// silent instead of leaking a full-volume startup block.
		Parameters unavailableParameters = parameters;
		unavailableParameters.useManualVolume = false;
		unavailableParameters.binding = Parameters::BINDING_SINGLE;
		LoudnessCorrectionFilter unavailableFilter(unavailableParameters);
		unavailableFilter.initialize(
			static_cast<float>(sampleRate), blockSize, vector<wstring>(1, L"C"));
		unavailableFilter.process(outputChannels, inputChannels, blockSize);
		bool unavailablePassed = std::abs(
			LoudnessCorrectionFilterTestAccess::currentVolumeFollowGain(
				unavailableFilter)) <= epsilon;
		for (unsigned frame = 0; frame < blockSize; ++frame)
			unavailablePassed =
				std::abs(outputStorage[frame]) <= epsilon && unavailablePassed;
		if (!unavailablePassed)
			fprintf(stderr, "volume-follow-unavailable-binding-fail-closed failed.\n");
		passed = unavailablePassed && passed;

		Parameters disabled = parameters;
		disabled.state = false;
		LoudnessCorrectionFilter disabledFilter(disabled);
		disabledFilter.initialize(
			static_cast<float>(sampleRate), blockSize, vector<wstring>(1, L"C"));
		disabledFilter.process(outputChannels, inputChannels, blockSize);
		bool disabledPassed = true;
		for (unsigned frame = 0; frame < blockSize; ++frame)
			disabledPassed =
				outputStorage[frame] == inputStorage[frame] && disabledPassed;
		if (!disabledPassed)
			fprintf(stderr, "volume-follow-disabled-identity failed.\n");
		passed = disabledPassed && passed;

		printf("Loudness APO volume follow: %s\n", passed ? "passed" : "failed");
		return passed;
	}

	bool runLoudnessNearZeroHeadroomTests()
	{
		using Parameters = LoudnessCorrectionFilter::FilterParameters;
		bool passed = true;
		for (Parameters::EngineMode engine : {
			Parameters::ENGINE_FULL, Parameters::ENGINE_FAST })
		{
			Parameters parameters;
			parameters.state = true;
			parameters.referenceLevel = 80.0f;
			parameters.referenceOffset = 0.0f;
			parameters.attenuation = 1.0f;
			parameters.useManualVolume = true;
			parameters.manualVolume = -0.001f;
			parameters.engine = engine;

			LoudnessCorrectionFilter filter(parameters);
			filter.initialize(48000.0f, 256, vector<wstring>(1, L"C"));
			vector<double> gains;
			double nearZeroOutputGain = 1.0;
			LoudnessCorrectionFilterTestAccess::calculateConfiguredTransfer(
				filter, -0.001, gains, nearZeroOutputGain);
			const double nearZeroLossDb = 20.0 * std::log10(
				(std::max)(nearZeroOutputGain, 1.0e-15));

			double zeroOutputGain = 1.0;
			LoudnessCorrectionFilterTestAccess::calculateConfiguredTransfer(
				filter, 0.0, gains, zeroOutputGain);
			bool zeroIdentity = std::abs(zeroOutputGain - 1.0) <= 1.0e-12;
			for (double gain : gains)
				zeroIdentity = zeroIdentity && std::abs(gain) <= 1.0e-12;

			const bool enginePassed =
				std::isfinite(nearZeroOutputGain) &&
				nearZeroOutputGain <= 1.0 &&
				nearZeroLossDb > -0.01 &&
				zeroIdentity;
			printf(
				"Loudness %s near-zero headroom: %.9f dB, %s\n",
				engine == Parameters::ENGINE_FAST ? "Fast" : "Full",
				nearZeroLossDb,
				enginePassed ? "passed" : "failed");
			passed = enginePassed && passed;
		}
		return passed;
	}

	bool runLoudnessCrossoverCoefficientTests()
	{
		const unsigned sampleRates[] = {
			8000, 44100, 48000, 96000, 192000, 384000
		};
		bool passed = true;
		for (unsigned sampleRate : sampleRates)
		{
			LoudnessCorrectionFilter::FilterParameters parameters;
			parameters.state = true;
			parameters.referenceLevel = 80.0f;
			parameters.referenceOffset = 0.0f;
			parameters.attenuation = 1.0f;
			parameters.useManualVolume = true;
			parameters.manualVolume = 0.0f;

			LoudnessCorrectionFilter filter(parameters);
			vector<wstring> channels(1, L"C");
			filter.initialize(static_cast<float>(sampleRate), 256, channels);
			double maximumPoleRadius = 0.0;
			double maximumComplementError = 0.0;
			bool finite = LoudnessCorrectionFilterTestAccess::inspectCrossover(
				filter, maximumPoleRadius, maximumComplementError);
			printf(
				"Loudness crossover %u Hz: pole %.12f complement error %.3g\n",
				sampleRate, maximumPoleRadius, maximumComplementError);
			if (!finite || maximumPoleRadius >= 1.0 ||
				maximumComplementError > 1.0e-6)
			{
				fprintf(stderr,
					"LR28 crossover is non-finite, unstable, or non-complementary at %u Hz.\n",
					sampleRate);
				passed = false;
			}
		}
		return passed;
	}

	bool runLoudnessRuntimeContextTests()
	{
		LoudnessCorrectionFilter::FilterParameters parameters;
		parameters.state = true;
		parameters.useManualVolume = false;
		parameters.binding =
			LoudnessCorrectionFilter::FilterParameters::BINDING_ALL;
		LoudnessCorrectionFilter filter(parameters);

		// Global binding preserves the original Mixomo behavior: it reads the
		// Windows default render/multimedia volume without depending on APO
		// endpoint metadata.
		bool passed = LoudnessCorrectionFilterTestAccess::canTrackAutomaticVolume(
			filter);
		FilterRuntimeContext context;
		context.flowKnown = true;
		context.isCapture = true;
		context.endpointId = L"capture-endpoint";
		filter.setRuntimeContext(context);
		passed = LoudnessCorrectionFilterTestAccess::canTrackAutomaticVolume(
			filter) && passed;

		context.isCapture = false;
		context.endpointId.clear();
		filter.setRuntimeContext(context);
		passed = LoudnessCorrectionFilterTestAccess::canTrackAutomaticVolume(
			filter) && passed;

		parameters.binding =
			LoudnessCorrectionFilter::FilterParameters::BINDING_SINGLE;
		LoudnessCorrectionFilter singleFilter(parameters);
		singleFilter.setRuntimeContext(context);
		passed = !LoudnessCorrectionFilterTestAccess::canTrackAutomaticVolume(
			singleFilter) && passed;
		context.endpointId = L"render-endpoint";
		singleFilter.setRuntimeContext(context);
		passed = LoudnessCorrectionFilterTestAccess::canTrackAutomaticVolume(
			singleFilter) && passed;
		context.isCapture = true;
		singleFilter.setRuntimeContext(context);
		passed = !LoudnessCorrectionFilterTestAccess::canTrackAutomaticVolume(
			singleFilter) && passed;

		printf("Loudness runtime context: %s\n", passed ? "passed" : "failed");
		return passed;
	}

	bool runLoudnessOfflineAnalysisTests()
	{
		const unsigned sampleRate = 48000;
		const unsigned frameCount = 65536;
		auto renderFirstWindow = [=](
			bool state,
			float referenceOffset,
			bool manualVolume,
			vector<double>& output)
		{
			LoudnessCorrectionFilter::FilterParameters parameters;
			parameters.state = state;
			parameters.referenceLevel = 80.0f;
			parameters.referenceOffset = referenceOffset;
			parameters.attenuation = 1.0f;
			parameters.binding =
				LoudnessCorrectionFilter::FilterParameters::BINDING_SINGLE;
			parameters.useManualVolume = manualVolume;
			parameters.manualVolume = 0.0f;

			LoudnessCorrectionFilter filter(parameters);
			FilterRuntimeContext context;
			context.offlineAnalysis = true;
			filter.setRuntimeContext(context);
			vector<wstring> channels(1, L"C");
			filter.initialize(static_cast<float>(sampleRate), frameCount, channels);

			vector<double> input(frameCount, 0.0);
			input[0] = 1.0;
			output.assign(frameCount, 0.0);
			double* inputChannels[] = { input.data() };
			double* outputChannels[] = { output.data() };
			filter.process(outputChannels, inputChannels, frameCount);
			return input;
		};

		vector<double> offset0;
		vector<double> offset40;
		vector<double> disabled;
		vector<double> unavailableAutomatic;
		vector<double> input = renderFirstWindow(true, 0.0f, true, offset0);
		(void)renderFirstWindow(true, 40.0f, true, offset40);
		(void)renderFirstWindow(false, 40.0f, true, disabled);
		(void)renderFirstWindow(true, 40.0f, false, unavailableAutomatic);

		double maximumOffsetDifference = 0.0;
		double maximumDisabledError = 0.0;
		double maximumUnavailableError = 0.0;
		bool finite = true;
		for (unsigned frame = 0; frame < frameCount; ++frame)
		{
			finite = finite && std::isfinite(offset0[frame]) &&
				std::isfinite(offset40[frame]) && std::isfinite(disabled[frame]) &&
				std::isfinite(unavailableAutomatic[frame]);
			maximumOffsetDifference = (std::max)(maximumOffsetDifference,
				std::abs(offset0[frame] - offset40[frame]));
			maximumDisabledError = (std::max)(maximumDisabledError,
				std::abs(disabled[frame] - input[frame]));
			maximumUnavailableError = (std::max)(maximumUnavailableError,
				std::abs(unavailableAutomatic[frame] - input[frame]));
		}

		bool passed = finite && maximumOffsetDifference > 1.0e-6 &&
			maximumDisabledError <= 1.0e-12 &&
			maximumUnavailableError <= 1.0e-12;
		printf(
			"Loudness offline first window: offset delta %.9g, State0 error %.9g, unavailable error %.9g, %s\n",
			maximumOffsetDifference,
			maximumDisabledError,
			maximumUnavailableError,
			passed ? "passed" : "failed");
		return passed;
	}

	bool runFilterEngineDeviceInfoReuseTests()
	{
		FilterEngine engine;
		engine.setDeviceInfo(
			true, false, L"old-device", L"old-connection", L"old-guid", L"old-string");
		bool passed = engine.isDeviceInfoKnown() && engine.isCapture() &&
			!engine.isPostMixInstalled() && engine.getDeviceName() == L"old-device" &&
			engine.getConnectionName() == L"old-connection" &&
			engine.getDeviceGuid() == L"old-guid" &&
			engine.getDeviceString() == L"old-string";

		engine.clearDeviceInfo();
		passed = !engine.isDeviceInfoKnown() && !engine.isCapture() &&
			engine.isPostMixInstalled() && engine.getDeviceName().empty() &&
			engine.getConnectionName().empty() && engine.getDeviceGuid().empty() &&
			engine.getDeviceString().empty() && passed;

		printf("FilterEngine device-info reuse: %s\n", passed ? "passed" : "failed");
		return passed;
	}

	bool writeFilterEngineTestConfig(
		const std::wstring& path,
		const char* contents)
	{
		HANDLE file = CreateFileW(
			path.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
			FILE_ATTRIBUTE_TEMPORARY, NULL);
		if (file == INVALID_HANDLE_VALUE)
			return false;
		const DWORD length = static_cast<DWORD>(strlen(contents));
		DWORD written = 0;
		const bool succeeded = WriteFile(
			file, contents, length, &written, NULL) != FALSE &&
			written == length;
		CloseHandle(file);
		return succeeded;
	}

	bool runFilterEngineFailedReloadTransactionTests()
	{
		wchar_t tempDirectory[MAX_PATH] = {};
		wchar_t tempFilename[MAX_PATH] = {};
		if (GetTempPathW(MAX_PATH, tempDirectory) == 0 ||
			GetTempFileNameW(tempDirectory, L"EAP", 0, tempFilename) == 0)
		{
			printf("FilterEngine failed-reload transaction: FAIL (temp file)\n");
			return false;
		}

		const std::wstring configPath(tempFilename);
		bool passed = writeFilterEngineTestConfig(
			configPath, "Preamp: -6 dB\r\n");
		if (passed)
		{
			FilterEngine engine;
			FilterEngineTestAccess::addFactory(
				engine, new AllocationFailureFilterFactory());
			engine.initialize(48000.0f, 1, 1, 1, 0, 8, configPath);
			passed = FilterEngineTestAccess::transitionLength(engine) == 480 && passed;

			double input = 1.0;
			double initialOutput = 0.0;
			engine.process(&initialOutput, &input, 1);
			const std::wstring activeWatchKey =
				L"Software\\EqualizerAPO\\BenchmarkActiveWatch";
			engine.watchRegistryKey(activeWatchKey);

			passed = writeFilterEngineTestConfig(
				configPath,
				"Preamp: 6 dB\r\nForceAllocationFailure: ON\r\n") && passed;
			const bool failedReloadNeedsRetirement = engine.loadConfig(configPath);
			double failedReloadOutput = 0.0;
			engine.process(&failedReloadOutput, &input, 1);
			passed = !failedReloadNeedsRetirement &&
				!FilterEngineTestAccess::hasPendingConfiguration(engine) &&
				FilterEngineTestAccess::watchesRegistryKey(engine, activeWatchKey) &&
				std::isfinite(initialOutput) &&
				std::abs(failedReloadOutput - initialOutput) <= 1.0e-12 && passed;

			HANDLE lockedConfig = CreateFileW(
				configPath.c_str(), GENERIC_READ, 0, NULL, OPEN_EXISTING,
				FILE_ATTRIBUTE_NORMAL, NULL);
			bool lockedReloadPassed = lockedConfig != INVALID_HANDLE_VALUE;
			if (lockedReloadPassed)
			{
				const ULONGLONG start = GetTickCount64();
				const bool lockedReloadNeedsRetirement =
					engine.loadConfig(configPath);
				const ULONGLONG elapsed = GetTickCount64() - start;
				CloseHandle(lockedConfig);
				double lockedReloadOutput = 0.0;
				engine.process(&lockedReloadOutput, &input, 1);
				lockedReloadPassed = !lockedReloadNeedsRetirement &&
					elapsed < 5000 &&
					!FilterEngineTestAccess::hasPendingConfiguration(engine) &&
					FilterEngineTestAccess::watchesRegistryKey(
						engine, activeWatchKey) &&
					std::abs(lockedReloadOutput - initialOutput) <= 1.0e-12;
			}
			printf("FilterEngine locked-file reload timeout: %s\n",
				lockedReloadPassed ? "PASS" : "FAIL");
			passed = lockedReloadPassed && passed;

			passed = writeFilterEngineTestConfig(
				configPath, "Preamp: -3 dB\r\n") && passed;
			const bool publishedReloadNeedsRetirement = engine.loadConfig(configPath);
			const std::wstring pendingWatchKey =
				L"Software\\EqualizerAPO\\BenchmarkPendingWatch";
			engine.watchRegistryKey(pendingWatchKey);
			const bool overlappingReloadNeedsRetirement = engine.loadConfig(configPath);
			passed = publishedReloadNeedsRetirement &&
				!overlappingReloadNeedsRetirement &&
				FilterEngineTestAccess::hasPendingConfiguration(engine) &&
				FilterEngineTestAccess::watchesRegistryKey(engine, pendingWatchKey) &&
				passed;

			FilterEngineTestAccess::forceTransitionLength(engine, 0);
			double zeroLengthTransitionOutput = 0.0;
			engine.process(&zeroLengthTransitionOutput, &input, 1);
			const double expectedNewOutput = std::pow(10.0, -3.0 / 20.0);
			const bool zeroLengthTransitionPassed =
				!FilterEngineTestAccess::hasPendingConfiguration(engine) &&
				std::isfinite(zeroLengthTransitionOutput) &&
				std::abs(zeroLengthTransitionOutput - expectedNewOutput) <= 1.0e-12;
			printf("FilterEngine zero-length transition: %s\n",
				zeroLengthTransitionPassed ? "PASS" : "FAIL");
			passed = zeroLengthTransitionPassed && passed;

			FilterEngine tinyRateEngine;
			tinyRateEngine.initialize(50.0f, 1, 1, 1, 0, 8, configPath);
			const bool tinyRatePassed =
				FilterEngineTestAccess::transitionLength(tinyRateEngine) == 1;

			FilterEngine invalidRateEngine;
			invalidRateEngine.initialize(
				(std::numeric_limits<float>::quiet_NaN)(),
				1, 1, 1, 0, 8, configPath);
			double invalidRateOutput = 0.0;
			invalidRateEngine.process(&invalidRateOutput, &input, 1);
			const bool invalidRatePassed =
				FilterEngineTestAccess::transitionLength(invalidRateEngine) == 0 &&
				invalidRateEngine.getMaxFrameCount() == 0 &&
				invalidRateOutput == input;

			FilterEngine excessiveRateEngine;
			excessiveRateEngine.initialize(
				(std::numeric_limits<float>::max)(),
				1, 1, 1, 0, 8, configPath);
			double excessiveRateOutput = 0.0;
			excessiveRateEngine.process(&excessiveRateOutput, &input, 1);
			const bool excessiveRatePassed =
				FilterEngineTestAccess::transitionLength(excessiveRateEngine) == 0 &&
				excessiveRateEngine.getMaxFrameCount() == 0 &&
				excessiveRateOutput == input;
			const bool sampleRateBoundaryPassed = tinyRatePassed &&
				invalidRatePassed && excessiveRatePassed;
			printf("FilterEngine sample-rate transition bounds: %s\n",
				sampleRateBoundaryPassed ? "PASS" : "FAIL");
			passed = sampleRateBoundaryPassed && passed;
		}

		DeleteFileW(configPath.c_str());
		printf("FilterEngine failed-reload transaction: %s\n",
			passed ? "PASS" : "FAIL");
		return passed;
	}

	bool runLoudnessAnchorFitCase(unsigned sampleRate)
	{
		LoudnessCorrectionFilter::FilterParameters parameters;
		parameters.state = true;
		parameters.referenceLevel = 80.0f;
		parameters.referenceOffset = 0.0f;
		parameters.attenuation = 1.0f;
		parameters.useManualVolume = true;
		parameters.manualVolume = -40.0f;

		LoudnessCorrectionFilter filter(parameters);
		vector<wstring> channels(1, L"C");
		filter.initialize(static_cast<float>(sampleRate), 256, channels);
		vector<double> gains;
		double outputGainLinear = 1.0;
		LoudnessCorrectionFilterTestAccess::calculateTransfer(
			filter, parameters.manualVolume, gains, outputGainLinear);

		double maximumAnchorError = 0.0;
		for (size_t band = 0;
			band < LoudnessCorrectionFilterTestAccess::activeBandCount(filter);
			++band)
		{
			double frequency =
				LoudnessProfile::LOUDNESS_PROFILE_TABLE[band].frequency;
			double target = LoudnessProfile::computeContourDelta(
				40.0, 80.0, band);
			double actual =
				LoudnessCorrectionFilterTestAccess::rawCorrectionResponseDb(
					filter, gains, frequency);
			maximumAnchorError = (std::max)(maximumAnchorError,
				std::abs(actual - target));
		}

		printf("Loudness raw anchor fit %u Hz: maximum error %.9f dB\n",
			sampleRate, maximumAnchorError);
		if (!std::isfinite(outputGainLinear) || maximumAnchorError > 0.02)
		{
			fprintf(stderr, "Raw 29-point correction anchor fit regressed.\n");
			return false;
		}
		return true;
	}

	bool runLoudnessGuardedTransferCase(
		const char* name,
		unsigned sampleRate,
		double referenceLevel,
		double volume)
	{
		LoudnessCorrectionFilter::FilterParameters parameters;
		parameters.state = true;
		parameters.referenceLevel = static_cast<float>(referenceLevel);
		parameters.referenceOffset = 0.0f;
		parameters.attenuation = 1.0f;
		parameters.useManualVolume = true;
		parameters.manualVolume = static_cast<float>(volume);

		LoudnessCorrectionFilter filter(parameters);
		vector<wstring> channels(1, L"C");
		filter.initialize(static_cast<float>(sampleRate), 256, channels);
		vector<double> gains;
		double outputGainLinear = 1.0;
		LoudnessCorrectionFilterTestAccess::calculateTransfer(
			filter, volume, gains, outputGainLinear);

		const double subsonicFrequencies[] = { 1.0, 5.0, 10.0, 15.0, 19.0 };
		double maximumSubsonicError = 0.0;
		for (double frequency : subsonicFrequencies)
		{
			double response = LoudnessCorrectionFilterTestAccess::guardedResponseDb(
				filter, gains, outputGainLinear, frequency);
			if (!std::isfinite(response))
				return false;
			maximumSubsonicError = (std::max)(maximumSubsonicError,
				std::abs(response));
		}
		double maximumResponse =
			LoudnessCorrectionFilterTestAccess::maximumGuardedResponseDb(
				filter, gains, outputGainLinear);
		double rawMaximumResponse =
			LoudnessCorrectionFilterTestAccess::maximumRawCorrectionResponseDb(
				filter, gains);
		// Peak normalization must not add a fixed audible loss. The complete
		// L + H*C verification below can still reduce this candidate if needed.
		double expectedOutputGain = rawMaximumResponse <= 0.0 ? 1.0 :
			std::pow(10.0, -rawMaximumResponse / 20.0);
		const double probeFrequencies[] = { 31.5, 80.0, 1000.0 };
		double probeResponses[3] = {};
		double probeCorrectionDeltas[3] = {};
		for (size_t index = 0; index < 3; ++index)
		{
			probeResponses[index] =
				LoudnessCorrectionFilterTestAccess::guardedResponseDb(
					filter, gains, outputGainLinear, probeFrequencies[index]);
			double lowpassOnly =
				LoudnessCorrectionFilterTestAccess::guardedResponseDb(
					filter, gains, 0.0, probeFrequencies[index]);
			probeCorrectionDeltas[index] =
				std::abs(probeResponses[index] - lowpassOnly);
		}
		printf(
			"Loudness guarded transfer %s: gain %.12f expected %.12f "
			"subsonic error %.9f dB peak %.9f dB probes %.6f/%.6f/%.6f dB\n",
			name, outputGainLinear, expectedOutputGain, maximumSubsonicError,
			maximumResponse,
			probeResponses[0], probeResponses[1], probeResponses[2]);
		if (!std::isfinite(outputGainLinear) || outputGainLinear <= 0.0 ||
			outputGainLinear > 1.0 ||
			std::abs(outputGainLinear - expectedOutputGain) >
				1.0e-9 * (std::max)(1.0, expectedOutputGain) ||
			probeCorrectionDeltas[0] <= 1.0e-3 ||
			probeCorrectionDeltas[1] <= 1.0e-3 ||
			probeCorrectionDeltas[2] <= 1.0e-3 ||
			maximumSubsonicError > 0.01 || maximumResponse > 1.0e-6)
		{
			fprintf(stderr,
				"%s violated subsonic unity or full-transfer headroom.\n", name);
			return false;
		}
		return true;
	}

	bool runLoudnessIdentityCase(
		const char* name,
		bool state,
		float attenuation,
		float volume)
	{
		const unsigned sampleRate = 48000;
		const unsigned batchSize = 256;
		LoudnessCorrectionFilter::FilterParameters parameters;
		parameters.state = state;
		parameters.referenceLevel = 80.0f;
		parameters.referenceOffset = 0.0f;
		parameters.attenuation = attenuation;
		parameters.useManualVolume = true;
		parameters.manualVolume = volume;

		LoudnessCorrectionFilter filter(parameters);
		vector<wstring> channels(1, L"C");
		filter.initialize(static_cast<float>(sampleRate), batchSize, channels);
		double inputStorage[batchSize];
		double outputStorage[batchSize];
		double* inputChannels[] = { inputStorage };
		double* outputChannels[] = { outputStorage };
		for (unsigned index = 0; index < batchSize; ++index)
		{
			inputStorage[index] =
				static_cast<double>(static_cast<int>(index % 37) - 18) / 19.0;
			outputStorage[index] = 0.0;
		}
		filter.process(outputChannels, inputChannels, batchSize);
		for (unsigned index = 0; index < batchSize; ++index)
		{
			if (outputStorage[index] != inputStorage[index])
			{
				fprintf(stderr, "%s was not bit-transparent.\n", name);
				return false;
			}
		}
		printf("Loudness identity %s: bit-transparent\n", name);
		return true;
	}

	bool runLoudnessSubsonicSineCase(double frequency)
	{
		const unsigned sampleRate = 48000;
		const unsigned batchSize = 256;
		const unsigned totalFrames = sampleRate * 5;
		const unsigned measureFrom = sampleRate * 4;
		LoudnessCorrectionFilter::FilterParameters parameters;
		parameters.state = true;
		parameters.referenceLevel = 100.0f;
		parameters.referenceOffset = 0.0f;
		parameters.attenuation = 1.0f;
		parameters.useManualVolume = true;
		parameters.manualVolume = -100.0f;

		LoudnessCorrectionFilter filter(parameters);
		vector<wstring> channels(1, L"C");
		filter.initialize(static_cast<float>(sampleRate), batchSize, channels);
		if (!LoudnessCorrectionFilterTestAccess::crossoverPrewarmActive(filter) ||
			LoudnessCorrectionFilterTestAccess::crossoverPrewarmLength(filter) !=
				sampleRate)
		{
			fprintf(stderr, "Initial LR28 prewarm timing regressed.\n");
			return false;
		}
		double inputStorage[batchSize];
		double outputStorage[batchSize];
		double* inputChannels[] = { inputStorage };
		double* outputChannels[] = { outputStorage };
		double inputPower = 0.0;
		double outputPower = 0.0;
		double maximumOutput = 0.0;
		unsigned frame = 0;
		while (frame < totalFrames)
		{
			unsigned frameCount = (std::min)(batchSize, totalFrames - frame);
			for (unsigned index = 0; index < frameCount; ++index)
			{
				double time = static_cast<double>(frame + index) /
					static_cast<double>(sampleRate);
				inputStorage[index] = std::sin(2.0 * M_PI * frequency * time);
				outputStorage[index] = 0.0;
			}
			filter.process(outputChannels, inputChannels, frameCount);
			for (unsigned index = 0; index < frameCount; ++index)
			{
				double outputSample = outputStorage[index];
				if (!std::isfinite(outputSample))
					return false;
				maximumOutput = (std::max)(maximumOutput, std::abs(outputSample));
				if (frame + index >= measureFrom)
				{
					inputPower += inputStorage[index] * inputStorage[index];
					outputPower += outputSample * outputSample;
				}
			}
			frame += frameCount;
		}

		double responseDb = 10.0 * std::log10(outputPower / inputPower);
		printf(
			"Loudness subsonic sine %.1f Hz: response %.9f dB maximum %.9f\n",
			frequency, responseDb, maximumOutput);
		if (std::abs(responseDb) > 0.01 || maximumOutput > 1.000001)
		{
			fprintf(stderr,
				"%.1f Hz end-to-end response violated unity or clipped.\n",
				frequency);
			return false;
		}
		return true;
	}

	bool runLoudnessCrossoverReloadHandoffCase()
	{
		LoudnessCorrectionFilterTestAccess::resetCrossoverHandoffRegistry();

		const unsigned sampleRate = 48000;
		const unsigned batchSize = 256;
		const unsigned prewarmFrames = sampleRate * 2;

		LoudnessCorrectionFilter::FilterParameters params1;
		params1.state = true;
		params1.referenceLevel = 80.0f;
		params1.referenceOffset = 0.0f;
		params1.attenuation = 1.0f;
		params1.useManualVolume = true;
		params1.manualVolume = -30.0f;

		LoudnessCorrectionFilter filter1(params1);
		vector<wstring> channels{ L"L", L"R" };
		filter1.initialize(static_cast<float>(sampleRate), batchSize, channels);

		if (!LoudnessCorrectionFilterTestAccess::crossoverPrewarmActive(filter1))
		{
			fprintf(stderr, "Filter 1 should have prewarm active on cold start.\n");
			return false;
		}

		vector<double> inL(batchSize, 0.0);
		vector<double> inR(batchSize, 0.0);
		vector<double> outL(batchSize, 0.0);
		vector<double> outR(batchSize, 0.0);
		double* inChannels[] = { inL.data(), inR.data() };
		double* outChannels[] = { outL.data(), outR.data() };

		unsigned framesRun = 0;
		while (framesRun < prewarmFrames)
		{
			for (unsigned i = 0; i < batchSize; ++i)
			{
				double t = static_cast<double>(framesRun + i) / sampleRate;
				inL[i] = 0.5 * std::sin(2.0 * M_PI * 100.0 * t);
				inR[i] = 0.5 * std::cos(2.0 * M_PI * 100.0 * t);
			}
			filter1.process(outChannels, inChannels, batchSize);
			framesRun += batchSize;
		}

		if (LoudnessCorrectionFilterTestAccess::crossoverPrewarmActive(filter1) ||
			!LoudnessCorrectionFilterTestAccess::crossoverDomainReady(filter1))
		{
			fprintf(stderr, "Filter 1 failed to reach crossover domain.\n");
			return false;
		}

		// Simulate config reload with new parameters
		LoudnessCorrectionFilter::FilterParameters params2 = params1;
		params2.attenuation = 0.6f;
		params2.manualVolume = -25.0f;

		LoudnessCorrectionFilter filter2(params2);
		filter2.initialize(static_cast<float>(sampleRate), batchSize, channels);

		// Before first process() block, filter2 has _crossoverPrewarmActive = true
		if (!LoudnessCorrectionFilterTestAccess::crossoverPrewarmActive(filter2))
		{
			fprintf(stderr, "Filter 2 prewarm flag should start true before first process block.\n");
			return false;
		}

		// Process first block of filter2
		for (unsigned i = 0; i < batchSize; ++i)
		{
			double t = static_cast<double>(framesRun + i) / sampleRate;
			inL[i] = 0.5 * std::sin(2.0 * M_PI * 100.0 * t);
			inR[i] = 0.5 * std::cos(2.0 * M_PI * 100.0 * t);
		}
		filter2.process(outChannels, inChannels, batchSize);

		// In the very first block, filter2 must have adopted the live crossover history!
		if (LoudnessCorrectionFilterTestAccess::crossoverPrewarmActive(filter2) ||
			!LoudnessCorrectionFilterTestAccess::crossoverDomainReady(filter2))
		{
			fprintf(stderr, "Filter 2 failed to adopt live crossover history on reload!\n");
			return false;
		}

		// Verify output is finite
		for (unsigned i = 0; i < batchSize; ++i)
		{
			if (!std::isfinite(outL[i]) || !std::isfinite(outR[i]))
			{
				fprintf(stderr, "Filter 2 produced non-finite output after reload handoff.\n");
				return false;
			}
		}

		// Sample rate mismatch must NOT adopt
		LoudnessCorrectionFilter filterRateMismatch(params2);
		filterRateMismatch.initialize(96000.0f, batchSize, channels);
		vector<double> in96(batchSize, 0.1);
		vector<double> out96(batchSize, 0.0);
		double* inM[] = { in96.data(), in96.data() };
		double* outM[] = { out96.data(), out96.data() };
		filterRateMismatch.process(outM, inM, batchSize);
		if (!LoudnessCorrectionFilterTestAccess::crossoverPrewarmActive(filterRateMismatch))
		{
			fprintf(stderr, "Filter with mismatched sample rate should NOT adopt crossover history.\n");
			return false;
		}

		LoudnessCorrectionFilterTestAccess::resetCrossoverHandoffRegistry();
		printf("Loudness crossover reload handoff: passed\n");
		return true;
	}

	bool runLoudnessTransitionCase(
		const char* name,
		unsigned sampleRate,
		double frequency,
		double initialVolume,
		double targetVolume,
		double phase)
	{
		const unsigned batchSize = 256;
		const unsigned switchFrame = sampleRate * 3;
		const unsigned totalFrames = sampleRate * 5;
		LoudnessCorrectionFilter::FilterParameters parameters;
		parameters.state = true;
		parameters.referenceLevel = 100.0f;
		parameters.referenceOffset = 0.0f;
		parameters.attenuation = 1.0f;
		parameters.useManualVolume = true;
		parameters.manualVolume = static_cast<float>(initialVolume);

		LoudnessCorrectionFilter filter(parameters);
		vector<wstring> channels(1, L"C");
		filter.initialize(static_cast<float>(sampleRate), batchSize, channels);

		double inputStorage[batchSize];
		double outputStorage[batchSize];
		double* inputChannels[] = { inputStorage };
		double* outputChannels[] = { outputStorage };
		double maximumOutput = 0.0;
		unsigned frame = 0;
		while (frame < totalFrames)
		{
			if (frame == switchFrame)
			{
				LoudnessCorrectionFilterTestAccess::publishVolumeUpdate(
					filter, targetVolume);
			}

			unsigned frameCount = (std::min)(batchSize, totalFrames - frame);
			if (frame < switchFrame && frame + frameCount > switchFrame)
				frameCount = switchFrame - frame;

			for (unsigned index = 0; index < frameCount; ++index)
			{
				double time = static_cast<double>(frame + index) /
					static_cast<double>(sampleRate);
				inputStorage[index] =
					std::sin(2.0 * M_PI * frequency * time + phase);
				outputStorage[index] = 0.0;
			}
			filter.process(outputChannels, inputChannels, frameCount);
			for (unsigned index = 0; index < frameCount; ++index)
			{
				double magnitude = std::abs(outputStorage[index]);
				if (!std::isfinite(magnitude))
				{
					fprintf(stderr, "%s produced a non-finite sample.\n", name);
					return false;
				}
				maximumOutput = (std::max)(maximumOutput, magnitude);
			}
			frame += frameCount;
		}

		printf("Loudness transition %s: maximum output %.9f\n", name, maximumOutput);
		if (maximumOutput > 1.000001)
		{
			fprintf(stderr, "%s exceeded full scale.\n", name);
			return false;
		}
		return true;
	}





	bool runLoudnessAdaptiveHandoffSweep()
	{
		const unsigned sampleRates[] = {
			8000, 44100, 48000, 96000, 192000, 384000
		};
		const double phases[] = { 0.0, M_PI / 4.0, M_PI / 2.0, 3.0 * M_PI / 4.0 };
		bool passed = true;
		for (unsigned sampleRate : sampleRates)
		{
			LoudnessCorrectionFilter::FilterParameters parameters;
			parameters.state = true;
			parameters.referenceLevel = 100.0f;
			parameters.attenuation = 1.0f;
			parameters.useManualVolume = true;
			parameters.manualVolume = 0.0f;
			LoudnessCorrectionFilter filter(parameters);
			vector<wstring> channels(1, L"C");
			filter.initialize(static_cast<float>(sampleRate), 64, channels);

			size_t caseCount = 0;
			double maximumSecondDifferenceRatio = 0.0;
			double maximumWaitSeconds = 0.0;
			auto checkFrequency = [&](double frequency)
			{
				complex<double> identityResponse =
					LoudnessCorrectionFilterTestAccess::crossoverIdentityResponse(
						filter, frequency);
				double omega = 2.0 * M_PI * frequency /
					static_cast<double>(sampleRate);
				unsigned maximumWait = static_cast<unsigned>(std::ceil(
					static_cast<double>(sampleRate) / (2.0 * frequency))) + 8;
				for (double phase : phases)
				{
					bool switched = false;
					double previousRaw = 0.0;
					double previousIdentity = 0.0;
					for (unsigned sample = 0; sample <= maximumWait; ++sample)
					{
						double angle = omega * static_cast<double>(sample) + phase;
						double raw = std::sin(angle);
						double identity = std::imag(
							identityResponse * std::exp(complex<double>(0.0, angle)));
						if (sample > 0)
						{
							double handoffStep = 0.0;
							double naturalStep = 0.0;
							if (LoudnessCorrectionFilterTestAccess::isSafeCrossoverHandoff(
								previousRaw,
								raw,
								previousIdentity,
								identity,
								handoffStep,
								naturalStep))
							{
								double nextAngle = angle + omega;
								double nextIdentity = std::imag(identityResponse *
									std::exp(complex<double>(0.0, nextAngle)));
								double secondDifference = std::abs(
									nextIdentity - 2.0 * identity + previousRaw);
								double firstDifferenceBound =
									2.0 * std::sin(M_PI * frequency /
										static_cast<double>(sampleRate));
								double secondDifferenceBound =
									2.0 * firstDifferenceBound + 1.0e-9;
								maximumSecondDifferenceRatio = (std::max)(
									maximumSecondDifferenceRatio,
									secondDifference /
										(std::max)(firstDifferenceBound, 1.0e-15));
								maximumWaitSeconds = (std::max)(
									maximumWaitSeconds,
									static_cast<double>(sample) /
										static_cast<double>(sampleRate));
								if (handoffStep > naturalStep + 1.0e-12 ||
									secondDifference > secondDifferenceBound)
								{
									fprintf(stderr,
										"Adaptive handoff bounds failed at %u Hz/%.6f Hz.\n",
										sampleRate, frequency);
									passed = false;
								}
								switched = true;
								break;
							}
						}
						previousRaw = raw;
						previousIdentity = identity;
					}
					if (!switched)
					{
						fprintf(stderr,
							"Adaptive handoff did not complete at %u Hz/%.6f Hz.\n",
							sampleRate, frequency);
						passed = false;
					}
					++caseCount;
				}
			};

			unsigned upperFrequency = (std::min)(
				4000u,
				static_cast<unsigned>(std::floor(0.499 * sampleRate)));
			for (unsigned frequency = 1; frequency <= upperFrequency; ++frequency)
				checkFrequency(static_cast<double>(frequency));
			checkFrequency(4.38);
			checkFrequency(12.7711);
			printf(
				"Loudness adaptive handoff sweep %u Hz: %zu cases, "
				"max wait %.6f s, max second-difference/B1 %.6f\n",
				sampleRate, caseCount, maximumWaitSeconds,
				maximumSecondDifferenceRatio);
		}
		return passed;
	}

	bool runLoudnessExactHandoffCase(
		unsigned sampleRate,
		double frequency,
		double phase)
	{
		const unsigned batchSize = 64;
		LoudnessCorrectionFilter::FilterParameters parameters;
		parameters.state = true;
		parameters.referenceLevel = 100.0f;
		parameters.attenuation = 1.0f;
		parameters.useManualVolume = true;
		parameters.manualVolume = 0.0f;
		LoudnessCorrectionFilter filter(parameters);
		vector<wstring> channels(1, L"C");
		filter.initialize(static_cast<float>(sampleRate), batchSize, channels);

		double inputStorage[batchSize];
		double outputStorage[batchSize];
		double* inputChannels[] = { inputStorage };
		double* outputChannels[] = { outputStorage };
		double previousOutput = 0.0;
		double previousPreviousOutput = 0.0;
		bool havePrevious = false;
		bool havePreviousPrevious = false;
		double maximumSecondDifference = 0.0;
		unsigned frame = 0;
		unsigned maximumFrame =
			LoudnessCorrectionFilterTestAccess::crossoverPrewarmLength(filter) +
			static_cast<unsigned>(std::ceil(
				static_cast<double>(sampleRate) / (2.0 * frequency))) +
			4 * batchSize;
		while (!LoudnessCorrectionFilterTestAccess::crossoverDomainReady(filter) &&
			frame < maximumFrame)
		{
			unsigned frameCount = (std::min)(batchSize, maximumFrame - frame);
			for (unsigned index = 0; index < frameCount; ++index)
			{
				double time = static_cast<double>(frame + index) /
					static_cast<double>(sampleRate);
				inputStorage[index] =
					std::sin(2.0 * M_PI * frequency * time + phase);
				outputStorage[index] = 0.0;
			}
			filter.process(outputChannels, inputChannels, frameCount);
			for (unsigned index = 0; index < frameCount; ++index)
			{
				double sample = outputStorage[index];
				if (!std::isfinite(sample))
					return false;
				if (havePreviousPrevious)
				{
					maximumSecondDifference = (std::max)(
						maximumSecondDifference,
						std::abs(sample - 2.0 * previousOutput +
							previousPreviousOutput));
				}
				previousPreviousOutput = previousOutput;
				previousOutput = sample;
				havePreviousPrevious = havePrevious;
				havePrevious = true;
			}
			frame += frameCount;
		}

		double firstDifferenceBound = 2.0 * std::sin(
			M_PI * frequency / static_cast<double>(sampleRate));
		double secondDifferenceBound = 2.0 * firstDifferenceBound + 1.0e-8;
		double maximumHandoffStep =
			LoudnessCorrectionFilterTestAccess::maximumCrossoverHandoffStep(filter);
		bool passed =
			LoudnessCorrectionFilterTestAccess::crossoverDomainReady(filter) &&
			maximumHandoffStep <= firstDifferenceBound + 1.0e-6 &&
			maximumSecondDifference <= secondDifferenceBound;
		if (!passed)
		{
			fprintf(stderr,
				"Exact adaptive handoff failed at %u Hz/%.6f Hz/phase %.3f: "
				"frame %u, step %.9g, second %.9g.\n",
				sampleRate, frequency, phase, frame,
				maximumHandoffStep, maximumSecondDifference);
		}
		else
		{
			printf(
				"Loudness exact handoff %u Hz/%.6f Hz/phase %.3f: "
				"step %.9g, second %.9g\n",
				sampleRate, frequency, phase,
				maximumHandoffStep, maximumSecondDifference);
		}
		return passed;
	}

	enum LoudnessFailureStage
	{
		FAILURE_AT_SETTLED,
		FAILURE_AT_WARMUP,
		FAILURE_AT_CROSSFADE
	};

	bool runLoudnessCommonDomainCase(
		const char* name,
		unsigned sampleRate,
		double frequency,
		unsigned earlyUpdateDelayMs,
		LoudnessFailureStage failureStage,
		bool exerciseIdentityUpdates,
		bool repeatFailureDuringRecovery)
	{
		const unsigned batchSize = 64;
		const double minimumEnvelope = std::pow(10.0, -0.01 / 20.0);
		LoudnessCorrectionFilter::FilterParameters parameters;
		parameters.state = true;
		parameters.referenceLevel = 100.0f;
		parameters.referenceOffset = 0.0f;
		parameters.attenuation = 1.0f;
		parameters.useManualVolume = true;
		parameters.manualVolume = 0.0f;
		LoudnessCorrectionFilter filter(parameters);
		vector<wstring> channels{ L"I", L"Q" };
		filter.initialize(static_cast<float>(sampleRate), batchSize, channels);

		double inputI[batchSize];
		double inputQ[batchSize];
		double outputI[batchSize];
		double outputQ[batchSize];
		double* inputChannels[] = { inputI, inputQ };
		double* outputChannels[] = { outputI, outputQ };
		unsigned absoluteFrame = 0;
		double measuredMinimumEnvelope =
			std::numeric_limits<double>::infinity();
		double measuredMaximumEnvelope = 0.0;
		double maximumBypassFirstDifference = 0.0;
		double previousOutputs[2] = {};
		bool havePreviousOutputs = false;
		bool trackBypassDifferences = false;
		bool bypassFadeWasActive = false;
		unsigned bypassFadeStartCount = 0;

		auto processFrames = [&](unsigned requestedFrames)
		{
			while (requestedFrames > 0)
			{
				unsigned frameCount = (std::min)(batchSize, requestedFrames);
				bool measureCommonDomain =
					LoudnessCorrectionFilterTestAccess::crossoverDomainReady(filter);
				for (unsigned index = 0; index < frameCount; ++index)
				{
					double time = static_cast<double>(absoluteFrame + index) /
						static_cast<double>(sampleRate);
					double angle = 2.0 * M_PI * frequency * time;
					inputI[index] = std::cos(angle);
					inputQ[index] = std::sin(angle);
					outputI[index] = 0.0;
					outputQ[index] = 0.0;
				}
				filter.process(outputChannels, inputChannels, frameCount);
				for (unsigned index = 0; index < frameCount; ++index)
				{
					if (!std::isfinite(outputI[index]) ||
						!std::isfinite(outputQ[index]))
					{
						return false;
					}
					if (measureCommonDomain)
					{
						double envelope = std::hypot(
							outputI[index], outputQ[index]);
						measuredMinimumEnvelope = (std::min)(
							measuredMinimumEnvelope, envelope);
						measuredMaximumEnvelope = (std::max)(
							measuredMaximumEnvelope, envelope);
					}
					if (trackBypassDifferences && havePreviousOutputs)
					{
						maximumBypassFirstDifference = (std::max)(
							maximumBypassFirstDifference,
							(std::max)(
								std::abs(outputI[index] - previousOutputs[0]),
								std::abs(outputQ[index] - previousOutputs[1])));
					}
					previousOutputs[0] = outputI[index];
					previousOutputs[1] = outputQ[index];
					havePreviousOutputs = true;
				}
				absoluteFrame += frameCount;
				requestedFrames -= frameCount;
				bool bypassFadeIsActive =
					LoudnessCorrectionFilterTestAccess::bypassFadeActive(filter);
				if (bypassFadeIsActive && !bypassFadeWasActive)
					++bypassFadeStartCount;
				bypassFadeWasActive = bypassFadeIsActive;
				if (trackBypassDifferences &&
					!LoudnessCorrectionFilterTestAccess::bypassFadeActive(filter) &&
					LoudnessCorrectionFilterTestAccess::runtimeBypass(filter))
				{
					trackBypassDifferences = false;
				}
			}
			return true;
		};

		auto waitUntil = [&](auto predicate, unsigned maximumFrames)
		{
			unsigned startFrame = absoluteFrame;
			while (!predicate() && absoluteFrame - startFrame < maximumFrames)
			{
				unsigned remaining =
					maximumFrames - (absoluteFrame - startFrame);
				if (!processFrames((std::min)(batchSize, remaining)))
					return false;
			}
			return predicate();
		};

		unsigned updateDelayFrames =
			sampleRate * earlyUpdateDelayMs / 1000;
		if (!processFrames(updateDelayFrames))
			return false;
		LoudnessCorrectionFilterTestAccess::publishVolumeUpdate(filter, -100.0);

		unsigned maximumHandoffFrame =
			LoudnessCorrectionFilterTestAccess::crossoverPrewarmLength(filter) +
			static_cast<unsigned>(std::ceil(
				static_cast<double>(sampleRate) / (2.0 * frequency))) +
			4 * batchSize;
		if (absoluteFrame >= maximumHandoffFrame ||
			!waitUntil([&]()
			{
				return LoudnessCorrectionFilterTestAccess::crossoverDomainReady(
					filter);
			}, maximumHandoffFrame - absoluteFrame))
		{
			fprintf(stderr, "%s did not enter the common A domain.\n", name);
			return false;
		}

		auto waitForNonIdentity = [&]()
		{
			return waitUntil([&]()
			{
				return LoudnessCorrectionFilterTestAccess::settledOnNonIdentityBank(
					filter);
			}, sampleRate);
		};
		if (exerciseIdentityUpdates || failureStage == FAILURE_AT_SETTLED)
		{
			if (!waitForNonIdentity())
				return false;
		}
		if (exerciseIdentityUpdates)
		{
			LoudnessCorrectionFilterTestAccess::publishVolumeUpdate(filter, 0.0);
			if (!waitUntil([&]()
			{
				return LoudnessCorrectionFilterTestAccess::settledOnIdentityBank(filter);
			}, sampleRate))
			{
				return false;
			}
			LoudnessCorrectionFilterTestAccess::publishVolumeUpdate(filter, -100.0);
			if (!waitForNonIdentity())
				return false;
		}
		else if (failureStage == FAILURE_AT_WARMUP)
		{
			if (!waitUntil([&]()
			{
				return LoudnessCorrectionFilterTestAccess::warmupActive(filter);
			}, sampleRate / 2))
			{
				return false;
			}
			while (LoudnessCorrectionFilterTestAccess::warmupActive(filter) &&
				LoudnessCorrectionFilterTestAccess::warmupPosition(filter) <
					LoudnessCorrectionFilterTestAccess::warmupLength(filter) / 2)
			{
				if (!processFrames(batchSize))
					return false;
			}
		}
		else if (failureStage == FAILURE_AT_CROSSFADE)
		{
			if (!waitUntil([&]()
			{
				return LoudnessCorrectionFilterTestAccess::crossfadeActive(filter);
			}, sampleRate))
			{
				return false;
			}
			while (LoudnessCorrectionFilterTestAccess::crossfadeActive(filter) &&
				LoudnessCorrectionFilterTestAccess::crossfadePosition(filter) <
					LoudnessCorrectionFilterTestAccess::crossfadeLength(filter) / 2)
			{
				if (!processFrames(batchSize))
					return false;
			}
		}

		LoudnessCorrectionFilterTestAccess::beginRuntimeBypass(filter);
		trackBypassDifferences = true;
		unsigned recoveryDelayFrames =
			sampleRate * earlyUpdateDelayMs / 1000;
		if (recoveryDelayFrames > 0 && !processFrames(recoveryDelayFrames))
			return false;
		LoudnessCorrectionFilterTestAccess::publishRuntimeRecovery(filter, -100.0);

		if (repeatFailureDuringRecovery)
		{
			if (!waitUntil([&]()
			{
				return !LoudnessCorrectionFilterTestAccess::runtimeBypass(filter) &&
					LoudnessCorrectionFilterTestAccess::warmupActive(filter);
			}, sampleRate))
			{
				return false;
			}
			LoudnessCorrectionFilterTestAccess::beginRuntimeBypass(filter);
			LoudnessCorrectionFilterTestAccess::publishRuntimeRecovery(filter, -100.0);
			trackBypassDifferences = true;
		}
		if (!waitForNonIdentity())
			return false;

		double firstDifferenceBound =
			2.0 * std::sin(M_PI * frequency /
				static_cast<double>(sampleRate)) +
			2.0 / static_cast<double>((std::max)(
				2u,
				LoudnessCorrectionFilterTestAccess::bypassFadeLength(filter)) - 1) +
			2.0 / static_cast<double>((std::max)(
				2u,
				LoudnessCorrectionFilterTestAccess::crossfadeLength(filter)) - 1) +
			1.0e-6;
		bool passed = std::isfinite(measuredMinimumEnvelope) &&
			measuredMinimumEnvelope >= minimumEnvelope &&
			measuredMaximumEnvelope <= 1.000001 &&
			maximumBypassFirstDifference <= firstDifferenceBound &&
			LoudnessCorrectionFilterTestAccess::bypassFadeLength(filter) ==
				static_cast<unsigned>(std::lround(sampleRate * 0.01)) &&
			bypassFadeStartCount >= (repeatFailureDuringRecovery ? 2u : 1u);
		printf(
			"Loudness common-A %s: envelope %.9f..%.9f, bypass step %.9f "
			"(bound %.9f), handoff %.6f s\n",
			name, measuredMinimumEnvelope, measuredMaximumEnvelope,
			maximumBypassFirstDifference, firstDifferenceBound,
			static_cast<double>(absoluteFrame) / static_cast<double>(sampleRate));
		if (!passed)
		{
			fprintf(stderr,
				"%s violated the common-A envelope, headroom, or bypass-fade contract.\n",
				name);
		}
		return passed;
	}

	bool runLoudnessPartialHandoffFailureCase(unsigned sampleRate)
	{
		const unsigned batchSize = 64;
		const double frequency = 1.0;
		LoudnessCorrectionFilter::FilterParameters parameters;
		parameters.state = true;
		parameters.referenceLevel = 100.0f;
		parameters.referenceOffset = 0.0f;
		parameters.attenuation = 1.0f;
		parameters.useManualVolume = true;
		parameters.manualVolume = -100.0f;
		LoudnessCorrectionFilter filter(parameters);
		vector<wstring> channels{ L"I", L"Q" };
		filter.initialize(static_cast<float>(sampleRate), batchSize, channels);

		double inputI[batchSize];
		double inputQ[batchSize];
		double outputI[batchSize];
		double outputQ[batchSize];
		double* inputChannels[] = { inputI, inputQ };
		double* outputChannels[] = { outputI, outputQ };
		unsigned absoluteFrame = 0;
		auto processBlock = [&]()
		{
			for (unsigned index = 0; index < batchSize; ++index)
			{
				double time = static_cast<double>(absoluteFrame + index) /
					static_cast<double>(sampleRate);
				double angle = 2.0 * M_PI * frequency * time;
				inputI[index] = std::cos(angle);
				inputQ[index] = std::sin(angle);
				outputI[index] = 0.0;
				outputQ[index] = 0.0;
			}
			filter.process(outputChannels, inputChannels, batchSize);
			absoluteFrame += batchSize;
			for (unsigned index = 0; index < batchSize; ++index)
			{
				if (!std::isfinite(outputI[index]) ||
					!std::isfinite(outputQ[index]))
				{
					return false;
				}
			}
			return true;
		};

		bool observedPartialHandoff = false;
		unsigned partialDeadline =
			LoudnessCorrectionFilterTestAccess::crossoverPrewarmLength(filter) +
			sampleRate;
		while (absoluteFrame < partialDeadline)
		{
			if (!processBlock())
				return false;
			bool channel0 =
				LoudnessCorrectionFilterTestAccess::crossoverChannelInDomain(filter, 0);
			bool channel1 =
				LoudnessCorrectionFilterTestAccess::crossoverChannelInDomain(filter, 1);
			if (channel0 != channel1)
			{
				observedPartialHandoff = true;
				break;
			}
		}
		if (!observedPartialHandoff)
		{
			fprintf(stderr,
				"Partial-channel handoff was not observed at %u Hz.\n", sampleRate);
			return false;
		}

		LoudnessCorrectionFilterTestAccess::beginRuntimeBypass(filter);
		unsigned readyDeadline = absoluteFrame + sampleRate;
		while (!LoudnessCorrectionFilterTestAccess::crossoverDomainReady(filter) &&
			absoluteFrame < readyDeadline)
		{
			if (!processBlock() ||
				LoudnessCorrectionFilterTestAccess::warmupActive(filter) ||
				LoudnessCorrectionFilterTestAccess::crossfadeActive(filter) ||
				LoudnessCorrectionFilterTestAccess::bypassFadeActive(filter))
			{
				fprintf(stderr,
					"Partial-channel failure started an unsafe correction transition.\n");
				return false;
			}
		}
		if (!LoudnessCorrectionFilterTestAccess::crossoverDomainReady(filter))
			return false;

		double minimumEnvelope = std::numeric_limits<double>::infinity();
		double maximumEnvelope = 0.0;
		for (unsigned remaining = sampleRate / 4; remaining > 0;)
		{
			if (!processBlock())
				return false;
			for (unsigned index = 0; index < batchSize; ++index)
			{
				double envelope = std::hypot(outputI[index], outputQ[index]);
				minimumEnvelope = (std::min)(minimumEnvelope, envelope);
				maximumEnvelope = (std::max)(maximumEnvelope, envelope);
			}
			remaining = remaining > batchSize ? remaining - batchSize : 0;
		}

		bool passed =
			LoudnessCorrectionFilterTestAccess::holdingBypassOnIdentityBank(filter) &&
			minimumEnvelope >= std::pow(10.0, -0.01 / 20.0) &&
			maximumEnvelope <= 1.000001;
		printf(
			"Loudness partial handoff failure %u Hz: envelope %.9f..%.9f, %s\n",
			sampleRate, minimumEnvelope, maximumEnvelope,
			passed ? "passed" : "failed");
		return passed;
	}

	bool runLoudnessBlockProcessingCase(
		LoudnessCorrectionFilter::FilterParameters::EngineMode engine,
		bool inPlace)
	{
		const unsigned sampleRate = 48000;
		const unsigned channelCount = 2;
		const unsigned maximumFrameCount = 256;
		const unsigned storageFrameCount = maximumFrameCount + 1;
		const unsigned framePattern[] = {
			0, 1, 16, 64, maximumFrameCount - 1,
			maximumFrameCount, maximumFrameCount + 1
		};
		LoudnessCorrectionFilter::FilterParameters parameters;
		parameters.state = true;
		parameters.referenceLevel = 80.0f;
		parameters.referenceOffset = 0.0f;
		parameters.attenuation = 1.0f;
		parameters.useManualVolume = true;
		parameters.manualVolume = -38.0f;
		parameters.engine = engine;

		LoudnessCorrectionFilter blockFilter(parameters);
		LoudnessCorrectionFilter scalarFilter(parameters);
		FilterRuntimeContext context;
		context.offlineAnalysis = true;
		blockFilter.setRuntimeContext(context);
		scalarFilter.setRuntimeContext(context);
		vector<wstring> channels(channelCount, L"C");
		blockFilter.initialize(
			static_cast<float>(sampleRate), maximumFrameCount, channels);
		// A zero advertised capacity deliberately selects the existing scalar
		// fallback while leaving all filter coefficients and states unchanged.
		scalarFilter.initialize(static_cast<float>(sampleRate), 0, channels);

		vector<vector<double>> input(
			channelCount, vector<double>(storageFrameCount));
		vector<vector<double>> blockOutput(
			channelCount, vector<double>(storageFrameCount));
		vector<vector<double>> scalarOutput(
			channelCount, vector<double>(storageFrameCount));
		vector<double*> inputChannels(channelCount);
		vector<double*> blockInputChannels(channelCount);
		vector<double*> scalarInputChannels(channelCount);
		vector<double*> blockOutputChannels(channelCount);
		vector<double*> scalarOutputChannels(channelCount);
		double maximumError = 0.0;
		bool finite = true;
		unsigned samplePosition = 0;
		for (unsigned block = 0; block < 256; ++block)
		{
			const unsigned frameCount = framePattern[
				block % (sizeof(framePattern) / sizeof(framePattern[0]))];
			if (block == 64)
			{
				LoudnessCorrectionFilterTestAccess::publishVolumeUpdate(
					blockFilter, -52.0);
				LoudnessCorrectionFilterTestAccess::publishVolumeUpdate(
					scalarFilter, -52.0);
			}
			for (unsigned channel = 0; channel < channelCount; ++channel)
			{
				for (unsigned frame = 0; frame < frameCount; ++frame)
				{
					const double time = static_cast<double>(
						samplePosition + frame) / sampleRate;
					const double value =
						0.21 * sin(2.0 * M_PI * (37.0 + 311.0 * channel) * time) +
						0.13 * cos(2.0 * M_PI * (997.0 + 701.0 * channel) * time);
					input[channel][frame] = value;
					blockOutput[channel][frame] = value;
					scalarOutput[channel][frame] = value;
				}
				inputChannels[channel] = input[channel].data();
				blockOutputChannels[channel] = blockOutput[channel].data();
				scalarOutputChannels[channel] = scalarOutput[channel].data();
				blockInputChannels[channel] = inPlace ?
					blockOutput[channel].data() : input[channel].data();
				scalarInputChannels[channel] = inPlace ?
					scalarOutput[channel].data() : input[channel].data();
			}

			blockFilter.process(
				blockOutputChannels.data(), blockInputChannels.data(), frameCount);
			scalarFilter.process(
				scalarOutputChannels.data(), scalarInputChannels.data(), frameCount);
			for (unsigned channel = 0; channel < channelCount; ++channel)
			{
				for (unsigned frame = 0; frame < frameCount; ++frame)
				{
					finite = finite && isfinite(blockOutput[channel][frame]) &&
						isfinite(scalarOutput[channel][frame]);
					maximumError = (std::max)(maximumError, abs(
						blockOutput[channel][frame] - scalarOutput[channel][frame]));
				}
			}
			samplePosition += frameCount;
		}

		const bool passed = finite && maximumError <= 1.0e-12;
		printf(
			"Loudness %s mixed block/scalar %s: maximum error %.3g, %s\n",
			engine == LoudnessCorrectionFilter::FilterParameters::ENGINE_FAST ?
				"Fast" : "Full",
			inPlace ? "in-place" : "out-of-place",
			maximumError,
			passed ? "passed" : "failed");
		return passed;
	}

	bool runLoudnessResponseAggregationTests()
	{
		const unsigned sampleRates[] = { 8000, 48000, 384000 };
		const double volumes[] = { -100.0, -38.0, 0.0 };
		double maximumError = 0.0;
		bool finite = true;
		for (unsigned sampleRate : sampleRates)
		{
			for (double volume : volumes)
			{
				LoudnessCorrectionFilter::FilterParameters parameters;
				parameters.state = true;
				parameters.referenceLevel = 80.0f;
				parameters.referenceOffset = 0.0f;
				parameters.attenuation = 1.0f;
				parameters.useManualVolume = true;
				parameters.manualVolume = static_cast<float>(volume);
				LoudnessCorrectionFilter filter(parameters);
				FilterRuntimeContext context;
				context.offlineAnalysis = true;
				filter.setRuntimeContext(context);
				filter.initialize(
					static_cast<float>(sampleRate), 256, vector<wstring>(1, L"C"));
				vector<double> gains;
				double outputGainLinear = 1.0;
				LoudnessCorrectionFilterTestAccess::calculateTransfer(
					filter, volume, gains, outputGainLinear);

				const double minimumFrequency = 1.0;
				const double maximumFrequency = (std::min)(
					20000.0, 0.499 * static_cast<double>(sampleRate));
				const double logMinimum = log(minimumFrequency);
				const double logMaximum = log(maximumFrequency);
				for (unsigned point = 0; point < 1025; ++point)
				{
					const double frequency = exp(
						logMinimum + static_cast<double>(point) *
						(logMaximum - logMinimum) / 1024.0);
					const double legacy =
						LoudnessCorrectionFilterTestAccess::rawCorrectionResponseDb(
							filter, gains, frequency);
					const double aggregate =
						LoudnessCorrectionFilterTestAccess::aggregateRawCorrectionResponseDb(
							filter, gains, frequency);
					finite = finite && isfinite(legacy) && isfinite(aggregate);
					maximumError = (std::max)(
						maximumError, abs(legacy - aggregate));
				}
			}
		}

		const bool passed = finite && maximumError <= 1.0e-9;
		printf(
			"Loudness aggregate response: maximum legacy error %.3g dB, %s\n",
			maximumError,
			passed ? "passed" : "failed");
		return passed;
	}

	int runLoudnessTransitionTests()
	{
		bool passed = runLoudnessFormulaTests();
		passed = runOriginalLoudnessTests() && passed;
		passed = runVSTMidiBindingCodecTests() && passed;
		passed = runLoudnessParameterCodecTests() && passed;
		passed = runLoudnessVolumeFollowTests() && passed;
		passed = runLoudnessNearZeroHeadroomTests() && passed;
		passed = runLoudnessRuntimeContextTests() && passed;
		passed = runLoudnessOfflineAnalysisTests() && passed;
		passed = runFilterEngineDeviceInfoReuseTests() && passed;
		passed = runFilterEngineFailedReloadTransactionTests() && passed;
		passed = runLoudnessCrossoverCoefficientTests() && passed;
		passed = runLoudnessResponseAggregationTests() && passed;
		for (LoudnessCorrectionFilter::FilterParameters::EngineMode engine : {
			LoudnessCorrectionFilter::FilterParameters::ENGINE_FULL,
			LoudnessCorrectionFilter::FilterParameters::ENGINE_FAST })
		{
			passed = runLoudnessBlockProcessingCase(engine, false) && passed;
			passed = runLoudnessBlockProcessingCase(engine, true) && passed;
		}
		passed = runLoudnessAnchorFitCase(48000) && passed;
		passed = runLoudnessAnchorFitCase(192000) && passed;
		passed = runLoudnessGuardedTransferCase(
			"8k-extreme", 8000, 100.0, -100.0) && passed;
		passed = runLoudnessGuardedTransferCase(
			"48k-extreme", 48000, 100.0, -100.0) && passed;
		passed = runLoudnessGuardedTransferCase(
			"192k-extreme", 192000, 100.0, -100.0) && passed;
		passed = runLoudnessGuardedTransferCase(
			"384k-extreme", 384000, 100.0, -100.0) && passed;
		passed = runLoudnessIdentityCase(
			"State0", false, 1.0f, -100.0f) && passed;
		passed = runLoudnessIdentityCase(
			"zero-attenuation", true, 0.0f, -100.0f) && passed;
		LoudnessCorrectionFilterTestAccess::resetCrossoverHandoffRegistry();
		const double subsonicFrequencies[] = { 1.0, 5.0, 10.0, 15.0, 19.0 };
		for (double frequency : subsonicFrequencies)
			passed = runLoudnessSubsonicSineCase(frequency) && passed;
		passed = runLoudnessTransitionCase(
			"8k-minus100-to-0", 8000, 31.5, -100.0, 0.0, 0.0) && passed;
		passed = runLoudnessTransitionCase(
			"8k-0-to-minus100", 8000, 31.5, 0.0, -100.0, 0.0) && passed;
		passed = runLoudnessTransitionCase(
			"48k-inter-bin", 48000, 80.216, -40.0, 0.0, 0.0) && passed;
		passed = runLoudnessCrossoverReloadHandoffCase() && passed;

		passed = runLoudnessAdaptiveHandoffSweep() && passed;
		const unsigned handoffSampleRates[] = { 8000, 48000 };
		const double handoffPhases[] = { 0.0, M_PI / 2.0 };
		const double handoffCoreFrequencies[] = {
			1.0, 4.38, 5.0, 10.0, 12.5, 12.7711, 15.0, 19.0
		};
		for (unsigned sampleRate : handoffSampleRates)
		{
			for (double frequency : handoffCoreFrequencies)
			{
				for (double phase : handoffPhases)
				{
					passed = runLoudnessExactHandoffCase(
						sampleRate, frequency, phase) && passed;
				}
			}

			const double high8k[] = {
				80.0, 100.0, 125.0, 250.0, 400.0, 800.0, 1000.0, 3992.0
			};
			const double high48k[] = {
				160.0, 200.0, 400.0, 500.0, 1000.0, 2000.0, 4000.0
			};
			const double* highFrequencies =
				sampleRate == 8000 ? high8k : high48k;
			size_t highFrequencyCount = sampleRate == 8000 ?
				sizeof(high8k) / sizeof(high8k[0]) :
				sizeof(high48k) / sizeof(high48k[0]);
			for (size_t index = 0; index < highFrequencyCount; ++index)
			{
				for (double phase : handoffPhases)
				{
					passed = runLoudnessExactHandoffCase(
						sampleRate, highFrequencies[index], phase) && passed;
				}
			}
			passed = runLoudnessPartialHandoffFailureCase(sampleRate) && passed;
		}

		const unsigned commonDomainDelaysMs[] = { 0, 64 };
		for (unsigned sampleRate : handoffSampleRates)
		{
			for (double frequency : handoffCoreFrequencies)
			{
				for (unsigned delayMs : commonDomainDelaysMs)
				{
					char name[128];
					sprintf_s(name, "%uk-%.4fHz-delay-%ums-full",
						sampleRate / 1000, frequency, delayMs);
					bool repeatFailure = sampleRate == 48000 &&
						std::abs(frequency - 12.7711) < 1.0e-9 && delayMs == 0;
					passed = runLoudnessCommonDomainCase(
						name,
						sampleRate,
						frequency,
						delayMs,
						FAILURE_AT_SETTLED,
						true,
						repeatFailure) && passed;
				}
			}
			for (LoudnessFailureStage stage : {
				FAILURE_AT_WARMUP, FAILURE_AT_CROSSFADE })
			{
				char name[128];
				sprintf_s(name, "%uk-12.7711Hz-failure-%s",
					sampleRate / 1000,
					stage == FAILURE_AT_WARMUP ? "warmup" : "crossfade");
				passed = runLoudnessCommonDomainCase(
					name,
					sampleRate,
					12.7711,
					0,
					stage,
					false,
					false) && passed;
			}
		}

		return passed ? 0 : 1;
	}

	struct LoudnessPerformanceResult
	{
		double initializeMilliseconds;
		double updateMilliseconds;
		double processingNanosecondsPerSample;
		bool valid;
	};

	double median(vector<double> values)
	{
		sort(values.begin(), values.end());
		const size_t middle = values.size() / 2;
		if ((values.size() & 1) != 0)
			return values[middle];
		return 0.5 * (values[middle - 1] + values[middle]);
	}

	LoudnessPerformanceResult measureLoudnessPerformance(
		LoudnessCorrectionFilter::FilterParameters::EngineMode engine,
		bool useBlockPath,
		unsigned batchSize = 256)
	{
		const unsigned sampleRate = 48000;
		const unsigned channelCount = 2;
		const unsigned measuredFrameCount = 1024 * 1024;
		const unsigned blockCount = (std::max)(
			1u, measuredFrameCount / batchSize);
		const unsigned repeatCount = 7;
		vector<double> initializeTimes;
		vector<double> updateTimes;
		vector<double> processingTimes;
		volatile double outputChecksum = 0.0;
		bool valid = true;

		for (unsigned repeat = 0; repeat < repeatCount; ++repeat)
		{
			LoudnessCorrectionFilter::FilterParameters parameters;
			parameters.state = true;
			parameters.referenceLevel = 80.0f;
			parameters.referenceOffset = 0.0f;
			parameters.attenuation = 1.0f;
			parameters.useManualVolume = true;
			parameters.manualVolume = -38.0f;
			parameters.engine = engine;

			LoudnessCorrectionFilter filter(parameters);
			FilterRuntimeContext context;
			context.offlineAnalysis = true;
			filter.setRuntimeContext(context);
			vector<wstring> channels(channelCount, L"C");
			PrecisionTimer timer;
			timer.start();
			filter.initialize(
				static_cast<float>(sampleRate),
				useBlockPath ? batchSize : 0,
				channels);
			initializeTimes.push_back(timer.stop() * 1000.0);

			timer.start();
			LoudnessCorrectionFilterTestAccess::publishVolumeUpdate(
				filter, -42.0 + static_cast<double>(repeat));
			updateTimes.push_back(timer.stop() * 1000.0);

			vector<vector<double>> input(channelCount, vector<double>(batchSize));
			vector<vector<double>> output(channelCount, vector<double>(batchSize));
			vector<double*> inputChannels(channelCount);
			vector<double*> outputChannels(channelCount);
			for (unsigned channel = 0; channel < channelCount; ++channel)
			{
				inputChannels[channel] = input[channel].data();
				outputChannels[channel] = output[channel].data();
				for (unsigned frame = 0; frame < batchSize; ++frame)
				{
					input[channel][frame] = 0.25 * sin(
						2.0 * M_PI * (440.0 + 37.0 * channel) * frame /
						static_cast<double>(sampleRate));
				}
			}

			// Consume the pending coefficient handoff before timing the settled
			// one-bank path. Offline analysis already starts in the common A domain.
			unsigned settlingBlocks = 0;
			const unsigned maximumSettlingBlocks =
				(2 * sampleRate + batchSize - 1) / batchSize;
			do
			{
				filter.process(outputChannels.data(), inputChannels.data(), batchSize);
				++settlingBlocks;
			} while (!LoudnessCorrectionFilterTestAccess::settledOnNonIdentityBank(
				filter) && settlingBlocks < maximumSettlingBlocks);
			if (!LoudnessCorrectionFilterTestAccess::settledOnNonIdentityBank(filter))
			{
				fprintf(stderr, "Loudness performance filter did not settle.\n");
				valid = false;
			}

			timer.start();
			for (unsigned block = 0; block < blockCount; ++block)
				filter.process(outputChannels.data(), inputChannels.data(), batchSize);
			double processingSeconds = timer.stop();
			const double sampleCount = static_cast<double>(
				blockCount) * batchSize * channelCount;
			processingTimes.push_back(
				processingSeconds * 1.0e9 / sampleCount);
			outputChecksum += output[repeat % channelCount][repeat % batchSize];
		}

		if (!isfinite(outputChecksum))
		{
			fprintf(stderr, "Loudness performance checksum is non-finite.\n");
			valid = false;
		}
		return LoudnessPerformanceResult{
			median(initializeTimes),
			median(updateTimes),
			median(processingTimes),
			valid
		};
	}

	int runLoudnessPerformanceBenchmark()
	{
		const LoudnessPerformanceResult full = measureLoudnessPerformance(
			LoudnessCorrectionFilter::FilterParameters::ENGINE_FULL, true);
		const LoudnessPerformanceResult fullScalar = measureLoudnessPerformance(
			LoudnessCorrectionFilter::FilterParameters::ENGINE_FULL, false);
		const LoudnessPerformanceResult fast = measureLoudnessPerformance(
			LoudnessCorrectionFilter::FilterParameters::ENGINE_FAST, true);
		bool valid = full.valid && fullScalar.valid && fast.valid;
		printf(
			"Loudness Full: init %.3f ms, update %.3f ms, process %.3f ns/sample\n",
			full.initializeMilliseconds,
			full.updateMilliseconds,
			full.processingNanosecondsPerSample);
		printf(
			"Loudness Full scalar fallback: process %.3f ns/sample\n",
			fullScalar.processingNanosecondsPerSample);
		printf(
			"Loudness Fast: init %.3f ms, update %.3f ms, process %.3f ns/sample\n",
			fast.initializeMilliseconds,
			fast.updateMilliseconds,
			fast.processingNanosecondsPerSample);
		printf(
			"Ratios: Full block/scalar %.3f; Fast/Full init %.3f, update %.3f, process %.3f\n",
			full.processingNanosecondsPerSample /
				fullScalar.processingNanosecondsPerSample,
			fast.initializeMilliseconds / full.initializeMilliseconds,
			fast.updateMilliseconds / full.updateMilliseconds,
			fast.processingNanosecondsPerSample /
				full.processingNanosecondsPerSample);
		const unsigned additionalBatchSizes[] = { 16, 64, 1024 };
		for (unsigned batchSize : additionalBatchSizes)
		{
			const LoudnessPerformanceResult block = measureLoudnessPerformance(
				LoudnessCorrectionFilter::FilterParameters::ENGINE_FULL,
				true,
				batchSize);
			const LoudnessPerformanceResult scalar = measureLoudnessPerformance(
				LoudnessCorrectionFilter::FilterParameters::ENGINE_FULL,
				false,
				batchSize);
			valid = block.valid && scalar.valid && valid;
			printf(
				"Loudness Full batch %u: block %.3f, scalar %.3f ns/sample, ratio %.3f\n",
				batchSize,
				block.processingNanosecondsPerSample,
				scalar.processingNanosecondsPerSample,
				block.processingNanosecondsPerSample /
					scalar.processingNanosecondsPerSample);
		}
		return valid ? 0 : 1;
	}
}

static int runVSTSelfLoadGuardTest(const string& pathArgument)
{
	const wstring path = StringHelper::toWString(pathArgument, CP_ACP);
	if (path.empty() || !VSTPluginLibrary::isCurrentModulePath(path))
	{
		fprintf(stderr,
			"VST self-load guard did not recognize the current module identity.\n");
		return 1;
	}

	shared_ptr<VSTPluginLibrary> library = VSTPluginLibrary::getInstance(path);
	if (library == nullptr ||
		library->initialize() != AbstractLibrary::RECURSIVE_LOADING)
	{
		fprintf(stderr,
			"In-process VST self-load was not rejected before LoadLibrary.\n");
		return 1;
	}

	OutProcVSTPluginFilterFactory factory;
	wstring command = L"OutProcVSTPlugin";
	wstring parameters = L"Library \"" + path + L"\"";
	vector<IFilter*> filters = factory.createFilter(L"", command, parameters);
	if (!filters.empty())
	{
		fprintf(stderr,
			"Out-of-process VST self-load was not rejected before host creation.\n");
		return 1;
	}

	printf("VST in-process/out-of-process self-load guard: PASS\n");
	return 0;
}

int main(int argc, char** argv)
{
	try
	{
		stringstream versionStream;
		versionStream << MAJOR << "." << MINOR;
		if (REVISION != 0)
			versionStream << "." << REVISION;
		TCLAP::CmdLine cmd("Benchmark generates a linear sine sweep or reads from the given input file. "
			"It then filters the waveform using the Hibiki EQAPO filter configuration "
			"and finally writes to the given file or into the user's temp directory.", ' ', versionStream.str());

		TCLAP::SwitchArg convSelfTestArg(
			"", "convselftest",
			"Run internal HybridConv correctness benchmark and exit", cmd);
		TCLAP::SwitchArg noPauseArg("", "nopause", "Do not wait for key press at the end", cmd);
		TCLAP::SwitchArg verboseArg("v", "verbose", "Print trace and error messages to console instead of logfile", cmd);
		TCLAP::SwitchArg loudnessTransitionTestArg(
			"", "loudness-transition-test",
			"Run native loudness coefficient-transition safety regressions", cmd);
		TCLAP::SwitchArg loudnessPerformanceArg(
			"", "loudness-performance",
			"Measure native loudness initialization, update, and callback cost", cmd);
		TCLAP::ValueArg<string> vstSelfIdentityTestArg(
			"", "vst-self-identity-test",
			"Verify that a path or hardlink to this executable is rejected by both VST self-load paths",
			false, "", "path", cmd);
		TCLAP::ValueArg<string> configArg("", "config", "Configuration file to load instead of the installed config.txt", false, "", "path", cmd);
		TCLAP::ValueArg<string> guidArg("", "guid", "Endpoint GUID to use when parsing configuration (Default: <empty>)", false, "", "string", cmd);
		TCLAP::ValueArg<string> connectionnameArg("", "connectionname", "Connection name to use when parsing configuration (Default: File output)", false, "File output", "string", cmd);
		TCLAP::ValueArg<string> devicenameArg("", "devicename", "Device name to use when parsing configuration (Default: Benchmark)", false, "Benchmark", "string", cmd);
		TCLAP::ValueArg<unsigned> batchsizeArg("", "batchsize", "Number of frames processed in one batch (Default: 65536)", false, 65536, "integer", cmd);
		TCLAP::ValueArg<string> outputArg("o", "output", "File to write sound data to", false, "", "string", cmd);
		TCLAP::ValueArg<string> inputArg("i", "input", "File to load sound data from instead of generating sweep", false, "", "string", cmd);
		TCLAP::ValueArg<unsigned> rateArg("r", "rate", "Sample rate of generated sweep (Default: 44100)", false, 44100, "integer", cmd);
		TCLAP::ValueArg<float> toArg("t", "to", "End frequency of generated sweep in Hz (Default: 20000.0)", false, 20000.0f, "float", cmd);
		TCLAP::ValueArg<float> fromArg("f", "from", "Start frequency of generated sweep in Hz (Default: 0.1)", false, 1.0f, "float", cmd);
		TCLAP::ValueArg<float> lengthArg("l", "length", "Length of generated sweep in seconds (Default: 200.0)", false, 200.0f, "float", cmd);
		TCLAP::ValueArg<unsigned> channelArg("c", "channels", "Number of channels of generated sweep (Default: 2)", false, 2, "integer", cmd);

		cmd.parse(argc, argv);

		bool verbose = verboseArg.getValue();
		LogHelper::set(stderr, verbose, true, true);
		if (!vstSelfIdentityTestArg.getValue().empty())
			return runVSTSelfLoadGuardTest(vstSelfIdentityTestArg.getValue());
		if (convSelfTestArg.getValue())
		{
			// Expected fail-closed cases intentionally exercise error logging. Keep
			// those diagnostics in the captured test transcript without presenting
			// successful self-tests as native stderr failures to PowerShell callers.
			LogHelper::set(stdout, verbose, true, false);
			return runConvolutionSelfTest();
		}
		if (loudnessTransitionTestArg.getValue())
			return runLoudnessTransitionTests();
		if (loudnessPerformanceArg.getValue())
			return runLoudnessPerformanceBenchmark();
		if (batchsizeArg.getValue() == 0)
		{
			fprintf(stderr, "Error: --batchsize must be greater than zero.\n");
			return 1;
		}
#ifdef _DEBUG
		_CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
		// _CrtSetBreakAlloc(3318);
#endif

		unsigned sampleRate;
		unsigned channelCount;
		unsigned channelMask;
		unsigned frameCount;
		float length;
		size_t sampleCount = 0;
		vector<float> buf;

		if (REVISION == 0)
			printf("Benchmark %d.%d\n", MAJOR, MINOR);
		else
			printf("Benchmark %d.%d.%d\n", MAJOR, MINOR, REVISION);

		printf("Run \"%s -h\" to show usage info\n", argv[0]);
		printf("\n");

		string input = inputArg.getValue();
		if (input != "")
		{
			printf("Reading sound data from %s\n", input.c_str());

			PrecisionTimer timer;
			timer.start();

			SF_INFO info = {};
			SNDFILE* inFile = sf_open(input.c_str(), SFM_READ, &info);
			if (inFile == NULL)
			{
				fprintf(stderr, "%s", sf_strerror(inFile));
				return 1;
			}
			SCOPE_EXIT
			{
				if (inFile != NULL)
					sf_close(inFile);
			};
			if (info.samplerate <= 0 ||
				info.samplerate >= (std::numeric_limits<int>::max)() ||
				info.channels <= 0 ||
				info.frames <= 0 ||
				static_cast<unsigned long long>(info.frames) >
					(std::numeric_limits<unsigned>::max)())
			{
				fprintf(stderr, "Input file has invalid rate, channel, or frame metadata.\n");
				return 1;
			}

			sampleRate = static_cast<unsigned>(info.samplerate);
			channelCount = static_cast<unsigned>(info.channels);
			channelMask = 0;
			frameCount = static_cast<unsigned>(info.frames);
			length = static_cast<float>(
				static_cast<double>(frameCount) / sampleRate);

			if (!getSafeSampleCount<float>(
					channelCount, frameCount, sampleCount))
			{
				fprintf(stderr, "Input file sample count is too large.\n");
				return 1;
			}
			buf.resize(sampleCount);

			sf_count_t numRead = 0;
			while (numRead < info.frames)
			{
				const sf_count_t framesRead = sf_readf_float(
					inFile,
					buf.data() + static_cast<size_t>(numRead) * channelCount,
					info.frames - numRead);
				if (framesRead <= 0)
				{
					fprintf(stderr, "Input read made no progress: %s\n",
						sf_strerror(inFile));
					return 1;
				}
				numRead += framesRead;
			}

			if (sf_close(inFile) != 0)
			{
				inFile = NULL;
				fprintf(stderr, "Could not close input file cleanly.\n");
				return 1;
			}
			inFile = NULL;

			double readTime = timer.stop();
			printf("Reading input file took %g seconds\n", readTime);
		}
		else
		{
			sampleRate = rateArg.getValue();
			channelMask = 0;
			channelCount = channelArg.getValue();
			const double sweepFrom = fromArg.getValue();
			const double sweepTo = toArg.getValue();
			length = lengthArg.getValue();
			if (sampleRate == 0 ||
				sampleRate >= static_cast<unsigned>((std::numeric_limits<int>::max)()) ||
				channelCount == 0 ||
				channelCount > static_cast<unsigned>((std::numeric_limits<int>::max)()) ||
				!std::isfinite(length) || length <= 0.0f ||
				!std::isfinite(sweepFrom) || sweepFrom <= 0.0 ||
				!std::isfinite(sweepTo) || sweepTo <= 0.0 ||
				sweepFrom > static_cast<double>(sampleRate) * 0.5 ||
				sweepTo > static_cast<double>(sampleRate) * 0.5)
			{
				fprintf(stderr, "Generated sweep parameters must be finite, positive, and within Nyquist.\n");
				return 1;
			}
			const double requestedFrames =
				static_cast<double>(length) * sampleRate;
			if (!std::isfinite(requestedFrames) || requestedFrames < 1.0 ||
				requestedFrames >
					static_cast<double>((std::numeric_limits<unsigned>::max)()))
			{
				fprintf(stderr, "Generated sweep frame count is out of range.\n");
				return 1;
			}
			frameCount = static_cast<unsigned>(requestedFrames);
			const double sweepDiff = sweepTo - sweepFrom;

			printf("No input file given, so generating linear sine sweep from %g to %g Hz over %g seconds\n", sweepFrom, sweepTo, length);

			PrecisionTimer timer;
			timer.start();

			if (!getSafeSampleCount<float>(
					channelCount, frameCount, sampleCount))
			{
				fprintf(stderr, "Generated sweep sample count is too large.\n");
				return 1;
			}
			buf.resize(sampleCount);
			for (unsigned i = 0; i < frameCount; i++)
			{
				double t = i * 1.0 / sampleRate;
				float s = static_cast<float>(sin(
					((sweepFrom + sweepDiff * (t / length) / 2) * t) *
					2 * M_PI));

				for (unsigned j = 0; j < channelCount; j++)
					buf[static_cast<size_t>(i) * channelCount + j] = s;
			}

			double genTime = timer.stop();
			printf("Generating sweep took %g seconds\n", genTime);
		}

		const unsigned batchsize =
			min(batchsizeArg.getValue(), frameCount);

		vector<float> buf2(sampleCount, 0.0f);

		PrecisionTimer timer;
		timer.start();
		{
			FilterEngine engine;
			wstring deviceName = StringHelper::toWString(devicenameArg.getValue(), CP_ACP);
			wstring connectionName = StringHelper::toWString(connectionnameArg.getValue(), CP_ACP);
			wstring deviceGuid = StringHelper::toWString(guidArg.getValue(), CP_ACP);
			wstring customConfigPath = StringHelper::toWString(configArg.getValue(), CP_ACP);
			engine.setDeviceInfo(false, true, deviceName, connectionName, deviceGuid, deviceName + L" " + connectionName + L" " + deviceGuid);
			engine.initialize((float)sampleRate, channelCount, channelCount, channelCount, channelMask, batchsize, customConfigPath);

			double initTime = timer.stop();
			if (!verbose)
				printf("\nLoading configuration took %g ms\n", initTime * 1000.0);

			printf("\nProcessing %d frames from %d channel(s)\n", frameCount, channelCount);

			timer.start();

			unsigned processedFrames = 0;
			while (processedFrames < frameCount)
			{
				const unsigned blockFrames =
					min(batchsize, frameCount - processedFrames);
				const size_t offset =
					static_cast<size_t>(processedFrames) * channelCount;
				engine.process(
					buf2.data() + offset, buf.data() + offset, blockFrames);
				processedFrames += blockFrames;
			}

			double time = timer.stop();

			printf("%zu samples processed in %f seconds\n", sampleCount, time);
			printf("This is equivalent to %.2f%% CPU load (one core) when processing in real time\n", 100.0f * time / length);

			size_t clipCount = 0;
			float max = 0;
			bool outputIsFinite = true;
			for (size_t i = 0; i < sampleCount; i++)
			{
				if (!std::isfinite(buf2[i]))
				{
					outputIsFinite = false;
					continue;
				}
				float f = fabs(buf2[i]);
				if (f > max)
					max = f;
				if (f > 1.0f)
					clipCount++;
			}

			printf("Max output level: %f (%f dB)", max, log10(max) * 20.0f);
			if (clipCount > 0)
				printf(" (%zu samples clipped!)", clipCount);
			printf("\n");
			if (!outputIsFinite)
			{
				fprintf(stderr, "Processing produced non-finite output samples.\n");
				return 1;
			}

			string output = outputArg.getValue();
			if (output == "")
			{
				char temp[255];
				const DWORD pathLength = GetTempPathA(
					static_cast<DWORD>(sizeof(temp) / sizeof(temp[0])), temp);
				if (pathLength == 0 || pathLength >= sizeof(temp))
				{
					fprintf(stderr, "Could not resolve the temporary output path.\n");
					return 1;
				}

				output = temp;
				output += "testout.wav";
			}

			printf("\nWriting output to %s\n", output.c_str());

			SF_INFO info = {};
			info.frames = static_cast<sf_count_t>(frameCount);
			info.samplerate = static_cast<int>(sampleRate);
			info.channels = static_cast<int>(channelCount);
			info.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;
			SNDFILE* outFile = sf_open(output.c_str(), SFM_WRITE, &info);
			if (outFile == NULL)
			{
				fprintf(stderr, "%s", sf_strerror(outFile));
				return 1;
			}
			SCOPE_EXIT
			{
				if (outFile != NULL)
					sf_close(outFile);
			};

			sf_count_t numWritten = 0;
			while (numWritten < frameCount)
			{
				const sf_count_t framesWritten = sf_writef_float(
					outFile,
					buf2.data() + static_cast<size_t>(numWritten) * channelCount,
					static_cast<sf_count_t>(frameCount) - numWritten);
				if (framesWritten <= 0)
				{
					fprintf(stderr, "Output write made no progress: %s\n",
						sf_strerror(outFile));
					return 1;
				}
				numWritten += framesWritten;
			}

			if (sf_close(outFile) != 0)
			{
				outFile = NULL;
				fprintf(stderr, "Could not close output file cleanly.\n");
				return 1;
			}
			outFile = NULL;
		}

		if (!noPauseArg.getValue())
			system("pause");

		return 0;
	}
	catch (const TCLAP::ArgException& e)
	{
		printf("Error: %s for arg %s\n", e.error().c_str(), e.argId().c_str());
		return -1;
	}
	catch (const std::bad_alloc&)
	{
		fprintf(stderr, "Benchmark could not allocate the requested buffers.\n");
		return 1;
	}
	catch (const std::exception& e)
	{
		fprintf(stderr, "Benchmark failed: %s\n", e.what());
		return 1;
	}
	catch (...)
	{
		fprintf(stderr, "Benchmark failed with an unknown error.\n");
		return 1;
	}
}
