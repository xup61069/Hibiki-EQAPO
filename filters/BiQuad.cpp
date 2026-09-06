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

#include "stdafx.h"
#include "BiQuad.h"

using namespace std;

bool BiQuad::isConfigurationValid(
	Type type,
	double dbGain,
	double freq,
	double bandwidthOrQOrS,
	bool isBandwidthOrS,
	bool isCornerFreq) noexcept
{
	if (type < LOW_PASS || type > HIGH_SHELF ||
		!std::isfinite(dbGain) || !std::isfinite(freq) || freq <= 0.0 ||
		!std::isfinite(bandwidthOrQOrS) || bandwidthOrQOrS <= 0.0)
	{
		return false;
	}

	const bool gainUsesAmplitude =
		type == PEAKING || type == LOW_SHELF || type == HIGH_SHELF;
	const double amplitude = pow(10.0, dbGain / (gainUsesAmplitude ? 40.0 : 20.0));
	if (!std::isfinite(amplitude) || amplitude <= 0.0)
		return false;

	if (type == LOW_SHELF || type == HIGH_SHELF)
	{
		const double amplitudeTerm = amplitude + 1.0 / amplitude;
		if (!std::isfinite(amplitudeTerm))
			return false;

		if (isBandwidthOrS)
		{
			const double radicand = amplitudeTerm *
				(1.0 / bandwidthOrQOrS - 1.0) + 2.0;
			if (!std::isfinite(radicand) || radicand < 0.0)
				return false;
		}

		if (isCornerFreq)
		{
			double slope = bandwidthOrQOrS;
			if (!isBandwidthOrS)
			{
				const double inverseQ = 1.0 / bandwidthOrQOrS;
				const double denominator =
					(inverseQ * inverseQ - 2.0) / amplitudeTerm + 1.0;
				if (!std::isfinite(denominator) || denominator == 0.0)
					return false;
				slope = 1.0 / denominator;
			}
			if (!std::isfinite(slope) || slope <= 0.0)
				return false;

			const double exponent = abs(dbGain) / 80.0 / slope;
			const double centerFreqFactor = pow(10.0, exponent);
			if (!std::isfinite(exponent) || !std::isfinite(centerFreqFactor) ||
				centerFreqFactor <= 0.0)
			{
				return false;
			}
			const double centerFrequency = type == LOW_SHELF ?
				freq * centerFreqFactor : freq / centerFreqFactor;
			if (!std::isfinite(centerFrequency) || centerFrequency <= 0.0)
				return false;
		}
	}

	return true;
}

