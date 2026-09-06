#include "AsioTargetStore.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cwctype>
#include <optional>
#include <utility>
#include <vector>

#include <objbase.h>
#include <shlwapi.h>

#include "AsioProxyIdentity.h"

namespace HibikiAsio
{
namespace
{
constexpr DWORD kMaximumRegistryStringBytes = 64U * 1024U;

struct RegistryClsid
{
	bool present = false;
	bool valid = false;
	CLSID value{};
};

enum class RegistryStringStatus
{
	Missing,
	Invalid,
	Value
};

struct RegistryString
{
	RegistryStringStatus status = RegistryStringStatus::Missing;
	std::wstring value;
};

RegistryString readRegistryString(
	HKEY root,
	const std::wstring& subkey,
	const wchar_t* valueName,
	bool allowExpand = true);
RegistryClsid readConfiguredClsid(HKEY root);
std::vector<AsioDriverCandidate> enumerateCandidates(
	const std::wstring& proxyPath);
std::wstring modulePath(HMODULE module);
std::optional<std::wstring> inprocPathFor(
	HKEY root, const std::wstring& prefix, const CLSID& clsid);

ResolvedAsioTarget resolveWithRegistry(HMODULE proxyModule)
{
	// Per-user selection is the only override. The installer writes the same
	// value under HKLM as the 64-bit machine default for UAC-safe setup.
	const RegistryClsid user = readConfiguredClsid(HKEY_CURRENT_USER);
	RegistryClsid machine;
	if (!user.present)
		machine = readConfiguredClsid(HKEY_LOCAL_MACHINE);
	if ((user.present && !user.valid) ||
		(!user.present && machine.present && !machine.valid))
	{
		return {TargetSelectionStatus::ConfiguredUnavailable, {}, {}, {}};
	}

	const std::wstring proxyPath = modulePath(proxyModule);
	std::vector<AsioDriverCandidate> candidates = enumerateCandidates(proxyPath);
	const std::optional<CLSID> userOverride =
		user.present ? std::optional<CLSID>(user.value) : std::nullopt;
	const std::optional<CLSID> machineDefault =
		machine.present ? std::optional<CLSID>(machine.value) : std::nullopt;
	const TargetSelection selection = selectTarget(
		kDriverClsid, userOverride, machineDefault, candidates);
	if (selection.status != TargetSelectionStatus::Selected ||
		selection.candidateIndex >= candidates.size())
	{
		return {selection.status, {}, {}, {}};
	}
	const AsioDriverCandidate& selected = candidates[selection.candidateIndex];
	// CoCreateInstance resolves the merged HKCR view. A per-user Classes
	// override must not redirect the already-validated HKLM ASIO registration to
	// another DLL between discovery and activation.
	const std::optional<std::wstring> effectivePath = inprocPathFor(
		HKEY_CLASSES_ROOT, L"CLSID\\", selected.clsid);
	if (!effectivePath.has_value() ||
		!samePhysicalFile(selected.dllPath, *effectivePath))
	{
		return {TargetSelectionStatus::ConfiguredUnavailable, {}, {}, {}};
	}
	return {selection.status, selected.clsid, selected.name, selected.dllPath};
}

RegistryString readRegistryString(
	HKEY root,
	const std::wstring& subkey,
	const wchar_t* valueName,
	bool allowExpand)
{
	HKEY key = nullptr;
	const LONG openResult = RegOpenKeyExW(
		root,
		subkey.c_str(),
		0,
		KEY_QUERY_VALUE | KEY_WOW64_64KEY,
		&key);
	if (openResult != ERROR_SUCCESS)
	{
		return {openResult == ERROR_FILE_NOT_FOUND ||
			openResult == ERROR_PATH_NOT_FOUND ? RegistryStringStatus::Missing :
			RegistryStringStatus::Invalid, {}};
	}

	DWORD type = 0;
	DWORD byteCount = 0;
	LONG result = RegQueryValueExW(
		key, valueName, nullptr, &type, nullptr, &byteCount);
	if (result == ERROR_FILE_NOT_FOUND)
	{
		RegCloseKey(key);
		return {};
	}
	if (result != ERROR_SUCCESS ||
		(type != REG_SZ && !(allowExpand && type == REG_EXPAND_SZ)) ||
		byteCount < sizeof(wchar_t) ||
		byteCount > kMaximumRegistryStringBytes ||
		(byteCount % sizeof(wchar_t)) != 0)
	{
		RegCloseKey(key);
		return {RegistryStringStatus::Invalid, {}};
	}

	std::vector<wchar_t> buffer(byteCount / sizeof(wchar_t) + 1, L'\0');
	result = RegQueryValueExW(
		key,
		valueName,
		nullptr,
		&type,
		reinterpret_cast<BYTE*>(buffer.data()),
		&byteCount);
	RegCloseKey(key);
	if (result != ERROR_SUCCESS ||
		(type != REG_SZ && !(allowExpand && type == REG_EXPAND_SZ)) ||
		byteCount < sizeof(wchar_t) ||
		byteCount > kMaximumRegistryStringBytes ||
		(byteCount % sizeof(wchar_t)) != 0)
	{
		return {RegistryStringStatus::Invalid, {}};
	}
	buffer.back() = L'\0';
	std::wstring value(buffer.data());

	if (type == REG_EXPAND_SZ)
	{
		const DWORD required = ExpandEnvironmentStringsW(value.c_str(), nullptr, 0);
		if (required == 0 || required > 32768)
			return {RegistryStringStatus::Invalid, {}};
		std::vector<wchar_t> expanded(required, L'\0');
		if (ExpandEnvironmentStringsW(value.c_str(), expanded.data(), required) != required)
			return {RegistryStringStatus::Invalid, {}};
		value.assign(expanded.data());
	}
	return {RegistryStringStatus::Value, std::move(value)};
}

RegistryClsid readConfiguredClsid(HKEY root)
{
	const RegistryString text = readRegistryString(
		root, kTargetRegistryKey, kTargetClsidValue, false);
	if (text.status == RegistryStringStatus::Missing)
		return {};
	RegistryClsid result;
	result.present = true;
	result.valid = text.status == RegistryStringStatus::Value &&
		CLSIDFromString(text.value.c_str(), &result.value) == S_OK;
	return result;
}

std::wstring trimAndUnquote(std::wstring value)
{
	auto isSpace = [](wchar_t character) { return std::iswspace(character) != 0; };
	value.erase(value.begin(), std::find_if_not(value.begin(), value.end(), isSpace));
	value.erase(std::find_if_not(value.rbegin(), value.rend(), isSpace).base(), value.end());
	if (value.size() >= 2 && value.front() == L'"' && value.back() == L'"')
		value = value.substr(1, value.size() - 2);
	return value;
}

std::optional<std::wstring> inprocPathFor(
	HKEY root,
	const std::wstring& prefix,
	const CLSID& clsid)
{
	std::array<wchar_t, 40> clsidText{};
	if (StringFromGUID2(clsid, clsidText.data(),
		static_cast<int>(clsidText.size())) == 0)
	{
		return std::nullopt;
	}
	const std::wstring key = prefix +
		std::wstring(clsidText.data()) + L"\\InprocServer32";
	RegistryString path = readRegistryString(root, key, nullptr);
	if (path.status != RegistryStringStatus::Value)
		return std::nullopt;
	path.value = trimAndUnquote(std::move(path.value));
	if (path.value.empty() || PathIsRelativeW(path.value.c_str()) != FALSE)
		return std::nullopt;
	return path.value;
}

std::vector<AsioDriverCandidate> enumerateCandidates(
	const std::wstring& proxyPath)
{
	std::vector<AsioDriverCandidate> candidates;
	HKEY asioKey = nullptr;
	if (RegOpenKeyExW(
		HKEY_LOCAL_MACHINE,
		L"SOFTWARE\\ASIO",
		0,
		KEY_ENUMERATE_SUB_KEYS | KEY_QUERY_VALUE | KEY_WOW64_64KEY,
		&asioKey) != ERROR_SUCCESS)
	{
		return candidates;
	}

	for (DWORD index = 0;; ++index)
	{
		std::array<wchar_t, 256> keyName{};
		DWORD keyNameLength = static_cast<DWORD>(keyName.size());
		const LONG enumResult = RegEnumKeyExW(
			asioKey,
			index,
			keyName.data(),
			&keyNameLength,
			nullptr,
			nullptr,
			nullptr,
			nullptr);
		if (enumResult == ERROR_NO_MORE_ITEMS)
			break;
		if (enumResult != ERROR_SUCCESS)
			continue;

		const std::wstring asioSubkey =
			L"SOFTWARE\\ASIO\\" + std::wstring(keyName.data(), keyNameLength);
		const RegistryString clsidText =
			readRegistryString(HKEY_LOCAL_MACHINE, asioSubkey, L"CLSID");
		if (clsidText.status != RegistryStringStatus::Value)
			continue;
		CLSID clsid{};
		if (CLSIDFromString(clsidText.value.c_str(), &clsid) != S_OK)
			continue;
		const std::optional<std::wstring> dllPath = inprocPathFor(
			HKEY_LOCAL_MACHINE, L"Software\\Classes\\CLSID\\", clsid);
		if (!dllPath.has_value())
			continue;
		RegistryString description =
			readRegistryString(HKEY_LOCAL_MACHINE, asioSubkey, L"Description");
		candidates.push_back({
			clsid,
			description.status == RegistryStringStatus::Value ?
				description.value : std::wstring(keyName.data(), keyNameLength),
			*dllPath,
			is64BitDll(*dllPath),
			!proxyPath.empty() && samePhysicalFile(proxyPath, *dllPath)
		});
	}
	RegCloseKey(asioKey);
	return candidates;
}

std::wstring modulePath(HMODULE module)
{
	if (module == nullptr)
		return {};
	std::vector<wchar_t> buffer(512, L'\0');
	while (buffer.size() <= 32768)
	{
		const DWORD length = GetModuleFileNameW(
			module, buffer.data(), static_cast<DWORD>(buffer.size()));
		if (length == 0)
			return {};
		if (length < buffer.size() - 1)
			return std::wstring(buffer.data(), length);
		buffer.resize(buffer.size() * 2, L'\0');
	}
	return {};
}

bool readExact(HANDLE file, void* destination, DWORD byteCount) noexcept
{
	DWORD bytesRead = 0;
	return ReadFile(file, destination, byteCount, &bytesRead, nullptr) != FALSE &&
		bytesRead == byteCount;
}
}

bool is64BitDll(const std::wstring& path) noexcept
{
	HANDLE file = CreateFileW(
		path.c_str(),
		GENERIC_READ,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		nullptr,
		OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL,
		nullptr);
	if (file == INVALID_HANDLE_VALUE)
		return false;

	LARGE_INTEGER fileSize{};
	IMAGE_DOS_HEADER dosHeader{};
	bool valid = GetFileSizeEx(file, &fileSize) != FALSE &&
		readExact(file, &dosHeader, sizeof(dosHeader)) &&
		dosHeader.e_magic == IMAGE_DOS_SIGNATURE &&
		dosHeader.e_lfanew > 0 &&
		static_cast<LONGLONG>(dosHeader.e_lfanew) <=
			fileSize.QuadPart - static_cast<LONGLONG>(
				sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER));
	if (valid)
	{
		LARGE_INTEGER offset{};
		offset.QuadPart = dosHeader.e_lfanew;
		DWORD signature = 0;
		IMAGE_FILE_HEADER fileHeader{};
		IMAGE_OPTIONAL_HEADER64 optionalHeader{};
		valid = SetFilePointerEx(file, offset, nullptr, FILE_BEGIN) != FALSE &&
			readExact(file, &signature, sizeof(signature)) &&
			readExact(file, &fileHeader, sizeof(fileHeader)) &&
			signature == IMAGE_NT_SIGNATURE &&
			fileHeader.Machine == IMAGE_FILE_MACHINE_AMD64 &&
			(fileHeader.Characteristics & IMAGE_FILE_DLL) != 0 &&
			fileHeader.NumberOfSections != 0 &&
			fileHeader.SizeOfOptionalHeader >= sizeof(optionalHeader);
		if (valid)
		{
			const std::uint64_t headerEnd =
				static_cast<std::uint64_t>(dosHeader.e_lfanew) +
				sizeof(signature) + sizeof(fileHeader) +
				fileHeader.SizeOfOptionalHeader +
				static_cast<std::uint64_t>(fileHeader.NumberOfSections) *
					sizeof(IMAGE_SECTION_HEADER);
			valid = headerEnd <= static_cast<std::uint64_t>(fileSize.QuadPart) &&
				readExact(file, &optionalHeader, sizeof(optionalHeader)) &&
				optionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC &&
				static_cast<std::uint64_t>(optionalHeader.SizeOfHeaders) >=
					headerEnd &&
				optionalHeader.SizeOfHeaders <=
					static_cast<std::uint64_t>(fileSize.QuadPart);
		}
	}
	CloseHandle(file);
	return valid;
}

