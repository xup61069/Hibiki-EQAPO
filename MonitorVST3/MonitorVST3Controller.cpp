#include "MonitorVST3Controller.h"

#include "MonitorVST3Identity.h"
#include "MonitorVST3State.h"

namespace EqualizerAPO::MonitorVST3
{

Steinberg::tresult PLUGIN_API MonitorVST3Controller::initialize(
	Steinberg::FUnknown* context)
{
	const Steinberg::tresult result = EditController::initialize(context);
	if (result != Steinberg::kResultOk)
		return result;

	parameters.addParameter(
		STR16("Bypass"),
		nullptr,
		1,
		0.0,
		Steinberg::Vst::ParameterInfo::kCanAutomate |
			Steinberg::Vst::ParameterInfo::kIsBypass,
		kMonitorBypassParamId);
	return Steinberg::kResultOk;
}

Steinberg::tresult PLUGIN_API MonitorVST3Controller::setComponentState(
	Steinberg::IBStream* state)
{
	bool bypass = false;
	if (!readMonitorState(state, bypass))
		return Steinberg::kResultFalse;
	return setParamNormalized(kMonitorBypassParamId, bypass ? 1.0 : 0.0);
}

Steinberg::tresult PLUGIN_API MonitorVST3Controller::setState(
	Steinberg::IBStream* state)
{
	return setComponentState(state);
}

Steinberg::tresult PLUGIN_API MonitorVST3Controller::getState(
	Steinberg::IBStream* state)
{
	return writeMonitorState(
		state, getParamNormalized(kMonitorBypassParamId) >= 0.5) ?
		Steinberg::kResultOk : Steinberg::kResultFalse;
}

} // namespace EqualizerAPO::MonitorVST3
