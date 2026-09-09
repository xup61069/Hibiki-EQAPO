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
#include <algorithm>

#include "FilterEngine.h"
#include "helpers/MemoryHelper.h"
#include "FilterConfiguration.h"

using namespace std;

FilterConfiguration::FilterConfiguration(FilterEngine* engine, const vector<FilterInfo*>& filterInfos, unsigned allChannelCount)
	: singlePrecision(engine->getLoadingSinglePrecision()),
	  realChannelCount(engine->getRealChannelCount()),
	  outputChannelCount(engine->getOutputChannelCount()),
	  allChannelCount(allChannelCount),
	  allSamples(NULL),
	  allSamples2(NULL),
	  currentSamples(NULL),
	  currentSamples2(NULL),
	  filterInfos(NULL),
	  filterCount(filterInfos.size()),
	  allocatedSampleChannelCount(0),
	  allocatedSample2ChannelCount(0),
	  valid(false)
{
	unsigned maxFrameCount = engine->getMaxFrameCount();

	this->filterInfos = static_cast<FilterInfo**>(MemoryHelper::allocArray(
		filterCount, sizeof(FilterInfo*)));
	if (this->filterInfos == NULL)
	{
		for (FilterInfo* filterInfo : filterInfos)
		{
			filterInfo->filter->~IFilter();
			MemoryHelper::free(filterInfo->filter);
			MemoryHelper::free(filterInfo->inChannels);
			MemoryHelper::free(filterInfo->outChannels);
			MemoryHelper::free(filterInfo);
		}
		filterCount = 0;
		return;
	}
	for (size_t i = 0; i < filterCount; i++)
		this->filterInfos[i] = filterInfos[i];
	if (singlePrecision)
	{
		// The double path compresses unchanged mappings into null arrays. Expand
		// them once before publication: native float and bridged double kernels
		// need complete channel lists on every call, including after bank swaps.
		const size_t* inputRoute = nullptr;
		const size_t* outputRoute = nullptr;
		size_t inputCount = 0, outputCount = 0;
		for (size_t index = 0; index < filterCount; ++index)
		{
			FilterInfo* info = this->filterInfos[index];
			if (info->inChannels)
			{
				inputRoute = info->inChannels;
				inputCount = info->inChannelCount;
			}
			else if (inputCount)
			{
				info->inChannels = static_cast<size_t*>(MemoryHelper::allocArray(inputCount, sizeof(size_t)));
				if (!info->inChannels) return;
				info->inChannelCount = inputCount;
				memcpy(info->inChannels, inputRoute, inputCount * sizeof(size_t));
			}
			if (info->outChannels)
			{
				outputRoute = info->outChannels;
				outputCount = info->outChannelCount;
			}
			else if (outputCount)
			{
				info->outChannels = static_cast<size_t*>(MemoryHelper::allocArray(outputCount, sizeof(size_t)));
				if (!info->outChannels) return;
				info->outChannelCount = outputCount;
				memcpy(info->outChannels, outputRoute, outputCount * sizeof(size_t));
			}
			if (!info->inPlace)
			{
				swap(inputRoute, outputRoute);
				swap(inputCount, outputCount);
			}
		}
	}

	allSamples = static_cast<double**>(MemoryHelper::allocArray(
		allChannelCount, sizeof(double*)));
	if (allSamples == NULL)
		return;
	memset(allSamples, 0, allChannelCount * sizeof(double*));
	for (size_t i = 0; i < allChannelCount; i++)
	{
		allSamples[i] = static_cast<double*>(MemoryHelper::allocArray(
			maxFrameCount, sizeof(double)));
		if (allSamples[i] == NULL)
			return;
		++allocatedSampleChannelCount;
	}

	allSamples2 = static_cast<double**>(MemoryHelper::allocArray(
		allChannelCount, sizeof(double*)));
	if (allSamples2 == NULL)
		return;
	memset(allSamples2, 0, allChannelCount * sizeof(double*));
	for (size_t i = 0; i < allChannelCount; i++)
	{
		allSamples2[i] = static_cast<double*>(MemoryHelper::allocArray(
			maxFrameCount, sizeof(double)));
		if (allSamples2[i] == NULL)
			return;
		++allocatedSample2ChannelCount;
	}

	currentSamples = static_cast<double**>(MemoryHelper::allocArray(
		allChannelCount, sizeof(double*)));
	if (currentSamples == NULL)
		return;
	currentSamples2 = static_cast<double**>(MemoryHelper::allocArray(
		allChannelCount, sizeof(double*)));
	if (currentSamples2 == NULL)
		return;

	if (singlePrecision)
	{
		samples32 = static_cast<float**>(MemoryHelper::allocArray(allChannelCount, sizeof(float*)));
		samples32Alternate = static_cast<float**>(MemoryHelper::allocArray(allChannelCount, sizeof(float*)));
		current32 = static_cast<float**>(MemoryHelper::allocArray(allChannelCount, sizeof(float*)));
		output32 = static_cast<float**>(MemoryHelper::allocArray(allChannelCount, sizeof(float*)));
		if (!samples32 || !samples32Alternate || !current32 || !output32) return;
		for (unsigned channel = 0; channel < allChannelCount; ++channel)
		{
			samples32[channel] = static_cast<float*>(MemoryHelper::allocArray(maxFrameCount, sizeof(float)));
			if (!samples32[channel]) return;
			++allocated32;
			samples32Alternate[channel] = static_cast<float*>(MemoryHelper::allocArray(maxFrameCount, sizeof(float)));
			if (!samples32Alternate[channel]) return;
			++allocated32Alternate;
		}
	}
	valid = true;
}

