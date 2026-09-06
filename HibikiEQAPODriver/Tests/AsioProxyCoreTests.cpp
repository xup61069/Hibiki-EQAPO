#include <asio.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

#include <intrin.h>

#include "../AsioCallbackBridge.h"
#include "../AsioProxyIdentity.h"
#include "../AsioSampleCodec.h"
#include "../AsioTargetSelection.h"

namespace
{
int failures = 0;

void check(bool condition, const char* message)
{
	if (!condition)
	{
		std::fprintf(stderr, "FAILED: %s\n", message);
		++failures;
	}
}

void testCodecRoundTrips()
{
	using namespace HibikiAsio;
	const std::array<ASIOSampleType, 18> types = {
		ASIOSTInt16MSB,
		ASIOSTInt24MSB,
		ASIOSTInt32MSB,
		ASIOSTFloat32MSB,
		ASIOSTFloat64MSB,
		ASIOSTInt32MSB16,
		ASIOSTInt32MSB18,
		ASIOSTInt32MSB20,
		ASIOSTInt32MSB24,
		ASIOSTInt16LSB,
		ASIOSTInt24LSB,
		ASIOSTInt32LSB,
		ASIOSTFloat32LSB,
		ASIOSTFloat64LSB,
		ASIOSTInt32LSB16,
		ASIOSTInt32LSB18,
		ASIOSTInt32LSB20,
		ASIOSTInt32LSB24
	};
	const std::array<double, 5> source = {-1.0, -0.25, 0.0, 0.25, 0.999};
	for (ASIOSampleType type : types)
	{
		const SampleFormat format = AsioSampleCodec::describe(type);
		check(format.isSupported(), "PCM format must be supported");
		std::vector<std::uint8_t> raw(source.size() * format.containerBytes);
		std::array<double, source.size()> decoded{};
		check(AsioSampleCodec::encode(source.data(), format, raw.data(), source.size()),
			"PCM encode failed");
		check(AsioSampleCodec::decode(raw.data(), format, decoded.data(), decoded.size()),
			"PCM decode failed");
		const double tolerance = format.encoding == SampleEncoding::Float64 ? 1e-12 :
			(format.encoding == SampleEncoding::Float32 ? 1e-6 :
				2.0 / std::ldexp(1.0, format.validBits - 1));
		for (std::size_t index = 0; index < source.size(); ++index)
			check(std::abs(source[index] - decoded[index]) <= tolerance,
				"PCM round trip exceeded tolerance");
	}
	check(AsioSampleCodec::describe(ASIOSTDSDInt8LSB1).isDsd,
		"DSD must be identified");
	check(!AsioSampleCodec::describe(ASIOSTDSDInt8LSB1).isSupported(),
		"DSD must be rejected");
}

void testCodecClipsAndSanitizes()
{
	using namespace HibikiAsio;
	const SampleFormat integer = AsioSampleCodec::describe(ASIOSTInt16LSB);
	const std::array<double, 4> source = {
		-2.0,
		2.0,
		std::numeric_limits<double>::infinity(),
		std::numeric_limits<double>::quiet_NaN()
	};
	std::array<std::int16_t, 4> raw{};
	std::array<double, 4> decoded{};
	AsioSampleCodec::encode(source.data(), integer, raw.data(), source.size());
	AsioSampleCodec::decode(raw.data(), integer, decoded.data(), decoded.size());
	check(raw[0] == (std::numeric_limits<std::int16_t>::min)(),
		"negative integer must clip");
	check(raw[1] == (std::numeric_limits<std::int16_t>::max)(),
		"positive integer must clip");
	check(raw[2] == 0 && raw[3] == 0, "non-finite values must become silence");

	const SampleFormat float32 = AsioSampleCodec::describe(ASIOSTFloat32LSB);
	const std::array<double, 4> extremeFloatSource = {
		(std::numeric_limits<double>::max)(),
		(std::numeric_limits<double>::lowest)(),
		std::numeric_limits<double>::infinity(),
		std::numeric_limits<double>::quiet_NaN()
	};
	std::array<float, 4> floatRaw{};
	check(AsioSampleCodec::encode(
		extremeFloatSource.data(), float32, floatRaw.data(), floatRaw.size()),
		"float32 extreme encode failed");
	check(floatRaw[0] == (std::numeric_limits<float>::max)() &&
		floatRaw[1] == (std::numeric_limits<float>::lowest)(),
		"finite double overflow must saturate to finite float32");
	check(floatRaw[2] == 0.0f && floatRaw[3] == 0.0f,
		"non-finite float32 values must become silence");
}

void testCodecExternalBytePatternsAndUnalignedStorage()
{
	using namespace HibikiAsio;
	const std::array<double, 2> expected = {-1.0, 0.5};
	const std::array<std::uint8_t, 8> lsb16In32 = {
		0x00, 0x80, 0xFF, 0xFF, 0x00, 0x40, 0x00, 0x00
	};
	const std::array<std::uint8_t, 8> msb16In32 = {
		0xFF, 0xFF, 0x80, 0x00, 0x00, 0x00, 0x40, 0x00
	};
	for (const auto& test : {
		std::pair{ASIOSTInt32LSB16, lsb16In32},
		std::pair{ASIOSTInt32MSB16, msb16In32}})
	{
		const SampleFormat format = AsioSampleCodec::describe(test.first);
		std::array<std::uint8_t, 10> unaligned{};
		unaligned.fill(0xCC);
		std::copy(test.second.begin(), test.second.end(), unaligned.begin() + 1);
		std::array<double, 2> decoded{};
		check(AsioSampleCodec::decode(
			unaligned.data() + 1, format, decoded.data(), decoded.size()),
			"external right-aligned Int32 pattern must decode");
		check(decoded[0] == expected[0] && decoded[1] == expected[1],
			"right-aligned Int32 sign extension/endian decode mismatch");

		unaligned.fill(0xCC);
		check(AsioSampleCodec::encode(
			expected.data(), format, unaligned.data() + 1, expected.size()),
			"unaligned right-aligned Int32 encode must succeed");
		check(std::equal(
			test.second.begin(), test.second.end(), unaligned.begin() + 1),
			"right-aligned Int32 encoded bytes must match the external ABI pattern");
		check(unaligned.front() == 0xCC && unaligned.back() == 0xCC,
			"unaligned encode must not overwrite guard bytes");
	}

	const std::array<std::uint8_t, 6> int24BigEndian = {
		0x80, 0x00, 0x00, 0x40, 0x00, 0x00
	};
	std::array<double, 2> decoded24{};
	check(AsioSampleCodec::decode(
		int24BigEndian.data(), AsioSampleCodec::describe(ASIOSTInt24MSB),
		decoded24.data(), decoded24.size()),
		"external big-endian Int24 pattern must decode");
	check(decoded24[0] == -1.0 && decoded24[1] == 0.5,
		"big-endian Int24 sign extension mismatch");

	const std::array<std::uint8_t, 4> float32BigEndian = {0x3F, 0x00, 0x00, 0x00};
	double decodedFloat = 0.0;
	check(AsioSampleCodec::decode(
		float32BigEndian.data(), AsioSampleCodec::describe(ASIOSTFloat32MSB),
		&decodedFloat, 1) && decodedFloat == 0.5,
		"external big-endian IEEE float pattern must decode");
}

void testIntegerCodecIdentityPreservesEveryBoundaryCode()
{
	using namespace HibikiAsio;
	const std::array<std::int16_t, 8> raw = {
		(std::numeric_limits<std::int16_t>::min)(),
		static_cast<std::int16_t>(-32767),
		static_cast<std::int16_t>(-2),
		static_cast<std::int16_t>(-1),
		static_cast<std::int16_t>(0),
		static_cast<std::int16_t>(1),
		static_cast<std::int16_t>(32766),
		(std::numeric_limits<std::int16_t>::max)()
	};
	std::array<double, raw.size()> decoded{};
	std::array<std::int16_t, raw.size()> encoded{};
	const SampleFormat format = AsioSampleCodec::describe(ASIOSTInt16LSB);
	check(AsioSampleCodec::decode(raw.data(), format, decoded.data(), raw.size()),
		"integer boundary codes must decode");
	check(AsioSampleCodec::encode(decoded.data(), format, encoded.data(), raw.size()),
		"decoded integer boundary codes must re-encode");
	check(encoded == raw,
		"identity DSP must preserve positive and negative full-scale PCM codes exactly");
}

void testRightAlignedInt32AcceptsZeroOrSignExtendedPadding()
{
	using namespace HibikiAsio;
	struct Case
	{
		ASIOSampleType type;
		std::array<std::uint8_t, 4> zeroPaddedNegative;
		std::array<std::uint8_t, 4> signExtendedNegative;
	};
	const std::array<Case, 8> cases = {{
		{ASIOSTInt32LSB16, {0x00, 0x80, 0x00, 0x00}, {0x00, 0x80, 0xFF, 0xFF}},
		{ASIOSTInt32MSB16, {0x00, 0x00, 0x80, 0x00}, {0xFF, 0xFF, 0x80, 0x00}},
		{ASIOSTInt32LSB18, {0x00, 0x00, 0x02, 0x00}, {0x00, 0x00, 0xFE, 0xFF}},
		{ASIOSTInt32MSB18, {0x00, 0x02, 0x00, 0x00}, {0xFF, 0xFE, 0x00, 0x00}},
		{ASIOSTInt32LSB20, {0x00, 0x00, 0x08, 0x00}, {0x00, 0x00, 0xF8, 0xFF}},
		{ASIOSTInt32MSB20, {0x00, 0x08, 0x00, 0x00}, {0xFF, 0xF8, 0x00, 0x00}},
		{ASIOSTInt32LSB24, {0x00, 0x00, 0x80, 0x00}, {0x00, 0x00, 0x80, 0xFF}},
		{ASIOSTInt32MSB24, {0x00, 0x80, 0x00, 0x00}, {0xFF, 0x80, 0x00, 0x00}}
	}};
	for (const Case& test : cases)
	{
		double zeroPadded = 0.0;
		double signExtended = 0.0;
		const SampleFormat format = AsioSampleCodec::describe(test.type);
		check(AsioSampleCodec::decode(
			test.zeroPaddedNegative.data(), format, &zeroPadded, 1),
			"zero-padded right-aligned negative sample must decode");
		check(AsioSampleCodec::decode(
			test.signExtendedNegative.data(), format, &signExtended, 1),
			"sign-extended right-aligned negative sample must decode");
		check(zeroPadded == -1.0 && signExtended == -1.0,
			"unused Int32 padding bits must not change the decoded signal");
	}
}

CLSID makeClsid(unsigned long value)
{
	return {value, 0, 0, {0, 0, 0, 0, 0, 0, 0, 0}};
}

void testTargetSelection()
{
	using namespace HibikiAsio;
	const CLSID own = kDriverClsid;
	const CLSID first = makeClsid(1);
	const CLSID second = makeClsid(2);
	std::vector<AsioDriverCandidate> candidates = {
		{first, L"first", L"first.dll", true, false},
		{second, L"second", L"second.dll", true, false}
	};
	TargetSelection result = selectTarget(own, first, second, candidates);
	check(result.status == TargetSelectionStatus::Selected &&
		result.candidateIndex == 0, "HKCU override must win");
	result = selectTarget(own, std::nullopt, second, candidates);
	check(result.status == TargetSelectionStatus::Selected &&
		result.candidateIndex == 1, "HKLM default must be used second");
	result = selectTarget(own, std::nullopt, std::nullopt, candidates);
	check(result.status == TargetSelectionStatus::Ambiguous,
		"multiple unconfigured targets must not be guessed");
	candidates.resize(1);
	result = selectTarget(own, std::nullopt, std::nullopt, candidates);
	check(result.status == TargetSelectionStatus::Selected,
		"one usable target may be selected automatically");
	result = selectTarget(own, own, std::nullopt, candidates);
	check(result.status == TargetSelectionStatus::SelfTarget,
		"the proxy must never target its own CLSID");
}

class TestSink final : public HibikiAsio::IAsioCallbackSink
{
public:
	explicit TestSink(HibikiAsio::AsioCallbackBridge& bridge) : bridge_(bridge) {}

