#include "AsioSampleCodec.h"

#include <cmath>
#include <cstring>
#include <immintrin.h>
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

	if (frameCount == 0)
		return true;

	const auto* bytes = static_cast<const std::uint8_t*>(source);

	// Fast paths for common Little-Endian formats
	if (!format.isBigEndian)
	{
		if (format.encoding == SampleEncoding::Float32)
		{
			std::size_t frame = 0;
#if defined(__AVX2__) && !defined(_M_ARM64)
			const __m128i expMask = _mm_set1_epi32(0x7F800000);
			for (; frame + 4 <= frameCount; frame += 4)
			{
				const __m128i raw = _mm_loadu_si128(reinterpret_cast<const __m128i*>(bytes + frame * 4));
				const __m128i isInfOrNan = _mm_cmpeq_epi32(_mm_and_si128(raw, expMask), expMask);
				if (_mm_testz_si128(isInfOrNan, isInfOrNan))
				{
					const __m256d d4 = _mm256_cvtps_pd(_mm_castsi128_ps(raw));
					_mm256_storeu_pd(destination + frame, d4);
				}
				else
				{
					for (std::size_t k = 0; k < 4; ++k)
					{
						float sampleVal{};
						std::memcpy(&sampleVal, bytes + (frame + k) * 4, sizeof(float));
						destination[frame + k] = sanitizeFloat(static_cast<double>(sampleVal));
					}
				}
			}
#endif
			for (; frame < frameCount; ++frame)
			{
				float sampleVal{};
				std::memcpy(&sampleVal, bytes + frame * 4, sizeof(float));
				destination[frame] = sanitizeFloat(static_cast<double>(sampleVal));
			}
			return true;
		}
		if (format.encoding == SampleEncoding::Float64)
		{
			std::size_t frame = 0;
#if defined(__AVX2__) && !defined(_M_ARM64)
			const __m256i expMask = _mm256_set1_epi64x(0x7FF0000000000000ULL);
			for (; frame + 4 <= frameCount; frame += 4)
			{
				const __m256i raw = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(bytes + frame * 8));
				const __m256i isInfOrNan = _mm256_cmpeq_epi64(_mm256_and_si256(raw, expMask), expMask);
				if (_mm256_testz_si256(isInfOrNan, isInfOrNan))
				{
					_mm256_storeu_pd(destination + frame, _mm256_castsi256_pd(raw));
				}
				else
				{
					for (std::size_t k = 0; k < 4; ++k)
					{
						double sampleVal{};
						std::memcpy(&sampleVal, bytes + (frame + k) * 8, sizeof(double));
						destination[frame + k] = sanitizeFloat(sampleVal);
					}
				}
			}
#endif
			for (; frame < frameCount; ++frame)
			{
				double sampleVal{};
				std::memcpy(&sampleVal, bytes + frame * 8, sizeof(double));
				destination[frame] = sanitizeFloat(sampleVal);
			}
			return true;
		}
		if (format.encoding == SampleEncoding::SignedInteger)
		{
			if (format.containerBytes == 4 && format.validBits == 32)
			{
				constexpr double invScale = 1.0 / 2147483648.0;
				std::size_t frame = 0;
#if defined(__AVX2__) && !defined(_M_ARM64)
				const __m256d scaleVec = _mm256_set1_pd(invScale);
				for (; frame + 4 <= frameCount; frame += 4)
				{
					__m128i in32 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(bytes + frame * 4));
					__m256d d4 = _mm256_cvtepi32_pd(in32);
					__m256d scaled = _mm256_mul_pd(d4, scaleVec);
					_mm256_storeu_pd(destination + frame, scaled);
				}
#endif
				for (; frame < frameCount; ++frame)
				{
					std::int32_t val{};
					std::memcpy(&val, bytes + frame * 4, sizeof(val));
					destination[frame] = static_cast<double>(val) * invScale;
				}
				return true;
			}
			if (format.containerBytes == 2 && format.validBits == 16)
			{
				constexpr double invScale = 1.0 / 32768.0;
				std::size_t frame = 0;
#if defined(__AVX2__) && !defined(_M_ARM64)
				const __m256d scaleVec = _mm256_set1_pd(invScale);
				for (; frame + 4 <= frameCount; frame += 4)
				{
					__m128i in16 = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(bytes + frame * 2));
					__m128i in32 = _mm_cvtepi16_epi32(in16);
					__m256d d4 = _mm256_cvtepi32_pd(in32);
					__m256d scaled = _mm256_mul_pd(d4, scaleVec);
					_mm256_storeu_pd(destination + frame, scaled);
				}
