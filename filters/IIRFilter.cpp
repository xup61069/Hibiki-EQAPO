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
#include <limits>
#include "helpers/MemoryHelper.h"
#include "IIRFilter.h"

using namespace std;

#define IS_DENORMAL(d) (abs(d) < DBL_MIN)

bool IIRFilter::coefficientsAreStable(const vector<double>& coefficients)
{
	if (coefficients.size() < 4 || coefficients.size() % 2 != 0)
		return false;
	const size_t filterOrder = coefficients.size() / 2 - 1;
	if (filterOrder == 0 || filterOrder > (std::numeric_limits<unsigned>::max)())
		return false;
	const double a0 = coefficients[filterOrder + 1];
	if (!std::isfinite(a0) || a0 == 0.0)
		return false;

	// Schur recursion for A(z) = a0*z^N + ... + aN. Every reflection
	// coefficient must be strictly inside the unit circle. Normalize each
	// reduction to avoid underflow for higher-order, otherwise valid filters.
	vector<double> denominator(filterOrder + 1);
	vector<double> reduced(filterOrder + 1);
	for (size_t i = 0; i <= filterOrder; ++i)
	{
		denominator[i] = coefficients[filterOrder + 1 + i] / a0;
		if (!std::isfinite(denominator[i]))
			return false;
	}
	for (size_t degree = filterOrder; degree > 0; --degree)
	{
		const double leading = denominator[0];
		const double reflection = denominator[degree] / leading;
		if (!std::isfinite(reflection) || abs(reflection) >= 1.0)
			return false;
		for (size_t i = 0; i < degree; ++i)
		{
			reduced[i] = denominator[i] -
				reflection * denominator[degree - i];
			if (!std::isfinite(reduced[i]))
				return false;
		}
		const double scale = reduced[0];
		if (!std::isfinite(scale) || scale == 0.0)
			return false;
		for (size_t i = 0; i < degree; ++i)
		{
			denominator[i] = reduced[i] / scale;
			if (!std::isfinite(denominator[i]))
				return false;
		}
	}
	return true;
}

IIRFilter::IIRFilter(const vector<double>& coefficients)
	: order(coefficients.size() >= 4 &&
		coefficients.size() / 2 - 1 <= (std::numeric_limits<unsigned>::max)() ?
		static_cast<unsigned>(coefficients.size() / 2 - 1) : 0),
	  b0(1.0),
	  a(NULL),
	  b(NULL),
	  channelCount(0),
	  x(NULL),
	  y(NULL),
	  allocationFailed(false),
	  stateReady(false)
{
	if (order == 0 || coefficients.size() != (static_cast<size_t>(order) + 1) * 2 ||
		!coefficientsAreStable(coefficients))
	{
		allocationFailed = true;
		return;
	}
	for (double coefficient : coefficients)
	{
		if (!std::isfinite(coefficient))
		{
			allocationFailed = true;
			return;
		}
	}
	if (coefficients[order + 1] == 0.0)
	{
		allocationFailed = true;
		return;
	}

	a = static_cast<double*>(MemoryHelper::allocArray(order, sizeof(double)));
	b = static_cast<double*>(MemoryHelper::allocArray(order, sizeof(double)));
	if (a == NULL || b == NULL)
	{
		MemoryHelper::free(a);
		MemoryHelper::free(b);
		a = NULL;
		b = NULL;
		allocationFailed = true;
		return;
	}

	double a0 = coefficients[order + 1];
	b0 = coefficients[0] / a0;
	if (!std::isfinite(b0))
	{
		MemoryHelper::free(a);
		MemoryHelper::free(b);
		a = NULL;
		b = NULL;
		allocationFailed = true;
		return;
	}
	for (unsigned i = 0; i < order; i++)
	{
		b[i] = coefficients[i + 1] / a0;
		a[i] = -coefficients[i + order + 2] / a0;
		if (!std::isfinite(a[i]) || !std::isfinite(b[i]))
		{
			MemoryHelper::free(a);
			MemoryHelper::free(b);
			a = NULL;
			b = NULL;
			allocationFailed = true;
			return;
		}
	}
}

