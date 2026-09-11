#pragma once

#include <string>
#include <vector>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "IFilter.h"
#include "VUMeterProtocol.h"

class VUMeterFilter : public IFilter
{
public:
	VUMeterFilter(std::wstring meterId, std::wstring channelSelector, std::wstring rmsStandard, std::wstring lufsStandard);
	~VUMeterFilter() override;
	bool getAllChannels() override { return true; }
	std::vector<std::wstring> initialize(float sampleRate, unsigned maxFrameCount, std::vector<std::wstring> channelNames) override;
	void process(double** output, double** input, unsigned frameCount) override;
	bool processSingle(float** output, float** input, unsigned frameCount) override;

private:
	void cleanup();
	void openSharedData();
	static DWORD WINAPI ownerThreadProc(LPVOID parameter);
	DWORD runOwnerThread();
	bool beginSharedWrite();
	void endSharedWrite();
	void resetMeasurements();
	template <typename SampleType>
	void processSamples(SampleType** output, SampleType** input, unsigned frameCount);
	std::wstring objectName() const;
	std::wstring ownerMutexName() const;

	std::wstring meterId;
	std::wstring channelSelector;
	std::wstring rmsStandard;
	std::wstring lufsStandard;
	double rmsScale = 1.0;
	std::vector<unsigned> channels;
	float sampleRate = 48000.0f;
	unsigned totalChannelCount = 0;
	unsigned measuredChannelCount = 0;
	HANDLE mapping = NULL;
	VUMeterSharedData* shared = NULL;
	HANDLE ownerMutex = NULL;
	HANDLE ownerStopEvent = NULL;
	HANDLE ownerThread = NULL;
	volatile LONG ownerActive = 0;
	volatile LONG activeUsers = 0;
	double momentaryMean = 0.0;
	double shortMean = 0.0;
	double integratedMean = 0.0;
	double integratedWeight = 0.0;
	double channelMomentaryMean[VUMETER_MAX_CHANNELS] = {};
	double channelShortMean[VUMETER_MAX_CHANNELS] = {};
	double channelIntegratedMean[VUMETER_MAX_CHANNELS] = {};
	double channelIntegratedWeight[VUMETER_MAX_CHANNELS] = {};
};