#endif
				for (; frame < frameCount; ++frame)
				{
					std::int16_t val{};
					std::memcpy(&val, bytes + frame * 2, sizeof(val));
					destination[frame] = static_cast<double>(val) * invScale;
				}
				return true;
			}
			if (format.containerBytes == 3 && format.validBits == 24)
			{
				constexpr double invScale = 1.0 / 8388608.0;
				std::size_t frame = 0;
#if defined(__AVX2__) && !defined(_M_ARM64)
				const __m256d scaleVec = _mm256_set1_pd(invScale);
				const __m128i unpackMask = _mm_setr_epi8(
					0, 1, 2, static_cast<char>(0x80),
					3, 4, 5, static_cast<char>(0x80),
					6, 7, 8, static_cast<char>(0x80),
					9, 10, 11, static_cast<char>(0x80));
				for (; frame + 4 <= frameCount; frame += 4)
				{
					const __m128i lo8 = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(bytes + frame * 3));
					std::uint32_t hi4{};
					std::memcpy(&hi4, bytes + frame * 3 + 8, sizeof(hi4));
					const __m128i in12 = _mm_unpacklo_epi64(lo8, _mm_cvtsi32_si128(hi4));
					const __m128i shuffled = _mm_shuffle_epi8(in12, unpackMask);
					const __m128i signExtended = _mm_srai_epi32(_mm_slli_epi32(shuffled, 8), 8);
					const __m256d d4 = _mm256_cvtepi32_pd(signExtended);
					_mm256_storeu_pd(destination + frame, _mm256_mul_pd(d4, scaleVec));
				}
#endif
				for (; frame < frameCount; ++frame)
				{
					const std::uint8_t* p = bytes + frame * 3;
					const std::uint32_t packed =
						static_cast<std::uint32_t>(p[0]) |
						(static_cast<std::uint32_t>(p[1]) << 8) |
						(static_cast<std::uint32_t>(p[2]) << 16);
					const std::int32_t val = (static_cast<std::int32_t>(packed << 8)) >> 8;
					destination[frame] = static_cast<double>(val) * invScale;
				}
				return true;
			}
		}
	}

	// General path for Big-Endian and right-aligned formats (e.g. Int32*16/18/20/24)
	const double invDenominator = 1.0 / std::ldexp(1.0, format.validBits - 1);
	const std::uint64_t valueMask = (format.validBits < 64) ?
		((static_cast<std::uint64_t>(1) << format.validBits) - 1) : ~0ULL;
	const std::uint64_t signBit = (format.validBits > 0 && format.validBits <= 64) ?
		(static_cast<std::uint64_t>(1) << (format.validBits - 1)) : 0ULL;
	const std::int64_t signExtend = (format.validBits < 64) ?
		(static_cast<std::int64_t>(1) << format.validBits) : 0;

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

			const std::uint64_t validValue =
				static_cast<std::uint64_t>(value) & valueMask;
			value = (validValue & signBit) != 0 ?
				static_cast<std::int64_t>(validValue) - signExtend :
				static_cast<std::int64_t>(validValue);

			destination[frame] = static_cast<double>(value) * invDenominator;
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

	if (frameCount == 0)
		return true;

	auto* bytes = static_cast<std::uint8_t*>(destination);

	// Fast paths for common Little-Endian formats
	if (!format.isBigEndian)
	{
		if (format.encoding == SampleEncoding::Float32)
		{
			std::size_t frame = 0;
#if defined(__AVX2__) && !defined(_M_ARM64)
			const __m256d maxFloatD = _mm256_set1_pd(static_cast<double>((std::numeric_limits<float>::max)()));
			const __m256d minFloatD = _mm256_set1_pd(static_cast<double>((std::numeric_limits<float>::lowest)()));
			const __m256i expMask = _mm256_set1_epi64x(0x7FF0000000000000ULL);
			for (; frame + 4 <= frameCount; frame += 4)
			{
				const __m256i raw = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(source + frame));
				const __m256i isInfOrNan = _mm256_cmpeq_epi64(_mm256_and_si256(raw, expMask), expMask);
				if (_mm256_testz_si256(isInfOrNan, isInfOrNan))
				{
					__m256d d4 = _mm256_castsi256_pd(raw);
					d4 = _mm256_min_pd(_mm256_max_pd(d4, minFloatD), maxFloatD);
					const __m128 f4 = _mm256_cvtpd_ps(d4);
					_mm_storeu_ps(reinterpret_cast<float*>(bytes + frame * 4), f4);
				}
				else
				{
					for (std::size_t k = 0; k < 4; ++k)
					{
						const float val = encodeFloat32(source[frame + k]);
						std::memcpy(bytes + (frame + k) * 4, &val, sizeof(float));
					}
				}
			}
#endif
			for (; frame < frameCount; ++frame)
			{
				const float val = encodeFloat32(source[frame]);
				std::memcpy(bytes + frame * 4, &val, sizeof(float));
			}
			return true;
		}
		if (format.encoding == SampleEncoding::Float64)
		{
			std::size_t frame = 0;
#if defined(__AVX2__) && !defined(_M_ARM64)
			const __m256i expMask = _mm256_set1_epi64x(0x7FF0000000000000ULL);
			for (; frame + 4 <= frameCount; frame += 4)
			{
				const __m256i raw = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(source + frame));
				const __m256i isInfOrNan = _mm256_cmpeq_epi64(_mm256_and_si256(raw, expMask), expMask);
				if (_mm256_testz_si256(isInfOrNan, isInfOrNan))
				{
					_mm256_storeu_pd(reinterpret_cast<double*>(bytes + frame * 8), _mm256_castsi256_pd(raw));
				}
				else
				{
					for (std::size_t k = 0; k < 4; ++k)
					{
						const double val = sanitizeFloat(source[frame + k]);
						std::memcpy(bytes + (frame + k) * 8, &val, sizeof(double));
					}
				}
			}