FilterConfiguration::~FilterConfiguration()
{
	for (size_t channel = 0; channel < allocated32; ++channel)
		MemoryHelper::free(samples32[channel]);
	for (size_t channel = 0; channel < allocated32Alternate; ++channel)
		MemoryHelper::free(samples32Alternate[channel]);
	MemoryHelper::free(samples32);
	MemoryHelper::free(samples32Alternate);
	MemoryHelper::free(current32);
	MemoryHelper::free(output32);
	if (currentSamples2 != NULL)
		MemoryHelper::free(currentSamples2);
	if (currentSamples != NULL)
		MemoryHelper::free(currentSamples);

	if (allSamples2 != NULL)
	{
		for (size_t i = 0; i < allocatedSample2ChannelCount; i++)
			MemoryHelper::free(allSamples2[i]);
		MemoryHelper::free(allSamples2);
	}

	if (allSamples != NULL)
	{
		for (size_t i = 0; i < allocatedSampleChannelCount; i++)
			MemoryHelper::free(allSamples[i]);
		MemoryHelper::free(allSamples);
	}

	for (size_t i = 0; filterInfos != NULL && i < filterCount; i++)
	{
		filterInfos[i]->filter->~IFilter();
		MemoryHelper::free(filterInfos[i]->filter);
		if (filterInfos[i]->inChannels != NULL)
			MemoryHelper::free(filterInfos[i]->inChannels);
		if (filterInfos[i]->outChannels != NULL)
			MemoryHelper::free(filterInfos[i]->outChannels);
		MemoryHelper::free(filterInfos[i]);
	}
	if (filterInfos != NULL)
		MemoryHelper::free(filterInfos);
}

#pragma AVRT_CODE_BEGIN
void FilterConfiguration::read(double* input, unsigned frameCount)
{
#define DEINTERLEAVE_MACRO(ccount)\
	{\
		for (size_t c = 0; c < ccount; c++)\
		{\
			double* sampleChannel = allSamples[c];\
			double* i2 = input + c;\
			for (size_t i = 0; i < frameCount; i++)\
			{\
				sampleChannel[i] = i2[i * ccount];\
			}\
		}\
	}

	switch (realChannelCount)
	{
	case 1:
		DEINTERLEAVE_MACRO(1)
		break;
	case 2:
		DEINTERLEAVE_MACRO(2)
		break;
	case 6:
		DEINTERLEAVE_MACRO(6)
		break;
	case 8:
		DEINTERLEAVE_MACRO(8)
		break;
	default:
		DEINTERLEAVE_MACRO(realChannelCount)
	}
}

void FilterConfiguration::read(double** input, unsigned frameCount)
{
	for (unsigned c = 0; c < realChannelCount; c++)
		memcpy(allSamples[c], input[c], frameCount * sizeof(double));
}

void FilterConfiguration::process(unsigned frameCount)
{
	if (singlePrecision)
	{
		processSingle(frameCount);
		return;
	}
	for (unsigned c = realChannelCount; c < allChannelCount; c++)
		memset(allSamples[c], 0, frameCount * sizeof(double));

	// for real mono input and >= stereo output, upmix to stereo as the Windows audio system would do automatically if no APO was present
	if (realChannelCount == 1 && outputChannelCount >= 2)
		memcpy(allSamples[1], allSamples[0], frameCount * sizeof(double));

	for (size_t i = 0; i < filterCount; i++)
	{
		FilterInfo* filterInfo = filterInfos[i];
		for (size_t j = 0; j < filterInfo->inChannelCount; j++)
			currentSamples[j] = allSamples[filterInfo->inChannels[j]];
		if (filterInfo->inPlace)
		{
			for (size_t j = 0; j < filterInfo->outChannelCount; j++)
				currentSamples2[j] = allSamples[filterInfo->outChannels[j]];
		}
		else
		{
			for (size_t j = 0; j < filterInfo->outChannelCount; j++)
				currentSamples2[j] = allSamples2[filterInfo->outChannels[j]];
		}

		filterInfo->filter->process(currentSamples2, currentSamples, frameCount);

		if (!filterInfo->inPlace)
		{
			for (size_t j = 0; j < filterInfo->outChannelCount; j++)
				swap(allSamples[filterInfo->outChannels[j]], allSamples2[filterInfo->outChannels[j]]);
			swap(currentSamples, currentSamples2);
		}
	}
}

