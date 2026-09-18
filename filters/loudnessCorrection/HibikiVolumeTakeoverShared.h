/*
    This file is part of Equalizer APO, a system-wide equalizer.
    Copyright (C) 2026  Equalizer APO contributors

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
*/

#pragma once

#include <windows.h>
#include <sddl.h>
#include <cstdint>
#include <cstring>
#include <algorithm>

#define HIBIKI_VOLUME_TAKEOVER_SHARED_NAME L"Local\\Hibiki_VolumeTakeover_v1"
#define HIBIKI_VOLUME_TAKEOVER_SHARED_NAME_GLOBAL L"Global\\Hibiki_VolumeTakeover_v1"

#pragma pack(push, 8)
struct HibikiVolumeTakeoverSharedData
{
	uint32_t version;
	uint32_t active;
	uint64_t sequence;
	double levelDb;
	double scalar;
	uint32_t muted;
	uint32_t processId;
	uint64_t lastHeartbeatTick;
	wchar_t endpointId[128];
};
#pragma pack(pop)

namespace HibikiTakeoverIpc
{
	inline std::wstring getTakeoverFilePath()
	{
		std::wstring path;
		HKEY hKey = NULL;
		if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\EqualizerAPO", 0, KEY_READ, &hKey) == ERROR_SUCCESS)
		{
			wchar_t buf[MAX_PATH] = {};
			DWORD cbData = sizeof(buf);
			if (RegQueryValueExW(hKey, L"ConfigPath", NULL, NULL, reinterpret_cast<LPBYTE>(buf), &cbData) == ERROR_SUCCESS)
			{
				path = buf;
			}
			RegCloseKey(hKey);
		}
		if (path.empty())
		{
			path = L"C:\\Program Files\\EqualizerAPO\\config";
		}
		return path + L"\\volume_takeover.dat";
	}

	inline HANDLE createOrOpenSharedMapping(bool forWriting = true)
	{
		PSECURITY_DESCRIPTOR sd = NULL;
		SECURITY_ATTRIBUTES sa = {};
		sa.nLength = sizeof(sa);
		// D:(A;;GA;;;WD)(A;;GA;;;AC) : Everyone & All Application Packages have Full Access
		// S:(ML;;NW;;;LW)           : Low Mandatory Level (audiodg.exe sandbox can access)
		if (ConvertStringSecurityDescriptorToSecurityDescriptorW(
			L"D:(A;;GA;;;WD)(A;;GA;;;AC)S:(ML;;NW;;;LW)", SDDL_REVISION_1, &sd, NULL))
		{
			sa.lpSecurityDescriptor = sd;
		}

		std::wstring filePath = getTakeoverFilePath();
		HANDLE mapping = NULL;

		if (!forWriting)
		{
			// 1. Try Global named mapping first
			mapping = OpenFileMappingW(
				FILE_MAP_READ,
				FALSE,
				HIBIKI_VOLUME_TAKEOVER_SHARED_NAME_GLOBAL);

			// 2. Try Local named mapping
			if (mapping == NULL)
			{
				mapping = OpenFileMappingW(
					FILE_MAP_READ,
					FALSE,
					HIBIKI_VOLUME_TAKEOVER_SHARED_NAME);
			}

			// 3. Try opening file-backed mapping from volume_takeover.dat (cross-session bridge for Session 0 audiodg.exe)
			if (mapping == NULL)
			{
				HANDLE hFile = CreateFileW(
					filePath.c_str(),
					GENERIC_READ,
					FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
					NULL,
					OPEN_EXISTING,
					FILE_ATTRIBUTE_NORMAL,
					NULL);
				if (hFile != INVALID_HANDLE_VALUE)
				{
					mapping = CreateFileMappingW(
						hFile,
						NULL,
						PAGE_READONLY,
						0,
						sizeof(HibikiVolumeTakeoverSharedData),
						NULL);
					CloseHandle(hFile);
				}
			}

			if (sd != NULL)
			{
				LocalFree(sd);
			}
			return mapping;
		}

		// Writer (Editor.exe):
		// 1. Ensure file-backed mapping on volume_takeover.dat so Session 0 audiodg.exe can always access it
		HANDLE hFile = CreateFileW(
			filePath.c_str(),
			GENERIC_READ | GENERIC_WRITE,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
			sa.lpSecurityDescriptor ? &sa : NULL,
			OPEN_ALWAYS,
			FILE_ATTRIBUTE_NORMAL,
			NULL);
		if (hFile != INVALID_HANDLE_VALUE)
		{
			DWORD fileSize = GetFileSize(hFile, NULL);
			if (fileSize < sizeof(HibikiVolumeTakeoverSharedData))
			{
				SetFilePointer(hFile, sizeof(HibikiVolumeTakeoverSharedData), NULL, FILE_BEGIN);
				SetEndOfFile(hFile);
			}

			// Try Global named mapping on this file first
			mapping = CreateFileMappingW(
				hFile,
				sa.lpSecurityDescriptor ? &sa : NULL,
				PAGE_READWRITE,
				0,
				sizeof(HibikiVolumeTakeoverSharedData),
				HIBIKI_VOLUME_TAKEOVER_SHARED_NAME_GLOBAL);

			if (mapping == NULL)
			{
				mapping = CreateFileMappingW(
					hFile,
					sa.lpSecurityDescriptor ? &sa : NULL,
					PAGE_READWRITE,
					0,
					sizeof(HibikiVolumeTakeoverSharedData),
					HIBIKI_VOLUME_TAKEOVER_SHARED_NAME);
			}

			if (mapping == NULL)
			{
				mapping = CreateFileMappingW(
					hFile,
					sa.lpSecurityDescriptor ? &sa : NULL,
					PAGE_READWRITE,
					0,
					sizeof(HibikiVolumeTakeoverSharedData),
					NULL);
			}

			CloseHandle(hFile);
		}

		// 2. Fallback to pure pagefile mappings if disk file failed
		if (mapping == NULL)
		{
			mapping = CreateFileMappingW(
				INVALID_HANDLE_VALUE,
				sa.lpSecurityDescriptor ? &sa : NULL,
				PAGE_READWRITE,
				0,
				sizeof(HibikiVolumeTakeoverSharedData),
				HIBIKI_VOLUME_TAKEOVER_SHARED_NAME_GLOBAL);
		}
		if (mapping == NULL)
		{
			mapping = CreateFileMappingW(
				INVALID_HANDLE_VALUE,
				sa.lpSecurityDescriptor ? &sa : NULL,
				PAGE_READWRITE,
				0,
				sizeof(HibikiVolumeTakeoverSharedData),
				HIBIKI_VOLUME_TAKEOVER_SHARED_NAME);
		}

		if (sd != NULL)
		{
			LocalFree(sd);
		}

		return mapping;
	}