#endif
			for (; frame < frameCount; ++frame)
			{
				const double val = sanitizeFloat(source[frame]);
				std::memcpy(bytes + frame * 8, &val, sizeof(double));
			}
			return true;
		}
		if (format.encoding == SampleEncoding::SignedInteger)
		{
			if (format.containerBytes == 4 && format.validBits == 32)
			{
				constexpr double magnitude = 2147483648.0;
				constexpr std::int32_t maximum = 2147483647;
				constexpr std::int32_t minimum = -2147483647 - 1;
				std::size_t frame = 0;
#if defined(__AVX2__) && !defined(_M_ARM64)
				const __m256i expMask = _mm256_set1_epi64x(0x7FF0000000000000ULL);
				const __m256d magVec = _mm256_set1_pd(magnitude);
				const __m256d minD = _mm256_set1_pd(static_cast<double>(minimum));
				const __m256d maxD = _mm256_set1_pd(static_cast<double>(maximum));
				for (; frame + 4 <= frameCount; frame += 4)
				{
					const __m256i raw = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(source + frame));
					const __m256i isInfOrNan = _mm256_cmpeq_epi64(_mm256_and_si256(raw, expMask), expMask);
					if (_mm256_testz_si256(isInfOrNan, isInfOrNan))
					{
						__m256d scaled = _mm256_mul_pd(_mm256_castsi256_pd(raw), magVec);
						scaled = _mm256_min_pd(_mm256_max_pd(scaled, minD), maxD);
						const __m128i i4 = _mm256_cvtpd_epi32(scaled);
						_mm_storeu_si128(reinterpret_cast<__m128i*>(bytes + frame * 4), i4);
					}
					else
					{
						for (std::size_t k = 0; k < 4; ++k)
						{
							double value = sanitizeFloat(source[frame + k]);
							std::int32_t encoded;
							if (value <= -1.0)
								encoded = minimum;
							else if (value >= 1.0)
								encoded = maximum;
							else
							{
								const std::int64_t rounded = _mm_cvtsd_si64(_mm_set_sd(value * magnitude));
								encoded = static_cast<std::int32_t>(rounded < minimum ? minimum : (rounded > maximum ? maximum : rounded));
							}
							std::memcpy(bytes + (frame + k) * 4, &encoded, sizeof(encoded));
						}
					}
				}
#endif
				for (; frame < frameCount; ++frame)
				{
					double value = sanitizeFloat(source[frame]);
					std::int32_t encoded;
					if (value <= -1.0)
						encoded = minimum;
					else if (value >= 1.0)
						encoded = maximum;
					else
					{
						const std::int64_t rounded = _mm_cvtsd_si64(_mm_set_sd(value * magnitude));
						encoded = static_cast<std::int32_t>(rounded < minimum ? minimum : (rounded > maximum ? maximum : rounded));
					}
					std::memcpy(bytes + frame * 4, &encoded, sizeof(encoded));
				}
				return true;
			}
			if (format.containerBytes == 2 && format.validBits == 16)
			{
				constexpr double magnitude = 32768.0;
				constexpr std::int16_t maximum = 32767;
				constexpr std::int16_t minimum = -32768;
				std::size_t frame = 0;
#if defined(__AVX2__) && !defined(_M_ARM64)
				const __m256i expMask = _mm256_set1_epi64x(0x7FF0000000000000ULL);
				const __m256d magVec = _mm256_set1_pd(magnitude);
				const __m256d minD = _mm256_set1_pd(static_cast<double>(minimum));
				const __m256d maxD = _mm256_set1_pd(static_cast<double>(maximum));
				for (; frame + 8 <= frameCount; frame += 8)
				{
					const __m256i raw0 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(source + frame));
					const __m256i raw1 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(source + frame + 4));
					const __m256i infOrNan0 = _mm256_cmpeq_epi64(_mm256_and_si256(raw0, expMask), expMask);
					const __m256i infOrNan1 = _mm256_cmpeq_epi64(_mm256_and_si256(raw1, expMask), expMask);
					const __m256i combined = _mm256_or_si256(infOrNan0, infOrNan1);
					if (_mm256_testz_si256(combined, combined))
					{
						__m256d scaled0 = _mm256_mul_pd(_mm256_castsi256_pd(raw0), magVec);
						__m256d scaled1 = _mm256_mul_pd(_mm256_castsi256_pd(raw1), magVec);
						scaled0 = _mm256_min_pd(_mm256_max_pd(scaled0, minD), maxD);
						scaled1 = _mm256_min_pd(_mm256_max_pd(scaled1, minD), maxD);
						const __m128i i4_0 = _mm256_cvtpd_epi32(scaled0);
						const __m128i i4_1 = _mm256_cvtpd_epi32(scaled1);
						const __m128i packed = _mm_packs_epi32(i4_0, i4_1);
						_mm_storeu_si128(reinterpret_cast<__m128i*>(bytes + frame * 2), packed);
					}
					else
					{
						for (std::size_t k = 0; k < 8; ++k)
						{
							double value = sanitizeFloat(source[frame + k]);
							std::int16_t encoded;
							if (value <= -1.0)
								encoded = minimum;
							else if (value >= 1.0)
								encoded = maximum;
							else
							{
								const std::int64_t rounded = _mm_cvtsd_si64(_mm_set_sd(value * magnitude));
								encoded = static_cast<std::int16_t>(rounded < minimum ? minimum : (rounded > maximum ? maximum : rounded));
							}
							std::memcpy(bytes + (frame + k) * 2, &encoded, sizeof(encoded));
						}
					}
				}
				if (frame + 4 <= frameCount)
				{
					const __m256i raw = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(source + frame));
					const __m256i isInfOrNan = _mm256_cmpeq_epi64(_mm256_and_si256(raw, expMask), expMask);
					if (_mm256_testz_si256(isInfOrNan, isInfOrNan))
					{
						__m256d scaled = _mm256_mul_pd(_mm256_castsi256_pd(raw), magVec);
						scaled = _mm256_min_pd(_mm256_max_pd(scaled, minD), maxD);
						const __m128i i4 = _mm256_cvtpd_epi32(scaled);
						const __m128i packed = _mm_packs_epi32(i4, i4);
						_mm_storel_epi64(reinterpret_cast<__m128i*>(bytes + frame * 2), packed);
						frame += 4;
					}
				}
