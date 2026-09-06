/*
    This file is part of Equalizer APO, a system-wide equalizer.
    Copyright (C) 2017  Alexander Walch
    Copyright (C) 2026  Equalizer APO contributors

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
*/

#include "stdafx.h"
#include "LoudnessCorrectionFilter.h"
#include "VolumeController.h"
#include "helpers/LogHelper.h"

#include <algorithm>
#include <cmath>
#include <limits>

LoudnessCorrectionFilter::LoudnessCorrectionFilter(const FilterParameters& fParameters)
	: _parameterUpdateThreadHandle(NULL),
	  _stopParameterUpdateThreadEvent(NULL),
	  _parameters(fParameters),
	  _runtimeContext(),
	  _hasInitialAutomaticVolume(false),
	  _initialAutomaticVolume(0.0),
	  _initialAutomaticVolumeScalar(1.0),
	  _initialAutomaticMuted(false),
	  _runtimeBypass(false),
	  _recoveryPending(false),
	  _channelCount(0),
	  _activeBandCount(0),
	  _maximumFrameCount(0),
	  _fastFitPointCount(0),
	  _sampleRate(48000.0f),
	  _activeBankIndex(0),
	  _transitionBankIndex(1),
	  _crossoverPrewarmPosition(0),
	  _crossoverPrewarmLength(36000),
	  _warmupPosition(0),
	  _warmupLength(12000),
	  _crossfadePosition(0),
	  _crossfadeLength(4800),
	  _bypassFadePosition(0),
	  _bypassFadeLength(480),
	  _warmupActive(false),
	  _crossfadeActive(false),
	  _bypassFadeActive(false),
	  _runtimeBypassWasActive(false),
	  _transitionFromBypass(false),
	  _crossoverPrewarmActive(false),
	  _crossoverHandoffActive(false),
	  _crossoverDomainActive(),
	  _crossoverHandoffHasPrevious(),
	  _crossoverHandoffPreviousRaw(),
	  _crossoverHandoffPreviousIdentity(),
	  _crossoverDomainChannelCount(0),
	  _maximumCrossoverHandoffStep(0.0),
	  _pendingCoeffs{},
	  _lowpassCoeffs{},
	  _highpassCoeffs{},
	  _bankIdentity{ false, false },
	  _pendingIdentity(false),
	  _outputGainLinear(1.0),
	  _targetOutputGainLinear(1.0),
	  _pendingOutputGainLinear(1.0),
	  _volumeFollowGainLinear(1.0),
	  _targetVolumeFollowGainLinear(1.0),
	  _volumeFollowStepPerSample(0.0),
	  _pendingVolumeFollowGainLinear(1.0),
	  _volumeFollowRampRemaining(0),
	  _volumeFollowRampLength(480),
	  _volumeFollowUpdated(false),
	  _coeffsUpdated(false)
{
	InitializeCriticalSection(&_parameterUpdateSection);

	for (size_t row = 0; row < NUM_BANDS; ++row)
	{
		for (size_t column = 0; column < NUM_BANDS; ++column)
			_inverseResponseMatrix[row][column] = 0.0;
	}
}

LoudnessCorrectionFilter::~LoudnessCorrectionFilter()
{
	if (_stopParameterUpdateThreadEvent)
		SetEvent(_stopParameterUpdateThreadEvent);

	// The worker owns references to this object and its critical section. Do not
	// destroy either until it has definitely exited.
	if (_parameterUpdateThreadHandle)
	{
		WaitForSingleObject(_parameterUpdateThreadHandle, INFINITE);
		CloseHandle(_parameterUpdateThreadHandle);
		_parameterUpdateThreadHandle = NULL;
	}

	if (_stopParameterUpdateThreadEvent)
	{
		CloseHandle(_stopParameterUpdateThreadEvent);
		_stopParameterUpdateThreadEvent = NULL;
	}

	DeleteCriticalSection(&_parameterUpdateSection);
}

bool LoudnessCorrectionFilter::canTrackAutomaticVolume() const
{
	// Preserve Mixomo's original Windows-volume binding: global mode reads the
	// default eRender/eMultimedia endpoint independently of APO endpoint metadata.
	// Source: https://github.com/Mixomo/EqAPO64_with_VST3_support/blob/e81f9c3d1faead9abef08aeb16ce6647ccd9d078/filters/loudnessCorrection/VolumeController.cpp#L24-L49
	if (_parameters.binding == FilterParameters::BINDING_ALL)
		return true;
	if (!_runtimeContext.flowKnown || _runtimeContext.isCapture)
		return false;
	return !_runtimeContext.endpointId.empty();
}

std::wstring LoudnessCorrectionFilter::getVolumeControllerEndpointId() const
{
	if (_parameters.binding == FilterParameters::BINDING_ALL)
		return L"";
	return _runtimeContext.endpointId;
}

