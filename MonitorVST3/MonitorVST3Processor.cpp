#include "MonitorVST3Processor.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

#include "MonitorVST3State.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/vstspeaker.h"

namespace EqualizerAPO::MonitorVST3
{

namespace
{

Steinberg::uint64 channelMask(Steinberg::int32 channelCount) noexcept
{
	if (channelCount <= 0)
		return 0;
	if (channelCount >= 64)
		return ~Steinberg::uint64{0};
	return (Steinberg::uint64{1} << channelCount) - 1;
}

constexpr Steinberg::uint64 windowsSpeakerBits =
	(Steinberg::uint64{1} << 18) - 1;

bool isSupportedSpeakerArrangement(
	Steinberg::Vst::SpeakerArrangement arrangement) noexcept
{
	// FilterEngine consumes a Windows KSAUDIO speaker mask.  VST3 reuses bits
	// 0..17 for those positions, but all higher VST3 positions have different
	// semantics and must not be silently re-labelled as Windows channels.
	return arrangement == Steinberg::Vst::SpeakerArr::kMono ||
		(arrangement != Steinberg::Vst::SpeakerArr::kEmpty &&
			(arrangement & ~windowsSpeakerBits) == 0);
}

unsigned toFilterEngineChannelMask(
	Steinberg::Vst::SpeakerArrangement arrangement) noexcept
{
	// VST3 speaker bits 0..17 use the same positions as the Windows
	// KSAUDIO_SPEAKER mask consumed by FilterEngine. VST3 mono is special.
	if (arrangement == Steinberg::Vst::SpeakerArr::kMono)
		return 1u << 2;
	return static_cast<unsigned>(arrangement & windowsSpeakerBits);
}

template <typename Sample>
void copyDryChannels(Steinberg::Vst::AudioBusBuffers& output,
	const Steinberg::Vst::AudioBusBuffers* input,
	Sample** outputChannels,
	Sample** inputChannels,
	Steinberg::int32 frameCount) noexcept
{
	Steinberg::uint64 silenceFlags = input == nullptr ?
		channelMask(output.numChannels) : input->silenceFlags;
	const std::size_t byteCount = frameCount > 0 ?
		static_cast<std::size_t>(frameCount) * sizeof(Sample) : 0;

	for (Steinberg::int32 channel = 0; channel < output.numChannels; ++channel)
	{
		Sample* destination = outputChannels == nullptr ? nullptr : outputChannels[channel];
		Sample* source = input == nullptr || inputChannels == nullptr ||
			channel >= input->numChannels ? nullptr : inputChannels[channel];

		if (destination != nullptr && byteCount != 0)
		{
			if (source != nullptr)
			{
				if (source != destination)
					std::memmove(destination, source, byteCount);
			}
			else
			{
				std::memset(destination, 0, byteCount);
			}
		}

		if (source == nullptr && channel < 64)
			silenceFlags |= Steinberg::uint64{1} << channel;
	}

	output.silenceFlags = silenceFlags & channelMask(output.numChannels);
}

} // namespace

MonitorVST3Processor::MonitorVST3Processor()
{
	setControllerClass(kMonitorControllerUid);
}

Steinberg::tresult PLUGIN_API MonitorVST3Processor::initialize(
	Steinberg::FUnknown* context)
{
	const Steinberg::tresult result = AudioEffect::initialize(context);
	if (result != Steinberg::kResultOk)
		return result;

	addAudioInput(STR16("Main Input"), Steinberg::Vst::SpeakerArr::kStereo);
	addAudioOutput(STR16("Main Output"), Steinberg::Vst::SpeakerArr::kStereo);
	configuredChannelCount.store(kDefaultChannelCount, std::memory_order_release);
	configuredChannelMask.store(
		toFilterEngineChannelMask(Steinberg::Vst::SpeakerArr::kStereo),
		std::memory_order_release);
	setStatus(MonitorProcessStatus::Uninitialized);
	return Steinberg::kResultOk;
}

Steinberg::tresult PLUGIN_API MonitorVST3Processor::terminate()
{
	// VST3 requires the host to stop processing and deactivate the component
	// before terminate(). Release the watcher-backed engine at that explicit
	// non-audio lifecycle boundary instead of waiting for object destruction.
	processing.store(false, std::memory_order_release);
	active.store(false, std::memory_order_release);
	engineReady.store(false, std::memory_order_release);
	engine.reset();
	configuredMaxBlockSize.store(0, std::memory_order_release);
	offlineMode.store(false, std::memory_order_release);
	setStatus(MonitorProcessStatus::Uninitialized);
	return AudioEffect::terminate();
}

Steinberg::tresult PLUGIN_API MonitorVST3Processor::setBusArrangements(
	Steinberg::Vst::SpeakerArrangement* inputs,
	Steinberg::int32 numInputs,
	Steinberg::Vst::SpeakerArrangement* outputs,
	Steinberg::int32 numOutputs)
{
	if (inputs == nullptr || outputs == nullptr || numInputs != 1 || numOutputs != 1 ||
		inputs[0] != outputs[0])
	{
		return Steinberg::kResultFalse;
	}

	const Steinberg::int32 channelCount =
		Steinberg::Vst::SpeakerArr::getChannelCount(inputs[0]);
	if (channelCount <= 0 || channelCount > kMaximumChannelCount ||
		!isSupportedSpeakerArrangement(inputs[0]))
		return Steinberg::kResultFalse;

	const Steinberg::tresult result =
		AudioEffect::setBusArrangements(inputs, numInputs, outputs, numOutputs);
	if (result == Steinberg::kResultOk)
	{
		configuredChannelCount.store(channelCount, std::memory_order_release);
		configuredChannelMask.store(
			toFilterEngineChannelMask(inputs[0]),
			std::memory_order_release);
		engineReady.store(false, std::memory_order_release);
		setStatus(MonitorProcessStatus::Uninitialized);
	}
	return result;
}

Steinberg::tresult PLUGIN_API MonitorVST3Processor::canProcessSampleSize(
	Steinberg::int32 symbolicSampleSize)
{
	return symbolicSampleSize == Steinberg::Vst::kSample32 ||
		symbolicSampleSize == Steinberg::Vst::kSample64 ?
		Steinberg::kResultTrue : Steinberg::kResultFalse;
}

Steinberg::tresult PLUGIN_API MonitorVST3Processor::setupProcessing(
	Steinberg::Vst::ProcessSetup& setup)
{
	engineReady.store(false, std::memory_order_release);
	engine.reset();
	configuredMaxBlockSize.store(0, std::memory_order_release);
	offlineMode.store(setup.processMode == Steinberg::Vst::ProcessModes::kOffline,
		std::memory_order_release);
	setStatus(MonitorProcessStatus::Uninitialized);

	const Steinberg::tresult baseResult = AudioEffect::setupProcessing(setup);
	if (baseResult != Steinberg::kResultOk)
		return baseResult;
	if (setup.processMode == Steinberg::Vst::ProcessModes::kOffline)
	{
		setStatus(MonitorProcessStatus::OfflineDry);
		return Steinberg::kResultOk;
	}

	const Steinberg::int32 channelCount =
		configuredChannelCount.load(std::memory_order_acquire);
	if (!std::isfinite(setup.sampleRate) || setup.sampleRate <= 0.0 ||
		setup.maxSamplesPerBlock <= 0 || channelCount <= 0 ||
		channelCount > kMaximumChannelCount ||
		canProcessSampleSize(setup.symbolicSampleSize) != Steinberg::kResultTrue)
	{
		return Steinberg::kResultOk;
	}

	configuredMaxBlockSize.store(setup.maxSamplesPerBlock, std::memory_order_release);
	try
	{
		std::unique_ptr<FilterEngine> newEngine =
			std::make_unique<FilterEngine>();
		newEngine->setDeviceInfo(
			false,
			true,
			kMonitorDeviceName,
			kMonitorConnectionName,
			kMonitorDeviceGuid,
			kMonitorDeviceString);
		newEngine->initialize(
			static_cast<float>(setup.sampleRate),
			static_cast<unsigned>(channelCount),
			static_cast<unsigned>(channelCount),
			static_cast<unsigned>(channelCount),
			configuredChannelMask.load(std::memory_order_acquire),
			static_cast<unsigned>(setup.maxSamplesPerBlock));

		const bool ready = newEngine->hasActiveConfiguration() &&
			newEngine->getMaxFrameCount() ==
			static_cast<unsigned>(setup.maxSamplesPerBlock) &&
			newEngine->getInputChannelCount() == static_cast<unsigned>(channelCount) &&
			newEngine->getOutputChannelCount() == static_cast<unsigned>(channelCount);
		if (ready)
			engine = std::move(newEngine);
		engineReady.store(ready, std::memory_order_release);
		setStatus(ready ? MonitorProcessStatus::Ready :
			MonitorProcessStatus::EngineFailure);
	}
	catch (...)
	{
		engineReady.store(false, std::memory_order_release);
		setStatus(MonitorProcessStatus::EngineFailure);
	}

	return Steinberg::kResultOk;
}

Steinberg::tresult PLUGIN_API MonitorVST3Processor::setActive(Steinberg::TBool state)
{
	if (!state)
		active.store(false, std::memory_order_release);

	const Steinberg::tresult result = AudioEffect::setActive(state);
	if (state && result == Steinberg::kResultOk)
		active.store(true, std::memory_order_release);
	return result;
}

Steinberg::tresult PLUGIN_API MonitorVST3Processor::setProcessing(Steinberg::TBool state)
{
	// AudioEffect's default implementation returns kNotImplemented. This
	// processor has no additional start/stop work after setupProcessing(), so
	// publishing the host state is the complete implementation.
	processing.store(state != 0, std::memory_order_release);
	return Steinberg::kResultOk;
}

void MonitorVST3Processor::updateBypassParameter(
	Steinberg::Vst::IParameterChanges* changes) noexcept
{
	if (changes == nullptr)
		return;

	const Steinberg::int32 queueCount = (std::min)(
		changes->getParameterCount(), kMaximumParameterQueuesPerBlock);
	for (Steinberg::int32 index = 0; index < queueCount; ++index)
	{
		Steinberg::Vst::IParamValueQueue* queue = changes->getParameterData(index);
		if (queue == nullptr || queue->getParameterId() != kMonitorBypassParamId)
			continue;

		const Steinberg::int32 pointCount = queue->getPointCount();
		if (pointCount <= 0)
			continue;

		Steinberg::int32 sampleOffset = 0;
		Steinberg::Vst::ParamValue value = 0.0;
		if (queue->getPoint(pointCount - 1, sampleOffset, value) == Steinberg::kResultTrue)
			bypass.store(value >= 0.5, std::memory_order_release);
	}
}

bool MonitorVST3Processor::buffersAreValid(
	const Steinberg::Vst::ProcessData& data,
	Steinberg::int32 channelCount) const noexcept
{
	if (data.inputs == nullptr || data.outputs == nullptr ||
		data.numInputs != 1 || data.numOutputs != 1 ||
		data.inputs[0].numChannels != channelCount ||
		data.outputs[0].numChannels != channelCount)
	{
		return false;
	}

	if (data.symbolicSampleSize == Steinberg::Vst::kSample32)
	{
		if (data.inputs[0].channelBuffers32 == nullptr ||
			data.outputs[0].channelBuffers32 == nullptr)
		{
			return false;
		}
		for (Steinberg::int32 channel = 0; channel < channelCount; ++channel)
		{
			if (data.inputs[0].channelBuffers32[channel] == nullptr ||
				data.outputs[0].channelBuffers32[channel] == nullptr)
			{
				return false;
			}
		}
		return true;
	}

	if (data.symbolicSampleSize == Steinberg::Vst::kSample64)
	{
		if (data.inputs[0].channelBuffers64 == nullptr ||
			data.outputs[0].channelBuffers64 == nullptr)
		{
			return false;
		}
		for (Steinberg::int32 channel = 0; channel < channelCount; ++channel)
		{
			if (data.inputs[0].channelBuffers64[channel] == nullptr ||
				data.outputs[0].channelBuffers64[channel] == nullptr)
			{
				return false;
			}
		}
		return true;
	}

	return false;
}

void MonitorVST3Processor::copyDry(Steinberg::Vst::ProcessData& data) noexcept
{
	if (data.outputs == nullptr || data.numOutputs <= 0)
		return;

	Steinberg::Vst::AudioBusBuffers& output = data.outputs[0];
	const Steinberg::Vst::AudioBusBuffers* input =
		data.inputs != nullptr && data.numInputs > 0 ? &data.inputs[0] : nullptr;
	if (data.symbolicSampleSize == Steinberg::Vst::kSample32)
	{
		copyDryChannels(output, input, output.channelBuffers32,
			input == nullptr ? nullptr : input->channelBuffers32, data.numSamples);
	}
	else if (data.symbolicSampleSize == Steinberg::Vst::kSample64)
	{
		copyDryChannels(output, input, output.channelBuffers64,
			input == nullptr ? nullptr : input->channelBuffers64, data.numSamples);
	}
	else
	{
		output.silenceFlags = channelMask(output.numChannels);
	}
}

Steinberg::tresult PLUGIN_API MonitorVST3Processor::process(
	Steinberg::Vst::ProcessData& data)
{
	updateBypassParameter(data.inputParameterChanges);
	if (data.symbolicSampleSize != Steinberg::Vst::kSample32 &&
		data.symbolicSampleSize != Steinberg::Vst::kSample64)
	{
		setStatus(MonitorProcessStatus::UnsupportedSampleSize);
		return Steinberg::kResultFalse;
	}

	const bool offlineMode =
		data.processMode == Steinberg::Vst::ProcessModes::kOffline ||
		this->offlineMode.load(std::memory_order_acquire);
	if (offlineMode)
	{
		setStatus(MonitorProcessStatus::OfflineDry);
		copyDry(data);
		return Steinberg::kResultOk;
	}
	if (bypass.load(std::memory_order_acquire))
	{
		setStatus(MonitorProcessStatus::Bypassed);
		copyDry(data);
		return Steinberg::kResultOk;
	}
	if (!engineReady.load(std::memory_order_acquire) || engine == nullptr)
	{
		// Keep the setup-time EngineFailure/Uninitialized diagnostic instead
		// of disguising a missing configuration as an inactive valid engine.
		copyDry(data);
		return Steinberg::kResultOk;
	}
	if (!active.load(std::memory_order_acquire) ||
		!processing.load(std::memory_order_acquire))
	{
		setStatus(MonitorProcessStatus::Inactive);
		copyDry(data);
		return Steinberg::kResultOk;
	}

	const Steinberg::int32 maxBlockSize =
		configuredMaxBlockSize.load(std::memory_order_acquire);
	if (data.numSamples < 0 || data.numSamples > maxBlockSize)
	{
		setStatus(MonitorProcessStatus::OversizedBlock);
		copyDry(data);
		return Steinberg::kResultOk;
	}

	const Steinberg::int32 channelCount =
		configuredChannelCount.load(std::memory_order_acquire);
	if (!buffersAreValid(data, channelCount))
	{
		setStatus(MonitorProcessStatus::InvalidBus);
		copyDry(data);
		return Steinberg::kResultOk;
	}

	try
	{
		if (data.symbolicSampleSize == Steinberg::Vst::kSample32)
		{
			engine->process(data.outputs[0].channelBuffers32,
				data.inputs[0].channelBuffers32,
				static_cast<unsigned>(data.numSamples));
		}
		else if (data.symbolicSampleSize == Steinberg::Vst::kSample64)
		{
			engine->process(data.outputs[0].channelBuffers64,
				data.inputs[0].channelBuffers64,
				static_cast<unsigned>(data.numSamples));
		}
	}
	catch (...)
	{
		engineReady.store(false, std::memory_order_release);
		setStatus(MonitorProcessStatus::EngineFailure);
		copyDry(data);
		return Steinberg::kResultOk;
	}

	data.outputs[0].silenceFlags = 0;
	setStatus(MonitorProcessStatus::Ready);
	return Steinberg::kResultOk;
}

Steinberg::tresult PLUGIN_API MonitorVST3Processor::setState(
	Steinberg::IBStream* state)
{
	bool savedBypass = false;
	if (!readMonitorState(state, savedBypass))
		return Steinberg::kResultFalse;
	bypass.store(savedBypass, std::memory_order_release);
	return Steinberg::kResultOk;
}

Steinberg::tresult PLUGIN_API MonitorVST3Processor::getState(
	Steinberg::IBStream* state)
{
	return writeMonitorState(state, bypass.load(std::memory_order_acquire)) ?
		Steinberg::kResultOk : Steinberg::kResultFalse;
}

} // namespace EqualizerAPO::MonitorVST3
