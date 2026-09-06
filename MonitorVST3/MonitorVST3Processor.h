#pragma once

#include <atomic>
#include <cstdint>
#include <memory>

#include "FilterEngine.h"
#include "public.sdk/source/vst/vstaudioeffect.h"

#include "MonitorVST3Identity.h"

namespace EqualizerAPO::MonitorVST3
{

class MonitorVST3Processor final : public Steinberg::Vst::AudioEffect
{
public:
	MonitorVST3Processor();
	~MonitorVST3Processor() override = default;

	static Steinberg::FUnknown* createInstance(void*)
	{
		return static_cast<Steinberg::Vst::IAudioProcessor*>(
			new MonitorVST3Processor());
	}

	Steinberg::tresult PLUGIN_API initialize(Steinberg::FUnknown* context) override;
	Steinberg::tresult PLUGIN_API terminate() override;
	Steinberg::tresult PLUGIN_API setBusArrangements(
		Steinberg::Vst::SpeakerArrangement* inputs,
		Steinberg::int32 numInputs,
		Steinberg::Vst::SpeakerArrangement* outputs,
		Steinberg::int32 numOutputs) override;
	Steinberg::tresult PLUGIN_API canProcessSampleSize(
		Steinberg::int32 symbolicSampleSize) override;
	Steinberg::tresult PLUGIN_API setupProcessing(
		Steinberg::Vst::ProcessSetup& setup) override;
	Steinberg::tresult PLUGIN_API setActive(Steinberg::TBool state) override;
	Steinberg::tresult PLUGIN_API setProcessing(Steinberg::TBool state) override;
	Steinberg::tresult PLUGIN_API process(Steinberg::Vst::ProcessData& data) override;

	Steinberg::tresult PLUGIN_API setState(Steinberg::IBStream* state) override;
	Steinberg::tresult PLUGIN_API getState(Steinberg::IBStream* state) override;

	MonitorProcessStatus getProcessStatus() const noexcept
	{
		return static_cast<MonitorProcessStatus>(
			processStatus.load(std::memory_order_acquire));
	}

private:
	void updateBypassParameter(
		Steinberg::Vst::IParameterChanges* changes) noexcept;
	void copyDry(Steinberg::Vst::ProcessData& data) noexcept;
	bool buffersAreValid(const Steinberg::Vst::ProcessData& data,
		Steinberg::int32 channelCount) const noexcept;
	void setStatus(MonitorProcessStatus status) noexcept
	{
		processStatus.store(static_cast<std::uint32_t>(status),
			std::memory_order_release);
	}

	std::unique_ptr<FilterEngine> engine;
	std::atomic<bool> bypass{false};
	std::atomic<bool> engineReady{false};
	std::atomic<bool> active{false};
	std::atomic<bool> processing{false};
	std::atomic<bool> offlineMode{false};
	std::atomic<Steinberg::int32> configuredChannelCount{kDefaultChannelCount};
	std::atomic<unsigned> configuredChannelMask{0x3u};
	std::atomic<Steinberg::int32> configuredMaxBlockSize{0};
	std::atomic<std::uint32_t> processStatus{
		static_cast<std::uint32_t>(MonitorProcessStatus::Uninitialized)};
};

static_assert(std::atomic<bool>::is_always_lock_free,
	"The audio callback requires lock-free boolean atomics.");
static_assert(std::atomic<Steinberg::int32>::is_always_lock_free,
	"The audio callback requires lock-free 32-bit atomics.");
static_assert(std::atomic<std::uint32_t>::is_always_lock_free,
	"The audio callback requires lock-free status publication.");

} // namespace EqualizerAPO::MonitorVST3
