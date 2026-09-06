#include "AsioProxyDriver.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>

#include <intrin.h>
#include <objbase.h>

#include "../version.h"
#include "AsioProxyIdentity.h"

namespace HibikiAsio
{
namespace
{
constexpr long kMaximumDescriptorCount = 4096;
static_assert(MAJOR >= 0 && MAJOR <= 0x7FFF &&
	MINOR >= 0 && MINOR <= 0xFF &&
	REVISION >= 0 && REVISION <= 0xFF,
	"ASIO driver version components exceed the stable packed ABI format.");
constexpr long kPackedDriverVersion =
	(static_cast<long>(MAJOR) << 16) |
	(static_cast<long>(MINOR) << 8) |
	static_cast<long>(REVISION);
thread_local bool vendorInitializationInProgress = false;

static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
	"ASIO async buffer generations must be lock-free.");
static_assert(std::atomic<unsigned>::is_always_lock_free,
	"ASIO async completion accounting must be lock-free.");
static_assert(std::atomic<bool>::is_always_lock_free,
	"ASIO async completion gating must be lock-free.");
void copyText(char* destination, std::size_t capacity, const char* source) noexcept
{
	if (destination == nullptr || capacity == 0)
		return;
	if (source == nullptr)
		source = "";
	strncpy_s(destination, capacity, source, _TRUNCATE);
}

const char* selectionError(TargetSelectionStatus status) noexcept
{
	switch (status)
	{
	case TargetSelectionStatus::Ambiguous:
		return "Multiple x64 ASIO drivers found. Run Hibiki EQAPO setup to select the hardware driver.";
	case TargetSelectionStatus::ConfiguredUnavailable:
		return "Configured ASIO driver is unavailable. Run Hibiki EQAPO setup to check the hardware driver.";
	case TargetSelectionStatus::SelfTarget:
		return "ASIO proxy recursion was blocked. Run Hibiki EQAPO setup to select the hardware driver.";
	case TargetSelectionStatus::Missing:
	default:
		return "No usable x64 ASIO driver found. Run Hibiki EQAPO setup to select the hardware driver.";
	}
}
}

std::atomic<long> AsioProxyDriver::liveInstances_{0};

HRESULT activateAsioVendor(const CLSID& clsid, IASIO** driver)
{
	if (driver == nullptr)
		return E_POINTER;
	*driver = nullptr;
	// ASIO's Windows ABI uses the concrete driver's CLSID as both class ID
	// and requested interface ID. There is no generic IID_IASIO.
	try
	{
		return CoCreateInstance(
			clsid,
			nullptr,
			CLSCTX_INPROC_SERVER,
			clsid,
			reinterpret_cast<void**>(driver));
	}
	catch (...)
	{
		*driver = nullptr;
		return E_UNEXPECTED;
	}
}

AsioProxyDriver::AsioProxyDriver(
	HMODULE module,
	AsioTargetResolver targetResolver,
	AsioVendorActivator vendorActivator,
	AsioDspFactory dspFactory) noexcept :
	module_(module),
	targetResolver_(targetResolver),
	vendorActivator_(vendorActivator),
	dspFactory_(dspFactory)
{
	resetAsyncBufferQueue();
	resetStagingState();
	liveInstances_.fetch_add(1, std::memory_order_relaxed);
}

AsioProxyDriver::~AsioProxyDriver()
{
	forceShutdown();
	liveInstances_.fetch_sub(1, std::memory_order_relaxed);
}