std::vector<std::wstring> LoudnessCorrectionFilter::initialize(
	float sampleRate,
	unsigned maxFrameCount,
	std::vector<std::wstring> channelNames)
{
	if (_stopParameterUpdateThreadEvent)
		SetEvent(_stopParameterUpdateThreadEvent);
	if (_parameterUpdateThreadHandle)
	{
		WaitForSingleObject(_parameterUpdateThreadHandle, INFINITE);
		CloseHandle(_parameterUpdateThreadHandle);
		_parameterUpdateThreadHandle = NULL;
	}
	if (_stopParameterUpdateThreadEvent)
	{
		CloseHandle(_stopParameterUpdateThreadEvent);
		_stopParameterUpdateThreadEvent = NULL;
	}

	_runtimeBypass.store(false, std::memory_order_relaxed);
	_recoveryPending.store(false, std::memory_order_relaxed);
	_hasInitialAutomaticVolume = false;
	_initialAutomaticVolume = 0.0;
	_initialAutomaticVolumeScalar = 1.0;
	_initialAutomaticMuted = false;
	_channelCount = channelNames.size();
	_activeBandCount = 0;
	_maximumFrameCount = maxFrameCount;
	_fastFitPointCount = 0;
	_lowpassBlockScratch.assign(_maximumFrameCount, 0.0);
	for (size_t bank = 0; bank < 2; ++bank)
	{
		_biquadBanks[bank].clear();
		_lowpassBanks[bank].clear();
		_highpassBanks[bank].clear();
	}
	_activeBankIndex = 0;
	_transitionBankIndex = 1;
	_crossoverPrewarmPosition = 0;
	_crossoverPrewarmLength = 36000;
	_warmupPosition = 0;
	_warmupLength = 12000;
	_crossfadePosition = 0;
	_bypassFadePosition = 0;
	_warmupActive = false;
	_crossfadeActive = false;
	_bypassFadeActive = false;
	_runtimeBypassWasActive = false;
	_transitionFromBypass = false;
	_crossoverPrewarmActive = false;
	_crossoverHandoffActive = false;
	_crossoverDomainActive.assign(_channelCount, 0);
	_crossoverHandoffHasPrevious.assign(_channelCount, 0);
	_crossoverHandoffPreviousRaw.assign(_channelCount, 0.0);
	_crossoverHandoffPreviousIdentity.assign(_channelCount, 0.0);
	_crossoverDomainChannelCount = 0;
	_maximumCrossoverHandoffStep = 0.0;
	_bankIdentity[0] = false;
	_bankIdentity[1] = false;
	_pendingIdentity = false;
	_volumeFollowGainLinear = 1.0;
	_targetVolumeFollowGainLinear = 1.0;
	_volumeFollowStepPerSample = 0.0;
	_pendingVolumeFollowGainLinear = 1.0;
	_volumeFollowRampRemaining = 0;
	_volumeFollowUpdated.store(false, std::memory_order_relaxed);
	_coeffsUpdated.store(false, std::memory_order_relaxed);
	_sampleRate = std::isfinite(sampleRate) && sampleRate >= 8000.0f ? sampleRate : 48000.0f;
	_crossoverPrewarmLength = (std::max)(1u, static_cast<unsigned>(
		std::lround(_sampleRate * CROSSOVER_HISTORY_PREWARM_SECONDS)));
	_warmupLength = (std::max)(1u, static_cast<unsigned>(
		std::lround(_sampleRate * FILTER_WARMUP_SECONDS)));
	_crossfadeLength = (std::max)(1u, static_cast<unsigned>(
		std::lround(_sampleRate * COEFFICIENT_CROSSFADE_SECONDS)));
	_bypassFadeLength = (std::max)(1u, static_cast<unsigned>(
		std::lround(_sampleRate * BYPASS_FADE_SECONDS)));
	_volumeFollowRampLength = (std::max)(1u, static_cast<unsigned>(
		std::lround(_sampleRate * VOLUME_FOLLOW_RAMP_SECONDS)));
	for (size_t section = 0; section < CROSSOVER_SECTION_COUNT; ++section)
	{
		computeCrossoverCoeffs(false, section, _lowpassCoeffs[section]);
		computeCrossoverCoeffs(true, section, _highpassCoeffs[section]);
	}

	// Frequencies at or above 90% of Nyquist are not representable reliably.
	double maximumCenterFrequency = 0.45 * static_cast<double>(_sampleRate);
	if (_parameters.engine == FilterParameters::ENGINE_FAST)
	{
		while (_activeBandCount < FAST_BAND_COUNT &&
			fastBandFrequency(_activeBandCount) <= maximumCenterFrequency)
		{
			++_activeBandCount;
		}
	}
	else
	{
		while (_activeBandCount < NUM_BANDS &&
			LoudnessProfile::LOUDNESS_PROFILE_TABLE[_activeBandCount].frequency <= maximumCenterFrequency)
		{
			++_activeBandCount;
		}
	}

	// Fit points always cover the whole representable profile table,
	// regardless of engine, so fast and full aim at the same targets.
	while (_fastFitPointCount < NUM_BANDS &&
		LoudnessProfile::LOUDNESS_PROFILE_TABLE[_fastFitPointCount].frequency <= maximumCenterFrequency)
	{
		++_fastFitPointCount;
	}

	for (size_t bank = 0; bank < 2; ++bank)
	{
		_biquadBanks[bank].resize(_channelCount);
		_lowpassBanks[bank].resize(_channelCount);
		_highpassBanks[bank].resize(_channelCount);
		for (size_t channel = 0; channel < _channelCount; ++channel)
		{
			_biquadBanks[bank][channel].reserve(_activeBandCount);
			for (size_t band = 0; band < _activeBandCount; ++band)
			{
				if (_parameters.engine == FilterParameters::ENGINE_FAST)
				{
					// The peaking top band takes Q; the shelf takes S.
					const bool isPeak = fastBandType(band) == BiQuad::PEAKING;
					_biquadBanks[bank][channel].push_back(BiQuad(
						fastBandType(band),
						0.0,
						fastBandFrequency(band),
						_sampleRate,
						isPeak ? FAST_PEAK_Q : FAST_SHELF_SLOPE,
						!isPeak));
				}
				else
				{
					_biquadBanks[bank][channel].push_back(BiQuad(
						BiQuad::PEAKING,
						0.0,
						LoudnessProfile::LOUDNESS_PROFILE_TABLE[band].frequency,
						_sampleRate,
						FILTER_Q,
						false));
				}
			}

			_lowpassBanks[bank][channel].reserve(CROSSOVER_SECTION_COUNT);
			_highpassBanks[bank][channel].reserve(CROSSOVER_SECTION_COUNT);
			for (size_t section = 0; section < CROSSOVER_SECTION_COUNT; ++section)
			{
				_lowpassBanks[bank][channel].push_back(BiQuad());
				_highpassBanks[bank][channel].push_back(BiQuad());
				double lowpassCoefficients[4] = {
					_lowpassCoeffs[section].b1,
					_lowpassCoeffs[section].b2,
					_lowpassCoeffs[section].a1,
					_lowpassCoeffs[section].a2
				};
				double highpassCoefficients[4] = {
					_highpassCoeffs[section].b1,
					_highpassCoeffs[section].b2,
					_highpassCoeffs[section].a1,
					_highpassCoeffs[section].a2
				};
				_lowpassBanks[bank][channel][section].setCoefficients(
					lowpassCoefficients, _lowpassCoeffs[section].b0);
				_highpassBanks[bank][channel][section].setCoefficients(
					highpassCoefficients, _highpassCoeffs[section].b0);
			}
		}
	}

	// The fast engine has no response matrix to invert; its shelf gains are
	// closed-form. Its larger initialization saving comes from also avoiding
	// the Full engine's dense refined headroom scan below.
	if (_parameters.engine != FilterParameters::ENGINE_FAST)
		computeResponseInverse();

	double initialVolume = 0.0;
	double initialVolumeScalar = 1.0;
	bool initialMuted = false;
	if (_parameters.state && _parameters.useManualVolume)
	{
		initialVolume = _parameters.manualVolume;
		initialVolumeScalar = (initialVolume + 100.0) / 100.0;
	}
	else if (_parameters.state)
	{
		if (!canTrackAutomaticVolume())
		{
			if (_runtimeContext.volumeObservations != nullptr)
			{
				FilterRuntimeVolumeObservation observation;
				observation.requestedEndpointId = getVolumeControllerEndpointId();
				observation.available = false;
				_runtimeContext.volumeObservations->push_back(observation);
			}
			_runtimeBypass.store(true, std::memory_order_relaxed);
			if (_parameters.volumeFollow == FilterParameters::VOLUME_FOLLOW_OFF)
				LogF(L"LoudnessCorrection automatic volume mode is unavailable for this binding; filter is bypassed. Use manual volume mode.");
			else
				LogF(L"LoudnessCorrection automatic volume mode is unavailable for this binding; correction is bypassed and APO volume follow stays muted. Use manual volume mode.");
		}
		else
		{
			VolumeController volumeController(getVolumeControllerEndpointId());
			EndpointVolumeState volumeState;
			const HRESULT volumeResult = volumeController.getVolumeState(volumeState);
			if (SUCCEEDED(volumeResult))
			{
				initialVolume = volumeState.levelDb;
				initialVolumeScalar = volumeState.scalar;
				initialMuted = volumeState.muted;
			}
			if (_runtimeContext.volumeObservations != nullptr)
			{
				FilterRuntimeVolumeObservation observation;
				observation.requestedEndpointId = getVolumeControllerEndpointId();
				observation.resolvedEndpointId = volumeController.getEndpointId();
				observation.volumeDb = initialVolume;
				observation.volumeScalar = initialVolumeScalar;
				observation.muted = initialMuted;
				observation.available = SUCCEEDED(volumeResult);
				_runtimeContext.volumeObservations->push_back(observation);
			}
			if (FAILED(volumeResult))
			{
				_runtimeBypass.store(true, std::memory_order_relaxed);
				initialVolume = 0.0;
				if (_parameters.volumeFollow == FilterParameters::VOLUME_FOLLOW_OFF)
					LogF(L"LoudnessCorrection could not read the configured endpoint volume; filter is bypassed until the endpoint recovers.");
				else
					LogF(L"LoudnessCorrection could not read the configured endpoint volume; correction is bypassed and APO volume follow stays muted until the endpoint recovers.");
			}
			else
			{
				_hasInitialAutomaticVolume = true;
				_initialAutomaticVolume = initialVolume;
				_initialAutomaticVolumeScalar = initialVolumeScalar;
				_initialAutomaticMuted = initialMuted;
			}
		}
	}

	// Install the synchronous snapshot directly. Configuration changes already
	// crossfade whole filter instances, so beginning at unity here would create
	// an avoidable full-volume burst before the polling thread's first update.
	// If an enabled automatic source has never yielded a valid snapshot, fail
	// closed to silence: this mode is explicitly used when the endpoint itself
	// does not attenuate the actual audio route.
	if (_parameters.state &&
		_parameters.volumeFollow != FilterParameters::VOLUME_FOLLOW_OFF)
	{
		if (!_parameters.useManualVolume && !_hasInitialAutomaticVolume)
			_volumeFollowGainLinear = 0.0;
		else
			_volumeFollowGainLinear = calculateVolumeFollowGain(
				_parameters.volumeFollow,
				initialVolume,
				initialVolumeScalar,
				initialMuted);
	}
	_targetVolumeFollowGainLinear = _volumeFollowGainLinear;
	_pendingVolumeFollowGainLinear = _volumeFollowGainLinear;

	std::vector<double> gains;
	if (_parameters.engine == FilterParameters::ENGINE_FAST)
		calculateFastShelfGains(initialVolume, gains, _outputGainLinear);
	else
		calculateBandGains(initialVolume, gains, _outputGainLinear);
	bool initialIdentity = _outputGainLinear == 1.0;
	for (size_t band = 0; band < _activeBandCount; ++band)
		initialIdentity = initialIdentity && std::abs(gains[band]) <= 1.0e-12;
	_bankIdentity[0] = initialIdentity;
	_bankIdentity[1] = initialIdentity;
	_pendingIdentity = initialIdentity;
	_targetOutputGainLinear = _outputGainLinear;
	_pendingOutputGainLinear = _outputGainLinear;
	for (size_t band = 0; band < _activeBandCount; ++band)
	{
		computeBandCoeffs(band, gains[band], _pendingCoeffs[band]);
		double coefficients[4] = {
			_pendingCoeffs[band].b1,
			_pendingCoeffs[band].b2,
			_pendingCoeffs[band].a1,
			_pendingCoeffs[band].a2
		};
		for (size_t bank = 0; bank < 2; ++bank)
		{
			for (size_t channel = 0; channel < _channelCount; ++channel)
			{
				_biquadBanks[bank][channel][band].setCoefficients(
					coefficients, _pendingCoeffs[band].b0);
			}
		}
	}

	// Offline editor analysis has no realtime handoff to protect. Start from the
	// configured bank so its first finite analysis window represents the saved
	// settings instead of the realtime cold-start bypass.
	if (_runtimeContext.offlineAnalysis)
	{
		// An unavailable automatic source must remain the same fail-closed raw
		// path as realtime processing; otherwise analysis would briefly expose
		// coefficients calculated from the placeholder 0 dB value.
		if (!_runtimeBypass.load(std::memory_order_relaxed))
		{
			std::fill(_crossoverDomainActive.begin(), _crossoverDomainActive.end(), 1);
			_crossoverDomainChannelCount = _channelCount;
		}
	}
	// Every enabled realtime instance first accumulates live fixed-crossover
	// history, even when its initial correction is neutral or endpoint tracking
	// starts in fail-closed bypass. A non-neutral target remains pending until
	// that history is ready; this prevents an early volume update or recovery
	// from copying a partially settled 25 Hz state into an audible transition.
	else if (_parameters.state && _parameters.attenuation > 0.0f &&
		_activeBandCount > 0)
	{
		_bankIdentity[0] = true;
		_activeBankIndex = 0;
		_transitionBankIndex = 1;
		_outputGainLinear = 1.0;
		_crossoverPrewarmPosition = 0;
		_warmupPosition = 0;
		_crossfadePosition = 0;
		_crossoverPrewarmActive = true;
		_crossoverHandoffActive = false;
		_warmupActive = false;
		_crossfadeActive = false;
		_transitionFromBypass = false;
		_coeffsUpdated.store(!initialIdentity, std::memory_order_relaxed);
	}

	// Manual mode is immutable for the lifetime of a filter instance, so it
	// needs no polling thread. Offline analysis uses the one synchronous volume
	// snapshot above; polling while rendering would make that snapshot depend on
	// thread scheduling. A configuration edit creates a new instance.
	if (_parameters.state && !_parameters.useManualVolume &&
		!_runtimeContext.offlineAnalysis && canTrackAutomaticVolume())
	{
		_stopParameterUpdateThreadEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
		if (_stopParameterUpdateThreadEvent)
		{
			_parameterUpdateThreadHandle =
				CreateThread(NULL, 0, &parameterUpdateThread, this, 0, NULL);
			if (!_parameterUpdateThreadHandle)
			{
				CloseHandle(_stopParameterUpdateThreadEvent);
				_stopParameterUpdateThreadEvent = NULL;
				_runtimeBypass.store(true, std::memory_order_relaxed);
				LogF(L"LoudnessCorrection could not start endpoint-volume tracking; correction is bypassed and any initial APO volume-follow gain is held.");
			}
		}
		else
		{
			_runtimeBypass.store(true, std::memory_order_relaxed);
			LogF(L"LoudnessCorrection could not create the endpoint-volume tracking event; correction is bypassed and any initial APO volume-follow gain is held.");
		}
	}

	return channelNames;
}

