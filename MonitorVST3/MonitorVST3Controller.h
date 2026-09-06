#pragma once

#include "public.sdk/source/vst/vsteditcontroller.h"

namespace EqualizerAPO::MonitorVST3
{

class MonitorVST3Controller final : public Steinberg::Vst::EditController
{
public:
	static Steinberg::FUnknown* createInstance(void*)
	{
		return static_cast<Steinberg::Vst::IEditController*>(
			new MonitorVST3Controller());
	}

	Steinberg::tresult PLUGIN_API initialize(Steinberg::FUnknown* context) override;
	Steinberg::tresult PLUGIN_API setComponentState(Steinberg::IBStream* state) override;
	Steinberg::tresult PLUGIN_API setState(Steinberg::IBStream* state) override;
	Steinberg::tresult PLUGIN_API getState(Steinberg::IBStream* state) override;
};

} // namespace EqualizerAPO::MonitorVST3
