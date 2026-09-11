/*
    This file is part of EqualizerAPO, a system-wide equalizer.
    Copyright (C) 2014  Jonas Thedering

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with this program; if not, write to the Free Software Foundation, Inc.,
    51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/

#include "stdafx.h"
#include <cmath>
#include <limits>

#include "helpers/MemoryHelper.h"
#include "DelayFilter.h"

using namespace std;

DelayFilter::DelayFilter(double delay, bool isMs)
	: delay(delay),
	  isMs(isMs),
	  bufferLength(0),
	  channelCount(0),
	  buffers(NULL),
	  buffers32(NULL),
	  bufferOffset(0),
	  bufferOffset32(0)
{
}

DelayFilter::~DelayFilter()
{
	cleanup();
}

vector<wstring> DelayFilter::initialize(float sampleRate, unsigned maxFrameCount, vector<wstring> channelNames)
{
	cleanup();

	if (channelNames.size() > (std::numeric_limits<unsigned>::max)())
	{
		channelCount = 0;
		return channelNames;
	}
	channelCount = static_cast<unsigned>(channelNames.size());

	const double roundedLength = isMs ?
		static_cast<double>(sampleRate) * delay / 1000.0 + 0.5 : delay + 0.5;
	if (!std::isfinite(roundedLength) || roundedLength < 0.0 ||
		roundedLength > (std::numeric_limits<unsigned>::max)())
		return channelNames;
	bufferLength = static_cast<unsigned>(roundedLength);
	if (bufferLength == 0)
		return channelNames;

	buffers = static_cast<double**>(MemoryHelper::allocArray(
		channelCount, sizeof(double*)));
	buffers32 = static_cast<float**>(MemoryHelper::allocArray(
		channelCount, sizeof(float*)));
	if (buffers == NULL || buffers32 == NULL)
	{
		cleanup();
		bufferLength = 0;
		return channelNames;
	}
	memset(buffers, 0, channelCount * sizeof(double*));
	memset(buffers32, 0, channelCount * sizeof(float*));

	for (unsigned i = 0; i < channelCount; i++)
	{
		buffers[i] = static_cast<double*>(MemoryHelper::allocArray(
			bufferLength, sizeof(double)));
		buffers32[i] = static_cast<float*>(MemoryHelper::allocArray(
			bufferLength, sizeof(float)));
		if (buffers[i] == NULL || buffers32[i] == NULL)
		{
			cleanup();
			bufferLength = 0;
			return channelNames;
		}
		memset(buffers[i], 0, sizeof(double) * bufferLength);
		memset(buffers32[i], 0, sizeof(float) * bufferLength);
	}

	bufferOffset = 0;
	bufferOffset32 = 0;

	return channelNames;
}

#pragma AVRT_CODE_BEGIN
void DelayFilter::process(double** output, double** input, unsigned frameCount)
{
	if (bufferLength == 0)
	{
		for (unsigned i = 0; i < channelCount; i++)
			if (output[i] != input[i])
				memcpy(output[i], input[i], frameCount * sizeof(double));
		return;
	}

	for (unsigned i = 0; i < channelCount; i++)
	{
		double* inputChannel = input[i];
		double* outputChannel = output[i];
		double* bufferChannel = buffers[i];

		if (bufferLength <= frameCount)
		{
			memcpy(outputChannel, bufferChannel + bufferOffset, (bufferLength - bufferOffset) * sizeof(double));
			memcpy(outputChannel + bufferLength - bufferOffset, bufferChannel, bufferOffset * sizeof(double));
			memcpy(outputChannel + bufferLength, inputChannel, (frameCount - bufferLength) * sizeof(double));
			memcpy(bufferChannel, inputChannel + frameCount - bufferLength, bufferLength * sizeof(double));
		}
		else
		{
			if (bufferLength < bufferOffset + frameCount)
			{
				memcpy(outputChannel, bufferChannel + bufferOffset, (bufferLength - bufferOffset) * sizeof(double));
				memcpy(outputChannel + bufferLength - bufferOffset, bufferChannel, (frameCount - (bufferLength - bufferOffset)) * sizeof(double));
				memcpy(bufferChannel + bufferOffset, inputChannel, (bufferLength - bufferOffset) * sizeof(double));
				memcpy(bufferChannel, inputChannel + bufferLength - bufferOffset, (frameCount - (bufferLength - bufferOffset)) * sizeof(double));
			}
			else
			{
				memcpy(outputChannel, bufferChannel + bufferOffset, frameCount * sizeof(double));
				memcpy(bufferChannel + bufferOffset, inputChannel, frameCount * sizeof(double));
			}
		}
	}

	if (bufferLength <= frameCount)
		bufferOffset = 0;
	else
		bufferOffset = (bufferOffset + frameCount) % bufferLength;
}

bool DelayFilter::processSingle(float** output, float** input, unsigned frameCount)
{
	if (bufferLength == 0)
	{
		for (unsigned i = 0; i < channelCount; i++)
			if (output[i] != input[i])
				memcpy(output[i], input[i], frameCount * sizeof(float));
		return true;
	}

	for (unsigned i = 0; i < channelCount; i++)
	{
		float* inputChannel = input[i];
		float* outputChannel = output[i];
		float* bufferChannel = buffers32[i];

		if (bufferLength <= frameCount)
		{
			memcpy(outputChannel, bufferChannel + bufferOffset32, (bufferLength - bufferOffset32) * sizeof(float));
			memcpy(outputChannel + bufferLength - bufferOffset32, bufferChannel, bufferOffset32 * sizeof(float));
			memcpy(outputChannel + bufferLength, inputChannel, (frameCount - bufferLength) * sizeof(float));
			memcpy(bufferChannel, inputChannel + frameCount - bufferLength, bufferLength * sizeof(float));
		}
		else
		{
			if (bufferLength < bufferOffset32 + frameCount)
			{
				memcpy(outputChannel, bufferChannel + bufferOffset32, (bufferLength - bufferOffset32) * sizeof(float));
				memcpy(outputChannel + bufferLength - bufferOffset32, bufferChannel, (frameCount - (bufferLength - bufferOffset32)) * sizeof(float));
				memcpy(bufferChannel + bufferOffset32, inputChannel, (bufferLength - bufferOffset32) * sizeof(float));
				memcpy(bufferChannel, inputChannel + bufferLength - bufferOffset32, (frameCount - (bufferLength - bufferOffset32)) * sizeof(float));
			}
			else
			{
				memcpy(outputChannel, bufferChannel + bufferOffset32, frameCount * sizeof(float));
				memcpy(bufferChannel + bufferOffset32, inputChannel, frameCount * sizeof(float));
			}
		}
	}

	if (bufferLength <= frameCount)
		bufferOffset32 = 0;
	else
		bufferOffset32 = (bufferOffset32 + frameCount) % bufferLength;

	return true;
}
#pragma AVRT_CODE_END

void DelayFilter::cleanup()
{
	if (buffers != NULL)
	{
		for (unsigned i = 0; i < channelCount; i++)
			MemoryHelper::free(buffers[i]);

		MemoryHelper::free(buffers);
		buffers = NULL;
	}
	if (buffers32 != NULL)
	{
		for (unsigned i = 0; i < channelCount; i++)
			MemoryHelper::free(buffers32[i]);

		MemoryHelper::free(buffers32);
		buffers32 = NULL;
	}
	bufferLength = 0;
	bufferOffset = 0;
	bufferOffset32 = 0;
}

bool DelayFilter::getIsMs() const
{
	return isMs;
}

double DelayFilter::getDelay() const
{
	return delay;
}
