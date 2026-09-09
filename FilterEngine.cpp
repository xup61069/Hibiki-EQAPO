/*
    This file is part of EqualizerAPO, a system-wide equalizer.
    Copyright (C) 2014  Jonas Thedering

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
#define _USE_MATH_DEFINES
#include <cmath>
#include <sstream>
#include <fstream>
#include <algorithm>
#include <exception>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <immintrin.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <Shlwapi.h>
#include <Ks.h>
#include <KsMedia.h>
#include <mpParser.h>
#include <mpPackageCommon.h>
#include <mpPackageNonCmplx.h>
#include <mpPackageStr.h>
#include <mpPackageMatrix.h>

#include "helpers/RegistryHelper.h"
#include "helpers/StringHelper.h"
#include "helpers/LogHelper.h"
#include "helpers/MemoryHelper.h"
#include "helpers/ChannelHelper.h"
#include "helpers/ScopeGuard.h"
#include "FilterEngine.h"
#include "filters/ExpressionFilterFactory.h"
#include "filters/DeviceFilterFactory.h"
#include "filters/StageFilterFactory.h"
#include "filters/IfFilterFactory.h"
#include "filters/ChannelFilterFactory.h"
#include "filters/BiQuadFilterFactory.h"
#include "filters/ParametricEQFilterFactory.h"
#include "filters/IIRFilterFactory.h"
#include "filters/PreampFilterFactory.h"
#include "filters/OutputGuardFilterFactory.h"
#include "filters/PanFilterFactory.h"
#include "filters/CrossfeedFilterFactory.h"
#include "filters/ChorusFilterFactory.h"
#include "filters/ReverbFilterFactory.h"
#include "filters/ToneGeneratorFilterFactory.h"
#include "filters/VUMeterFilterFactory.h"
#include "filters/HeadphoneCalibrationFilterFactory.h"
#include "filters/OutProcGainFilterFactory.h"
#include "filters/OutProcBiquadFilterFactory.h"
#include "filters/OutProcVSTPluginFilterFactory.h"
#include "filters/DelayFilterFactory.h"
#include "filters/CopyFilterFactory.h"
#include "filters/IncludeFilterFactory.h"
#include "filters/ConvolutionFilterFactory.h"
#include "filters/GraphicEQFilterFactory.h"
#include "filters/VSTPluginFilterFactory.h"
#include "filters/loudnessCorrection/LoudnessCorrectionFilterFactory.h"
#include "filters/loudnessCorrection/LoudnessCorrectionFilter.h"
#include "filters/loudnessCorrection/OriginalLoudnessCorrectionFilterFactory.h"
#include "filters/loudnessCorrection/OriginalLoudnessCorrectionFilter.h"

using namespace std;
using namespace mup;

static_assert(std::atomic<FilterConfiguration*>::is_always_lock_free,
	"Realtime configuration handoff requires lock-free atomic pointers.");

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

	template<typename Sample>
	void bypassInterleaved(
		Sample* output,
		const Sample* input,
		unsigned inputChannels,
		unsigned outputChannels,
		unsigned frameCount)
	{
		if (output == input)
			return;
		size_t inputSampleCount = 0;
		size_t outputSampleCount = 0;
		if (!getSafeSampleCount<Sample>(
				inputChannels, frameCount, inputSampleCount) ||
			!getSafeSampleCount<Sample>(
				outputChannels, frameCount, outputSampleCount))
		{
			return;
		}

		if (inputChannels == outputChannels)
		{
			memcpy(output, input,
				outputSampleCount * sizeof(Sample));
			return;
		}

		const unsigned copyChannels = min(inputChannels, outputChannels);
		for (unsigned frame = 0; frame < frameCount; ++frame)
		{
			const Sample* inputFrame = input +
				static_cast<size_t>(frame) * inputChannels;
			Sample* outputFrame = output +
				static_cast<size_t>(frame) * outputChannels;
			for (unsigned channel = 0; channel < copyChannels; ++channel)
				outputFrame[channel] = inputFrame[channel];
			for (unsigned channel = copyChannels; channel < outputChannels; ++channel)
				outputFrame[channel] = Sample();
		}
	}

	template<typename Sample>
	void bypassPlanar(
		Sample** output,
		Sample** input,
		unsigned inputChannels,
		unsigned outputChannels,
		unsigned frameCount)
	{
		if (output == input)
			return;
		size_t channelSampleCount = 0;
		if (!getSafeSampleCount<Sample>(1, frameCount, channelSampleCount))
			return;

		const unsigned copyChannels = min(inputChannels, outputChannels);
		for (unsigned channel = 0; channel < copyChannels; ++channel)
		{
			if (output[channel] != input[channel])
				memcpy(output[channel], input[channel],
					channelSampleCount * sizeof(Sample));
		}
		for (unsigned channel = copyChannels; channel < outputChannels; ++channel)
			memset(output[channel], 0, channelSampleCount * sizeof(Sample));
	}

	class RegistryWatchSetTransaction
	{
	public:
		explicit RegistryWatchSetTransaction(
			std::unordered_set<std::wstring>& currentWatchKeys)
			: currentWatchKeys(currentWatchKeys), completed(false)
		{
			currentWatchKeys.swap(previousWatchKeys);
		}

		~RegistryWatchSetTransaction() noexcept
		{
			rollback();
		}

		void commit() noexcept
		{
			completed = true;
		}

		void rollback() noexcept
		{
			if (!completed)
			{
				currentWatchKeys.swap(previousWatchKeys);
				completed = true;
			}
		}

	private:
		std::unordered_set<std::wstring>& currentWatchKeys;
		std::unordered_set<std::wstring> previousWatchKeys;
		bool completed;
	};

	class CriticalSectionGuard
	{
	public:
		explicit CriticalSectionGuard(CRITICAL_SECTION& section) noexcept
			: section(section)
		{
			EnterCriticalSection(&section);
		}

		~CriticalSectionGuard() noexcept
		{
			LeaveCriticalSection(&section);
		}

	private:
		CRITICAL_SECTION& section;
	};

	class ConfigurationFileLoadError final : public std::runtime_error
	{
	public:
		ConfigurationFileLoadError()
			: std::runtime_error("Configuration file could not be read completely")
		{
		}
	};

	void destroyFilterInfos(std::vector<FilterInfo*>& filterInfos) noexcept
	{
		for (FilterInfo* filterInfo : filterInfos)
		{
			filterInfo->filter->~IFilter();
			MemoryHelper::free(filterInfo->filter);
			MemoryHelper::free(filterInfo->inChannels);
			MemoryHelper::free(filterInfo->outChannels);
			MemoryHelper::free(filterInfo);
		}
		filterInfos.clear();
	}

	bool isKnownCallbackUnsafeCommand(const std::wstring& command) noexcept
	{
		return command == L"OutProcGain" ||
			command == L"OutProcBiquad" ||
			command == L"OutProcVSTPlugin" ||
			command == L"VSTPlugin" ||
			command == L"VUMeter";
	}

	bool isExplicitManualLoudness(
		const std::wstring& parameters) noexcept
	{
		try
		{
			LoudnessCorrectionFilter::FilterParameters parsed(parameters);
			return parsed.isInitialized() &&
				(!parsed.state || parsed.useManualVolume);
		}
		catch (...)
		{
			return false;
		}
	}

	bool isDisabledOriginalLoudness(
		const std::wstring& parameters) noexcept
	{
		try
		{
			OriginalLoudnessCorrectionFilter::FilterParameters parsed(parameters);
			return parsed.isInitialized() && !parsed.state;
		}
		catch (...)
		{
			return false;
		}
	}
}

FilterEngine::FilterEngine()
	: asioManualLoudnessFactory(nullptr),
	  asioOriginalLoudnessFactory(nullptr),
	  processingPolicy(ProcessingPolicy::Full),
	  unsafeConfigurationRejected(false),
	  allocatedFrameCount(0),
	  preMix(false),
	  offlineAnalysis(false),
	  analysisMode(false),
	  deviceInfoKnown(false),
	  capture(false),
	  postMixInstalled(true),
	  sampleRate(0.0f),
	  inputChannelCount(0),
	  realChannelCount(0),
	  outputChannelCount(0),
	  channelMask(0),
	  maxFrameCount(0),
	  lastInPlace(true),
	  parser(nullptr),
	  currentConfig(nullptr),
	  pendingConfig(nullptr),
	  retiredConfig(nullptr),
	  hasInitialConfiguration(false),
	  transitionCounter(0),
	  transitionLength(0),
	  loadSemaphore(NULL),
	  threadHandle(NULL),
	  shutdownEvent(NULL),
	  lastInputWasSilent(false)
{
	InitializeCriticalSection(&loadSection);
	try
	{
		loadSemaphore = CreateSemaphore(NULL, 1, 1, NULL);
		parser = new ParserX();
		parser->EnableAutoCreateVar(true);

		factories.reserve(28);
		auto addFactory = [&](IFilterFactory* factory, bool callbackSafe = true)
		{
			std::unique_ptr<IFilterFactory> owner(factory);
			if (!callbackSafe)
				callbackUnsafeFactories.insert(factory);
			factories.push_back(factory);
			owner.release();
		};

		addFactory(new DeviceFilterFactory());
		addFactory(new IfFilterFactory());
		addFactory(new ExpressionFilterFactory());
		addFactory(new IncludeFilterFactory());
		addFactory(new StageFilterFactory());
		addFactory(new ChannelFilterFactory());
		addFactory(new IIRFilterFactory());
		addFactory(new BiQuadFilterFactory());
		addFactory(new ParametricEQFilterFactory());
		addFactory(new PreampFilterFactory());
		addFactory(new OutputGuardFilterFactory());
		addFactory(new PanFilterFactory());
		addFactory(new CrossfeedFilterFactory());
		addFactory(new ChorusFilterFactory());
		addFactory(new ReverbFilterFactory());
		addFactory(new ToneGeneratorFilterFactory());
		addFactory(new VUMeterFilterFactory(), false);
		addFactory(new HeadphoneCalibrationFilterFactory());
		addFactory(new OutProcGainFilterFactory(), false);
		addFactory(new OutProcBiquadFilterFactory(), false);
		addFactory(new OutProcVSTPluginFilterFactory(), false);
		addFactory(new DelayFilterFactory());
		addFactory(new CopyFilterFactory());
		addFactory(new ConvolutionFilterFactory());
		addFactory(new GraphicEQFilterFactory());
		addFactory(new VSTPluginFilterFactory(), false);
		auto* loudnessFactory = new LoudnessCorrectionFilterFactory();
		addFactory(loudnessFactory);
		asioManualLoudnessFactory = loudnessFactory;
		auto* originalLoudnessFactory =
			new OriginalLoudnessCorrectionFilterFactory();
		addFactory(originalLoudnessFactory);
		asioOriginalLoudnessFactory = originalLoudnessFactory;
	}
	catch (...)
	{
		for (IFilterFactory* factory : factories)
			delete factory;
		factories.clear();
		delete parser;
		parser = nullptr;
		if (loadSemaphore != NULL)
		{
			CloseHandle(loadSemaphore);
			loadSemaphore = NULL;
		}
		DeleteCriticalSection(&loadSection);
		throw;
	}
}

void FilterEngine::setProcessingPolicy(ProcessingPolicy policy) noexcept
{
	// Policy changes after initialization could make an already-published
	// configuration violate its contract. Ignore such late calls.
	if (maxFrameCount == 0 &&
		currentConfig.load(std::memory_order_acquire) == nullptr &&
		pendingConfig.load(std::memory_order_acquire) == nullptr)
	{
		processingPolicy = policy;
	}
}

bool FilterEngine::isFactoryAllowed(const IFilterFactory* factory) const noexcept
{
	return processingPolicy != ProcessingPolicy::AsioCallbackSafe ||
		callbackUnsafeFactories.find(factory) == callbackUnsafeFactories.end();
}

FilterEngine::~FilterEngine()
{
	stopNotificationThread();

	cleanupConfigurations();

	for (IFilterFactory* factory : factories)
		delete factory;

	delete parser;
	if (loadSemaphore != NULL)
		CloseHandle(loadSemaphore);
	DeleteCriticalSection(&loadSection);
}

void FilterEngine::resizeBuffers(unsigned frameCount) {
	if (allocatedFrameCount < frameCount || inputBuf2D.size() != inputChannelCount || outputBuf2D.size() != outputChannelCount) {

		TraceF(L"Reallocating internal double-precision buffers for %u frames and %u/%u channels.", frameCount, inputChannelCount, outputChannelCount);
		size_t inputSampleCount = 0;
		size_t outputSampleCount = 0;
		if (!getSafeSampleCount<double>(
				inputChannelCount, frameCount, inputSampleCount) ||
			!getSafeSampleCount<double>(
				outputChannelCount, frameCount, outputSampleCount))
		{
			allocatedFrameCount = 0;
			return;
		}
		allocatedFrameCount = 0;

		// Resize 1D buffers (for interleaved audio)
		try {
			inputBuf1D.resize(inputSampleCount);
			outputBuf1D.resize(outputSampleCount);

			// Resize 2D buffers (for non-interleaved audio)
			inputBuf2D.resize(inputChannelCount);
			for (unsigned i = 0; i < inputChannelCount; ++i) {
				inputBuf2D[i] = make_unique<double[]>(frameCount);
			}
			outputBuf2D.resize(outputChannelCount);
			for (unsigned i = 0; i < outputChannelCount; ++i) {
				outputBuf2D[i] = make_unique<double[]>(frameCount);
			}

			// Cache the raw channel arrays while allocations are allowed. The
			// non-interleaved audio callbacks can then remain allocation-free.
			inputBufPointers.resize(inputChannelCount);
			for (unsigned i = 0; i < inputChannelCount; ++i)
				inputBufPointers[i] = inputBuf2D[i].get();
			outputBufPointers.resize(outputChannelCount);
			for (unsigned i = 0; i < outputChannelCount; ++i)
				outputBufPointers[i] = outputBuf2D[i].get();
			allocatedFrameCount = frameCount;
		}
		catch (const std::bad_alloc& e) {
			LogF(L"FATAL: Failed to allocate audio buffers. Exception: %S", e.what());
			allocatedFrameCount = 0;
		}
	}
}

void FilterEngine::setPreMix(bool preMix)
{
	this->preMix = preMix;
}

void FilterEngine::setOfflineAnalysis(bool offlineAnalysis)
{
	this->offlineAnalysis = offlineAnalysis;
}

void FilterEngine::clearDeviceInfo()
{
	deviceInfoKnown = false;
	capture = false;
	postMixInstalled = true;
	deviceName.clear();
	connectionName.clear();
	deviceGuid.clear();
	deviceString.clear();
}

void FilterEngine::setDeviceInfo(bool capture, bool postMixInstalled, const wstring& deviceName, const wstring& connectionName, const wstring& deviceGuid, const wstring& deviceString)
{
	this->deviceInfoKnown = true;
	this->capture = capture;
	this->postMixInstalled = postMixInstalled;
	this->deviceName = deviceName;
	this->connectionName = connectionName;
	this->deviceGuid = deviceGuid;
	this->deviceString = deviceString;
}

void FilterEngine::initialize(float sampleRate, unsigned inputChannelCount, unsigned realChannelCount, unsigned outputChannelCount, unsigned channelMask, unsigned maxFrameCount, const wstring& customPath)
{
	auto failInitialization = [&]() noexcept
	{
		stopNotificationThread();
		CriticalSectionGuard cleanupGuard(loadSection);
		cleanupConfigurations();
		destroyFilterInfos(filterInfos);
		this->sampleRate = sampleRate;
		this->inputChannelCount = inputChannelCount;
		this->realChannelCount = realChannelCount;
		this->outputChannelCount = outputChannelCount;
		this->channelMask = channelMask;
		this->maxFrameCount = 0;
		allocatedFrameCount = 0;
		transitionCounter = 0;
		transitionLength = 0;
		MemoryHelper::consumeAllocationFailure();
	};

	try
	{
	// LockForProcess may be called again on the same APO instance. Stop an old
	// notification thread before clearing a pending transition; otherwise that
	// thread could remain blocked forever waiting for the retired generation.
	stopNotificationThread();
	if (loadSemaphore != NULL)
		CloseHandle(loadSemaphore);
	loadSemaphore = CreateSemaphore(NULL, 1, 1, NULL);
	if (loadSemaphore == NULL)
	{
		LogF(L"Could not create the configuration reload semaphore: %s",
			StringHelper::getSystemErrorString(GetLastError()).c_str());
	}

	CriticalSectionGuard initializeGuard(loadSection);

	cleanupConfigurations();

	this->sampleRate = sampleRate;
	this->inputChannelCount = inputChannelCount;
	this->realChannelCount = realChannelCount;
	this->outputChannelCount = outputChannelCount;
	this->channelMask = channelMask;
	this->maxFrameCount = maxFrameCount;
	this->analysisMode = offlineAnalysis || !customPath.empty();
	this->transitionCounter = 0;
	const double transitionSampleCount =
		static_cast<double>(sampleRate) / 100.0;
	if (!std::isfinite(sampleRate) || sampleRate <= 0.0f ||
		sampleRate >= static_cast<float>((std::numeric_limits<int>::max)()) ||
		!std::isfinite(transitionSampleCount) ||
		transitionSampleCount >
			static_cast<double>((std::numeric_limits<unsigned>::max)()))
	{
		LogF(L"Invalid sample rate %.9g; filter engine will bypass",
			static_cast<double>(sampleRate));
		this->maxFrameCount = 0;
		allocatedFrameCount = 0;
		this->transitionLength = 0;
		return;
	}
	this->transitionLength = (std::max)(
		1u, static_cast<unsigned>(transitionSampleCount));
	resizeBuffers(maxFrameCount);
	if (maxFrameCount != 0 && allocatedFrameCount != maxFrameCount)
	{
		this->maxFrameCount = 0;
		return;
	}

	unsigned deviceChannelCount;
	if (capture)
		deviceChannelCount = inputChannelCount;
	else
		deviceChannelCount = outputChannelCount;

	if (channelMask == 0)
		channelMask = ChannelHelper::getDefaultChannelMask(deviceChannelCount);

	this->channelMask = channelMask;

	vector<wstring> channelNames = ChannelHelper::getChannelNames(deviceChannelCount, channelMask);
	TraceF(L"%d channels for this device: %s", deviceChannelCount, StringHelper::join(channelNames, L" ").c_str());

	if (customPath.empty())
	{
		try
		{
			configPath = RegistryHelper::readValue(APP_REGPATH, L"ConfigPath");
		}
		catch (RegistryException e)
		{
			LogF(L"Can't read config path because of: %s", e.getMessage().c_str());
			return;
		}
	}
	else
	{
		// A custom file is used by Benchmark and tests. It must also work on a
		// clean machine where Equalizer APO has not written ConfigPath yet.
		configPath.clear();
	}

	parser->ClearConst();
	parser->ClearFun();
	parser->ClearInfixOprt();
	parser->ClearOprt();
	parser->ClearPostfixOprt();
	parser->AddPackage(PackageCommon::Instance());
	parser->AddPackage(PackageNonCmplx::Instance());
	parser->AddPackage(PackageStr::Instance());
	parser->AddPackage(PackageMatrix::Instance());

	for (vector<IFilterFactory*>::const_iterator it = factories.cbegin(); it != factories.cend(); it++)
	{
		IFilterFactory* factory = *it;
		if (!isFactoryAllowed(factory))
			continue;
		factory->initialize(this);
	}

	if (!customPath.empty() || !configPath.empty())
	{
		loadConfig(customPath);

		if (threadHandle == NULL && customPath.empty() && loadSemaphore != NULL)
		{
			shutdownEvent = CreateEventW(NULL, true, false, NULL);
			if (shutdownEvent == NULL)
			{
				LogF(L"Could not create the configuration notification shutdown event: %s",
					StringHelper::getSystemErrorString(GetLastError()).c_str());
			}
			else
			{
				threadHandle = CreateThread(NULL, 0, notificationThread, this, 0, NULL);
			}
			if (threadHandle != NULL)
				TraceF(L"Successfully created directory change notification thread %d for %s and its subtree", GetThreadId(threadHandle), configPath.c_str());
			else if (shutdownEvent != NULL)
			{
				DWORD error = GetLastError();
				LogF(L"Could not create the configuration notification thread: %s",
					StringHelper::getSystemErrorString(error).c_str());
				CloseHandle(shutdownEvent);
				shutdownEvent = NULL;
			}
		}
	}
	}
	catch (const std::bad_alloc&)
	{
		MemoryHelper::markAllocationFailure();
		failInitialization();
	}
	catch (...)
	{
		failInitialization();
	}
}

bool FilterEngine::loadConfig(const wstring& customPath)
{
	CriticalSectionGuard loadGuard(loadSection);
	unsafeConfigurationRejected.store(false, std::memory_order_release);
	timer.start();
	reclaimRetiredConfiguration();
	MemoryHelper::clearAllocationFailure();
	void* configStorage = NULL;
	FilterConfiguration* config = NULL;
	auto discardUnpublishedBuild = [&]() noexcept
	{
		if (config != NULL)
		{
			destroyConfiguration(config);
			config = NULL;
			configStorage = NULL;
		}
		else
		{
			destroyFilterInfos(filterInfos);
			MemoryHelper::free(configStorage);
			configStorage = NULL;
		}
	};

	configStorage = MemoryHelper::alloc(sizeof(FilterConfiguration));
	if (configStorage == NULL)
	{
		MemoryHelper::consumeAllocationFailure();
		timer.stop();
		return false;
	}

	try
	{
		if (offlineAnalysis)
		{
			loadedConfigurationFiles.clear();
			runtimeVolumeObservations.clear();
		}

		allChannelNames = ChannelHelper::getChannelNames(
			max(realChannelCount, outputChannelCount), channelMask);
		currentChannelNames = allChannelNames;
		lastChannelNames.clear();
		lastNewChannelNames.clear();
		lastInPlace = true;
		loadingSinglePrecision = false;
		precisionDirectiveSeen = false;
		RegistryWatchSetTransaction watchTransaction(watchRegistryKeys);
		parser->ClearVar();

		for (vector<IFilterFactory*>::const_iterator it = factories.cbegin(); it != factories.cend(); it++)
		{
			IFilterFactory* factory = *it;
			if (!isFactoryAllowed(factory))
				continue;
			vector<IFilter*> newFilters = factory->startOfConfiguration();
			if (!newFilters.empty())
				addFilters(newFilters);
		}

		if (customPath.empty())
			loadConfigFile(configPath + L"\\config.txt");
		else
			loadConfigFile(customPath);

		for (vector<IFilterFactory*>::const_iterator it = factories.cbegin(); it != factories.cend(); it++)
		{
			IFilterFactory* factory = *it;
			if (!isFactoryAllowed(factory))
				continue;
			vector<IFilter*> newFilters = factory->endOfConfiguration();
			if (!newFilters.empty())
				addFilters(newFilters);
		}

		config = new(configStorage) FilterConfiguration(
			this, filterInfos, static_cast<unsigned>(allChannelNames.size()));
		filterInfos.clear();
		const bool allocationFailed = MemoryHelper::consumeAllocationFailure();
		if (allocationFailed || !config->isValid())
		{
			LogF(L"Discarding configuration after an allocation failure");
			discardUnpublishedBuild();
			watchTransaction.rollback();
			timer.stop();
			return false;
		}

		double loadTime = timer.stop();
		TraceF(L"Finished loading configuration after %lf milliseconds", loadTime * 1000.0);

		if (!hasInitialConfiguration)
		{
			currentConfig.store(config, std::memory_order_release);
			config = NULL;
			hasInitialConfiguration = true;
			watchTransaction.commit();
			return false;
		}

		FilterConfiguration* expected = nullptr;
		if (!pendingConfig.compare_exchange_strong(
			expected, config,
			std::memory_order_release,
			std::memory_order_relaxed))
		{
			// The semaphore protocol normally guarantees an empty pending slot.
			// A direct, overlapping loadConfig call is rejected without touching
			// the configuration currently in use by the audio thread.
			LogF(L"Discarding an overlapping configuration reload");
			discardUnpublishedBuild();
			watchTransaction.rollback();
			return false;
		}

		config = NULL;
		watchTransaction.commit();
		return true;
	}
	catch (const std::bad_alloc&)
	{
		MemoryHelper::markAllocationFailure();
		discardUnpublishedBuild();
		MemoryHelper::consumeAllocationFailure();
		timer.stop();
		LogF(L"Discarding configuration after a standard-library allocation failure");
		return false;
	}
	catch (const std::exception& e)
	{
		discardUnpublishedBuild();
		MemoryHelper::consumeAllocationFailure();
		timer.stop();
		LogF(L"Discarding configuration after an unexpected error: %S", e.what());
		return false;
	}
	catch (...)
	{
		discardUnpublishedBuild();
		MemoryHelper::consumeAllocationFailure();
		timer.stop();
		LogF(L"Discarding configuration after an unknown error");
		return false;
	}
}

void FilterEngine::loadConfigFile(const wstring& path)
{
	TraceF(L"Loading configuration from %s", path.c_str());

	stringstream inputStream;
	BOOL readSucceeded = TRUE;
	DWORD readError = ERROR_SUCCESS;
	{
		HANDLE hFile = INVALID_HANDLE_VALUE;
		const ULONGLONG retryDeadline = GetTickCount64() + 1000;
		while (hFile == INVALID_HANDLE_VALUE)
		{
			hFile = CreateFile(path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
			if (hFile == INVALID_HANDLE_VALUE)
			{
				DWORD error = GetLastError();
				if (error != ERROR_SHARING_VIOLATION)
				{
					if (offlineAnalysis)
						loadedConfigurationFiles.push_back({path, string(), false});
					LogF(L"Error while reading configuration file %s: %s", path.c_str(), StringHelper::getSystemErrorString(error).c_str());
					throw ConfigurationFileLoadError();
				}
				if (shutdownEvent != NULL &&
					WaitForSingleObject(shutdownEvent, 0) == WAIT_OBJECT_0)
				{
					TraceF(L"Configuration reload cancelled during shutdown");
					throw ConfigurationFileLoadError();
				}
				if (GetTickCount64() >= retryDeadline)
				{
					if (offlineAnalysis)
						loadedConfigurationFiles.push_back({path, string(), false});
					LogF(L"Timed out while waiting to read configuration file %s",
						path.c_str());
					throw ConfigurationFileLoadError();
				}

				// file is being written, so wait
				Sleep(1);
			}
		}
		SCOPE_EXIT
		{
			CloseHandle(hFile);
		};

		char buf[8192];
		unsigned long bytesRead = 0;
		while ((readSucceeded = ReadFile(hFile, buf, sizeof(buf), &bytesRead, NULL))
			&& bytesRead != 0)
		{
			inputStream.write(buf, bytesRead);
		}
		if (readSucceeded == FALSE)
			readError = GetLastError();
	}

	if (readSucceeded == FALSE)
	{
		if (offlineAnalysis)
			loadedConfigurationFiles.push_back({path, inputStream.str(), false});
		LogF(L"Error while reading configuration file %s: %s", path.c_str(),
			StringHelper::getSystemErrorString(readError).c_str());
		throw ConfigurationFileLoadError();
	}

	if (offlineAnalysis)
		loadedConfigurationFiles.push_back({path, inputStream.str(), true});

	inputStream.seekg(0);

	vector<wstring> savedChannelNames = currentChannelNames;

	for (vector<IFilterFactory*>::const_iterator it = factories.cbegin(); it != factories.cend(); it++)
	{
		IFilterFactory* factory = *it;
		if (!isFactoryAllowed(factory))
			continue;
		vector<IFilter*> newFilters = factory->startOfFile(path);
		if (!newFilters.empty())
			addFilters(newFilters);
	}

	while (inputStream.good())
	{
		string encodedLine;
		getline(inputStream, encodedLine);
		if (encodedLine.size() > 0 && encodedLine[encodedLine.size() - 1] == '\r')
			encodedLine.resize(encodedLine.size() - 1);

		wstring line = StringHelper::toWString(encodedLine, CP_UTF8);
		if (line.find(L'\uFFFD') != -1)
			line = StringHelper::toWString(encodedLine, CP_ACP);

		size_t pos = line.find(L':');
		if (pos != -1)
		{
			wstring key = line.substr(0, pos);
			wstring value = line.substr(pos + 1);

			// allow to use indentation
			key = StringHelper::trim(key);
			for (vector<IFilterFactory*>::const_iterator it = factories.cbegin(); it != factories.cend(); it++)
			{
				IFilterFactory* factory = *it;
				if (!isFactoryAllowed(factory))
					continue;

				vector<IFilter*> newFilters;
				try
				{
					if (processingPolicy == ProcessingPolicy::AsioCallbackSafe &&
						factory == asioManualLoudnessFactory &&
						key == L"LoudnessCorrection" &&
						!isExplicitManualLoudness(value))
					{
						unsafeConfigurationRejected.store(
							true, std::memory_order_release);
						LogF(L"ASIO policy rejected endpoint-bound loudness correction");
						throw ConfigurationFileLoadError();
					}
					if (processingPolicy == ProcessingPolicy::AsioCallbackSafe &&
						factory == asioOriginalLoudnessFactory &&
						key == L"LoudnessCorrectionOriginal" &&
						!isDisabledOriginalLoudness(value))
					{
						unsafeConfigurationRejected.store(
							true, std::memory_order_release);
						LogF(L"ASIO policy rejected endpoint-bound original loudness correction");
						throw ConfigurationFileLoadError();
					}
					newFilters = factory->createFilter(path, key, value);
				}
				catch (const std::bad_alloc&)
				{
					MemoryHelper::markAllocationFailure();
					LogF(L"Not enough memory to create filter %s", key.c_str());
				}
				catch (const ConfigurationFileLoadError&)
				{
					throw;
				}
				catch (const exception& e)
				{
					LogF(L"%S", e.what());
				}

				if (key == L"")
					break;
				if (!newFilters.empty())
				{
					addFilters(newFilters);
					break;
				}
			}
			if (key == L"ProcessingPrecision")
			{
				const wstring precision = StringHelper::trim(value);
				if (precisionDirectiveSeen || (precision != L"32" && precision != L"64"))
				{
					LogF(L"ProcessingPrecision must occur once and contain 32 or 64");
					throw ConfigurationFileLoadError();
				}
				precisionDirectiveSeen = true;
				loadingSinglePrecision = precision == L"32";
				key.clear();
			}
			// Device/If/Stage and the other control factories must see the line
			// first: an inactive scope clears key and is safe to ignore. If an
			// unsafe command remains after every callback-safe factory had a
			// chance to consume it, it is active and the whole new configuration
			// is rejected instead of being partially applied.
			if (processingPolicy == ProcessingPolicy::AsioCallbackSafe &&
				isKnownCallbackUnsafeCommand(key))
			{
				unsafeConfigurationRejected.store(true, std::memory_order_release);
				LogF(L"ASIO callback-safe policy rejected filter command %s", key.c_str());
				throw ConfigurationFileLoadError();
			}
		}
	}

	for (vector<IFilterFactory*>::const_iterator it = factories.cbegin(); it != factories.cend(); it++)
	{
		IFilterFactory* factory = *it;
		if (!isFactoryAllowed(factory))
			continue;
		vector<IFilter*> newFilters = factory->endOfFile(path);
		if (!newFilters.empty())
			addFilters(newFilters);
	}

	// restore channels selected in outer configuration file
	currentChannelNames = savedChannelNames;
}

void FilterEngine::watchRegistryKey(const std::wstring& key)
{
	watchRegistryKeys.insert(key);
}

#pragma AVRT_CODE_BEGIN
void convertFloatToDouble(double* dest, const float* src, size_t count) {
#if defined(__AVX512F__) && !defined(_M_ARM64) // AVX-512 Path (e.g., Zen 4, some Intel CPUs)
	size_t i = 0;
	for (; i + 16 <= count; i += 16) {
		// Load 16 floats
		__m512 float_vec = _mm512_loadu_ps(src + i);
		// Convert the lower 8 floats to 8 doubles
		__m512d double_vec_lo = _mm512_cvtps_pd(_mm512_extractf32x8_ps(float_vec, 0));
		// Convert the upper 8 floats to 8 doubles
		__m512d double_vec_hi = _mm512_cvtps_pd(_mm512_extractf32x8_ps(float_vec, 1));
		// Store the 16 resulting doubles
		_mm512_storeu_pd(dest + i, double_vec_lo);
		_mm512_storeu_pd(dest + i + 8, double_vec_hi);
	}
	// Handle any remaining elements
	for (; i < count; ++i) dest[i] = static_cast<double>(src[i]);
#elif defined(__AVX2__) && !defined(_M_ARM64) // AVX2 / AVX Fallback Path (e.g., Zen 2/3)
	size_t i = 0;
	for (; i + 8 <= count; i += 8) {
		// Load 8 floats into a 256-bit register
		__m256 float_vec = _mm256_loadu_ps(src + i);
		// Convert the lower 4 floats to 4 doubles
		__m256d double_vec_lo = _mm256_cvtps_pd(_mm256_extractf128_ps(float_vec, 0));
		// Convert the upper 4 floats to 4 doubles
		__m256d double_vec_hi = _mm256_cvtps_pd(_mm256_extractf128_ps(float_vec, 1));
		// Store the 8 resulting doubles
		_mm256_storeu_pd(dest + i, double_vec_lo);
		_mm256_storeu_pd(dest + i + 4, double_vec_hi);
	}
	// Handle any remaining elements
	for (; i < count; ++i) dest[i] = static_cast<double>(src[i]);
#else // Scalar fallback for non-x86 or very old CPUs
	for (size_t i = 0; i < count; ++i) dest[i] = static_cast<double>(src[i]);
#endif
}

// Converts a block of doubles back to floats.
void convertDoubleToFloat(float* dest, const double* src, size_t count) {
#if defined(__AVX512F__) && !defined(_M_ARM64) // AVX-512 Path
	size_t i = 0;
	for (; i + 16 <= count; i += 16) {
		// Load 16 doubles from memory
		__m512d double_vec_lo = _mm512_loadu_pd(src + i);
		__m512d double_vec_hi = _mm512_loadu_pd(src + i + 8);
		// Convert 8 doubles to 8 floats
		__m256 float_vec_lo = _mm512_cvtpd_ps(double_vec_lo);
		// Convert another 8 doubles to 8 floats
		__m256 float_vec_hi = _mm512_cvtpd_ps(double_vec_hi);
		// Combine the two 256-bit float vectors into one 512-bit vector
		__m512 float_vec = _mm512_insertf32x8(_mm512_castps256_ps512(float_vec_lo), float_vec_hi, 1);
		_mm512_storeu_ps(dest + i, float_vec);
	}
	for (; i < count; ++i) dest[i] = static_cast<float>(src[i]);
#elif defined(__AVX2__) && !defined(_M_ARM64) // AVX2 / AVX Fallback Path
	size_t i = 0;
	for (; i + 8 <= count; i += 8) {
		// Load 8 doubles from memory
		__m256d double_vec_lo = _mm256_loadu_pd(src + i);
		__m256d double_vec_hi = _mm256_loadu_pd(src + i + 4);
		// Convert 4 doubles to 4 floats
		__m128 float_vec_lo = _mm256_cvtpd_ps(double_vec_lo);
		// Convert another 4 doubles to 4 floats
		__m128 float_vec_hi = _mm256_cvtpd_ps(double_vec_hi);
		// Combine the two 128-bit float vectors into one 256-bit vector
		__m256 float_vec = _mm256_insertf128_ps(_mm256_castps128_ps256(float_vec_lo), float_vec_hi, 1);
		_mm256_storeu_ps(dest + i, float_vec);
	}
	for (; i < count; ++i) dest[i] = static_cast<float>(src[i]);
#else // Scalar fallback
	for (size_t i = 0; i < count; ++i) dest[i] = static_cast<float>(src[i]);
#endif
}

void FilterEngine::commitCompletedTransition(
	FilterConfiguration* pending) noexcept
{
	if (pending == nullptr || transitionCounter < transitionLength)
		return;

	// Only the audio thread removes a published pending configuration. The
	// compare/exchange also makes a stale snapshot harmless if the lifecycle
	// owner has already stopped processing and begun teardown.
	FilterConfiguration* expected = pending;
	if (!pendingConfig.compare_exchange_strong(
		expected, nullptr,
		std::memory_order_acq_rel,
		std::memory_order_acquire))
	{
		return;
	}

	FilterConfiguration* const retired = currentConfig.exchange(
		pending, std::memory_order_acq_rel);
	transitionCounter = 0;
	// Publishing the retired owner is the complete realtime-side handoff. The
	// notification thread observes this atomic slot and performs all waiting,
	// destruction, and Win32 signalling away from the audio callback.
	retiredConfig.store(retired, std::memory_order_release);
}


// Process interleaved audio (float*)
void FilterEngine::process(float* output, float* input, unsigned frameCount)
{
	if (frameCount > maxFrameCount || frameCount > allocatedFrameCount)
	{
		bypassInterleaved(
			output, input, inputChannelCount, outputChannelCount, frameCount);
		return;
	}

	FilterConfiguration* const active = currentConfig.load(
		std::memory_order_acquire);
	FilterConfiguration* const pending =
		pendingConfig.load(std::memory_order_acquire);
	if (active == nullptr || (active->isEmpty() && pending == nullptr))
	{
		bypassInterleaved(
			output, input, inputChannelCount, outputChannelCount, frameCount);
		return;
	}

	// Conversion from float to double using SIMD
	size_t inputSampleCount = 0;
	size_t outputSampleCount = 0;
	if (!getSafeSampleCount<double>(
			inputChannelCount, frameCount, inputSampleCount) ||
		!getSafeSampleCount<double>(
			outputChannelCount, frameCount, outputSampleCount))
	{
		bypassInterleaved(
			output, input, inputChannelCount, outputChannelCount, frameCount);
		return;
	}
	convertFloatToDouble(inputBuf1D.data(), input, inputSampleCount);

	// The core processing logic remains unchanged
	active->read(inputBuf1D.data(), frameCount);
	active->process(frameCount);

	if (pending != nullptr)
	{
		pending->read(inputBuf1D.data(), frameCount);
		pending->process(frameCount);
		transitionCounter = active->doTransition(
			pending, frameCount, transitionCounter, transitionLength);
	}

	active->write(outputBuf1D.data(), frameCount);

	// Conversion from double back to float using SIMD
	convertDoubleToFloat(output, outputBuf1D.data(), outputSampleCount);

	commitCompletedTransition(pending);
}

// Process non-interleaved audio (float**)
void FilterEngine::process(float** output, float** input, unsigned frameCount)
{
	if (frameCount > maxFrameCount || frameCount > allocatedFrameCount)
	{
		bypassPlanar(
			output, input, inputChannelCount, outputChannelCount, frameCount);
		return;
	}

	FilterConfiguration* const active = currentConfig.load(
		std::memory_order_acquire);
	FilterConfiguration* const pending =
		pendingConfig.load(std::memory_order_acquire);
	if (active == nullptr || (active->isEmpty() && pending == nullptr))
	{
		bypassPlanar(
			output, input, inputChannelCount, outputChannelCount, frameCount);
		return;
	}

	// Optimized conversion for each channel
	for (unsigned c = 0; c < inputChannelCount; c++) {
		convertFloatToDouble(inputBuf2D[c].get(), input[c], frameCount);
	}

	// Core processing logic is the same
	active->read(inputBufPointers.data(), frameCount);
	active->process(frameCount);

	if (pending != nullptr)
	{
		pending->read(inputBufPointers.data(), frameCount);
		pending->process(frameCount);
		transitionCounter = active->doTransition(
			pending, frameCount, transitionCounter, transitionLength);
	}

	active->write(outputBufPointers.data(), frameCount);

	// Optimized conversion back for each channel
	for (unsigned c = 0; c < outputChannelCount; c++) {
		convertDoubleToFloat(output[c], outputBuf2D[c].get(), frameCount);
	}

	commitCompletedTransition(pending);
}

// Process interleaved audio (double*) - native double precision without conversion
void FilterEngine::process(double* output, double* input, unsigned frameCount)
{
	if (frameCount > maxFrameCount)
	{
		bypassInterleaved(
			output, input, inputChannelCount, outputChannelCount, frameCount);
		return;
	}

	FilterConfiguration* const active = currentConfig.load(
		std::memory_order_acquire);
	FilterConfiguration* const pending =
		pendingConfig.load(std::memory_order_acquire);
	if (active == nullptr || (active->isEmpty() && pending == nullptr))
	{
		bypassInterleaved(
			output, input, inputChannelCount, outputChannelCount, frameCount);
		return;
	}

	// Direct double-precision processing - no float conversion needed!
	active->read(input, frameCount);
	active->process(frameCount);

	if (pending != nullptr)
	{
		pending->read(input, frameCount);
		pending->process(frameCount);
		transitionCounter = active->doTransition(
			pending, frameCount, transitionCounter, transitionLength);
	}

	active->write(output, frameCount);

	commitCompletedTransition(pending);
}

// Process non-interleaved audio (double**) - native double precision without conversion
void FilterEngine::process(double** output, double** input, unsigned frameCount)
{
	if (frameCount > maxFrameCount)
	{
		bypassPlanar(
			output, input, inputChannelCount, outputChannelCount, frameCount);
		return;
	}

	FilterConfiguration* const active = currentConfig.load(
		std::memory_order_acquire);
	FilterConfiguration* const pending =
		pendingConfig.load(std::memory_order_acquire);
	if (active == nullptr || (active->isEmpty() && pending == nullptr))
	{
		bypassPlanar(
			output, input, inputChannelCount, outputChannelCount, frameCount);
		return;
	}

	// Direct double-precision processing - no float conversion needed!
	active->read(input, frameCount);
	active->process(frameCount);

	if (pending != nullptr)
	{
		pending->read(input, frameCount);
		pending->process(frameCount);
		transitionCounter = active->doTransition(
			pending, frameCount, transitionCounter, transitionLength);
	}

	active->write(output, frameCount);

	commitCompletedTransition(pending);
}
#pragma AVRT_CODE_END

void FilterEngine::addFilters(vector<IFilter*>& filters)
{
	for (vector<IFilter*>::iterator it = filters.begin(); it != filters.end(); it++)
	{
		IFilter* filter = *it;
		FilterInfo* filterInfo = NULL;
		vector<wstring> savedChannelNames;
		vector<wstring> savedLastChannelNames;
		vector<wstring> savedLastNewChannelNames;
		vector<wstring> savedAllChannelNames;
		bool savedLastInPlace = lastInPlace;
		bool stateSnapshotReady = false;
		auto discardCurrentFilter = [&]() noexcept
		{
			if (filterInfo != NULL)
			{
				MemoryHelper::free(filterInfo->inChannels);
				MemoryHelper::free(filterInfo->outChannels);
				MemoryHelper::free(filterInfo);
			}
			if (filter != NULL)
			{
				filter->~IFilter();
				MemoryHelper::free(filter);
			}
		};
		auto restoreBuildState = [&]() noexcept
		{
			if (stateSnapshotReady)
			{
				currentChannelNames.swap(savedChannelNames);
				lastChannelNames.swap(savedLastChannelNames);
				lastNewChannelNames.swap(savedLastNewChannelNames);
				allChannelNames.swap(savedAllChannelNames);
				lastInPlace = savedLastInPlace;
			}
		};

		try
		{
			FilterRuntimeContext runtimeContext;
			runtimeContext.flowKnown = deviceInfoKnown;
			runtimeContext.isCapture = capture;
			runtimeContext.offlineAnalysis = offlineAnalysis;
			if (!deviceGuid.empty())
				runtimeContext.endpointId = deviceGuid;
			if (offlineAnalysis)
				runtimeContext.volumeObservations = &runtimeVolumeObservations;
			filter->setRuntimeContext(runtimeContext);

			filterInfo = static_cast<FilterInfo*>(
				MemoryHelper::alloc(sizeof(FilterInfo)));
			if (filterInfo == NULL)
			{
				discardCurrentFilter();
				filter = NULL;
				continue;
			}
			filterInfo->filter = filter;
			filterInfo->inPlace = true;
			filterInfo->inChannels = NULL;
			filterInfo->inChannelCount = 0;
			filterInfo->outChannels = NULL;
			filterInfo->outChannelCount = 0;
			filterInfo->inPlace = filter->getInPlace();

			savedChannelNames = currentChannelNames;
			savedLastChannelNames = lastChannelNames;
			savedLastNewChannelNames = lastNewChannelNames;
			savedAllChannelNames = allChannelNames;
			savedLastInPlace = lastInPlace;
			stateSnapshotReady = true;

			if (filter->getAllChannels())
				currentChannelNames = allChannelNames;

			if (lastChannelNames != currentChannelNames)
			{
				filterInfo->inChannelCount = currentChannelNames.size();
				filterInfo->inChannels = static_cast<size_t*>(
					MemoryHelper::allocArray(
						filterInfo->inChannelCount, sizeof(size_t)));
				if (filterInfo->inChannels == NULL)
				{
					restoreBuildState();
					discardCurrentFilter();
					filter = NULL;
					continue;
				}

				size_t c = 0;
				for (vector<wstring>::iterator it2 = currentChannelNames.begin(); it2 != currentChannelNames.end(); it2++)
				{
					vector<wstring>::iterator pos = find(allChannelNames.begin(), allChannelNames.end(), *it2);
					filterInfo->inChannels[c++] = pos - allChannelNames.begin();
				}
			}

			lastChannelNames = currentChannelNames;
			vector<wstring> newChannelNames =
				filter->initialize(sampleRate, maxFrameCount, currentChannelNames);

			if (!(filterInfo->inPlace && lastInPlace &&
				lastNewChannelNames == newChannelNames))
			{
				filterInfo->outChannelCount = newChannelNames.size();
				filterInfo->outChannels = static_cast<size_t*>(
					MemoryHelper::allocArray(
						filterInfo->outChannelCount, sizeof(size_t)));
				if (filterInfo->outChannels == NULL)
				{
					restoreBuildState();
					discardCurrentFilter();
					filter = NULL;
					continue;
				}

				size_t c = 0;
				for (vector<wstring>::iterator it2 = newChannelNames.begin(); it2 != newChannelNames.end(); it2++)
				{
					vector<wstring>::iterator pos = find(allChannelNames.begin(), allChannelNames.end(), *it2);
					if (pos == allChannelNames.end())
					{
						filterInfo->outChannels[c++] = allChannelNames.size();
						allChannelNames.push_back(*it2);
					}
					else
						filterInfo->outChannels[c++] = pos - allChannelNames.begin();
				}
			}

			lastNewChannelNames = newChannelNames;
			lastInPlace = filterInfo->inPlace;
			if (!lastInPlace)
				swap(lastChannelNames, lastNewChannelNames);
			if (filter->getSelectChannels())
				currentChannelNames = newChannelNames;
			else
				currentChannelNames = savedChannelNames;

			filterInfos.push_back(filterInfo);
			filterInfo = NULL;
			filter = NULL;
		}
		catch (const std::bad_alloc&)
		{
			MemoryHelper::markAllocationFailure();
			restoreBuildState();
			discardCurrentFilter();
			for (++it; it != filters.end(); ++it)
			{
				(*it)->~IFilter();
				MemoryHelper::free(*it);
			}
			return;
		}
		catch (...)
		{
			restoreBuildState();
			discardCurrentFilter();
			for (++it; it != filters.end(); ++it)
			{
				(*it)->~IFilter();
				MemoryHelper::free(*it);
			}
			throw;
		}
	}
}

void FilterEngine::destroyConfiguration(
	FilterConfiguration* configuration) noexcept
{
	if (configuration == nullptr)
		return;

	configuration->~FilterConfiguration();
	MemoryHelper::free(configuration);
}

void FilterEngine::reclaimRetiredConfiguration() noexcept
{
	FilterConfiguration* const retired = retiredConfig.exchange(
		nullptr, std::memory_order_acq_rel);
	destroyConfiguration(retired);
}

void FilterEngine::stopNotificationThread() noexcept
{
	// The lifecycle owner calls this only while audio callbacks are quiescent.
	// Signalling first also interrupts either notification-thread wait, including
	// a reload whose transition never received another audio block.
	if (threadHandle != NULL)
	{
		if (shutdownEvent != NULL)
			SetEvent(shutdownEvent);
		if (WaitForSingleObject(threadHandle, INFINITE) == WAIT_OBJECT_0)
			TraceF(L"Successfully terminated directory change notification thread");
		CloseHandle(threadHandle);
		threadHandle = NULL;
	}
	if (shutdownEvent != NULL)
	{
		CloseHandle(shutdownEvent);
		shutdownEvent = NULL;
	}
}

void FilterEngine::cleanupConfigurations() noexcept
{
	// Processing has stopped before this lifecycle cleanup begins. Clear each
	// ownership slot first, then destroy only distinct objects so a partially
	// completed handoff cannot cause a double free.
	FilterConfiguration* const active = currentConfig.exchange(
		nullptr, std::memory_order_acq_rel);
	FilterConfiguration* const pending = pendingConfig.exchange(
		nullptr, std::memory_order_acq_rel);
	FilterConfiguration* const retired = retiredConfig.exchange(
		nullptr, std::memory_order_acq_rel);
	hasInitialConfiguration = false;

	destroyConfiguration(active);
	if (pending != active)
		destroyConfiguration(pending);
	if (retired != active && retired != pending)
		destroyConfiguration(retired);
}

unsigned long __stdcall FilterEngine::notificationThread(void* parameter)
{
	FilterEngine* engine = (FilterEngine*)parameter;
	if (engine == NULL || engine->shutdownEvent == NULL || engine->loadSemaphore == NULL)
		return ERROR_INVALID_HANDLE;

	bool reloadTokenHeld = false;
	try
	{
		SCOPE_EXIT
		{
			if (reloadTokenHeld)
				ReleaseSemaphore(engine->loadSemaphore, 1, NULL);
		};

		HANDLE notificationHandle = FindFirstChangeNotificationW(engine->configPath.c_str(), true, FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE);
		if (notificationHandle == INVALID_HANDLE_VALUE)
		{
			DWORD error = GetLastError();
			LogFStatic(L"Could not watch the configuration directory %s: %s",
				engine->configPath.c_str(), StringHelper::getSystemErrorString(error).c_str());
			return error;
		}
		SCOPE_EXIT
		{
			FindCloseChangeNotification(notificationHandle);
		};

		HANDLE registryEvent = CreateEventW(NULL, true, false, NULL);
		if (registryEvent == NULL)
		{
			DWORD error = GetLastError();
			LogFStatic(L"Could not create the registry notification event: %s",
				StringHelper::getSystemErrorString(error).c_str());
			return error;
		}
		SCOPE_EXIT
		{
			CloseHandle(registryEvent);
		};

		HANDLE handles[3] = {engine->shutdownEvent, notificationHandle, registryEvent};
		while (true)
		{
			vector<HKEY> keyHandles;
			SCOPE_EXIT
			{
				for (auto it = keyHandles.begin(); it != keyHandles.end(); ++it)
					RegCloseKey(*it);
			};
			for (auto it = engine->watchRegistryKeys.begin(); it != engine->watchRegistryKeys.end(); it++)
			{
				try
				{
					HKEY keyHandle = RegistryHelper::openKey(*it, KEY_NOTIFY | KEY_WOW64_64KEY);
					try
					{
						keyHandles.push_back(keyHandle);
					}
					catch (...)
					{
						RegCloseKey(keyHandle);
						throw;
					}
					RegNotifyChangeKeyValue(keyHandle, false, REG_NOTIFY_CHANGE_LAST_SET, registryEvent, true);
				}
				catch (RegistryException& e)
				{
					LogFStatic(L"%s", e.getMessage().c_str());
				}
			}

			DWORD which = WaitForMultipleObjects(3, handles, false, INFINITE);

		if (which == WAIT_FAILED)
		{
			LogFStatic(L"Configuration notification wait failed: %s",
				StringHelper::getSystemErrorString(GetLastError()).c_str());
			break;
		}
		if (which == WAIT_OBJECT_0)
		{
			// Shutdown
			break;
		}
		else if (which == WAIT_OBJECT_0 + 1 || which == WAIT_OBJECT_0 + 2)
		{
			if (which == WAIT_OBJECT_0 + 1)
			{
				if (!FindNextChangeNotification(notificationHandle))
				{
					LogFStatic(L"Could not rearm the configuration directory notification: %s",
						StringHelper::getSystemErrorString(GetLastError()).c_str());
					break;
				}
				// Wait for second event within 10 milliseconds to avoid loading twice
				if (WaitForSingleObject(notificationHandle, 10) == WAIT_OBJECT_0
					&& !FindNextChangeNotification(notificationHandle))
				{
					LogFStatic(L"Could not rearm the configuration directory notification: %s",
						StringHelper::getSystemErrorString(GetLastError()).c_str());
					break;
				}
			}

			HANDLE loadHandles[2] = {engine->shutdownEvent, engine->loadSemaphore};
			DWORD loadWait = WaitForMultipleObjects(2, loadHandles, false, INFINITE);
			if (loadWait == WAIT_OBJECT_0)
			{
				// Shutdown
				break;
			}
			if (loadWait != WAIT_OBJECT_0 + 1)
			{
				LogFStatic(L"Configuration reload wait failed: %s",
					StringHelper::getSystemErrorString(GetLastError()).c_str());
				break;
			}
			reloadTokenHeld = true;

			const bool waitForRetirement = engine->loadConfig();
			ResetEvent(registryEvent);
			if (!waitForRetirement)
			{
				if (!ReleaseSemaphore(engine->loadSemaphore, 1, NULL))
				{
					LogFStatic(L"Could not restore the configuration reload token after a rejected reload: %s",
						StringHelper::getSystemErrorString(GetLastError()).c_str());
					break;
				}
				reloadTokenHeld = false;
				continue;
			}

			// The audio thread completes its work by publishing the old active
			// configuration in the lock-free retired slot. Poll that slot only
			// from this non-realtime thread, using the shutdown event for a
			// prompt and lifecycle-safe exit when processing stops.
			while (engine->retiredConfig.load(
				std::memory_order_acquire) == nullptr)
			{
				DWORD retirementWait = WaitForSingleObject(
					engine->shutdownEvent, 1);
				if (retirementWait == WAIT_OBJECT_0)
				{
					// cleanupConfigurations owns pending/retired state after join.
					return 0;
				}
				if (retirementWait != WAIT_TIMEOUT)
				{
					DWORD error = GetLastError();
					LogFStatic(L"Configuration retirement wait failed: %s",
						StringHelper::getSystemErrorString(error).c_str());
					return error;
				}
			}

			engine->reclaimRetiredConfiguration();
			if (!ReleaseSemaphore(engine->loadSemaphore, 1, NULL))
			{
				LogFStatic(L"Could not restore the configuration reload token: %s",
					StringHelper::getSystemErrorString(GetLastError()).c_str());
				break;
			}
			reloadTokenHeld = false;
		}
		else
		{
			LogFStatic(L"Configuration notification wait returned an unexpected result: %lu", which);
			break;
		}
		}

		return 0;
	}
	catch (const std::bad_alloc&)
	{
		// Never allow a C++ allocation failure to escape a Win32 thread entry.
		return ERROR_NOT_ENOUGH_MEMORY;
	}
	catch (...)
	{
		return ERROR_UNHANDLED_EXCEPTION;
	}
}
