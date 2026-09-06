#pragma once

#include <cstdint>

#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/vst/vsttypes.h"

namespace EqualizerAPO::MonitorVST3
{

inline const Steinberg::FUID kMonitorProcessorUid(
	0x7C8A3D91, 0x0E6F4B27, 0xB1D8A45C, 0x92F36710);
inline const Steinberg::FUID kMonitorControllerUid(
	0x3F2B86E4, 0xC95047AD, 0xA67E19D2, 0x548CB301);

constexpr Steinberg::Vst::ParamID kMonitorBypassParamId = 0x00010000u;

inline constexpr wchar_t kMonitorDeviceName[] = L"Hibiki EQAPO Monitor VST3";
inline constexpr wchar_t kMonitorConnectionName[] = L"DAW Monitor Insert";
inline constexpr wchar_t kMonitorDeviceGuid[] = L"";
inline constexpr wchar_t kMonitorDeviceString[] =
	L"DAW Monitor Insert Hibiki EQAPO Monitor VST3";

inline constexpr Steinberg::int32 kDefaultChannelCount = 2;
inline constexpr Steinberg::int32 kMaximumChannelCount = 18;
inline constexpr Steinberg::int32 kMaximumParameterQueuesPerBlock = 64;

enum class MonitorProcessStatus : std::uint32_t
{
	Uninitialized = 0,
	Ready,
	Inactive,
	OfflineDry,
	Bypassed,
	OversizedBlock,
	InvalidBus,
	UnsupportedSampleSize,
	EngineFailure
};

} // namespace EqualizerAPO::MonitorVST3
