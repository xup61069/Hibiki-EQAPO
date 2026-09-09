/*
    This file is part of EqualizerAPO, a system-wide equalizer.
    Copyright (C) 2013  Jonas Thedering

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

#pragma once

#define _USE_MATH_DEFINES
#include <cmath>
#include <climits>
#include <string>

#define IS_DENORMAL(d) (abs(d) < DBL_MIN)

class BiQuad
{
public:
	enum Type
	{
		LOW_PASS, HIGH_PASS, BAND_PASS, NOTCH, ALL_PASS, PEAKING, LOW_SHELF, HIGH_SHELF
	};

	BiQuad()
		: a{ 0.0, 0.0, 0.0, 0.0 },
		  a0(1.0),
		  x1(0.0),
		  x2(0.0),
		  y1(0.0),
		  y2(0.0),
		  valid(true)
	{
	}
	BiQuad(Type type, double dbGain, double freq, double srate, double bandwidthOrQOrS, bool isBandwidthOrS);
	static bool isConfigurationValid(
		Type type,
		double dbGain,
		double freq,
		double bandwidthOrQOrS,
		bool isBandwidthOrS,
		bool isCornerFreq) noexcept;

	__forceinline
	void removeDenormals()
	{
		if (IS_DENORMAL(x1))
			x1 = 0.0;
		if (IS_DENORMAL(x2))
			x2 = 0.0;
		if (IS_DENORMAL(y1))
			y1 = 0.0;
		if (IS_DENORMAL(y2))
			y2 = 0.0;
	}

	__forceinline
	void resetState()
	{
		x1 = 0.0;
		x2 = 0.0;
		y1 = 0.0;
		y2 = 0.0;
	}

	__forceinline
	double process(double sample)
	{
		// changed order of additions leads to better pipelining
		double result = a0 * sample + a[1] * x2 + a[0] * x1 - a[3] * y2 - a[2] * y1;

		x2 = x1;
		x1 = sample;

		y2 = y1;
		y1 = result;

		return result;
	}

	__forceinline
	void processBlock(double* samples, unsigned sampleCount)
	{
		// Keeping one section's coefficients and history in locals avoids
		// reloading a different BiQuad object for every sample in a cascade.
		// The recurrence and operation order are identical to process().
		const double b0 = a0;
		const double b1 = a[0];
		const double b2 = a[1];
		const double a1 = a[2];
		const double a2 = a[3];
		double previousX1 = x1;
		double previousX2 = x2;
		double previousY1 = y1;
		double previousY2 = y2;
		for (unsigned index = 0; index < sampleCount; ++index)
		{
			const double sample = samples[index];
			const double result = b0 * sample + b2 * previousX2 +
				b1 * previousX1 - a2 * previousY2 - a1 * previousY1;
			previousX2 = previousX1;
			previousX1 = sample;
			previousY2 = previousY1;
			previousY1 = result;
			samples[index] = result;
		}
		x1 = previousX1;
		x2 = previousX2;
		y1 = previousY1;
		y2 = previousY2;
	}

	__forceinline
	void setCoefficients(double ain[], const double& a0in)
	{
		for (int i = 0; i < 4; i++)
			a[i] = ain[i];
		a0 = a0in;
	}

	double gainAt(double freq, double srate);
	void getCoefficients(double(&out_coeffs)[4], double& out_a0) const;
	bool isValid() const noexcept { return valid; }
	__forceinline
	bool isStateFinite() const noexcept
	{
		return std::isfinite(x1) && std::isfinite(x2) &&
			std::isfinite(y1) && std::isfinite(y2);
	}

private:
	__declspec(align(16)) double a[4];
	double a0;

	double x1, x2;
	double y1, y2;
	bool valid;
};