void LoudnessCorrectionFilter::computeResponseInverse()
{
	for (size_t row = 0; row < NUM_BANDS; ++row)
	{
		for (size_t column = 0; column < NUM_BANDS; ++column)
			_inverseResponseMatrix[row][column] = 0.0;
	}

	if (_activeBandCount == 0)
		return;

	double augmented[NUM_BANDS][2 * NUM_BANDS] = {};
	// Unit-gain coefficients depend only on (band, sample rate), so build
	// each column once and reuse it for every row instead of recomputing
	// the same coefficients per frequency point.
	BiquadCoeffs unitCoeffs[NUM_BANDS];
	for (size_t column = 0; column < _activeBandCount; ++column)
		computeBiquadCoeffs(column, 1.0, unitCoeffs[column]);
	for (size_t row = 0; row < _activeBandCount; ++row)
	{
		double frequency = LoudnessProfile::LOUDNESS_PROFILE_TABLE[row].frequency;
		for (size_t column = 0; column < _activeBandCount; ++column)
			augmented[row][column] = bandResponseDb(&unitCoeffs[column], 1, frequency);
		augmented[row][row + _activeBandCount] = 1.0;
	}

	for (size_t column = 0; column < _activeBandCount; ++column)
	{
		size_t pivot = column;
		for (size_t row = column + 1; row < _activeBandCount; ++row)
		{
			if (std::abs(augmented[row][column]) > std::abs(augmented[pivot][column]))
				pivot = row;
		}

		if (std::abs(augmented[pivot][column]) < 1.0e-10)
		{
			// This should not occur with the fitted full-band basis. Keep
			// a safe identity fallback instead of creating NaN coefficients.
			for (size_t index = 0; index < _activeBandCount; ++index)
				_inverseResponseMatrix[index][index] = 1.0;
			return;
		}

		if (pivot != column)
		{
			for (size_t value = 0; value < 2 * _activeBandCount; ++value)
				std::swap(augmented[column][value], augmented[pivot][value]);
		}

		double divisor = augmented[column][column];
		for (size_t value = 0; value < 2 * _activeBandCount; ++value)
			augmented[column][value] /= divisor;

		for (size_t row = 0; row < _activeBandCount; ++row)
		{
			if (row == column)
				continue;
			double factor = augmented[row][column];
			for (size_t value = 0; value < 2 * _activeBandCount; ++value)
				augmented[row][value] -= factor * augmented[column][value];
		}
	}

	for (size_t row = 0; row < _activeBandCount; ++row)
	{
		for (size_t column = 0; column < _activeBandCount; ++column)
		{
			_inverseResponseMatrix[row][column] =
				augmented[row][column + _activeBandCount];
		}
	}
}

bool LoudnessCorrectionFilter::isSafeCrossoverHandoff(
	double previousRaw,
	double currentRaw,
	double previousIdentity,
	double currentIdentity,
	double& handoffStep,
	double& naturalStep)
{
	double previousDifference = previousIdentity - previousRaw;
	double currentDifference = currentIdentity - currentRaw;
	bool crossed = previousDifference == 0.0 || currentDifference == 0.0 ||
		(previousDifference < 0.0 && currentDifference >= 0.0) ||
		(previousDifference > 0.0 && currentDifference <= 0.0);
	handoffStep = std::abs(currentIdentity - previousRaw);
	naturalStep = (std::max)(
		std::abs(currentRaw - previousRaw),
		std::abs(currentIdentity - previousIdentity));
	double scale = (std::max)(1.0, (std::max)(
		(std::max)(std::abs(previousRaw), std::abs(currentRaw)),
		(std::max)(std::abs(previousIdentity), std::abs(currentIdentity))));
	double tolerance =
		64.0 * std::numeric_limits<double>::epsilon() * scale;
	return crossed && std::isfinite(handoffStep) &&
		std::isfinite(naturalStep) && handoffStep <= naturalStep + tolerance;
}

void LoudnessCorrectionFilter::calculateBandGains(
	double currentVolumeDb,
	std::vector<double>& outGains,
	double& outputGainLinear) const
{
	outGains.assign(NUM_BANDS, 0.0);
	outputGainLinear = 1.0;
	if (_activeBandCount == 0 || _parameters.attenuation <= 0.0f)
		return;

	double currentVolume = std::isfinite(currentVolumeDb) ? currentVolumeDb : 0.0;
	currentVolume = (std::max)(-100.0, (std::min)(0.0, currentVolume));
	double referenceLevel = (std::max)(1.0, (std::min)(100.0,
		static_cast<double>(_parameters.referenceLevel)));
	double loudnessLevel = referenceLevel + currentVolume - _parameters.referenceOffset;
	loudnessLevel = (std::max)(0.0, (std::min)(100.0, loudnessLevel));

	double target[NUM_BANDS] = {};
	for (size_t point = 0; point < _activeBandCount; ++point)
	{
		target[point] = static_cast<double>(_parameters.attenuation) *
			LoudnessProfile::computeContourDelta(loudnessLevel, referenceLevel, point);
	}

	for (size_t band = 0; band < _activeBandCount; ++band)
	{
		for (size_t point = 0; point < _activeBandCount; ++point)
			outGains[band] += _inverseResponseMatrix[band][point] * target[point];
		outGains[band] = (std::max)(-MAX_FILTER_GAIN_DB,
			(std::min)(MAX_FILTER_GAIN_DB, outGains[band]));
	}

	// A biquad's dB response is not perfectly linear in gain. Reusing the
	// well-conditioned unit-response inverse as a residual corrector converges
	// to the CSV anchors in a few inexpensive background-thread iterations.
	// Band coefficients are precomputed once per pass: they do not depend on
	// the evaluation frequency.
	BiquadCoeffs passCoeffs[NUM_BANDS];
	for (size_t band = 0; band < _activeBandCount; ++band)
		computeBiquadCoeffs(band, outGains[band], passCoeffs[band]);
	for (unsigned iteration = 1; iteration < FIT_ITERATIONS; ++iteration)
	{
		double residual[NUM_BANDS] = {};
		for (size_t point = 0; point < _activeBandCount; ++point)
		{
			double actual = bandResponseDb(passCoeffs, _activeBandCount,
				LoudnessProfile::LOUDNESS_PROFILE_TABLE[point].frequency);
			residual[point] = target[point] - actual;
		}

		for (size_t band = 0; band < _activeBandCount; ++band)
		{
			double correction = 0.0;
			for (size_t point = 0; point < _activeBandCount; ++point)
				correction += _inverseResponseMatrix[band][point] * residual[point];
			if (std::isfinite(correction))
			{
				outGains[band] = (std::max)(-MAX_FILTER_GAIN_DB,
					(std::min)(MAX_FILTER_GAIN_DB, outGains[band] + correction));
			}
		}
		for (size_t band = 0; band < _activeBandCount; ++band)
			computeBiquadCoeffs(band, outGains[band], passCoeffs[band]);
	}

	outputGainLinear = calculateHeadroomGain(outGains);
}

double LoudnessCorrectionFilter::calculateHeadroomGain(
	const std::vector<double>& gains) const
{
	if (_activeBandCount == 0)
		return 1.0;
	bool hasNonZeroGain = false;
	for (size_t band = 0; band < _activeBandCount; ++band)
	{
		if (std::abs(gains[band]) > 1.0e-12)
		{
			hasNonZeroGain = true;
			break;
		}
	}
	if (!hasNonZeroGain)
		return 1.0;

	double maximumResponse = findMaximumResponseDb(gains, 1.0, false);
	double outputGainLinear = maximumResponse <= 0.0 ? 1.0 :
		std::pow(10.0, -maximumResponse / 20.0);

	// The audible transfer is the complex sum L + H*C, not merely the
	// correction cascade C. LR28 gives L and H matching phase with
	// |L| + |H| = 1. The candidate normalizes the correction peak to 0 dB;
	// it must not impose an extra fixed attenuation at near-neutral volumes.
	// Scan the actual final transfer as a defense against implementation or
	// floating-point drift. The fallback search runs only if that invariant is
	// unexpectedly violated, and still happens off the audio callback.
	if (findMaximumResponseDb(gains, outputGainLinear, true) <=
		FINAL_RESPONSE_NUMERICAL_TOLERANCE_DB)
		return outputGainLinear;

	double safeGain = 0.0;
	double unsafeGain = outputGainLinear;
	for (unsigned iteration = 0; iteration < 48; ++iteration)
	{
		double candidate = 0.5 * (safeGain + unsafeGain);
		if (findMaximumResponseDb(gains, candidate, true) <=
			FINAL_RESPONSE_NUMERICAL_TOLERANCE_DB)
			safeGain = candidate;
		else
			unsafeGain = candidate;
	}
	return safeGain;
}

double LoudnessCorrectionFilter::findMaximumResponseDb(
	const std::vector<double>& gains,
	double outputGainLinear,
	bool includeSubsonicCrossover) const
{
	if (_activeBandCount == 0)
		return 0.0;

	double maximumFrequency = (std::min)(
		20000.0,
		0.499 * static_cast<double>(_sampleRate));
	if (maximumFrequency <= 1.0)
		return 0.0;

	const double minimumFrequency = 1.0;
	const double logMinimum = std::log(minimumFrequency);
	const double logMaximum = std::log(maximumFrequency);
	const double logStep = (logMaximum - logMinimum) /
		static_cast<double>(RESPONSE_SCAN_POINTS - 1);
	std::vector<double> responses(RESPONSE_SCAN_POINTS, 0.0);

	// Correction coefficients depend only on (band, gain, sample rate).
	// Precomputing them once makes the dense scan evaluate responses only;
	// the values are identical to recomputing per frequency point.
	BiquadCoeffs correctionCoeffs[NUM_BANDS];
	for (size_t band = 0; band < _activeBandCount; ++band)
		computeBiquadCoeffs(band, gains[band], correctionCoeffs[band]);

	auto responseAtLogFrequency = [
		this,
		&correctionCoeffs,
		outputGainLinear,
		includeSubsonicCrossover](double logFrequency)
	{
		double frequency = std::exp(logFrequency);
		if (includeSubsonicCrossover)
			return guardedTransferDb(correctionCoeffs, _activeBandCount,
				outputGainLinear, frequency);

		return 20.0 * std::log10(
			(std::max)(1.0e-15, outputGainLinear)) +
			bandResponseDb(
				correctionCoeffs, _activeBandCount, frequency);
	};

	// A dense logarithmic scan resolves the Q=2.2 peaking bands by hundreds of
	// samples. Each detected local maximum is then refined in log-frequency
	// space, avoiding the inter-bin peaks missed by the former 256-point scan.
	double maximumResponse = 0.0;
	for (unsigned point = 0; point < RESPONSE_SCAN_POINTS; ++point)
	{
		double logFrequency = logMinimum + static_cast<double>(point) * logStep;
		responses[point] = responseAtLogFrequency(logFrequency);
		maximumResponse = (std::max)(maximumResponse, responses[point]);
	}

	const double goldenRatioConjugate = 0.6180339887498948482;
	for (unsigned point = 1; point + 1 < RESPONSE_SCAN_POINTS; ++point)
	{
		if (responses[point] < responses[point - 1] ||
			responses[point] < responses[point + 1] ||
			(responses[point] == responses[point - 1] &&
				responses[point] == responses[point + 1]))
		{
			continue;
		}

		double left = logMinimum + static_cast<double>(point - 1) * logStep;
		double right = logMinimum + static_cast<double>(point + 1) * logStep;
		double innerLeft = right - goldenRatioConjugate * (right - left);
		double innerRight = left + goldenRatioConjugate * (right - left);
		double leftResponse = responseAtLogFrequency(innerLeft);
		double rightResponse = responseAtLogFrequency(innerRight);

		for (unsigned iteration = 0;
			iteration < RESPONSE_REFINEMENT_ITERATIONS;
			++iteration)
		{
			if (leftResponse < rightResponse)
			{
				left = innerLeft;
				innerLeft = innerRight;
				leftResponse = rightResponse;
				innerRight = left + goldenRatioConjugate * (right - left);
				rightResponse = responseAtLogFrequency(innerRight);
			}
			else
			{
				right = innerRight;
				innerRight = innerLeft;
				rightResponse = leftResponse;
				innerLeft = right - goldenRatioConjugate * (right - left);
				leftResponse = responseAtLogFrequency(innerLeft);
			}
		}

		maximumResponse = (std::max)(maximumResponse,
			(std::max)(leftResponse, rightResponse));
	}

	return maximumResponse;
}

