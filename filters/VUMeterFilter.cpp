#include "stdafx.h"
#include <algorithm>
#include <cmath>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <sddl.h>
#include "helpers/StringHelper.h"
#include "helpers/LogHelper.h"
#include "VUMeterFilter.h"
#include "AudioToolsHelper.h"

using namespace std;

namespace
{
volatile LONG64* sequenceAddress(VUMeterSharedData* data)
{
	return reinterpret_cast<volatile LONG64*>(&data->sequence);
}

volatile LONG* resetRequestAddress(VUMeterSharedData* data)
{
	return reinterpret_cast<volatile LONG*>(&data->resetRequest);
}
}

VUMeterFilter::VUMeterFilter(wstring meterId, wstring channelSelector, wstring rmsStandard, wstring lufsStandard)
	: meterId(VUMeterNormalizedId(meterId)),
	  channelSelector(VUMeterTrimToken(channelSelector)),
	  rmsStandard(VUMeterTrimToken(rmsStandard)),
	  lufsStandard(VUMeterTrimToken(lufsStandard))
{
	if (this->channelSelector.empty())
		this->channelSelector = L"all";
	if (this->rmsStandard.empty())
		this->rmsStandard = L"AES17";
	if (this->lufsStandard.empty())
		this->lufsStandard = L"ITU-R BS.1770-5";
}

VUMeterFilter::~VUMeterFilter()
{
	cleanup();
}

vector<wstring> VUMeterFilter::initialize(float sampleRate, unsigned maxFrameCount, vector<wstring> channelNames)
{
	cleanup();
	this->sampleRate = sampleRate > 0.0f ? sampleRate : 48000.0f;
	totalChannelCount = static_cast<unsigned>(channelNames.size());
	measuredChannelCount = min<unsigned>(totalChannelCount, VUMETER_MAX_CHANNELS);
	channels = AudioTools::resolveChannels(channelSelector, channelNames);
	const wstring rmsLower = StringHelper::toLowerCase(rmsStandard);
	rmsScale = rmsLower.find(L"aes17") != wstring::npos ? sqrt(2.0) : 1.0;
	resetMeasurements();
	openSharedData();
	return channelNames;
}

wstring VUMeterFilter::objectName() const
{
	return L"Global\\EqAPO_VUMeter_v3_" + VUMeterCanonicalId(meterId);
}

wstring VUMeterFilter::ownerMutexName() const
{
	return objectName() + L"_Owner";
}

void VUMeterFilter::openSharedData()
{
	PSECURITY_DESCRIPTOR sd = NULL;
	SECURITY_ATTRIBUTES sa = {};
	sa.nLength = sizeof(sa);
	if (ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(A;;GA;;;WD)", SDDL_REVISION_1, &sd, NULL))
		sa.lpSecurityDescriptor = sd;

	ownerMutex = CreateMutexW(sd ? &sa : NULL, FALSE, ownerMutexName().c_str());
	mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, sd ? &sa : NULL, PAGE_READWRITE, 0, sizeof(VUMeterSharedData), objectName().c_str());
	if (sd)
		LocalFree(sd);
	if (ownerMutex == NULL || mapping == NULL)
	{
		LogF(L"VUMeter: could not create shared ownership objects for meter %s", meterId.c_str());
		cleanup();
		return;
	}
	shared = static_cast<VUMeterSharedData*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(VUMeterSharedData)));
	if (shared == NULL)
	{
		LogF(L"VUMeter: could not map shared memory for meter %s", meterId.c_str());
		cleanup();
		return;
	}
	// Page-file-backed mappings start zeroed. Never clear an attached mapping
	// here: another generation may already be publishing to the same view.

	ownerStopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
	if (ownerStopEvent == NULL)
	{
		LogF(L"VUMeter: could not create owner stop event for meter %s", meterId.c_str());
		cleanup();
		return;
	}
	ownerThread = CreateThread(NULL, 0, ownerThreadProc, this, 0, NULL);
	if (ownerThread == NULL)
	{
		LogF(L"VUMeter: could not create owner thread for meter %s", meterId.c_str());
		cleanup();
	}
}

void VUMeterFilter::cleanup()
{
	if (ownerStopEvent != NULL)
		SetEvent(ownerStopEvent);
	if (ownerThread != NULL)
	{
		WaitForSingleObject(ownerThread, INFINITE);
		CloseHandle(ownerThread);
		ownerThread = NULL;
	}
	InterlockedExchange(&ownerActive, 0);
	if (shared != NULL)
	{
		UnmapViewOfFile(shared);
		shared = NULL;
	}
	if (mapping != NULL)
	{
		CloseHandle(mapping);
		mapping = NULL;
	}
	if (ownerStopEvent != NULL)
	{
		CloseHandle(ownerStopEvent);
		ownerStopEvent = NULL;
	}
	if (ownerMutex != NULL)
	{
		CloseHandle(ownerMutex);
		ownerMutex = NULL;
	}
}

