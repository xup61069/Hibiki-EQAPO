#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include <windows.h>
#include <iasiodrv.h>

#include "AsioCallbackBridge.h"
#include "AsioDsp.h"
#include "AsioSampleCodec.h"
#include "AsioTargetStore.h"

namespace HibikiAsio
{
using AsioTargetResolver = ResolvedAsioTarget (*)(HMODULE module);
using AsioVendorActivator = HRESULT (*)(const CLSID& clsid, IASIO** driver);

HRESULT activateAsioVendor(const CLSID& clsid, IASIO** driver);

class AsioProxyDriver final : public IASIO, public IAsioCallbackSink
{
public:
	explicit AsioProxyDriver(
		HMODULE module,
		AsioTargetResolver targetResolver = &resolveAsioTarget,
		AsioVendorActivator vendorActivator = &activateAsioVendor,
		AsioDspFactory dspFactory = &createDefaultAsioDsp) noexcept;
	~AsioProxyDriver() override;

	AsioProxyDriver(const AsioProxyDriver&) = delete;
	AsioProxyDriver& operator=(const AsioProxyDriver&) = delete;

	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override;
	ULONG STDMETHODCALLTYPE AddRef() override;
	ULONG STDMETHODCALLTYPE Release() override;

	ASIOBool init(void* systemHandle) override;
	void getDriverName(char* name) override;
	long getDriverVersion() override;
	void getErrorMessage(char* message) override;
	ASIOError start() override;
	ASIOError stop() override;
	ASIOError getChannels(long* inputChannels, long* outputChannels) override;
	ASIOError getLatencies(long* inputLatency, long* outputLatency) override;
	ASIOError getBufferSize(
		long* minimumSize,
		long* maximumSize,
		long* preferredSize,
		long* granularity) override;
	ASIOError canSampleRate(ASIOSampleRate sampleRate) override;
	ASIOError getSampleRate(ASIOSampleRate* sampleRate) override;
	ASIOError setSampleRate(ASIOSampleRate sampleRate) override;
	ASIOError getClockSources(ASIOClockSource* clocks, long* sourceCount) override;
	ASIOError setClockSource(long reference) override;
	ASIOError getSamplePosition(ASIOSamples* position, ASIOTimeStamp* timestamp) override;
	ASIOError getChannelInfo(ASIOChannelInfo* info) override;
	ASIOError createBuffers(
		ASIOBufferInfo* bufferInfos,
		long channelCount,
		long bufferSize,
		ASIOCallbacks* callbacks) override;
	ASIOError disposeBuffers() override;
	ASIOError controlPanel() override;
	ASIOError future(long selector, void* option) override;
	ASIOError outputReady() override;

	void onBufferSwitch(long bufferIndex, ASIOBool directProcess) noexcept override;
	ASIOTime* onBufferSwitchTimeInfo(
		ASIOTime* params,
		long bufferIndex,
		ASIOBool directProcess) noexcept override;
	void onSampleRateDidChange(ASIOSampleRate sampleRate) noexcept override;
	long onAsioMessage(
		long selector,
		long value,
		void* message,
		double* option) noexcept override;
	void retainCallbackLifetime() noexcept override;
	void releaseCallbackLifetime() noexcept override;

	static long liveInstanceCount() noexcept;

#if defined(HIBIKI_ASIO_TESTING)
	using StartGateHook = void (*)() noexcept;
	void setStartGateHookForTest(StartGateHook hook) noexcept
	{
		startGateHook_ = hook;
	}
#endif

private:
	enum class State : unsigned char
	{
		Constructed,
		Initialized,
		Prepared,
		Running,
		Quarantined
	};

	enum class RealtimeError : unsigned char
	{
		None,
		DspFailed,
		InvalidBuffer,
		StageMiss,
		VendorOutputReadyFailed,
		VendorTeardownFailed
	};

	enum class VendorOutputReadySupport : unsigned char
	{
		Unknown,
		Supported,
		Unsupported
	};
	static_assert(std::atomic<VendorOutputReadySupport>::is_always_lock_free,
		"ASIO outputReady capability state must be lock-free.");

	enum class AsyncCompletion : unsigned char
	{
		None,
		Stale,
		Current
	};

	enum class HostBufferState : unsigned char
	{
		Available,
		Announced,
		Processing
	};

	enum class StageState : unsigned char
	{
		Empty,
		Reserving,
		Announced,
		Writing,
		Ready,
		Reading,
		Expired,
		Cleaning
	};

	struct AsyncBufferToken
	{
		long bufferIndex = -1;
		std::uint64_t generation = 0;
		std::uint64_t callbackEpoch = 0;
	};

	struct AsyncQueueSlot
	{
		std::atomic<std::uint64_t> sequence{0};
		AsyncBufferToken token{};
	};

	struct StageSlot
	{
		std::atomic<std::uint64_t> word{0};
		AsyncBufferToken token{};
	};

	struct OutputChannel
	{
		std::size_t bufferInfoIndex = 0;
		SampleFormat format{};
		std::size_t byteCount = 0;
		std::array<std::unique_ptr<std::max_align_t[]>, 2> hostBuffers{};
		std::array<std::unique_ptr<std::max_align_t[]>, 2> stageBuffers{};
	};

	template <typename Callback>
	auto safeVendorCall(Callback callback, long failure) noexcept -> decltype(callback())
	{
		using Result = decltype(callback());
		try
		{
			return callback();
		}
		catch (...)
		{
			realtimeError_.store(
				RealtimeError::InvalidBuffer, std::memory_order_release);
			return static_cast<Result>(failure);
		}
	}