	void onBufferSwitch(long, ASIOBool) noexcept override
	{
		++bufferSwitchCount;
		deferredInsideCallback = bridge_.deferOutputReady();
		consumedInsideCallback = bridge_.consumeDeferredOutputReady();
	}

	ASIOTime* onBufferSwitchTimeInfo(
		ASIOTime* params, long, ASIOBool) noexcept override
	{
		return params;
	}

	void onSampleRateDidChange(ASIOSampleRate) noexcept override {}
	long onAsioMessage(long, long, void*, double*) noexcept override { return 0; }

	HibikiAsio::AsioCallbackBridge& bridge_;
	int bufferSwitchCount = 0;
	bool deferredInsideCallback = false;
	bool consumedInsideCallback = false;
};

void testSingleActiveCallbackOwner()
{
	using namespace HibikiAsio;
	AsioCallbackBridge firstBridge;
	AsioCallbackBridge secondBridge;
	TestSink first(firstBridge);
	TestSink second(secondBridge);
	check(firstBridge.acquire(&first), "first callback owner must acquire");
	check(!secondBridge.acquire(&second), "second callback owner must be rejected");
	firstBridge.vendorCallbacks()->bufferSwitch(0, ASIOTrue);
	check(first.bufferSwitchCount == 1 && second.bufferSwitchCount == 0,
		"callbacks must dispatch only to the active owner");
	firstBridge.release(&first);
	check(secondBridge.acquire(&second), "ownership must be reusable after release");
	secondBridge.release(&second);
}

void testDeferredOutputReady()
{
	using namespace HibikiAsio;
	AsioCallbackBridge bridge;
	TestSink sink(bridge);
	check(!bridge.deferOutputReady(), "outputReady outside callback is not deferred");
	check(bridge.acquire(&sink), "callback owner must acquire");
	bridge.vendorCallbacks()->bufferSwitch(0, ASIOTrue);
	check(sink.deferredInsideCallback && sink.consumedInsideCallback,
		"outputReady must defer and consume inside callback");
	check(!bridge.consumeDeferredOutputReady(), "deferred request must be coalesced once");
	bridge.release(&sink);
}

class NestedDeferredSink final : public HibikiAsio::IAsioCallbackSink
{
public:
	explicit NestedDeferredSink(HibikiAsio::AsioCallbackBridge& bridge) :
		bridge_(bridge) {}