void LoudnessCorrectionFilter::calculateFastShelfGains(
	double currentVolumeDb,
	std::vector<double>& outGains,
	double& outputGainLinear) const
{
	outGains.assign(NUM_BANDS, 0.0);
	outputGainLinear = 1.0;
	if (_activeBandCount == 0 || _fastFitPointCount == 0 ||
		_parameters.attenuation <= 0.0f)
		return;

	double currentVolume = std::isfinite(currentVolumeDb) ? currentVolumeDb : 0.0;
	currentVolume = (std::max)(-100.0, (std::min)(0.0, currentVolume));
	double referenceLevel = (std::max)(1.0, (std::min)(100.0,
		static_cast<double>(_parameters.referenceLevel)));
	double loudnessLevel = referenceLevel + currentVolume - _parameters.referenceOffset;
	loudnessLevel = (std::max)(0.0, (std::min)(100.0, loudnessLevel));

	// The exact same per-point targets as the full cascade. The shelves
	// below are least-squares fitted to them instead of solved exactly,
	// so the fast engine tracks the full response by construction.
	double target[NUM_BANDS] = {};
	for (size_t point = 0; point < _fastFitPointCount; ++point)
	{
		target[point] = static_cast<double>(_parameters.attenuation) *
			LoudnessProfile::computeContourDelta(loudnessLevel, referenceLevel, point);
	}

	const size_t shelfCount = (std::min)(_activeBandCount, FAST_BAND_COUNT);
	BiquadCoeffs unit[FAST_BAND_COUNT];
	for (size_t band = 0; band < shelfCount; ++band)
		computeFastShelfCoeffs(band, 1.0, unit[band]);

	// The fitted peak sets the headroom gain, so the target maximum is
	// anchored first; the remaining points share unit weight.
	size_t peakPoint = 0;
	for (size_t point = 1; point < _fastFitPointCount; ++point)
	{
		if (target[point] > target[peakPoint])
			peakPoint = point;
	}

	// Unit dB responses of each shelf at every fit point, then the
	// symmetric normal equations for the least-squares gains.
	double unitResponse[FAST_BAND_COUNT][NUM_BANDS] = {};
	double normal[FAST_BAND_COUNT][FAST_BAND_COUNT] = {};
	double projected[FAST_BAND_COUNT] = {};
	for (size_t point = 0; point < _fastFitPointCount; ++point)
	{
		double frequency = LoudnessProfile::LOUDNESS_PROFILE_TABLE[point].frequency;
		double weight = point == peakPoint ? FAST_PEAK_ANCHOR_WEIGHT : 1.0;
		for (size_t band = 0; band < shelfCount; ++band)
		{
			unitResponse[band][point] = bandResponseDb(&unit[band], 1, frequency);
			projected[band] += weight * unitResponse[band][point] * target[point];
			for (size_t other = 0; other <= band; ++other)
				normal[band][other] += weight * unitResponse[band][point] * unitResponse[other][point];
		}
	}
	normal[0][1] = normal[1][0];

	double fitted[FAST_BAND_COUNT] = {};
	if (solveFastNormalEquations(normal, projected, shelfCount, fitted))
	{
		for (size_t band = 0; band < shelfCount; ++band)
			outGains[band] = clampFastShelfGain(band, fitted[band]);
	}

	// One residual pass, mirroring the full cascade: shelf dB responses
	// are only approximately linear in gain.
	BiquadCoeffs rendered[FAST_BAND_COUNT];
	for (size_t band = 0; band < shelfCount; ++band)
		computeFastShelfCoeffs(band, outGains[band], rendered[band]);
	double residualProjected[FAST_BAND_COUNT] = {};
	for (size_t point = 0; point < _fastFitPointCount; ++point)
	{
		double frequency = LoudnessProfile::LOUDNESS_PROFILE_TABLE[point].frequency;
		double actual = 0.0;
		for (size_t band = 0; band < shelfCount; ++band)
			actual += bandResponseDb(&rendered[band], 1, frequency);
		double residual = target[point] - actual;
		double weight = point == peakPoint ? FAST_PEAK_ANCHOR_WEIGHT : 1.0;
		for (size_t band = 0; band < shelfCount; ++band)
			residualProjected[band] += weight * unitResponse[band][point] * residual;
	}
	double refinement[FAST_BAND_COUNT] = {};
	if (solveFastNormalEquations(normal, residualProjected, shelfCount, refinement))
	{
		for (size_t band = 0; band < shelfCount; ++band)
			outGains[band] = clampFastShelfGain(band, outGains[band] + refinement[band]);
	}

	outputGainLinear = calculateFastHeadroomGain(outGains);
}

bool LoudnessCorrectionFilter::solveFastNormalEquations(
	const double normal[FAST_BAND_COUNT][FAST_BAND_COUNT],
	const double projected[FAST_BAND_COUNT],
	size_t shelfCount,
	double (&solution)[FAST_BAND_COUNT])
{
	solution[0] = 0.0;
	solution[1] = 0.0;
	if (shelfCount == 0)
		return false;
	if (shelfCount == 1)
	{
		if (std::abs(normal[0][0]) < 1.0e-12)
			return false;
		solution[0] = projected[0] / normal[0][0];
		return true;
	}
	double scale = std::abs(normal[0][0] * normal[1][1]);
	double determinant = normal[0][0] * normal[1][1] - normal[0][1] * normal[1][0];
	if (std::abs(determinant) < 1.0e-12 * scale)
		return false;
	solution[0] = (projected[0] * normal[1][1] - projected[1] * normal[0][1]) / determinant;
	solution[1] = (normal[0][0] * projected[1] - normal[1][0] * projected[0]) / determinant;
	return true;
}

double LoudnessCorrectionFilter::clampFastShelfGain(size_t bandIndex, double gainDb)
{
	const double maximumGain = bandIndex == 0 ?
		FAST_LOW_SHELF_MAX_GAIN_DB : FAST_PEAK_MAX_GAIN_DB;
	return (std::max)(-maximumGain, (std::min)(maximumGain, gainDb));
}

double LoudnessCorrectionFilter::calculateFastHeadroomGain(
	const std::vector<double>& gains) const
{
	if (_activeBandCount == 0)
		return 1.0;
	bool hasNonZeroGain = false;
	for (size_t band = 0; band < _activeBandCount; ++band)
	{
		if (std::abs(gains[band]) > 1.0e-12)
		{
			hasNonZeroGain = true;
			break;
		}
	}
	if (!hasNonZeroGain)
		return 1.0;

	// The Fast response has at most one Q=2 peak in addition to its low shelf,
	// so the coarse sweep below is enough for the candidate. The guarded
	// A-domain transfer is still verified exactly as on the full path.
	double maximumResponse = findFastMaximumResponseDb(gains, 1.0, false);
	double outputGainLinear = maximumResponse <= 0.0 ? 1.0 :
		std::pow(10.0, -maximumResponse / 20.0);

	if (findFastMaximumResponseDb(gains, outputGainLinear, true) <=
		FINAL_RESPONSE_NUMERICAL_TOLERANCE_DB)
		return outputGainLinear;

	double safeGain = 0.0;
	double unsafeGain = outputGainLinear;
	for (unsigned iteration = 0; iteration < 48; ++iteration)
	{
		double candidate = 0.5 * (safeGain + unsafeGain);
		if (findFastMaximumResponseDb(gains, candidate, true) <=
			FINAL_RESPONSE_NUMERICAL_TOLERANCE_DB)
			safeGain = candidate;
		else
			unsafeGain = candidate;
	}
	return safeGain;
}

