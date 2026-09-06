/*
	This file is part of EqualizerAPO, a system-wide equalizer.
	Copyright (C) 2015  Jonas Thedering

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
#include <cmath>
#include <limits>
#include <new>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#define ENABLE_SNDFILE_WINDOWS_PROTOTYPES 1
#include <sndfile.h>
#include <fftw3.h>

#include "helpers/LogHelper.h"
#include "helpers/MemoryHelper.h"
#include "ConvolutionFilter.h"

#ifdef max
#undef max
#endif
#ifdef min
#undef min
#endif

using namespace std;

namespace
{
	constexpr sf_count_t MaxImpulseFrames = 1024 * 1024;
	constexpr size_t MaxImpulseSamples = 8 * 1024 * 1024;

	bool isSafeImpulseShape(
		sf_count_t frames,
		int channels,
		size_t& sampleCount)
	{
		sampleCount = 0;
		if (frames <= 0 || channels <= 0 ||
			frames > std::numeric_limits<int>::max() ||
			frames > MaxImpulseFrames ||
			static_cast<unsigned long long>(frames) >
				std::numeric_limits<size_t>::max() /
				static_cast<size_t>(channels))
		{
			return false;
		}

		sampleCount = static_cast<size_t>(frames) *
			static_cast<size_t>(channels);
		return sampleCount <= MaxImpulseSamples;
	}
}

struct ConvolutionFilter::ConvolutionBank
{
	HConvSingle* filters;
	unsigned frameCount;
	unsigned channelCount;

	ConvolutionBank()
		: filters(NULL), frameCount(0), channelCount(0)
	{
	}
};

ConvolutionFilter::ConvolutionFilter(wstring filename)
	: sampleRate(0.0f),
	  filename(filename),
	  channelCount(0),
	  maxFrameCount(0),
	  fadeScratch(NULL),
	  activeBank(NULL),
	  pendingBank(NULL),
	  retiredBank(NULL),
	  requestedFrameCount(0),
	  requestGeneration(0),
	  workerStopEvent(NULL),
	  workerThread(NULL),
	  fadeLength(0),
	  fadePosition(0),
	  lastRequestedFrame(0),
	  activeUsable(false)
{
	static_assert(std::atomic<ConvolutionBank*>::is_always_lock_free,
		"Convolution bank handoff must be lock-free");
	static_assert(std::atomic<unsigned>::is_always_lock_free,
		"Convolution frame requests must be lock-free");
	static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
		"Convolution request generations must be lock-free");
}

ConvolutionFilter::~ConvolutionFilter()
{
	cleanup();
}

vector<wstring> ConvolutionFilter::initialize(float sampleRate, unsigned maxFrameCount, vector<wstring> channelNames)
{
	cleanup();

	this->sampleRate = sampleRate;
	this->maxFrameCount = maxFrameCount;
	channelCount = 0;
	if (channelNames.size() > (std::numeric_limits<unsigned>::max)())
		return channelNames;
	channelCount = static_cast<unsigned>(channelNames.size());
	if (!std::isfinite(sampleRate) || sampleRate <= 0.0f ||
		sampleRate > static_cast<float>(std::numeric_limits<int>::max()) ||
		maxFrameCount == 0 ||
		maxFrameCount > static_cast<unsigned>(
			std::numeric_limits<int>::max() / 2) ||
		maxFrameCount >
			(std::numeric_limits<size_t>::max() - 16) / sizeof(double) ||
		channelCount == 0 ||
		channelCount > static_cast<unsigned>(std::numeric_limits<int>::max()))
	{
		return channelNames;
	}

	std::vector<std::vector<double>> preparedImpulseResponses;
	try
	{
		if (!prepareImpulseResponse(preparedImpulseResponses) ||
			preparedImpulseResponses.empty())
		{
			return channelNames;
		}
		for (const std::vector<double>& impulse : preparedImpulseResponses)
		{
			if (impulse.empty() || std::any_of(
					impulse.begin(), impulse.end(),
					[](double sample) { return !std::isfinite(sample); }))
			{
				return channelNames;
			}
		}
		impulseResponses.swap(preparedImpulseResponses);
	}
	catch (const std::bad_alloc&)
	{
		LogF(L"Not enough memory to prepare convolution");
		impulseResponses.clear();
		return channelNames;
	}

	bool asyncRebuildEnabled = false;
	const size_t scratchChannelBytes =
		static_cast<size_t>(maxFrameCount) * sizeof(double);
	if (channelCount <=
		(std::numeric_limits<size_t>::max() - 16) / scratchChannelBytes)
	{
		const size_t scratchBytes =
			static_cast<size_t>(channelCount) * scratchChannelBytes;
		fadeScratch = static_cast<double*>(MemoryHelper::alloc(scratchBytes));
		if (fadeScratch != NULL)
		{
			memset(fadeScratch, 0, scratchBytes);
			asyncRebuildEnabled = true;
		}
	}

	const double fadeFrames = static_cast<double>(sampleRate) * 0.010;
	fadeLength = (std::max)(1u, static_cast<unsigned>(fadeFrames + 0.5));
	fadePosition = fadeLength;
	// Build the maximum-frame bank synchronously while this configuration is
	// still off the audio thread. Only unexpected valid-frame sizes use the
	// asynchronous handoff below.
	activeBank = createBank(maxFrameCount);
	activeUsable = activeBank != NULL;
	if (asyncRebuildEnabled && !startWorker())
		LogF(L"Could not start convolution rebuild worker");

	return channelNames;
}

#pragma AVRT_CODE_BEGIN
void ConvolutionFilter::copyDry(
	double** output,
	double** input,
	unsigned frameCount) const
{
	for (unsigned i = 0; i < channelCount; ++i)
		if (output[i] != input[i])
			memcpy(output[i], input[i], frameCount * sizeof(double));
}

void ConvolutionFilter::process(double** output, double** input, unsigned frameCount)
{
	if (frameCount == 0)
		return;

	if (activeUsable && activeBank != NULL &&
		frameCount == activeBank->frameCount)
	{
		ConvolutionBank* duplicate =
			pendingBank.load(std::memory_order_acquire);
		if (duplicate != NULL &&
			retiredBank.load(std::memory_order_acquire) == NULL)
		{
			ConvolutionBank* expected = duplicate;
			if (pendingBank.compare_exchange_strong(
					expected, NULL,
					std::memory_order_acq_rel,
					std::memory_order_acquire))
			{
				retiredBank.store(duplicate, std::memory_order_release);
			}
		}
	}

	if (!activeUsable || activeBank == NULL ||
		frameCount != activeBank->frameCount)
	{
		activeUsable = false;
		if (frameCount <= maxFrameCount)
		{
			requestedFrameCount.store(frameCount, std::memory_order_release);
			if (lastRequestedFrame != frameCount)
			{
				lastRequestedFrame = frameCount;
				requestGeneration.fetch_add(1, std::memory_order_release);
			}
			ConvolutionBank* pending =
				pendingBank.load(std::memory_order_acquire);
			if (pending != NULL &&
				retiredBank.load(std::memory_order_acquire) == NULL)
			{
				if (pending->frameCount == frameCount)
				{
					lastRequestedFrame = 0;
					requestedFrameCount.store(0, std::memory_order_release);
					requestGeneration.fetch_add(1, std::memory_order_release);
				}

				ConvolutionBank* expected = pending;
				if (pendingBank.compare_exchange_strong(
						expected, NULL,
					std::memory_order_acq_rel,
					std::memory_order_acquire))
				{
					if (pending->frameCount == frameCount)
					{
						ConvolutionBank* previous = activeBank;
						activeBank = pending;
						activeUsable = true;
						fadePosition = fadeScratch != NULL ?
							0 : fadeLength;
						if (previous != NULL)
							retiredBank.store(
								previous, std::memory_order_release);
					}
					else
					{
						retiredBank.store(
							pending, std::memory_order_release);
					}
				}
			}
		}

		if (!activeUsable)
		{
			copyDry(output, input, frameCount);
			return;
		}
	}

	const bool fading = fadePosition < fadeLength && fadeScratch != NULL;
	if (fading)
	{
		for (unsigned i = 0; i < channelCount; ++i)
			memcpy(
				fadeScratch + static_cast<size_t>(i) * maxFrameCount,
				input[i],
				frameCount * sizeof(double));
	}

	for (unsigned i = 0; i < channelCount; ++i)
	{
		double* inputChannel = input[i];
		double* outputChannel = output[i];
		HConvSingle* filter = &activeBank->filters[i];

		hcPutSingle(filter, inputChannel);
		hcProcessSingle(filter);
		hcGetSingle(filter, outputChannel);
	}

	if (fading)
	{
		for (unsigned i = 0; i < channelCount; ++i)
		{
			const double* dry =
				fadeScratch + static_cast<size_t>(i) * maxFrameCount;
			for (unsigned frame = 0; frame < frameCount; ++frame)
			{
				const unsigned fadeFrame = (std::min)(
					fadeLength, fadePosition + frame + 1);
				const double wet = static_cast<double>(fadeFrame) /
					static_cast<double>(fadeLength);
				output[i][frame] = dry[frame] * (1.0 - wet) +
					output[i][frame] * wet;
			}
		}
		const unsigned remaining = fadeLength - fadePosition;
		fadePosition += (std::min)(remaining, frameCount);
	}
}
#pragma AVRT_CODE_END

unsigned long __stdcall ConvolutionFilter::bankWorkerEntry(void* context)
{
	ConvolutionFilter* filter = static_cast<ConvolutionFilter*>(context);
	filter->bankWorkerLoop();
	return 0;
}

void ConvolutionFilter::bankWorkerLoop()
{
	std::uint64_t failedGeneration = 0;
	bool requestFailed = false;
	HANDLE stopEvent = static_cast<HANDLE>(workerStopEvent);
	for (;;)
	{
		const DWORD waitResult = WaitForSingleObject(stopEvent, 10);
		if (waitResult == WAIT_OBJECT_0 || waitResult == WAIT_FAILED)
			break;

		ConvolutionBank* retired = retiredBank.exchange(
			NULL, std::memory_order_acq_rel);
		if (retired != NULL)
			destroyBank(retired);

		const std::uint64_t generation =
			requestGeneration.load(std::memory_order_acquire);
		const unsigned requested =
			requestedFrameCount.load(std::memory_order_acquire);
		if (requested == 0 ||
			(requestFailed && generation == failedGeneration) ||
			pendingBank.load(std::memory_order_acquire) != NULL)
		{
			continue;
		}

		ConvolutionBank* bank = createBank(requested);
		if (bank == NULL)
		{
			failedGeneration = generation;
			requestFailed = true;
			continue;
		}

		if (WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0 ||
			requestedFrameCount.load(std::memory_order_acquire) != requested ||
			requestGeneration.load(std::memory_order_acquire) != generation)
		{
			destroyBank(bank);
			continue;
		}

		ConvolutionBank* expected = NULL;
		if (!pendingBank.compare_exchange_strong(
				expected, bank,
				std::memory_order_release,
				std::memory_order_relaxed))
		{
			destroyBank(bank);
		}
	}
}

void ConvolutionFilter::cleanup()
{
	// FilterConfiguration only destroys or reinitializes filters after audio
	// callbacks have quiesced. Join our private builder before releasing the
	// immutable impulse cache or any bank it may still be constructing.
	stopWorker();
	requestedFrameCount.store(0, std::memory_order_release);
	requestGeneration.fetch_add(1, std::memory_order_release);

	ConvolutionBank* pending = pendingBank.exchange(
		NULL, std::memory_order_acq_rel);
	ConvolutionBank* retired = retiredBank.exchange(
		NULL, std::memory_order_acq_rel);
	ConvolutionBank* active = activeBank;
	activeBank = NULL;
	if (pending != NULL && pending != active)
		destroyBank(pending);
	if (retired != NULL && retired != active && retired != pending)
		destroyBank(retired);
	if (active != NULL)
		destroyBank(active);

	impulseResponses.clear();
	if (fadeScratch != NULL)
	{
		MemoryHelper::free(fadeScratch);
		fadeScratch = NULL;
	}
	fadeLength = 0;
	fadePosition = 0;
	lastRequestedFrame = 0;
	activeUsable = false;
}

void ConvolutionFilter::stopWorker()
{
	HANDLE thread = static_cast<HANDLE>(workerThread);
	HANDLE stopEvent = static_cast<HANDLE>(workerStopEvent);
	if (thread != NULL)
	{
		if (stopEvent != NULL)
			SetEvent(stopEvent);
		WaitForSingleObject(thread, INFINITE);
		CloseHandle(thread);
		workerThread = NULL;
	}
	if (stopEvent != NULL)
	{
		CloseHandle(stopEvent);
		workerStopEvent = NULL;
	}
}

bool ConvolutionFilter::startWorker()
{
	workerStopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
	if (workerStopEvent == NULL)
		return false;

	workerThread = CreateThread(
		NULL, 0, &ConvolutionFilter::bankWorkerEntry, this, 0, NULL);
	if (workerThread == NULL)
	{
		CloseHandle(static_cast<HANDLE>(workerStopEvent));
		workerStopEvent = NULL;
		return false;
	}
	return true;
}

bool ConvolutionFilter::prepareImpulseResponse(
	std::vector<std::vector<double>>& preparedImpulseResponses)
{
	SF_INFO info = {};

	SNDFILE* inFile = sf_wchar_open(filename.c_str(), SFM_READ, &info);
	if (inFile == NULL)
	{
		LogF(L"Error while reading impulse response file: %S", sf_strerror(inFile));
		return false;
	}

	TraceF(L"Convolving using impulse response file %s", filename.c_str());
	if (info.channels <= 0 || channelCount == 0 ||
		channelCount > static_cast<unsigned>(std::numeric_limits<int>::max()) ||
		info.samplerate <= 0 || !std::isfinite(sampleRate) ||
		sampleRate <= 0.0f ||
		sampleRate > static_cast<float>(std::numeric_limits<int>::max()))
	{
		LogF(L"Invalid impulse response metadata for %s: %d channels, %lld frames, source SR %d, target SR %f",
			filename.c_str(), info.channels,
			static_cast<long long>(info.frames), info.samplerate, sampleRate);
		sf_close(inFile);
		return false;
	}

	size_t interleavedSampleCount = 0;
	size_t convolutionSampleCount = 0;
	const int convolutionChannelCount = (std::max)(
		info.channels, static_cast<int>(channelCount));
	if (!isSafeImpulseShape(
			info.frames, info.channels, interleavedSampleCount) ||
		!isSafeImpulseShape(
			info.frames, convolutionChannelCount,
			convolutionSampleCount) ||
		convolutionSampleCount == 0)
	{
		LogF(L"Impulse response is too large to process safely: %s (%d channels, %lld frames)",
			filename.c_str(), info.channels,
			static_cast<long long>(info.frames));
		sf_close(inFile);
		return false;
	}

	const unsigned fileChannelCount =
		static_cast<unsigned>(info.channels);
	const unsigned fileFrameCount = static_cast<unsigned>(info.frames);
	const int targetSampleRate = static_cast<int>(sampleRate + 0.5f);
	TraceF(L"Impulse response metadata: source SR %d Hz, target SR %d Hz, %u channels, %u frames",
		info.samplerate, targetSampleRate, fileChannelCount, fileFrameCount);
	if (info.samplerate != targetSampleRate)
	{
		LogF(L"Impulse response sample rate (%d Hz) does not match device sample rate (%d Hz). Load or export an IR/FIR at the current device sample rate.",
			info.samplerate, targetSampleRate);
		sf_close(inFile);
		return false;
	}

	vector<double> interleavedBuf;
	vector<vector<double>> bufs;
	const unsigned usedFileChannelCount =
		(std::min)(fileChannelCount, channelCount);
	try
	{
		interleavedBuf.resize(interleavedSampleCount);
		bufs.resize(usedFileChannelCount);
		for (vector<double>& buf : bufs)
			buf.resize(fileFrameCount);
	}
	catch (const std::bad_alloc&)
	{
		LogF(L"Not enough memory to load impulse response file %s",
			filename.c_str());
		sf_close(inFile);
		return false;
	}

	sf_count_t numRead = 0;
	while (numRead < fileFrameCount)
	{
		const sf_count_t read = sf_readf_double(
			inFile,
			interleavedBuf.data() +
				static_cast<size_t>(numRead) * fileChannelCount,
			fileFrameCount - numRead);
		if (read <= 0)
			break;
		numRead += read;
	}

	const int readError = sf_error(inFile);
	const int closeResult = sf_close(inFile);
	if (numRead <= 0 || readError != SF_ERR_NO_ERROR || closeResult != 0)
	{
		LogF(L"Could not read impulse response samples from %s", filename.c_str());
		return false;
	}
	if (static_cast<unsigned>(numRead) != fileFrameCount)
	{
		LogF(L"Impulse response file %s was shorter than expected: read %ld of %u frames",
			filename.c_str(), static_cast<long>(numRead), fileFrameCount);
		return false;
	}

	for (unsigned i = 0; i < usedFileChannelCount; ++i)
	{
		double* const buf = bufs[i].data();
		const double* const p = interleavedBuf.data() + i;
		for (unsigned j = 0; j < fileFrameCount; ++j)
		{
			const double sample = p[j * fileChannelCount];
			if (!std::isfinite(sample))
			{
				LogF(L"Impulse response contains invalid samples: %s",
					filename.c_str());
				return false;
			}
			buf[j] = sample;
		}
	}

	preparedImpulseResponses.swap(bufs);
	return true;
}

ConvolutionFilter::ConvolutionBank* ConvolutionFilter::createBank(
	unsigned frameCount)
{
	if (frameCount == 0 || frameCount > maxFrameCount ||
		frameCount > static_cast<unsigned>(std::numeric_limits<int>::max() / 2) ||
		channelCount == 0 || impulseResponses.empty() ||
		channelCount >
			(std::numeric_limits<size_t>::max() - 16) / sizeof(HConvSingle))
	{
		return NULL;
	}

	void* bankStorage = MemoryHelper::alloc(sizeof(ConvolutionBank));
	if (bankStorage == NULL)
		return NULL;
	ConvolutionBank* bank = new(bankStorage) ConvolutionBank();
	bank->frameCount = frameCount;
	bank->channelCount = channelCount;
	const size_t filterBytes = sizeof(HConvSingle) * channelCount;
	bank->filters = static_cast<HConvSingle*>(MemoryHelper::alloc(filterBytes));
	if (bank->filters == NULL)
	{
		destroyBank(bank);
		return NULL;
	}
	memset(bank->filters, 0, filterBytes);

	for (unsigned i = 0; i < channelCount; ++i)
	{
		std::vector<double>& impulse =
			impulseResponses[i % impulseResponses.size()];
		if (impulse.empty() ||
			impulse.size() > static_cast<size_t>(std::numeric_limits<int>::max()) ||
			!hcInitSingle(&bank->filters[i], impulse.data(),
				static_cast<int>(impulse.size()),
				static_cast<int>(frameCount), 1))
		{
			destroyBank(bank);
			return NULL;
		}
	}
	return bank;
}

void ConvolutionFilter::destroyBank(ConvolutionBank* bank)
{
	if (bank == NULL)
		return;
	if (bank->filters != NULL)
	{
		for (unsigned i = 0; i < bank->channelCount; ++i)
			hcCloseSingle(&bank->filters[i]);
		MemoryHelper::free(bank->filters);
	}
	bank->~ConvolutionBank();
	MemoryHelper::free(bank);
}