DWORD WINAPI VUMeterFilter::ownerThreadProc(LPVOID parameter)
{
	return static_cast<VUMeterFilter*>(parameter)->runOwnerThread();
}

DWORD VUMeterFilter::runOwnerThread()
{
	HANDLE waits[] = {ownerStopEvent, ownerMutex};
	const DWORD waitResult = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
	const bool acquired = waitResult == WAIT_OBJECT_0 + 1 || waitResult == WAIT_ABANDONED_0 + 1;
	if (!acquired)
		return 0;
	if (WaitForSingleObject(ownerStopEvent, 0) == WAIT_OBJECT_0)
	{
		ReleaseMutex(ownerMutex);
		return 0;
	}

	// The mutex proves that the previous writer is gone. A process crash may
	// leave the shared seqlock odd while the Editor keeps the mapping alive.
	volatile LONG64* sequence = sequenceAddress(shared);
	const LONG64 abandonedSequence = InterlockedCompareExchange64(sequence, 0, 0);
	InterlockedExchange64(sequence, abandonedSequence | 1);
	shared->magic = VUMETER_MAGIC;
	shared->version = VUMETER_VERSION;
	shared->channelCount = measuredChannelCount;
	shared->sampleRate = static_cast<std::uint32_t>(sampleRate);
	InterlockedExchange(resetRequestAddress(shared), 0);
	resetMeasurements();
	InterlockedIncrement64(sequence);
	InterlockedExchange(&ownerActive, 1);

	WaitForSingleObject(ownerStopEvent, INFINITE);
	InterlockedExchange(&ownerActive, 0);
	while (InterlockedCompareExchange(&activeUsers, 0, 0) != 0)
		SwitchToThread();
	ReleaseMutex(ownerMutex);
	return 0;
}

bool VUMeterFilter::beginSharedWrite()
{
	if (shared == NULL)
		return false;
	InterlockedIncrement(&activeUsers);
	if (InterlockedCompareExchange(&ownerActive, 0, 0) == 0)
	{
		InterlockedDecrement(&activeUsers);
		return false;
	}

	volatile LONG64* sequence = sequenceAddress(shared);
	const LONG64 observed = InterlockedCompareExchange64(sequence, 0, 0);
	if ((observed & 1) != 0
		|| InterlockedCompareExchange64(sequence, observed + 1, observed) != observed)
	{
		InterlockedDecrement(&activeUsers);
		return false;
	}

	if (InterlockedCompareExchange(&ownerActive, 0, 0) == 0)
	{
		InterlockedIncrement64(sequence);
		InterlockedDecrement(&activeUsers);
		return false;
	}
	return true;
}

void VUMeterFilter::endSharedWrite()
{
	InterlockedIncrement64(sequenceAddress(shared));
	InterlockedDecrement(&activeUsers);
}

void VUMeterFilter::resetMeasurements()
{
	momentaryMean = 0.0;
	shortMean = 0.0;
	integratedMean = 0.0;
	integratedWeight = 0.0;
	for (unsigned c = 0; c < VUMETER_MAX_CHANNELS; ++c)
	{
		channelMomentaryMean[c] = 0.0;
		channelShortMean[c] = 0.0;
		channelIntegratedMean[c] = 0.0;
		channelIntegratedWeight[c] = 0.0;
		if (shared != NULL)
		{
			shared->peak[c] = 0.0;
			shared->peakHold[c] = 0.0;
			shared->rms[c] = 0.0;
			shared->channelLufsMomentary[c] = -INFINITY;
			shared->channelLufsShortTerm[c] = -INFINITY;
			shared->channelLufsIntegrated[c] = -INFINITY;
			shared->clip[c] = 0;
		}
	}
	if (shared != NULL)
	{
		shared->lufsMomentary = -INFINITY;
		shared->lufsShortTerm = -INFINITY;
		shared->lufsIntegrated = -INFINITY;
	}
}