double LoudnessCorrectionFilter::findFastMaximumResponseDb(
	const std::vector<double>& gains,
	double outputGainLinear,
	bool includeSubsonicCrossover) const
{
	if (_activeBandCount == 0)
		return 0.0;

	double maximumFrequency = (std::min)(
		20000.0,
		0.499 * static_cast<double>(_sampleRate));
	if (maximumFrequency <= 1.0)
		return 0.0;

	const double minimumFrequency = 1.0;
	const double logMinimum = std::log(minimumFrequency);
	const double logMaximum = std::log(maximumFrequency);
	const double logStep = (logMaximum - logMinimum) /
		static_cast<double>(FAST_RESPONSE_SCAN_POINTS - 1);
	std::vector<double> responses(FAST_RESPONSE_SCAN_POINTS, 0.0);

	BiquadCoeffs correctionCoeffs[FAST_BAND_COUNT];
	for (size_t band = 0; band < _activeBandCount; ++band)
		computeFastShelfCoeffs(band, gains[band], correctionCoeffs[band]);

	auto responseAtLogFrequency = [
		this,
		&correctionCoeffs,
		outputGainLinear,
		includeSubsonicCrossover](double logFrequency)
	{
		double frequency = std::exp(logFrequency);
		if (includeSubsonicCrossover)
			return guardedTransferDb(correctionCoeffs, _activeBandCount,
				outputGainLinear, frequency);

		return 20.0 * std::log10(
			(std::max)(1.0e-15, outputGainLinear)) +
			bandResponseDb(
				correctionCoeffs, _activeBandCount, frequency);
	};

	double maximumResponse = 0.0;
	for (unsigned point = 0; point < FAST_RESPONSE_SCAN_POINTS; ++point)
	{
		double logFrequency = logMinimum + static_cast<double>(point) * logStep;
		responses[point] = responseAtLogFrequency(logFrequency);
		maximumResponse = (std::max)(maximumResponse, responses[point]);
	}

	const double goldenRatioConjugate = 0.6180339887498948482;
	for (unsigned point = 1; point + 1 < FAST_RESPONSE_SCAN_POINTS; ++point)
	{
		if (responses[point] < responses[point - 1] ||
			responses[point] < responses[point + 1] ||
			(responses[point] == responses[point - 1] &&
				responses[point] == responses[point + 1]))
		{
			continue;
		}

		double left = logMinimum + static_cast<double>(point - 1) * logStep;
		double right = logMinimum + static_cast<double>(point + 1) * logStep;
		double innerLeft = right - goldenRatioConjugate * (right - left);
		double innerRight = left + goldenRatioConjugate * (right - left);
		double leftResponse = responseAtLogFrequency(innerLeft);
		double rightResponse = responseAtLogFrequency(innerRight);

		for (unsigned iteration = 0;
			iteration < FAST_RESPONSE_REFINEMENT_ITERATIONS;
			++iteration)
		{
			if (leftResponse < rightResponse)
			{
				left = innerLeft;
				innerLeft = innerRight;
				leftResponse = rightResponse;
				innerRight = left + goldenRatioConjugate * (right - left);
				rightResponse = responseAtLogFrequency(innerRight);
			}
			else
			{
				right = innerRight;
				innerRight = innerLeft;
				rightResponse = leftResponse;
				innerLeft = right - goldenRatioConjugate * (right - left);
				leftResponse = responseAtLogFrequency(innerLeft);
			}
		}

		maximumResponse = (std::max)(maximumResponse,
			(std::max)(leftResponse, rightResponse));
	}

	return maximumResponse;
}

void LoudnessCorrectionFilter::publishVolumeUpdate(
	double currentVolumeDb,
	std::vector<double>& scratchGains)
{
	double clampedVolumeDb = std::isfinite(currentVolumeDb) ?
		(std::max)(-100.0, (std::min)(0.0, currentVolumeDb)) : 0.0;
	double derivedScalar = (clampedVolumeDb + 100.0) / 100.0;
	publishVolumeUpdate(
		clampedVolumeDb,
		derivedScalar,
		false,
		scratchGains);
}

double LoudnessCorrectionFilter::calculateVolumeFollowGain(
	FilterParameters::VolumeFollowMode mode,
	double currentVolumeDb,
	double currentVolumeScalar,
	bool muted)
{
	if (mode == FilterParameters::VOLUME_FOLLOW_OFF)
		return 1.0;
	if (muted)
		return 0.0;

	const double minimumGain = 1.0e-5; // -100 dB, exact zero is reserved for mute.
	double levelDb = std::isfinite(currentVolumeDb) ? currentVolumeDb : 0.0;
	levelDb = (std::max)(-100.0, (std::min)(0.0, levelDb));
	double scalar = std::isfinite(currentVolumeScalar) ?
		currentVolumeScalar : (levelDb + 100.0) / 100.0;
	scalar = (std::max)(0.0, (std::min)(1.0, scalar));

	switch (mode)
	{
	case FilterParameters::VOLUME_FOLLOW_LINEAR:
		return (std::max)(minimumGain, scalar);
	case FilterParameters::VOLUME_FOLLOW_LOGARITHMIC:
		return (std::max)(minimumGain, scalar * scalar);
	case FilterParameters::VOLUME_FOLLOW_WINDOWS:
		return std::pow(10.0, levelDb / 20.0);
	case FilterParameters::VOLUME_FOLLOW_OFF:
	default:
		return 1.0;
	}
}

void LoudnessCorrectionFilter::publishVolumeFollowUpdate(
	double currentVolumeDb,
	double currentVolumeScalar,
	bool muted)
{
	const double followGain = calculateVolumeFollowGain(
		_parameters.volumeFollow,
		currentVolumeDb,
		currentVolumeScalar,
		muted);
	EnterCriticalSection(&_parameterUpdateSection);
	_pendingVolumeFollowGainLinear = followGain;
	_volumeFollowUpdated.store(true, std::memory_order_release);
	LeaveCriticalSection(&_parameterUpdateSection);
}

void LoudnessCorrectionFilter::publishVolumeUpdate(
	double currentVolumeDb,
	double currentVolumeScalar,
	bool muted,
	std::vector<double>& scratchGains)
{
	double outputGainLinear = 1.0;
	if (_parameters.engine == FilterParameters::ENGINE_FAST)
		calculateFastShelfGains(currentVolumeDb, scratchGains, outputGainLinear);
	else
		calculateBandGains(currentVolumeDb, scratchGains, outputGainLinear);
	bool identity = outputGainLinear == 1.0;
	for (size_t band = 0; band < _activeBandCount; ++band)
		identity = identity && std::abs(scratchGains[band]) <= 1.0e-12;

	EnterCriticalSection(&_parameterUpdateSection);
	for (size_t band = 0; band < _activeBandCount; ++band)
		computeBandCoeffs(band, scratchGains[band], _pendingCoeffs[band]);
	_pendingOutputGainLinear = outputGainLinear;
	_pendingIdentity = identity;
	_pendingVolumeFollowGainLinear = calculateVolumeFollowGain(
		_parameters.volumeFollow,
		currentVolumeDb,
		currentVolumeScalar,
		muted);
	_volumeFollowUpdated.store(true, std::memory_order_release);
	_coeffsUpdated.store(true, std::memory_order_release);
	LeaveCriticalSection(&_parameterUpdateSection);
}

void LoudnessCorrectionFilter::computeBiquadCoeffs(
	size_t bandIndex,
	double gainDb,
	BiquadCoeffs& coeffs) const
{
	gainDb = (std::max)(-MAX_FILTER_GAIN_DB, (std::min)(MAX_FILTER_GAIN_DB, gainDb));
	double frequency = LoudnessProfile::LOUDNESS_PROFILE_TABLE[bandIndex].frequency;
	double A = std::pow(10.0, gainDb / 40.0);
	double omega = 2.0 * PI * frequency / _sampleRate;
	double cosine = std::cos(omega);

	if (bandIndex + 1 == NUM_BANDS)
	{
		double alpha = std::sin(omega) / (2.0 * HIGH_SHELF_Q);
		double beta = 2.0 * std::sqrt(A) * alpha;
		double b0 = A * ((A + 1.0) + (A - 1.0) * cosine + beta);
		double b1 = -2.0 * A * ((A - 1.0) + (A + 1.0) * cosine);
		double b2 = A * ((A + 1.0) + (A - 1.0) * cosine - beta);
		double a0 = (A + 1.0) - (A - 1.0) * cosine + beta;
		double a1 = 2.0 * ((A - 1.0) - (A + 1.0) * cosine);
		double a2 = (A + 1.0) - (A - 1.0) * cosine - beta;
		coeffs.b0 = b0 / a0;
		coeffs.b1 = b1 / a0;
		coeffs.b2 = b2 / a0;
		coeffs.a1 = a1 / a0;
		coeffs.a2 = a2 / a0;
		return;
	}

	double alpha = std::sin(omega) / (2.0 * FILTER_Q);
	double b0 = 1.0 + alpha * A;
	double b1 = -2.0 * cosine;
	double b2 = 1.0 - alpha * A;
	double a0 = 1.0 + alpha / A;
	double a1 = -2.0 * cosine;
	double a2 = 1.0 - alpha / A;

	coeffs.b0 = b0 / a0;
	coeffs.b1 = b1 / a0;
	coeffs.b2 = b2 / a0;
	coeffs.a1 = a1 / a0;
	coeffs.a2 = a2 / a0;
}

void LoudnessCorrectionFilter::computeBandCoeffs(
	size_t bandIndex,
	double gainDb,
	BiquadCoeffs& coeffs) const
{
	if (_parameters.engine == FilterParameters::ENGINE_FAST)
		computeFastShelfCoeffs(bandIndex, gainDb, coeffs);
	else
		computeBiquadCoeffs(bandIndex, gainDb, coeffs);
}

double LoudnessCorrectionFilter::fastBandFrequency(size_t bandIndex)
{
	return bandIndex == 0 ?
		FAST_LOW_SHELF_FREQUENCY_HZ : FAST_PEAK_FREQUENCY_HZ;
}

BiQuad::Type LoudnessCorrectionFilter::fastBandType(size_t bandIndex)
{
	return bandIndex == 0 ? BiQuad::LOW_SHELF : BiQuad::PEAKING;
}

