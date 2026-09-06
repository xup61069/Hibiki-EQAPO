/*
    This file is part of Equalizer APO, a system-wide equalizer.
    Copyright (C) 2017  Jonas Thedering

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with this program; if not, write to the Free Software Foundation, Inc.,
    51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/

#include "stdafx.h"
#include <algorithm>
#include <condition_variable>
#include <thread>
#include "RegistryHelper.h"
#include "LogHelper.h"
#include "VSTPluginLibrary.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"

using namespace std;

struct VSTPluginLibrary::InstanceSlot
{
	std::recursive_mutex mutex;
	std::condition_variable_any condition;
	std::weak_ptr<VSTPluginLibrary> instance;
	bool hasInstance = false;
	bool destroying = false;
	std::thread::id destructorThread;
};

std::unordered_map<std::wstring, std::shared_ptr<VSTPluginLibrary::InstanceSlot>> VSTPluginLibrary::instanceMap;
std::once_flag VSTPluginLibrary::defaultPluginPathOnce;
std::wstring VSTPluginLibrary::defaultPluginPath;

std::shared_ptr<VSTPluginLibrary> VSTPluginLibrary::getInstance(const wstring& libPath)
{
	const wstring key = getInstanceKey(libPath);
	shared_ptr<InstanceSlot> slot;
	{
		std::lock_guard<std::mutex> mapLock(instanceMapMutex());
		auto it = instanceMap.find(key);
		if (it == instanceMap.end())
		{
			slot = make_shared<InstanceSlot>();
			instanceMap[key] = slot;
		}
		else
		{
			slot = it->second;
		}
	}

	std::unique_lock<std::recursive_mutex> slotLock(slot->mutex);
	while (slot->hasInstance)
	{
		shared_ptr<VSTPluginLibrary> ptr = slot->instance.lock();
		if (ptr != NULL)
			return ptr;
		if (slot->destroying && slot->destructorThread == this_thread::get_id())
			return nullptr;
		slot->condition.wait(slotLock);
	}

	weak_ptr<InstanceSlot> weakSlot = slot;
	shared_ptr<VSTPluginLibrary> ptr(
		new VSTPluginLibrary(libPath),
		[weakSlot](VSTPluginLibrary* library) {
			shared_ptr<InstanceSlot> ownedSlot = weakSlot.lock();
			if (ownedSlot == NULL)
			{
				delete library;
				return;
			}

			std::unique_lock<std::recursive_mutex> destructionLock(ownedSlot->mutex);
			ownedSlot->destroying = true;
			ownedSlot->destructorThread = this_thread::get_id();
			delete library;
			ownedSlot->instance.reset();
			ownedSlot->hasInstance = false;
			ownedSlot->destroying = false;
			ownedSlot->destructorThread = std::thread::id();
			destructionLock.unlock();
			ownedSlot->condition.notify_all();
		});
	slot->instance = ptr;
	slot->hasInstance = true;
	return ptr;
}

wstring VSTPluginLibrary::getDefaultPluginPath()
{
#ifdef EQAPO_ENABLE_UI_SNAPSHOTS
	// Snapshot builds use in-memory configuration rows. Never consult the
	// installed product's registry-backed VST directory for those rows.
	return L"";
#else
	std::call_once(defaultPluginPathOnce, []() {
		wstring installPath = RegistryHelper::readValue(APP_REGPATH, L"InstallPath");
		defaultPluginPath = installPath + L"\\VSTPlugins";
	});

	return defaultPluginPath;
#endif
}

VSTPluginLibrary::~VSTPluginLibrary()
{
	customUninitialize();
}

int VSTPluginLibrary::initialize()
{
	if (selfModule)
		return RECURSIVE_LOADING;
	return AbstractLibrary::initialize();
}

std::wstring VSTPluginLibrary::getLibPath()
{
	return libPath;
}

std::wstring VSTPluginLibrary::getLoadPath()
{
	return loadPath;
}

bool VSTPluginLibrary::isVST3() const
{
	return vst3;
}

Steinberg::IPluginFactory* VSTPluginLibrary::getFactory() const
{
	return factory;
}

const Steinberg::PClassInfo& VSTPluginLibrary::getVST3ClassInfo(int index) const
{
	if (!vst3ClassInfos.empty())
	{
		if (index < 0 || index >= static_cast<int>(vst3ClassInfos.size()))
			index = 0;
		return vst3ClassInfos[index];
	}
	return vst3ClassInfo;
}

int VSTPluginLibrary::getVST3ClassCount() const
{
	return static_cast<int>(vst3ClassInfos.size());
}

bool VSTPluginLibrary::loadFunctions()
{
	if (vst3)
	{
		GetPluginFactory = (getPluginFactoryFunc)GetProcAddress(module, "GetPluginFactory");
		InitModule = (moduleEntryFunc)GetProcAddress(module, "InitDll");
		ExitModule = (moduleEntryFunc)GetProcAddress(module, "ExitDll");
		return GetPluginFactory != NULL;
	}

	VSTPluginMain = (vstPluginMain)GetProcAddress(module, "VSTPluginMain");
	return VSTPluginMain != NULL;
}

int VSTPluginLibrary::customInitialize()
{
	if (!vst3)
		return 0;

	try
	{
		vst3ModuleEntered = true;
		if (InitModule != NULL && !InitModule())
			return FUNCTIONS_MISSING;

		factory = GetPluginFactory();
		if (factory == NULL)
		{
			customUninitialize();
			return FUNCTIONS_MISSING;
		}

		for (Steinberg::int32 i = 0; i < factory->countClasses(); i++)
		{
			Steinberg::PClassInfo info;
			if (factory->getClassInfo(i, &info) == Steinberg::kResultOk
				&& strcmp(info.category, kVstAudioEffectClass) == 0)
			{
				if (vst3ClassInfos.empty())
					vst3ClassInfo = info;
				vst3ClassInfos.push_back(info);
			}
		}

		if (vst3ClassInfos.empty())
		{
			customUninitialize();
			return FUNCTIONS_MISSING;
		}
		return 0;
	}
	catch (...)
	{
		customUninitialize();
		throw;
	}
}

void VSTPluginLibrary::customUninitialize()
{
	if (factory != NULL)
	{
		factory->release();
		factory = NULL;
	}
	vst3ClassInfos.clear();
	vst3ClassInfo = {};
	if (vst3ModuleEntered && ExitModule != NULL)
		ExitModule();
	vst3ModuleEntered = false;
	GetPluginFactory = NULL;
	InitModule = NULL;
	ExitModule = NULL;
}

VSTPluginLibrary::VSTPluginLibrary(const wstring& libPath)
	: libPath(libPath), loadPath(libPath)
{
	wchar_t extension[_MAX_EXT];
	_wsplitpath_s(libPath.c_str(), NULL, 0, NULL, 0, NULL, 0, extension, _MAX_EXT);
	vst3 = _wcsicmp(extension, L".vst3") == 0;
	if (vst3)
		loadPath = resolveVST3ModulePath(libPath);
	selfModule = isCurrentModulePath(loadPath);
}

wstring VSTPluginLibrary::getInstanceKey(const wstring& libPath)
{
	wstring key = resolveVST3ModulePath(libPath);
	if (key.empty())
		return key;

	const DWORD length = GetFullPathNameW(key.c_str(), 0, NULL, NULL);
	if (length > 0)
	{
		vector<wchar_t> absolutePath(static_cast<size_t>(length) + 1, L'\0');
		const DWORD written = GetFullPathNameW(
			key.c_str(), static_cast<DWORD>(absolutePath.size()), absolutePath.data(), NULL);
		if (written > 0 && written < absolutePath.size())
			key.assign(absolutePath.data(), written);
	}
	std::replace(key.begin(), key.end(), L'/', L'\\');
	if (!key.empty())
		CharLowerBuffW(&key[0], static_cast<DWORD>(key.size()));

	wstring identityKey;
	if (tryGetFileIdentityKey(key, identityKey))
		return identityKey;
	return key;
}

bool VSTPluginLibrary::tryGetFileIdentityKey(const wstring& filePath, wstring& identityKey)
{
	HANDLE file = CreateFileW(
		filePath.c_str(),
		FILE_READ_ATTRIBUTES,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		NULL,
		OPEN_EXISTING,
		FILE_FLAG_BACKUP_SEMANTICS,
		NULL);
	if (file == INVALID_HANDLE_VALUE)
		return false;

	FILE_ID_INFO info = {};
	const BOOL hasIdentity = GetFileInformationByHandleEx(
		file, FileIdInfo, &info, sizeof(info));
	CloseHandle(file);
	if (!hasIdentity)
		return false;

	wchar_t volume[17] = {};
	swprintf_s(
		volume,
		L"%016llX",
		static_cast<unsigned long long>(info.VolumeSerialNumber));
	identityKey = L"fileid:";
	identityKey += volume;
	identityKey += L":";
	static const wchar_t hex[] = L"0123456789ABCDEF";
	for (unsigned char value : info.FileId.Identifier)
	{
		identityKey.push_back(hex[(value >> 4) & 0x0F]);
		identityKey.push_back(hex[value & 0x0F]);
	}
	return true;
}

bool VSTPluginLibrary::isCurrentModulePath(const wstring& libPath)
{
	return isCurrentModuleKey(getInstanceKey(libPath));
}

bool VSTPluginLibrary::isCurrentModuleKey(const wstring& instanceKey)
{
	if (instanceKey.empty())
		return false;

	HMODULE currentModule = NULL;
	if (!GetModuleHandleExW(
		GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
			GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		reinterpret_cast<LPCWSTR>(&instanceMap),
		&currentModule))
	{
		return false;
	}

	vector<wchar_t> modulePath(32768, L'\0');
	const DWORD length = GetModuleFileNameW(
		currentModule, modulePath.data(), static_cast<DWORD>(modulePath.size()));
	if (length == 0 || length >= modulePath.size())
		return false;
	return instanceKey == getInstanceKey(wstring(modulePath.data(), length));
}

std::mutex& VSTPluginLibrary::instanceMapMutex()
{
	static std::mutex mutex;
	return mutex;
}

wstring VSTPluginLibrary::resolveVST3ModulePath(const wstring& libPath)
{
	DWORD attributes = GetFileAttributesW(libPath.c_str());
	if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
		return libPath;

#if defined(_M_ARM64)
	const wchar_t* platformDir = L"arm64-win";
#elif defined(_WIN64)
	const wchar_t* platformDir = L"x86_64-win";
#else
	const wchar_t* platformDir = L"x86-win";
#endif

	wchar_t searchPath[MAX_PATH];
	wcscpy_s(searchPath, libPath.c_str());
	PathAppendW(searchPath, L"Contents");
	PathAppendW(searchPath, platformDir);
	PathAppendW(searchPath, L"*.vst3");

	WIN32_FIND_DATAW findData;
	HANDLE hFind = FindFirstFileW(searchPath, &findData);
	if (hFind == INVALID_HANDLE_VALUE)
		return libPath;

	wchar_t modulePath[MAX_PATH];
	wcscpy_s(modulePath, libPath.c_str());
	PathAppendW(modulePath, L"Contents");
	PathAppendW(modulePath, platformDir);
	PathAppendW(modulePath, findData.cFileName);
	FindClose(hFind);

	return modulePath;
}
