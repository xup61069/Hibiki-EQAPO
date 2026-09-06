/*
    This file is part of Equalizer APO, a system-wide equalizer.
    Copyright (C) 2017  Jonas Thedering

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

#include <array>
#include <atomic>
#include <string>
#include <memory>
#include <functional>
#include <vector>
#include "aeffectx.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivsthostapplication.h"
#include "pluginterfaces/vst/ivstmessage.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "pluginterfaces/gui/iplugview.h"

class VSTPluginLibrary;

namespace Steinberg
{
	namespace Vst
	{
		class ParameterChanges;
	}
}

enum class VSTParameterApi
{
	VST2,
	VST3
};

struct VSTParameterDescriptor
{
	VSTParameterApi api = VSTParameterApi::VST2;
	// Stable VST3 ParamID or stable VST2 parameter index.
	std::uint32_t stableId = 0;
	std::wstring name;
	std::uint32_t stepCount = 0;
	double normalizedValue = 0.0;
	bool readOnly = false;
	bool hidden = false;
};

class VSTPluginInstance
{
public:
	VSTPluginInstance(const std::shared_ptr<VSTPluginLibrary>& library, int processLevel, int vst3ClassIndex = 0);
	~VSTPluginInstance();

	bool initialize();

	int numInputs() const;
	int numOutputs() const;
	bool canReplacing() const;
	int uniqueID() const;
	std::wstring getName() const;
	int getUsedChannelCount() const;
	void setUsedChannelCount(int count);
	float getSampleRate() const;
	int getProcessLevel() const;
	void setProcessLevel(int value);
	int getLanguage() const;
	void setLanguage(int value);
	vst_time_info* getVST2TimeInfo();
	bool canDoubleReplacing() const;
	int getInitialDelay() const;

	void prepareForProcessing(float sampleRate, int blockSize);
	void writeToEffect(const std::wstring& chunkData, const std::unordered_map<std::wstring, float>& paramMap);
	void readFromEffect(std::wstring& chunkData, std::unordered_map<std::wstring, float>& paramMap) const;
	const std::vector<VSTParameterDescriptor>& getParameterDescriptors() const;
	bool setParameterNormalized(const VSTParameterDescriptor& parameter, double value, bool realtimeThread = false);

	void startProcessing();
	void processDoubleReplacing(double** inputArray, double** outputArray, int frameCount);
	void processReplacing(float** inputArray, float** outputArray, int frameCount);
	void process(float** inputArray, float** outputArray, int frameCount);
	void stopProcessing();

	bool startEditing(HWND hWnd, short* width, short* height);
	void doIdle();
	void stopEditing();

	void setAutomateFunc(std::function<void()> func);
	void setParameterAutomateFunc(std::function<void(const VSTParameterDescriptor&, double)> func);
	void onAutomate();
	void onAutomate(Steinberg::Vst::ParamID id, Steinberg::Vst::ParamValue value);
	void onAutomate(int index, float value);

	void setSizeWindowFunc(std::function<void(int, int)> func);
	void onSizeWindow(int w, int h);
	bool getEditorSize(int* width, int* height);

private:
	class VST3HostContext;
	class VST3MemoryStream;
	struct VST3ParameterChange
	{
		Steinberg::Vst::ParamID id = Steinberg::Vst::kNoParamId;
		Steinberg::Vst::ParamValue value = 0.0;
	};
	class VST3ParameterChangeQueue
	{
	public:
		bool tryPush(const VST3ParameterChange& change);
		bool tryPop(VST3ParameterChange& change);
		void clear();

	private:
		static constexpr std::uint32_t capacity = 1024;
		static_assert((capacity & (capacity - 1)) == 0, "VST3 change queue capacity must be a power of two");
		static_assert(std::atomic<std::uint32_t>::is_always_lock_free, "VST3 change queue requires lock-free atomics");
		std::array<VST3ParameterChange, capacity> entries = {};
		std::atomic<std::uint32_t> readIndex{ 0 };
		std::atomic<std::uint32_t> writeIndex{ 0 };
	};

	bool initializeVST2();
	bool initializeVST3();
	void releaseVST3();
	void configureVST3Buses(int requestedChannelCount);
	Steinberg::Vst::SpeakerArrangement speakerArrangementForChannelCount(int count) const;
	void queueVST3ParameterChange(Steinberg::Vst::ParamID id, Steinberg::Vst::ParamValue value);
	void prewarmVST3InputParameterChanges();
	void prepareVST3InputParameterChanges();
	void rebuildParameterDescriptors();
	const VSTParameterDescriptor* findVST3Parameter(Steinberg::Vst::ParamID id) const;
	const VSTParameterDescriptor* findVST2Parameter(int index) const;

	std::shared_ptr<VSTPluginLibrary> library;
	vst_effect_t* effect = NULL;
	Steinberg::Vst::IComponent* vst3Component = NULL;
	Steinberg::Vst::IAudioProcessor* vst3Processor = NULL;
	Steinberg::Vst::IEditController* vst3Controller = NULL;
	Steinberg::Vst::IConnectionPoint* vst3ComponentConnection = NULL;
	Steinberg::Vst::IConnectionPoint* vst3ControllerConnection = NULL;
	Steinberg::IPlugView* vst3View = NULL;
	HWND vst3EditorHostWindow = NULL;
	VST3HostContext* vst3HostContext = NULL;
	int vst3InputBusCount = 0;
	int vst3OutputBusCount = 0;
	int vst3InputChannelCount = 0;
	int vst3OutputChannelCount = 0;
	bool vst3SupportsDouble = false;
	bool vst3Active = false;
	bool vst3Processing = false;
	Steinberg::Vst::ProcessContext vst3ProcessContext = {};
	Steinberg::Vst::TSamples vst3SamplePosition = 0;
	VST3ParameterChangeQueue vst3ParameterChangeQueue;
	std::array<VST3ParameterChange, 256> realtimeVST3ParameterChanges = {};
	std::uint32_t realtimeVST3ParameterChangeCount = 0;
	std::unique_ptr<Steinberg::Vst::ParameterChanges> vst3InputParameterChanges;
	std::vector<VSTParameterDescriptor> parameterDescriptors;
	std::function<void()> automateFunc;
	std::function<void(const VSTParameterDescriptor&, double)> parameterAutomateFunc;
	std::function<void(int, int)> sizeWindowFunc;
	float sampleRate = 0.0f;
	int usedChannelCount = -1;
	int processLevel = 0;
	int language = 1;
	int vst3ClassIndex = 0;
	vst_time_info vstTime{ 0,0,0,0,0,0,0,0,0,0,{0}, 0xFFFF };
};
