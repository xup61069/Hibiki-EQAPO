#pragma once

#include <string>
#include <vector>

#include <windows.h>

#include "AsioTargetSelection.h"

namespace HibikiAsio
{
struct ResolvedAsioTarget
{
	TargetSelectionStatus status = TargetSelectionStatus::Missing;
	CLSID clsid{};
	std::wstring name;
	std::wstring dllPath;

	bool isSelected() const noexcept
	{
		return status == TargetSelectionStatus::Selected;
	}
};

struct ConfiguredAsioTarget
{
	bool present = false;
	bool valid = false;
	CLSID clsid{};
};

struct AsioTargetControlState
{
	bool proxyInstalled = false;
	ConfiguredAsioTarget userOverride;
	ConfiguredAsioTarget machineDefault;
	TargetSelectionStatus status = TargetSelectionStatus::Missing;
	CLSID selectedClsid{};
	std::vector<AsioDriverCandidate> candidates;
};

enum class AsioTargetUpdateStatus
{
	Updated,
	ProxyUnavailable,
	InvalidTarget,
	RegistryError,
	VerificationFailed
};

// These helpers perform I/O only during IASIO::init. They are deliberately
// separate from selectTarget(), whose precedence and ambiguity rules are pure
// and covered by the native unit tests.
bool is64BitDll(const std::wstring& path) noexcept;
bool samePhysicalFile(
	const std::wstring& left,
	const std::wstring& right) noexcept;
ResolvedAsioTarget resolveAsioTarget(HMODULE proxyModule);

// Editor control-plane helpers. Discovery validates the installed proxy and
// every candidate without loading vendor DLLs. Updates affect only the current
// user's target override; the proxy revalidates it when a DAW next initializes
// the driver.
AsioTargetControlState inspectAsioTargets(const std::wstring& proxyPath);
AsioTargetUpdateStatus setUserAsioTarget(
	const std::wstring& proxyPath,
	const CLSID& target,
	LONG* win32Error = nullptr);
AsioTargetUpdateStatus clearUserAsioTarget(
	const std::wstring& proxyPath,
	LONG* win32Error = nullptr);
}
