/*
    This file is part of Equalizer APO, a system-wide equalizer.
    Copyright (C) 2017  Alexander Walch
    Copyright (C) 2026  Equalizer APO contributors

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
*/

#pragma once

#include "LoudnessProfile.h"
#include "ParameterArchive.h"
#include <IFilter.h>
#include <filters/BiQuad.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <regex>
#include <vector>

#pragma AVRT_VTABLES_BEGIN
class LoudnessCorrectionFilter : public IFilter
{
public:
	static const size_t NUM_BANDS = LoudnessProfile::FREQUENCY_COUNT;

	struct FilterParameters
	{
		enum BindingMode
		{
			BINDING_SINGLE = 0,
			BINDING_ALL = 1
		};

		// Full runs the 29-band fitted correction. Fast fits a low shelf
		// plus, when the sample rate permits, a top peaking filter to the
		// same targets; it skips the response-matrix fit and dense search.
		enum EngineMode
		{
			ENGINE_FULL = 0,
			ENGINE_FAST = 1
		};

		// Optional APO-owned attenuation for endpoints whose Windows master
		// volume is observable but is not actually applied by the audio route.
		enum VolumeFollowMode
		{
			VOLUME_FOLLOW_OFF = 0,
			VOLUME_FOLLOW_LINEAR = 1,
			VOLUME_FOLLOW_LOGARITHMIC = 2,
			VOLUME_FOLLOW_WINDOWS = 3
		};

		bool state;
		float referenceLevel;
		float referenceOffset;
		float attenuation;
		float manualVolume;
		bool useManualVolume;
		BindingMode binding;
		EngineMode engine;
		VolumeFollowMode volumeFollow;

		std::vector<char> serialize()
		{
			ParameterArchive archive;
			archive.add(1, L"Schema");
			archive.add(std::wstring(L"FormulaLoudnessV1"), L"Model");
			archive.add(std::wstring(
				binding == BINDING_ALL ? L"All" : L"Single"), L"Binding");
			// The default full engine is omitted so profiles written before
			// Engine existed keep their exact text.
			if (engine == ENGINE_FAST)
				archive.add(std::wstring(L"Fast"), L"Engine");
			// Existing profiles and ordinary endpoints must not receive the same
			// Windows attenuation twice, so Off remains the omitted default.
			if (volumeFollow == VOLUME_FOLLOW_LINEAR ||
				volumeFollow == VOLUME_FOLLOW_LOGARITHMIC ||
				volumeFollow == VOLUME_FOLLOW_WINDOWS)
			{
				const wchar_t* mode = L"Windows";
				if (volumeFollow == VOLUME_FOLLOW_LINEAR)
					mode = L"Linear";
				else if (volumeFollow == VOLUME_FOLLOW_LOGARITHMIC)
					mode = L"Logarithmic";
				archive.add(std::wstring(mode), L"VolumeFollow");
			}
			archive.add(state, L"State");
			archive.add(referenceLevel, L"ReferenceLevel");
			archive.add(referenceOffset, L"ReferenceOffset");
			archive.add(attenuation, L"Attenuation");
			if (useManualVolume)
				archive.add(manualVolume, L"Volume");
			return archive.getSerializedParameters();
		}