BiQuad::BiQuad(Type type, double dbGain, double freq, double srate, double bandwidthOrQOrS, bool isBandwidthOrS)
	: BiQuad()
{
	valid = false;
	if (!isConfigurationValid(
			type, dbGain, freq, bandwidthOrQOrS, isBandwidthOrS, false) ||
		!std::isfinite(srate) || srate <= 0.0 || freq >= srate * 0.5)
	{
		return;
	}

	double A;
	if (type == PEAKING || type == LOW_SHELF || type == HIGH_SHELF)
		A = pow(10, dbGain / 40);
	else
		A = pow(10, dbGain / 20);
	double omega = 2 * M_PI * freq / srate;
	double sn = sin(omega);
	double cs = cos(omega);
	double alpha;

	if (!isBandwidthOrS) // Q
		alpha = sn / (2 * bandwidthOrQOrS);
	else if (type == LOW_SHELF || type == HIGH_SHELF) // S
		alpha = sn / 2 * sqrt((A + 1 / A) * (1 / bandwidthOrQOrS - 1) + 2);
	else // BW
		alpha = sn * sinh(M_LN2 / 2 * bandwidthOrQOrS * omega / sn);

	double beta = 2 * sqrt(A) * alpha;

	// Keep a safe identity response if an invalid enum value ever reaches this
	// constructor. Every supported type below overwrites all six coefficients.
	double b0 = 1.0;
	double b1 = 0.0;
	double b2 = 0.0;
	double a0 = 1.0;
	double a1 = 0.0;
	double a2 = 0.0;

	switch (type)
	{
	case LOW_PASS:
		b0 = (1 - cs) / 2;
		b1 = 1 - cs;
		b2 = (1 - cs) / 2;
		a0 = 1 + alpha;
		a1 = -2 * cs;
		a2 = 1 - alpha;
		break;
	case HIGH_PASS:
		b0 = (1 + cs) / 2;
		b1 = -(1 + cs);
		b2 = (1 + cs) / 2;
		a0 = 1 + alpha;
		a1 = -2 * cs;
		a2 = 1 - alpha;
		break;
	case BAND_PASS:
		b0 = alpha;
		b1 = 0;
		b2 = -alpha;
		a0 = 1 + alpha;
		a1 = -2 * cs;
		a2 = 1 - alpha;
		break;
	case NOTCH:
		b0 = 1;
		b1 = -2 * cs;
		b2 = 1;
		a0 = 1 + alpha;
		a1 = -2 * cs;
		a2 = 1 - alpha;
		break;
	case ALL_PASS:
		b0 = 1 - alpha;
		b1 = -2 * cs;
		b2 = 1 + alpha;
		a0 = 1 + alpha;
		a1 = -2 * cs;
		a2 = 1 - alpha;
		break;
	case PEAKING:
		b0 = 1 + (alpha * A);
		b1 = -2 * cs;
		b2 = 1 - (alpha * A);
		a0 = 1 + (alpha / A);
		a1 = -2 * cs;
		a2 = 1 - (alpha / A);
		break;
	case LOW_SHELF:
		b0 = A * ((A + 1) - (A - 1) * cs + beta);
		b1 = 2 * A * ((A - 1) - (A + 1) * cs);
		b2 = A * ((A + 1) - (A - 1) * cs - beta);
		a0 = (A + 1) + (A - 1) * cs + beta;
		a1 = -2 * ((A - 1) + (A + 1) * cs);
		a2 = (A + 1) + (A - 1) * cs - beta;
		break;
	case HIGH_SHELF:
		b0 = A * ((A + 1) + (A - 1) * cs + beta);
		b1 = -2 * A * ((A - 1) + (A + 1) * cs);
		b2 = A * ((A + 1) + (A - 1) * cs - beta);
		a0 = (A + 1) - (A - 1) * cs + beta;
		a1 = 2 * ((A - 1) - (A + 1) * cs);
		a2 = (A + 1) - (A - 1) * cs - beta;
		break;
	}

	const double normalizedA0 = b0 / a0;
	const double normalizedA1 = b1 / a0;
	const double normalizedA2 = b2 / a0;
	const double normalizedB1 = a1 / a0;
	const double normalizedB2 = a2 / a0;
	const double stabilityMargin = 1.0e-12;
	const bool stable =
		1.0 + normalizedB1 + normalizedB2 > stabilityMargin &&
		1.0 - normalizedB1 + normalizedB2 > stabilityMargin &&
		1.0 - normalizedB2 > stabilityMargin;
	if (std::isfinite(normalizedA0) && std::isfinite(normalizedA1) &&
		std::isfinite(normalizedA2) && std::isfinite(normalizedB1) &&
		std::isfinite(normalizedB2) && stable)
	{
		this->a0 = normalizedA0;
		this->a[0] = normalizedA1;
		this->a[1] = normalizedA2;
		this->a[2] = normalizedB1;
		this->a[3] = normalizedB2;
		valid = true;
	}
}

double BiQuad::gainAt(double freq, double srate)
{
	double omega = 2 * M_PI * freq / srate;
	double sn = sin(omega / 2.0);
	double phi = sn * sn;
	double b0 = this->a0;
	double b1 = this->a[0];
	double b2 = this->a[1];
	double a0 = 1.0;
	double a1 = this->a[2];
	double a2 = this->a[3];

	double dbGain = 10 * log10(pow(b0 + b1 + b2, 2) - 4 * (b0 * b1 + 4 * b0 * b2 + b1 * b2) * phi + 16 * b0 * b2 * phi * phi)
		- 10 * log10(pow(a0 + a1 + a2, 2) - 4 * (a0 * a1 + 4 * a0 * a2 + a1 * a2) * phi + 16 * a0 * a2 * phi * phi);

	return dbGain;
}

void BiQuad::getCoefficients(double(&out_coeffs)[4], double& out_a0) const
{
	out_a0 = this->a0;
	for (int i = 0; i < 4; ++i)
	{
		out_coeffs[i] = this->a[i];
	}
}
