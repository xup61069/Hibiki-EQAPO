#include "AsioDsp.h"

#include <cmath>
#include <memory>
#include <utility>

#include "../FilterEngine.h"
#include "../helpers/ChannelHelper.h"

namespace HibikiAsio
{
namespace
{
class FilterEngineAsioDsp final : public IAsioDsp
{
public:
	FilterEngineAsioDsp(
		double sampleRate,
		unsigned channelCount,
		unsigned maximumFrameCount)
	{
		if (!std::isfinite(sampleRate) || sampleRate <= 0.0 ||
			channelCount == 0 || channelCount > 2 || maximumFrameCount == 0)
		{
			return;
		}
		engine_ = std::make_unique<FilterEngine>();
		engine_->setProcessingPolicy(
			FilterEngine::ProcessingPolicy::AsioCallbackSafe);
		engine_->setDeviceInfo(
			false,
			true,
			L"Hibiki EQAPO",
			L"ASIO",
			L"",
			L"Hibiki EQAPO");
		engine_->initialize(
			static_cast<float>(sampleRate),
			channelCount,
			channelCount,
			channelCount,
			static_cast<unsigned>(ChannelHelper::getDefaultChannelMask(
				static_cast<int>(channelCount))),
			maximumFrameCount);
		if (engine_->rejectedUnsafeConfiguration() ||
			!engine_->hasActiveConfiguration())
			engine_.reset();
	}

	bool process(
		double** output,
		double** input,
		unsigned channelCount,
		unsigned frameCount) override
	{
		if (engine_ == nullptr || output == nullptr || input == nullptr ||
			channelCount != engine_->getOutputChannelCount() ||
			frameCount > engine_->getMaxFrameCount())
		{
			return false;
		}
		engine_->process(output, input, frameCount);
		return true;
	}

	bool isReady() const noexcept
	{
		return engine_ != nullptr;
	}

private:
	std::unique_ptr<FilterEngine> engine_;
};
}

std::unique_ptr<IAsioDsp> createDefaultAsioDsp(
	double sampleRate,
	unsigned channelCount,
	unsigned maximumFrameCount)
{
	try
	{
		auto dsp = std::make_unique<FilterEngineAsioDsp>(
			sampleRate, channelCount, maximumFrameCount);
		if (!dsp->isReady())
			return nullptr;
		return dsp;
	}
	catch (...)
	{
		return nullptr;
	}
}
}
