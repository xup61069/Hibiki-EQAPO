#include "AsioDsp.h"

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "AsioTargetStore.h"
#include "../FilterEngine.h"
#include "../helpers/ChannelHelper.h"
#include "../helpers/StringHelper.h"

namespace HibikiAsio
{
namespace
{
struct EndpointMatchInfo
{
	std::wstring friendlyName;
	std::wstring interfaceName;
	std::wstring guid;
};

std::wstring readRegistryStringValue(HKEY key, const wchar_t* valueName)
{
	DWORD type = 0;
	DWORD byteCount = 0;
	if (RegQueryValueExW(key, valueName, nullptr, &type, nullptr, &byteCount) != ERROR_SUCCESS ||
		(type != REG_SZ && type != REG_EXPAND_SZ) || byteCount < sizeof(wchar_t))
	{
		return {};
	}
	std::vector<wchar_t> buffer(byteCount / sizeof(wchar_t) + 1, L'\0');
	if (RegQueryValueExW(key, valueName, nullptr, nullptr,
		reinterpret_cast<LPBYTE>(buffer.data()), &byteCount) != ERROR_SUCCESS)
	{
		return {};
	}
	buffer.back() = L'\0';
	return std::wstring(buffer.data());
}

std::vector<std::wstring> extractDistinctiveTokens(const std::wstring& text)
{
	static const std::vector<std::wstring> stopWords = {
		L"asio", L"driver", L"usb", L"audio", L"device", L"x64", L"64",
		L"bit", L"the", L"sound", L"system", L"virtual", L"output", L"input"
	};
	std::vector<std::wstring> tokens;
	std::wstring current;
	for (wchar_t ch : text)
	{
		if (std::iswalnum(ch))
		{
			current += static_cast<wchar_t>(std::towlower(ch));
		}
		else if (!current.empty())
		{
			if (std::find(stopWords.begin(), stopWords.end(), current) == stopWords.end() &&
				current.length() >= 3)
			{
				tokens.push_back(current);
			}
			current.clear();
		}
	}
	if (!current.empty() &&
		std::find(stopWords.begin(), stopWords.end(), current) == stopWords.end() &&
		current.length() >= 3)
	{
		tokens.push_back(current);
	}
	return tokens;
}

std::vector<EndpointMatchInfo> findMatchingEndpoints(const std::wstring& vendorName)
{
	std::vector<EndpointMatchInfo> matched;
	const std::vector<std::wstring> targetTokens = extractDistinctiveTokens(vendorName);
	if (targetTokens.empty())
		return matched;

	HKEY renderKey = nullptr;
	if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
		L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Render",
		0, KEY_READ | KEY_WOW64_64KEY, &renderKey) != ERROR_SUCCESS)
	{
		return matched;
	}

	wchar_t subkeyName[256]{};
	DWORD subkeyIndex = 0;
	DWORD subkeyNameLength = sizeof(subkeyName) / sizeof(subkeyName[0]);

	while (RegEnumKeyExW(renderKey, subkeyIndex++, subkeyName, &subkeyNameLength,
		nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS)
	{
		subkeyNameLength = sizeof(subkeyName) / sizeof(subkeyName[0]);
		const std::wstring endpointGuid = subkeyName;
		if (endpointGuid.empty() || endpointGuid.front() != L'{' || endpointGuid.back() != L'}')
			continue;

		HKEY deviceKey = nullptr;
		if (RegOpenKeyExW(renderKey, subkeyName, 0, KEY_READ | KEY_WOW64_64KEY, &deviceKey) != ERROR_SUCCESS)
			continue;

		DWORD deviceState = 0;
		DWORD stateType = 0;
		DWORD stateBytes = sizeof(deviceState);
		if (RegQueryValueExW(deviceKey, L"DeviceState", nullptr, &stateType,
			reinterpret_cast<LPBYTE>(&deviceState), &stateBytes) == ERROR_SUCCESS)
		{
			if ((deviceState & 0x4) != 0 || (deviceState & 0x1) == 0)
			{
				RegCloseKey(deviceKey);
				continue;
			}
		}

		HKEY propsKey = nullptr;
		if (RegOpenKeyExW(deviceKey, L"Properties", 0, KEY_READ | KEY_WOW64_64KEY, &propsKey) == ERROR_SUCCESS)
		{
			const std::wstring friendlyName = readRegistryStringValue(propsKey, L"{a45c254e-df1c-4efd-8020-67d146a850e0},2");
			const std::wstring interfaceName = readRegistryStringValue(propsKey, L"{b3f8fa53-0004-438e-9003-51a46e139bfc},6");
			const std::wstring deviceDesc = readRegistryStringValue(propsKey, L"{a45c254e-df1c-4efd-8020-67d146a850e0},6");

			const std::wstring searchable = StringHelper::toLowerCase(friendlyName + L" " + interfaceName + L" " + deviceDesc);

			bool matches = false;
			for (const std::wstring& token : targetTokens)
			{
				if (searchable.find(token) != std::wstring::npos)
				{
					matches = true;
					break;
				}
			}
			if (matches)
			{
				matched.push_back({friendlyName, interfaceName, endpointGuid});
			}
			RegCloseKey(propsKey);
		}
		RegCloseKey(deviceKey);
	}
	RegCloseKey(renderKey);
	return matched;
}

std::wstring buildAsioCompositeDeviceString(
	const std::wstring& vendorName,
	std::wstring& outConnectionName,
	std::wstring& outDeviceGuid)
{
	std::wstring composite = L"Hibiki EQAPO";
	if (!vendorName.empty())
		composite += L" " + vendorName;

	std::wstring overrideGuid;
	for (HKEY root : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE})
	{
		HKEY key = nullptr;
		if (RegOpenKeyExW(root, L"Software\\EqualizerAPO\\ASIOProxy", 0, KEY_READ | KEY_WOW64_64KEY, &key) == ERROR_SUCCESS)
		{
			overrideGuid = readRegistryStringValue(key, L"TargetEndpointGuid");
			RegCloseKey(key);
			if (!overrideGuid.empty())
				break;
		}
	}

