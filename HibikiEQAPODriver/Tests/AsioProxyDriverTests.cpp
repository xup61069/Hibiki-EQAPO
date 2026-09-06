#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "../AsioDsp.h"
#include "../AsioProxyDriver.h"
#include "../AsioProxyIdentity.h"
#include "../../FilterEngine.h"

extern "C" HRESULT __stdcall DllGetClassObject(
	REFCLSID clsid, REFIID iid, void** object);
extern "C" HRESULT __stdcall DllCanUnloadNow();

namespace
{
using HibikiAsio::AsioProxyDriver;

constexpr CLSID kFakeVendorClsid = {
	0x10203040, 0x5060, 0x7080, {0x90, 0xA0, 0xB0, 0xC0, 0xD0, 0xE0, 0xF0, 0x11}
};

enum class Event
{
	Negotiation,
	HostBegin,
	HostReadyReturned,
	HostEnd,
	Dsp,
	VendorReady,
	VendorStop
};

std::array<std::atomic<Event>, 64> events{};
std::atomic<std::size_t> eventCount{0};
int failures = 0;

void record(Event event)
{
	std::size_t index = eventCount.load(std::memory_order_relaxed);
	while (index < events.size() &&
		!eventCount.compare_exchange_weak(
			index, index + 1,
			std::memory_order_acq_rel, std::memory_order_relaxed))
	{
	}
	if (index < events.size())
		events[index].store(event, std::memory_order_release);
}

void check(bool condition, const char* message)
{
	if (!condition)
	{
		std::fprintf(stderr, "FAILED: %s\n", message);
		++failures;
	}
}

struct FakeConfig
{
	ASIOSampleType outputType = ASIOSTFloat32LSB;
	ASIOSampleRate sampleRate = 48000.0;
	ASIOError createResult = ASE_OK;
	ASIOError startResult = ASE_OK;
	ASIOError stopResult = ASE_OK;
	ASIOError disposeResult = ASE_OK;
	ASIOError controlPanelResult = ASE_OK;
	ASIOError futureResult = ASE_SUCCESS;
	ASIOError getChannelsResult = ASE_OK;
	ASIOError outputReadyResult = ASE_OK;
	ASIOIoFormatType ioFormatOnGet = kASIOPCMFormat;
	long internalInputSamples = 7;
	long internalOutputSamples = 11;
	long inputLatency = 17;
	long outputLatency = 23;
	long inputChannels = 4;
	long outputChannels = 8;
	long preferredBufferSize = 4;
	ASIOError getBufferSizeResult = ASE_OK;
	bool outputReadyDuringStop = false;
	bool callbackDuringStart = false;
	bool sampleRateCallbackDuringQuery = false;
	bool inlineCallbackDuringStart = false;
	ASIOBool callbackDuringStartDirectProcess = ASIOTrue;
	bool waitForConcurrentInit = false;
	bool reenterProxyInit = false;
	bool throwInit = false;
	bool throwStart = false;
	bool throwStop = false;
	bool throwCreate = false;
	bool throwDispose = false;
	bool throwRelease = false;
	bool throwOutputReady = false;
	long omittedBufferDescriptor = -1;
	long omittedBufferHalf = -1;
};

FakeConfig fakeConfig;
CLSID activatedClsid{};
bool dspThrows = false;

enum class AsyncHostMode
{
	None,
	LateWorker,
	FastWorker,
	BlockedWorker
};

AsyncHostMode asyncHostMode = AsyncHostMode::None;
ASIOError asyncWorkerReadyResult = ASE_NotPresent;
std::atomic<bool> blockDsp{false};
std::atomic<bool> dspEntered{false};
std::atomic<bool> releaseDsp{false};
std::atomic<int> dspProcessCount{0};
std::atomic<int> dspFactoryCount{0};
double lastDspSampleRate = 0.0;
std::atomic<unsigned> concurrentInitEntries{0};
std::atomic<bool> releaseConcurrentInit{false};
std::atomic<bool> blockHostCallback{false};
std::atomic<bool> hostCallbackEntered{false};
std::atomic<bool> releaseHostCallback{false};
std::atomic<bool> vendorStartReturned{false};
std::atomic<bool> blockedWorkerHasBuffer{false};
std::atomic<bool> releaseBlockedWorker{false};
std::thread blockedHostWorker;
bool hostThrowsAfterReady = false;
bool hostThrowsBeforeReady = false;
bool hostReentersDirect = false;
bool disposeDuringMessage = false;
bool disposeDuringSampleRate = false;
bool createDuringMessage = false;
bool releaseDuringMessage = false;
ASIOError reentrantMessageDisposeResult = ASE_OK;
ASIOError reentrantSampleRateDisposeResult = ASE_OK;
ASIOError reentrantMessageCreateResult = ASE_OK;
ULONG reentrantReleaseRemaining = 0;
std::atomic<bool> vendorDestroyed{false};
ASIOBool reentrantInitResult = ASIOTrue;
bool fakeActivatorThrows = false;
long hostMessageResult = 0;
std::atomic<int> hostBufferCallbackCount{0};
bool overrideTimeInfoResult = false;
ASIOTime* timeInfoResult = nullptr;
extern ASIOCallbacks hostCallbacks;
thread_local long fakeVendorCallbackIndex = -1;
double hostLeftValue = 0.25;
double hostRightValue = 0.5;

void performReentrantInit();
ASIOError signalCurrentProxyReadyDuringStop();

class FakeVendor final : public IASIO
{
public:
	struct ReadySnapshot
	{
		long callbackIndex = -1;
		std::array<double, 2> outputSamples{};
		std::atomic<bool> published{false};
	};

	FakeVendor() : config(fakeConfig) {}

	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override
	{
		if (object == nullptr)
			return E_POINTER;
		*object = nullptr;
		if (IsEqualIID(iid, IID_IUnknown) || InlineIsEqualGUID(iid, kFakeVendorClsid))
		{
			*object = static_cast<IASIO*>(this);
			AddRef();
			return S_OK;
		}
		return E_NOINTERFACE;
	}

	ULONG STDMETHODCALLTYPE AddRef() override
	{
		return references.fetch_add(1, std::memory_order_relaxed) + 1;
	}

	ULONG STDMETHODCALLTYPE Release() override
	{
		if (config.throwRelease)
			throw std::runtime_error("test vendor release failure");
		const ULONG remaining = references.fetch_sub(1, std::memory_order_acq_rel) - 1;
		if (remaining == 0)
			delete this;
		return remaining;
	}

	ASIOBool init(void*) override
	{
		++initCount;
		if (config.throwInit)
			throw std::runtime_error("test vendor init failure");
		if (config.reenterProxyInit)
			performReentrantInit();
		if (config.waitForConcurrentInit)
		{
			concurrentInitEntries.fetch_add(1, std::memory_order_acq_rel);
			while (!releaseConcurrentInit.load(std::memory_order_acquire))
				std::this_thread::yield();
		}
		return ASIOTrue;
	}
	void getDriverName(char* name) override { std::strcpy(name, "Fake Vendor"); }
	long getDriverVersion() override { return 1; }
	void getErrorMessage(char* message) override { std::strcpy(message, "fake error"); }
	ASIOError start() override
	{
		++startCount;
		if (config.throwStart)
			throw std::runtime_error("test vendor start failure");
		joinStartCallback();
		if (config.callbackDuringStart && callbacks != nullptr)
		{
			startCallbackThread = std::thread([this]() {
				callbacks->bufferSwitch(
					0, config.callbackDuringStartDirectProcess);
			});
			while (!hostCallbackEntered.load(std::memory_order_acquire))
				std::this_thread::yield();
		}
		if (config.inlineCallbackDuringStart && callbacks != nullptr)
			callbacks->bufferSwitch(
				0, config.callbackDuringStartDirectProcess);
		vendorStartReturned.store(true, std::memory_order_release);
		return config.startResult;
	}
	ASIOError stop() override
	{
		++stopCount;
		record(Event::VendorStop);
		if (config.outputReadyDuringStop)
			signalCurrentProxyReadyDuringStop();
		if (config.throwStop)
			throw std::runtime_error("test vendor stop failure");
		return config.stopResult;
	}
	ASIOError getChannels(long* inputs, long* outputs) override
	{
		*inputs = config.inputChannels;
		*outputs = config.outputChannels;
		return config.getChannelsResult;
	}
	ASIOError getLatencies(long* input, long* output) override
	{
		*input = config.inputLatency;
		*output = config.outputLatency;
		return ASE_OK;
	}
	ASIOError getBufferSize(long* minimum, long* maximum, long* preferred, long* granularity) override
	{
		*minimum = 2;
		*maximum = 64;
		*preferred = config.preferredBufferSize;
		*granularity = 0;
		return config.getBufferSizeResult;
	}
	ASIOError canSampleRate(ASIOSampleRate rate) override
	{
		return rate == 48000.0 ? ASE_OK : ASE_NoClock;
	}
	ASIOError getSampleRate(ASIOSampleRate* rate) override
	{
		++getSampleRateCount;
		*rate = config.sampleRate;
		if (config.sampleRateCallbackDuringQuery && callbacks != nullptr)
		{
			config.sampleRateCallbackDuringQuery = false;
			callbacks->sampleRateDidChange(config.sampleRate);
		}
		return ASE_OK;
	}
	ASIOError setSampleRate(ASIOSampleRate rate) override
	{
		++setSampleRateCount;
		return rate == 48000.0 ? ASE_OK : ASE_NoClock;
	}
	ASIOError getClockSources(ASIOClockSource*, long* count) override
	{
		++getClockSourcesCount;
		*count = 0;
		return ASE_OK;
	}
	ASIOError setClockSource(long) override { return ASE_OK; }
	ASIOError getSamplePosition(ASIOSamples* position, ASIOTimeStamp* timestamp) override
	{
		*position = ASIOSamples{};
		*timestamp = ASIOTimeStamp{};
		return ASE_OK;
	}
	ASIOError getChannelInfo(ASIOChannelInfo* info) override
	{
		queriedOutputChannels.push_back(info->channel);
		info->type = info->isInput == ASIOFalse ? config.outputType : ASIOSTFloat32LSB;
		return ASE_OK;
	}
	ASIOError createBuffers(
		ASIOBufferInfo* infos,
		long count,
		long size,
		ASIOCallbacks* suppliedCallbacks) override
	{
		++createCount;
		callbacks = suppliedCallbacks;
		retainedInfos = infos;
		retainedInfoCount = count;
		if (config.throwCreate)
			throw std::runtime_error("test vendor create failure");
		if (callbacks != nullptr && callbacks->asioMessage != nullptr)
		{
			createNegotiationResult = callbacks->asioMessage(
				kAsioSupportsTimeInfo, 0, nullptr, nullptr);
		}
		if (config.createResult != ASE_OK)
		{
			callbacks = nullptr;
			retainedInfos = nullptr;
			retainedInfoCount = 0;
			return config.createResult;
		}
		storage.clear();
		storage.resize(static_cast<std::size_t>(count));
		payloadBytes.clear();
		payloadBytes.resize(static_cast<std::size_t>(count));
		for (long index = 0; index < count; ++index)
		{
			const ASIOSampleType type = infos[index].isInput == ASIOFalse ?
				config.outputType : ASIOSTFloat32LSB;
			const std::size_t bytes =
				HibikiAsio::AsioSampleCodec::describe(type).containerBytes;
			payloadBytes[static_cast<std::size_t>(index)] =
				static_cast<std::size_t>(size) * bytes;
			for (long half = 0; half < 2; ++half)
			{
				storage[index][half].assign(
					payloadBytes[static_cast<std::size_t>(index)] + 2 * kGuardBytes,
					kGuardValue);
				if (index != config.omittedBufferDescriptor ||
					half != config.omittedBufferHalf)
				{
					infos[index].buffers[half] =
						storage[index][half].data() + kGuardBytes;
				}
			}
		}
		return ASE_OK;
	}
	ASIOError disposeBuffers() override
	{
		++disposeCount;
		if (config.throwDispose)
			throw std::runtime_error("test vendor dispose failure");
		if (config.disposeResult == ASE_OK)
		{
			joinStartCallback();
			callbacks = nullptr;
			retainedInfos = nullptr;
			retainedInfoCount = 0;
			storage.clear();
		}
		return config.disposeResult;
	}
	ASIOError controlPanel() override { return config.controlPanelResult; }
	ASIOError future(long selector, void* option) override
	{
		++futureCount;
		lastFutureSelector = selector;
		if (selector == kAsioGetIoFormat && option != nullptr)
			static_cast<ASIOIoFormat*>(option)->FormatType = config.ioFormatOnGet;
		if (selector == kAsioGetInternalBufferSamples && option != nullptr)
		{
			auto* const info = static_cast<ASIOInternalBufferInfo*>(option);
			info->inputSamples = config.internalInputSamples;
			info->outputSamples = config.internalOutputSamples;
		}
		return config.futureResult;
	}
	ASIOError outputReady() override
	{
		outputReadyCount.fetch_add(1, std::memory_order_acq_rel);
		if (config.throwOutputReady)
			throw std::runtime_error("test vendor outputReady failure");
		const std::size_t snapshotIndex = readySnapshotCount.fetch_add(
			1, std::memory_order_acq_rel);
		if (snapshotIndex < readySnapshots.size())
		{
			ReadySnapshot& snapshot = readySnapshots[snapshotIndex];
			snapshot.callbackIndex = fakeVendorCallbackIndex;
			std::size_t output = 0;
			if (fakeVendorCallbackIndex >= 0 && fakeVendorCallbackIndex <= 1 &&
				retainedInfos != nullptr)
			{
				for (long descriptor = 0;
					descriptor < retainedInfoCount && output < 2; ++descriptor)
				{
					if (retainedInfos[descriptor].isInput != ASIOFalse)
						continue;
					std::array<double, 1> sample{};
					if (HibikiAsio::AsioSampleCodec::decode(
						retainedInfos[descriptor].buffers[fakeVendorCallbackIndex],
						HibikiAsio::AsioSampleCodec::describe(config.outputType),
						sample.data(), sample.size()))
					{
						snapshot.outputSamples[output] = sample[0];
					}
					++output;
				}
			}
			snapshot.published.store(true, std::memory_order_release);
		}
		record(Event::VendorReady);
		return config.outputReadyResult;
	}

	void triggerBuffer(long index, ASIOBool directProcess = ASIOTrue)
	{
		const long prior = fakeVendorCallbackIndex;
		fakeVendorCallbackIndex = index;
		if (callbacks != nullptr)
			callbacks->bufferSwitch(index, directProcess);
		fakeVendorCallbackIndex = prior;
	}