	void onBufferSwitch(long index, ASIOBool) noexcept override
	{
		if (index == 0)
		{
			outerDeferred = bridge_.deferOutputReady();
			bridge_.vendorCallbacks()->bufferSwitch(1, ASIOTrue);
			outerConsumed = bridge_.consumeDeferredOutputReady();
		}
		else
		{
			nestedConsumed = bridge_.consumeDeferredOutputReady();
		}
	}
	ASIOTime* onBufferSwitchTimeInfo(
		ASIOTime* value, long, ASIOBool) noexcept override { return value; }
	void onSampleRateDidChange(ASIOSampleRate) noexcept override {}
	long onAsioMessage(long, long, void*, double*) noexcept override { return 0; }

	HibikiAsio::AsioCallbackBridge& bridge_;
	bool outerDeferred = false;
	bool outerConsumed = false;
	bool nestedConsumed = false;
};

void testNestedDeferredOutputReadyDoesNotCrossCallbackDepth()
{
	using namespace HibikiAsio;
	AsioCallbackBridge bridge;
	NestedDeferredSink sink(bridge);
	check(bridge.acquire(&sink), "nested deferred callback owner must acquire");
	bridge.vendorCallbacks()->bufferSwitch(0, ASIOTrue);
	check(sink.outerDeferred && sink.outerConsumed && !sink.nestedConsumed,
		"nested callback must not consume its caller's deferred outputReady");
	bridge.release(&sink);
}

class CrossThreadDeferredSink final : public HibikiAsio::IAsioCallbackSink
{
public:
	explicit CrossThreadDeferredSink(HibikiAsio::AsioCallbackBridge& bridge) :
		bridge_(bridge) {}