IIRFilter::~IIRFilter()
{
	MemoryHelper::free(a);
	MemoryHelper::free(b);

	if (x != NULL)
		MemoryHelper::free(x);
	if (y != NULL)
		MemoryHelper::free(y);
}

vector<wstring> IIRFilter::initialize(float sampleRate, unsigned maxFrameCount, vector<wstring> channelNames)
{
	stateReady = false;
	if (x != NULL)
	{
		MemoryHelper::free(x);
		x = NULL;
	}
	if (y != NULL)
	{
		MemoryHelper::free(y);
		y = NULL;
	}
	channelCount = 0;
	if (channelNames.size() > (std::numeric_limits<unsigned>::max)())
		return channelNames;
	channelCount = static_cast<unsigned>(channelNames.size());
	if (allocationFailed || channelCount == 0)
		return channelNames;

	if (channelCount != 0 &&
		order > (std::numeric_limits<size_t>::max)() / channelCount)
	{
		return channelNames;
	}
	const size_t stateCount = static_cast<size_t>(order) * channelCount;
	x = static_cast<double*>(MemoryHelper::allocArray(stateCount, sizeof(double)));
	y = static_cast<double*>(MemoryHelper::allocArray(stateCount, sizeof(double)));
	if (x == NULL || y == NULL)
	{
		MemoryHelper::free(x);
		MemoryHelper::free(y);
		x = NULL;
		y = NULL;
		return channelNames;
	}
	memset(x, 0, stateCount * sizeof(double));
	memset(y, 0, stateCount * sizeof(double));
	stateReady = true;

	return channelNames;
}

#pragma AVRT_CODE_BEGIN
void IIRFilter::process(double** output, double** input, unsigned frameCount)
{
	if (allocationFailed || !stateReady)
	{
		for (unsigned channel = 0; channel < channelCount; ++channel)
		{
			if (output[channel] != input[channel])
				memcpy(output[channel], input[channel], frameCount * sizeof(double));
		}
		return;
	}

	for (unsigned i = 0; i < channelCount; i++)
	{
		double* inputChannel = input[i];
		double* outputChannel = output[i];

		size_t channelOffset = static_cast<size_t>(i) * order;
		double* xo = x + channelOffset;
		double* yo = y + channelOffset;
		for (unsigned j = 0; j < frameCount; j++)
		{
			double sample = inputChannel[j];
			double sum = b0 * sample;

			for (unsigned k = order - 1; k > 0; k--)
			{
				sum += b[k] * xo[k];
				xo[k] = xo[k - 1];
			}

			sum += b[0] * xo[0];

			for (unsigned k = order - 1; k > 0; k--)
			{
				sum += a[k] * yo[k];
				yo[k] = yo[k - 1];
			}

			sum += a[0] * yo[0];
			if (!std::isfinite(sum))
			{
				const size_t stateCount = static_cast<size_t>(channelCount) * order;
				memset(x, 0, stateCount * sizeof(double));
				memset(y, 0, stateCount * sizeof(double));
				stateReady = false;
				for (unsigned dryChannel = i; dryChannel < channelCount; ++dryChannel)
				{
					const unsigned firstFrame = dryChannel == i ? j : 0;
					for (unsigned dryFrame = firstFrame; dryFrame < frameCount; ++dryFrame)
					{
						const double drySample = input[dryChannel][dryFrame];
						output[dryChannel][dryFrame] = std::isfinite(drySample) ?
							drySample : 0.0;
					}
				}
				return;
			}

			xo[0] = sample;
			yo[0] = sum;

			outputChannel[j] = (double)sum;
		}
	}

	const size_t stateCount = static_cast<size_t>(channelCount) * order;
	for (size_t i = 0; i < stateCount; i++)
	{
		if (IS_DENORMAL(x[i]))
			x[i] = 0.0;
		if (IS_DENORMAL(y[i]))
			y[i] = 0.0;
	}
}
#pragma AVRT_CODE_END
