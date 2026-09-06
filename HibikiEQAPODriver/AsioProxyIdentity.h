#pragma once

#include <guiddef.h>

namespace HibikiAsio
{
// This identity is part of the public ASIO/COM ABI. Never regenerate it during
// a product rename or upgrade.
inline constexpr CLSID kDriverClsid = {
	0xD47C55C9, 0x3F7D, 0x422F,
	{0x86, 0xE9, 0x32, 0xE1, 0x70, 0x81, 0x5D, 0x53}
};
inline constexpr wchar_t kDriverClsidString[] =
	L"{D47C55C9-3F7D-422F-86E9-32E170815D53}";

inline constexpr char kDriverDisplayName[] = "Hibiki EQAPO";
inline constexpr wchar_t kDriverDisplayNameWide[] = L"Hibiki EQAPO";
inline constexpr wchar_t kDriverDllName[] = L"HibikiEQAPODriver.dll";
inline constexpr wchar_t kAsioRegistryKey[] = L"SOFTWARE\\ASIO\\Hibiki EQAPO";
inline constexpr wchar_t kTargetRegistryKey[] =
	L"Software\\EqualizerAPO\\ASIOProxy";
inline constexpr wchar_t kTargetClsidValue[] = L"TargetCLSID";
}