void FilterConfiguration::processSingle(unsigned frameCount)
{
	for (unsigned channel = 0; channel < allChannelCount; ++channel)
		for (unsigned frame = 0; frame < frameCount; ++frame)
			samples32[channel][frame] = channel < realChannelCount
				? static_cast<float>(allSamples[channel][frame]) : 0.0f;
	if (realChannelCount == 1 && outputChannelCount >= 2)
		memcpy(samples32[1], samples32[0], frameCount * sizeof(float));
	for (size_t index = 0; index < filterCount; ++index)
	{
		FilterInfo* info = filterInfos[index];
		for (size_t channel = 0; channel < info->inChannelCount; ++channel)
			current32[channel] = samples32[info->inChannels[channel]];
		for (size_t channel = 0; channel < info->outChannelCount; ++channel)
			output32[channel] = info->inPlace ? samples32[info->outChannels[channel]]
				: samples32Alternate[info->outChannels[channel]];
		if (!info->filter->processSingle(output32, current32, frameCount))
		{
			// Legacy/specialized kernels retain their internal precision. Every
			// module boundary still stores float; scratch is allocated at load.
			for (size_t channel = 0; channel < info->inChannelCount; ++channel)
			{
				currentSamples[channel] = allSamples[info->inChannels[channel]];
				for (unsigned frame = 0; frame < frameCount; ++frame)
					currentSamples[channel][frame] = current32[channel][frame];
			}
			for (size_t channel = 0; channel < info->outChannelCount; ++channel)
				currentSamples2[channel] = info->inPlace ? allSamples[info->outChannels[channel]]
					: allSamples2[info->outChannels[channel]];
			info->filter->process(currentSamples2, currentSamples, frameCount);
			for (size_t channel = 0; channel < info->outChannelCount; ++channel)
				for (unsigned frame = 0; frame < frameCount; ++frame)
					output32[channel][frame] = static_cast<float>(currentSamples2[channel][frame]);
		}
		if (!info->inPlace)
			for (size_t channel = 0; channel < info->outChannelCount; ++channel)
				swap(samples32[info->outChannels[channel]], samples32Alternate[info->outChannels[channel]]);
	}
	// Share the existing double transition/output boundary so old and new
	// configurations can safely crossfade even when their bus formats differ.
	for (unsigned channel = 0; channel < outputChannelCount; ++channel)
		for (unsigned frame = 0; frame < frameCount; ++frame)
			allSamples[channel][frame] = samples32[channel][frame];
}

unsigned FilterConfiguration::doTransition(FilterConfiguration* nextConfig, unsigned frameCount, unsigned transitionCounter, unsigned transitionLength)
{
	double** currentSamples = allSamples;
	double** nextSamples = nextConfig->allSamples;

	for (unsigned f = 0; f < frameCount; f++)
	{
		double factor = 1.0;
		if (transitionLength != 0 && transitionCounter < transitionLength)
		{
			factor = 0.5 * (1.0 - cos(
				transitionCounter * static_cast<double>(M_PI) /
				transitionLength));
		}

		for (unsigned c = 0; c < outputChannelCount; c++)
			currentSamples[c][f] = currentSamples[c][f] * (1 - factor) + nextSamples[c][f] * factor;

		if (transitionCounter < transitionLength)
			transitionCounter++;
	}

	return transitionCounter;
}

void FilterConfiguration::write(double* output, unsigned frameCount)
{
#define INTERLEAVE_MACRO(ccount)\
	for (size_t c = 0; c < ccount; c++)\
	{\
		double* sampleChannel = allSamples[c];\
		double* o2 = output + c;\
		for (unsigned i = 0; i < frameCount; i++)\
		{\
			o2[i * ccount] = sampleChannel[i];\
		}\
	}

	switch (outputChannelCount)
	{
	case 1:
		INTERLEAVE_MACRO(1)
		break;
	case 2:
		INTERLEAVE_MACRO(2)
		break;
	case 6:
		INTERLEAVE_MACRO(6)
		break;
	case 8:
		INTERLEAVE_MACRO(8)
		break;
	default:
		INTERLEAVE_MACRO(outputChannelCount)
	}
}

void FilterConfiguration::write(double** output, unsigned frameCount)
{
	for (unsigned i = 0; i < outputChannelCount; i++)
		memcpy(output[i], allSamples[i], frameCount * sizeof(double));
}
#pragma AVRT_CODE_END

bool FilterConfiguration::isEmpty()
{
	return filterCount == 0 && !singlePrecision;
}
