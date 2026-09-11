#include "stdafx.h"
#define _USE_MATH_DEFINES
#include <algorithm>
#include <cmath>
#ifndef _M_ARM64
#include <immintrin.h>
#endif
#include "PanFilter.h"
#include "AudioToolsHelper.h"

using namespace std;

PanFilter::PanFilter(double position, double widthPercent)
	: position(max(-100.0, min(100.0, position)) / 100.0), width(max(0.0, min(200.0, widthPercent)) / 100.0)
{
}

vector<wstring> PanFilter::initialize(float sampleRate, unsigned maxFrameCount, vector<wstring> channelNames)
{
	channelCount = static_cast<unsigned>(channelNames.size());
	return channelNames;
}

#pragma AVRT_CODE_BEGIN
void PanFilter::process(double** output, double** input, unsigned frameCount)
{
	if (channelCount < 2)
	{
		for (unsigned c = 0; c < channelCount; c++)
			if (output[c] != input[c])
				memcpy(output[c], input[c], frameCount * sizeof(double));
		return;
	}

	const double angle = (position + 1.0) * M_PI_4;
	const double leftPan = cos(angle);
	const double rightPan = sin(angle);
	const double midGain = max(0.0, 1.0 - width);
	const double sideGain = width;
	const double halfMidGain = 0.5 * midGain;
	const double sideLeftFactor = sideGain * leftPan;
	const double sideRightFactor = sideGain * rightPan;

	size_t i = 0;

#if defined(__AVX2__) && !defined(_M_ARM64)
	{
		const size_t simd_width = 4;
		if (frameCount >= simd_width)
		{
			const __m256d halfMid_v = _mm256_set1_pd(halfMidGain);
			const __m256d sideLeft_v = _mm256_set1_pd(sideLeftFactor);
			const __m256d sideRight_v = _mm256_set1_pd(sideRightFactor);
			for (; i + simd_width <= frameCount; i += simd_width)
			{
				const __m256d l = _mm256_loadu_pd(input[0] + i);
				const __m256d r = _mm256_loadu_pd(input[1] + i);
				const __m256d mid = _mm256_mul_pd(_mm256_add_pd(l, r), halfMid_v);
				const __m256d outL = _mm256_add_pd(mid, _mm256_mul_pd(l, sideLeft_v));
				const __m256d outR = _mm256_add_pd(mid, _mm256_mul_pd(r, sideRight_v));
				_mm256_storeu_pd(output[0] + i, outL);
				_mm256_storeu_pd(output[1] + i, outR);
			}
		}
	}
#endif

#if !defined(_M_ARM64)
	{
		const size_t simd_width = 2;
		if (frameCount - i >= simd_width)
		{
			const __m128d halfMid_v = _mm_set1_pd(halfMidGain);
			const __m128d sideLeft_v = _mm_set1_pd(sideLeftFactor);
			const __m128d sideRight_v = _mm_set1_pd(sideRightFactor);
			for (; i + simd_width <= frameCount; i += simd_width)
			{
				const __m128d l = _mm_loadu_pd(input[0] + i);
				const __m128d r = _mm_loadu_pd(input[1] + i);
				const __m128d mid = _mm_mul_pd(_mm_add_pd(l, r), halfMid_v);
				const __m128d outL = _mm_add_pd(mid, _mm_mul_pd(l, sideLeft_v));
				const __m128d outR = _mm_add_pd(mid, _mm_mul_pd(r, sideRight_v));
				_mm_storeu_pd(output[0] + i, outL);
				_mm_storeu_pd(output[1] + i, outR);
			}
		}
	}
#endif

	for (; i < frameCount; i++)
	{
		const double left = input[0][i];
		const double right = input[1][i];
		const double mid = 0.5 * (left + right) * midGain;
		const double sideLeft = left * sideGain * leftPan;
		const double sideRight = right * sideGain * rightPan;
		output[0][i] = mid + sideLeft;
		output[1][i] = mid + sideRight;
	}

	for (unsigned c = 2; c < channelCount; c++)
		if (output[c] != input[c])
			memcpy(output[c], input[c], frameCount * sizeof(double));
}

bool PanFilter::processSingle(float** output, float** input, unsigned frameCount)
{
	if (channelCount < 2)
	{
		for (unsigned c = 0; c < channelCount; c++)
			if (output[c] != input[c])
				memcpy(output[c], input[c], frameCount * sizeof(float));
		return true;
	}

	const double angle = (position + 1.0) * M_PI_4;
	const float leftPan = static_cast<float>(cos(angle));
	const float rightPan = static_cast<float>(sin(angle));
	const float midGain = static_cast<float>(max(0.0, 1.0 - width));
	const float sideGain = static_cast<float>(width);
	const float halfMidGain = 0.5f * midGain;
	const float sideLeftFactor = sideGain * leftPan;
	const float sideRightFactor = sideGain * rightPan;

	size_t i = 0;

#if defined(__AVX2__) && !defined(_M_ARM64)
	{
		const size_t simd_width = 8;
		if (frameCount >= simd_width)
		{
			const __m256 halfMid_v = _mm256_set1_ps(halfMidGain);
			const __m256 sideLeft_v = _mm256_set1_ps(sideLeftFactor);
			const __m256 sideRight_v = _mm256_set1_ps(sideRightFactor);
			for (; i + simd_width <= frameCount; i += simd_width)
			{
				const __m256 l = _mm256_loadu_ps(input[0] + i);
				const __m256 r = _mm256_loadu_ps(input[1] + i);
				const __m256 mid = _mm256_mul_ps(_mm256_add_ps(l, r), halfMid_v);
				const __m256 outL = _mm256_add_ps(mid, _mm256_mul_ps(l, sideLeft_v));
				const __m256 outR = _mm256_add_ps(mid, _mm256_mul_ps(r, sideRight_v));
				_mm256_storeu_ps(output[0] + i, outL);
				_mm256_storeu_ps(output[1] + i, outR);
			}
		}
	}
#endif

#if !defined(_M_ARM64)
	{
		const size_t simd_width = 4;
		if (frameCount - i >= simd_width)
		{
			const __m128 halfMid_v = _mm_set1_ps(halfMidGain);
			const __m128 sideLeft_v = _mm_set1_ps(sideLeftFactor);
			const __m128 sideRight_v = _mm_set1_ps(sideRightFactor);
			for (; i + simd_width <= frameCount; i += simd_width)
			{
				const __m128 l = _mm_loadu_ps(input[0] + i);
				const __m128 r = _mm_loadu_ps(input[1] + i);
				const __m128 mid = _mm_mul_ps(_mm_add_ps(l, r), halfMid_v);
				const __m128 outL = _mm_add_ps(mid, _mm_mul_ps(l, sideLeft_v));
				const __m128 outR = _mm_add_ps(mid, _mm_mul_ps(r, sideRight_v));
				_mm_storeu_ps(output[0] + i, outL);
				_mm_storeu_ps(output[1] + i, outR);
			}
		}
	}
#endif

	for (; i < frameCount; i++)
	{
		const float left = input[0][i];
		const float right = input[1][i];
		const float mid = (left + right) * halfMidGain;
		const float sideLeft = left * sideLeftFactor;
		const float sideRight = right * sideRightFactor;
		output[0][i] = mid + sideLeft;
		output[1][i] = mid + sideRight;
	}

	for (unsigned c = 2; c < channelCount; c++)
		if (output[c] != input[c])
			memcpy(output[c], input[c], frameCount * sizeof(float));

	return true;
}
#pragma AVRT_CODE_END