	void onBufferSwitch(long index, ASIOBool) noexcept override
	{
		if (index == 0)
		{
			firstDeferred = bridge_.deferOutputReady();
			firstReady.store(true, std::memory_order_release);
			while (!secondConsumed.load(std::memory_order_acquire))
				_mm_pause();
			firstConsumed = bridge_.consumeDeferredOutputReady();
		}
		else
		{
			while (!firstReady.load(std::memory_order_acquire))
				_mm_pause();
			secondSawReady = bridge_.consumeDeferredOutputReady();
			secondConsumed.store(true, std::memory_order_release);
		}
	}
	ASIOTime* onBufferSwitchTimeInfo(
		ASIOTime* value, long, ASIOBool) noexcept override { return value; }
	void onSampleRateDidChange(ASIOSampleRate) noexcept override {}
	long onAsioMessage(long, long, void*, double*) noexcept override { return 0; }

	HibikiAsio::AsioCallbackBridge& bridge_;
	std::atomic<bool> firstReady{false};
	std::atomic<bool> secondConsumed{false};
	bool firstDeferred = false;
	bool firstConsumed = false;
	bool secondSawReady = false;
};

void testDeferredOutputReadyIsThreadLocal()
{
	using namespace HibikiAsio;
	AsioCallbackBridge bridge;
	CrossThreadDeferredSink sink(bridge);
	check(bridge.acquire(&sink), "cross-thread deferred callback owner must acquire");
	std::thread first([&]() {
		bridge.vendorCallbacks()->bufferSwitch(0, ASIOTrue);
	});
	std::thread second([&]() {
		bridge.vendorCallbacks()->bufferSwitch(1, ASIOTrue);
	});
	first.join();
	second.join();
	check(sink.firstDeferred && sink.firstConsumed && !sink.secondSawReady,
		"one callback thread must not consume another thread's outputReady request");
	bridge.release(&sink);
}

class BlockingSink final : public HibikiAsio::IAsioCallbackSink
{
public:
	void retainCallbackLifetime() noexcept override
	{
		lifetimeReferences.fetch_add(1, std::memory_order_acq_rel);
	}
	void releaseCallbackLifetime() noexcept override
	{
		lifetimeReferences.fetch_sub(1, std::memory_order_acq_rel);
	}
	void onBufferSwitch(long, ASIOBool) noexcept override
	{
		entered.store(true, std::memory_order_release);
		while (!mayLeave.load(std::memory_order_acquire))
			_mm_pause();
		++completed;
	}
	ASIOTime* onBufferSwitchTimeInfo(ASIOTime* value, long, ASIOBool) noexcept override
	{
		return value;
	}
	void onSampleRateDidChange(ASIOSampleRate) noexcept override {}
	long onAsioMessage(long, long, void*, double*) noexcept override { return 0; }

