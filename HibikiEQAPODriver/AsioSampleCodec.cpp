#include "AsioSampleCodec.h"

#include <cmath>
#include <cstring>
#include <limits>

namespace HibikiAsio
{
namespace
{
template <typename T>
T loadUnaligned(const std::uint8_t* source, bool bigEndian) noexcept
{
	T value{};
	if (!bigEndian)
	{
		std::memcpy(&value, source, sizeof(value));
	}
	else
	{
		std::uint8_t native[sizeof(T)]{};
		for (std::size_t index = 0; index < sizeof(T); ++index)
			native[index] = source[sizeof(T) - index - 1];
		std::memcpy(&value, native, sizeof(value));
	}
	return value;
}

template <typename T>
void storeUnaligned(
	std::uint8_t* destination,
	T value,
	bool bigEndian) noexcept
{
	std::uint8_t native[sizeof(T)]{};
	std::memcpy(native, &value, sizeof(value));
	if (!bigEndian)
	{
		std::memcpy(destination, native, sizeof(value));
	}
	else
	{
		for (std::size_t index = 0; index < sizeof(T); ++index)
			destination[index] = native[sizeof(T) - index - 1];
	}
}

std::int32_t loadInt24(
	const std::uint8_t* source,
	bool bigEndian) noexcept
{
	const std::uint32_t packed = bigEndian ?
		(static_cast<std::uint32_t>(source[2]) |
			(static_cast<std::uint32_t>(source[1]) << 8) |
			(static_cast<std::uint32_t>(source[0]) << 16)) :
		(static_cast<std::uint32_t>(source[0]) |
			(static_cast<std::uint32_t>(source[1]) << 8) |
			(static_cast<std::uint32_t>(source[2]) << 16));
	return (packed & 0x00800000U) != 0U ?
		static_cast<std::int32_t>(packed) - 0x01000000 :
		static_cast<std::int32_t>(packed);
}

void storeInt24(
	std::uint8_t* destination,
	std::int32_t value,
	bool bigEndian) noexcept
{
	const std::uint32_t packed = static_cast<std::uint32_t>(value);
	destination[bigEndian ? 2 : 0] = static_cast<std::uint8_t>(packed & 0xFFU);
	destination[1] = static_cast<std::uint8_t>((packed >> 8) & 0xFFU);
	destination[bigEndian ? 0 : 2] = static_cast<std::uint8_t>((packed >> 16) & 0xFFU);
}

double sanitizeFloat(double value) noexcept
{
	return std::isfinite(value) ? value : 0.0;
}

float encodeFloat32(double value) noexcept
{
	value = sanitizeFloat(value);
	const double maximum =
		static_cast<double>((std::numeric_limits<float>::max)());
	if (value > maximum)
		return (std::numeric_limits<float>::max)();
	if (value < -maximum)
		return (std::numeric_limits<float>::lowest)();
	return static_cast<float>(value);
}

std::int64_t encodeSignedInteger(double value, std::uint8_t validBits) noexcept
{
	const std::int64_t magnitude =
		static_cast<std::int64_t>(1) << (validBits - 1);
	const std::int64_t maximum = magnitude - 1;
	const std::int64_t minimum = -magnitude;
	value = sanitizeFloat(value);
	if (value <= -1.0)
		return minimum;
	if (value >= 1.0)
		return maximum;
	// decode() normalizes against 2^(bits-1), so using the same magnitude here
	// preserves every representable PCM code across an identity DSP pass. Using
	// maximum instead would turn 32767/32768 back into 32766, for example.
	const std::int64_t encoded =
		static_cast<std::int64_t>(std::llround(value * magnitude));
	return encoded < minimum ? minimum : (encoded > maximum ? maximum : encoded);
}
}

SampleFormat AsioSampleCodec::describe(ASIOSampleType type) noexcept
{
	switch (type)
	{
	case ASIOSTInt16MSB:
		return {SampleEncoding::SignedInteger, 2, 16, false, true};
	case ASIOSTInt24MSB:
		return {SampleEncoding::SignedInteger, 3, 24, false, true};
	case ASIOSTInt32MSB:
		return {SampleEncoding::SignedInteger, 4, 32, false, true};
	case ASIOSTFloat32MSB:
		return {SampleEncoding::Float32, 4, 32, false, true};
	case ASIOSTFloat64MSB:
		return {SampleEncoding::Float64, 8, 64, false, true};
	case ASIOSTInt32MSB16:
		return {SampleEncoding::SignedInteger, 4, 16, false, true};
	case ASIOSTInt32MSB18:
		return {SampleEncoding::SignedInteger, 4, 18, false, true};
	case ASIOSTInt32MSB20:
		return {SampleEncoding::SignedInteger, 4, 20, false, true};
	case ASIOSTInt32MSB24:
		return {SampleEncoding::SignedInteger, 4, 24, false, true};
	case ASIOSTInt16LSB:
		return {SampleEncoding::SignedInteger, 2, 16, false, false};
	case ASIOSTInt24LSB:
		return {SampleEncoding::SignedInteger, 3, 24, false, false};
	case ASIOSTInt32LSB:
		return {SampleEncoding::SignedInteger, 4, 32, false, false};
	case ASIOSTFloat32LSB:
		return {SampleEncoding::Float32, 4, 32, false, false};
	case ASIOSTFloat64LSB:
		return {SampleEncoding::Float64, 8, 64, false, false};
	case ASIOSTInt32LSB16:
		return {SampleEncoding::SignedInteger, 4, 16, false, false};
	case ASIOSTInt32LSB18:
		return {SampleEncoding::SignedInteger, 4, 18, false, false};
	case ASIOSTInt32LSB20:
		return {SampleEncoding::SignedInteger, 4, 20, false, false};
	case ASIOSTInt32LSB24:
		return {SampleEncoding::SignedInteger, 4, 24, false, false};
	case ASIOSTDSDInt8LSB1:
	case ASIOSTDSDInt8MSB1:
	case ASIOSTDSDInt8NER8:
		return {SampleEncoding::Unsupported, 1, 1, true, false};
	default:
		return {SampleEncoding::Unsupported, 0, 0, false, false};
	}
}

bool AsioSampleCodec::decode(
	const void* source,
	const SampleFormat& format,
	double* destination,
	std::size_t frameCount) noexcept
{
	if (source == nullptr || destination == nullptr || !format.isSupported())
		return false;

	const auto* bytes = static_cast<const std::uint8_t*>(source);
	for (std::size_t frame = 0; frame < frameCount; ++frame)
	{
		const std::uint8_t* sample = bytes + frame * format.containerBytes;
		switch (format.encoding)
		{
		case SampleEncoding::Float32:
			destination[frame] = sanitizeFloat(
				static_cast<double>(loadUnaligned<float>(sample, format.isBigEndian)));
			break;
		case SampleEncoding::Float64:
			destination[frame] = sanitizeFloat(
				loadUnaligned<double>(sample, format.isBigEndian));
			break;
		case SampleEncoding::SignedInteger:
		{
			std::int64_t value = 0;
			if (format.containerBytes == 2)
				value = loadUnaligned<std::int16_t>(sample, format.isBigEndian);
			else if (format.containerBytes == 3)
				value = loadInt24(sample, format.isBigEndian);
			else if (format.containerBytes == 4)
				value = loadUnaligned<std::int32_t>(sample, format.isBigEndian);
			else
				return false;

			// The Int32*16/18/20/24 variants are right-aligned. Some ASIO
			// producers sign-extend the unused high bits while others leave them
			// zero; only validBits belongs to the signal in either representation.
			const std::uint64_t valueMask =
				(static_cast<std::uint64_t>(1) << format.validBits) - 1;
			const std::uint64_t signBit =
				static_cast<std::uint64_t>(1) << (format.validBits - 1);
			const std::uint64_t validValue =
				static_cast<std::uint64_t>(value) & valueMask;
			value = (validValue & signBit) != 0 ?
				static_cast<std::int64_t>(validValue) -
					(static_cast<std::int64_t>(1) << format.validBits) :
				static_cast<std::int64_t>(validValue);

			const double denominator = std::ldexp(1.0, format.validBits - 1);
			destination[frame] = static_cast<double>(value) / denominator;
			break;
		}
		default:
			return false;
		}
	}
	return true;
}

bool AsioSampleCodec::encode(
	const double* source,
	const SampleFormat& format,
	void* destination,
	std::size_t frameCount) noexcept
{
	if (source == nullptr || destination == nullptr || !format.isSupported())
		return false;

	auto* bytes = static_cast<std::uint8_t*>(destination);
	for (std::size_t frame = 0; frame < frameCount; ++frame)
	{
		std::uint8_t* sample = bytes + frame * format.containerBytes;
		switch (format.encoding)
		{
		case SampleEncoding::Float32:
			storeUnaligned(
				sample, encodeFloat32(source[frame]), format.isBigEndian);
			break;
		case SampleEncoding::Float64:
			storeUnaligned(sample, sanitizeFloat(source[frame]), format.isBigEndian);
			break;
		case SampleEncoding::SignedInteger:
		{
			const std::int64_t encoded =
				encodeSignedInteger(source[frame], format.validBits);
			if (format.containerBytes == 2)
				storeUnaligned(sample,
					static_cast<std::int16_t>(encoded), format.isBigEndian);
			else if (format.containerBytes == 3)
				storeInt24(sample,
					static_cast<std::int32_t>(encoded), format.isBigEndian);
			else if (format.containerBytes == 4)
				storeUnaligned(sample,
					static_cast<std::int32_t>(encoded), format.isBigEndian);
			else
				return false;
			break;
		}
		default:
			return false;
		}
	}
	return true;
}
}