void LoudnessCorrectionFilter::computeFastShelfCoeffs(
	size_t bandIndex,
	double gainDb,
	BiquadCoeffs& coeffs) const
{
	coeffs.b0 = 1.0;
	coeffs.b1 = 0.0;
	coeffs.b2 = 0.0;
	coeffs.a1 = 0.0;
	coeffs.a2 = 0.0;
	if (bandIndex >= FAST_BAND_COUNT)
		return;

	const double maximumGain = bandIndex == 0 ?
		FAST_LOW_SHELF_MAX_GAIN_DB : FAST_PEAK_MAX_GAIN_DB;
	gainDb = (std::max)(-maximumGain, (std::min)(maximumGain, gainDb));
	const double A = std::pow(10.0, gainDb / 40.0);
	const double omega = 2.0 * PI *
		fastBandFrequency(bandIndex) / _sampleRate;
	const double sine = std::sin(omega);
	const double cosine = std::cos(omega);

	double b0 = 1.0;
	double b1 = 0.0;
	double b2 = 0.0;
	double a0 = 1.0;
	double a1 = 0.0;
	double a2 = 0.0;
	if (fastBandType(bandIndex) == BiQuad::PEAKING)
	{
		const double alpha = sine / (2.0 * FAST_PEAK_Q);
		b0 = 1.0 + alpha * A;
		b1 = -2.0 * cosine;
		b2 = 1.0 - alpha * A;
		a0 = 1.0 + alpha / A;
		a1 = -2.0 * cosine;
		a2 = 1.0 - alpha / A;
	}
	else
	{
		const double alpha = sine / 2.0 * std::sqrt(
			(A + 1.0 / A) * (1.0 / FAST_SHELF_SLOPE - 1.0) + 2.0);
		const double beta = 2.0 * std::sqrt(A) * alpha;
		b0 = A * ((A + 1.0) - (A - 1.0) * cosine + beta);
		b1 = 2.0 * A * ((A - 1.0) - (A + 1.0) * cosine);
		b2 = A * ((A + 1.0) - (A - 1.0) * cosine - beta);
		a0 = (A + 1.0) + (A - 1.0) * cosine + beta;
		a1 = -2.0 * ((A - 1.0) + (A + 1.0) * cosine);
		a2 = (A + 1.0) + (A - 1.0) * cosine - beta;
	}

	if (!std::isfinite(a0) || std::abs(a0) < 1.0e-20)
		return;
	coeffs.b0 = b0 / a0;
	coeffs.b1 = b1 / a0;
	coeffs.b2 = b2 / a0;
	coeffs.a1 = a1 / a0;
	coeffs.a2 = a2 / a0;
}

void LoudnessCorrectionFilter::computeCrossoverCoeffs(
	bool highPass,
	size_t sectionIndex,
	BiquadCoeffs& coeffs) const
{
	size_t butterworthSection =
		sectionIndex % (CROSSOVER_BUTTERWORTH_ORDER / 2);
	double q = 1.0 / (2.0 * std::sin(
		(2.0 * static_cast<double>(butterworthSection) + 1.0) * PI /
		(2.0 * static_cast<double>(CROSSOVER_BUTTERWORTH_ORDER))));
	double omega = 2.0 * PI * SUBSONIC_CROSSOVER_FREQUENCY_HZ / _sampleRate;
	double alpha = std::sin(omega) / (2.0 * q);
	double cosine = std::cos(omega);
	double a0 = 1.0 + alpha;
	double sineHalf = std::sin(0.5 * omega);
	double cosineHalf = std::cos(0.5 * omega);

	if (highPass)
	{
		// cos(omega / 2)^2 avoids adding nearly equal values.
		coeffs.b0 = cosineHalf * cosineHalf / a0;
		coeffs.b1 = -2.0 * coeffs.b0;
		coeffs.b2 = coeffs.b0;
	}
	else
	{
		// sin(omega / 2)^2 avoids the 1-cos cancellation that otherwise
		// becomes measurable at very high sample rates.
		coeffs.b0 = sineHalf * sineHalf / a0;
		coeffs.b1 = 2.0 * coeffs.b0;
		coeffs.b2 = coeffs.b0;
	}
	coeffs.a1 = -2.0 * cosine / a0;
	coeffs.a2 = (1.0 - alpha) / a0;
}

std::complex<double> LoudnessCorrectionFilter::biquadResponse(
	const BiquadCoeffs& coeffs,
	double frequency) const
{
	double omega = 2.0 * PI * frequency / _sampleRate;
	std::complex<double> z(std::cos(omega), -std::sin(omega));
	return biquadResponseAtZ(coeffs, z, z * z);
}

std::complex<double> LoudnessCorrectionFilter::biquadResponseAtZ(
	const BiquadCoeffs& coeffs,
	const std::complex<double>& z,
	const std::complex<double>& zSquared)
{
	std::complex<double> numerator =
		coeffs.b0 + coeffs.b1 * z + coeffs.b2 * zSquared;
	std::complex<double> denominator =
		1.0 + coeffs.a1 * z + coeffs.a2 * zSquared;
	if (std::norm(denominator) < 1.0e-30)
		return std::complex<double>(0.0, 0.0);
	return numerator / denominator;
}

double LoudnessCorrectionFilter::biquadResponseDb(
	size_t bandIndex,
	double gainDb,
	double frequency) const
{
	BiquadCoeffs coefficients;
	computeBiquadCoeffs(bandIndex, gainDb, coefficients);
	double magnitudeSquared = (std::max)(
		1.0e-30,
		std::norm(biquadResponse(coefficients, frequency)));
	return 10.0 * std::log10(magnitudeSquared);
}

double LoudnessCorrectionFilter::bandResponseDb(
	const BiquadCoeffs* coeffs,
	size_t bandCount,
	double frequency) const
{
	const double omega = 2.0 * PI * frequency / _sampleRate;
	const std::complex<double> z(std::cos(omega), -std::sin(omega));
	const std::complex<double> zSquared = z * z;
	std::complex<double> response(1.0, 0.0);
	for (size_t band = 0; band < bandCount; ++band)
		response *= biquadResponseAtZ(coeffs[band], z, zSquared);
	const double magnitudeSquared = (std::max)(
		1.0e-30, std::norm(response));
	return 10.0 * std::log10(magnitudeSquared);
}

double LoudnessCorrectionFilter::guardedTransferDb(
	const BiquadCoeffs* correctionCoeffs,
	size_t bandCount,
	double outputGainLinear,
	double frequency) const
{
	const double omega = 2.0 * PI * frequency / _sampleRate;
	const std::complex<double> z(std::cos(omega), -std::sin(omega));
	const std::complex<double> zSquared = z * z;
	std::complex<double> correction(outputGainLinear, 0.0);
	for (size_t band = 0; band < bandCount; ++band)
		correction *= biquadResponseAtZ(
			correctionCoeffs[band], z, zSquared);

	std::complex<double> lowpass(1.0, 0.0);
	std::complex<double> highpass(1.0, 0.0);
	for (size_t section = 0; section < CROSSOVER_SECTION_COUNT; ++section)
	{
		lowpass *= biquadResponseAtZ(
			_lowpassCoeffs[section], z, zSquared);
		highpass *= biquadResponseAtZ(
			_highpassCoeffs[section], z, zSquared);
	}

	double magnitudeSquared = (std::max)(
		1.0e-30,
		std::norm(lowpass + highpass * correction));
	return 10.0 * std::log10(magnitudeSquared);
}

double LoudnessCorrectionFilter::guardedResponseDb(
	const std::vector<double>& gains,
	double outputGainLinear,
	double frequency) const
{
	const double omega = 2.0 * PI * frequency / _sampleRate;
	const std::complex<double> z(std::cos(omega), -std::sin(omega));
	const std::complex<double> zSquared = z * z;
	std::complex<double> correction(outputGainLinear, 0.0);
	for (size_t band = 0; band < _activeBandCount; ++band)
	{
		BiquadCoeffs coefficients;
		computeBiquadCoeffs(band, gains[band], coefficients);
		correction *= biquadResponseAtZ(coefficients, z, zSquared);
	}

	std::complex<double> lowpass(1.0, 0.0);
	std::complex<double> highpass(1.0, 0.0);
	for (size_t section = 0; section < CROSSOVER_SECTION_COUNT; ++section)
	{
		lowpass *= biquadResponseAtZ(
			_lowpassCoeffs[section], z, zSquared);
		highpass *= biquadResponseAtZ(
			_highpassCoeffs[section], z, zSquared);
	}

	double magnitudeSquared = (std::max)(
		1.0e-30,
		std::norm(lowpass + highpass * correction));
	return 10.0 * std::log10(magnitudeSquared);
}

unsigned long __stdcall LoudnessCorrectionFilter::parameterUpdateThread(void* parameter)
{
	LoudnessCorrectionFilter* self = static_cast<LoudnessCorrectionFilter*>(parameter);
	VolumeController volumeController(self->getVolumeControllerEndpointId());
	double lastCorrectionVolume = self->_hasInitialAutomaticVolume ?
		self->_initialAutomaticVolume :
		std::numeric_limits<double>::quiet_NaN();
	double lastFollowVolume = lastCorrectionVolume;
	double lastVolumeScalar = self->_hasInitialAutomaticVolume ?
		self->_initialAutomaticVolumeScalar :
		std::numeric_limits<double>::quiet_NaN();
	bool lastMuted = self->_initialAutomaticMuted;
	ULONGLONG lastReadTime = 0;
	std::vector<double> gains;
	const bool correctionEnabled =
		self->_parameters.attenuation > 0.0f && self->_activeBandCount > 0;

	while (WaitForSingleObject(
		self->_stopParameterUpdateThreadEvent,
		UPDATE_POLL_INTERVAL_MS) == WAIT_TIMEOUT)
	{
		ULONGLONG now = GetTickCount64();
		bool callbackChanged = volumeController.hasVolumeChanged();
		bool fallbackPollDue = lastReadTime == 0 ||
			now - lastReadTime >= FALLBACK_POLL_INTERVAL_MS;
		if (!callbackChanged && !fallbackPollDue)
			continue;

		lastReadTime = now;
		EndpointVolumeState currentState;
		if (FAILED(volumeController.getVolumeState(currentState)))
		{
			self->_recoveryPending.store(false, std::memory_order_release);
			self->_runtimeBypass.store(true, std::memory_order_release);
			// Volume follow is the safety-critical realization of the user's
			// requested attenuation. Keep its last known gain while only the
			// contour-correction branch fades to identity.
			continue;
		}

		bool recovering = self->_runtimeBypass.load(std::memory_order_acquire);
		const bool correctionVolumeChanged =
			!std::isfinite(lastCorrectionVolume) ||
			std::abs(currentState.levelDb - lastCorrectionVolume) > 0.05;
		const bool followStateChanged = !std::isfinite(lastVolumeScalar) ||
			!std::isfinite(lastFollowVolume) ||
			std::abs(currentState.levelDb - lastFollowVolume) > 1.0e-6 ||
			std::abs(currentState.scalar - lastVolumeScalar) > 1.0e-6 ||
			currentState.muted != lastMuted || correctionVolumeChanged;
		if (!recovering && !correctionVolumeChanged && !followStateChanged)
			continue;

		if (correctionEnabled && (recovering || correctionVolumeChanged))
		{
			self->publishVolumeUpdate(
				currentState.levelDb,
				currentState.scalar,
				currentState.muted,
				gains);
		}
		else
		{
			// A mute or scalar-only notification changes only the final wideband
			// gain. With no correction branch, every endpoint update does too.
			// Avoid needless coefficient publication and 350 ms bank warmup.
			self->publishVolumeFollowUpdate(
				currentState.levelDb,
				currentState.scalar,
				currentState.muted);
		}
		if (correctionVolumeChanged)
			lastCorrectionVolume = currentState.levelDb;
		if (recovering)
		{
			// Keep bypass asserted until the audio thread consumes this recovery.
			// With correction enabled, it installs the recovered coefficients and
			// lets them crossfade from the common magnitude-unity A = L + H domain;
			// without a correction branch, consuming the follow target is sufficient.
			self->_recoveryPending.store(true, std::memory_order_release);
		}
		lastFollowVolume = currentState.levelDb;
		lastVolumeScalar = currentState.scalar;
		lastMuted = currentState.muted;
	}
	return 0;
}

