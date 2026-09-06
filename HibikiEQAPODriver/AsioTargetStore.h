#pragma once

#include <string>

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

// These helpers perform I/O only during IASIO::init. They are deliberately
// separate from selectTarget(), whose precedence and ambiguity rules are pure
// and covered by the native unit tests.
bool is64BitDll(const std::wstring& path) noexcept;
bool samePhysicalFile(
	const std::wstring& left,
	const std::wstring& right) noexcept;
ResolvedAsioTarget resolveAsioTarget(HMODULE proxyModule);
}
