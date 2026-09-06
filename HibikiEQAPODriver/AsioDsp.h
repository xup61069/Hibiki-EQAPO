#pragma once

#include <memory>

namespace HibikiAsio
{
class IAsioDsp
{
public:
	virtual ~IAsioDsp() = default;
	virtual bool process(
		double** output,
		double** input,
		unsigned channelCount,
		unsigned frameCount) = 0;
};

using AsioDspFactory = std::unique_ptr<IAsioDsp> (*)(
	double sampleRate,
	unsigned channelCount,
	unsigned maximumFrameCount);

std::unique_ptr<IAsioDsp> createDefaultAsioDsp(
	double sampleRate,
	unsigned channelCount,
	unsigned maximumFrameCount);
}
