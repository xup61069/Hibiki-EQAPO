/*
    This file is part of Equalizer APO, a system-wide equalizer.
    Copyright (C) 2026 Equalizer APO contributors
*/

#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <sddl.h>
#include <cstdint>
#include <cstddef>
#include <cmath>
#include <algorithm>

#define HIBIKI_VOLUME_TAKEOVER_SHARED_NAME L"Global\\Hibiki_VolumeTakeover_v1"

#pragma pack(push, 8)
struct HibikiVolumeTakeoverSharedData
{
	uint32_t version;            // 1
	uint32_t active;             // 1 = active, 0 = disabled
	uint64_t sequence;           // Sequence counter for lock-free reader (even = valid, odd = writing)
	double levelDb;              // User target volume in dB (-100.0 to 0.0)
	double scalar;               // User target scalar (0.0 to 1.0)
	uint32_t muted;              // 1 = muted, 0 = unmuted
	uint64_t lastHeartbeatTick;  // GetTickCount64() heartbeat from Editor
	uint32_t editorPid;          // Editor process ID
	wchar_t endpointId[128];     // Target endpoint GUID/ID, or L"" for all/default
};
#pragma pack(pop)

namespace HibikiTakeoverIpc
{
	inline void writeTakeoverSnapshot(
		HibikiVolumeTakeoverSharedData* shared,
		bool active,
		double levelDb,
		double scalar,
		bool muted,
		const wchar_t* endpointId = nullptr,
		uint32_t pid = 0)
	{
		if (!shared)
			return;

		volatile LONG64* seq = reinterpret_cast<volatile LONG64*>(&shared->sequence);
		InterlockedIncrement64(seq);
		MemoryBarrier();

		shared->version = 1;
		shared->active = active ? 1 : 0;
		shared->levelDb = std::isfinite(levelDb) ? (std::max)(-100.0, (std::min)(0.0, levelDb)) : 0.0;
		shared->scalar = std::isfinite(scalar) ? (std::max)(0.0, (std::min)(1.0, scalar)) : 1.0;
		shared->muted = muted ? 1 : 0;
		shared->lastHeartbeatTick = GetTickCount64();
		if (pid != 0)
			shared->editorPid = pid;

		if (endpointId)
		{
			wcsncpy_s(shared->endpointId, endpointId, _TRUNCATE);
		}

		MemoryBarrier();
		InterlockedIncrement64(seq);
	}

	inline void updateHeartbeat(HibikiVolumeTakeoverSharedData* shared)
	{
		if (shared && shared->active)
		{
			shared->lastHeartbeatTick = GetTickCount64();
		}
	}

	inline bool readTakeoverSnapshot(
		const HibikiVolumeTakeoverSharedData* shared,
		HibikiVolumeTakeoverSharedData& result)
	{
		if (!shared)
			return false;

		volatile const LONG64* seq = reinterpret_cast<volatile const LONG64*>(&shared->sequence);

		for (int attempt = 0; attempt < 4; ++attempt)
		{
			const LONG64 before = *seq;
			if ((before & 1) != 0)
				continue;

			MemoryBarrier();
			HibikiVolumeTakeoverSharedData snapshot = *shared;
			MemoryBarrier();

			const LONG64 after = *seq;
			if (before == after && (after & 1) == 0)
			{
				result = snapshot;
				return true;
			}
		}
		return false;
	}

	inline HANDLE createOrOpenSharedMapping(bool forWriting = true)
	{
		PSECURITY_DESCRIPTOR sd = NULL;
		SECURITY_ATTRIBUTES sa = {};
		sa.nLength = sizeof(sa);
		if (ConvertStringSecurityDescriptorToSecurityDescriptorW(
			L"D:(A;;GA;;;WD)", SDDL_REVISION_1, &sd, NULL))
		{
			sa.lpSecurityDescriptor = sd;
		}

		HANDLE mapping = CreateFileMappingW(
			INVALID_HANDLE_VALUE,
			sd ? &sa : NULL,
			PAGE_READWRITE,
			0,
			sizeof(HibikiVolumeTakeoverSharedData),
			HIBIKI_VOLUME_TAKEOVER_SHARED_NAME);

		if (sd)
			LocalFree(sd);

		if (mapping == NULL)
		{
			DWORD desiredAccess = forWriting ? FILE_MAP_ALL_ACCESS : FILE_MAP_READ;
			mapping = OpenFileMappingW(
				desiredAccess,
				FALSE,
				HIBIKI_VOLUME_TAKEOVER_SHARED_NAME);
		}
		return mapping;
	}
}