#endif
				for (; frame < frameCount; ++frame)
				{
					double value = sanitizeFloat(source[frame]);
					std::int16_t encoded;
					if (value <= -1.0)
						encoded = minimum;
					else if (value >= 1.0)
						encoded = maximum;
					else
					{
						const std::int64_t rounded = _mm_cvtsd_si64(_mm_set_sd(value * magnitude));
						encoded = static_cast<std::int16_t>(rounded < minimum ? minimum : (rounded > maximum ? maximum : rounded));
					}
					std::memcpy(bytes + frame * 2, &encoded, sizeof(encoded));
				}
				return true;
			}
			if (format.containerBytes == 3 && format.validBits == 24)
			{
				constexpr double magnitude = 8388608.0;
				constexpr std::int32_t maximum = 8388607;
				constexpr std::int32_t minimum = -8388608;
				std::size_t frame = 0;
#if defined(__AVX2__) && !defined(_M_ARM64)
				const __m256i expMask = _mm256_set1_epi64x(0x7FF0000000000000ULL);
				const __m256d magVec = _mm256_set1_pd(magnitude);
				const __m256d minD = _mm256_set1_pd(static_cast<double>(minimum));
				const __m256d maxD = _mm256_set1_pd(static_cast<double>(maximum));
				const __m128i packMask = _mm_setr_epi8(
					0, 1, 2,  4, 5, 6,  8, 9, 10,  12, 13, 14,  -1, -1, -1, -1);
				for (; frame + 4 <= frameCount; frame += 4)
				{
					const __m256i raw = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(source + frame));
					const __m256i isInfOrNan = _mm256_cmpeq_epi64(_mm256_and_si256(raw, expMask), expMask);
					if (_mm256_testz_si256(isInfOrNan, isInfOrNan))
					{
						__m256d scaled = _mm256_mul_pd(_mm256_castsi256_pd(raw), magVec);
						scaled = _mm256_min_pd(_mm256_max_pd(scaled, minD), maxD);
						const __m128i i4 = _mm256_cvtpd_epi32(scaled);
						const __m128i packed = _mm_shuffle_epi8(i4, packMask);
						_mm_storel_epi64(reinterpret_cast<__m128i*>(bytes + frame * 3), packed);
						const std::uint32_t tail4 = static_cast<std::uint32_t>(
							_mm_cvtsi128_si32(_mm_srli_si128(packed, 8)));
						std::memcpy(bytes + frame * 3 + 8, &tail4, sizeof(tail4));
					}
					else
					{
						for (std::size_t k = 0; k < 4; ++k)
						{
							double value = sanitizeFloat(source[frame + k]);
							std::int32_t encoded;
							if (value <= -1.0)
								encoded = minimum;
							else if (value >= 1.0)
								encoded = maximum;
							else
							{
								const std::int64_t rounded = _mm_cvtsd_si64(_mm_set_sd(value * magnitude));
								encoded = static_cast<std::int32_t>(rounded < minimum ? minimum : (rounded > maximum ? maximum : rounded));
							}
							const std::uint32_t p = static_cast<std::uint32_t>(encoded);
							std::uint8_t* dst = bytes + (frame + k) * 3;
							dst[0] = static_cast<std::uint8_t>(p & 0xFFU);
							dst[1] = static_cast<std::uint8_t>((p >> 8) & 0xFFU);
							dst[2] = static_cast<std::uint8_t>((p >> 16) & 0xFFU);
						}
					}
				}
