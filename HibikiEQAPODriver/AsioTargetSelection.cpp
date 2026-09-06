#include "AsioTargetSelection.h"

#include <windows.h>

namespace HibikiAsio
{
namespace
{
bool isEqual(const CLSID& left, const CLSID& right) noexcept
{
	return InlineIsEqualGUID(left, right) != FALSE;
}

bool isUsable(const AsioDriverCandidate& candidate) noexcept
{
	return candidate.is64Bit && !candidate.isSamePhysicalFile;
}

TargetSelection findConfigured(
	const CLSID& ownClsid,
	const CLSID& configured,
	const std::vector<AsioDriverCandidate>& candidates) noexcept
{
	if (isEqual(ownClsid, configured))
		return {TargetSelectionStatus::SelfTarget, 0};
	for (std::size_t index = 0; index < candidates.size(); ++index)
	{
		if (isEqual(candidates[index].clsid, configured))
		{
			return isUsable(candidates[index]) ?
				TargetSelection{TargetSelectionStatus::Selected, index} :
				TargetSelection{TargetSelectionStatus::ConfiguredUnavailable, 0};
		}
	}
	return {TargetSelectionStatus::ConfiguredUnavailable, 0};
}
}

TargetSelection selectTarget(
	const CLSID& ownClsid,
	const std::optional<CLSID>& userOverride,
	const std::optional<CLSID>& machineDefault,
	const std::vector<AsioDriverCandidate>& candidates) noexcept
{
	if (userOverride.has_value())
		return findConfigured(ownClsid, *userOverride, candidates);
	if (machineDefault.has_value())
		return findConfigured(ownClsid, *machineDefault, candidates);

	std::size_t selected = 0;
	bool found = false;
	for (std::size_t index = 0; index < candidates.size(); ++index)
	{
		const AsioDriverCandidate& candidate = candidates[index];
		if (!isUsable(candidate) || isEqual(candidate.clsid, ownClsid))
			continue;

		bool duplicate = false;
		for (std::size_t prior = 0; prior < index; ++prior)
		{
			if (isUsable(candidates[prior]) &&
				isEqual(candidates[prior].clsid, candidate.clsid))
			{
				duplicate = true;
				break;
			}
		}
		if (duplicate)
			continue;
		if (found)
			return {TargetSelectionStatus::Ambiguous, 0};
		selected = index;
		found = true;
	}
	return found ? TargetSelection{TargetSelectionStatus::Selected, selected} :
		TargetSelection{TargetSelectionStatus::Missing, 0};
}
}