	ASIOTime* triggerTimeInfo(
		ASIOTime* time, long index, ASIOBool directProcess = ASIOTrue)
	{
		const long prior = fakeVendorCallbackIndex;
		fakeVendorCallbackIndex = index;
		ASIOTime* const result = callbacks == nullptr ? time :
			callbacks->bufferSwitchTimeInfo(time, index, directProcess);
		fakeVendorCallbackIndex = prior;
		return result;
	}

	long triggerMessage(long selector)
	{
		return callbacks == nullptr || callbacks->asioMessage == nullptr ? 0 :
			callbacks->asioMessage(selector, 0, nullptr, nullptr);
	}

	void triggerSampleRateDidChange(ASIOSampleRate sampleRate)
	{
		if (callbacks != nullptr && callbacks->sampleRateDidChange != nullptr)
			callbacks->sampleRateDidChange(sampleRate);
	}

	void* buffer(long descriptor, long half)
	{
		return storage[static_cast<std::size_t>(descriptor)]
			[static_cast<std::size_t>(half)].data() + kGuardBytes;
	}

	bool guardsIntact() const
	{
		for (std::size_t descriptor = 0; descriptor < storage.size(); ++descriptor)
		{
			for (const auto& half : storage[descriptor])
			{
				for (std::size_t index = 0; index < kGuardBytes; ++index)
				{
					if (half[index] != kGuardValue ||
						half[kGuardBytes + payloadBytes[descriptor] + index] != kGuardValue)
						return false;
				}
			}
		}
		return true;
	}

	FakeConfig config;
	ASIOCallbacks* callbacks = nullptr;
	std::vector<std::array<std::vector<std::uint8_t>, 2>> storage;
	std::vector<std::size_t> payloadBytes;
	std::vector<long> queriedOutputChannels;
	ASIOBufferInfo* retainedInfos = nullptr;
	long retainedInfoCount = 0;
	int initCount = 0;
	int createCount = 0;
	int startCount = 0;
	std::atomic<int> stopCount{0};
	int disposeCount = 0;
	std::atomic<int> outputReadyCount{0};
	std::array<ReadySnapshot, 64> readySnapshots{};
	std::atomic<std::size_t> readySnapshotCount{0};
	int setSampleRateCount = 0;
	int getSampleRateCount = 0;
	int getClockSourcesCount = 0;
	int futureCount = 0;
	long lastFutureSelector = 0;
	long createNegotiationResult = 0;

private:
	static constexpr std::size_t kGuardBytes = 16;
	static constexpr std::uint8_t kGuardValue = 0xA5;
	void joinStartCallback()
	{
		if (startCallbackThread.joinable())
			startCallbackThread.join();
	}