#endif
				for (; frame < frameCount; ++frame)
				{
					double value = sanitizeFloat(source[frame]);
					std::int32_t encoded;
					if (value <= -1.0)
						encoded = minimum;
					else if (value >= 1.0)
						encoded = maximum;
					else
					{
						const std::int64_t rounded = _mm_cvtsd_si64(_mm_set_sd(value * magnitude));
						encoded = static_cast<std::int32_t>(rounded < minimum ? minimum : (rounded > maximum ? maximum : rounded));
					}
					const std::uint32_t packed = static_cast<std::uint32_t>(encoded);
					std::uint8_t* dst = bytes + frame * 3;
					dst[0] = static_cast<std::uint8_t>(packed & 0xFFU);
					dst[1] = static_cast<std::uint8_t>((packed >> 8) & 0xFFU);
					dst[2] = static_cast<std::uint8_t>((packed >> 16) & 0xFFU);
				}
				return true;
			}
		}
	}

	// General path for Big-Endian and right-aligned formats
	const std::int64_t magnitude = (format.validBits > 0 && format.validBits <= 63) ?
		(static_cast<std::int64_t>(1) << (format.validBits - 1)) : 0;
	const std::int64_t maximum = magnitude - 1;
	const std::int64_t minimum = -magnitude;

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
			double value = sanitizeFloat(source[frame]);
			std::int64_t encoded;
			if (value <= -1.0)
				encoded = minimum;
			else if (value >= 1.0)
				encoded = maximum;
			else
			{
				encoded = _mm_cvtsd_si64(_mm_set_sd(value * magnitude));
				if (encoded < minimum) encoded = minimum;
				else if (encoded > maximum) encoded = maximum;
			}

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
