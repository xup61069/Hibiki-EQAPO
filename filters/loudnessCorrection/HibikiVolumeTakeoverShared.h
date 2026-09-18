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
	inline HANDLE createOrOpenSharedMapping(bool forWriting = true)
	{
		if (!forWriting)
		{
			HANDLE mapping = OpenFileMappingW(
				FILE_MAP_READ,
				FALSE,
				HIBIKI_VOLUME_TAKEOVER_SHARED_NAME);
			if (mapping == NULL)
			{
				mapping = OpenFileMappingW(
					FILE_MAP_READ,
					FALSE,
					HIBIKI_VOLUME_TAKEOVER_SHARED_NAME_GLOBAL);
			}
			return mapping;
		}

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

		HANDLE mapping = CreateFileMappingW(
			INVALID_HANDLE_VALUE,
			sa.lpSecurityDescriptor ? &sa : NULL,
			PAGE_READWRITE,
			0,
			sizeof(HibikiVolumeTakeoverSharedData),
			HIBIKI_VOLUME_TAKEOVER_SHARED_NAME);

		if (mapping == NULL)
		{
			mapping = OpenFileMappingW(
				FILE_MAP_ALL_ACCESS,
				FALSE,
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
