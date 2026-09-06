#pragma once

#include <atomic>
#include <cstdint>

#include <asio.h>

namespace HibikiAsio
{
class IAsioCallbackSink
{
public:
	virtual ~IAsioCallbackSink() = default;
	virtual void onBufferSwitch(long bufferIndex, ASIOBool directProcess) noexcept = 0;
	virtual ASIOTime* onBufferSwitchTimeInfo(
		ASIOTime* params,
		long bufferIndex,
		ASIOBool directProcess) noexcept = 0;
	virtual void onSampleRateDidChange(ASIOSampleRate sampleRate) noexcept = 0;
	virtual long onAsioMessage(
		long selector,
		long value,
		void* message,
		double* option) noexcept = 0;

	// The bridge owns one lifetime reference from acquire() until release().
	// This makes publishing activeSink_ safe: a trampoline never has to load a
	// raw pointer and then race a final external COM Release before AddRef.
	virtual void retainCallbackLifetime() noexcept {}
	virtual void releaseCallbackLifetime() noexcept {}
};

class AsioCallbackBridge final
{
public:
	AsioCallbackBridge() noexcept;
	~AsioCallbackBridge();

	AsioCallbackBridge(const AsioCallbackBridge&) = delete;
	AsioCallbackBridge& operator=(const AsioCallbackBridge&) = delete;

	bool acquire(IAsioCallbackSink* sink) noexcept;
	void release(IAsioCallbackSink* sink) noexcept;
	void poison(IAsioCallbackSink* sink) noexcept;
	void abandon(IAsioCallbackSink* sink) noexcept;
	ASIOCallbacks* vendorCallbacks() noexcept;
	static bool isPoisoned() noexcept;
	static bool isInsideBufferCallback() noexcept;
	static bool isInsideNestedBufferCallback() noexcept;
	static bool isInsideCallback() noexcept;

	// Called from IASIO::outputReady. A request made while the vendor callback
	// is on the stack is acknowledged now and forwarded only after DSP commits.
	bool deferOutputReady() noexcept;
	bool consumeDeferredOutputReady() noexcept;

private:
	static void bufferSwitchTrampoline(
		long bufferIndex,
		ASIOBool directProcess) noexcept;
	static ASIOTime* bufferSwitchTimeInfoTrampoline(
		ASIOTime* params,
		long bufferIndex,
		ASIOBool directProcess) noexcept;
	static void sampleRateDidChangeTrampoline(ASIOSampleRate sampleRate) noexcept;
	static long asioMessageTrampoline(
		long selector,
		long value,
		void* message,
		double* option) noexcept;
	static bool tryEnterCallback() noexcept;
	static void leaveCallback() noexcept;
	static void closeCallbackGateAndDrain() noexcept;
	static void openCallbackGate() noexcept;

	static std::atomic<IAsioCallbackSink*> activeSink_;
	static constexpr std::uint64_t kCallbackGateClosed = std::uint64_t{1} << 63;
	static constexpr std::uint64_t kCallbackCountMask = ~kCallbackGateClosed;
	static std::atomic<std::uint64_t> callbackActivity_;
	static thread_local unsigned callbackDepth_;
	static thread_local unsigned bufferCallbackDepth_;
	static thread_local std::uint32_t outputReadyPendingMask_;
	static ASIOCallbacks callbacks_;

	IAsioCallbackSink* sink_ = nullptr;
};
}
