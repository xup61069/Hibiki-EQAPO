#include "stdafx.h"
#include <algorithm>
#include "HeadphoneCalibrationFilter.h"

using namespace std;

vector<wstring> HeadphoneCalibrationFilter::initialize(float sampleRate, unsigned maxFrameCount, vector<wstring> channelNames)
{
	channelCount = static_cast<unsigned>(channelNames.size());
	return channelNames;
}

#pragma AVRT_CODE_BEGIN
void HeadphoneCalibrationFilter::process(double** output, double** input, unsigned frameCount)
{
	if (output == input)
		return;

	for (unsigned c = 0; c < channelCount; c++)
		memcpy(output[c], input[c], frameCount * sizeof(double));
}

bool HeadphoneCalibrationFilter::processSingle(float** output, float** input, unsigned frameCount)
{
	if (output == input)
		return true;

	for (unsigned c = 0; c < channelCount; c++)
		memcpy(output[c], input[c], frameCount * sizeof(float));
	return true;
}
#pragma AVRT_CODE_END
