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

#pragma once

#include <atomic>
#include <string>
#include <vector>
#include <unordered_set>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "IFilterFactory.h"
#include "FilterConfiguration.h"
#include "helpers/PrecisionTimer.h"
#include "helpers/MemoryHelper.h"

namespace mup {
class ParserX;
}

struct LoadedConfigurationFile
{
	std::wstring path;
	std::string contents;
	bool readable = false;
};

#pragma AVRT_VTABLES_BEGIN
class FilterEngine
{
public:
	enum class ProcessingPolicy
	{
		Full,
		AsioCallbackSafe
	};

	FilterEngine();
	~FilterEngine();

	// Must be selected before initialize(). The ASIO policy admits only the
	// built-in, non-blocking filter allowlist and rejects the whole new
	// configuration if it contains a known out-of-process or plug-in command.
	void setProcessingPolicy(ProcessingPolicy policy) noexcept;
	void setPreMix(bool preMix);
	void setOfflineAnalysis(bool offlineAnalysis);
	void clearDeviceInfo();
	void setDeviceInfo(bool capture, bool postMixInstalled, const std::wstring& deviceName, const std::wstring& connectionName, const std::wstring& deviceGuid, const std::wstring& deviceString);
	void initialize(float sampleRate, unsigned inputChannelCount, unsigned realChannelCount, unsigned outputChannelCount, unsigned channelMask, unsigned maxFrameCount, const std::wstring& customPath = L"");
	bool loadConfig(const std::wstring& customPath = L"");
	void loadConfigFile(const std::wstring& path);
	void watchRegistryKey(const std::wstring& key);
	void process(float* output, float* input, unsigned frameCount);
	void process(float** output, float** input, unsigned frameCount);
	void process(double* output, double* input, unsigned frameCount);
	void process(double** output, double** input, unsigned frameCount);

	bool isPreMix() const {return preMix;}
	bool isAnalysisMode() const {return analysisMode;}
	bool isDeviceInfoKnown() const {return deviceInfoKnown;}
	bool isCapture() const {return capture;}
	bool isPostMixInstalled() const {return postMixInstalled;}
	std::wstring getDeviceName() const {return deviceName;}
	std::wstring getConnectionName() const {return connectionName;}
	std::wstring getDeviceGuid() const {return deviceGuid;}
	std::wstring getDeviceString() const {return deviceString;}
	unsigned getInputChannelCount() const {return inputChannelCount;}
	unsigned getRealChannelCount() const {return realChannelCount;}
	unsigned getOutputChannelCount() const {return outputChannelCount;}
	unsigned getChannelMask() const {return channelMask;}
	float getSampleRate() const {return sampleRate;}
	unsigned getMaxFrameCount() const {return maxFrameCount;}
	bool getLoadingSinglePrecision() const {return loadingSinglePrecision;}
	bool hasActiveConfiguration() const noexcept
	{
		return currentConfig.load(std::memory_order_acquire) != nullptr;
	}
	bool rejectedUnsafeConfiguration() const noexcept
	{
		return unsafeConfigurationRejected.load(std::memory_order_acquire);
	}
	mup::ParserX* getParser() {return parser;}
	const std::vector<LoadedConfigurationFile>& getLoadedConfigurationFiles() const {return loadedConfigurationFiles;}
	const std::vector<FilterRuntimeVolumeObservation>& getRuntimeVolumeObservations() const {return runtimeVolumeObservations;}

private:
	friend class FilterEngineTestAccess;

	void addFilters(std::vector<IFilter*>& filters);
	bool isFactoryAllowed(const IFilterFactory* factory) const noexcept;
	void commitCompletedTransition(FilterConfiguration* pending) noexcept;
	void cleanupConfigurations() noexcept;
	static void destroyConfiguration(FilterConfiguration* configuration) noexcept;
	void reclaimRetiredConfiguration() noexcept;
	void stopNotificationThread() noexcept;
	static unsigned long __stdcall notificationThread(void* parameter);
	void resizeBuffers(unsigned frameCount);

	std::vector<IFilterFactory*> factories;
	std::unordered_set<const IFilterFactory*> callbackUnsafeFactories;
	const IFilterFactory* asioManualLoudnessFactory;
	const IFilterFactory* asioOriginalLoudnessFactory;
	ProcessingPolicy processingPolicy;
	std::atomic<bool> unsafeConfigurationRejected;

	std::vector<std::unique_ptr<double[]>> inputBuf2D, outputBuf2D;
	std::vector<double*> inputBufPointers, outputBufPointers;
	std::vector<double> inputBuf1D, outputBuf1D;
	unsigned allocatedFrameCount;

	bool preMix;
	bool offlineAnalysis;
	bool analysisMode;
	bool deviceInfoKnown;
	bool capture;
	bool postMixInstalled;
	std::wstring deviceName;
	std::wstring connectionName;
	std::wstring deviceGuid;
	std::wstring deviceString;
	std::wstring configPath;
	float sampleRate;
	// number of input channels that originally existed before child APO processing
	unsigned inputChannelCount;
	// number of input channels in process (including channels coming from child APO output)
	unsigned realChannelCount;
	unsigned outputChannelCount;
	unsigned channelMask;
	unsigned maxFrameCount;

	// only used during loading
	bool loadingSinglePrecision = false;
	bool precisionDirectiveSeen = false;
	std::vector<FilterInfo*> filterInfos;
	std::vector<std::wstring> currentChannelNames;
	std::vector<std::wstring> lastChannelNames;
	std::vector<std::wstring> lastNewChannelNames;
	std::vector<std::wstring> allChannelNames;
	std::vector<LoadedConfigurationFile> loadedConfigurationFiles;
	std::vector<FilterRuntimeVolumeObservation> runtimeVolumeObservations;
	bool lastInPlace;
	mup::ParserX* parser;

	// The active configuration is published with release semantics. After the
	// first successful load, only the audio thread replaces it during handoff.
	std::atomic<FilterConfiguration*> currentConfig;
	std::atomic<FilterConfiguration*> pendingConfig;
	std::atomic<FilterConfiguration*> retiredConfig;
	bool hasInitialConfiguration;

	unsigned transitionCounter;
	unsigned transitionLength;
	HANDLE loadSemaphore;
	CRITICAL_SECTION loadSection;
	PrecisionTimer timer;
	void* threadHandle;
	void* shutdownEvent;
	std::unordered_set<std::wstring> watchRegistryKeys;
	bool lastInputWasSilent;
};
#pragma AVRT_VTABLES_END