	inline bool writeTakeoverSnapshot(
		HibikiVolumeTakeoverSharedData* shared,
		bool active,
		double levelDb,
		double scalar,
		bool muted,
		const wchar_t* endpointId,
		DWORD processId)
	{
		if (shared == nullptr)
			return false;

		shared->version = 1;
		shared->levelDb = levelDb;
		shared->scalar = (std::max)(0.0, (std::min)(1.0, scalar));
		shared->muted = muted ? 1 : 0;
		shared->processId = processId;
		shared->lastHeartbeatTick = GetTickCount64();

		if (endpointId != nullptr)
		{
			wcsncpy_s(shared->endpointId, endpointId, _TRUNCATE);
		}
		else
		{
			shared->endpointId[0] = L'\0';
		}

		MemoryBarrier();
		shared->active = active ? 1 : 0;
		InterlockedIncrement64(reinterpret_cast<volatile LONG64*>(&shared->sequence));
		return true;
	}

	inline bool readTakeoverSnapshot(
		const HibikiVolumeTakeoverSharedData* shared,
		HibikiVolumeTakeoverSharedData& outSnapshot)
	{
		if (shared == nullptr)
			return false;

		for (int attempt = 0; attempt < 5; ++attempt)
		{
			const volatile LONG64* seqPtr = reinterpret_cast<volatile const LONG64*>(&shared->sequence);
			LONG64 seqBefore = *seqPtr;
			MemoryBarrier();

			outSnapshot = *shared;

			MemoryBarrier();
			LONG64 seqAfter = *seqPtr;
			if (seqBefore == seqAfter)
			{
				return true;
			}
		}
		return false;
	}

	inline void updateHeartbeat(HibikiVolumeTakeoverSharedData* shared)
	{
		if (shared != nullptr)
		{
			shared->lastHeartbeatTick = GetTickCount64();
		}
	}
}