	IASIO* vendor() const noexcept;
	void safeVendorRelease(IASIO* driver) noexcept;
	void setError(const char* message) noexcept;
	void clearPreparedState() noexcept;
	void forceShutdown() noexcept;
	bool announceStage(const AsyncBufferToken& token) noexcept;
	bool stageHostOutput(const AsyncBufferToken& token) noexcept;
	bool prepareHostBuffer(const AsyncBufferToken& token) noexcept;
	void expireStage(const AsyncBufferToken& token) noexcept;
	void commitNextStageToVendor(long bufferIndex) noexcept;
	void zeroVendorOutput(long bufferIndex) noexcept;
	void resetStagingState() noexcept;
	bool rebuildDspForStart() noexcept;
	static std::uint64_t makeStageWord(
		std::uint64_t sequence, StageState state) noexcept;
	static StageState stageState(std::uint64_t word) noexcept;
	static std::uint64_t stageSequence(std::uint64_t word) noexcept;
	AsyncBufferToken announceBuffer(long bufferIndex) noexcept;
	void invalidateBuffer(const AsyncBufferToken& token) noexcept;
	bool enqueueAsyncBuffer(const AsyncBufferToken& token) noexcept;
	bool dequeueAsyncBuffer(AsyncBufferToken& token) noexcept;
	AsyncCompletion completeNextAsyncBuffer() noexcept;
	void discardFailedHostCompletion(const AsyncBufferToken& token) noexcept;
	void resetAsyncBufferQueue() noexcept;
	bool tryEnterCallbackActivity() noexcept;
	void leaveCallbackActivity() noexcept;
	bool closeCallbackActivityGateAndDrain() noexcept;
	void openCallbackActivityGate() noexcept;
	void drainProcessing() noexcept;
	void signalVendorOutputReady() noexcept;
	ASIOError invalidState(const char* message) noexcept;

	static std::atomic<long> liveInstances_;

	std::atomic<ULONG> referenceCount_{1};
	std::atomic<bool> callbackLifetimeOwned_{false};
	HMODULE module_ = nullptr;
	AsioTargetResolver targetResolver_ = nullptr;
	AsioVendorActivator vendorActivator_ = nullptr;
	AsioDspFactory dspFactory_ = nullptr;
	IASIO* vendor_ = nullptr;
	CLSID vendorClsid_{};
	std::atomic<State> state_{State::Constructed};
	std::array<char, 124> errorMessage_{};
	std::atomic<RealtimeError> realtimeError_{RealtimeError::None};
	std::atomic<bool> engineReady_{false};
	std::atomic<bool> createInProgress_{false};
	std::atomic<bool> createNegotiationActive_{false};
	std::atomic<bool> processingClaim_{false};
	static constexpr std::uint64_t kAsyncQueueCapacity = 4;
	std::array<AsyncQueueSlot, kAsyncQueueCapacity> asyncQueue_{};
	std::array<std::atomic<std::uint64_t>, 2> announcedGenerations_{};
	std::atomic<std::uint64_t> callbackEpoch_{0};
	std::array<std::atomic<HostBufferState>, 2> hostBufferStates_{};
	std::array<StageSlot, 2> stageSlots_{};
	std::atomic<std::uint64_t> nextCommitSequence_{0};
	std::atomic<std::uint64_t> asyncEnqueuePosition_{0};
	std::atomic<std::uint64_t> asyncDequeuePosition_{0};
	// One atomic owns both the lifecycle gate (high bit) and the number of
	// callback/outputReady handlers touching buffer state (low bits). This makes
	// entry versus teardown a single linearizable CAS decision.
	static constexpr std::uint64_t kCallbackActivityClosed =
		std::uint64_t{1} << 63;
	static constexpr std::uint64_t kCallbackActivityCountMask =
		~kCallbackActivityClosed;
	std::atomic<std::uint64_t> callbackActivity_{kCallbackActivityClosed};
	std::atomic<unsigned> activeBufferCallbacks_{0};
	// Incremented before the instance callback gate is consulted so a hardware
	// clock change cannot be lost while start() rebuilds rate-dependent DSP.
	std::atomic<std::uint64_t> sampleRateChangeGeneration_{0};
	std::atomic<bool> asyncDesynchronized_{false};
	// Once an ASIOFalse host producer outlives stop/start failure, outputReady has
	// no index with which to prove which raw H pointer was returned. This latch is
	// intentionally irreversible for the prepared allocation: successful dispose
	// abandons/pins the arena until process exit instead of risking a host UAF.
	std::atomic<bool> startRetryBlocked_{false};
	std::atomic<VendorOutputReadySupport> vendorOutputReadySupport_{
		VendorOutputReadySupport::Unknown};
	ASIOCallbacks hostCallbacks_{};
	AsioCallbackBridge callbackBridge_;
	long bufferSize_ = 0;
	double sampleRate_ = 0.0;
	std::vector<ASIOBufferInfo> bufferInfos_;
	std::unique_ptr<std::vector<ASIOBufferInfo>> vendorBufferInfos_;
	std::vector<OutputChannel> outputChannels_;
	std::array<std::vector<std::unique_ptr<double[]>>, 2> decodedBuffers_;
	std::array<std::vector<std::unique_ptr<double[]>>, 2> processedBuffers_;
	std::array<std::vector<double*>, 2> decodedPointers_;
	std::array<std::vector<double*>, 2> processedPointers_;
	long lastVendorBufferIndex_ = -1;
	std::unique_ptr<IAsioDsp> dsp_;
#if defined(HIBIKI_ASIO_TESTING)
	StartGateHook startGateHook_ = nullptr;
#endif
};
}
