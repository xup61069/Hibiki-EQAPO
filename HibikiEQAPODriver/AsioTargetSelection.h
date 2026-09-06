#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include <guiddef.h>

namespace HibikiAsio
{
struct AsioDriverCandidate
{
	CLSID clsid{};
	std::wstring name;
	std::wstring dllPath;
	bool is64Bit = false;
	bool isSamePhysicalFile = false;
};

enum class TargetSelectionStatus
{
	Selected,
	Missing,
	Ambiguous,
	ConfiguredUnavailable,
	SelfTarget
};

struct TargetSelection
{
	TargetSelectionStatus status = TargetSelectionStatus::Missing;
	std::size_t candidateIndex = 0;
};

TargetSelection selectTarget(
	const CLSID& ownClsid,
	const std::optional<CLSID>& userOverride,
	const std::optional<CLSID>& machineDefault,
	const std::vector<AsioDriverCandidate>& candidates) noexcept;
}
