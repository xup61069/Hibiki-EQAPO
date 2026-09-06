#pragma once

#include <cstddef>
#include <cstdint>

#include <asio.h>

namespace HibikiAsio
{
enum class SampleEncoding : std::uint8_t
{
	Unsupported,
	Float32,
	Float64,
	SignedInteger
};

struct SampleFormat
{
	SampleEncoding encoding = SampleEncoding::Unsupported;
	std::uint8_t containerBytes = 0;
	std::uint8_t validBits = 0;
	bool isDsd = false;
	bool isBigEndian = false;

	bool isSupported() const noexcept
	{
		return encoding != SampleEncoding::Unsupported;
	}
};

class AsioSampleCodec final
{
public:
	static SampleFormat describe(ASIOSampleType type) noexcept;
	static bool decode(
		const void* source,
		const SampleFormat& format,
		double* destination,
		std::size_t frameCount) noexcept;
	static bool encode(
		const double* source,
		const SampleFormat& format,
		void* destination,
		std::size_t frameCount) noexcept;
};
}
