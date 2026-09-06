#pragma once

#include <cstdint>

#include "pluginterfaces/base/ibstream.h"

namespace EqualizerAPO::MonitorVST3
{

namespace StateDetail
{

inline constexpr std::uint32_t kMagic = 0x334D4145u; // EAM3, little-endian.
inline constexpr std::uint32_t kVersion = 1u;
inline constexpr Steinberg::int32 kStateSize = 12;

inline void encode32(std::uint8_t* destination, std::uint32_t value) noexcept
{
	destination[0] = static_cast<std::uint8_t>(value);
	destination[1] = static_cast<std::uint8_t>(value >> 8);
	destination[2] = static_cast<std::uint8_t>(value >> 16);
	destination[3] = static_cast<std::uint8_t>(value >> 24);
}

inline std::uint32_t decode32(const std::uint8_t* source) noexcept
{
	return static_cast<std::uint32_t>(source[0]) |
		(static_cast<std::uint32_t>(source[1]) << 8) |
		(static_cast<std::uint32_t>(source[2]) << 16) |
		(static_cast<std::uint32_t>(source[3]) << 24);
}

} // namespace StateDetail

inline bool writeMonitorState(Steinberg::IBStream* stream, bool bypass) noexcept
{
	if (stream == nullptr)
		return false;

	std::uint8_t bytes[StateDetail::kStateSize] = {};
	StateDetail::encode32(bytes, StateDetail::kMagic);
	StateDetail::encode32(bytes + 4, StateDetail::kVersion);
	StateDetail::encode32(bytes + 8, bypass ? 1u : 0u);

	Steinberg::int32 written = 0;
	return stream->write(bytes, StateDetail::kStateSize, &written) == Steinberg::kResultOk &&
		written == StateDetail::kStateSize;
}

inline bool readMonitorState(Steinberg::IBStream* stream, bool& bypass) noexcept
{
	if (stream == nullptr)
		return false;

	std::uint8_t bytes[StateDetail::kStateSize] = {};
	Steinberg::int32 read = 0;
	if (stream->read(bytes, StateDetail::kStateSize, &read) != Steinberg::kResultOk ||
		read != StateDetail::kStateSize)
	{
		return false;
	}

	if (StateDetail::decode32(bytes) != StateDetail::kMagic ||
		StateDetail::decode32(bytes + 4) != StateDetail::kVersion)
	{
		return false;
	}

	bypass = StateDetail::decode32(bytes + 8) != 0;
	return true;
}

} // namespace EqualizerAPO::MonitorVST3
