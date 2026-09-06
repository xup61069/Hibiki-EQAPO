#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

constexpr std::uint32_t OUTPROC_AUDIO_MAGIC = 0x4F504147; // OPAG
constexpr std::uint32_t OUTPROC_AUDIO_VERSION = 2;

constexpr std::uint32_t OUTPROC_AUDIO_STATUS_OK = 0;
constexpr std::uint32_t OUTPROC_AUDIO_STATUS_ERROR = 1;

constexpr std::uint32_t OUTPROC_AUDIO_DSP_GAIN = 1;
constexpr std::uint32_t OUTPROC_AUDIO_DSP_BIQUAD = 2;
constexpr std::uint32_t OUTPROC_AUDIO_DSP_VST = 3;
// Runtime filters use this only after a cooperative shutdown has timed out.
// A forced host exit must never be indistinguishable from a clean exit.
constexpr std::uint32_t OUTPROC_RUNTIME_FORCED_TERMINATION_EXIT_CODE = 25;
constexpr std::uint32_t OUTPROC_GUI_INFO_MAGIC = 0x4F504749; // OPGI
constexpr std::uint32_t OUTPROC_GUI_INFO_VERSION = 2;

struct OutProcGuiInfo
{
	std::uint32_t magic;
	std::uint32_t version;
	std::uint32_t processId;
	std::uint32_t reserved;
	std::uint64_t processCreationTime;
};

struct OutProcAudioHeader
{
	std::uint32_t magic;
	std::uint32_t version;
	std::uint32_t sampleRate;
	std::uint32_t channelCount;
	std::uint32_t maxFrames;
	std::uint32_t frameCount;
	std::uint32_t smoothingSamples;
	std::uint32_t dspType;
	double gainDb;
	double biquadA0;
	double biquadA1;
	double biquadA2;
	double biquadB1;
	double biquadB2;
	std::uint64_t requestSeq;
	std::uint64_t responseSeq;
	std::uint32_t status;
	std::uint32_t hostState;
};

inline bool OutProcAudioSampleCount(std::uint32_t channelCount, std::uint32_t frameCount, std::size_t& sampleCount)
{
	if (channelCount == 0 || frameCount == 0)
		return false;

	const std::size_t channels = channelCount;
	const std::size_t frames = frameCount;
	if (channels > (std::numeric_limits<std::size_t>::max)() / frames)
		return false;

	sampleCount = channels * frames;
	return true;
}

inline bool OutProcAudioSharedMemorySize(std::uint32_t channelCount, std::uint32_t maxFrames, std::size_t& byteCount)
{
	std::size_t sampleCount = 0;
	if (!OutProcAudioSampleCount(channelCount, maxFrames, sampleCount))
		return false;

	if (sampleCount > ((std::numeric_limits<std::size_t>::max)() - sizeof(OutProcAudioHeader)) / (2 * sizeof(double)))
		return false;

	byteCount = sizeof(OutProcAudioHeader) + sampleCount * 2 * sizeof(double);
	return true;
}

inline double* OutProcAudioInput(OutProcAudioHeader* header)
{
	return reinterpret_cast<double*>(header + 1);
}

inline double* OutProcAudioOutput(OutProcAudioHeader* header)
{
	return OutProcAudioInput(header) + static_cast<std::size_t>(header->channelCount) * header->maxFrames;
}