#pragma AVRT_CODE_BEGIN
void LoudnessCorrectionFilter::installPendingVolumeFollow()
{
	// Manual volume is immutable for this filter instance; initialization has
	// already installed its fixed gain and no publisher thread exists.
	if (_parameters.useManualVolume)
		return;
	if (!_volumeFollowUpdated.load(std::memory_order_acquire) ||
		!TryEnterCriticalSection(&_parameterUpdateSection))
	{
		return;
	}

	_targetVolumeFollowGainLinear = _pendingVolumeFollowGainLinear;
	const double difference =
		_targetVolumeFollowGainLinear - _volumeFollowGainLinear;
	if (std::abs(difference) <= 1.0e-15)
	{
		_volumeFollowGainLinear = _targetVolumeFollowGainLinear;
		_volumeFollowStepPerSample = 0.0;
		_volumeFollowRampRemaining = 0;
	}
	else
	{
		_volumeFollowStepPerSample =
			difference / static_cast<double>(_volumeFollowRampLength);
		_volumeFollowRampRemaining = _volumeFollowRampLength;
	}
	_volumeFollowUpdated.store(false, std::memory_order_release);
	LeaveCriticalSection(&_parameterUpdateSection);
}

double LoudnessCorrectionFilter::volumeFollowGainAtFrame(unsigned frame) const
{
	if (_volumeFollowRampRemaining == 0)
		return _volumeFollowGainLinear;
	unsigned step = (std::min)(frame + 1, _volumeFollowRampRemaining);
	return _volumeFollowGainLinear +
		_volumeFollowStepPerSample * static_cast<double>(step);
}

void LoudnessCorrectionFilter::applyVolumeFollow(
	double* samples,
	unsigned frameCount) const
{
	if (_parameters.volumeFollow == FilterParameters::VOLUME_FOLLOW_OFF)
		return;
	if (_volumeFollowRampRemaining == 0)
	{
		if (_volumeFollowGainLinear == 1.0)
			return;
		for (unsigned frame = 0; frame < frameCount; ++frame)
			samples[frame] *= _volumeFollowGainLinear;
		return;
	}
	for (unsigned frame = 0; frame < frameCount; ++frame)
		samples[frame] *= volumeFollowGainAtFrame(frame);
}

void LoudnessCorrectionFilter::advanceVolumeFollow(unsigned frameCount)
{
	if (_volumeFollowRampRemaining == 0 || frameCount == 0)
		return;
	unsigned advanced = (std::min)(frameCount, _volumeFollowRampRemaining);
	_volumeFollowGainLinear +=
		_volumeFollowStepPerSample * static_cast<double>(advanced);
	_volumeFollowRampRemaining -= advanced;
	if (_volumeFollowRampRemaining == 0)
	{
		_volumeFollowGainLinear = _targetVolumeFollowGainLinear;
		_volumeFollowStepPerSample = 0.0;
	}
}