	~FakeVendor()
	{
		joinStartCallback();
		vendorDestroyed.store(true, std::memory_order_release);
	}
	std::atomic<ULONG> references{1};
	std::thread startCallbackThread;
};

FakeVendor* currentVendor = nullptr;
AsioProxyDriver* currentProxy = nullptr;
ASIOBufferInfo* currentInfos = nullptr;
std::array<std::size_t, 2> currentOutputIndices{2, 3};

HibikiAsio::ResolvedAsioTarget fakeResolver(HMODULE)
{
	return {HibikiAsio::TargetSelectionStatus::Selected,
		kFakeVendorClsid, L"Fake", L"C:\\FakeVendor.dll"};
}

HibikiAsio::ResolvedAsioTarget selfResolver(HMODULE)
{
	return {HibikiAsio::TargetSelectionStatus::Selected,
		HibikiAsio::kDriverClsid, L"Self", L"C:\\HibikiEQAPODriver.dll"};
}

HRESULT fakeActivator(const CLSID& clsid, IASIO** driver)
{
	if (fakeActivatorThrows)
		throw std::runtime_error("test vendor activation failure");
	activatedClsid = clsid;
	currentVendor = new FakeVendor();
	*driver = currentVendor;
	return S_OK;
}

HRESULT independentFakeActivator(const CLSID&, IASIO** driver)
{
	if (driver == nullptr)
		return E_POINTER;
	*driver = new FakeVendor();
	return S_OK;
}

class FakeDsp final : public HibikiAsio::IAsioDsp
{
public:
	bool process(
		double** output,
		double** input,
		unsigned channels,
		unsigned frames) override
	{
		dspProcessCount.fetch_add(1, std::memory_order_relaxed);
		if (blockDsp.load(std::memory_order_acquire))
		{
			dspEntered.store(true, std::memory_order_release);
			while (!releaseDsp.load(std::memory_order_acquire))
				std::this_thread::yield();
		}
		record(Event::Dsp);
		if (dspThrows)
			throw std::runtime_error("test DSP failure");
		for (unsigned channel = 0; channel < channels; ++channel)
			for (unsigned frame = 0; frame < frames; ++frame)
				output[channel][frame] = input[channel][frame] * 2.0;
		return true;
	}
};

std::unique_ptr<HibikiAsio::IAsioDsp> fakeDspFactory(
	double sampleRate, unsigned, unsigned)
{
	dspFactoryCount.fetch_add(1, std::memory_order_relaxed);
	lastDspSampleRate = sampleRate;
	return std::make_unique<FakeDsp>();
}

void performReentrantInit()
{
	auto* nested = new AsioProxyDriver(
		nullptr, &fakeResolver, &fakeActivator, &fakeDspFactory);
	reentrantInitResult = nested->init(nullptr);
	nested->Release();
}

ASIOError signalCurrentProxyReadyDuringStop()
{
	return currentProxy == nullptr ? ASE_NotPresent : currentProxy->outputReady();
}

void fillHostOutputs(
	long bufferIndex,
	double left = 0.25,
	double right = 0.5)
{
	for (std::size_t channel = 0; channel < currentOutputIndices.size(); ++channel)
	{
		std::array<double, 4> samples{};
		samples.fill(channel == 0 ? left : right);
		check(HibikiAsio::AsioSampleCodec::encode(
			samples.data(),
			HibikiAsio::AsioSampleCodec::describe(fakeConfig.outputType),
			currentInfos[currentOutputIndices[channel]].buffers[bufferIndex],
			samples.size()),
			"host test buffer must encode in the selected ASIO format");
	}
}

double decodeSample(const void* raw, ASIOSampleType type, std::size_t frame = 0)
{
	std::array<double, 4> samples{};
	check(HibikiAsio::AsioSampleCodec::decode(
		raw, HibikiAsio::AsioSampleCodec::describe(type),
		samples.data(), samples.size()),
		"test buffer must decode in the selected ASIO format");
	return samples[frame];
}

double vendorOutputSample(
	std::size_t output,
	long half,
	std::size_t frame = 0)
{
	return decodeSample(
		currentVendor->buffer(
			static_cast<long>(currentOutputIndices[output]), half),
		fakeConfig.outputType,
		frame);
}

void hostBufferSwitch(long bufferIndex, ASIOBool)
{
	hostBufferCallbackCount.fetch_add(1, std::memory_order_relaxed);
	record(Event::HostBegin);
	if (blockHostCallback.load(std::memory_order_acquire))
	{
		hostCallbackEntered.store(true, std::memory_order_release);
		while (!releaseHostCallback.load(std::memory_order_acquire))
			std::this_thread::yield();
	}
	if (asyncHostMode != AsyncHostMode::None)
	{
		if (asyncHostMode == AsyncHostMode::FastWorker)
		{
			const double left = hostLeftValue;
			const double right = hostRightValue;
			std::thread worker([bufferIndex, left, right]() {
				fillHostOutputs(bufferIndex, left, right);
				asyncWorkerReadyResult = currentProxy->outputReady();
			});
			worker.join();
		}
		else if (asyncHostMode == AsyncHostMode::BlockedWorker)
		{
			if (blockedHostWorker.joinable())
				blockedHostWorker.join();
			const double left = hostLeftValue;
			const double right = hostRightValue;
			blockedHostWorker = std::thread([bufferIndex, left, right]() {
				blockedWorkerHasBuffer.store(true, std::memory_order_release);
				while (!releaseBlockedWorker.load(std::memory_order_acquire))
					std::this_thread::yield();
				fillHostOutputs(bufferIndex, left, right);
				asyncWorkerReadyResult = currentProxy->outputReady();
			});
			while (!blockedWorkerHasBuffer.load(std::memory_order_acquire))
				std::this_thread::yield();
		}
		if (hostThrowsBeforeReady)
			throw std::runtime_error(
				"test host callback failure after launching a worker");
		record(Event::HostEnd);
		return;
	}
	fillHostOutputs(bufferIndex, hostLeftValue, hostRightValue);
	if (hostReentersDirect)
	{
		hostReentersDirect = false;
		currentVendor->triggerBuffer(bufferIndex, ASIOTrue);
	}
	if (hostThrowsBeforeReady)
		throw std::runtime_error("test host callback failure before ready");
	check(currentProxy->outputReady() == ASE_OK,
		"host outputReady request must be acknowledged");
	record(Event::HostReadyReturned);
	if (hostThrowsAfterReady)
		throw std::runtime_error("test host callback failure");
	record(Event::HostEnd);
}

ASIOTime* hostTimeInfo(ASIOTime* time, long bufferIndex, ASIOBool)
{
	hostBufferSwitch(bufferIndex, ASIOTrue);
	return overrideTimeInfoResult ? timeInfoResult : time;
}

void hostSampleRateChanged(ASIOSampleRate)
{
	if (disposeDuringSampleRate)
		reentrantSampleRateDisposeResult = currentProxy->disposeBuffers();
}

long hostAsioMessage(long selector, long, void*, double*)
{
	if (disposeDuringMessage)
		reentrantMessageDisposeResult = currentProxy->disposeBuffers();
	if (createDuringMessage)
	{
		ASIOBufferInfo nested{};
		nested.isInput = ASIOFalse;
		nested.channelNum = 0;
		reentrantMessageCreateResult = currentProxy->createBuffers(
			&nested, 1, 4, &hostCallbacks);
	}
	if (releaseDuringMessage)
	{
		releaseDuringMessage = false;
		reentrantReleaseRemaining = currentProxy->Release();
	}
	if (selector == kAsioSupportsTimeInfo)
	{
		record(Event::Negotiation);
		return 1;
	}
	if (selector == kAsioResetRequest || selector == kAsioBufferSizeChange)
		return hostMessageResult;
	return 0;
}

ASIOCallbacks hostCallbacks = {
	&hostBufferSwitch,
	&hostSampleRateChanged,
	&hostAsioMessage,
	&hostTimeInfo
};

void inputOnlyBufferSwitch(long, ASIOBool)
{
	hostBufferCallbackCount.fetch_add(1, std::memory_order_relaxed);
}

ASIOTime* inputOnlyTimeInfo(ASIOTime* time, long, ASIOBool)
{
	hostBufferCallbackCount.fetch_add(1, std::memory_order_relaxed);
	return time;
}

ASIOCallbacks inputOnlyCallbacks = {
	&inputOnlyBufferSwitch,
	&hostSampleRateChanged,
	&hostAsioMessage,
	&inputOnlyTimeInfo
};

AsioProxyDriver* createInitializedDriver()
{
	auto* driver = new AsioProxyDriver(
		nullptr, &fakeResolver, &fakeActivator, &fakeDspFactory);
	check(driver->init(nullptr) == ASIOTrue, "proxy init must activate fake vendor");
	check(InlineIsEqualGUID(activatedClsid, kFakeVendorClsid) != FALSE,
		"vendor activation must request the configured CLSID");
	return driver;
}

std::array<ASIOBufferInfo, 4> makeStereoDescriptors()
{
	std::array<ASIOBufferInfo, 4> infos{};
	infos[0].isInput = ASIOTrue;
	infos[0].channelNum = 0;
	infos[1].isInput = ASIOTrue;
	infos[1].channelNum = 3;
	infos[2].isInput = ASIOFalse;
	infos[2].channelNum = 0;
	infos[3].isInput = ASIOFalse;
	infos[3].channelNum = 1;
	return infos;
}

void resetHarness()
{
	if (blockedHostWorker.joinable())
	{
		releaseBlockedWorker.store(true, std::memory_order_release);
		blockedHostWorker.join();
	}
	fakeConfig = {};
	dspThrows = false;
	asyncHostMode = AsyncHostMode::None;
	asyncWorkerReadyResult = ASE_NotPresent;
	blockDsp.store(false, std::memory_order_relaxed);
	dspEntered.store(false, std::memory_order_relaxed);
	releaseDsp.store(false, std::memory_order_relaxed);
	dspProcessCount.store(0, std::memory_order_relaxed);
	dspFactoryCount.store(0, std::memory_order_relaxed);
	lastDspSampleRate = 0.0;
	concurrentInitEntries.store(0, std::memory_order_relaxed);
	releaseConcurrentInit.store(false, std::memory_order_relaxed);
	blockHostCallback.store(false, std::memory_order_relaxed);
	hostCallbackEntered.store(false, std::memory_order_relaxed);
	releaseHostCallback.store(false, std::memory_order_relaxed);
	vendorStartReturned.store(false, std::memory_order_relaxed);
	blockedWorkerHasBuffer.store(false, std::memory_order_relaxed);
	releaseBlockedWorker.store(false, std::memory_order_relaxed);
	hostThrowsAfterReady = false;
	hostThrowsBeforeReady = false;
	hostReentersDirect = false;
	disposeDuringMessage = false;
	disposeDuringSampleRate = false;
	createDuringMessage = false;
	releaseDuringMessage = false;
	reentrantMessageDisposeResult = ASE_OK;
	reentrantSampleRateDisposeResult = ASE_OK;
	reentrantMessageCreateResult = ASE_OK;
	reentrantReleaseRemaining = 0;
	vendorDestroyed.store(false, std::memory_order_relaxed);
	reentrantInitResult = ASIOTrue;
	fakeActivatorThrows = false;
	hostMessageResult = 0;
	hostBufferCallbackCount.store(0, std::memory_order_relaxed);
	overrideTimeInfoResult = false;
	timeInfoResult = nullptr;
	hostLeftValue = 0.25;
	hostRightValue = 0.5;
	eventCount.store(0, std::memory_order_relaxed);
	currentVendor = nullptr;
	currentProxy = nullptr;
	currentInfos = nullptr;
}

void testClsidQueryAndActivation()
{
	resetHarness();
	auto* driver = createInitializedDriver();
	void* object = nullptr;
	check(driver->QueryInterface(HibikiAsio::kDriverClsid, &object) == S_OK &&
		object != nullptr, "QI must accept the proxy CLSID ASIO IID");
	static_cast<IUnknown*>(object)->Release();
	object = nullptr;
	check(driver->QueryInterface(IID_IUnknown, &object) == S_OK && object != nullptr,
		"QI must accept IID_IUnknown");
	static_cast<IUnknown*>(object)->Release();
	const CLSID unknown = {0xDEADBEEF, 0, 0, {0}};
	check(driver->QueryInterface(unknown, &object) == E_NOINTERFACE,
		"QI must reject unrelated IIDs");
	driver->Release();

	auto* self = new AsioProxyDriver(
		nullptr, &selfResolver, &fakeActivator, &fakeDspFactory);
	check(self->init(nullptr) == ASIOFalse,
		"init must reject the proxy's own CLSID before activation");
	self->Release();
}

void testIndirectProxyInitializationRecursionIsBlocked()
{
	resetHarness();
	fakeConfig.reenterProxyInit = true;
	auto* driver = createInitializedDriver();
	check(reentrantInitResult == ASIOFalse,
		"same-call-stack initialization guard must block indirect wrapper recursion");
	driver->Release();

	resetHarness();
	driver = createInitializedDriver();
	check(driver != nullptr,
		"initialization guard must release after the outer initialization returns");
	driver->Release();
}

void testIndependentThreadsMayInitializeInParallel()
{
	resetHarness();
	fakeConfig.waitForConcurrentInit = true;
	auto* first = new AsioProxyDriver(
		nullptr, &fakeResolver, &independentFakeActivator, &fakeDspFactory);
	auto* second = new AsioProxyDriver(
		nullptr, &fakeResolver, &independentFakeActivator, &fakeDspFactory);
	ASIOBool firstResult = ASIOFalse;
	ASIOBool secondResult = ASIOFalse;
	std::thread firstThread([&]() { firstResult = first->init(nullptr); });
	std::thread secondThread([&]() { secondResult = second->init(nullptr); });
	for (unsigned spin = 0;
		spin < 100000 && concurrentInitEntries.load(std::memory_order_acquire) < 2;
		++spin)
	{
		std::this_thread::yield();
	}
	releaseConcurrentInit.store(true, std::memory_order_release);
	firstThread.join();
	secondThread.join();
	check(concurrentInitEntries.load(std::memory_order_acquire) == 2 &&
		firstResult == ASIOTrue && secondResult == ASIOTrue,
		"independent host threads must not be mistaken for recursive initialization");
	first->Release();
	second->Release();
}

void testOneBlockDirectStagingAndLatencyAccounting()
{
	resetHarness();
	auto* driver = createInitializedDriver();
	long inputLatency = 0;
	long outputLatency = 0;
	check(driver->getLatencies(&inputLatency, &outputLatency) == ASE_OK &&
		inputLatency == 17 && outputLatency == 27,
		"latency before createBuffers must include the preferred staged block");
	auto infos = makeStereoDescriptors();
	currentProxy = driver;
	currentInfos = infos.data();
	check(driver->createBuffers(infos.data(), 4, 4, &hostCallbacks) == ASE_OK,
		"direct staging buffers must prepare");
	void* const hostLeft0 = infos[2].buffers[0];
	void* const hostLeft1 = infos[2].buffers[1];
	check(hostLeft0 != currentVendor->buffer(2, 0) &&
		hostLeft1 != currentVendor->buffer(2, 1),
		"host output pointers must be fixed proxy-owned buffers");
	check(driver->getLatencies(&inputLatency, &outputLatency) == ASE_OK &&
		inputLatency == 17 && outputLatency == 27,
		"prepared output latency must include exactly one block");
	ASIOInternalBufferInfo internal{};
	check(driver->future(kAsioGetInternalBufferSamples, &internal) == ASE_SUCCESS &&
		internal.inputSamples == 7 && internal.outputSamples == 15,
		"internal-buffer extension must include the staged output block");

	fillHostOutputs(1, 0.10, 0.20);
	check(driver->start() == ASE_OK,
		"direct staging stream must start after pre-roll capture");
	check(dspProcessCount.load(std::memory_order_acquire) == 1 &&
		std::abs(vendorOutputSample(0, 0)) < 1e-12 &&
		std::abs(vendorOutputSample(0, 1)) < 1e-12,
		"start must DSP prefilled H1 but leave both vendor halves silent");
	eventCount = 0;
	hostLeftValue = 0.30;
	hostRightValue = 0.40;
	currentVendor->triggerBuffer(0, ASIOTrue);
	const std::array<Event, 5> expected = {
		Event::VendorReady,
		Event::HostBegin,
		Event::HostReadyReturned,
		Event::HostEnd,
		Event::Dsp
	};
	check(eventCount == expected.size(),
		"direct staged callback must have five ordered events");
	for (std::size_t index = 0; index < expected.size() && index < eventCount; ++index)
		check(events[index] == expected[index],
			"vendor commit must precede this callback's host/DSP staging");
	check(std::abs(vendorOutputSample(0, 0) - 0.20) < 1e-6 &&
		std::abs(vendorOutputSample(1, 0) - 0.40) < 1e-6,
		"first callback must commit corrected pre-roll to the safe vendor half");
	check(std::abs(decodeSample(hostLeft0, fakeConfig.outputType) - 0.30) < 1e-6,
		"DSP must not rewrite the host-owned output buffer");
	check(infos[2].buffers[0] == hostLeft0 && infos[2].buffers[1] == hostLeft1,
		"host output buffer addresses must remain stable after callbacks");
	hostLeftValue = 0.45;
	hostRightValue = 0.35;
	currentVendor->triggerBuffer(1, ASIOTrue);
	check(std::abs(vendorOutputSample(0, 1) - 0.60) < 1e-6 &&
		std::abs(vendorOutputSample(1, 1) - 0.80) < 1e-6,
		"next callback must commit the prior host block at fixed one-block latency");
	hostLeftValue = 0.15;
	hostRightValue = 0.25;
	currentVendor->triggerBuffer(0, ASIOTrue);
	check(std::abs(vendorOutputSample(0, 0) - 0.90) < 1e-6 &&
		std::abs(vendorOutputSample(1, 0) - 0.70) < 1e-6,
		"third callback must commit the immediately preceding C block without swap/repeat");
	bool snapshotsMatch =
		currentVendor->readySnapshotCount.load(std::memory_order_acquire) == 3;
	const std::array<long, 3> expectedHalves = {0, 1, 0};
	const std::array<double, 3> expectedLeft = {0.20, 0.60, 0.90};
	const std::array<double, 3> expectedRight = {0.40, 0.80, 0.70};
	for (std::size_t index = 0; index < expectedHalves.size(); ++index)
	{
		const auto& snapshot = currentVendor->readySnapshots[index];
		snapshotsMatch = snapshotsMatch &&
			snapshot.published.load(std::memory_order_acquire) &&
			snapshot.callbackIndex == expectedHalves[index] &&
			std::abs(snapshot.outputSamples[0] - expectedLeft[index]) < 1e-6 &&
			std::abs(snapshot.outputSamples[1] - expectedRight[index]) < 1e-6;
	}
	check(snapshotsMatch,
		"vendor outputReady snapshots must observe each fully committed A/B/C block in order");
	check(currentVendor->outputReadyCount == 3 && currentVendor->guardsIntact(),
		"each vendor commit must signal once and preserve allocation canaries");
	driver->stop();
	driver->disposeBuffers();
	driver->Release();
}

void testPreCreateLatencyFallbackAndSaturation()
{
	resetHarness();
	fakeConfig.getBufferSizeResult = ASE_HWMalfunction;
	auto* driver = createInitializedDriver();
	long input = 0;
	long output = 0;
	check(driver->getLatencies(&input, &output) == ASE_OK &&
		input == 17 && output == 23,
		"failed preferred-size lookup must preserve the valid vendor latency");
	driver->Release();

	resetHarness();
	fakeConfig.outputLatency = (std::numeric_limits<long>::max)() - 2;
	fakeConfig.preferredBufferSize = 4;
	driver = createInitializedDriver();
	check(driver->getLatencies(&input, &output) == ASE_OK &&
		output == (std::numeric_limits<long>::max)(),
		"pre-create staged latency addition must saturate without overflow");
	driver->Release();
}

void testAdvertisedMonitorPairHandlesVendorCountsTransactionally()
{
	resetHarness();
	fakeConfig.outputChannels = 0;
	auto* driver = createInitializedDriver();
	long inputs = -1;
	long outputs = -1;
	check(driver->getChannels(&inputs, &outputs) == ASE_OK &&
		inputs == 4 && outputs == 0,
		"an input-only vendor must advertise no proxy outputs");
	ASIOBufferInfo inputOnly{};
	inputOnly.isInput = ASIOTrue;
	inputOnly.channelNum = 3;
	check(driver->createBuffers(&inputOnly, 1, 4, &inputOnlyCallbacks) == ASE_OK,
		"an input-only vendor must remain usable without output staging");
	check(driver->disposeBuffers() == ASE_OK,
		"input-only channel-count buffers must dispose cleanly");
	driver->Release();

	resetHarness();
	fakeConfig.outputChannels = 1;
	driver = createInitializedDriver();
	inputs = -1;
	outputs = -1;
	check(driver->getChannels(&inputs, &outputs) == ASE_OK && outputs == 1,
		"a mono vendor must advertise exactly one proxy output");
	ASIOBufferInfo mono{};
	mono.isInput = ASIOFalse;
	mono.channelNum = 0;
	check(driver->createBuffers(&mono, 1, 4, &hostCallbacks) == ASE_OK,
		"the advertised mono output must be creatable");
	check(driver->disposeBuffers() == ASE_OK,
		"mono channel-count buffers must dispose cleanly");
	driver->Release();

	resetHarness();
	fakeConfig.outputChannels = -1;
	driver = createInitializedDriver();
	inputs = 91;
	outputs = 92;
	check(driver->getChannels(&inputs, &outputs) == ASE_HWMalfunction &&
		inputs == 91 && outputs == 92,
		"negative vendor channel counts must fail without publishing caller values");
	driver->Release();

	resetHarness();
	fakeConfig.getChannelsResult = ASE_HWMalfunction;
	driver = createInitializedDriver();
	inputs = 93;
	outputs = 94;
	check(driver->getChannels(&inputs, &outputs) == ASE_HWMalfunction &&
		inputs == 93 && outputs == 94,
		"vendor getChannels failure must leave both caller values unchanged");
	driver->Release();
}

void testOutputReadyProbeDoesNotConsumePrefill()
{
	resetHarness();
	auto* driver = createInitializedDriver();
	check(driver->outputReady() == ASE_OK && currentVendor->outputReadyCount == 1,
		"Initialized outputReady must probe the vendor once");
	auto infos = makeStereoDescriptors();
	currentProxy = driver;
	currentInfos = infos.data();
	check(driver->createBuffers(infos.data(), 4, 4, &hostCallbacks) == ASE_OK,
		"probe staging buffers must prepare");
	fillHostOutputs(1);
	check(driver->outputReady() == ASE_OK && currentVendor->outputReadyCount == 1,
		"Prepared probe must be cached and must not consume pre-roll content");
	check(driver->start() == ASE_OK, "probe stream must start");
	currentVendor->triggerBuffer(0, ASIOTrue);
	check(std::abs(vendorOutputSample(0, 0) - 0.5) < 1e-6 &&
		currentVendor->outputReadyCount == 2,
		"pre-start probes must not steal H1 from the first staged commit");
	driver->stop();
	driver->disposeBuffers();
	driver->Release();
}

void testCreateBuffersRejectsNegotiationReentryTransactionally()
{
	resetHarness();
	auto* driver = createInitializedDriver();
	auto infos = makeStereoDescriptors();
	currentProxy = driver;
	currentInfos = infos.data();
	createDuringMessage = true;
	check(driver->createBuffers(infos.data(), 4, 4, &hostCallbacks) == ASE_OK,
		"outer createBuffers must survive host negotiation reentry");
	createDuringMessage = false;
	check(reentrantMessageCreateResult == ASE_InvalidMode &&
		currentVendor->createCount == 1 && infos[2].buffers[0] != nullptr,
		"nested createBuffers must fail before mutating outer prepared state");
	check(driver->disposeBuffers() == ASE_OK,
		"outer buffers must remain disposable after rejected reentry");
	driver->Release();
}

void testPreparedAndFailedDisposeCallbacksCannotEnterThePipeline()
{
	resetHarness();
	auto* driver = createInitializedDriver();
	auto infos = makeStereoDescriptors();
	currentProxy = driver;
	currentInfos = infos.data();
	check(driver->createBuffers(infos.data(), 4, 4, &hostCallbacks) == ASE_OK,
		"prepared-state gate buffers must prepare");
	fillHostOutputs(1);
	currentVendor->triggerBuffer(0, ASIOTrue);
	ASIOTime time{};
	check(currentVendor->triggerTimeInfo(&time, 1, ASIOTrue) == &time &&
		hostBufferCallbackCount.load(std::memory_order_acquire) == 0 &&
		dspProcessCount.load(std::memory_order_acquire) == 0 &&
		currentVendor->outputReadyCount.load(std::memory_order_acquire) == 0,
		"pre-start vendor callbacks must not consume sequence, host buffers, or DSP");
	check(driver->outputReady() == ASE_OK &&
		currentVendor->outputReadyCount.load(std::memory_order_acquire) == 1,
		"Prepared outputReady must remain a capability probe only");
	check(driver->start() == ASE_OK, "prepared-state gate stream must start");
	currentVendor->triggerBuffer(0, ASIOTrue);
	check(std::abs(vendorOutputSample(0, 0) - 0.5) < 1e-6,
		"ignored pre-start callbacks must not disturb the first H1 pre-roll commit");
	check(driver->stop() == ASE_OK, "prepared-state gate stream must stop");
	const int hostBefore = hostBufferCallbackCount.load(std::memory_order_acquire);
	const int dspBefore = dspProcessCount.load(std::memory_order_acquire);
	const int readyBefore = currentVendor->outputReadyCount.load(std::memory_order_acquire);
	currentVendor->config.disposeResult = ASE_InvalidMode;
	check(driver->disposeBuffers() == ASE_InvalidMode,
		"failed vendor dispose must propagate while retaining prepared state");
	currentVendor->triggerBuffer(1, ASIOTrue);
	check(hostBufferCallbackCount.load(std::memory_order_acquire) == hostBefore &&
		dspProcessCount.load(std::memory_order_acquire) == dspBefore &&
		currentVendor->outputReadyCount.load(std::memory_order_acquire) == readyBefore,
		"callbacks after failed dispose must not re-enter a non-running pipeline");
	currentVendor->config.disposeResult = ASE_OK;
	check(driver->disposeBuffers() == ASE_OK,
		"failed dispose must remain recoverable when no host producer is unresolved");
	driver->Release();
}

void testDeferredWorkerStagingAndVendorReadyCapability()
{
	resetHarness();
	fakeConfig.outputReadyResult = ASE_NotPresent;
	auto* driver = createInitializedDriver();
	auto infos = makeStereoDescriptors();
	currentProxy = driver;
	currentInfos = infos.data();
	check(driver->createBuffers(infos.data(), 4, 4, &hostCallbacks) == ASE_OK,
		"deferred worker buffers must prepare");
	fillHostOutputs(1);
	check(driver->start() == ASE_OK, "deferred worker stream must start");
	asyncHostMode = AsyncHostMode::LateWorker;
	currentVendor->triggerBuffer(0, ASIOFalse);
	fillHostOutputs(0);
	check(driver->outputReady() == ASE_OK &&
		dspProcessCount.load(std::memory_order_acquire) == 2,
		"cross-thread outputReady must DSP proxy H0 into private stage memory");
	currentVendor->triggerBuffer(1, ASIOFalse);
	check(std::abs(vendorOutputSample(0, 1) - 0.5) < 1e-6 &&
		std::abs(vendorOutputSample(1, 1) - 1.0) < 1e-6,
		"next vendor callback must commit deferred worker correction");
	fillHostOutputs(1);
	check(driver->outputReady() == ASE_OK && driver->outputReady() == ASE_OK,
		"one deferred completion plus a duplicate probe must both be acknowledged");
	const int hostBeforeDuplicateBoundary =
		hostBufferCallbackCount.load(std::memory_order_acquire);
	currentVendor->triggerBuffer(0, ASIOFalse);
	check(std::abs(vendorOutputSample(0, 0)) < 1e-12 &&
		hostBufferCallbackCount.load(std::memory_order_acquire) ==
			hostBeforeDuplicateBoundary &&
		currentVendor->outputReadyCount == 1,
		"empty-token duplicate must terminally silence staging without a host handoff");
	check(currentVendor->guardsIntact(),
		"deferred staging must stay inside vendor buffer canaries");
	driver->stop();
	driver->disposeBuffers();
	driver->Release();
}

void testInlineFalseCompletionStillAppliesCorrection()
{
	resetHarness();
	auto* driver = createInitializedDriver();
	auto infos = makeStereoDescriptors();
	currentProxy = driver;
	currentInfos = infos.data();
	check(driver->createBuffers(infos.data(), 4, 4, &hostCallbacks) == ASE_OK,
		"inline ASIOFalse buffers must prepare");
	fillHostOutputs(1);
	check(driver->start() == ASE_OK, "inline ASIOFalse stream must start");
	currentVendor->triggerBuffer(0, ASIOFalse);
	check(dspProcessCount.load(std::memory_order_acquire) == 2,
		"inline host outputReady must run callback-safe DSP on proxy memory");
	currentVendor->triggerBuffer(1, ASIOFalse);
	check(std::abs(vendorOutputSample(0, 1) - 0.5) < 1e-6 &&
		std::abs(vendorOutputSample(1, 1) - 1.0) < 1e-6,
		"official-sample-style ASIOFalse inline completion must be corrected");
	driver->stop();
	driver->disposeBuffers();
	driver->Release();
}

void testStartSynchronousDeferredCompletionAndFailureRetry()
{
	resetHarness();
	fakeConfig.inlineCallbackDuringStart = true;
	fakeConfig.callbackDuringStartDirectProcess = ASIOFalse;
	auto* driver = createInitializedDriver();
	auto infos = makeStereoDescriptors();
	currentProxy = driver;
	currentInfos = infos.data();
	check(driver->createBuffers(infos.data(), 4, 4, &hostCallbacks) == ASE_OK,
		"synchronous-start buffers must prepare");
	fillHostOutputs(1);
	asyncHostMode = AsyncHostMode::FastWorker;
	check(driver->start() == ASE_OK && asyncWorkerReadyResult == ASE_OK,
		"worker completion inside vendor start must see the Running boundary");
	check(std::abs(vendorOutputSample(0, 0) - 0.5) < 1e-6 &&
		dspProcessCount.load(std::memory_order_acquire) == 2,
		"synchronous start callback must commit pre-roll and stage its new block");
	driver->stop();
	driver->disposeBuffers();
	driver->Release();

	resetHarness();
	fakeConfig.inlineCallbackDuringStart = true;
	fakeConfig.startResult = ASE_InvalidMode;
	driver = createInitializedDriver();
	infos = makeStereoDescriptors();
	currentProxy = driver;
	currentInfos = infos.data();
	check(driver->createBuffers(infos.data(), 4, 4, &hostCallbacks) == ASE_OK,
		"start-retry buffers must prepare");
	fillHostOutputs(1);
	check(driver->start() == ASE_InvalidMode,
		"vendor start failure must propagate after its synchronous callback drains");
	currentVendor->config.startResult = ASE_OK;
	fillHostOutputs(1);
	check(driver->start() == ASE_OK && currentVendor->startCount == 2 &&
		dspFactoryCount.load(std::memory_order_acquire) == 3,
		"retry must rebuild DSP and staging instead of reusing advanced history");
	check(std::abs(vendorOutputSample(0, 0) - 0.5) < 1e-6,
		"retry must zero vendor buffers then commit freshly rebuilt pre-roll");
	driver->stop();
	driver->disposeBuffers();
	driver->Release();
}

void testLateWorkerDeadlineAndHostHalfReuseAreSafe()
{
	resetHarness();
	auto* driver = createInitializedDriver();
	auto infos = makeStereoDescriptors();
	currentProxy = driver;
	currentInfos = infos.data();
	check(driver->createBuffers(infos.data(), 4, 4, &hostCallbacks) == ASE_OK,
		"deadline buffers must prepare");
	fillHostOutputs(1);
	check(driver->start() == ASE_OK, "deadline stream must start");
	asyncHostMode = AsyncHostMode::LateWorker;
	currentVendor->triggerBuffer(0, ASIOFalse);
	fillHostOutputs(0);
	blockDsp.store(true, std::memory_order_release);
	dspEntered.store(false, std::memory_order_release);
	releaseDsp.store(false, std::memory_order_release);
	std::thread worker([&]() { check(driver->outputReady() == ASE_OK,
		"late worker completion must be acknowledged"); });
	while (!dspEntered.load(std::memory_order_acquire))
		std::this_thread::yield();
	currentVendor->triggerBuffer(1, ASIOFalse);
	const int callbacksBeforeReuse =
		hostBufferCallbackCount.load(std::memory_order_acquire);
	currentVendor->triggerBuffer(0, ASIOFalse);
	check(hostBufferCallbackCount.load(std::memory_order_acquire) ==
		callbacksBeforeReuse,
		"H0 must not be returned to the host while its old worker still reads it");
	check(std::abs(vendorOutputSample(0, 1)) < 1e-12 &&
		std::abs(vendorOutputSample(0, 0)) < 1e-12,
		"missed stages must commit bounded silence instead of late/partial output");
	releaseDsp.store(true, std::memory_order_release);
	worker.join();
	check(currentVendor->guardsIntact(),
		"late DSP completion must never write vendor memory or cross canaries");
	driver->stop();
	driver->disposeBuffers();
	driver->Release();
}

void testSampleRateChangeInvalidatesReadyStage()
{
	resetHarness();
	auto* driver = createInitializedDriver();
	auto infos = makeStereoDescriptors();
	currentProxy = driver;
	currentInfos = infos.data();
	check(driver->createBuffers(infos.data(), 4, 4, &hostCallbacks) == ASE_OK,
		"sample-rate stage buffers must prepare");
	fillHostOutputs(1);
	check(driver->start() == ASE_OK, "sample-rate stage stream must start");
	currentVendor->triggerBuffer(0, ASIOTrue);
	currentVendor->triggerSampleRateDidChange(44100.0);
	currentVendor->triggerBuffer(1, ASIOTrue);
	check(std::abs(vendorOutputSample(0, 1)) < 1e-12 &&
		std::abs(vendorOutputSample(1, 1)) < 1e-12,
		"ready data calculated at the old rate must be silenced, not committed");
	driver->stop();
	driver->disposeBuffers();
	driver->Release();
}

void testTimeInfoReturnPointerIsTransparent()
{
	resetHarness();
	auto* driver = createInitializedDriver();
	auto infos = makeStereoDescriptors();
	currentProxy = driver;
	currentInfos = infos.data();
	check(driver->createBuffers(infos.data(), 4, 4, &hostCallbacks) == ASE_OK,
		"time-info transparency buffers must prepare");
	fillHostOutputs(1);
	check(driver->start() == ASE_OK, "time-info transparency stream must start");
	ASIOTime first{};
	ASIOTime alternate{};
	overrideTimeInfoResult = true;
	timeInfoResult = nullptr;
	check(currentVendor->triggerTimeInfo(&first, 0, ASIOTrue) == nullptr,
		"host nullptr time-info return must reach the vendor unchanged");
	timeInfoResult = &alternate;
	check(currentVendor->triggerTimeInfo(&first, 1, ASIOTrue) == &alternate,
		"alternate host time-info pointer must reach the vendor unchanged");
	timeInfoResult = &first;
	check(currentVendor->triggerTimeInfo(&first, 0, ASIOTrue) == &first,
		"same host time-info pointer must reach the vendor unchanged");
	driver->stop();
	driver->disposeBuffers();
	driver->Release();
}

void testInputOnlyCallbacksDoNotEnterStaging()
{
	resetHarness();
	auto* driver = createInitializedDriver();
	std::array<ASIOBufferInfo, 2> infos{};
	infos[0].isInput = ASIOTrue;
	infos[0].channelNum = 0;
	infos[1].isInput = ASIOTrue;
	infos[1].channelNum = 1;
	check(driver->createBuffers(infos.data(), 2, 4, &inputOnlyCallbacks) == ASE_OK,
		"input-only ASIO descriptors must remain supported");
	check(driver->start() == ASE_OK, "input-only stream must start");
	for (long index = 0; index < 8; ++index)
		currentVendor->triggerBuffer(index & 1,
			(index & 2) == 0 ? ASIOTrue : ASIOFalse);
	check(hostBufferCallbackCount.load(std::memory_order_acquire) == 8,
		"input-only callbacks must stay transparent across repeated half reuse");
	check(currentVendor->outputReadyCount == 0,
		"input-only callbacks must not synthesize vendor output readiness");
	driver->stop();
	driver->disposeBuffers();
	driver->Release();
}

void testStagingSupportsWideAndPackedPcmWithoutOverrun()
{
	const std::array<ASIOSampleType, 6> formats = {
		ASIOSTFloat64LSB,
		ASIOSTFloat64MSB,
		ASIOSTInt24LSB,
		ASIOSTInt24MSB,
		ASIOSTInt32LSB24,
		ASIOSTInt32MSB24
	};
	for (const ASIOSampleType format : formats)
	{
		resetHarness();
		fakeConfig.outputType = format;
		auto* driver = createInitializedDriver();
		auto infos = makeStereoDescriptors();
		currentProxy = driver;
		currentInfos = infos.data();
		check(driver->createBuffers(infos.data(), 4, 4, &hostCallbacks) == ASE_OK,
			"wide/packed PCM staging buffers must prepare");
		fillHostOutputs(1);
		check(driver->start() == ASE_OK, "wide/packed PCM stream must start");
		currentVendor->triggerBuffer(0, ASIOTrue);
		check(std::abs(vendorOutputSample(0, 0) - 0.5) < 1e-5 &&
			std::abs(vendorOutputSample(1, 0) - 1.0) < 1e-5,
			"wide/packed PCM pre-roll must round-trip through staged DSP");
		check(currentVendor->guardsIntact(),
			"format-specific staged byte counts must preserve both canaries");
		driver->stop();
		driver->disposeBuffers();
		driver->Release();
	}
}

void testRecursiveCallbackSilencesStaleVendorHalf()
{
	resetHarness();
	auto* driver = createInitializedDriver();
	auto infos = makeStereoDescriptors();
	currentProxy = driver;
	currentInfos = infos.data();
	check(driver->createBuffers(infos.data(), 4, 4, &hostCallbacks) == ASE_OK,
		"recursive-callback buffers must prepare");
	fillHostOutputs(1);
	check(driver->start() == ASE_OK, "recursive-callback stream must start");
	hostReentersDirect = true;
	currentVendor->triggerBuffer(0, ASIOTrue);
	check(std::abs(vendorOutputSample(0, 0)) < 1e-12 &&
		std::abs(vendorOutputSample(1, 0)) < 1e-12,
		"recursive callback must silence the nested vendor half instead of replaying data");
	check(hostBufferCallbackCount.load(std::memory_order_acquire) == 1,
		"nested callback must not hand a concurrently-owned proxy half to the host");
	driver->stop();
	driver->disposeBuffers();
	driver->Release();
}

void testFailedStopRemainsTerminalUntilSuccessfulBoundary()
{
	resetHarness();
	auto* driver = createInitializedDriver();
	auto infos = makeStereoDescriptors();
	currentProxy = driver;
	currentInfos = infos.data();
	check(driver->createBuffers(infos.data(), 4, 4, &hostCallbacks) == ASE_OK,
		"failed-stop buffers must prepare");
	fillHostOutputs(1);
	check(driver->start() == ASE_OK, "failed-stop stream must start");
	currentVendor->triggerBuffer(0, ASIOTrue);
	const int callbackCount =
		hostBufferCallbackCount.load(std::memory_order_acquire);
	const int dspCount = dspProcessCount.load(std::memory_order_acquire);
	const int readyCount = currentVendor->outputReadyCount.load(std::memory_order_acquire);
	const double vendorBeforeFailure = vendorOutputSample(0, 0);
	currentVendor->config.stopResult = ASE_InvalidMode;
	check(driver->stop() == ASE_InvalidMode,
		"wrapped stop failure must propagate");
	currentVendor->triggerBuffer(0, ASIOTrue);
	check(driver->outputReady() == ASE_OK &&
		hostBufferCallbackCount.load(std::memory_order_acquire) == callbackCount &&
		dspProcessCount.load(std::memory_order_acquire) == dspCount &&
		currentVendor->outputReadyCount.load(std::memory_order_acquire) == readyCount &&
		std::abs(vendorOutputSample(0, 0) - vendorBeforeFailure) < 1e-12,
		"failed stop must make the proxy permanently no-touch/no-ready; hardware state remains vendor-defined");
	currentVendor->config.stopResult = ASE_OK;
	check(driver->stop() == ASE_OK && driver->start() == ASE_OK,
		"a later successful stop/start must establish a clean pipeline boundary");
	currentVendor->triggerBuffer(0, ASIOTrue);
	check(hostBufferCallbackCount.load(std::memory_order_acquire) == callbackCount + 1,
		"callbacks must resume after the successful lifecycle boundary");
	driver->stop();
	driver->disposeBuffers();
	driver->Release();
}

void testFailedStartWithOutstandingWorkerRequiresRecreate()
{
	resetHarness();
	fakeConfig.inlineCallbackDuringStart = true;
	fakeConfig.callbackDuringStartDirectProcess = ASIOFalse;
	fakeConfig.startResult = ASE_InvalidMode;
	auto* driver = createInitializedDriver();
	auto infos = makeStereoDescriptors();
	currentProxy = driver;
	currentInfos = infos.data();
	check(driver->createBuffers(infos.data(), 4, 4, &hostCallbacks) == ASE_OK,
		"failed-start quarantine buffers must prepare");
	fillHostOutputs(1);
	asyncHostMode = AsyncHostMode::BlockedWorker;
	check(driver->start() == ASE_InvalidMode,
		"failed start with an outstanding deferred token must propagate");
	currentVendor->config.startResult = ASE_OK;
	check(driver->start() == ASE_InvalidMode && currentVendor->startCount == 1,
		"an unidentifiable old worker completion must block in-place start retry");
	check(driver->disposeBuffers() == ASE_OK,
		"vendor buffers may be disposed while the unresolved host arena is pinned");
	check(driver->start() == ASE_InvalidMode &&
		HibikiAsio::AsioCallbackBridge::isPoisoned() &&
		DllCanUnloadNow() == S_FALSE,
		"unresolved host ownership must permanently quarantine the process");
	const ULONG remaining = driver->Release();
	check(remaining == 1 && !vendorDestroyed.load(std::memory_order_acquire),
		"the bridge owner must pin the complete prepared arena after dispose");
	releaseBlockedWorker.store(true, std::memory_order_release);
	blockedHostWorker.join();
	check(asyncWorkerReadyResult == ASE_OK &&
		!vendorDestroyed.load(std::memory_order_acquire),
		"the late host write/outputReady must finish against pinned memory safely");
	currentProxy = nullptr;
	currentVendor = nullptr;
}

void testSuccessfulStopWithOutstandingWorkerQuarantinesPreparedArena()
{
	resetHarness();
	auto* driver = createInitializedDriver();
	auto infos = makeStereoDescriptors();
	currentProxy = driver;
	currentInfos = infos.data();
	check(driver->createBuffers(infos.data(), 4, 4, &hostCallbacks) == ASE_OK,
		"stop-quarantine buffers must prepare");
	fillHostOutputs(1);
	check(driver->start() == ASE_OK, "stop-quarantine stream must start");
	asyncHostMode = AsyncHostMode::BlockedWorker;
	currentVendor->triggerBuffer(0, ASIOFalse);
	check(blockedWorkerHasBuffer.load(std::memory_order_acquire),
		"deferred host worker must retain the proxy H pointer");
	check(driver->stop() == ASE_OK,
		"vendor stop may succeed while a host producer is still outstanding");
	check(driver->start() == ASE_InvalidMode,
		"successful stop cannot make an unindexed old producer safe to reuse");
	check(driver->disposeBuffers() == ASE_OK &&
		HibikiAsio::AsioCallbackBridge::isPoisoned(),
		"dispose must abandon and pin unresolved host-visible backing");
	check(driver->Release() == 1 && DllCanUnloadNow() == S_FALSE,
		"quarantined backing must keep the instance and module alive");
	releaseBlockedWorker.store(true, std::memory_order_release);
	blockedHostWorker.join();
	check(asyncWorkerReadyResult == ASE_OK &&
		!vendorDestroyed.load(std::memory_order_acquire),
		"late stopped worker must safely finish on process-lifetime backing");
	currentProxy = nullptr;
	currentVendor = nullptr;
}

void testInitializedUnsupportedProbeRetriesAfterCreate()
{
	resetHarness();
	fakeConfig.outputReadyResult = ASE_NotPresent;
	auto* driver = createInitializedDriver();
	check(driver->outputReady() == ASE_OK && currentVendor->outputReadyCount == 1,
		"pre-create unsupported probe must still be acknowledged");
	auto infos = makeStereoDescriptors();
	currentProxy = driver;
	currentInfos = infos.data();
	check(driver->createBuffers(infos.data(), 4, 4, &hostCallbacks) == ASE_OK,
		"post-probe buffers must prepare");
	currentVendor->config.outputReadyResult = ASE_OK;
	check(driver->outputReady() == ASE_OK && currentVendor->outputReadyCount == 2,
		"ASE_NotPresent before create must be retried once after buffers exist");
	driver->disposeBuffers();
	driver->Release();
}

void testBrokenVendorOutputReadyIsDisabledAfterFirstFailure()
{
	resetHarness();
	fakeConfig.throwOutputReady = true;
	auto* driver = createInitializedDriver();
	check(driver->outputReady() == ASE_OK && driver->outputReady() == ASE_OK &&
		currentVendor->outputReadyCount.load(std::memory_order_acquire) == 1,
		"throwing vendor outputReady must be attempted only once per init phase");
	char readyError[124]{};
	driver->getErrorMessage(readyError);
	check(std::strstr(readyError, "audio continues") != nullptr,
		"optional outputReady exceptions must not claim that audio was silenced");
	driver->Release();

	resetHarness();
	fakeConfig.outputReadyResult = ASE_HWMalfunction;
	driver = createInitializedDriver();
	auto infos = makeStereoDescriptors();
	currentProxy = driver;
	currentInfos = infos.data();
	check(driver->createBuffers(infos.data(), 4, 4, &hostCallbacks) == ASE_OK,
		"broken-outputReady buffers must prepare");
	check(driver->outputReady() == ASE_OK && driver->outputReady() == ASE_OK &&
		currentVendor->outputReadyCount.load(std::memory_order_acquire) == 1,
		"non-success vendor outputReady must be cached as unsupported");
	readyError[0] = '\0';
	driver->getErrorMessage(readyError);
	check(std::strstr(readyError, "audio continues") != nullptr,
		"optional outputReady failures must report continued audio accurately");
	driver->disposeBuffers();
	driver->Release();
}

void testTimeInfoAndDelegates()
{
	resetHarness();
	auto* driver = createInitializedDriver();
	long inputChannels = -1;
	long outputChannels = -1;
	check(driver->getChannels(&inputChannels, &outputChannels) == ASE_OK &&
		inputChannels == 4 && outputChannels == 2,
		"proxy must advertise only the first monitor pair it can create");
	ASIOChannelInfo hiddenOutputInfo{};
	hiddenOutputInfo.channel = 2;
	hiddenOutputInfo.isInput = ASIOFalse;
	check(driver->getChannelInfo(&hiddenOutputInfo) == ASE_InvalidParameter,
		"getChannelInfo must not expose an output beyond the advertised monitor pair");
	long inputLatency = 0;
	long outputLatency = 0;
	check(driver->getLatencies(&inputLatency, &outputLatency) == ASE_OK &&
		inputLatency == 17 && outputLatency == 27,
		"pre-create latency must assume the vendor preferred block size");
	check(driver->canSampleRate(48000.0) == ASE_OK &&
		driver->canSampleRate(44100.0) == ASE_NoClock,
		"sample-rate errors must propagate");
	long invalidClockSourceCount = -1;
	check(driver->getClockSources(nullptr, &invalidClockSourceCount) ==
			ASE_InvalidParameter && currentVendor->getClockSourcesCount == 0,
		"negative clock-source capacity must fail before reaching the vendor");
	check(driver->controlPanel() == ASE_OK && driver->future(1, nullptr) == ASE_SUCCESS,
		"controlPanel/future results must propagate");
	auto infos = makeStereoDescriptors();
	currentProxy = driver;
	currentInfos = infos.data();
	check(driver->createBuffers(infos.data(), 4, 4, &hostCallbacks) == ASE_OK,
		"time-info buffers must prepare");
	check(driver->start() == ASE_OK, "time-info start must succeed");
	eventCount = 0;
	ASIOTime time{};
	check(currentVendor->triggerTimeInfo(&time, 1) == &time,
		"time-info pointer must pass through the host");
	check(eventCount == 5 && events[0] == Event::VendorReady &&
		events[1] == Event::HostBegin && events[4] == Event::Dsp,
		"time-info path must commit the prior stage before handing off and staging the next one");
	driver->stop();
	driver->disposeBuffers();
	driver->Release();
}

void testDescriptorValidationAndDsdRejection()
{
	resetHarness();
	auto* driver = createInitializedDriver();
	auto infos = makeStereoDescriptors();
	ASIOCallbacks noBufferCallback = hostCallbacks;
	noBufferCallback.bufferSwitch = nullptr;
	check(driver->createBuffers(infos.data(), 4, 4, &noBufferCallback) ==
		ASE_InvalidParameter, "null host buffer callback must be rejected");
	check(driver->createBuffers(infos.data(), 4, 0, &hostCallbacks) ==
		ASE_InvalidParameter, "zero buffer size must be rejected");
	auto duplicate = infos;
	duplicate[3].channelNum = duplicate[2].channelNum;
	check(driver->createBuffers(duplicate.data(), 4, 4, &hostCallbacks) ==
		ASE_InvalidParameter, "duplicate output descriptors must be rejected");
	std::array<ASIOBufferInfo, 1> hiddenOutput{};
	hiddenOutput[0].isInput = ASIOFalse;
	hiddenOutput[0].channelNum = 2;
	check(driver->createBuffers(hiddenOutput.data(), 1, 4, &hostCallbacks) ==
		ASE_InvalidParameter,
		"createBuffers must reject outputs beyond the advertised monitor pair");
	std::array<ASIOBufferInfo, 3> threeOutputs{};
	for (long index = 0; index < 3; ++index)
	{
		threeOutputs[index].isInput = ASIOFalse;
		threeOutputs[index].channelNum = index;
	}
	check(driver->createBuffers(threeOutputs.data(), 3, 4, &hostCallbacks) ==
		ASE_InvalidParameter, "nonstandard multichannel layout must be rejected");
	driver->Release();

	resetHarness();
	fakeConfig.outputType = ASIOSTDSDInt8LSB1;
	driver = createInitializedDriver();
	std::array<ASIOBufferInfo, 1> dsd{};
	dsd[0].isInput = ASIOFalse;
	dsd[0].channelNum = 0;
	check(driver->createBuffers(dsd.data(), 1, 4, &hostCallbacks) ==
		ASE_InvalidParameter, "DSD must be rejected before vendor buffers start");
	check(currentVendor->createCount == 0,
		"DSD rejection must not partially create vendor buffers");
	driver->Release();
}

void testVendorMustPopulateEveryPrivateDescriptorBuffer()
{
	const std::array<std::array<long, 2>, 2> omissions = {{{0, 1}, {2, 0}}};
	for (const auto& omission : omissions)
	{
		resetHarness();
		fakeConfig.omittedBufferDescriptor = omission[0];
		fakeConfig.omittedBufferHalf = omission[1];
		auto* driver = createInitializedDriver();
		auto infos = makeStereoDescriptors();
		std::array<std::array<std::uint8_t, 32>, 8> callerCanaries{};
		for (auto& canary : callerCanaries)
			canary.fill(0xCC);
		std::array<void*, 8> originalPointers{};
		for (std::size_t descriptor = 0; descriptor < infos.size(); ++descriptor)
		{
			for (std::size_t half = 0; half < 2; ++half)
			{
				const std::size_t flat = descriptor * 2 + half;
				infos[descriptor].buffers[half] = callerCanaries[flat].data();
				originalPointers[flat] = infos[descriptor].buffers[half];
			}
		}
		check(driver->createBuffers(infos.data(), 4, 4, &hostCallbacks) ==
			ASE_InvalidParameter,
			"a missing vendor input/output half must fail closed");
		bool callerUntouched = true;
		for (std::size_t descriptor = 0; descriptor < infos.size(); ++descriptor)
		{
			for (std::size_t half = 0; half < 2; ++half)
			{
				const std::size_t flat = descriptor * 2 + half;
				callerUntouched = callerUntouched &&
					infos[descriptor].buffers[half] == originalPointers[flat];
				for (const std::uint8_t byte : callerCanaries[flat])
					callerUntouched = callerUntouched && byte == 0xCC;
			}
		}
		check(callerUntouched && currentVendor->disposeCount == 1,
			"private descriptor validation must be transactional and never write stale caller pointers");
		driver->Release();
	}
}

void testFailedDescriptorCleanupPinsVendorDescriptorArena()
{
	resetHarness();
	fakeConfig.omittedBufferDescriptor = 0;
	fakeConfig.omittedBufferHalf = 1;
	fakeConfig.disposeResult = ASE_InvalidMode;
	auto* driver = createInitializedDriver();
	auto infos = makeStereoDescriptors();
	currentProxy = driver;
	currentInfos = infos.data();
	check(driver->createBuffers(infos.data(), 4, 4, &hostCallbacks) ==
		ASE_InvalidParameter,
		"failed descriptor teardown must preserve the validation failure");
	ASIOBufferInfo* const retained = currentVendor->retainedInfos;
	check(retained != nullptr && currentVendor->retainedInfoCount == 4 &&
		retained[0].buffers[1] == nullptr && retained[2].buffers[0] != nullptr,
		"failed cleanup must retain the exact vendor descriptor arena");
	if (retained != nullptr)
	{
		const long saved = retained[2].channelNum;
		retained[2].channelNum = saved + 1;
		check(retained[2].channelNum == saved + 1,
			"pinned descriptor storage must remain readable and writable");
		retained[2].channelNum = saved;
	}
	check(driver->Release() == 1 &&
		HibikiAsio::AsioCallbackBridge::isPoisoned() &&
		DllCanUnloadNow() == S_FALSE &&
		!vendorDestroyed.load(std::memory_order_acquire),
		"failed vendor teardown must pin descriptors, vendor, proxy, and module");
	currentProxy = nullptr;
	currentVendor = nullptr;
}

void testDsdIoFormatNegotiationIsRejectedBeforeVendorMutation()
{
	resetHarness();
	auto* driver = createInitializedDriver();
	ASIOIoFormat format{};
	format.FormatType = kASIODSDFormat;
	check(driver->future(kAsioCanDoIoFormat, &format) == ASE_InvalidMode &&
		format.FormatType == kASIOFormatInvalid,
		"DSD capability queries must be rejected by the PCM-only proxy");
	format.FormatType = kASIODSDFormat;
	check(driver->future(kAsioSetIoFormat, &format) == ASE_InvalidMode &&
		currentVendor->futureCount == 0,
		"DSD mode changes must never reach the wrapped driver");
	format.FormatType = kASIOPCMFormat;
	check(driver->future(kAsioCanDoIoFormat, &format) == ASE_SUCCESS &&
		currentVendor->lastFutureSelector == kAsioCanDoIoFormat,
		"PCM io-format negotiation must still delegate to the wrapped driver");
	currentVendor->config.ioFormatOnGet = kASIODSDFormat;
	check(driver->future(kAsioGetIoFormat, &format) == ASE_InvalidMode &&
		format.FormatType == kASIOFormatInvalid,
		"a wrapped driver already in DSD mode must be surfaced as unsupported");
	currentVendor->config.futureResult = ASE_OK;
	format.FormatType = kASIOPCMFormat;
	check(driver->future(kAsioGetIoFormat, &format) == ASE_InvalidMode &&
		format.FormatType == kASIOFormatInvalid,
		"ASE_OK get-format responses must receive the same DSD post-check");
	currentVendor->config.futureResult = ASE_SUCCESS;
	currentVendor->config.ioFormatOnGet = kASIOPCMFormat;
	check(driver->future(kAsioGetIoFormat, &format) == ASE_SUCCESS &&
		format.FormatType == kASIOPCMFormat,
		"the current PCM mode must be reported normally");
	check(driver->future(kAsioGetIoFormat, nullptr) == ASE_InvalidParameter,
		"io-format selectors require an ASIOIoFormat payload");
	auto infos = makeStereoDescriptors();
	currentProxy = driver;
	currentInfos = infos.data();
	check(driver->createBuffers(infos.data(), 4, 4, &hostCallbacks) == ASE_OK,
		"I/O-format lifecycle buffers must prepare");
	const int futureCalls = currentVendor->futureCount;
	format.FormatType = kASIOPCMFormat;
	check(driver->future(kAsioSetIoFormat, &format) == ASE_InvalidMode &&
		currentVendor->futureCount == futureCalls,
		"prepared PCM format changes must not invalidate staged byte geometry");
	driver->disposeBuffers();
	driver->Release();
}

void testPartialFailureReleasesCallbackOwner()
{
	resetHarness();
	fakeConfig.createResult = ASE_HWMalfunction;
	auto* failed = createInitializedDriver();
	auto failedInfos = makeStereoDescriptors();
	check(failed->createBuffers(failedInfos.data(), 4, 4, &hostCallbacks) ==
		ASE_HWMalfunction, "vendor create failure must propagate");
	failed->Release();

	resetHarness();
	auto* retry = createInitializedDriver();
	auto retryInfos = makeStereoDescriptors();
	check(retry->createBuffers(retryInfos.data(), 4, 4, &hostCallbacks) == ASE_OK,
		"failed create must release the single callback owner");
	retry->disposeBuffers();
	retry->Release();
}

void testRecoverableCreateFailureDoesNotPoisonTheProcess()
{
	resetHarness();
	fakeConfig.createResult = ASE_InvalidMode;
	fakeConfig.disposeResult = ASE_InvalidMode;
	auto* driver = createInitializedDriver();
	auto infos = makeStereoDescriptors();
	check(driver->createBuffers(infos.data(), 4, 4, &hostCallbacks) ==
		ASE_InvalidMode,
		"unsupported buffer-size probe must propagate InvalidMode");
	check(currentVendor->disposeCount == 0,
		"a normal createBuffers failure must not invent a live-buffer teardown");
	check(!HibikiAsio::AsioCallbackBridge::isPoisoned(),
		"a recoverable createBuffers failure must not poison callback ownership");
	currentVendor->config.createResult = ASE_OK;
	check(driver->createBuffers(infos.data(), 4, 4, &hostCallbacks) == ASE_OK,
		"the same proxy instance must accept a valid retry after InvalidMode");
	check(driver->disposeBuffers() == ASE_InvalidMode,
		"the fake vendor's configured dispose result must still propagate");
	currentVendor->config.disposeResult = ASE_OK;
	check(driver->disposeBuffers() == ASE_OK,
		"a failed dispose may be retried after the vendor recovers");
	driver->Release();
}

void testStartErrorAndDspExceptionStayDry()
{
	resetHarness();
	fakeConfig.startResult = ASE_HWMalfunction;
	auto* driver = createInitializedDriver();
	auto infos = makeStereoDescriptors();
	currentProxy = driver;
	currentInfos = infos.data();
	check(driver->createBuffers(infos.data(), 4, 4, &hostCallbacks) == ASE_OK,
		"start-error buffers must prepare");
	check(driver->start() == ASE_HWMalfunction,
		"vendor start error must propagate without changing state");
	driver->disposeBuffers();
	driver->Release();

	resetHarness();
	dspThrows = true;
	driver = createInitializedDriver();
	infos = makeStereoDescriptors();
	currentProxy = driver;
	currentInfos = infos.data();
	check(driver->createBuffers(infos.data(), 4, 4, &hostCallbacks) == ASE_OK,
		"throwing DSP buffers must prepare");
	fillHostOutputs(1);
	check(driver->start() == ASE_OK, "throwing DSP start must succeed");
	currentVendor->triggerBuffer(0);
	check(std::abs(vendorOutputSample(0, 0) - 0.25) < 1e-6,
		"DSP exception must commit only the private raw pre-roll stage");
	check(currentVendor->outputReadyCount == 1,
		"DSP exception must still signal the completed vendor commit");
	char message[124]{};
	driver->getErrorMessage(message);
	check(std::strstr(message, "remains dry") != nullptr,
		"DSP exception must surface a fail-dry diagnostic");
	driver->stop();
	driver->disposeBuffers();
	driver->Release();
}

void testStartFailureDrainsCallbackBeforeResettingBuffers()
{
	resetHarness();
	fakeConfig.startResult = ASE_HWMalfunction;
	fakeConfig.callbackDuringStart = true;
	blockHostCallback.store(true, std::memory_order_release);
	auto* driver = createInitializedDriver();
	auto infos = makeStereoDescriptors();
	currentProxy = driver;
	currentInfos = infos.data();
	check(driver->createBuffers(infos.data(), 4, 4, &hostCallbacks) == ASE_OK,
		"start-callback boundary buffers must prepare");
	ASIOError startResult = ASE_OK;
	std::atomic<bool> startReturned{false};
	std::thread startThread([&]() {
		startResult = driver->start();
		startReturned.store(true, std::memory_order_release);
	});
	while (!hostCallbackEntered.load(std::memory_order_acquire) ||
		!vendorStartReturned.load(std::memory_order_acquire))
	{
		std::this_thread::yield();
	}
	check(!startReturned.load(std::memory_order_acquire),
		"failed start must wait for a callback already launched by the vendor");
	releaseHostCallback.store(true, std::memory_order_release);
	startThread.join();
	check(startResult == ASE_HWMalfunction,
		"start failure after a callback must propagate only after callback drain");
	check(driver->disposeBuffers() == ASE_OK,
		"prepared buffers must remain safely disposable after failed start");
	driver->Release();
}

void testStopDrainsEnteredCallbackBeforeCallingVendor()
{
	resetHarness();
	auto* driver = createInitializedDriver();
	auto infos = makeStereoDescriptors();
	currentProxy = driver;
	currentInfos = infos.data();
	check(driver->createBuffers(infos.data(), 4, 4, &hostCallbacks) == ASE_OK &&
		driver->start() == ASE_OK,
		"stop/callback boundary stream must start");
	eventCount = 0;
	blockHostCallback.store(true, std::memory_order_release);
	std::thread callbackThread([&]() { currentVendor->triggerBuffer(0, ASIOTrue); });
	while (!hostCallbackEntered.load(std::memory_order_acquire))
		std::this_thread::yield();
	std::atomic<bool> stopEntered{false};
	std::atomic<bool> stopReturned{false};
	ASIOError stopResult = ASE_HWMalfunction;
	std::thread stopThread([&]() {
		stopEntered.store(true, std::memory_order_release);
		stopResult = driver->stop();
		stopReturned.store(true, std::memory_order_release);
	});
	while (!stopEntered.load(std::memory_order_acquire))
		std::this_thread::yield();
	std::this_thread::sleep_for(std::chrono::milliseconds(10));
	check(currentVendor->stopCount.load(std::memory_order_acquire) == 0 &&
		!stopReturned.load(std::memory_order_acquire),
		"stop must not enter the vendor while a pre-existing callback is active");
	releaseHostCallback.store(true, std::memory_order_release);
	callbackThread.join();
	stopThread.join();
	check(stopResult == ASE_OK &&
		currentVendor->stopCount.load(std::memory_order_acquire) == 1,
		"vendor stop must run after the callback reaches quiescence");
	std::size_t readyPosition = eventCount;
	std::size_t stopPosition = eventCount;
	for (std::size_t index = 0; index < eventCount; ++index)
	{
		if (events[index] == Event::VendorReady && readyPosition == eventCount)
			readyPosition = index;
		if (events[index] == Event::VendorStop && stopPosition == eventCount)
			stopPosition = index;
	}
	check(readyPosition < stopPosition,
		"entered callback completion must finish before vendor stop");
	driver->disposeBuffers();
	driver->Release();
}

void testDuplicateCrossThreadReadyDoesNotAdvanceTheVendor()
{
	resetHarness();
	auto* driver = createInitializedDriver();
	auto infos = makeStereoDescriptors();
	currentProxy = driver;
	currentInfos = infos.data();
	check(driver->createBuffers(infos.data(), 4, 4, &hostCallbacks) == ASE_OK &&
		driver->start() == ASE_OK,
		"duplicate cross-thread ready stream must start");
	asyncHostMode = AsyncHostMode::LateWorker;
	currentVendor->triggerBuffer(0, ASIOFalse);
	fillHostOutputs(0);
	check(driver->outputReady() == ASE_OK &&
		currentVendor->outputReadyCount == 1,
		"one queued host completion must advance a supported vendor once");
	check(driver->outputReady() == ASE_OK &&
		currentVendor->outputReadyCount == 1,
		"duplicate outputReady without a queued callback credit must be suppressed");
	const int hostCount = hostBufferCallbackCount.load(std::memory_order_acquire);
	std::array<double, 4> stale{};
	stale.fill(0.75);
	check(HibikiAsio::AsioSampleCodec::encode(
		stale.data(), HibikiAsio::AsioSampleCodec::describe(fakeConfig.outputType),
		currentVendor->buffer(2, 1), stale.size()),
		"duplicate regression must seed a stale vendor half");
	currentVendor->triggerBuffer(1, ASIOFalse);
	check(std::abs(vendorOutputSample(0, 1)) < 1e-12 &&
		hostBufferCallbackCount.load(std::memory_order_acquire) == hostCount &&
		currentVendor->outputReadyCount == 2,
		"empty-token duplicate must make the next safe callback zero-only with no host handoff");
	driver->stop();
	driver->disposeBuffers();
	driver->Release();
}

void testHostThrowAfterReadyStillSignalsVendorAndStaysDry()
{
	resetHarness();
	auto* driver = createInitializedDriver();
	auto infos = makeStereoDescriptors();
	currentProxy = driver;
	currentInfos = infos.data();
	check(driver->createBuffers(infos.data(), 4, 4, &hostCallbacks) == ASE_OK,
		"host-throw buffers must prepare");
	fillHostOutputs(1);
	check(driver->start() == ASE_OK, "host-throw start must succeed");
	hostThrowsAfterReady = true;
	currentVendor->triggerBuffer(0, ASIOFalse);
	check(currentVendor->outputReadyCount == 1,
		"the prior staged vendor block must still commit before a host throw");
	check(dspProcessCount.load(std::memory_order_acquire) == 1 &&
		std::abs(static_cast<float*>(infos[2].buffers[0])[0] - 0.25F) < 1e-6F,
		"throwing host callback must discard its H block without additional DSP");
	driver->stop();
	driver->disposeBuffers();
	driver->Release();

	resetHarness();
	driver = createInitializedDriver();
	infos = makeStereoDescriptors();
	currentProxy = driver;
	currentInfos = infos.data();
	check(driver->createBuffers(infos.data(), 4, 4, &hostCallbacks) == ASE_OK,
		"direct host-throw buffers must prepare");
	fillHostOutputs(1);
	check(driver->start() == ASE_OK, "direct host-throw start must succeed");
	const int dspBeforeThrow = dspProcessCount.load(std::memory_order_acquire);
	hostThrowsBeforeReady = true;
	currentVendor->triggerBuffer(0, ASIOTrue);
	hostThrowsBeforeReady = false;
	currentVendor->triggerBuffer(1, ASIOTrue);
	check(dspProcessCount.load(std::memory_order_acquire) == dspBeforeThrow &&
		std::abs(static_cast<float*>(infos[2].buffers[1])[0] - 0.25F) < 1e-6F,
		"direct host exception must keep later callbacks dry until restart");
	check(driver->stop() == ASE_OK && driver->start() == ASE_OK,
		"direct host exception desynchronization must reset on restart");
	const int dspAfterRestartPreroll =
		dspProcessCount.load(std::memory_order_acquire);
	currentVendor->triggerBuffer(1, ASIOTrue);
	check(dspProcessCount.load(std::memory_order_acquire) ==
		dspAfterRestartPreroll + 1,
		"direct processing must recover only after restart");
	driver->stop();
	driver->disposeBuffers();
	driver->Release();
}

void testDeferredHostThrowWithWorkerPinsTheArena()
{
	resetHarness();
	auto* driver = createInitializedDriver();
	auto infos = makeStereoDescriptors();
	currentProxy = driver;
	currentInfos = infos.data();
	check(driver->createBuffers(infos.data(), 4, 4, &hostCallbacks) == ASE_OK,
		"throwing-worker quarantine buffers must prepare");
	fillHostOutputs(1);
	check(driver->start() == ASE_OK,
		"throwing-worker quarantine stream must start");
	asyncHostMode = AsyncHostMode::BlockedWorker;
	hostThrowsBeforeReady = true;
	currentVendor->triggerBuffer(0, ASIOFalse);
	hostThrowsBeforeReady = false;
	check(blockedWorkerHasBuffer.load(std::memory_order_acquire),
		"throwing deferred callback may leave a worker holding the proxy H pointer");
	check(driver->stop() == ASE_OK && driver->start() == ASE_InvalidMode,
		"callback unwind without outputReady proof must preserve unresolved ownership");
	check(driver->disposeBuffers() == ASE_OK &&
		HibikiAsio::AsioCallbackBridge::isPoisoned(),
		"disposing an unresolved throwing worker must quarantine the full arena");
	check(driver->Release() == 1 && DllCanUnloadNow() == S_FALSE,
		"throwing-worker arena must keep its instance and module pinned");
	releaseBlockedWorker.store(true, std::memory_order_release);
	blockedHostWorker.join();
	check(asyncWorkerReadyResult == ASE_OK &&
		!vendorDestroyed.load(std::memory_order_acquire),
		"late worker write and outputReady must remain safe after quarantined dispose");
	currentProxy = nullptr;
	currentVendor = nullptr;
}

void testDirectTimeInfoReentrancyDesynchronizesUntilRestart()
{
	resetHarness();
	auto* driver = createInitializedDriver();
	auto infos = makeStereoDescriptors();
	currentProxy = driver;
	currentInfos = infos.data();
	check(driver->createBuffers(infos.data(), 4, 4, &hostCallbacks) == ASE_OK,
		"direct reentrancy buffers must prepare");
	fillHostOutputs(1);
	check(driver->start() == ASE_OK, "direct reentrancy start must succeed");
	hostReentersDirect = true;
	ASIOTime time{};
	currentVendor->triggerTimeInfo(&time, 0, ASIOTrue);
	const int processedBeforeLaterCallback =
		dspProcessCount.load(std::memory_order_acquire);
	currentVendor->triggerBuffer(1, ASIOTrue);
	check(dspProcessCount.load(std::memory_order_acquire) ==
		processedBeforeLaterCallback &&
		std::abs(static_cast<float*>(infos[2].buffers[1])[0] - 0.25F) < 1e-6F,
		"direct generation mismatch must keep later callbacks fail-dry");
	check(driver->stop() == ASE_OK && driver->start() == ASE_OK,
		"direct generation mismatch must clear only at a stream boundary");
	const int processedAfterRestartPreroll =
		dspProcessCount.load(std::memory_order_acquire);
	currentVendor->triggerBuffer(1, ASIOTrue);
	check(dspProcessCount.load(std::memory_order_acquire) ==
		processedAfterRestartPreroll + 1,
		"time-info path must resume correction after restart");
	driver->stop();
	driver->disposeBuffers();
	driver->Release();
}

void testResetAcceptanceAndSampleRateLifecycle()
{
	resetHarness();
	auto* driver = createInitializedDriver();
	auto infos = makeStereoDescriptors();
	currentProxy = driver;
	currentInfos = infos.data();
	check(driver->createBuffers(infos.data(), 4, 4, &hostCallbacks) == ASE_OK,
		"reset-message buffers must prepare");
	check(driver->start() == ASE_OK, "reset-message start must succeed");
	const int beforeRejectedResetCallback =
		dspProcessCount.load(std::memory_order_acquire);
	hostMessageResult = 0;
	check(currentVendor->triggerMessage(kAsioResetRequest) == 0,
		"host rejection of reset must be preserved");
	currentVendor->triggerBuffer(0, ASIOTrue);
	check(dspProcessCount.load(std::memory_order_acquire) ==
		beforeRejectedResetCallback + 1,
		"rejected reset must not disable correction on the live stream");
	const int beforeAcceptedResetCallback =
		dspProcessCount.load(std::memory_order_acquire);
	hostMessageResult = 1;
	check(currentVendor->triggerMessage(kAsioBufferSizeChange) == 1,
		"accepted buffer-size request must be preserved");
	currentVendor->triggerBuffer(1, ASIOTrue);
	check(dspProcessCount.load(std::memory_order_acquire) ==
		beforeAcceptedResetCallback &&
		std::abs(static_cast<float*>(infos[2].buffers[1])[0] - 0.25F) < 1e-6F,
		"accepted reset-class request must make subsequent output fail dry");
	check(driver->setSampleRate(48000.0) == ASE_InvalidMode &&
		currentVendor->setSampleRateCount == 0,
		"sample rate changes with live buffers must be refused, not silently dry DSP");
	driver->stop();
	driver->disposeBuffers();
	check(driver->setSampleRate(48000.0) == ASE_OK &&
		currentVendor->setSampleRateCount == 1,
		"sample rate changes must delegate after buffers are disposed");
	driver->Release();
}

void testRestartRefreshesSampleRateAfterExternalClockChange()
{
	resetHarness();
	auto* driver = createInitializedDriver();
	auto infos = makeStereoDescriptors();
	currentProxy = driver;
	currentInfos = infos.data();
	check(driver->createBuffers(infos.data(), 4, 4, &hostCallbacks) == ASE_OK &&
		lastDspSampleRate == 48000.0,
		"clock-change test must prepare DSP at the initial hardware rate");
	currentVendor->config.sampleRate = 44100.0;
	currentVendor->config.sampleRateCallbackDuringQuery = true;
	check(driver->start() == ASE_NoClock && currentVendor->startCount == 0,
		"a sample-rate callback during the closed start window must abort stale DSP startup");
	driver->setStartGateHookForTest([]() noexcept {
		currentVendor->triggerSampleRateDidChange(
			currentVendor->config.sampleRate);
	});
	check(driver->start() == ASE_NoClock && currentVendor->startCount == 0,
		"a sample-rate callback between the final check and gate reopen must abort startup");
	driver->setStartGateHookForTest(nullptr);
	check(driver->start() == ASE_OK && lastDspSampleRate == 44100.0,
		"retry must refresh and rebuild at the stable hardware rate");
	currentVendor->config.sampleRate = 96000.0;
	currentVendor->triggerSampleRateDidChange(96000.0);
	check(driver->stop() == ASE_OK && driver->start() == ASE_OK,
		"external clock change must permit a clean stop/start boundary");
	check(lastDspSampleRate == 96000.0 && currentVendor->getSampleRateCount == 5,
		"restart must query the vendor again and rebuild DSP at the new clock rate");
	driver->stop();
	driver->disposeBuffers();
	driver->Release();
}

void testNonBufferCallbacksRejectLifecycleReentry()
{
	resetHarness();
	auto* driver = createInitializedDriver();
	auto infos = makeStereoDescriptors();
	currentProxy = driver;
	currentInfos = infos.data();
	check(driver->createBuffers(infos.data(), 4, 4, &hostCallbacks) == ASE_OK,
		"non-buffer callback lifecycle test must prepare buffers");

	disposeDuringMessage = true;
	currentVendor->triggerMessage(kAsioResetRequest);
	disposeDuringMessage = false;
	check(reentrantMessageDisposeResult == ASE_InvalidMode &&
		currentVendor->disposeCount == 0,
		"asioMessage must reject reentrant dispose instead of self-draining");

	disposeDuringSampleRate = true;
	currentVendor->triggerSampleRateDidChange(44100.0);
	disposeDuringSampleRate = false;
	check(reentrantSampleRateDisposeResult == ASE_InvalidMode &&
		currentVendor->disposeCount == 0,
		"sampleRateDidChange must reject reentrant dispose instead of self-draining");
	check(driver->disposeBuffers() == ASE_OK,
		"dispose must still succeed after both callbacks have returned");
	driver->Release();
}

void testFinalReleaseInsideMessageIsDeferredPastTheTrampoline()
{
	resetHarness();
	auto* driver = createInitializedDriver();
	auto infos = makeStereoDescriptors();
	currentProxy = driver;
	currentInfos = infos.data();
	check(driver->createBuffers(infos.data(), 4, 4, &hostCallbacks) == ASE_OK,
		"callback lifetime test must prepare buffers");
	ASIOCallbacks* const retainedCallbacks = currentVendor->callbacks;
	const long instancesBefore = AsioProxyDriver::liveInstanceCount();
	releaseDuringMessage = true;
	if (retainedCallbacks != nullptr)
		retainedCallbacks->asioMessage(0x7FFF, 0, nullptr, nullptr);
	check(reentrantReleaseRemaining == 1,
		"static callback bridge must hold one lifetime reference during host callback");
	check(AsioProxyDriver::liveInstanceCount() == instancesBefore &&
		!vendorDestroyed.load(std::memory_order_acquire),
		"final in-callback Release must not reenter vendor teardown");
	check(HibikiAsio::AsioCallbackBridge::isPoisoned() &&
		DllCanUnloadNow() == S_FALSE,
		"abandoned in-callback Release must poison dispatch and pin the module");
	currentProxy = nullptr;
	currentVendor = nullptr;
}

void testFinalExternalReleaseCannotRaceAnInFlightCallback()
{
	resetHarness();
	auto* driver = createInitializedDriver();
	auto infos = makeStereoDescriptors();
	currentProxy = driver;
	currentInfos = infos.data();
	check(driver->createBuffers(infos.data(), 4, 4, &hostCallbacks) == ASE_OK,
		"cross-thread callback lifetime test must prepare buffers");
	fillHostOutputs(1);
	check(driver->start() == ASE_OK,
		"cross-thread callback lifetime test must enter the running state");
	ASIOCallbacks* const retainedCallbacks = currentVendor->callbacks;
	const long instancesBefore = AsioProxyDriver::liveInstanceCount();
	blockHostCallback.store(true, std::memory_order_release);
	std::thread callbackThread([&]() {
		if (retainedCallbacks != nullptr)
			retainedCallbacks->bufferSwitch(0, ASIOTrue);
	});
	while (!hostCallbackEntered.load(std::memory_order_acquire))
		std::this_thread::yield();
	const ULONG remaining = driver->Release();
	check(remaining == 1 &&
		AsioProxyDriver::liveInstanceCount() == instancesBefore &&
		!vendorDestroyed.load(std::memory_order_acquire),
		"bridge owner reference must prevent cross-thread final Release UAF");
	check(HibikiAsio::AsioCallbackBridge::isPoisoned() &&
		DllCanUnloadNow() == S_FALSE,
		"last external Release with active buffers must close dispatch and pin DLL");
	releaseHostCallback.store(true, std::memory_order_release);
	callbackThread.join();
	check(AsioProxyDriver::liveInstanceCount() == instancesBefore &&
		!vendorDestroyed.load(std::memory_order_acquire),
		"callback return must not run destructor or reenter vendor teardown");
	currentProxy = nullptr;
	currentVendor = nullptr;
}

void runIsolatedLifetimeTest(const wchar_t* argument, const char* failureMessage)
{
	wchar_t executable[MAX_PATH]{};
	const DWORD length = GetModuleFileNameW(nullptr, executable, MAX_PATH);
	if (length == 0 || length >= MAX_PATH)
	{
		check(false, "final-release subprocess must resolve the test executable");
		return;
	}
	std::wstring command = L"\"" + std::wstring(executable, length) +
		L"\" " + argument;
	std::vector<wchar_t> commandLine(command.begin(), command.end());
	commandLine.push_back(L'\0');
	STARTUPINFOW startup{};
	startup.cb = sizeof(startup);
	PROCESS_INFORMATION process{};
	const BOOL created = CreateProcessW(
		executable, commandLine.data(), nullptr, nullptr, FALSE, 0,
		nullptr, nullptr, &startup, &process);
	if (created == FALSE)
	{
		check(false, "final-release subprocess must start");
		return;
	}
	WaitForSingleObject(process.hProcess, INFINITE);
	DWORD exitCode = 1;
	GetExitCodeProcess(process.hProcess, &exitCode);
	CloseHandle(process.hThread);
	CloseHandle(process.hProcess);
	check(exitCode == 0, failureMessage);

}

void testFinalReleaseSafetyInIsolatedProcesses()
{
	runIsolatedLifetimeTest(
		L"--final-release",
		"isolated in-callback final-release safety test must pass");
	runIsolatedLifetimeTest(
		L"--cross-thread-final-release",
		"isolated cross-thread final-release safety test must pass");
	runIsolatedLifetimeTest(
		L"--failed-start-quarantine",
		"failed-start host-buffer quarantine subprocess must pass");
	runIsolatedLifetimeTest(
		L"--stopped-worker-quarantine",
		"successful-stop host-buffer quarantine subprocess must pass");
	runIsolatedLifetimeTest(
		L"--throwing-worker-quarantine",
		"throwing deferred host worker quarantine subprocess must pass");
	runIsolatedLifetimeTest(
		L"--failed-descriptor-pin",
		"failed vendor descriptor cleanup must pin its arena in a subprocess");
}

void testVendorCppExceptionsDoNotEscapeTheAsioAbi()
{
	resetHarness();
	fakeActivatorThrows = true;
	auto* driver = new AsioProxyDriver(
		nullptr, &fakeResolver, &fakeActivator, &fakeDspFactory);
	check(driver->init(nullptr) == ASIOFalse,
		"activator exception must become an init failure");
	driver->Release();

	resetHarness();
	fakeConfig.throwInit = true;
	driver = new AsioProxyDriver(
		nullptr, &fakeResolver, &fakeActivator, &fakeDspFactory);
	check(driver->init(nullptr) == ASIOFalse,
		"vendor init exception must not cross the ASIO ABI");
	driver->Release();

	resetHarness();
	fakeConfig.throwCreate = true;
	driver = createInitializedDriver();
	auto infos = makeStereoDescriptors();
	check(driver->createBuffers(infos.data(), 4, 4, &hostCallbacks) ==
		ASE_HWMalfunction,
		"vendor create exception must become ASE_HWMalfunction");
	driver->Release();

	resetHarness();
	fakeConfig.throwStart = true;
	driver = createInitializedDriver();
	infos = makeStereoDescriptors();
	check(driver->createBuffers(infos.data(), 4, 4, &hostCallbacks) == ASE_OK,
		"throwing-start buffers must prepare");
	check(driver->start() == ASE_HWMalfunction,
		"vendor start exception must become ASE_HWMalfunction");
	driver->disposeBuffers();
	driver->Release();

	resetHarness();
	driver = createInitializedDriver();
	check(driver != nullptr,
		"exception paths must release callback and init guards for a retry");
	driver->Release();
}

void testFailedTeardownPoisonsStaticCallbacksAndBlocksUnload()
{
	resetHarness();
	fakeConfig.throwStop = true;
	fakeConfig.throwDispose = true;
	fakeConfig.throwRelease = true;
	auto* driver = createInitializedDriver();
	auto infos = makeStereoDescriptors();
	currentProxy = driver;
	currentInfos = infos.data();
	check(driver->createBuffers(infos.data(), 4, 4, &hostCallbacks) == ASE_OK,
		"poison test buffers must prepare");
	check(driver->start() == ASE_OK, "poison test start must succeed");
	ASIOCallbacks* const retainedCallbacks = currentVendor->callbacks;
	driver->Release();
	check(HibikiAsio::AsioCallbackBridge::isPoisoned(),
		"failed vendor teardown must permanently poison callback ownership");
	check(DllCanUnloadNow() == S_FALSE,
		"poisoned late-callback protection must keep the COM server loaded");
	if (retainedCallbacks != nullptr)
		retainedCallbacks->bufferSwitch(0, ASIOTrue);
	resetHarness();
	auto* replacement = createInitializedDriver();
	auto replacementInfos = makeStereoDescriptors();
	check(replacement->createBuffers(
		replacementInfos.data(), 4, 4, &hostCallbacks) == ASE_InvalidMode,
		"poisoned callback owner must never route a late callback to a new proxy");
	replacement->Release();
}

void testClassFactoryKeepsServerLoadedAndLockDoesNotUnderflow()
{
	check(DllCanUnloadNow() == S_OK,
		"idle COM server must initially be unloadable");
	IClassFactory* factory = nullptr;
	check(DllGetClassObject(
		HibikiAsio::kDriverClsid, IID_IClassFactory,
		reinterpret_cast<void**>(&factory)) == S_OK && factory != nullptr,
		"DllGetClassObject must return the ASIO class factory");
	check(DllCanUnloadNow() == S_FALSE,
		"a live class factory must keep the COM server loaded");
	if (factory != nullptr)
	{
		check(factory->LockServer(FALSE) == S_OK,
			"unlocking an already-unlocked server must be harmless");
		check(DllCanUnloadNow() == S_FALSE,
			"unlock underflow must not make the live factory unloadable");
		check(factory->LockServer(TRUE) == S_OK,
			"class factory must accept a server lock");
		check(factory->LockServer(FALSE) == S_OK,
			"class factory must release a server lock");
		factory->Release();
	}
	check(DllCanUnloadNow() == S_OK,
		"releasing the last class factory must make the server unloadable");
}

bool writeConfigFile(const wchar_t* path, const char* contents)
{
	HANDLE file = CreateFileW(
		path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
		FILE_ATTRIBUTE_TEMPORARY, nullptr);
	if (file == INVALID_HANDLE_VALUE)
		return false;
	const DWORD byteCount = static_cast<DWORD>(std::strlen(contents));
	DWORD written = 0;
	const bool wrote = WriteFile(
		file, contents, byteCount, &written, nullptr) != FALSE &&
		written == byteCount;
	CloseHandle(file);
	return wrote;
}

bool runAsioPolicyConfig(
	const char* contents,
	bool& rejected,
	bool& active)
{
	wchar_t tempDirectory[MAX_PATH]{};
	wchar_t tempPath[MAX_PATH]{};
	if (GetTempPathW(MAX_PATH, tempDirectory) == 0 ||
		GetTempFileNameW(tempDirectory, L"HAQ", 0, tempPath) == 0)
	{
		return false;
	}
	const bool wrote = writeConfigFile(tempPath, contents);
	if (wrote)
	{
		FilterEngine engine;
		engine.setProcessingPolicy(
			FilterEngine::ProcessingPolicy::AsioCallbackSafe);
		engine.setDeviceInfo(
			false, true, L"Hibiki EQAPO", L"ASIO", L"", L"Hibiki EQAPO");
		engine.initialize(48000.0F, 2, 2, 2, 0x3, 4, tempPath);
		rejected = engine.rejectedUnsafeConfiguration();
		active = engine.hasActiveConfiguration();
	}
	DeleteFileW(tempPath);
	return wrote;
}

void testAsioFilterPolicyHonorsInactiveScopesAndRejectsActiveUnsafeFilters()
{
	bool rejected = true;
	bool active = false;
	check(runAsioPolicyConfig(
		"Device: Definitely Not Hibiki EQAPO\r\n"
		"VSTPlugin: Library \\\"missing.dll\\\"\r\n"
		"Device: Hibiki EQAPO\r\n"
		"Preamp: -6 dB\r\n",
		rejected, active) && !rejected && active,
		"unsafe filters in an inactive Device scope must not reject the ASIO config");

	rejected = true;
	active = false;
	check(runAsioPolicyConfig(
		"If: 0\r\n"
		"OutProcGain: 6 dB\r\n"
		"EndIf:\r\n"
		"Preamp: -6 dB\r\n",
		rejected, active) && !rejected && active,
		"unsafe filters in a false If branch must not reject the ASIO config");

	rejected = true;
	active = false;
	check(runAsioPolicyConfig(
		"Device: Hibiki EQAPO\r\n"
		"LoudnessCorrection: Schema 1 Model FormulaLoudnessV1 Binding All "
		"State 1 ReferenceLevel 80 ReferenceOffset 0 Attenuation 1.0 Volume -38\r\n",
		rejected, active) && !rejected && active,
		"ASIO must allow formula loudness only with an explicit fixed Volume");

	rejected = true;
	active = false;
	check(runAsioPolicyConfig(
		"Device: Hibiki EQAPO\r\n"
		"LoudnessCorrection: Schema 1 Model FormulaLoudnessV1 Binding All "
		"State 0 ReferenceLevel 80 ReferenceOffset 0 Attenuation 1.0\r\n"
		"LoudnessCorrectionOriginal: Schema 1 Model MixomoShelfV1 State 0 "
		"ReferenceLevel 80 ReferenceOffset 0 Attenuation 1.0\r\n",
		rejected, active) && !rejected && active,
		"disabled endpoint-bound loudness commands must remain inert and allowed");

	for (const char* command : {
		"VSTPlugin: Library \\\"missing.dll\\\"\r\n",
		"OutProcVSTPlugin: Library \\\"missing.dll\\\"\r\n",
		"OutProcGain: 6 dB\r\n",
		"OutProcBiquad: PK Fc 1000 Hz Gain 3 dB Q 1\r\n",
		"VUMeter: all\r\n",
		"LoudnessCorrectionOriginal: Schema 1 Model MixomoShelfV1 State 1 "
			"ReferenceLevel 80 ReferenceOffset 0 Attenuation 1.0\r\n",
		"LoudnessCorrection: Schema 1 Model FormulaLoudnessV1 Binding All "
			"State 1 ReferenceLevel 80 ReferenceOffset 0 Attenuation 1.0\r\n"})
	{
		const std::string config =
			std::string("Device: Hibiki EQAPO\r\n") + command;
		rejected = false;
		active = true;
		check(runAsioPolicyConfig(config.c_str(), rejected, active) &&
			rejected && !active,
			"active plug-in/out-of-process commands must fail the ASIO config closed");
	}
}

void testUnsafeAsioReloadPreservesThePublishedSafeConfiguration()
{
	wchar_t tempDirectory[MAX_PATH]{};
	wchar_t tempPath[MAX_PATH]{};
	if (GetTempPathW(MAX_PATH, tempDirectory) == 0 ||
		GetTempFileNameW(tempDirectory, L"HAR", 0, tempPath) == 0)
	{
		check(false, "reload policy test must create a temporary configuration");
		return;
	}
	const char* const safeConfig =
		"Device: Hibiki EQAPO\r\nPreamp: -6 dB\r\n";
	if (!writeConfigFile(tempPath, safeConfig))
	{
		DeleteFileW(tempPath);
		check(false, "reload policy test must write its safe configuration");
		return;
	}

	FilterEngine engine;
	engine.setProcessingPolicy(FilterEngine::ProcessingPolicy::AsioCallbackSafe);
	engine.setDeviceInfo(
		false, true, L"Hibiki EQAPO", L"ASIO", L"", L"Hibiki EQAPO");
	engine.initialize(48000.0F, 2, 2, 2, 0x3, 4, tempPath);
	std::array<double, 4> inputLeft{1.0, 1.0, 1.0, 1.0};
	std::array<double, 4> inputRight{1.0, 1.0, 1.0, 1.0};
	std::array<double, 4> outputLeft{};
	std::array<double, 4> outputRight{};
	double* input[] = {inputLeft.data(), inputRight.data()};
	double* output[] = {outputLeft.data(), outputRight.data()};
	engine.process(output, input, 4);
	const double safeOutput = outputLeft[0];

	const char* const unsafeConfig =
		"Device: Hibiki EQAPO\r\nOutProcGain: 6 dB\r\n";
	const bool wroteUnsafe = writeConfigFile(tempPath, unsafeConfig);
	const bool queuedReload = wroteUnsafe && engine.loadConfig(tempPath);
	outputLeft.fill(0.0);
	outputRight.fill(0.0);
	engine.process(output, input, 4);
	check(wroteUnsafe && !queuedReload && engine.rejectedUnsafeConfiguration() &&
		engine.hasActiveConfiguration() &&
		std::abs(outputLeft[0] - safeOutput) < 1e-12,
		"unsafe ASIO reload must leave the previously published safe config active");
	DeleteFileW(tempPath);
}
}

