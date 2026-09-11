#include "stdafx.h"
#include <algorithm>
#include <cmath>
#include "ReverbFilter.h"
#include "AudioToolsHelper.h"

using namespace std;

double ReverbFilter::DelayLine::process(double input)
{
	if (buffer.empty())
		return input;
	const double out = buffer[index];
	buffer[index] = input;
	index++;
	if (index >= buffer.size())
		index = 0;
	return out;
}

ReverbFilter::ReverbFilter(double roomSizePercent, double dampingPercent, double wetPercent, double dryPercent, double widthPercent)
	: roomSize(AudioTools::percentToUnit(roomSizePercent)),
	  damping(AudioTools::percentToUnit(dampingPercent)),
	  wet(AudioTools::percentToUnit(wetPercent)),
	  dry(AudioTools::percentToUnit(dryPercent, 1.5)),
	  width(AudioTools::percentToUnit(widthPercent))
{
}

vector<wstring> ReverbFilter::initialize(float sampleRate, unsigned maxFrameCount, vector<wstring> channelNames)
{
	channelCount = static_cast<unsigned>(channelNames.size());
	const double scale = (sampleRate > 0.0f ? sampleRate : 48000.0f) / 48000.0;
	const int combBase[] = {1116, 1188, 1277, 1356};
	const int allpassBase[] = {556, 441};
	combs.assign(channelCount, vector<DelayLine>(4));
	allpasses.assign(channelCount, vector<DelayLine>(2));
	dampState.assign(channelCount, vector<double>(4, 0.0));
	for (unsigned c = 0; c < channelCount; c++)
	{
		for (unsigned i = 0; i < 4; i++)
			combs[c][i].buffer.assign(max(1, static_cast<int>((combBase[i] + c * 23) * scale)), 0.0);
		for (unsigned i = 0; i < 2; i++)
			allpasses[c][i].buffer.assign(max(1, static_cast<int>((allpassBase[i] + c * 17) * scale)), 0.0);
	}
	return channelNames;
}

#pragma AVRT_CODE_BEGIN
void ReverbFilter::process(double** output, double** input, unsigned frameCount)
{
	if (wet <= 0.0)
	{
		if (dry == 1.0)
		{
			for (unsigned c = 0; c < channelCount; c++)
				if (output[c] != input[c])
					memcpy(output[c], input[c], frameCount * sizeof(double));
		}
		else
		{
			for (unsigned c = 0; c < channelCount; c++)
				for (unsigned frame = 0; frame < frameCount; frame++)
					output[c][frame] = input[c][frame] * dry;
		}
		return;
	}

	const double feedback = 0.72 + roomSize * 0.23;
	for (unsigned frame = 0; frame < frameCount; frame++)
	{
		double dryLeft = 0.0;
		double dryRight = 0.0;
		double wetLeft = 0.0;
		double wetRight = 0.0;
		for (unsigned c = 0; c < channelCount; c++)
		{
			double acc = 0.0;
			const double in = input[c][frame];
			for (unsigned i = 0; i < 4; i++)
			{
				double delayed = combs[c][i].process(in + dampState[c][i] * feedback);
				dampState[c][i] = delayed * (1.0 - damping) + dampState[c][i] * damping;
				acc += delayed;
			}
			acc *= 0.25;
			for (unsigned i = 0; i < 2; i++)
			{
				double delayed = allpasses[c][i].process(acc);
				acc = delayed - acc * 0.5;
			}
			const double drySignal = in * dry;
			const double wetSignal = acc * wet;
			output[c][frame] = drySignal + wetSignal;
			if (c == 0)
			{
				dryLeft = drySignal;
				wetLeft = wetSignal;
			}
			else if (c == 1)
			{
				dryRight = drySignal;
				wetRight = wetSignal;
			}
		}

		if (channelCount >= 2 && width < 1.0)
		{
			const double wetMid = 0.5 * (wetLeft + wetRight);
			output[0][frame] = dryLeft + wetMid + (wetLeft - wetMid) * width;
			output[1][frame] = dryRight + wetMid + (wetRight - wetMid) * width;
		}
	}
}

bool ReverbFilter::processSingle(float** output, float** input, unsigned frameCount)
{
	if (wet <= 0.0)
	{
		if (dry == 1.0)
		{
			for (unsigned c = 0; c < channelCount; c++)
				if (output[c] != input[c])
					memcpy(output[c], input[c], frameCount * sizeof(float));
		}
		else
		{
			const float dryF = static_cast<float>(dry);
			for (unsigned c = 0; c < channelCount; c++)
				for (unsigned frame = 0; frame < frameCount; frame++)
					output[c][frame] = input[c][frame] * dryF;
		}
		return true;
	}

	const double feedback = 0.72 + roomSize * 0.23;
	for (unsigned frame = 0; frame < frameCount; frame++)
	{
		double dryLeft = 0.0;
		double dryRight = 0.0;
		double wetLeft = 0.0;
		double wetRight = 0.0;
		for (unsigned c = 0; c < channelCount; c++)
		{
			double acc = 0.0;
			const double in = static_cast<double>(input[c][frame]);
			for (unsigned i = 0; i < 4; i++)
			{
				double delayed = combs[c][i].process(in + dampState[c][i] * feedback);
				dampState[c][i] = delayed * (1.0 - damping) + dampState[c][i] * damping;
				acc += delayed;
			}
			acc *= 0.25;
			for (unsigned i = 0; i < 2; i++)
			{
				double delayed = allpasses[c][i].process(acc);
				acc = delayed - acc * 0.5;
			}
			const double drySignal = in * dry;
			const double wetSignal = acc * wet;
			output[c][frame] = static_cast<float>(drySignal + wetSignal);
			if (c == 0)
			{
				dryLeft = drySignal;
				wetLeft = wetSignal;
			}
			else if (c == 1)
			{
				dryRight = drySignal;
				wetRight = wetSignal;
			}
		}

		if (channelCount >= 2 && width < 1.0)
		{
			const double wetMid = 0.5 * (wetLeft + wetRight);
			output[0][frame] = static_cast<float>(dryLeft + wetMid + (wetLeft - wetMid) * width);
			output[1][frame] = static_cast<float>(dryRight + wetMid + (wetRight - wetMid) * width);
		}
	}
	return true;
}
#pragma AVRT_CODE_END
