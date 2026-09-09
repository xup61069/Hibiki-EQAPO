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

#pragma once

#include <string>
#include <vector>

#include "helpers/MemoryHelper.h"

struct FilterRuntimeVolumeObservation
{
	std::wstring requestedEndpointId;
	std::wstring resolvedEndpointId;
	double volumeDb = 0.0;
	double volumeScalar = 1.0;
	bool muted = false;
	bool available = false;
};

struct FilterRuntimeContext
{
	bool flowKnown = false;
	bool isCapture = false;
	bool offlineAnalysis = false;
	std::wstring endpointId;
	// Offline analysis owns this optional sink. Automatic loudness filters
	// record the exact Windows endpoint and volume used to build their
	// coefficients so a later destructive editor action can reject stale data.
	std::vector<FilterRuntimeVolumeObservation>* volumeObservations = nullptr;
};

#pragma AVRT_VTABLES_BEGIN
class IFilter
{
public:
	virtual ~IFilter() {}

	// request to get all channel names instead of selection
	virtual bool getAllChannels() {return false;}
	// return false to request that output and input do not point to the same memory locations
	virtual bool getInPlace() {return true;}
	// request that the channelNames returned by initialize become the new selection
	virtual bool getSelectChannels() {return false;}
	// Called before initialize(). Filters that do not depend on device identity
	// can keep the default no-op implementation.
	virtual void setRuntimeContext(const FilterRuntimeContext& context) {(void)context;}
	// return value is the channelNames vector, which may contain additional or fewer channel names
	virtual std::vector<std::wstring> initialize(float sampleRate, unsigned maxFrameCount, std::vector<std::wstring> channelNames) = 0;
	virtual void process(double** output, double** input, unsigned frameCount) = 0;
	// Optional native float kernel. Returning false must leave buffers/state
	// untouched; the configuration then uses its preallocated double bridge.
	virtual bool processSingle(float** output, float** input, unsigned frameCount)
	{
		(void)output; (void)input; (void)frameCount;
		return false;
	}

protected:
};
#pragma AVRT_VTABLES_END