		template<typename T> bool deSerialize(const T& parameters)
		{
			ParameterArchive archive(parameters);
			const bool isFormulaSchema = archive.find(std::wregex(
				L"(?:^|\\s)Schema\\s+1(?=\\s|$)", std::regex_constants::icase));
			const bool isFormulaModel = archive.find(std::wregex(
				L"(?:^|\\s)Model\\s+FormulaLoudnessV1(?=\\s|$)",
				std::regex_constants::icase));

			// Only an explicit, complete marker can select this model. Older
			// unmarked formula settings overlap the original Mixomo shelf syntax,
			// so the editor must ask the user which interpretation to preserve.
			if (!(isFormulaSchema && isFormulaModel))
				return true;
			size_t bindingCount = archive.count(std::wregex(
				L"(?:^|\\s)Binding(?=\\s|$)", std::regex_constants::icase));
			size_t engineCount = archive.count(std::wregex(
				L"(?:^|\\s)Engine(?=\\s|$)", std::regex_constants::icase));
			size_t volumeFollowCount = archive.count(std::wregex(
				L"(?:^|\\s)VolumeFollow(?=\\s|$)", std::regex_constants::icase));
			if (archive.count(std::wregex(L"(?:^|\\s)Schema(?=\\s|$)",
					std::regex_constants::icase)) != 1 ||
				archive.count(std::wregex(L"(?:^|\\s)Model(?=\\s|$)",
					std::regex_constants::icase)) != 1 ||
				archive.count(std::wregex(L"(?:^|\\s)State(?=\\s|$)",
					std::regex_constants::icase)) != 1 ||
				archive.count(std::wregex(L"(?:^|\\s)ReferenceLevel(?=\\s|$)",
					std::regex_constants::icase)) != 1 ||
				archive.count(std::wregex(L"(?:^|\\s)ReferenceOffset(?=\\s|$)",
					std::regex_constants::icase)) != 1 ||
				bindingCount > 1 ||
				engineCount > 1 ||
				volumeFollowCount > 1 ||
				archive.count(std::wregex(L"(?:^|\\s)Attenuation(?=\\s|$)",
					std::regex_constants::icase)) > 1 ||
				archive.count(std::wregex(L"(?:^|\\s)Volume(?=\\s|$)",
					std::regex_constants::icase)) > 1)
			{
				return true;
			}
			if (bindingCount == 0)
			{
				// Schema 1 profiles written before Binding was introduced used
				// exact APO-endpoint tracking.
				binding = BINDING_SINGLE;
			}
			else if (archive.find(std::wregex(
				L"(?:^|\\s)Binding\\s+All(?=\\s|$)",
				std::regex_constants::icase)))
			{
				binding = BINDING_ALL;
			}
			else if (archive.find(std::wregex(
				L"(?:^|\\s)Binding\\s+Single(?=\\s|$)",
				std::regex_constants::icase)))
			{
				binding = BINDING_SINGLE;
			}
			else
			{
				return true;
			}

			if (volumeFollowCount == 0 || archive.find(std::wregex(
				L"(?:^|\\s)VolumeFollow\\s+Off(?=\\s|$)",
				std::regex_constants::icase)))
			{
				volumeFollow = VOLUME_FOLLOW_OFF;
			}
			else if (archive.find(std::wregex(
				L"(?:^|\\s)VolumeFollow\\s+Linear(?=\\s|$)",
				std::regex_constants::icase)))
			{
				volumeFollow = VOLUME_FOLLOW_LINEAR;
			}
			else if (archive.find(std::wregex(
				L"(?:^|\\s)VolumeFollow\\s+Logarithmic(?=\\s|$)",
				std::regex_constants::icase)))
			{
				volumeFollow = VOLUME_FOLLOW_LOGARITHMIC;
			}
			else if (archive.find(std::wregex(
				L"(?:^|\\s)VolumeFollow\\s+Windows(?=\\s|$)",
				std::regex_constants::icase)))
			{
				volumeFollow = VOLUME_FOLLOW_WINDOWS;
			}
			else
			{
				return true;
			}

			if (engineCount == 0)
			{
				// Schema 1 profiles written before Engine was introduced use
				// the full engine.
				engine = ENGINE_FULL;
			}
			else if (archive.find(std::wregex(
				L"(?:^|\\s)Engine\\s+Fast(?=\\s|$)",
				std::regex_constants::icase)))
			{
				engine = ENGINE_FAST;
			}
			else if (archive.find(std::wregex(
				L"(?:^|\\s)Engine\\s+Full(?=\\s|$)",
				std::regex_constants::icase)))
			{
				engine = ENGINE_FULL;
			}
			else
			{
				return true;
			}

			int error = 0;
			error += archive.get(state, std::wregex(
				L"(?:^|\\s)State\\s+(0|1)(?=\\s|$)", std::regex_constants::icase));
			error += archive.get(referenceLevel,
				std::wregex(L"(?:^|\\s)ReferenceLevel\\s+([-+]?(?:[0-9]+(?:[\\.,][0-9]*)?|[\\.,][0-9]+))(?=\\s|$)",
					std::regex_constants::icase));
			error += archive.get(referenceOffset,
				std::wregex(L"(?:^|\\s)ReferenceOffset\\s+([-+]?(?:[0-9]+(?:[\\.,][0-9]*)?|[\\.,][0-9]+))(?=\\s|$)",
					std::regex_constants::icase));

			// Attenuation and Volume are optional for backward compatibility.
			int attenuationError = archive.get(attenuation,
				std::wregex(L"(?:^|\\s)Attenuation\\s+([-+]?(?:[0-9]+(?:[\\.,][0-9]*)?|[\\.,][0-9]+))(?=\\s|$)",
					std::regex_constants::icase));
			if (attenuationError != 0 || !std::isfinite(attenuation))
				attenuation = 1.0f;

			int volumeError = archive.get(manualVolume,
				std::wregex(L"(?:^|\\s)Volume\\s+([-+]?(?:[0-9]+(?:[\\.,][0-9]*)?|[\\.,][0-9]+))(?=\\s|$)",
					std::regex_constants::icase));
			useManualVolume = volumeError == 0 && std::isfinite(manualVolume);
			if (!useManualVolume)
				manualVolume = 0.0f;

			if (error != 0)
				return true;

			// Invalid levels fail closed instead of being normalized into a
			// different audible setting.
			if (!std::isfinite(referenceLevel) ||
				!std::isfinite(referenceOffset) ||
				referenceLevel <= 0.0f)
				return true;

			normalize();
			return false;
		}