	if (!overrideGuid.empty() && overrideGuid.front() == L'{' && overrideGuid.back() == L'}')
	{
		HKEY propsKey = nullptr;
		const std::wstring subkey = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Render\\" + overrideGuid + L"\\Properties";
		if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, subkey.c_str(), 0, KEY_READ | KEY_WOW64_64KEY, &propsKey) == ERROR_SUCCESS)
		{
			const std::wstring friendlyName = readRegistryStringValue(propsKey, L"{a45c254e-df1c-4efd-8020-67d146a850e0},2");
			const std::wstring interfaceName = readRegistryStringValue(propsKey, L"{b3f8fa53-0004-438e-9003-51a46e139bfc},6");
			RegCloseKey(propsKey);
			outConnectionName = interfaceName.empty() ? friendlyName : interfaceName;
			outDeviceGuid = overrideGuid;
			composite += L" " + friendlyName + L" " + interfaceName + L" " + overrideGuid;
			return composite;
		}
	}

	const std::vector<EndpointMatchInfo> endpoints = findMatchingEndpoints(vendorName);
	for (const EndpointMatchInfo& ep : endpoints)
	{
		if (outConnectionName.empty())
			outConnectionName = ep.interfaceName.empty() ? ep.friendlyName : ep.interfaceName;
		if (outDeviceGuid.empty())
			outDeviceGuid = ep.guid;

		composite += L" " + ep.friendlyName + L" " + ep.interfaceName + L" " + ep.guid;
	}

	return composite;
}

class FilterEngineAsioDsp final : public IAsioDsp
{
public:
	FilterEngineAsioDsp(
		double sampleRate,
		unsigned channelCount,
		unsigned maximumFrameCount,
		const ResolvedAsioTarget& target = {})
	{
		if (!std::isfinite(sampleRate) || sampleRate <= 0.0 ||
			channelCount == 0 || channelCount > 2 || maximumFrameCount == 0)
		{
			return;
		}
		std::wstring connectionName;
		std::wstring deviceGuid;
		const std::wstring compositeDeviceString =
			buildAsioCompositeDeviceString(target.name, connectionName, deviceGuid);

		engine_ = std::make_unique<FilterEngine>();
		engine_->setProcessingPolicy(
			FilterEngine::ProcessingPolicy::AsioCallbackSafe);
		engine_->setDeviceInfo(
			false,
			true,
			L"Hibiki EQAPO",
			connectionName.empty() ? L"ASIO" : connectionName,
			deviceGuid,
			compositeDeviceString);
		engine_->initialize(
			static_cast<float>(sampleRate),
			channelCount,
			channelCount,
			channelCount,
			static_cast<unsigned>(ChannelHelper::getDefaultChannelMask(
				static_cast<int>(channelCount))),
			maximumFrameCount);
		if (engine_->rejectedUnsafeConfiguration() ||
			!engine_->hasActiveConfiguration())
			engine_.reset();
	}

	bool process(
		double** output,
		double** input,
		unsigned channelCount,
		unsigned frameCount) override
	{
		if (engine_ == nullptr || output == nullptr || input == nullptr ||
			channelCount != engine_->getOutputChannelCount() ||
			frameCount > engine_->getMaxFrameCount())
		{
			return false;
		}
		engine_->process(output, input, frameCount);
		return true;
	}

	bool isReady() const noexcept
	{
		return engine_ != nullptr;
	}

private:
	std::unique_ptr<FilterEngine> engine_;
};
}

std::unique_ptr<IAsioDsp> createDefaultAsioDsp(
	double sampleRate,
	unsigned channelCount,
	unsigned maximumFrameCount)
{
	try
	{
		HMODULE proxyModule = nullptr;
		GetModuleHandleExW(
			GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
				GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCWSTR>(&createDefaultAsioDsp),
			&proxyModule);
		const ResolvedAsioTarget target = resolveAsioTarget(proxyModule);
		auto dsp = std::make_unique<FilterEngineAsioDsp>(
			sampleRate, channelCount, maximumFrameCount, target);
		if (!dsp->isReady())
			return nullptr;
		return dsp;
	}
	catch (...)
	{
		return nullptr;
	}
}
}
