#include "AsioCallbackBridge.h"

#include <cstdint>
#include <intrin.h>

namespace HibikiAsio
{
namespace
{
class ReleasingCallbackSink final : public IAsioCallbackSink
{
public:
	void onBufferSwitch(long, ASIOBool) noexcept override {}
	ASIOTime* onBufferSwitchTimeInfo(
		ASIOTime* params, long, ASIOBool) noexcept override { return params; }
	void onSampleRateDidChange(ASIOSampleRate) noexcept override {}
	long onAsioMessage(long, long, void*, double*) noexcept override { return 0; }
};

ReleasingCallbackSink releasingCallbackSink;
}

std::atomic<IAsioCallbackSink*> AsioCallbackBridge::activeSink_{nullptr};
std::atomic<std::uint64_t> AsioCallbackBridge::callbackActivity_{
	AsioCallbackBridge::kCallbackGateClosed};
thread_local unsigned AsioCallbackBridge::callbackDepth_ = 0;
thread_local unsigned AsioCallbackBridge::bufferCallbackDepth_ = 0;
thread_local std::uint32_t AsioCallbackBridge::outputReadyPendingMask_ = 0;
ASIOCallbacks AsioCallbackBridge::callbacks_ = {
	&AsioCallbackBridge::bufferSwitchTrampoline,
	&AsioCallbackBridge::sampleRateDidChangeTrampoline,
	&AsioCallbackBridge::asioMessageTrampoline,
	&AsioCallbackBridge::bufferSwitchTimeInfoTrampoline
};

static_assert(std::atomic<IAsioCallbackSink*>::is_always_lock_free,
	"The ASIO callback owner must be lock-free.");
static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
	"The ASIO callback lifecycle gate must be lock-free.");
AsioCallbackBridge::AsioCallbackBridge() noexcept = default;

AsioCallbackBridge::~AsioCallbackBridge()
{
	release(sink_);
}

bool AsioCallbackBridge::acquire(IAsioCallbackSink* sink) noexcept
{
	if (sink == nullptr)
		return false;
	// Establish the lifetime reference before publishing the raw callback
	// pointer. It remains held until release() has unpublished and drained every
	// trampoline that could have observed the pointer.
	sink->retainCallbackLifetime();
	IAsioCallbackSink* expected = nullptr;
	if (!activeSink_.compare_exchange_strong(
		expected,
		sink,
		std::memory_order_acq_rel,
		std::memory_order_acquire))
	{
		sink->releaseCallbackLifetime();
		return false;
	}
	sink_ = sink;
	openCallbackGate();
	return true;
}

void AsioCallbackBridge::release(IAsioCallbackSink* sink) noexcept
{
	if (sink == nullptr)
		return;
	IAsioCallbackSink* expected = sink;
	if (!activeSink_.compare_exchange_strong(
		expected,
		&releasingCallbackSink,
		std::memory_order_acq_rel,
		std::memory_order_acquire))
	{
		return;
	}
	// The sentinel prevents a second owner while the single atomic gate closes
	// and drains every trampoline that already entered.
	closeCallbackGateAndDrain();
	if (sink_ == sink)
		sink_ = nullptr;
	activeSink_.store(nullptr, std::memory_order_release);
	sink->releaseCallbackLifetime();
}

void AsioCallbackBridge::poison(IAsioCallbackSink* sink) noexcept
{
	if (sink == nullptr)
		return;
	IAsioCallbackSink* expected = sink;
	if (!activeSink_.compare_exchange_strong(
		expected,
		&releasingCallbackSink,
		std::memory_order_acq_rel,
		std::memory_order_acquire))
	{
		return;
	}
	closeCallbackGateAndDrain();
	if (sink_ == sink)
		sink_ = nullptr;
	// Keep the sentinel published for the rest of this process. A vendor that
	// failed teardown may call the static callback table late; it must never be
	// routed to a new proxy instance.
}

void AsioCallbackBridge::abandon(IAsioCallbackSink* sink) noexcept
{
	if (sink == nullptr)
		return;
	IAsioCallbackSink* expected = sink;
	if (!activeSink_.compare_exchange_strong(
		expected,
		&releasingCallbackSink,
		std::memory_order_acq_rel,
		std::memory_order_acquire))
	{
		return;
	}
	// A host released its last ordinary reference while a vendor callback was
	// active (or while buffers were still owned). Closing is non-blocking: the
	// current realtime callback must never spin waiting for itself or a sibling.
	// The acquire-time lifetime reference is deliberately retained, which pins
	// both the sink and this module while the process-lifetime sentinel rejects
	// all later callbacks and owners.
	callbackActivity_.fetch_or(kCallbackGateClosed, std::memory_order_seq_cst);
}

ASIOCallbacks* AsioCallbackBridge::vendorCallbacks() noexcept
{
	return &callbacks_;
}

bool AsioCallbackBridge::isPoisoned() noexcept
{
	return activeSink_.load(std::memory_order_acquire) == &releasingCallbackSink;
}

bool AsioCallbackBridge::isInsideBufferCallback() noexcept
{
	return bufferCallbackDepth_ != 0;
}