		FilterParameters()
			: state(true),
			  referenceLevel(80.0f),
			  referenceOffset(0.0f),
			  attenuation(1.0f),
			  manualVolume(0.0f),
			  useManualVolume(false),
			  binding(BINDING_SINGLE),
			  engine(ENGINE_FULL),
			  volumeFollow(VOLUME_FOLLOW_OFF),
			  _isInitialized(false)
		{
		}

		template<typename T> FilterParameters(T input)
			: state(true),
			  referenceLevel(80.0f),
			  referenceOffset(0.0f),
			  attenuation(1.0f),
			  manualVolume(0.0f),
			  useManualVolume(false),
			  binding(BINDING_SINGLE),
			  engine(ENGINE_FULL),
			  volumeFollow(VOLUME_FOLLOW_OFF),
			  _isInitialized(false)
		{
			_isInitialized = !deSerialize<T>(input);
		}

		bool isInitialized() const
		{
			return _isInitialized;
		}

	private:
		void normalize()
		{
			if (!std::isfinite(referenceLevel) || referenceLevel <= 0.0f)
				referenceLevel = 1.0f;
			referenceLevel = (std::max)(1.0f, (std::min)(100.0f, referenceLevel));

			if (!std::isfinite(referenceOffset))
				referenceOffset = 0.0f;
			referenceOffset = (std::max)(-100.0f, (std::min)(100.0f, referenceOffset));

			if (!std::isfinite(attenuation))
				attenuation = 1.0f;
			attenuation = (std::max)(0.0f, (std::min)(1.0f, attenuation));

			if (!std::isfinite(manualVolume))
				manualVolume = 0.0f;
			manualVolume = (std::max)(-100.0f, (std::min)(0.0f, manualVolume));

			if (volumeFollow < VOLUME_FOLLOW_OFF ||
				volumeFollow > VOLUME_FOLLOW_WINDOWS)
			{
				volumeFollow = VOLUME_FOLLOW_OFF;
			}
		}

		bool _isInitialized;
	};

	struct BiquadCoeffs
	{
		double b0;
		double b1;
		double b2;
		double a1;
		double a2;
	};

	explicit LoudnessCorrectionFilter(const FilterParameters& fParameters);
	virtual ~LoudnessCorrectionFilter();
	virtual bool getInPlace() { return true; }
	virtual void setRuntimeContext(const FilterRuntimeContext& context) { _runtimeContext = context; }
	virtual std::vector<std::wstring> initialize(
		float sampleRate,
		unsigned maxFrameCount,
		std::vector<std::wstring> channelNames);
	virtual void process(double** output, double** input, unsigned frameCount);
	static double calculateVolumeFollowGain(
		FilterParameters::VolumeFollowMode mode,
		double currentVolumeDb,
		double currentVolumeScalar,
		bool muted);

private:
	friend class LoudnessCorrectionFilterTestAccess;

	static const unsigned UPDATE_POLL_INTERVAL_MS = 50;
	static const unsigned FALLBACK_POLL_INTERVAL_MS = 1000;
	static constexpr double FILTER_Q = 2.2;
	static constexpr double HIGH_SHELF_Q = 0.9;
	static constexpr double PI = 3.1415926535897932384626433832795;
	static constexpr double MAX_FILTER_GAIN_DB = 48.0;
	static constexpr double FINAL_RESPONSE_NUMERICAL_TOLERANCE_DB = 1.0e-6;
	static constexpr double SUBSONIC_CROSSOVER_FREQUENCY_HZ = 25.0;
	static const size_t CROSSOVER_SECTION_COUNT = 14;
	static const unsigned CROSSOVER_BUTTERWORTH_ORDER = 14;
	static constexpr double CROSSOVER_HISTORY_PREWARM_SECONDS = 1.0;
	static constexpr double BYPASS_FADE_SECONDS = 0.01;
	static constexpr double FILTER_WARMUP_SECONDS = 0.25;
	static constexpr double COEFFICIENT_CROSSFADE_SECONDS = 0.1;
	static constexpr double VOLUME_FOLLOW_RAMP_SECONDS = 0.01;
	static const unsigned FIT_ITERATIONS = 4;
	static const unsigned RESPONSE_SCAN_POINTS = 4097;
	static const unsigned RESPONSE_REFINEMENT_ITERATIONS = 32;