void LoudnessCorrectionFilter::process(double** output, double** input, unsigned frameCount)
{
	if (!_parameters.state)
	{
		for (size_t channel = 0; channel < _channelCount; ++channel)
		{
			for (unsigned frame = 0; frame < frameCount; ++frame)
				output[channel][frame] = input[channel][frame];
		}
		return;
	}

	installPendingVolumeFollow();

	const bool correctionEnabled =
		_parameters.attenuation > 0.0f && _activeBandCount > 0;
	if (!correctionEnabled)
	{
		for (size_t channel = 0; channel < _channelCount; ++channel)
		{
			for (unsigned frame = 0; frame < frameCount; ++frame)
				output[channel][frame] = input[channel][frame];
			applyVolumeFollow(output[channel], frameCount);
		}
		advanceVolumeFollow(frameCount);

		// With no tonal-correction branch there is no bank to warm or crossfade.
		// Still consume a successful endpoint-recovery token so the worker does
		// not repeat a needless Full fit on every fallback poll. Preserve the
		// same failure-wins ordering as the correction-enabled path below.
		if (_recoveryPending.load(std::memory_order_acquire))
		{
			_runtimeBypass.store(false, std::memory_order_release);
			bool expectedRecovery = true;
			if (!_recoveryPending.compare_exchange_strong(
				expectedRecovery,
				false,
				std::memory_order_acq_rel,
				std::memory_order_acquire))
			{
				_runtimeBypass.store(true, std::memory_order_release);
			}
		}
		return;
	}

	bool runtimeBypass = _runtimeBypass.load(std::memory_order_acquire);
	bool crossoverDomainReady = !_crossoverPrewarmActive &&
		!_crossoverHandoffActive &&
		_crossoverDomainChannelCount == _channelCount;
	if (runtimeBypass && !_runtimeBypassWasActive && crossoverDomainReady)
	{
		// Preserve the currently audible correction-bank composite and fade only
		// its residual relative to A = L + H. Cancelling the bank transition here
		// would create a single-sample corrected-to-A discontinuity.
		_bypassFadePosition = 0;
		_bypassFadeActive = true;
	}
	else if (runtimeBypass && !crossoverDomainReady)
	{
		// Before the cold-start raw-to-A handoff there is no audible correction
		// transition to preserve. Stay raw and keep the fixed LR28 histories live.
		_bypassFadePosition = 0;
		_bypassFadeActive = false;
		_warmupActive = false;
		_crossfadeActive = false;
		_transitionFromBypass = false;
	}

	bool recoveryReady = runtimeBypass &&
		!_bypassFadeActive &&
		_recoveryPending.load(std::memory_order_acquire);

	const bool pendingCoefficientsReady =
		(!runtimeBypass || recoveryReady) &&
		!_crossoverPrewarmActive && !_crossoverHandoffActive &&
		!_warmupActive && !_crossfadeActive && !_bypassFadeActive &&
		_coeffsUpdated.load(std::memory_order_acquire);
	const bool pendingCoefficientsLocked = pendingCoefficientsReady &&
		(_parameters.useManualVolume ||
		 TryEnterCriticalSection(&_parameterUpdateSection));
	if (pendingCoefficientsLocked)
	{
		bool requiresTransition =
			!(_pendingIdentity && _bankIdentity[_activeBankIndex]);
		if (requiresTransition)
		{
			_transitionBankIndex = 1 - _activeBankIndex;
			for (size_t channel = 0; channel < _channelCount; ++channel)
			{
				// Correction-filter histories belong to their old coefficients and
				// cannot be reused safely. The fixed LR crossover histories, however,
				// are live input histories and avoid a multi-second 25 Hz restart.
				for (size_t band = 0; band < _activeBandCount; ++band)
					_biquadBanks[_transitionBankIndex][channel][band].resetState();
				for (size_t section = 0;
					section < CROSSOVER_SECTION_COUNT;
					++section)
				{
					_lowpassBanks[_transitionBankIndex][channel][section] =
						_lowpassBanks[_activeBankIndex][channel][section];
					_highpassBanks[_transitionBankIndex][channel][section] =
						_highpassBanks[_activeBankIndex][channel][section];
				}
			}
			for (size_t band = 0; band < _activeBandCount; ++band)
			{
				double coefficients[4] = {
					_pendingCoeffs[band].b1,
					_pendingCoeffs[band].b2,
					_pendingCoeffs[band].a1,
					_pendingCoeffs[band].a2
				};
				for (size_t channel = 0; channel < _channelCount; ++channel)
					_biquadBanks[_transitionBankIndex][channel][band]
						.setCoefficients(
							coefficients,
							_pendingCoeffs[band].b0);
			}
			_targetOutputGainLinear = _pendingOutputGainLinear;
			_bankIdentity[_transitionBankIndex] = _pendingIdentity;
			_warmupPosition = 0;
			_crossfadePosition = 0;
			_warmupActive = true;
			_crossfadeActive = false;
			_transitionFromBypass = recoveryReady;
		}
		else
		{
			// A latest pending identity target is already audible in the shared
			// magnitude-unity A = L + H domain. No coefficient transition is needed.
			_outputGainLinear = 1.0;
			_warmupActive = false;
			_crossfadeActive = false;
			_transitionFromBypass = false;
		}
		_coeffsUpdated.store(false, std::memory_order_release);
		if (recoveryReady)
		{
			// Clear bypass before claiming the recovery token. A simultaneous
			// read failure clears that token first; if so, restore bypass instead
			// of overwriting the newer failure with a stale success.
			_runtimeBypass.store(false, std::memory_order_release);
			bool expectedRecovery = true;
			if (_recoveryPending.compare_exchange_strong(
				expectedRecovery,
				false,
				std::memory_order_acq_rel,
				std::memory_order_acquire))
			{
				runtimeBypass = false;
			}
			else
			{
				_runtimeBypass.store(true, std::memory_order_release);
				runtimeBypass = true;
				_warmupActive = false;
				_crossfadeActive = false;
				_transitionFromBypass = false;
			}
		}
		if (!_parameters.useManualVolume)
			LeaveCriticalSection(&_parameterUpdateSection);
	}

	// The common case has no coefficient transition in flight. Process each
	// recursive section across the complete block so its coefficients and state
	// remain in registers instead of walking all 57 Full-engine BiQuad objects
	// for every sample. Transition and handoff paths retain the sample-ordered
	// implementation below because their decisions can change within a block.
	const bool useSectionMajorBlockPath =
		frameCount <= _maximumFrameCount &&
		crossoverDomainReady &&
		!_crossoverHandoffActive &&
		!_warmupActive &&
		!_crossfadeActive &&
		!_bypassFadeActive;
	if (useSectionMajorBlockPath)
	{
		for (size_t channel = 0; channel < _channelCount; ++channel)
		{
			double* const inputChannel = input[channel];
			double* const outputChannel = output[channel];
			double* const lowpass = _lowpassBlockScratch.data();
			for (unsigned frame = 0; frame < frameCount; ++frame)
				lowpass[frame] = inputChannel[frame];

			for (size_t section = 0;
				section < CROSSOVER_SECTION_COUNT;
				++section)
			{
				_lowpassBanks[_activeBankIndex][channel][section]
					.processBlock(lowpass, frameCount);
			}

			if (outputChannel != inputChannel)
			{
				for (unsigned frame = 0; frame < frameCount; ++frame)
					outputChannel[frame] = inputChannel[frame];
			}
			for (size_t section = 0;
				section < CROSSOVER_SECTION_COUNT;
				++section)
			{
				_highpassBanks[_activeBankIndex][channel][section]
					.processBlock(outputChannel, frameCount);
			}

			if (runtimeBypass || _bankIdentity[_activeBankIndex])
			{
				// A bypassed or identity bank needs only the common A = L + H
				// domain. The correction state is reset and warmed before reuse.
				for (unsigned frame = 0; frame < frameCount; ++frame)
					outputChannel[frame] = lowpass[frame] + outputChannel[frame];
			}
			else
			{
				for (unsigned frame = 0; frame < frameCount; ++frame)
					outputChannel[frame] *= _outputGainLinear;
				for (size_t band = 0; band < _activeBandCount; ++band)
				{
					_biquadBanks[_activeBankIndex][channel][band]
						.processBlock(outputChannel, frameCount);
				}
				for (unsigned frame = 0; frame < frameCount; ++frame)
					outputChannel[frame] = lowpass[frame] + outputChannel[frame];
			}

			for (size_t band = 0; band < _activeBandCount; ++band)
				_biquadBanks[_activeBankIndex][channel][band].removeDenormals();
			for (size_t section = 0;
				section < CROSSOVER_SECTION_COUNT;
				++section)
			{
				_lowpassBanks[_activeBankIndex][channel][section].removeDenormals();
				_highpassBanks[_activeBankIndex][channel][section].removeDenormals();
			}

			applyVolumeFollow(outputChannel, frameCount);
		}

		advanceVolumeFollow(frameCount);
		_runtimeBypassWasActive = runtimeBypass;
		return;
	}

	struct BankSample
	{
		double identity;
		double corrected;
	};

	for (size_t channel = 0; channel < _channelCount; ++channel)
	{
		double* inputChannel = input[channel];
		double* outputChannel = output[channel];
		for (unsigned frame = 0; frame < frameCount; ++frame)
		{
			double inputSample = inputChannel[frame];
			auto processBank = [this, channel, inputSample](
				size_t bankIndex,
				double outputGainLinear) -> BankSample
			{
				double lowpassSample = inputSample;
				for (size_t section = 0;
					section < CROSSOVER_SECTION_COUNT;
					++section)
				{
					lowpassSample = _lowpassBanks[bankIndex][channel][section]
						.process(lowpassSample);
				}

				double highpassIdentitySample = inputSample;
				for (size_t section = 0;
					section < CROSSOVER_SECTION_COUNT;
					++section)
				{
					highpassIdentitySample =
						_highpassBanks[bankIndex][channel][section]
							.process(highpassIdentitySample);
				}
				const double identitySample =
					lowpassSample + highpassIdentitySample;
				if (_bankIdentity[bankIndex])
					return BankSample{ identitySample, identitySample };
				double highpassCorrectedSample =
					highpassIdentitySample * outputGainLinear;
				for (size_t band = 0; band < _activeBandCount; ++band)
				{
					highpassCorrectedSample =
						_biquadBanks[bankIndex][channel][band]
							.process(highpassCorrectedSample);
				}
				return BankSample{
					identitySample,
					lowpassSample + highpassCorrectedSample
				};
			};

			BankSample activeSample = processBank(
				_activeBankIndex,
				_outputGainLinear);

			if (!_crossoverDomainActive[channel])
			{
				if (_crossoverHandoffActive)
				{
					bool hasPrevious =
						_crossoverHandoffHasPrevious[channel] != 0;
					bool switchToCrossoverDomain = false;
					double handoffStep = 0.0;
					if (hasPrevious)
					{
						double previousRaw =
							_crossoverHandoffPreviousRaw[channel];
						double previousIdentity =
							_crossoverHandoffPreviousIdentity[channel];
						// Switch only when the sampled raw/A intersection produces no
						// larger output step than either signal's natural one-sample step.
						// If this crossing is not safe, keep raw and try the next one;
						// there is deliberately no timeout that can force a click.
						double naturalStep = 0.0;
						switchToCrossoverDomain = isSafeCrossoverHandoff(
							previousRaw,
							inputSample,
							previousIdentity,
							activeSample.identity,
							handoffStep,
							naturalStep);
						if (switchToCrossoverDomain)
						{
							_crossoverDomainActive[channel] = 1;
							++_crossoverDomainChannelCount;
							_maximumCrossoverHandoffStep = (std::max)(
								_maximumCrossoverHandoffStep,
								handoffStep);
						}
					}

					_crossoverHandoffPreviousRaw[channel] = inputSample;
					_crossoverHandoffPreviousIdentity[channel] =
						activeSample.identity;
					_crossoverHandoffHasPrevious[channel] = 1;

					if (switchToCrossoverDomain)
					{
						outputChannel[frame] = activeSample.identity;
						continue;
					}
				}

				outputChannel[frame] = inputSample;
				continue;
			}

			double audibleComposite;
			if (_warmupActive)
			{
				// Feed every state in the target bank real input while keeping it
				// silent. LP and HP state are bank-local, so recovery never reuses a
				// frozen crossover history.
				(void)processBank(
					_transitionBankIndex,
					_targetOutputGainLinear);

				audibleComposite =
					(_transitionFromBypass || _bankIdentity[_activeBankIndex]) ?
					activeSample.identity : activeSample.corrected;
			}
			else if (_crossfadeActive)
			{
				BankSample transitionBankSample = processBank(
					_transitionBankIndex,
					_targetOutputGainLinear);
				double transitionSample =
					_bankIdentity[_transitionBankIndex] ?
					transitionBankSample.identity :
					transitionBankSample.corrected;

				if (_crossfadePosition + frame < _crossfadeLength)
				{
					double activeAudibleSample =
						(_transitionFromBypass ||
							_bankIdentity[_activeBankIndex]) ?
						activeSample.identity : activeSample.corrected;
					double mix =
						static_cast<double>(_crossfadePosition + frame + 1) /
						static_cast<double>(_crossfadeLength);
					audibleComposite =
						activeAudibleSample * (1.0 - mix) +
						transitionSample * mix;
				}
				else
				{
					audibleComposite = transitionSample;
				}
			}
			else
			{
				audibleComposite = _bankIdentity[_activeBankIndex] ?
					activeSample.identity : activeSample.corrected;
			}

			if (runtimeBypass)
			{
				double residualGain = 0.0;
				if (_bypassFadeActive && _bypassFadeLength > 1)
				{
					unsigned fadeIndex = (std::min)(
						_bypassFadePosition + frame,
						_bypassFadeLength - 1);
					residualGain = 1.0 -
						static_cast<double>(fadeIndex) /
						static_cast<double>(_bypassFadeLength - 1);
				}
				outputChannel[frame] = activeSample.identity +
					residualGain * (audibleComposite - activeSample.identity);
			}
			else
			{
				outputChannel[frame] = audibleComposite;
			}
		}

		for (size_t bank = 0; bank < 2; ++bank)
		{
			for (size_t band = 0; band < _activeBandCount; ++band)
				_biquadBanks[bank][channel][band].removeDenormals();
			for (size_t section = 0; section < CROSSOVER_SECTION_COUNT; ++section)
			{
				_lowpassBanks[bank][channel][section].removeDenormals();
				_highpassBanks[bank][channel][section].removeDenormals();
			}
		}

		applyVolumeFollow(outputChannel, frameCount);
	}

	if (_crossoverHandoffActive &&
		_crossoverDomainChannelCount >= _channelCount)
	{
		_crossoverHandoffActive = false;
	}

	if (_bypassFadeActive)
	{
		unsigned remaining = _bypassFadeLength - _bypassFadePosition;
		unsigned advanced = (std::min)(frameCount, remaining);
		_bypassFadePosition += advanced;
		if (_bypassFadePosition >= _bypassFadeLength)
		{
			// The audible residual has reached zero; only now is it safe to
			// discard a partially warmed or crossfading correction bank.
			_bypassFadeActive = false;
			_warmupActive = false;
			_crossfadeActive = false;
			_transitionFromBypass = false;
		}
	}

	if (_crossoverPrewarmActive)
	{
		unsigned remaining =
			_crossoverPrewarmLength - _crossoverPrewarmPosition;
		unsigned advanced = (std::min)(frameCount, remaining);
		_crossoverPrewarmPosition += advanced;
		if (_crossoverPrewarmPosition >= _crossoverPrewarmLength)
		{
			// Fixed LR28 histories are now ready, but raw and A = L + H can be
			// nearly opposite in the subsonic band. Each channel must first switch
			// at a bounded discrete intersection; until then it remains fail-closed
			// raw. Pending correction is installed only after every channel is in A.
			_crossoverPrewarmActive = false;
			_crossoverHandoffActive = true;
			std::fill(
				_crossoverHandoffHasPrevious.begin(),
				_crossoverHandoffHasPrevious.end(),
				0);
		}
	}
	else if (_warmupActive)
	{
		unsigned remaining = _warmupLength - _warmupPosition;
		unsigned advanced = (std::min)(frameCount, remaining);
		_warmupPosition += advanced;
		if (_warmupPosition >= _warmupLength)
		{
			_warmupActive = false;
			_crossfadePosition = 0;
			_crossfadeActive = true;
		}
	}
	else if (_crossfadeActive)
	{
		unsigned remaining = _crossfadeLength - _crossfadePosition;
		unsigned advanced = (std::min)(frameCount, remaining);
		_crossfadePosition += advanced;
		if (_crossfadePosition >= _crossfadeLength)
		{
			_activeBankIndex = _transitionBankIndex;
			_outputGainLinear = _targetOutputGainLinear;
			_crossfadeActive = false;
			_transitionFromBypass = false;
		}
	}

	advanceVolumeFollow(frameCount);
	_runtimeBypassWasActive = runtimeBypass;
}
#pragma AVRT_CODE_END