namespace HibikiAsio
{
ResolvedAsioTarget resolveAsioTarget(HMODULE module)
{
	return fakeResolver(module);
}

std::unique_ptr<IAsioDsp> createDefaultAsioDsp(
	double sampleRate, unsigned channelCount, unsigned maxFrames)
{
	return fakeDspFactory(sampleRate, channelCount, maxFrames);
}
}

int main(int argc, char** argv)
{
	if (argc == 2 && std::strcmp(argv[1], "--final-release") == 0)
	{
		testFinalReleaseInsideMessageIsDeferredPastTheTrampoline();
		return failures == 0 ? 0 : 1;
	}
	if (argc == 2 && std::strcmp(argv[1], "--cross-thread-final-release") == 0)
	{
		testFinalExternalReleaseCannotRaceAnInFlightCallback();
		return failures == 0 ? 0 : 1;
	}
	if (argc == 2 && std::strcmp(argv[1], "--failed-start-quarantine") == 0)
	{
		testFailedStartWithOutstandingWorkerRequiresRecreate();
		return failures == 0 ? 0 : 1;
	}
	if (argc == 2 && std::strcmp(argv[1], "--stopped-worker-quarantine") == 0)
	{
		testSuccessfulStopWithOutstandingWorkerQuarantinesPreparedArena();
		return failures == 0 ? 0 : 1;
	}
	if (argc == 2 && std::strcmp(argv[1], "--throwing-worker-quarantine") == 0)
	{
		testDeferredHostThrowWithWorkerPinsTheArena();
		return failures == 0 ? 0 : 1;
	}
	if (argc == 2 && std::strcmp(argv[1], "--failed-descriptor-pin") == 0)
	{
		testFailedDescriptorCleanupPinsVendorDescriptorArena();
		return failures == 0 ? 0 : 1;
	}
#define RUN_TEST(test) do { std::fprintf(stderr, "RUN: %s\n", #test); test(); } while (false)
	RUN_TEST(testClassFactoryKeepsServerLoadedAndLockDoesNotUnderflow);
	RUN_TEST(testAsioFilterPolicyHonorsInactiveScopesAndRejectsActiveUnsafeFilters);
	RUN_TEST(testUnsafeAsioReloadPreservesThePublishedSafeConfiguration);
	RUN_TEST(testClsidQueryAndActivation);
	RUN_TEST(testIndirectProxyInitializationRecursionIsBlocked);
	RUN_TEST(testIndependentThreadsMayInitializeInParallel);
	RUN_TEST(testOneBlockDirectStagingAndLatencyAccounting);
	RUN_TEST(testPreCreateLatencyFallbackAndSaturation);
	RUN_TEST(testAdvertisedMonitorPairHandlesVendorCountsTransactionally);
	RUN_TEST(testOutputReadyProbeDoesNotConsumePrefill);
	RUN_TEST(testCreateBuffersRejectsNegotiationReentryTransactionally);
	RUN_TEST(testPreparedAndFailedDisposeCallbacksCannotEnterThePipeline);
	RUN_TEST(testDeferredWorkerStagingAndVendorReadyCapability);
	RUN_TEST(testDuplicateCrossThreadReadyDoesNotAdvanceTheVendor);
	RUN_TEST(testInlineFalseCompletionStillAppliesCorrection);
	RUN_TEST(testStartSynchronousDeferredCompletionAndFailureRetry);
	RUN_TEST(testLateWorkerDeadlineAndHostHalfReuseAreSafe);
	RUN_TEST(testSampleRateChangeInvalidatesReadyStage);
	RUN_TEST(testTimeInfoReturnPointerIsTransparent);
	RUN_TEST(testTimeInfoAndDelegates);
	RUN_TEST(testInputOnlyCallbacksDoNotEnterStaging);
	RUN_TEST(testStagingSupportsWideAndPackedPcmWithoutOverrun);
	RUN_TEST(testRecursiveCallbackSilencesStaleVendorHalf);
	RUN_TEST(testFailedStopRemainsTerminalUntilSuccessfulBoundary);
	RUN_TEST(testInitializedUnsupportedProbeRetriesAfterCreate);
	RUN_TEST(testBrokenVendorOutputReadyIsDisabledAfterFirstFailure);
	RUN_TEST(testDescriptorValidationAndDsdRejection);
	RUN_TEST(testVendorMustPopulateEveryPrivateDescriptorBuffer);
	RUN_TEST(testDsdIoFormatNegotiationIsRejectedBeforeVendorMutation);
	RUN_TEST(testPartialFailureReleasesCallbackOwner);
	RUN_TEST(testRecoverableCreateFailureDoesNotPoisonTheProcess);
	RUN_TEST(testStartErrorAndDspExceptionStayDry);
	RUN_TEST(testHostThrowAfterReadyStillSignalsVendorAndStaysDry);
	RUN_TEST(testDirectTimeInfoReentrancyDesynchronizesUntilRestart);
	RUN_TEST(testResetAcceptanceAndSampleRateLifecycle);
	RUN_TEST(testRestartRefreshesSampleRateAfterExternalClockChange);
	RUN_TEST(testStartFailureDrainsCallbackBeforeResettingBuffers);
	RUN_TEST(testStopDrainsEnteredCallbackBeforeCallingVendor);
	RUN_TEST(testNonBufferCallbacksRejectLifecycleReentry);
	RUN_TEST(testVendorCppExceptionsDoNotEscapeTheAsioAbi);
	RUN_TEST(testFailedTeardownPoisonsStaticCallbacksAndBlocksUnload);
	RUN_TEST(testFinalReleaseSafetyInIsolatedProcesses);
#undef RUN_TEST
	if (failures == 0)
		std::puts("All ASIO proxy driver fake-vendor tests passed.");
	return failures == 0 ? 0 : 1;
}