	// Fast engine: a low shelf plus a top peaking filter when the sample
	// rate permits, least-squares fitted to the same targets as the full
	// cascade. The coarse sweep below is enough for smooth responses; the
	// dense refined search stays on the full path only.
	static const size_t FAST_BAND_COUNT = 2;
	static constexpr double FAST_LOW_SHELF_FREQUENCY_HZ = 120.0;
	static constexpr double FAST_PEAK_FREQUENCY_HZ = 12500.0;
	static constexpr double FAST_PEAK_Q = 2.0;
	// Same bounds as the full cascade: the fit needs the same room, and
	// the guarded headroom gain keeps it clip-free.
	static constexpr double FAST_LOW_SHELF_MAX_GAIN_DB = 48.0;
	static constexpr double FAST_PEAK_MAX_GAIN_DB = 48.0;
	static constexpr double FAST_SHELF_SLOPE = 0.4;
	// The least-squares peak anchor. The fitted peak sets the headroom
	// gain, so the target maximum is weighted to match it first.
	static constexpr double FAST_PEAK_ANCHOR_WEIGHT = 20.0;
	static const unsigned FAST_RESPONSE_SCAN_POINTS = 256;
	static const unsigned FAST_RESPONSE_REFINEMENT_ITERATIONS = 8;

	void computeResponseInverse();
	static bool isSafeCrossoverHandoff(
		double previousRaw,
		double currentRaw,
		double previousIdentity,
		double currentIdentity,
		double& handoffStep,
		double& naturalStep);
	bool canTrackAutomaticVolume() const;
	std::wstring getVolumeControllerEndpointId() const;
	void calculateBandGains(
		double currentVolumeDb,
		std::vector<double>& outGains,
		double& outputGainLinear) const;
	double calculateHeadroomGain(const std::vector<double>& gains) const;
	void publishVolumeUpdate(double currentVolumeDb, std::vector<double>& scratchGains);
	void publishVolumeUpdate(
		double currentVolumeDb,
		double currentVolumeScalar,
		bool muted,
		std::vector<double>& scratchGains);
	void publishVolumeFollowUpdate(
		double currentVolumeDb,
		double currentVolumeScalar,
		bool muted);
	void installPendingVolumeFollow();
	double volumeFollowGainAtFrame(unsigned frame) const;
	void applyVolumeFollow(double* samples, unsigned frameCount) const;
	void advanceVolumeFollow(unsigned frameCount);
	void calculateFastShelfGains(
		double currentVolumeDb,
		std::vector<double>& outGains,
		double& outputGainLinear) const;
	double calculateFastHeadroomGain(const std::vector<double>& gains) const;
	double findFastMaximumResponseDb(
		const std::vector<double>& gains,
		double outputGainLinear,
		bool includeSubsonicCrossover) const;
	// Solves the symmetric shelf normal equations N*x = p. Returns false
	// when degenerate; the solution is zeroed then.
	static bool solveFastNormalEquations(
		const double normal[FAST_BAND_COUNT][FAST_BAND_COUNT],
		const double projected[FAST_BAND_COUNT],
		size_t shelfCount,
		double (&solution)[FAST_BAND_COUNT]);
	static double clampFastShelfGain(size_t bandIndex, double gainDb);
	void computeBiquadCoeffs(size_t bandIndex, double gainDb, BiquadCoeffs& coeffs) const;
	// Dispatches to the full peaking cascade or the fast shelves depending
	// on the configured engine. Coefficients depend only on (band, gain,
	// sample rate), never on the evaluation frequency, so scans precompute
	// them once instead of once per frequency point.
	void computeBandCoeffs(size_t bandIndex, double gainDb, BiquadCoeffs& coeffs) const;
	void computeFastShelfCoeffs(
		size_t bandIndex,
		double gainDb,
		BiquadCoeffs& coeffs) const;
	static double fastBandFrequency(size_t bandIndex);
	static BiQuad::Type fastBandType(size_t bandIndex);
	// Sum of |H| over precomputed bands, in dB. No coefficient work.
	double bandResponseDb(
		const BiquadCoeffs* coeffs,
		size_t bandCount,
		double frequency) const;
	// Complete A-domain transfer |L + H*C| over precomputed correction
	// bands, in dB. No coefficient work.
	double guardedTransferDb(
		const BiquadCoeffs* correctionCoeffs,
		size_t bandCount,
		double outputGainLinear,
		double frequency) const;
	void computeCrossoverCoeffs(
		bool highPass,
		size_t sectionIndex,
		BiquadCoeffs& coeffs) const;
	std::complex<double> biquadResponse(
		const BiquadCoeffs& coeffs,
		double frequency) const;
	static std::complex<double> biquadResponseAtZ(
		const BiquadCoeffs& coeffs,
		const std::complex<double>& z,
		const std::complex<double>& zSquared);
	double biquadResponseDb(size_t bandIndex, double gainDb, double frequency) const;
	double guardedResponseDb(
		const std::vector<double>& gains,
		double outputGainLinear,
		double frequency) const;
	double findMaximumResponseDb(
		const std::vector<double>& gains,
		double outputGainLinear,
		bool includeSubsonicCrossover) const;