bool samePhysicalFile(
	const std::wstring& left,
	const std::wstring& right) noexcept
{
	HANDLE leftFile = CreateFileW(
		left.c_str(), FILE_READ_ATTRIBUTES,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (leftFile == INVALID_HANDLE_VALUE)
		return false;
	HANDLE rightFile = CreateFileW(
		right.c_str(), FILE_READ_ATTRIBUTES,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (rightFile == INVALID_HANDLE_VALUE)
	{
		CloseHandle(leftFile);
		return false;
	}
	BY_HANDLE_FILE_INFORMATION leftInfo{};
	BY_HANDLE_FILE_INFORMATION rightInfo{};
	const bool same = GetFileInformationByHandle(leftFile, &leftInfo) != FALSE &&
		GetFileInformationByHandle(rightFile, &rightInfo) != FALSE &&
		leftInfo.dwVolumeSerialNumber == rightInfo.dwVolumeSerialNumber &&
		leftInfo.nFileIndexHigh == rightInfo.nFileIndexHigh &&
		leftInfo.nFileIndexLow == rightInfo.nFileIndexLow;
	CloseHandle(rightFile);
	CloseHandle(leftFile);
	return same;
}

ResolvedAsioTarget resolveAsioTarget(HMODULE proxyModule)
{
	return resolveWithRegistry(proxyModule);
}
}
