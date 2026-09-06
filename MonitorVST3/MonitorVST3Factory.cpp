#include "MonitorVST3Controller.h"
#include "MonitorVST3Identity.h"
#include "MonitorVST3Processor.h"

#include "public.sdk/source/main/pluginfactory.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "../version.h"

using namespace EqualizerAPO::MonitorVST3;

#define MONITOR_VST3_NAME "Hibiki EQAPO Monitor"
#define MONITOR_VST3_STRINGIFY_INNER(value) #value
#define MONITOR_VST3_STRINGIFY(value) MONITOR_VST3_STRINGIFY_INNER(value)
#define MONITOR_VST3_VERSION \
	MONITOR_VST3_STRINGIFY(MAJOR) "." \
	MONITOR_VST3_STRINGIFY(MINOR) "." \
	MONITOR_VST3_STRINGIFY(REVISION)

BEGIN_FACTORY_DEF(
	"Hibiki EQAPO",
	"https://github.com/xup61069/loudness-correction-apo",
	"")

	DEF_CLASS2(
		INLINE_UID_FROM_FUID(kMonitorProcessorUid),
		Steinberg::PClassInfo::kManyInstances,
		kVstAudioEffectClass,
		MONITOR_VST3_NAME,
		0,
		Steinberg::Vst::PlugType::kFx,
		MONITOR_VST3_VERSION,
		kVstVersionString,
		MonitorVST3Processor::createInstance)

	DEF_CLASS2(
		INLINE_UID_FROM_FUID(kMonitorControllerUid),
		Steinberg::PClassInfo::kManyInstances,
		kVstComponentControllerClass,
		MONITOR_VST3_NAME " Controller",
		0,
		"",
		MONITOR_VST3_VERSION,
		kVstVersionString,
		MonitorVST3Controller::createInstance)

END_FACTORY