	static unsigned long __stdcall parameterUpdateThread(void* parameter);

	void* _parameterUpdateThreadHandle;
	void* _stopParameterUpdateThreadEvent;
	CRITICAL_SECTION _parameterUpdateSection;

	FilterParameters _parameters;
	FilterRuntimeContext _runtimeContext;
	bool _hasInitialAutomaticVolume;
	double _initialAutomaticVolume;
	double _initialAutomaticVolumeScalar;
	bool _initialAutomaticMuted;
	std::atomic<bool> _runtimeBypass;
	std::atomic<bool> _recoveryPending;
	size_t _channelCount;
	size_t _activeBandCount;
	unsigned _maximumFrameCount;
	// Number of profile-table points the fast engine fits. It follows the
	// same Nyquist cap as the full cascade so both engines aim at the same
	// audible target set.
	size_t _fastFitPointCount;
	float _sampleRate;

	// Both banks are allocated during initialize(). A new bank starts with
	// cleared state, warms silently, and then crossfades without allocating.
	std::vector<std::vector<BiQuad>> _biquadBanks[2]; // [bank][channel][band]
	std::vector<std::vector<BiQuad>> _lowpassBanks[2]; // [bank][channel][section]
	std::vector<std::vector<BiQuad>> _highpassBanks[2]; // [bank][channel][section]
	// Settled processing runs one recursive section across a whole block so
	// its coefficients and history stay hot. Buffers are allocated only here,
	// during initialize(), and never resized by the audio callback.
	std::vector<double> _lowpassBlockScratch;
	size_t _activeBankIndex;
	size_t _transitionBankIndex;
	unsigned _crossoverPrewarmPosition;
	unsigned _crossoverPrewarmLength;
	unsigned _warmupPosition;
	unsigned _warmupLength;
	unsigned _crossfadePosition;
	unsigned _crossfadeLength;
	unsigned _bypassFadePosition;
	unsigned _bypassFadeLength;
	bool _warmupActive;
	bool _crossfadeActive;
	bool _bypassFadeActive;
	bool _runtimeBypassWasActive;
	bool _transitionFromBypass;
	bool _crossoverPrewarmActive;
	bool _crossoverHandoffActive;
	std::vector<unsigned char> _crossoverDomainActive;
	std::vector<unsigned char> _crossoverHandoffHasPrevious;
	std::vector<double> _crossoverHandoffPreviousRaw;
	std::vector<double> _crossoverHandoffPreviousIdentity;
	size_t _crossoverDomainChannelCount;
	double _maximumCrossoverHandoffStep;
	double _inverseResponseMatrix[NUM_BANDS][NUM_BANDS];
	BiquadCoeffs _pendingCoeffs[NUM_BANDS];
	BiquadCoeffs _lowpassCoeffs[CROSSOVER_SECTION_COUNT];
	BiquadCoeffs _highpassCoeffs[CROSSOVER_SECTION_COUNT];
	bool _bankIdentity[2];
	bool _pendingIdentity;
	double _outputGainLinear;
	double _targetOutputGainLinear;
	double _pendingOutputGainLinear;
	// APO-owned volume follows the complete output after the correction
	// crossover. The worker publishes targets; the callback consumes them with
	// a short, allocation-free ramp shared by every channel.
	double _volumeFollowGainLinear;
	double _targetVolumeFollowGainLinear;
	double _volumeFollowStepPerSample;
	double _pendingVolumeFollowGainLinear;
	unsigned _volumeFollowRampRemaining;
	unsigned _volumeFollowRampLength;
	std::atomic<bool> _volumeFollowUpdated;
	std::atomic<bool> _coeffsUpdated;
};
#pragma AVRT_VTABLES_END