	std::atomic<bool> entered{false};
	std::atomic<bool> mayLeave{false};
	std::atomic<unsigned> completed{0};
	std::atomic<unsigned> lifetimeReferences{0};
};

void testCallbackReleaseDrainsInFlight()
{
	using namespace HibikiAsio;
	AsioCallbackBridge bridge;
	AsioCallbackBridge nextBridge;
	BlockingSink sink;
	TestSink nextSink(nextBridge);
	check(bridge.acquire(&sink), "blocking callback owner must acquire");
	check(sink.lifetimeReferences.load(std::memory_order_acquire) == 1,
		"bridge acquire must retain the sink before callback publication");
	std::thread callbackThread([&bridge]() {
		bridge.vendorCallbacks()->bufferSwitch(0, ASIOTrue);
	});
	while (!sink.entered.load(std::memory_order_acquire))
		_mm_pause();
	std::atomic<bool> releaseReturned{false};
	std::thread releaseThread([&]() {
		bridge.release(&sink);
		releaseReturned.store(true, std::memory_order_release);
	});
	while (!AsioCallbackBridge::isPoisoned())
		_mm_pause();
	check(!releaseReturned.load(std::memory_order_acquire),
		"release must drain an in-flight callback before returning");
	check(sink.lifetimeReferences.load(std::memory_order_acquire) == 1,
		"bridge must retain callback lifetime throughout the drain");
	check(!nextBridge.acquire(&nextSink),
		"a new callback owner must not acquire during the release drain");
	sink.mayLeave.store(true, std::memory_order_release);
	callbackThread.join();
	releaseThread.join();
	check(releaseReturned.load(std::memory_order_acquire) &&
		sink.completed.load(std::memory_order_acquire) == 1 &&
		sink.lifetimeReferences.load(std::memory_order_acquire) == 0,
		"callback must complete before ownership is released");
	bridge.vendorCallbacks()->bufferSwitch(0, ASIOTrue);
	check(sink.completed.load(std::memory_order_acquire) == 1,
		"callback must not dispatch after release");
	check(nextBridge.acquire(&nextSink),
		"a new callback owner may acquire after release fully drains");
	nextBridge.release(&nextSink);
}

void testWorkerOutputReadyDoesNotRaceBridgeRelease()
{
	using namespace HibikiAsio;
	AsioCallbackBridge bridge;
	TestSink sink(bridge);
	check(bridge.acquire(&sink), "worker/release callback owner must acquire");
	std::atomic<bool> keepRunning{true};
	std::atomic<bool> incorrectlyDeferred{false};
	std::thread worker([&]() {
		while (keepRunning.load(std::memory_order_acquire))
		{
			if (bridge.deferOutputReady())
				incorrectlyDeferred.store(true, std::memory_order_release);
		}
	});
	bridge.release(&sink);
	keepRunning.store(false, std::memory_order_release);
	worker.join();
	check(!incorrectlyDeferred.load(std::memory_order_acquire),
		"worker-thread outputReady must bypass callback-only owner state during release");
}
}

int main()
{
	testCodecRoundTrips();
	testCodecClipsAndSanitizes();
	testCodecExternalBytePatternsAndUnalignedStorage();
	testIntegerCodecIdentityPreservesEveryBoundaryCode();
	testRightAlignedInt32AcceptsZeroOrSignExtendedPadding();
	testTargetSelection();
	testSingleActiveCallbackOwner();
	testDeferredOutputReady();
	testNestedDeferredOutputReadyDoesNotCrossCallbackDepth();
	testDeferredOutputReadyIsThreadLocal();
	testCallbackReleaseDrainsInFlight();
	testWorkerOutputReadyDoesNotRaceBridgeRelease();
	if (failures == 0)
		std::puts("All ASIO proxy core tests passed.");
	return failures == 0 ? 0 : 1;
}
