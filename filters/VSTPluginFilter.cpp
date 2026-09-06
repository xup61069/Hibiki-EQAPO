/*
    This file is part of Equalizer APO, a system-wide equalizer.
    Copyright (C) 2017  Jonas Thedering

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
#include <limits>
#include "helpers/StringHelper.h"
#include "helpers/LogHelper.h"
#include "helpers/PrecisionTimer.h"
#include "VSTPluginFilter.h"

using namespace std;

VSTPluginFilter::VSTPluginFilter(std::shared_ptr<VSTPluginLibrary> library, std::wstring chunkData, std::unordered_map<std::wstring, float> paramMap, int vst3ClassIndex, std::wstring midiConfig)
	: library(library), chunkData(chunkData), paramMap(paramMap), vst3ClassIndex(vst3ClassIndex), midiConfig(std::move(midiConfig))
{
	libPath = library->getLibPath();
}

VSTPluginFilter::~VSTPluginFilter()
{
	cleanup();
}

std::vector<std::wstring> VSTPluginFilter::initialize(float sampleRate, unsigned maxFrameCount, std::vector<std::wstring> channelNames)
{
	cleanup();

	channelCount = channelNames.size();
	if (channelCount > (std::numeric_limits<unsigned>::max)())
	{
		skipProcessing = true;
		return channelNames;
	}
	slowProcessingLimitSeconds = max(0.25, (static_cast<double>(maxFrameCount) / max(1.0f, sampleRate)) * 8.0);
	if (channelCount == 0)
		return channelNames;

	skipProcessing = false;
	auto failAllocation = [&]()
	{
		cleanup();
		skipProcessing = true;
	};

	void* mem = MemoryHelper::alloc(sizeof(VSTPluginInstance));
	if (mem == NULL)
	{
		skipProcessing = true;
		return channelNames;
	}
	VSTPluginInstance* firstEffect = NULL;
	try
	{
		firstEffect = new(mem) VSTPluginInstance(
			library, 2, vst3ClassIndex);
	}
	catch (...)
	{
		MemoryHelper::free(mem);
		throw;
	}
	SCOPE_EXIT
	{
		if (firstEffect != NULL)
		{
			firstEffect->~VSTPluginInstance();
			MemoryHelper::free(firstEffect);
		}
	};
	if (!firstEffect->initialize())
	{
		LogF(L"The VST plugin %s crashed during initialization.", libPath.c_str());
		skipProcessing = true;
	}

	const int inputCount = firstEffect->numInputs();
	const int outputCount = firstEffect->numOutputs();
	if (inputCount < 0 || outputCount < 0)
	{
		skipProcessing = true;
		return channelNames;
	}
	pluginInputCount = inputCount;
	pluginOutputCount = outputCount;
	effectChannelCount = static_cast<unsigned>(max(inputCount, outputCount));
	if (effectChannelCount == 0)
	{
		LogF(L"The VST plugin %s does not expose audio inputs or outputs.", libPath.c_str());
		skipProcessing = true;
		effectChannelCount = static_cast<unsigned>(channelCount);
	}
	// round up
	effectCount = channelCount / effectChannelCount +
		(channelCount % effectChannelCount != 0 ? 1 : 0);
	effects = static_cast<VSTPluginInstance**>(MemoryHelper::allocArray(
		effectCount, sizeof(VSTPluginInstance*)));
	if (effects == NULL)
	{
		effectCount = 0;
		skipProcessing = true;
		return channelNames;
	}
	memset(effects, 0, effectCount * sizeof(VSTPluginInstance*));
	effects[0] = firstEffect;
	firstEffect = NULL;
	for (size_t i = 1; i < effectCount; i++)
	{
		mem = MemoryHelper::alloc(sizeof(VSTPluginInstance));
		if (mem == NULL)
		{
			failAllocation();
			return channelNames;
		}
		try
		{
			effects[i] = new(mem) VSTPluginInstance(
				library, 2, vst3ClassIndex);
		}
		catch (...)
		{
			MemoryHelper::free(mem);
			throw;
		}
		const bool initialized = effects[i]->initialize();
		if (effects[i]->numInputs() != pluginInputCount ||
			effects[i]->numOutputs() != pluginOutputCount)
		{
			failAllocation();
			return channelNames;
		}
		if (!initialized && !skipProcessing)
		{
			LogF(L"The VST plugin %s crashed during initialization.", libPath.c_str());
			skipProcessing = true;
		}
	}

	prepareForProcessing(sampleRate, maxFrameCount);
	if (!skipProcessing && !this->midiConfig.empty())
		midiRuntime.configure(
			this->midiConfig, effects[0]->getParameterDescriptors());

	// 2 times for input and output
	if (effectCount > (std::numeric_limits<size_t>::max)() / effectChannelCount)
	{
		failAllocation();
		return channelNames;
	}
	const size_t effectChannels = effectCount * effectChannelCount;
	const size_t unusedChannelCount = effectChannels - channelCount;
	if (unusedChannelCount > (std::numeric_limits<size_t>::max)() / 2)
	{
		failAllocation();
		return channelNames;
	}
	emptyChannelCount = 2 * unusedChannelCount;
	if (emptyChannelCount > 0)
	{
		emptyChannels = static_cast<double**>(MemoryHelper::allocArray(
			emptyChannelCount, sizeof(double*)));
		if (emptyChannels == NULL)
		{
			failAllocation();
			return channelNames;
		}
		memset(emptyChannels, 0, emptyChannelCount * sizeof(double*));
		for (size_t i = 0; i < emptyChannelCount; i++)
		{
			emptyChannels[i] = static_cast<double*>(MemoryHelper::allocArray(
				maxFrameCount, sizeof(double)));
			if (emptyChannels[i] == NULL)
			{
				failAllocation();
				return channelNames;
			}
			memset(emptyChannels[i], 0, maxFrameCount * sizeof(double));
		}
	}

	if (inputCount > 0)
	{
		inputArray = static_cast<double**>(MemoryHelper::allocArray(
			static_cast<size_t>(inputCount), sizeof(double*)));
		if (inputArray == NULL)
		{
			failAllocation();
			return channelNames;
		}
	}
	if (outputCount > 0)
	{
		outputArray = static_cast<double**>(MemoryHelper::allocArray(
			static_cast<size_t>(outputCount), sizeof(double*)));
		if (outputArray == NULL)
		{
			failAllocation();
			return channelNames;
		}
	}

	// Allocate float buffers for conversion
	if (inputCount > 0) {
		const size_t inputCountSize = static_cast<size_t>(inputCount);
		if (maxFrameCount != 0 &&
			inputCountSize > (std::numeric_limits<size_t>::max)() / maxFrameCount)
		{
			failAllocation();
			return channelNames;
		}
		const size_t inputSampleCount = inputCountSize * maxFrameCount;
		floatInputs = static_cast<float**>(MemoryHelper::allocArray(
			inputCountSize, sizeof(float*)));
		_floatInputBuffer = static_cast<float*>(MemoryHelper::allocArray(
			inputSampleCount, sizeof(float)));
		if (floatInputs == NULL || _floatInputBuffer == NULL)
		{
			failAllocation();
			return channelNames;
		}
		for (int i = 0; i < inputCount; ++i) {
			floatInputs[i] = _floatInputBuffer + static_cast<size_t>(i) * maxFrameCount;
		}
	}

	if (outputCount > 0) {
		const size_t outputCountSize = static_cast<size_t>(outputCount);
		if (maxFrameCount != 0 &&
			outputCountSize > (std::numeric_limits<size_t>::max)() / maxFrameCount)
		{
			failAllocation();
			return channelNames;
		}
		const size_t outputSampleCount = outputCountSize * maxFrameCount;
		floatOutputs = static_cast<float**>(MemoryHelper::allocArray(
			outputCountSize, sizeof(float*)));
		_floatOutputBuffer = static_cast<float*>(MemoryHelper::allocArray(
			outputSampleCount, sizeof(float)));
		if (floatOutputs == NULL || _floatOutputBuffer == NULL)
		{
			failAllocation();
			return channelNames;
		}
		for (int i = 0; i < outputCount; ++i) {
			floatOutputs[i] = _floatOutputBuffer + static_cast<size_t>(i) * maxFrameCount;
		}
	}

	// Allocate delay compensation buffers
	const int initialDelay = effects[0]->getInitialDelay();
	delayBufferLength = initialDelay > 0 ? static_cast<unsigned>(initialDelay) : 0;
	if (delayBufferLength > 0)
	{
		delayBuffers = static_cast<double**>(MemoryHelper::allocArray(
			channelCount, sizeof(double*)));
		if (delayBuffers == NULL)
		{
			failAllocation();
			return channelNames;
		}
		memset(delayBuffers, 0, channelCount * sizeof(double*));
		for (size_t i = 0; i < channelCount; i++)
		{
			delayBuffers[i] = static_cast<double*>(MemoryHelper::allocArray(
				delayBufferLength, sizeof(double)));
			if (delayBuffers[i] == NULL)
			{
				failAllocation();
				return channelNames;
			}
			memset(delayBuffers[i], 0, delayBufferLength * sizeof(double));
		}
		delayTempBuffer = static_cast<double*>(MemoryHelper::allocArray(
			maxFrameCount, sizeof(double)));
		if (delayTempBuffer == NULL)
		{
			failAllocation();
			return channelNames;
		}
		delayBufferOffset = 0;
	}

	return channelNames;
}

void VSTPluginFilter::prepareForProcessing(float sampleRate, unsigned maxFrameCount)
{
	__try
	{
		for (size_t i = 0; i < effectCount; i++)
		{
			VSTPluginInstance* effect = effects[i];
			if (effect == NULL)
				continue;

			if (i == effectCount - 1 && (channelCount % effectChannelCount) != 0)
				effect->setUsedChannelCount(channelCount % effectChannelCount);
			else
				effect->setUsedChannelCount(effectChannelCount);
			effect->writeToEffect(chunkData, paramMap);
			effect->prepareForProcessing(sampleRate, maxFrameCount);
			effect->startProcessing();
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		LogF(L"The VST plugin %s crashed while preparing for processing.", libPath.c_str());
		skipProcessing = true;
	}
}

#pragma AVRT_CODE_BEGIN
void convertFloatToDouble(double* dest, const float* src, size_t count);

// Converts a block of doubles back to floats.
void convertDoubleToFloat(float* dest, const double* src, size_t count);

void VSTPluginFilter::process(double** output, double** input, unsigned frameCount)
{
	if (skipProcessing)
	{
		for (size_t i = 0; i < channelCount; i++)
			memcpy(output[i], input[i], frameCount * sizeof(double));
		return;
	}

	PrecisionTimer processingTimer;
	processingTimer.start();

	__try
	{
		applyMidiUpdates();
		size_t channelOffset = 0;
		size_t emptyChannelIndex = 0;
		for (size_t i = 0; i < effectCount; i++)
		{
			VSTPluginInstance* effect = effects[i];
			// Setup double pointer arrays to point to the correct source/destination double buffers
			for (int j = 0; j < pluginInputCount; j++)
			{
				if (channelOffset + static_cast<size_t>(j) < channelCount)
					inputArray[j] = input[channelOffset + static_cast<size_t>(j)];
				else
					inputArray[j] = emptyChannels[emptyChannelIndex++];
			}

			for (int j = 0; j < pluginOutputCount; j++)
			{
				if (channelOffset + static_cast<size_t>(j) < channelCount)
					outputArray[j] = output[channelOffset + static_cast<size_t>(j)];
				else
					outputArray[j] = emptyChannels[emptyChannelIndex++];
			}

			if (effect->canDoubleReplacing()) {
				effect->processDoubleReplacing(inputArray, outputArray, frameCount);
			}
			else {
				// Convert input from double** to float** using pre-allocated buffers
				for (int j = 0; j < pluginInputCount; j++)
				{
					convertDoubleToFloat(floatInputs[j], inputArray[j], frameCount);
				}

				if (effect->canReplacing())
				{
					effect->processReplacing(floatInputs, floatOutputs, frameCount);
				}
				else
				{
					// For non-replacing, VST expects to add to the output. Clear float buffer first.
					for (int j = 0; j < pluginOutputCount; j++)
						memset(floatOutputs[j], 0, frameCount * sizeof(float));
					effect->process(floatInputs, floatOutputs, frameCount);
				}

				// Convert output from float** back to double** into the final destination
				for (int j = 0; j < pluginOutputCount; j++)
				{
					convertFloatToDouble(outputArray[j], floatOutputs[j], frameCount);
				}
			}

			if (pluginOutputCount < pluginInputCount)
			{
				for (int j = pluginOutputCount; j < pluginInputCount; j++)
				{
					if (channelOffset + static_cast<size_t>(j) < channelCount)
						memset(output[channelOffset + static_cast<size_t>(j)], 0, frameCount * sizeof(double));
				}
			}

			channelOffset += effectChannelCount;
		}

		// Apply delay compensation if needed
		if (delayBuffers != NULL && delayBufferLength > 0)
		{
			for (size_t i = 0; i < channelCount; i++)
			{
				double* outputChannel = output[i];
				double* delayBuffer = delayBuffers[i];
				memcpy(delayTempBuffer, outputChannel, frameCount * sizeof(double));

				if (delayBufferLength <= frameCount)
				{
					// Delay is smaller than frame count - output from buffer then from current processing
					memcpy(outputChannel, delayBuffer + delayBufferOffset, (delayBufferLength - delayBufferOffset) * sizeof(double));
					memcpy(outputChannel + delayBufferLength - delayBufferOffset, delayBuffer, delayBufferOffset * sizeof(double));
					memcpy(outputChannel + delayBufferLength, delayTempBuffer, (frameCount - delayBufferLength) * sizeof(double));
					memcpy(delayBuffer, delayTempBuffer + frameCount - delayBufferLength, delayBufferLength * sizeof(double));
				}
				else
				{
					if (delayBufferLength < delayBufferOffset + frameCount)
					{
						// Wrapping around the delay buffer
						memcpy(outputChannel, delayBuffer + delayBufferOffset, (delayBufferLength - delayBufferOffset) * sizeof(double));
						memcpy(outputChannel + delayBufferLength - delayBufferOffset, delayBuffer, (frameCount - (delayBufferLength - delayBufferOffset)) * sizeof(double));
						memcpy(delayBuffer + delayBufferOffset, delayTempBuffer, (delayBufferLength - delayBufferOffset) * sizeof(double));
						memcpy(delayBuffer, delayTempBuffer + delayBufferLength - delayBufferOffset, (frameCount - (delayBufferLength - delayBufferOffset)) * sizeof(double));
					}
					else
					{
						// Simple case - no wrapping
						memcpy(outputChannel, delayBuffer + delayBufferOffset, frameCount * sizeof(double));
						memcpy(delayBuffer + delayBufferOffset, delayTempBuffer, frameCount * sizeof(double));
					}
				}
			}

			// Update buffer offset
			if (delayBufferLength <= frameCount)
				delayBufferOffset = 0;
			else
				delayBufferOffset = (delayBufferOffset + frameCount) % delayBufferLength;
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		if (reportCrash)
		{
			LogF(L"The VST plugin %s crashed during audio processing.", libPath.c_str());
			reportCrash = false;
		}

		for (size_t i = 0; i < channelCount; i++)
			memcpy(output[i], input[i], frameCount * sizeof(double));
	}

	const double elapsedSeconds = processingTimer.stop();
	if (elapsedSeconds > slowProcessingLimitSeconds)
	{
		if (reportSlowProcessing)
		{
			LogF(L"The VST plugin %s blocked audio processing for %.3f seconds. Disabling this in-process plugin until the configuration is reloaded.", libPath.c_str(), elapsedSeconds);
			reportSlowProcessing = false;
		}
		skipProcessing = true;
	}
}

std::shared_ptr<VSTPluginLibrary> VSTPluginFilter::getLibrary() const
{
	return library;
}

std::wstring VSTPluginFilter::getChunkData() const
{
	return chunkData;
}

std::unordered_map<std::wstring, float> VSTPluginFilter::getParamMap() const
{
	return paramMap;
}

int VSTPluginFilter::getVST3ClassIndex() const
{
	return vst3ClassIndex;
}

std::wstring VSTPluginFilter::getMidiConfig() const
{
	return midiConfig;
}

void VSTPluginFilter::applyMidiUpdates()
{
	VSTMidiParameterUpdate update;
	for (unsigned processed = 0; processed < 256 && midiRuntime.tryPopParameterUpdate(update); ++processed)
	{
		if (update.parameter == nullptr)
			continue;
		for (size_t i = 0; i < effectCount; ++i)
			effects[i]->setParameterNormalized(*update.parameter, update.normalizedValue, true);
	}
}

void VSTPluginFilter::cleanup()
{
	midiRuntime.stop();
	if (effects != NULL)
	{
		for (size_t i = 0; i < effectCount; i++)
		{
			VSTPluginInstance* effect = effects[i];
			if (effect == NULL)
				continue;
			__try
			{
				effect->stopProcessing();
				effect->~VSTPluginInstance();
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				LogF(L"The VST plugin %s crashed while being unloaded.", libPath.c_str());
			}
			MemoryHelper::free(effect);
		}
		MemoryHelper::free(effects);
		effects = NULL;
	}
	effectCount = 0;
	pluginInputCount = 0;
	pluginOutputCount = 0;

	if (emptyChannels != NULL)
	{
		for (size_t i = 0; i < emptyChannelCount; i++)
			MemoryHelper::free(emptyChannels[i]);
		MemoryHelper::free(emptyChannels);
		emptyChannels = NULL;
	}
	emptyChannelCount = 0;

	if (inputArray != NULL)
	{
		MemoryHelper::free(inputArray);
		inputArray = NULL;
	}

	if (outputArray != NULL)
	{
		MemoryHelper::free(outputArray);
		outputArray = NULL;
	}
    
    if (floatInputs != NULL) {
		MemoryHelper::free(floatInputs);
		floatInputs = NULL;
	}
	if (_floatInputBuffer != NULL) {
		MemoryHelper::free(_floatInputBuffer);
		_floatInputBuffer = NULL;
	}
	if (floatOutputs != NULL) {
		MemoryHelper::free(floatOutputs);
		floatOutputs = NULL;
	}
	if (_floatOutputBuffer != NULL) {
		MemoryHelper::free(_floatOutputBuffer);
		_floatOutputBuffer = NULL;
	}

	if (delayBuffers != NULL)
	{
		for (size_t i = 0; i < channelCount; i++)
			MemoryHelper::free(delayBuffers[i]);
		MemoryHelper::free(delayBuffers);
		delayBuffers = NULL;
	}
	if (delayTempBuffer != NULL)
	{
		MemoryHelper::free(delayTempBuffer);
		delayTempBuffer = NULL;
	}
	delayBufferLength = 0;
	delayBufferOffset = 0;
}