HRESULT STDMETHODCALLTYPE AsioProxyDriver::QueryInterface(
	REFIID iid,
	void** object)
{
	if (object == nullptr)
		return E_POINTER;
	*object = nullptr;
	if (IsEqualIID(iid, IID_IUnknown) || InlineIsEqualGUID(iid, kDriverClsid))
	{
		*object = static_cast<IASIO*>(this);
		AddRef();
		return S_OK;
	}
	return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE AsioProxyDriver::AddRef()
{
	return referenceCount_.fetch_add(1, std::memory_order_relaxed) + 1;
}

ULONG STDMETHODCALLTYPE AsioProxyDriver::Release()
{
	const ULONG remaining =
		referenceCount_.fetch_sub(1, std::memory_order_acq_rel) - 1;
	if (remaining == 0)
		delete this;
	else if (remaining == 1 &&
		callbackLifetimeOwned_.load(std::memory_order_acquire))
	{
		// Only the bridge's acquire-time lifetime reference remains. Teardown
		// cannot safely run from an ASIO callback, and an external final Release
		// racing a callback must not delete the raw activeSink_. Permanently close
		// dispatch without waiting; the retained owner pins the object and module.
		callbackBridge_.abandon(this);
		asyncDesynchronized_.store(true, std::memory_order_release);
		realtimeError_.store(
			RealtimeError::InvalidBuffer, std::memory_order_release);
	}
	return remaining;
}

ASIOBool AsioProxyDriver::init(void* systemHandle)
{
	if (state_.load(std::memory_order_acquire) != State::Constructed)
		return ASIOTrue;
	if (vendorInitializationInProgress)
	{
		setError("ASIO proxy initialization recursion was blocked. Run Hibiki EQAPO setup to select the hardware driver.");
		return ASIOFalse;
	}
	vendorInitializationInProgress = true;
	struct InitializationGuard
	{
		~InitializationGuard()
		{
			vendorInitializationInProgress = false;
		}
	} initializationGuard;
	if (targetResolver_ == nullptr || vendorActivator_ == nullptr)
	{
		setError("ASIO proxy initialization is unavailable.");
		return ASIOFalse;
	}

	ResolvedAsioTarget target;
	try
	{
		target = targetResolver_(module_);
	}
	catch (...)
	{
		setError("ASIO driver discovery failed. Run Hibiki EQAPO setup to check the hardware driver.");
		return ASIOFalse;
	}
	if (!target.isSelected())
	{
		setError(selectionError(target.status));
		return ASIOFalse;
	}
	if (InlineIsEqualGUID(target.clsid, kDriverClsid))
	{
		setError(selectionError(TargetSelectionStatus::SelfTarget));
		return ASIOFalse;
	}

	IASIO* loadedVendor = nullptr;
	HRESULT activationResult = E_UNEXPECTED;
	try
	{
		activationResult = vendorActivator_(target.clsid, &loadedVendor);
	}
	catch (...)
	{
		activationResult = E_UNEXPECTED;
	}
	if (FAILED(activationResult) || loadedVendor == nullptr)
	{
		if (loadedVendor != nullptr)
			safeVendorRelease(loadedVendor);
		setError("Configured ASIO driver could not be loaded. Run Hibiki EQAPO setup to check the hardware driver.");
		return ASIOFalse;
	}
	if (safeVendorCall(
		[&]() { return loadedVendor->init(systemHandle); }, ASIOFalse) != ASIOTrue)
	{
		safeVendorRelease(loadedVendor);
		setError("The selected ASIO driver failed to initialize.");
		return ASIOFalse;
	}

	vendor_ = loadedVendor;
	vendorClsid_ = target.clsid;
	errorMessage_.fill('\0');
	realtimeError_.store(RealtimeError::None, std::memory_order_release);
	vendorOutputReadySupport_.store(
		VendorOutputReadySupport::Unknown, std::memory_order_release);
	state_.store(State::Initialized, std::memory_order_release);
	return ASIOTrue;
}

void AsioProxyDriver::getDriverName(char* name)
{
	copyText(name, 32, kDriverDisplayName);
}

long AsioProxyDriver::getDriverVersion()
{
	return kPackedDriverVersion;
}

void AsioProxyDriver::getErrorMessage(char* message)
{
	if (message == nullptr)
		return;
	switch (realtimeError_.load(std::memory_order_acquire))
	{
	case RealtimeError::DspFailed:
		copyText(message, 124, "Correction failed; ASIO output remains dry until reset.");
		return;
	case RealtimeError::InvalidBuffer:
		copyText(message, 124, "ASIO callback/staging order became unsafe; output is silenced until restart.");
		return;
	case RealtimeError::StageMiss:
		copyText(message, 124, "ASIO output missed its one-block staging deadline; output is silenced until restart.");
		return;
	case RealtimeError::VendorOutputReadyFailed:
		copyText(message, 124, "The hardware driver rejected its optional output-ready hint; audio continues without it.");
		return;
	case RealtimeError::VendorTeardownFailed:
		copyText(message, 124, "The hardware ASIO driver failed to stop; output state is vendor-defined. Restart the host.");
		return;
	default:
		break;
	}
	if (errorMessage_[0] != '\0')
	{
		copyText(message, 124, errorMessage_.data());
		return;
	}
	if (vendor_ != nullptr)
	{
		try
		{
			vendor_->getErrorMessage(message);
			message[123] = '\0';
		}
		catch (...)
		{
			copyText(message, 124, "The selected ASIO driver raised an exception.");
		}
		return;
	}
	copyText(message, 124, "ASIO proxy is not initialized. Run Hibiki EQAPO setup to select the hardware driver.");
}

ASIOError AsioProxyDriver::start()
{
	if (AsioCallbackBridge::isInsideCallback())
		return invalidState("ASIO start cannot run from a driver callback.");
	if (state_.load(std::memory_order_acquire) != State::Prepared)
		return invalidState("ASIO buffers must be created before start.");
	if (startRetryBlocked_.load(std::memory_order_acquire))
		return invalidState("A deferred ASIO worker outlived the stream; dispose buffers, then restart the host process.");
	closeCallbackActivityGateAndDrain();
	drainProcessing();
	const std::uint64_t sampleRateGeneration =
		sampleRateChangeGeneration_.load(std::memory_order_acquire);
	// An external clock can change the hardware rate while buffers stay
	// prepared. Refresh it on every start before rebuilding rate-dependent DSP;
	// the realtime sampleRateDidChange callback only invalidates staged data.
	ASIOSampleRate currentSampleRate = 0.0;
	const ASIOError sampleRateResult = safeVendorCall(
		[&]() { return vendor_->getSampleRate(&currentSampleRate); },
		ASE_HWMalfunction);
	if (sampleRateResult != ASE_OK)
	{
		setError("The hardware ASIO sample rate could not be refreshed before start.");
		openCallbackActivityGate();
		return sampleRateResult;
	}
	if (!std::isfinite(currentSampleRate) || currentSampleRate <= 0.0)
	{
		setError("The hardware ASIO driver reported an invalid sample rate.");
		openCallbackActivityGate();
		return ASE_NoClock;
	}
	sampleRate_ = currentSampleRate;
	resetAsyncBufferQueue();
	resetStagingState();
	asyncDesynchronized_.store(false, std::memory_order_release);
	realtimeError_.store(RealtimeError::None, std::memory_order_release);
	rebuildDspForStart();
	zeroVendorOutput(0);
	zeroVendorOutput(1);

	// ASIO hosts may prefill output half 1 before ASIOStart. Capture that fixed
	// proxy-owned buffer now, but do not touch either vendor buffer: sequence 0
	// is committed only when the first serialized vendor callback identifies a
	// safe destination half. This is the explicit one-block pipeline pre-roll.
	if (!outputChannels_.empty())
	{
		announcedGenerations_[1].store(1, std::memory_order_release);
		const AsyncBufferToken preRoll{1, 1, 0};
		if (!prepareHostBuffer(preRoll) || !stageHostOutput(preRoll))
		{
			asyncDesynchronized_.store(true, std::memory_order_release);
			realtimeError_.store(RealtimeError::StageMiss, std::memory_order_release);
		}
	}
	if (sampleRateChangeGeneration_.load(std::memory_order_acquire) !=
		sampleRateGeneration)
	{
		resetAsyncBufferQueue();
		resetStagingState();
		engineReady_.store(false, std::memory_order_release);
		setError("The hardware ASIO sample rate changed while starting; retry start.");
		openCallbackActivityGate();
		return ASE_NoClock;
	}

	// Some vendors issue their first bufferSwitch synchronously from start().
	// Publish Running before opening the callback gate so that an inline host
	// outputReady is handled as content completion rather than a capability probe.
	state_.store(State::Running, std::memory_order_release);
#if defined(HIBIKI_ASIO_TESTING)
	if (startGateHook_ != nullptr)
		startGateHook_();
#endif
	openCallbackActivityGate();
	// Linearize the rate snapshot against a callback that arrived after the
	// closed-gate check but immediately before reopening. Once the gate is open,
	// any later notification enters onSampleRateDidChange and invalidates DSP.
	if (sampleRateChangeGeneration_.load(std::memory_order_acquire) !=
		sampleRateGeneration)
	{
		closeCallbackActivityGateAndDrain();
		state_.store(State::Prepared, std::memory_order_release);
		resetAsyncBufferQueue();
		resetStagingState();
		engineReady_.store(false, std::memory_order_release);
		asyncDesynchronized_.store(false, std::memory_order_release);
		setError("The hardware ASIO sample rate changed while starting; retry start.");
		openCallbackActivityGate();
		return ASE_NoClock;
	}
	const ASIOError result = safeVendorCall(
		[&]() { return vendor_->start(); }, ASE_HWMalfunction);
	if (result != ASE_OK)
	{
		// A vendor may launch a callback before returning a start failure. Close
		// and drain every callback/outputReady handler before resetting slots.
		closeCallbackActivityGateAndDrain();
		drainProcessing();
		const bool deferredCompletionOutstanding =
			asyncEnqueuePosition_.load(std::memory_order_acquire) !=
			asyncDequeuePosition_.load(std::memory_order_acquire);
		startRetryBlocked_.store(
			deferredCompletionOutstanding, std::memory_order_release);
		if (deferredCompletionOutstanding)
		{
			// A host worker may still be writing a proxy-owned H half. Preserve the
			// queue, stage slots, and all pointed-to storage until that worker's
			// outputReady credit is observed and disposeBuffers performs cleanup.
			asyncDesynchronized_.store(true, std::memory_order_release);
		}
		else
		{
			resetAsyncBufferQueue();
			resetStagingState();
		}
		state_.store(State::Prepared, std::memory_order_release);
	}
	return result;
}

ASIOError AsioProxyDriver::stop()
{
	if (AsioCallbackBridge::isInsideCallback())
		return invalidState("ASIO stop cannot run from a driver callback.");
	if (state_.load(std::memory_order_acquire) != State::Running)
		return invalidState("ASIO driver is not running.");
	closeCallbackActivityGateAndDrain();
	drainProcessing();
	const bool deferredCompletionOutstanding =
		asyncEnqueuePosition_.load(std::memory_order_acquire) !=
		asyncDequeuePosition_.load(std::memory_order_acquire);
	const ASIOError result = safeVendorCall(
		[&]() { return vendor_->stop(); }, ASE_HWMalfunction);
	if (result == ASE_OK)
	{
		state_.store(State::Prepared, std::memory_order_release);
		closeCallbackActivityGateAndDrain();
		startRetryBlocked_.store(
			deferredCompletionOutstanding, std::memory_order_release);
		if (deferredCompletionOutstanding)
		{
			// Do not recycle or free a proxy H half while the host worker can still
			// hold its raw pointer. outputReady has no identity, so this ownership
			// loss is irreversible for the process and dispose will pin the arena.
			asyncDesynchronized_.store(true, std::memory_order_release);
		}
		else
		{
			resetAsyncBufferQueue();
			resetStagingState();
			asyncDesynchronized_.store(false, std::memory_order_release);
		}
	}
	else
	{
		// A failed vendor stop leaves buffer ownership and completion ordering
		// unknowable. Keep this stream terminally gated and no-touch; hardware
		// output is vendor-defined. The host may
		// retry stop, then start again after a successful boundary. Replaying
		// completions here would race later outputReady calls and invert order.
		asyncDesynchronized_.store(true, std::memory_order_release);
		realtimeError_.store(
			RealtimeError::VendorTeardownFailed, std::memory_order_release);
	}
	return result;
}

ASIOError AsioProxyDriver::getChannels(
	long* inputChannels,
	long* outputChannels)
{
	if (vendor_ == nullptr)
		return ASE_NotPresent;
	if (inputChannels == nullptr || outputChannels == nullptr)
		return ASE_InvalidParameter;
	long vendorInputs = 0;
	long vendorOutputs = 0;
	const ASIOError result = safeVendorCall(
		[&]() { return vendor_->getChannels(&vendorInputs, &vendorOutputs); },
		ASE_HWMalfunction);
	if (result != ASE_OK)
		return result;
	if (vendorInputs < 0 || vendorOutputs < 0)
	{
		setError("The hardware ASIO driver reported an invalid channel count.");
		return ASE_HWMalfunction;
	}
	*inputChannels = vendorInputs;
	// Version 1 is intentionally a main-monitor proxy. Advertise the same
	// first mono/stereo pair that createBuffers can actually accept so hosts
	// which prepare every advertised output do not fail during initialization.
	*outputChannels = (std::min)(vendorOutputs, 2L);
	return ASE_OK;
}

ASIOError AsioProxyDriver::getLatencies(
	long* inputLatency,
	long* outputLatency)
{
	if (vendor_ == nullptr)
		return ASE_NotPresent;
	if (inputLatency == nullptr || outputLatency == nullptr)
		return ASE_InvalidParameter;
	const ASIOError result = safeVendorCall(
		[&]() { return vendor_->getLatencies(inputLatency, outputLatency); },
		ASE_HWMalfunction);
	if (result != ASE_OK)
		return result;
	// Output is staged into the next vendor callback buffer. ASIO requires the
	// preferred size to be assumed before createBuffers; afterward the actual
	// configured size is authoritative. A failed auxiliary query leaves the
	// successfully returned vendor latency unchanged.
	long stagedFrames = 0;
	if (bufferSize_ > 0)
	{
		if (!outputChannels_.empty())
			stagedFrames = bufferSize_;
	}
	else
	{
		long minimum = 0;
		long maximum = 0;
		long preferred = 0;
		long granularity = 0;
		const ASIOError sizeResult = safeVendorCall(
			[&]() { return vendor_->getBufferSize(
				&minimum, &maximum, &preferred, &granularity); },
			ASE_HWMalfunction);
		if (sizeResult == ASE_OK && preferred > 0)
			stagedFrames = preferred;
	}
	if (stagedFrames > 0 && *outputLatency >= 0)
	{
		if (*outputLatency > (std::numeric_limits<long>::max)() - stagedFrames)
			*outputLatency = (std::numeric_limits<long>::max)();
		else
			*outputLatency += stagedFrames;
	}
	return ASE_OK;
}

ASIOError AsioProxyDriver::getBufferSize(
	long* minimumSize,
	long* maximumSize,
	long* preferredSize,
	long* granularity)
{
	if (vendor_ == nullptr)
		return ASE_NotPresent;
	if (minimumSize == nullptr || maximumSize == nullptr ||
		preferredSize == nullptr || granularity == nullptr)
	{
		return ASE_InvalidParameter;
	}
	return safeVendorCall(
		[&]() { return vendor_->getBufferSize(
			minimumSize, maximumSize, preferredSize, granularity); },
		ASE_HWMalfunction);
}

ASIOError AsioProxyDriver::canSampleRate(ASIOSampleRate sampleRate)
{
	return vendor_ == nullptr ? ASE_NotPresent : safeVendorCall(
		[&]() { return vendor_->canSampleRate(sampleRate); }, ASE_HWMalfunction);
}

ASIOError AsioProxyDriver::getSampleRate(ASIOSampleRate* sampleRate)
{
	if (vendor_ == nullptr)
		return ASE_NotPresent;
	if (sampleRate == nullptr)
		return ASE_InvalidParameter;
	return safeVendorCall(
		[&]() { return vendor_->getSampleRate(sampleRate); }, ASE_HWMalfunction);
}

ASIOError AsioProxyDriver::setSampleRate(ASIOSampleRate sampleRate)
{
	if (vendor_ == nullptr)
		return ASE_NotPresent;
	if (state_.load(std::memory_order_acquire) != State::Initialized)
		return invalidState(
			"Dispose ASIO buffers before changing sample rate.");
	return safeVendorCall(
		[&]() { return vendor_->setSampleRate(sampleRate); }, ASE_HWMalfunction);
}

ASIOError AsioProxyDriver::getClockSources(
	ASIOClockSource* clocks,
	long* sourceCount)
{
	if (vendor_ == nullptr)
		return ASE_NotPresent;
	if (sourceCount == nullptr || *sourceCount < 0 ||
		(clocks == nullptr && *sourceCount > 0))
		return ASE_InvalidParameter;
	return safeVendorCall(
		[&]() { return vendor_->getClockSources(clocks, sourceCount); },
		ASE_HWMalfunction);
}

ASIOError AsioProxyDriver::setClockSource(long reference)
{
	return vendor_ == nullptr ? ASE_NotPresent : safeVendorCall(
		[&]() { return vendor_->setClockSource(reference); }, ASE_HWMalfunction);
}

ASIOError AsioProxyDriver::getSamplePosition(
	ASIOSamples* position,
	ASIOTimeStamp* timestamp)
{
	if (vendor_ == nullptr)
		return ASE_NotPresent;
	if (position == nullptr || timestamp == nullptr)
		return ASE_InvalidParameter;
	return safeVendorCall(
		[&]() { return vendor_->getSamplePosition(position, timestamp); },
		ASE_HWMalfunction);
}

ASIOError AsioProxyDriver::getChannelInfo(ASIOChannelInfo* info)
{
	if (vendor_ == nullptr)
		return ASE_NotPresent;
	if (info == nullptr)
		return ASE_InvalidParameter;
	if (info->isInput == ASIOFalse &&
		(info->channel < 0 || info->channel >= 2))
	{
		return ASE_InvalidParameter;
	}
	return safeVendorCall(
		[&]() { return vendor_->getChannelInfo(info); }, ASE_HWMalfunction);
}

ASIOError AsioProxyDriver::createBuffers(
	ASIOBufferInfo* bufferInfos,
	long channelCount,
	long bufferSize,
	ASIOCallbacks* callbacks)
{
	if (state_.load(std::memory_order_acquire) != State::Initialized)
		return invalidState("ASIO buffers already exist or the driver is not initialized.");
	bool expectedCreate = false;
	if (!createInProgress_.compare_exchange_strong(
		expectedCreate, true,
		std::memory_order_acq_rel, std::memory_order_acquire))
	{
		return invalidState("ASIO buffer creation is already in progress.");
	}
	struct CreateGuard
	{
		std::atomic<bool>& inProgress;
		~CreateGuard()
		{
			inProgress.store(false, std::memory_order_release);
		}
	} createGuard{createInProgress_};
	if (bufferInfos == nullptr || callbacks == nullptr ||
		callbacks->bufferSwitch == nullptr || channelCount <= 0 ||
		channelCount > kMaximumDescriptorCount || bufferSize <= 0 ||
		static_cast<unsigned long long>(bufferSize) >
			(std::numeric_limits<std::size_t>::max)() / sizeof(double))
	{
		setError("Invalid ASIO buffer descriptors.");
		return ASE_InvalidParameter;
	}

	long availableInputs = 0;
	long availableOutputs = 0;
	ASIOError result = safeVendorCall(
		[&]() { return vendor_->getChannels(&availableInputs, &availableOutputs); },
		ASE_HWMalfunction);
	if (result != ASE_OK)
		return result;
	if (availableInputs < 0 || availableOutputs < 0)
	{
		setError("The hardware ASIO driver reported an invalid channel count.");
		return ASE_HWMalfunction;
	}
	availableOutputs = (std::min)(availableOutputs, 2L);

	std::vector<OutputChannel> outputChannels;
	try
	{
		outputChannels.reserve(2);
		for (long index = 0; index < channelCount; ++index)
		{
			const bool isInput = bufferInfos[index].isInput != ASIOFalse;
			const long available = isInput ? availableInputs : availableOutputs;
			if (bufferInfos[index].channelNum < 0 ||
				bufferInfos[index].channelNum >= available)
			{
				setError("ASIO channel descriptor is out of range.");
				return ASE_InvalidParameter;
			}
			for (long prior = 0; prior < index; ++prior)
			{
				if (bufferInfos[prior].isInput == bufferInfos[index].isInput &&
					bufferInfos[prior].channelNum == bufferInfos[index].channelNum)
				{
					setError("Duplicate ASIO channel descriptor.");
					return ASE_InvalidParameter;
				}
			}
			if (isInput)
				continue;
			if (outputChannels.size() >= 2)
			{
				setError("Hibiki EQAPO currently supports mono or stereo ASIO output.");
				return ASE_InvalidParameter;
			}
			ASIOChannelInfo info{};
			info.channel = bufferInfos[index].channelNum;
			info.isInput = ASIOFalse;
			result = safeVendorCall(
				[&]() { return vendor_->getChannelInfo(&info); }, ASE_HWMalfunction);
			if (result != ASE_OK)
				return result;
			const SampleFormat format = AsioSampleCodec::describe(info.type);
			if (!format.isSupported())
			{
				setError(format.isDsd ?
					"DSD output is not supported; select PCM in the ASIO control panel." :
					"The selected ASIO PCM sample format is not supported.");
				return ASE_InvalidParameter;
			}
			outputChannels.push_back({static_cast<std::size_t>(index), format});
		}

		ASIOSampleRate sampleRate = 0.0;
		result = safeVendorCall(
			[&]() { return vendor_->getSampleRate(&sampleRate); }, ASE_HWMalfunction);
		if (result != ASE_OK)
			return result;
		if (!std::isfinite(sampleRate) || sampleRate <= 0.0)
			return ASE_NoClock;

		std::array<std::vector<std::unique_ptr<double[]>>, 2> decoded;
		std::array<std::vector<std::unique_ptr<double[]>>, 2> processed;
		std::array<std::vector<double*>, 2> decodedPointers;
		std::array<std::vector<double*>, 2> processedPointers;
		for (std::size_t half = 0; half < 2; ++half)
		{
			decoded[half].reserve(outputChannels.size());
			processed[half].reserve(outputChannels.size());
			decodedPointers[half].reserve(outputChannels.size());
			processedPointers[half].reserve(outputChannels.size());
			for (std::size_t channel = 0;
				channel < outputChannels.size(); ++channel)
			{
				decoded[half].push_back(std::make_unique<double[]>(
					static_cast<std::size_t>(bufferSize)));
				processed[half].push_back(std::make_unique<double[]>(
					static_cast<std::size_t>(bufferSize)));
				decodedPointers[half].push_back(decoded[half].back().get());
				processedPointers[half].push_back(processed[half].back().get());
			}
		}
		for (OutputChannel& output : outputChannels)
		{
			if (output.format.containerBytes == 0 ||
				static_cast<std::size_t>(bufferSize) >
					(std::numeric_limits<std::size_t>::max)() /
					output.format.containerBytes)
			{
				throw std::bad_alloc();
			}
			output.byteCount = static_cast<std::size_t>(bufferSize) *
				output.format.containerBytes;
			const std::size_t alignedUnits = output.byteCount == 0 ? 0 :
				1 + (output.byteCount - 1) / sizeof(std::max_align_t);
			for (std::size_t half = 0; half < 2; ++half)
			{
				output.hostBuffers[half] =
					std::make_unique<std::max_align_t[]>(alignedUnits);
				output.stageBuffers[half] =
					std::make_unique<std::max_align_t[]>(alignedUnits);
				std::memset(output.hostBuffers[half].get(), 0, output.byteCount);
				std::memset(output.stageBuffers[half].get(), 0, output.byteCount);
			}
		}

		std::unique_ptr<IAsioDsp> dsp;
		if (!outputChannels.empty() && dspFactory_ != nullptr)
		{
			dsp = dspFactory_(
				sampleRate,
				static_cast<unsigned>(outputChannels.size()),
				static_cast<unsigned>(bufferSize));
		}

		hostCallbacks_ = *callbacks;
		bufferSize_ = bufferSize;
		sampleRate_ = sampleRate;
		vendorBufferInfos_ = std::make_unique<std::vector<ASIOBufferInfo>>(
			bufferInfos, bufferInfos + channelCount);
		// buffer pointers are vendor outputs, never caller inputs. Clearing both
		// halves is essential: otherwise a vendor that forgets one assignment could
		// inherit an arbitrary stale host pointer and pass the null-buffer gate.
		for (ASIOBufferInfo& info : *vendorBufferInfos_)
		{
			info.buffers[0] = nullptr;
			info.buffers[1] = nullptr;
		}
		outputChannels_ = std::move(outputChannels);
		decodedBuffers_ = std::move(decoded);
		processedBuffers_ = std::move(processed);
		decodedPointers_ = std::move(decodedPointers);
		processedPointers_ = std::move(processedPointers);
		dsp_ = std::move(dsp);
		engineReady_.store(outputChannels_.empty() || dsp_ != nullptr,
			std::memory_order_release);
	}
	catch (const std::bad_alloc&)
	{
		clearPreparedState();
		setError("Not enough memory to prepare ASIO buffers.");
		return ASE_NoMemory;
	}
	catch (...)
	{
		clearPreparedState();
		setError("Equalizer configuration could not be prepared; output remains dry.");
		return ASE_InvalidMode;
	}

	if (!callbackBridge_.acquire(this))
	{
		clearPreparedState();
		setError("Another Hibiki EQAPO driver instance is already active.");
		return ASE_InvalidMode;
	}
	createNegotiationActive_.store(true, std::memory_order_release);
	bool createThrew = false;
	try
	{
		result = vendor_->createBuffers(
			vendorBufferInfos_->data(), channelCount, bufferSize,
			callbackBridge_.vendorCallbacks());
	}
	catch (...)
	{
		createThrew = true;
		result = ASE_HWMalfunction;
		realtimeError_.store(
			RealtimeError::InvalidBuffer, std::memory_order_release);
	}
	if (result != ASE_OK)
	{
		if (createThrew)
		{
			// An exception leaves the third-party driver state unknowable. Try to
			// tear it down, and keep the process-lifetime callback sentinel if that
			// cleanup cannot prove that late callbacks are impossible.
			const ASIOError cleanup = safeVendorCall(
				[&]() { return vendor_->disposeBuffers(); }, ASE_HWMalfunction);
			if (cleanup == ASE_OK)
				callbackBridge_.release(this);
			else
			{
				callbackBridge_.poison(this);
				// The vendor may retain the exact descriptor array even though its
				// teardown failed. Leak that preallocated owner with the poisoned
				// module rather than leave a dangling vendor pointer.
				(void)vendorBufferInfos_.release();
			}
		}
		else
		{
			// A normal non-OK return means createBuffers did not establish the
			// prepared state. In particular, drivers commonly return InvalidMode
			// while a host probes buffer sizes, and disposeBuffers may also return
			// InvalidMode because there is nothing to dispose. Keep this path
			// recoverable and let the host retry.
			callbackBridge_.release(this);
		}
		clearPreparedState();
		return result;
	}

	for (const ASIOBufferInfo& info : *vendorBufferInfos_)
	{
		if (info.buffers[0] == nullptr || info.buffers[1] == nullptr)
		{
			const ASIOError cleanup = safeVendorCall(
				[&]() { return vendor_->disposeBuffers(); }, ASE_HWMalfunction);
			if (cleanup == ASE_OK)
				callbackBridge_.release(this);
			else
			{
				callbackBridge_.poison(this);
				(void)vendorBufferInfos_.release();
			}
			clearPreparedState();
			setError("The selected ASIO driver returned a null ASIO buffer.");
			return ASE_InvalidParameter;
		}
	}
	try
	{
		std::vector<ASIOBufferInfo> hostBufferInfos = *vendorBufferInfos_;
		for (const OutputChannel& output : outputChannels_)
		{
			ASIOBufferInfo& hostInfo = hostBufferInfos[output.bufferInfoIndex];
			hostInfo.buffers[0] = output.hostBuffers[0].get();
			hostInfo.buffers[1] = output.hostBuffers[1].get();
		}
		// Finish every allocation before publishing any pointer into the caller's
		// descriptor array. The final element-wise copy is non-throwing.
		bufferInfos_ = hostBufferInfos;
		for (long index = 0; index < channelCount; ++index)
			bufferInfos[index] = hostBufferInfos[static_cast<std::size_t>(index)];
	}
	catch (const std::bad_alloc&)
	{
		const ASIOError cleanup = safeVendorCall(
			[&]() { return vendor_->disposeBuffers(); }, ASE_HWMalfunction);
		if (cleanup == ASE_OK)
			callbackBridge_.release(this);
		else
		{
			callbackBridge_.poison(this);
			(void)vendorBufferInfos_.release();
		}
		clearPreparedState();
		return ASE_NoMemory;
	}
	zeroVendorOutput(0);
	zeroVendorOutput(1);

	if (!outputChannels_.empty() && dsp_ == nullptr)
		setError("Correction configuration is unavailable; ASIO output remains dry.");
	else
		errorMessage_.fill('\0');
	realtimeError_.store(RealtimeError::None, std::memory_order_release);
	// Some drivers answer ASE_NotPresent until their buffers exist. An earlier
	// Initialized-state capability probe must not permanently suppress the one
	// useful retry after createBuffers succeeds.
	if (vendorOutputReadySupport_.load(std::memory_order_acquire) ==
		VendorOutputReadySupport::Unsupported)
	{
		vendorOutputReadySupport_.store(
			VendorOutputReadySupport::Unknown, std::memory_order_release);
	}
	state_.store(State::Prepared, std::memory_order_release);
	openCallbackActivityGate();
	createNegotiationActive_.store(false, std::memory_order_release);
	return ASE_OK;
}

ASIOError AsioProxyDriver::disposeBuffers()
{
	if (AsioCallbackBridge::isInsideCallback())
		return invalidState("ASIO buffers cannot be disposed from a driver callback.");
	if (state_.load(std::memory_order_acquire) == State::Running)
	{
		const ASIOError stopResult = stop();
		if (stopResult != ASE_OK)
			return stopResult;
	}
	if (state_.load(std::memory_order_acquire) != State::Prepared)
		return invalidState("ASIO buffers have not been created.");
	// Close the worker-thread outputReady gate before asking the vendor to tear
	// down its buffers. A completion that entered before the gate closed drains
	// first; a later one observes the closed gate and cannot touch buffer state.
	const bool wasAccepting = closeCallbackActivityGateAndDrain();
	drainProcessing();
	const ASIOError result = safeVendorCall(
		[&]() { return vendor_->disposeBuffers(); }, ASE_HWMalfunction);
	if (result == ASE_OK)
	{
		if (startRetryBlocked_.load(std::memory_order_acquire))
		{
			// ASIO outputReady carries no buffer identity. Even a late signal cannot
			// prove whether every producer released every H pointer (duplicates and
			// multiple pending halves are indistinguishable). The vendor is disposed,
			// but the bridge owner reference and the complete prepared arena are
			// intentionally retained until process exit.
			callbackBridge_.abandon(this);
			state_.store(State::Quarantined, std::memory_order_release);
			setError("A deferred ASIO worker outlived the stream; restart the host before reopening Hibiki EQAPO.");
			return ASE_OK;
		}
		// The vendor has stopped issuing callbacks. Unpublish and drain any
		// trampoline already in flight before destroying callback-owned state.
		callbackBridge_.release(this);
		clearPreparedState();
		state_.store(State::Initialized, std::memory_order_release);
	}
	else
	{
		if (wasAccepting)
			openCallbackActivityGate();
	}
	return result;
}

ASIOError AsioProxyDriver::controlPanel()
{
	return vendor_ == nullptr ? ASE_NotPresent : safeVendorCall(
		[&]() { return vendor_->controlPanel(); }, ASE_HWMalfunction);
}

ASIOError AsioProxyDriver::future(long selector, void* option)
{
	if (vendor_ == nullptr)
		return ASE_NotPresent;
	if (selector == kAsioSetIoFormat || selector == kAsioGetIoFormat ||
		selector == kAsioCanDoIoFormat)
	{
		if (option == nullptr)
			return ASE_InvalidParameter;
		if (selector == kAsioSetIoFormat &&
			state_.load(std::memory_order_acquire) != State::Initialized)
		{
			return invalidState(
				"Dispose ASIO buffers before changing the hardware I/O format.");
		}
		auto* const format = static_cast<ASIOIoFormat*>(option);
		if (selector != kAsioGetIoFormat &&
			format->FormatType != kASIOPCMFormat)
		{
			format->FormatType = kASIOFormatInvalid;
			setError("DSD mode is unavailable through Hibiki EQAPO; select PCM.");
			return ASE_InvalidMode;
		}
		const ASIOError result = safeVendorCall(
			[&]() { return vendor_->future(selector, option); }, ASE_HWMalfunction);
		if ((result == ASE_SUCCESS || result == ASE_OK) &&
			selector == kAsioGetIoFormat &&
			format->FormatType != kASIOPCMFormat)
		{
			format->FormatType = kASIOFormatInvalid;
			setError("The selected ASIO driver is not in PCM mode.");
			return ASE_InvalidMode;
		}
		return result;
	}
	if (selector == kAsioGetInternalBufferSamples)
	{
		if (option == nullptr)
			return ASE_InvalidParameter;
		const ASIOError result = safeVendorCall(
			[&]() { return vendor_->future(selector, option); }, ASE_HWMalfunction);
		if (result != ASE_OK && result != ASE_SUCCESS)
			return result;
		auto* const info = static_cast<ASIOInternalBufferInfo*>(option);
		if (bufferSize_ > 0 && !outputChannels_.empty() && info->outputSamples >= 0)
		{
			if (info->outputSamples >
				(std::numeric_limits<long>::max)() - bufferSize_)
			{
				info->outputSamples = (std::numeric_limits<long>::max)();
			}
			else
				info->outputSamples += bufferSize_;
		}
		return result;
	}
	return safeVendorCall(
		[&]() { return vendor_->future(selector, option); }, ASE_HWMalfunction);
}

ASIOError AsioProxyDriver::outputReady()
{
	if (vendor_ == nullptr)
		return ASE_NotPresent;
	if (callbackBridge_.deferOutputReady())
		return ASE_OK;

	const State state = state_.load(std::memory_order_acquire);
	// Before start there is no announced content token. ASIO hosts commonly call
	// outputReady only to probe capability here. Probe the wrapped driver once,
	// but do not treat this as prefilled content or advance the staging FIFO.
	if (state != State::Running)
	{
		if (state == State::Quarantined ||
			(state == State::Prepared &&
				startRetryBlocked_.load(std::memory_order_acquire)))
			return ASE_OK;
		if ((state == State::Initialized || state == State::Prepared) &&
			vendorOutputReadySupport_.load(std::memory_order_acquire) ==
				VendorOutputReadySupport::Unknown)
		{
			signalVendorOutputReady();
		}
		return ASE_OK;
	}
	if (!tryEnterCallbackActivity())
		return ASE_OK;
	struct CallbackActivityGuard
	{
		AsioProxyDriver& driver;
		~CallbackActivityGuard() { driver.leaveCallbackActivity(); }
	} activityGuard{*this};

	// outputReady has no buffer index. Completion association is therefore the
	// fixed FIFO of deferred callbacks. DSP writes only proxy-owned stage memory;
	// the next serialized vendor callback performs the sole vendor-buffer commit.
	if (!asyncDesynchronized_.load(std::memory_order_acquire))
	{
		if (completeNextAsyncBuffer() == AsyncCompletion::None)
		{
			// In Running state an external (non-callback-stack) call is not a
			// capability probe. With no FIFO credit it is a duplicate/out-of-order
			// completion and the following unindexed association is unknowable.
			asyncDesynchronized_.store(true, std::memory_order_release);
			realtimeError_.store(
				RealtimeError::InvalidBuffer, std::memory_order_release);
		}
	}
	return ASE_OK;
}

void AsioProxyDriver::onBufferSwitch(
	long bufferIndex,
	ASIOBool directProcess) noexcept
{
	if (!tryEnterCallbackActivity())
		return;
	if (state_.load(std::memory_order_acquire) != State::Running)
	{
		// createBuffers must publish the vendor callback table before start, and a
		// broken vendor may call it while merely Prepared.  Such a callback has no
		// stream ownership boundary: do not advance staging or expose host buffers.
		leaveCallbackActivity();
		return;
	}
	const unsigned priorCallbacks =
		activeBufferCallbacks_.fetch_add(1, std::memory_order_acq_rel);
	struct CallbackHandlerGuard
	{
		AsioProxyDriver& driver;
		~CallbackHandlerGuard()
		{
			driver.activeBufferCallbacks_.fetch_sub(1, std::memory_order_acq_rel);
			driver.leaveCallbackActivity();
		}
	} callbackGuard{*this};
	if (priorCallbacks != 0)
	{
		// This one-block pipeline relies on the ASIO driver's normal serialized
		// buffer callbacks. Never race a second callback against a vendor-buffer
		// commit or host-half handoff.
		asyncDesynchronized_.store(true, std::memory_order_release);
		realtimeError_.store(
			RealtimeError::InvalidBuffer, std::memory_order_release);
		if (bufferIndex >= 0 && bufferIndex <= 1 &&
			AsioCallbackBridge::isInsideNestedBufferCallback())
		{
			// ASIO permits same-thread recursive callbacks. The outer callback is
			// no longer touching vendor memory after its entry commit, so silence
			// the nested half to prevent replaying two-cycles-old data. A truly
			// concurrent callback does not get this write because it could race.
			zeroVendorOutput(bufferIndex);
		}
		return;
	}
	if (bufferIndex < 0 || bufferIndex > 1)
	{
		asyncDesynchronized_.store(true, std::memory_order_release);
		realtimeError_.store(RealtimeError::InvalidBuffer, std::memory_order_release);
		return;
	}
	if (outputChannels_.empty())
	{
		try
		{
			if (hostCallbacks_.bufferSwitch != nullptr)
				hostCallbacks_.bufferSwitch(bufferIndex, directProcess);
		}
		catch (...)
		{
			realtimeError_.store(
				RealtimeError::InvalidBuffer, std::memory_order_release);
		}
		callbackBridge_.consumeDeferredOutputReady();
		return;
	}
	if (lastVendorBufferIndex_ == bufferIndex)
	{
		// Reusing the same half does not provide the ownership boundary needed by
		// the fixed two-slot pipeline. Commit silence only; do not expose a host
		// buffer that may still be owned by a worker.
		asyncDesynchronized_.store(true, std::memory_order_release);
		realtimeError_.store(RealtimeError::InvalidBuffer, std::memory_order_release);
		commitNextStageToVendor(bufferIndex);
		return;
	}
	lastVendorBufferIndex_ = bufferIndex;
	commitNextStageToVendor(bufferIndex);
	if (asyncDesynchronized_.load(std::memory_order_acquire))
		return;

	const AsyncBufferToken token = announceBuffer(bufferIndex);
	const bool deferred = directProcess == ASIOFalse;
	if (!prepareHostBuffer(token))
	{
		asyncDesynchronized_.store(true, std::memory_order_release);
		realtimeError_.store(RealtimeError::InvalidBuffer, std::memory_order_release);
		return;
	}
	if (deferred && !enqueueAsyncBuffer(token))
	{
		expireStage(token);
		asyncDesynchronized_.store(true, std::memory_order_release);
		realtimeError_.store(RealtimeError::InvalidBuffer, std::memory_order_release);
		return;
	}
	bool hostSucceeded = true;
	try
	{
		if (hostCallbacks_.bufferSwitch != nullptr)
			hostCallbacks_.bufferSwitch(bufferIndex, directProcess);
	}
	catch (...)
	{
		hostSucceeded = false;
		realtimeError_.store(RealtimeError::InvalidBuffer, std::memory_order_release);
	}
	const bool hostRequestedReady = callbackBridge_.consumeDeferredOutputReady();
	if (!hostSucceeded)
	{
		if (deferred && hostRequestedReady)
		{
			// An inline outputReady is the only proof that a deferred host no
			// longer owns H. Without it, the callback may have launched a worker
			// before throwing, so its FIFO credit and backing must remain pinned.
			discardFailedHostCompletion(token);
		}
		else if (!deferred)
			expireStage(token);
		asyncDesynchronized_.store(true, std::memory_order_release);
	}
	if (hostSucceeded)
	{
		if (!deferred)
			stageHostOutput(token);
		else if (hostRequestedReady)
			completeNextAsyncBuffer();
	}
}

ASIOTime* AsioProxyDriver::onBufferSwitchTimeInfo(
	ASIOTime* params,
	long bufferIndex,
	ASIOBool directProcess) noexcept
{
	if (!tryEnterCallbackActivity())
		return params;
	if (state_.load(std::memory_order_acquire) != State::Running)
	{
		leaveCallbackActivity();
		return params;
	}
	const unsigned priorCallbacks =
		activeBufferCallbacks_.fetch_add(1, std::memory_order_acq_rel);
	struct CallbackHandlerGuard
	{
		AsioProxyDriver& driver;
		~CallbackHandlerGuard()
		{
			driver.activeBufferCallbacks_.fetch_sub(1, std::memory_order_acq_rel);
			driver.leaveCallbackActivity();
		}
	} callbackGuard{*this};
	if (priorCallbacks != 0)
	{
		asyncDesynchronized_.store(true, std::memory_order_release);
		realtimeError_.store(
			RealtimeError::InvalidBuffer, std::memory_order_release);
		if (bufferIndex >= 0 && bufferIndex <= 1 &&
			AsioCallbackBridge::isInsideNestedBufferCallback())
		{
			zeroVendorOutput(bufferIndex);
		}
		return params;
	}
	if (bufferIndex < 0 || bufferIndex > 1)
	{
		asyncDesynchronized_.store(true, std::memory_order_release);
		realtimeError_.store(RealtimeError::InvalidBuffer, std::memory_order_release);
		return params;
	}
	if (outputChannels_.empty())
	{
		ASIOTime* result = params;
		try
		{
			if (hostCallbacks_.bufferSwitchTimeInfo != nullptr)
				result = hostCallbacks_.bufferSwitchTimeInfo(
					params, bufferIndex, directProcess);
		}
		catch (...)
		{
			realtimeError_.store(
				RealtimeError::InvalidBuffer, std::memory_order_release);
		}
		callbackBridge_.consumeDeferredOutputReady();
		return result;
	}
	if (lastVendorBufferIndex_ == bufferIndex)
	{
		asyncDesynchronized_.store(true, std::memory_order_release);
		realtimeError_.store(RealtimeError::InvalidBuffer, std::memory_order_release);
		commitNextStageToVendor(bufferIndex);
		return params;
	}
	lastVendorBufferIndex_ = bufferIndex;
	commitNextStageToVendor(bufferIndex);
	if (asyncDesynchronized_.load(std::memory_order_acquire))
		return params;

	ASIOTime* result = params;
	const AsyncBufferToken token = announceBuffer(bufferIndex);
	const bool deferred = directProcess == ASIOFalse;
	if (!prepareHostBuffer(token))
	{
		asyncDesynchronized_.store(true, std::memory_order_release);
		realtimeError_.store(RealtimeError::InvalidBuffer, std::memory_order_release);
		return result;
	}
	if (deferred && !enqueueAsyncBuffer(token))
	{
		expireStage(token);
		asyncDesynchronized_.store(true, std::memory_order_release);
		realtimeError_.store(RealtimeError::InvalidBuffer, std::memory_order_release);
		return result;
	}
	bool hostSucceeded = true;
	try
	{
		if (hostCallbacks_.bufferSwitchTimeInfo != nullptr)
		{
			result = hostCallbacks_.bufferSwitchTimeInfo(
				params, bufferIndex, directProcess);
		}
	}
	catch (...)
	{
		hostSucceeded = false;
		realtimeError_.store(RealtimeError::InvalidBuffer, std::memory_order_release);
	}
	const bool hostRequestedReady = callbackBridge_.consumeDeferredOutputReady();
	if (!hostSucceeded)
	{
		if (deferred && hostRequestedReady)
			discardFailedHostCompletion(token);
		else if (!deferred)
			expireStage(token);
		asyncDesynchronized_.store(true, std::memory_order_release);
	}
	if (hostSucceeded)
	{
		if (!deferred)
			stageHostOutput(token);
		else if (hostRequestedReady)
			completeNextAsyncBuffer();
	}
	return result;
}

void AsioProxyDriver::onSampleRateDidChange(
	ASIOSampleRate sampleRate) noexcept
{
	sampleRateChangeGeneration_.fetch_add(1, std::memory_order_acq_rel);
	if (!tryEnterCallbackActivity())
		return;
	struct CallbackActivityGuard
	{
		AsioProxyDriver& driver;
		~CallbackActivityGuard() { driver.leaveCallbackActivity(); }
	} activityGuard{*this};
	engineReady_.store(false, std::memory_order_release);
	// Any READY stage was calculated for the old sample rate. The next buffer
	// callback must commit silence rather than publish stale-rate correction.
	asyncDesynchronized_.store(true, std::memory_order_release);
	try
	{
		if (hostCallbacks_.sampleRateDidChange != nullptr)
			hostCallbacks_.sampleRateDidChange(sampleRate);
	}
	catch (...)
	{
		realtimeError_.store(RealtimeError::DspFailed, std::memory_order_release);
	}
}

long AsioProxyDriver::onAsioMessage(
	long selector,
	long value,
	void* message,
	double* option) noexcept
{
	const bool enteredCallbackActivity = tryEnterCallbackActivity();
	if (!enteredCallbackActivity &&
		!createNegotiationActive_.load(std::memory_order_acquire))
		return 0;
	struct CallbackActivityGuard
	{
		AsioProxyDriver& driver;
		bool entered;
		~CallbackActivityGuard()
		{
			if (entered)
				driver.leaveCallbackActivity();
		}
	} activityGuard{*this, enteredCallbackActivity};
	try
	{
		const long result = hostCallbacks_.asioMessage == nullptr ? 0 :
			hostCallbacks_.asioMessage(selector, value, message, option);
		if (result != 0 &&
			(selector == kAsioResetRequest || selector == kAsioBufferSizeChange))
		{
			engineReady_.store(false, std::memory_order_release);
		}
		return result;
	}
	catch (...)
	{
		realtimeError_.store(RealtimeError::DspFailed, std::memory_order_release);
		return 0;
	}
}

long AsioProxyDriver::liveInstanceCount() noexcept
{
	return liveInstances_.load(std::memory_order_acquire);
}

void AsioProxyDriver::retainCallbackLifetime() noexcept
{
	AddRef();
	callbackLifetimeOwned_.store(true, std::memory_order_release);
}

void AsioProxyDriver::releaseCallbackLifetime() noexcept
{
	callbackLifetimeOwned_.store(false, std::memory_order_release);
	Release();
}

IASIO* AsioProxyDriver::vendor() const noexcept
{
	return vendor_;
}

void AsioProxyDriver::safeVendorRelease(IASIO* driver) noexcept
{
	if (driver == nullptr)
		return;
	try
	{
		driver->Release();
	}
	catch (...)
	{
		realtimeError_.store(RealtimeError::InvalidBuffer, std::memory_order_release);
	}
}

void AsioProxyDriver::setError(const char* message) noexcept
{
	copyText(errorMessage_.data(), errorMessage_.size(), message);
}

void AsioProxyDriver::clearPreparedState() noexcept
{
	createNegotiationActive_.store(false, std::memory_order_release);
	callbackActivity_.store(kCallbackActivityClosed, std::memory_order_seq_cst);
	resetAsyncBufferQueue();
	resetStagingState();
	asyncDesynchronized_.store(false, std::memory_order_release);
	startRetryBlocked_ = false;
	engineReady_.store(false, std::memory_order_release);
	dsp_.reset();
	for (auto& pointers : processedPointers_)
		pointers.clear();
	for (auto& pointers : decodedPointers_)
		pointers.clear();
	for (auto& buffers : processedBuffers_)
		buffers.clear();
	for (auto& buffers : decodedBuffers_)
		buffers.clear();
	outputChannels_.clear();
	bufferInfos_.clear();
	vendorBufferInfos_.reset();
	bufferSize_ = 0;
	sampleRate_ = 0.0;
	hostCallbacks_ = {};
}

void AsioProxyDriver::forceShutdown() noexcept
{
	if (vendor_ == nullptr)
		return;
	State state = state_.load(std::memory_order_acquire);
	ASIOError disposeResult = ASE_OK;
	closeCallbackActivityGateAndDrain();
	drainProcessing();
	if (state == State::Running)
	{
		safeVendorCall(
			[&]() { return vendor_->stop(); }, ASE_HWMalfunction);
		state = State::Prepared;
	}
	if (state == State::Prepared)
		disposeResult = safeVendorCall(
			[&]() { return vendor_->disposeBuffers(); }, ASE_HWMalfunction);
	// Vendor stop/dispose completes before callback ownership is unpublished.
	if (disposeResult == ASE_OK)
		callbackBridge_.release(this);
	else
	{
		callbackBridge_.poison(this);
		(void)vendorBufferInfos_.release();
		// Teardown did not prove that the vendor released either the descriptor
		// array or its COM object. Keep both alive with the poisoned module.
		vendor_ = nullptr;
	}
	clearPreparedState();
	if (disposeResult == ASE_OK)
		safeVendorRelease(vendor_);
	vendor_ = nullptr;
	state_.store(State::Constructed, std::memory_order_release);
}

std::uint64_t AsioProxyDriver::makeStageWord(
	std::uint64_t sequence,
	StageState state) noexcept
{
	return (sequence << 3) | static_cast<std::uint64_t>(state);
}

AsioProxyDriver::StageState AsioProxyDriver::stageState(
	std::uint64_t word) noexcept
{
	return static_cast<StageState>(word & 0x7u);
}

std::uint64_t AsioProxyDriver::stageSequence(std::uint64_t word) noexcept
{
	return word >> 3;
}

bool AsioProxyDriver::announceStage(const AsyncBufferToken& token) noexcept
{
	const std::size_t slotIndex =
		static_cast<std::size_t>(token.callbackEpoch & 1u);
	StageSlot& slot = stageSlots_[slotIndex];
	std::uint64_t expected = slot.word.load(std::memory_order_acquire);
	if (stageState(expected) != StageState::Empty)
		return false;
	const std::uint64_t reserving =
		makeStageWord(token.callbackEpoch, StageState::Reserving);
	if (!slot.word.compare_exchange_strong(
		expected, reserving,
		std::memory_order_acq_rel, std::memory_order_acquire))
	{
		return false;
	}
	slot.token = token;
	slot.word.store(
		makeStageWord(token.callbackEpoch, StageState::Announced),
		std::memory_order_release);
	return true;
}

bool AsioProxyDriver::prepareHostBuffer(
	const AsyncBufferToken& token) noexcept
{
	if (token.bufferIndex < 0 || token.bufferIndex > 1 || bufferSize_ <= 0)
		return false;
	const std::size_t half = static_cast<std::size_t>(token.bufferIndex);
	HostBufferState expected = HostBufferState::Available;
	if (!hostBufferStates_[half].compare_exchange_strong(
		expected, HostBufferState::Announced,
		std::memory_order_acq_rel, std::memory_order_acquire))
	{
		return false;
	}
	if (announceStage(token))
		return true;
	hostBufferStates_[half].store(
		HostBufferState::Available, std::memory_order_release);
	return false;
}

void AsioProxyDriver::expireStage(const AsyncBufferToken& token) noexcept
{
	if (token.bufferIndex < 0 || token.bufferIndex > 1)
		return;
	const std::size_t slotIndex =
		static_cast<std::size_t>(token.callbackEpoch & 1u);
	StageSlot& slot = stageSlots_[slotIndex];
	for (;;)
	{
		std::uint64_t word = slot.word.load(std::memory_order_acquire);
		if (stageSequence(word) != token.callbackEpoch)
			return;
		const StageState state = stageState(word);
		if (state == StageState::Writing)
		{
			if (slot.word.compare_exchange_weak(
				word, makeStageWord(token.callbackEpoch, StageState::Expired),
				std::memory_order_acq_rel, std::memory_order_acquire))
			{
				return;
			}
			continue;
		}
		if (state == StageState::Announced || state == StageState::Ready)
		{
			if (!slot.word.compare_exchange_weak(
				word, makeStageWord(token.callbackEpoch, StageState::Expired),
				std::memory_order_acq_rel, std::memory_order_acquire))
			{
				continue;
			}
			if (state == StageState::Ready)
			{
				// READY means the producer already stopped touching the host half.
				slot.token = {};
				slot.word.store(
					makeStageWord(token.callbackEpoch, StageState::Empty),
					std::memory_order_release);
			}
			// ANNOUNCED may still be filled by a directProcess=false worker. Keep
			// the host half unavailable until its stale outputReady arrives and
			// performs the exclusive token cleanup below.
		}
		return;
	}
}

bool AsioProxyDriver::stageHostOutput(const AsyncBufferToken& token) noexcept
{
	if (token.bufferIndex < 0 || token.bufferIndex > 1 || bufferSize_ <= 0)
	{
		realtimeError_.store(RealtimeError::InvalidBuffer, std::memory_order_release);
		return false;
	}
	if (outputChannels_.empty())
		return true;

	const std::size_t half = static_cast<std::size_t>(token.bufferIndex);
	const std::size_t slotIndex =
		static_cast<std::size_t>(token.callbackEpoch & 1u);
	StageSlot& slot = stageSlots_[slotIndex];
	std::uint64_t expected =
		makeStageWord(token.callbackEpoch, StageState::Announced);
	if (!slot.word.compare_exchange_strong(
		expected, makeStageWord(token.callbackEpoch, StageState::Writing),
		std::memory_order_acq_rel, std::memory_order_acquire))
	{
		if (expected == makeStageWord(token.callbackEpoch, StageState::Expired))
		{
			// A late outputReady is the first proof that an expired deferred
			// producer no longer touches this host half. Clear payload while the
			// slot is still exclusively Expired, then release-publish Empty.
			std::uint64_t expired = expected;
			if (slot.word.compare_exchange_strong(
				expired, makeStageWord(token.callbackEpoch, StageState::Cleaning),
				std::memory_order_acq_rel, std::memory_order_acquire))
			{
				HostBufferState hostExpected = HostBufferState::Announced;
				hostBufferStates_[half].compare_exchange_strong(
					hostExpected, HostBufferState::Available,
					std::memory_order_acq_rel, std::memory_order_acquire);
				slot.token = {};
				slot.word.store(
					makeStageWord(token.callbackEpoch, StageState::Empty),
					std::memory_order_release);
			}
		}
		return false;
	}
	HostBufferState hostExpected = HostBufferState::Announced;
	if (!hostBufferStates_[half].compare_exchange_strong(
		hostExpected, HostBufferState::Processing,
		std::memory_order_acq_rel, std::memory_order_acquire))
	{
		slot.word.store(
			makeStageWord(token.callbackEpoch, StageState::Expired),
			std::memory_order_release);
		slot.word.store(
			makeStageWord(token.callbackEpoch, StageState::Empty),
			std::memory_order_release);
		return false;
	}
	struct HostBufferGuard
	{
		std::atomic<HostBufferState>& state;
		~HostBufferGuard()
		{
			state.store(HostBufferState::Available, std::memory_order_release);
		}
	} hostGuard{hostBufferStates_[half]};

	// Preserve a raw, uncorrected stage first. Every later failure can publish
	// this private copy without modifying a host or vendor buffer.
	for (OutputChannel& output : outputChannels_)
	{
		std::memcpy(
			output.stageBuffers[slotIndex].get(),
			output.hostBuffers[half].get(),
			output.byteCount);
	}

	bool corrected = false;
	bool processingExpected = false;
	if (!processingClaim_.compare_exchange_strong(
		processingExpected, true, std::memory_order_acquire, std::memory_order_relaxed))
	{
		// FilterEngine and the preallocated scratch buffers are single-owner.
		// A concurrent/reentrant completion destroys FIFO/DSP association. Leave
		// this and all later buffers dry until a stop/start boundary.
		asyncDesynchronized_.store(true, std::memory_order_release);
		realtimeError_.store(RealtimeError::InvalidBuffer, std::memory_order_release);
	}
	else
	{
		struct ProcessingClaimGuard
		{
			std::atomic<bool>& claim;
			~ProcessingClaimGuard() { claim.store(false, std::memory_order_release); }
		} guard{processingClaim_};

		bool decoded = true;
		for (std::size_t channel = 0; channel < outputChannels_.size(); ++channel)
		{
			const OutputChannel& output = outputChannels_[channel];
			if (!AsioSampleCodec::decode(
				output.hostBuffers[half].get(),
				output.format,
				decodedPointers_[half][channel],
				static_cast<std::size_t>(bufferSize_)))
			{
				decoded = false;
				break;
			}
		}

		if (decoded &&
			engineReady_.load(std::memory_order_acquire) && dsp_ != nullptr &&
			!asyncDesynchronized_.load(std::memory_order_acquire))
		{
			try
			{
				corrected = dsp_->process(
					processedPointers_[half].data(),
					decodedPointers_[half].data(),
					static_cast<unsigned>(outputChannels_.size()),
					static_cast<unsigned>(bufferSize_));
			}
			catch (...)
			{
				corrected = false;
			}
		}
		if (corrected)
		{
			for (std::size_t channel = 0; channel < outputChannels_.size(); ++channel)
			{
				OutputChannel& output = outputChannels_[channel];
				if (!AsioSampleCodec::encode(
					processedPointers_[half][channel],
					output.format,
					output.stageBuffers[slotIndex].get(),
					static_cast<std::size_t>(bufferSize_)))
				{
					corrected = false;
					break;
				}
			}
		}
	}
	if (!corrected && engineReady_.load(std::memory_order_acquire) &&
		!asyncDesynchronized_.load(std::memory_order_acquire))
	{
		engineReady_.store(false, std::memory_order_release);
		realtimeError_.store(RealtimeError::DspFailed, std::memory_order_release);
	}
	// The vendor callback may have expired this exact sequence while DSP ran.
	// Publishing is one CAS; a late worker can never resurrect the block or turn
	// fixed one-block latency into variable latency.
	expected = makeStageWord(token.callbackEpoch, StageState::Writing);
	if (!slot.word.compare_exchange_strong(
		expected, makeStageWord(token.callbackEpoch, StageState::Ready),
		std::memory_order_release, std::memory_order_acquire))
	{
		if (expected == makeStageWord(token.callbackEpoch, StageState::Expired))
		{
			std::uint64_t expired = expected;
			if (slot.word.compare_exchange_strong(
				expired, makeStageWord(token.callbackEpoch, StageState::Cleaning),
				std::memory_order_acq_rel, std::memory_order_acquire))
			{
				slot.token = {};
				slot.word.store(
					makeStageWord(token.callbackEpoch, StageState::Empty),
					std::memory_order_release);
			}
		}
		return false;
	}
	return true;
}

void AsioProxyDriver::zeroVendorOutput(long bufferIndex) noexcept
{
	if (bufferIndex < 0 || bufferIndex > 1)
		return;
	for (const OutputChannel& output : outputChannels_)
	{
		if (vendorBufferInfos_ == nullptr ||
			output.bufferInfoIndex >= vendorBufferInfos_->size())
			continue;
		void* const raw =
			(*vendorBufferInfos_)[output.bufferInfoIndex].buffers[bufferIndex];
		if (raw != nullptr)
			std::memset(raw, 0, output.byteCount);
	}
}

void AsioProxyDriver::commitNextStageToVendor(long bufferIndex) noexcept
{
	if (outputChannels_.empty())
		return;
	const std::uint64_t sequence =
		nextCommitSequence_.fetch_add(1, std::memory_order_acq_rel);
	const std::size_t slotIndex = static_cast<std::size_t>(sequence & 1u);
	StageSlot& slot = stageSlots_[slotIndex];
	std::uint64_t expected = makeStageWord(sequence, StageState::Ready);
	const bool ready = !asyncDesynchronized_.load(std::memory_order_acquire) &&
		slot.word.compare_exchange_strong(
			expected, makeStageWord(sequence, StageState::Reading),
			std::memory_order_acq_rel, std::memory_order_acquire);
	if (ready)
	{
		for (const OutputChannel& output : outputChannels_)
		{
			void* const destination =
				(*vendorBufferInfos_)[output.bufferInfoIndex].buffers[bufferIndex];
			if (destination == nullptr)
			{
				asyncDesynchronized_.store(true, std::memory_order_release);
				realtimeError_.store(
					RealtimeError::InvalidBuffer, std::memory_order_release);
				break;
			}
			std::memcpy(destination, output.stageBuffers[slotIndex].get(),
				output.byteCount);
		}
		slot.token = {};
		slot.word.store(makeStageWord(sequence, StageState::Empty),
			std::memory_order_release);
	}
	else
	{
		zeroVendorOutput(bufferIndex);
		if (stageSequence(expected) == sequence &&
			stageState(expected) != StageState::Expired &&
			stageState(expected) != StageState::Empty)
			expireStage(slot.token);
		asyncDesynchronized_.store(true, std::memory_order_release);
		realtimeError_.store(RealtimeError::StageMiss, std::memory_order_release);
	}
	// outputReady now describes the vendor buffer committed above. Host
	// outputReady only completes a proxy stage and is never forwarded 1:1.
	signalVendorOutputReady();
}

void AsioProxyDriver::resetStagingState() noexcept
{
	for (auto& state : hostBufferStates_)
		state.store(HostBufferState::Available, std::memory_order_relaxed);
	for (std::size_t index = 0; index < stageSlots_.size(); ++index)
	{
		stageSlots_[index].token = {};
		stageSlots_[index].word.store(
			makeStageWord(index, StageState::Empty), std::memory_order_relaxed);
	}
	for (OutputChannel& output : outputChannels_)
		for (auto& stage : output.stageBuffers)
			if (stage != nullptr)
				std::memset(stage.get(), 0, output.byteCount);
	nextCommitSequence_.store(0, std::memory_order_relaxed);
	lastVendorBufferIndex_ = -1;
}

bool AsioProxyDriver::rebuildDspForStart() noexcept
{
	if (outputChannels_.empty())
	{
		dsp_.reset();
		engineReady_.store(true, std::memory_order_release);
		return true;
	}
	try
	{
		dsp_ = dspFactory_ == nullptr ? nullptr : dspFactory_(
			sampleRate_, static_cast<unsigned>(outputChannels_.size()),
			static_cast<unsigned>(bufferSize_));
	}
	catch (...)
	{
		dsp_.reset();
	}
	const bool ready = dsp_ != nullptr;
	engineReady_.store(ready, std::memory_order_release);
	if (!ready)
		realtimeError_.store(RealtimeError::DspFailed, std::memory_order_release);
	return ready;
}

AsioProxyDriver::AsyncBufferToken AsioProxyDriver::announceBuffer(
	long bufferIndex) noexcept
{
	const std::uint64_t callbackEpoch = callbackEpoch_.fetch_add(
		1, std::memory_order_acq_rel) + 1;
	if (bufferIndex < 0 || bufferIndex > 1)
	{
		return {-1, 0, callbackEpoch};
	}
	const std::size_t index = static_cast<std::size_t>(bufferIndex);
	std::uint64_t generation = announcedGenerations_[index].fetch_add(
		1, std::memory_order_acq_rel) + 1;
	if (generation == 0)
	{
		generation = announcedGenerations_[index].fetch_add(
			1, std::memory_order_acq_rel) + 1;
	}
	return {bufferIndex, generation, callbackEpoch};
}

void AsioProxyDriver::invalidateBuffer(const AsyncBufferToken& token) noexcept
{
	if (token.bufferIndex < 0 || token.bufferIndex > 1 || token.generation == 0)
		return;
	auto& generation = announcedGenerations_[
		static_cast<std::size_t>(token.bufferIndex)];
	std::uint64_t expected = token.generation;
	generation.compare_exchange_strong(
		expected, token.generation + 1,
		std::memory_order_acq_rel, std::memory_order_relaxed);
}

bool AsioProxyDriver::enqueueAsyncBuffer(
	const AsyncBufferToken& token) noexcept
{
	std::uint64_t position = asyncEnqueuePosition_.load(std::memory_order_relaxed);
	for (;;)
	{
		AsyncQueueSlot& slot = asyncQueue_[
			static_cast<std::size_t>(position % kAsyncQueueCapacity)];
		const std::uint64_t sequence = slot.sequence.load(std::memory_order_acquire);
		const std::int64_t difference = static_cast<std::int64_t>(sequence - position);
		if (difference == 0)
		{
			if (asyncEnqueuePosition_.compare_exchange_weak(
				position, position + 1,
				std::memory_order_relaxed, std::memory_order_relaxed))
			{
				slot.token = token;
				slot.sequence.store(position + 1, std::memory_order_release);
				return true;
			}
		}
		else if (difference < 0)
		{
			return false;
		}
		else
		{
			position = asyncEnqueuePosition_.load(std::memory_order_relaxed);
		}
	}
}

bool AsioProxyDriver::dequeueAsyncBuffer(AsyncBufferToken& token) noexcept
{
	std::uint64_t position = asyncDequeuePosition_.load(std::memory_order_relaxed);
	for (;;)
	{
		AsyncQueueSlot& slot = asyncQueue_[
			static_cast<std::size_t>(position % kAsyncQueueCapacity)];
		const std::uint64_t sequence = slot.sequence.load(std::memory_order_acquire);
		const std::int64_t difference =
			static_cast<std::int64_t>(sequence - (position + 1));
		if (difference == 0)
		{
			if (asyncDequeuePosition_.compare_exchange_weak(
				position, position + 1,
				std::memory_order_relaxed, std::memory_order_relaxed))
			{
				token = slot.token;
				slot.sequence.store(
					position + kAsyncQueueCapacity, std::memory_order_release);
				return true;
			}
		}
		else if (difference < 0)
		{
			return false;
		}
		else
		{
			position = asyncDequeuePosition_.load(std::memory_order_relaxed);
		}
	}
}

AsioProxyDriver::AsyncCompletion AsioProxyDriver::completeNextAsyncBuffer() noexcept
{
	AsyncBufferToken token;
	if (!dequeueAsyncBuffer(token))
		return AsyncCompletion::None;
	if (token.bufferIndex < 0 || token.bufferIndex > 1 || token.generation == 0)
	{
		asyncDesynchronized_.store(true, std::memory_order_release);
		realtimeError_.store(RealtimeError::InvalidBuffer, std::memory_order_release);
		return AsyncCompletion::Stale;
	}
	if (announcedGenerations_[static_cast<std::size_t>(token.bufferIndex)].load(
		std::memory_order_acquire) != token.generation)
	{
		asyncDesynchronized_.store(true, std::memory_order_release);
		realtimeError_.store(RealtimeError::InvalidBuffer, std::memory_order_release);
		return AsyncCompletion::Stale;
	}
	if (!stageHostOutput(token))
	{
		// An exact token may already have expired at the next vendor callback.
		// Never replay it into a newer slot; fixed latency is more important than
		// trying to recover an ambiguous completion stream in place.
		asyncDesynchronized_.store(true, std::memory_order_release);
		realtimeError_.store(RealtimeError::StageMiss, std::memory_order_release);
		return AsyncCompletion::Stale;
	}
	return AsyncCompletion::Current;
}

void AsioProxyDriver::discardFailedHostCompletion(
	const AsyncBufferToken& token) noexcept
{
	AsyncBufferToken queued;
	if (!dequeueAsyncBuffer(queued) ||
		queued.bufferIndex != token.bufferIndex ||
		queued.generation != token.generation ||
		queued.callbackEpoch != token.callbackEpoch)
	{
		return;
	}
	// Callers use this only after the host produced an inline outputReady marker,
	// which proves that its deferred producer released H before callback unwind.
	// Reuse Expired->Cleaning to clear payload before publishing the half.
	expireStage(queued);
	(void)stageHostOutput(queued);
}

void AsioProxyDriver::resetAsyncBufferQueue() noexcept
{
	asyncEnqueuePosition_.store(0, std::memory_order_relaxed);
	asyncDequeuePosition_.store(0, std::memory_order_relaxed);
	for (std::uint64_t index = 0; index < kAsyncQueueCapacity; ++index)
	{
		asyncQueue_[static_cast<std::size_t>(index)].token = {};
		asyncQueue_[static_cast<std::size_t>(index)].sequence.store(
			index, std::memory_order_relaxed);
	}
	for (auto& generation : announcedGenerations_)
		generation.store(0, std::memory_order_relaxed);
	callbackEpoch_.store(0, std::memory_order_relaxed);
}

bool AsioProxyDriver::tryEnterCallbackActivity() noexcept
{
	std::uint64_t state = callbackActivity_.load(std::memory_order_seq_cst);
	for (;;)
	{
		if ((state & kCallbackActivityClosed) != 0 ||
			(state & kCallbackActivityCountMask) == kCallbackActivityCountMask)
		{
			return false;
		}
		if (callbackActivity_.compare_exchange_weak(
			state, state + 1,
			std::memory_order_seq_cst, std::memory_order_seq_cst))
		{
			return true;
		}
	}
}

void AsioProxyDriver::leaveCallbackActivity() noexcept
{
	callbackActivity_.fetch_sub(1, std::memory_order_seq_cst);
}

bool AsioProxyDriver::closeCallbackActivityGateAndDrain() noexcept
{
	const std::uint64_t previous = callbackActivity_.fetch_or(
		kCallbackActivityClosed, std::memory_order_seq_cst);
	while ((callbackActivity_.load(std::memory_order_seq_cst) &
		kCallbackActivityCountMask) != 0)
	{
		_mm_pause();
	}
	return (previous & kCallbackActivityClosed) == 0;
}

void AsioProxyDriver::openCallbackActivityGate() noexcept
{
	// Every opener follows a close-and-drain boundary, so no count can be lost.
	callbackActivity_.store(0, std::memory_order_seq_cst);
}

void AsioProxyDriver::drainProcessing() noexcept
{
	while (processingClaim_.load(std::memory_order_acquire))
		_mm_pause();
}

void AsioProxyDriver::signalVendorOutputReady() noexcept
{
	if (vendor_ == nullptr ||
		vendorOutputReadySupport_.load(std::memory_order_acquire) ==
			VendorOutputReadySupport::Unsupported)
	{
		return;
	}
	ASIOError result = ASE_HWMalfunction;
	try
	{
		result = vendor_->outputReady();
	}
	catch (...)
	{
		realtimeError_.store(
			RealtimeError::VendorOutputReadyFailed, std::memory_order_release);
		vendorOutputReadySupport_.store(
			VendorOutputReadySupport::Unsupported, std::memory_order_release);
		return;
	}
	if (result == ASE_NotPresent)
	{
		vendorOutputReadySupport_.store(
			VendorOutputReadySupport::Unsupported, std::memory_order_release);
	}
	else if (result == ASE_OK || result == ASE_SUCCESS)
	{
		vendorOutputReadySupport_.store(
			VendorOutputReadySupport::Supported, std::memory_order_release);
	}
	else
	{
		realtimeError_.store(
			RealtimeError::VendorOutputReadyFailed, std::memory_order_release);
		vendorOutputReadySupport_.store(
			VendorOutputReadySupport::Unsupported, std::memory_order_release);
	}
}

ASIOError AsioProxyDriver::invalidState(const char* message) noexcept
{
	setError(message);
	return ASE_InvalidMode;
}
}