#pragma AVRT_CODE_BEGIN
template <typename SampleType>
void VUMeterFilter::processSamples(SampleType** output, SampleType** input, unsigned frameCount)
{
	// Metering is protocol-limited, but the filter is transparent to every
	// channel exposed by the endpoint, including channels above that limit.
	for (unsigned c = 0; c < totalChannelCount; c++)
		if (output[c] != input[c])
			memcpy(output[c], input[c], frameCount * sizeof(SampleType));

	if (shared == NULL || frameCount == 0 || !beginSharedWrite())
		return;

	shared->magic = VUMETER_MAGIC;
	shared->version = VUMETER_VERSION;
	shared->channelCount = measuredChannelCount;
	shared->sampleRate = static_cast<std::uint32_t>(sampleRate);
	const bool resetRequested = InterlockedExchange(resetRequestAddress(shared), 0) != 0;
	if (resetRequested)
	{
		resetMeasurements();
	}

	double blockMean = 0.0;
	unsigned activeChannels = 0;
	bool measured[VUMETER_MAX_CHANNELS] = {};
	const double blockSeconds = frameCount / max(1.0f, sampleRate);
	const double momentaryAlpha = exp(-blockSeconds / 0.4);
	const double shortAlpha = exp(-blockSeconds / 3.0);
	auto toLufs = [](double mean) { return mean > 1e-12 ? 10.0 * log10(mean) - 0.691 : -INFINITY; };
	for (unsigned channel : channels)
	{
		if (channel >= measuredChannelCount)
			continue;
		measured[channel] = true;
		activeChannels++;
		double peak = 0.0;
		double sumSquares = 0.0;
		for (unsigned i = 0; i < frameCount; i++)
		{
			const double sample = static_cast<double>(input[channel][i]);
			const double absSample = fabs(sample);
			peak = max(peak, absSample);
			sumSquares += sample * sample;
		}
		const double mean = sumSquares / frameCount;
		blockMean += mean;
		shared->peak[channel] = peak;
		shared->rms[channel] = sqrt(mean) * rmsScale;
		shared->peakHold[channel] = max(shared->peakHold[channel] * 0.9995, peak);
		if (peak >= 1.0)
			shared->clip[channel]++;
		channelMomentaryMean[channel] = channelMomentaryMean[channel] * momentaryAlpha + mean * (1.0 - momentaryAlpha);
		channelShortMean[channel] = channelShortMean[channel] * shortAlpha + mean * (1.0 - shortAlpha);
		channelIntegratedMean[channel] = (channelIntegratedMean[channel] * channelIntegratedWeight[channel] + mean * blockSeconds) / max(1e-9, channelIntegratedWeight[channel] + blockSeconds);
		channelIntegratedWeight[channel] += blockSeconds;
		shared->channelLufsMomentary[channel] = toLufs(channelMomentaryMean[channel]);
		shared->channelLufsShortTerm[channel] = toLufs(channelShortMean[channel]);
		shared->channelLufsIntegrated[channel] = toLufs(channelIntegratedMean[channel]);
	}
	for (unsigned channel = 0; channel < VUMETER_MAX_CHANNELS; channel++)
	{
		if (channel >= measuredChannelCount || !measured[channel])
		{
			shared->peak[channel] = 0.0;
			shared->rms[channel] = 0.0;
			shared->peakHold[channel] = 0.0;
			channelMomentaryMean[channel] = 0.0;
			channelShortMean[channel] = 0.0;
			channelIntegratedMean[channel] = 0.0;
			channelIntegratedWeight[channel] = 0.0;
			shared->channelLufsMomentary[channel] = -INFINITY;
			shared->channelLufsShortTerm[channel] = -INFINITY;
			shared->channelLufsIntegrated[channel] = -INFINITY;
		}
	}

	if (activeChannels > 0)
		blockMean /= activeChannels;
	momentaryMean = momentaryMean * momentaryAlpha + blockMean * (1.0 - momentaryAlpha);
	shortMean = shortMean * shortAlpha + blockMean * (1.0 - shortAlpha);
	integratedMean = (integratedMean * integratedWeight + blockMean * blockSeconds) / max(1e-9, integratedWeight + blockSeconds);
	integratedWeight += blockSeconds;
	shared->lufsMomentary = toLufs(momentaryMean);
	shared->lufsShortTerm = toLufs(shortMean);
	shared->lufsIntegrated = toLufs(integratedMean);
	endSharedWrite();
}

void VUMeterFilter::process(double** output, double** input, unsigned frameCount)
{
	processSamples(output, input, frameCount);
}

bool VUMeterFilter::processSingle(float** output, float** input, unsigned frameCount)
{
	processSamples(output, input, frameCount);
	return true;
}
#pragma AVRT_CODE_END