bool AsioCallbackBridge::isInsideNestedBufferCallback() noexcept
{
	return bufferCallbackDepth_ > 1;
}

bool AsioCallbackBridge::isInsideCallback() noexcept
{
	return callbackDepth_ != 0;
}

bool AsioCallbackBridge::tryEnterCallback() noexcept
{
	std::uint64_t state = callbackActivity_.load(std::memory_order_seq_cst);
	for (;;)
	{
		if ((state & kCallbackGateClosed) != 0 ||
			(state & kCallbackCountMask) == kCallbackCountMask)
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

void AsioCallbackBridge::leaveCallback() noexcept
{
	callbackActivity_.fetch_sub(1, std::memory_order_seq_cst);
}

void AsioCallbackBridge::closeCallbackGateAndDrain() noexcept
{
	callbackActivity_.fetch_or(kCallbackGateClosed, std::memory_order_seq_cst);
	while ((callbackActivity_.load(std::memory_order_seq_cst) &
		kCallbackCountMask) != 0)
	{
		_mm_pause();
	}
}

void AsioCallbackBridge::openCallbackGate() noexcept
{
	callbackActivity_.store(0, std::memory_order_seq_cst);
}

bool AsioCallbackBridge::deferOutputReady() noexcept
{
	// Worker-thread outputReady calls are never deferred. Check TLS first so
	// they do not touch sink_ while release() is clearing that owner pointer.
	if (bufferCallbackDepth_ == 0)
		return false;
	if (sink_ == nullptr ||
		activeSink_.load(std::memory_order_acquire) != sink_)
	{
		return false;
	}
	if (bufferCallbackDepth_ > 32)
		return false;
	outputReadyPendingMask_ |=
		static_cast<std::uint32_t>(1u << (bufferCallbackDepth_ - 1));
	return true;
}

bool AsioCallbackBridge::consumeDeferredOutputReady() noexcept
{
	if (bufferCallbackDepth_ == 0 || bufferCallbackDepth_ > 32)
		return false;
	const std::uint32_t mask =
		static_cast<std::uint32_t>(1u << (bufferCallbackDepth_ - 1));
	const bool pending = (outputReadyPendingMask_ & mask) != 0;
	outputReadyPendingMask_ &= ~mask;
	return pending;
}

void AsioCallbackBridge::bufferSwitchTrampoline(
	long bufferIndex,
	ASIOBool directProcess) noexcept
{
	if (!tryEnterCallback())
		return;
	IAsioCallbackSink* const sink = activeSink_.load(std::memory_order_acquire);
	if (sink == nullptr || sink == &releasingCallbackSink)
	{
		leaveCallback();
		return;
	}
	if (bufferCallbackDepth_ < 32)
		outputReadyPendingMask_ &=
			~static_cast<std::uint32_t>(1u << bufferCallbackDepth_);
	++callbackDepth_;
	++bufferCallbackDepth_;
	sink->onBufferSwitch(bufferIndex, directProcess);
	--bufferCallbackDepth_;
	--callbackDepth_;
	leaveCallback();
}

ASIOTime* AsioCallbackBridge::bufferSwitchTimeInfoTrampoline(
	ASIOTime* params,
	long bufferIndex,
	ASIOBool directProcess) noexcept
{
	if (!tryEnterCallback())
		return params;
	IAsioCallbackSink* const sink = activeSink_.load(std::memory_order_acquire);
	if (sink == nullptr || sink == &releasingCallbackSink)
	{
		leaveCallback();
		return params;
	}
	if (bufferCallbackDepth_ < 32)
		outputReadyPendingMask_ &=
			~static_cast<std::uint32_t>(1u << bufferCallbackDepth_);
	++callbackDepth_;
	++bufferCallbackDepth_;
	ASIOTime* const result =
		sink->onBufferSwitchTimeInfo(params, bufferIndex, directProcess);
	--bufferCallbackDepth_;
	--callbackDepth_;
	leaveCallback();
	return result;
}

void AsioCallbackBridge::sampleRateDidChangeTrampoline(
	ASIOSampleRate sampleRate) noexcept
{
	if (!tryEnterCallback())
		return;
	IAsioCallbackSink* const sink = activeSink_.load(std::memory_order_acquire);
	if (sink == nullptr || sink == &releasingCallbackSink)
	{
		leaveCallback();
		return;
	}
	++callbackDepth_;
	sink->onSampleRateDidChange(sampleRate);
	--callbackDepth_;
	leaveCallback();
}

long AsioCallbackBridge::asioMessageTrampoline(
	long selector,
	long value,
	void* message,
	double* option) noexcept
{
	if (!tryEnterCallback())
		return 0;
	IAsioCallbackSink* const sink = activeSink_.load(std::memory_order_acquire);
	if (sink == nullptr || sink == &releasingCallbackSink)
	{
		leaveCallback();
		return 0;
	}
	++callbackDepth_;
	const long result = sink->onAsioMessage(selector, value, message, option);
	--callbackDepth_;
	leaveCallback();
	return result;
}
}
