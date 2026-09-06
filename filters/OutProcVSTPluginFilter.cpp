/*
    This file is part of Equalizer APO, a system-wide equalizer.
    Copyright (C) 2017  Jonas Thedering

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
*/

#include "stdafx.h"

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <limits>
#include <sstream>
#include <sddl.h>

#include "helpers/LogHelper.h"
#include "OutProcVSTPluginFilter.h"

using namespace std;

static void outProcVSTModuleAnchor()
{
}

namespace
{
	constexpr DWORD NAMED_HOST_HANDOFF_EXIT_CODE = 20;

	struct NamedHostMonitorContext
	{
		HANDLE stopEvent = NULL;
		HANDLE shutdownEvent = NULL;
		HANDLE hostLeaseMutex = NULL;
		HANDLE hostHandoffEvent = NULL;
		HANDLE hostReadyEvent = NULL;
		wstring hostPath;
		wstring sessionId;
		wstring configPath;

		~NamedHostMonitorContext()
		{
			if (stopEvent != NULL)
				CloseHandle(stopEvent);
			if (shutdownEvent != NULL)
				CloseHandle(shutdownEvent);
			if (hostLeaseMutex != NULL)
				CloseHandle(hostLeaseMutex);
			if (hostHandoffEvent != NULL)
				CloseHandle(hostHandoffEvent);
			if (hostReadyEvent != NULL)
				CloseHandle(hostReadyEvent);
		}
	};

	bool duplicateMonitorHandle(HANDLE source, HANDLE& destination)
	{
		return source != NULL && DuplicateHandle(
			GetCurrentProcess(), source,
			GetCurrentProcess(), &destination,
			0, FALSE, DUPLICATE_SAME_ACCESS) != FALSE;
	}

	HANDLE launchNamedHostProcess(const NamedHostMonitorContext& context)
	{
		wstringstream commandLine;
		commandLine << L"\"" << context.hostPath << L"\""
			<< L" --session \"" << context.sessionId << L"\""
			<< L" --vst-config \"" << context.configPath << L"\""
			<< L" --parent-pid " << GetCurrentProcessId();

		wstring commandLineStorage = commandLine.str();
		const size_t slash = context.hostPath.find_last_of(L"\\/");
		const wstring workingDirectory = slash == wstring::npos ? L"" : context.hostPath.substr(0, slash);

		STARTUPINFOW startupInfo = {};
		startupInfo.cb = sizeof(startupInfo);
		PROCESS_INFORMATION processInformation = {};
		if (!CreateProcessW(context.hostPath.c_str(), &commandLineStorage[0], NULL, NULL, FALSE,
			CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS, NULL,
			workingDirectory.empty() ? NULL : workingDirectory.c_str(), &startupInfo, &processInformation))
		{
			LogFStatic(L"OutProcVSTPlugin: named host launch failed for %s (error %lu)", context.hostPath.c_str(), GetLastError());
			return NULL;
		}

		CloseHandle(processInformation.hThread);
		TraceFStatic(L"OutProcVSTPlugin: launched cold-start host for session %s", context.sessionId.c_str());
		return processInformation.hProcess;
	}

	DWORD WINAPI namedHostMonitorProc(LPVOID parameter)
	{
		unique_ptr<NamedHostMonitorContext> context(static_cast<NamedHostMonitorContext*>(parameter));
		if (!context || context->stopEvent == NULL || context->shutdownEvent == NULL
			|| context->hostLeaseMutex == NULL || context->hostHandoffEvent == NULL
			|| context->hostReadyEvent == NULL)
			return 1;

		unsigned failureCount = 0;
		while (WaitForSingleObject(context->stopEvent, 0) != WAIT_OBJECT_0)
		{
			HANDLE leaseWaitHandles[] = { context->stopEvent, context->hostLeaseMutex };
			const DWORD leaseResult = WaitForMultipleObjects(2, leaseWaitHandles, FALSE, 250);
			if (leaseResult == WAIT_OBJECT_0)
				break;
			if (leaseResult != WAIT_OBJECT_0 + 1 && leaseResult != WAIT_ABANDONED_0 + 1)
				continue;

			// The monitor only samples availability. Clear state while it still
			// owns the lease so a GUI host cannot publish Ready and have that
			// signal erased immediately afterwards. A pending manual-reset
			// Handoff gives the Editor-launched host first chance at the lease.
			const bool handoffPending =
				WaitForSingleObject(context->hostHandoffEvent, 0) == WAIT_OBJECT_0;
			ResetEvent(context->hostReadyEvent);
			if (handoffPending)
				ResetEvent(context->hostHandoffEvent);
			ReleaseMutex(context->hostLeaseMutex);

			if (handoffPending)
			{
				HANDLE handoffWaitHandles[] = { context->stopEvent, context->hostReadyEvent };
				if (WaitForMultipleObjects(2, handoffWaitHandles, FALSE, 5000) == WAIT_OBJECT_0)
					break;
				failureCount = 0;
				continue;
			}

			HANDLE child = launchNamedHostProcess(*context);
			if (child == NULL)
			{
				failureCount = min(failureCount + 1, 5u);
				const DWORD backoffMs = min<DWORD>(30000, 1000u << failureCount);
				if (WaitForSingleObject(context->stopEvent, backoffMs) == WAIT_OBJECT_0)
					break;
				continue;
			}

			const ULONGLONG launchTick = GetTickCount64();
			HANDLE childWaitHandles[] = { context->stopEvent, child, context->hostHandoffEvent };
			const DWORD childResult = WaitForMultipleObjects(3, childWaitHandles, FALSE, INFINITE);
			if (childResult == WAIT_OBJECT_0)
			{
				SetEvent(context->shutdownEvent);
				if (WaitForSingleObject(child, 1000) == WAIT_TIMEOUT)
					TerminateProcess(
						child, OUTPROC_RUNTIME_FORCED_TERMINATION_EXIT_CODE);
				CloseHandle(child);
				break;
			}

			// No child can service audio after this wait. Withdraw Ready before
			// inspecting its exit code or waiting for the GUI replacement.
			ResetEvent(context->hostReadyEvent);
			DWORD exitCode = 0;
			if (childResult == WAIT_OBJECT_0 + 2)
			{
				// A plug-in may be stuck inside processBlock and unable to consume the
				// handoff itself. Bound the wait so the interactive host can still open.
				if (WaitForSingleObject(child, 1000) == WAIT_TIMEOUT)
					TerminateProcess(child, NAMED_HOST_HANDOFF_EXIT_CODE);
				WaitForSingleObject(child, 1000);
				exitCode = NAMED_HOST_HANDOFF_EXIT_CODE;
				ResetEvent(context->hostHandoffEvent);
			}
			else if (childResult == WAIT_OBJECT_0 + 1)
				GetExitCodeProcess(child, &exitCode);
			else
				exitCode = 1;
			CloseHandle(child);

			if (exitCode == NAMED_HOST_HANDOFF_EXIT_CODE)
			{
				// Give the Editor-launched GUI first chance to acquire HostLease.
				ResetEvent(context->hostHandoffEvent);
				HANDLE handoffWaitHandles[] = { context->stopEvent, context->hostReadyEvent };
				if (WaitForMultipleObjects(2, handoffWaitHandles, FALSE, 5000) == WAIT_OBJECT_0)
					break;
				failureCount = 0;
				continue;
			}

			if (GetTickCount64() - launchTick >= 30000)
				failureCount = 0;
			failureCount = min(failureCount + 1, 5u);
			const DWORD backoffMs = min<DWORD>(30000, 1000u << failureCount);
			if (WaitForSingleObject(context->stopEvent, backoffMs) == WAIT_OBJECT_0)
				break;
		}

		return 0;
	}
}

OutProcVSTPluginFilter::OutProcVSTPluginFilter(wstring libPath, wstring chunkData, unordered_map<wstring, float> paramMap, wstring hostId, bool analysisMode, int vst3ClassIndex, wstring midiConfig)
	: sampleRate(0.0f),
	  hostId(hostId),
	  channelCount(0),
	  maxFrameCount(0),
	  timeoutMs(1),
	  requestSeq(0),
	  sharedByteCount(0),
	  mappingHandle(NULL),
	  requestEvent(NULL),
	  responseEvent(NULL),
	  shutdownEvent(NULL),
	  runtimeOwnerToken(NULL),
	  hostLeaseMutex(NULL),
	  hostHandoffEvent(NULL),
	  hostReadyEvent(NULL),
	  monitorStopEvent(NULL),
	  monitorThread(NULL),
	  processHandle(NULL),
	  sharedHeader(nullptr),
	  namedHostMode(!hostId.empty() && !analysisMode),
	  analysisMode(analysisMode),
	  launchFailureLogged(false),
	  timeoutLogged(false),
	  exitedLogged(false),
	  protocolLogged(false),
	  wasBypassed(false),
	  disabled(false),
	  consecutiveTimeouts(0)
{
	vstConfig.libraryPath = libPath;
	vstConfig.vst3ClassIndex = vst3ClassIndex;
	vstConfig.chunkData = chunkData;
	vstConfig.paramMap = paramMap;
	vstConfig.midiConfig = std::move(midiConfig);
}

OutProcVSTPluginFilter::~OutProcVSTPluginFilter()
{
	closeHost();
	if (!configFilePath.empty())
		DeleteFileW(configFilePath.c_str());
}

vector<wstring> OutProcVSTPluginFilter::initialize(float sampleRate, unsigned maxFrameCount, vector<wstring> channelNames)
{
	closeHost();
	this->sampleRate = sampleRate;
	this->channelCount = 0;
	this->maxFrameCount = maxFrameCount;
	sharedByteCount = 0;
	if (channelNames.size() > (std::numeric_limits<unsigned>::max)())
		return channelNames;
	this->channelCount = static_cast<unsigned>(channelNames.size());
	if (channelCount == 0 || maxFrameCount == 0 ||
		!std::isfinite(sampleRate) || sampleRate <= 0.0f ||
		static_cast<double>(sampleRate) >
			static_cast<double>((std::numeric_limits<unsigned>::max)()))
	{
		LogF(L"OutProcVSTPlugin: invalid audio format, filter will bypass");
		return channelNames;
	}
	const double blockMs = (static_cast<double>(maxFrameCount) * 1000.0) / sampleRate;
	const double halfBlockMs = blockMs * 0.5;
	if (analysisMode)
		timeoutMs = max(100u, static_cast<unsigned>(min(2000.0, max(100.0, blockMs * 2.0))));
	else
		timeoutMs = max(1u, static_cast<unsigned>(min(5.0, halfBlockMs)));

	if (!OutProcAudioSharedMemorySize(channelCount, maxFrameCount, sharedByteCount))
	{
		LogF(L"OutProcVSTPlugin: invalid shared memory size, filter will bypass");
		return channelNames;
	}

	startHost();
	return channelNames;
}

#pragma AVRT_CODE_BEGIN
void OutProcVSTPluginFilter::process(double** output, double** input, unsigned frameCount)
{
	if (disabled || frameCount == 0 || frameCount > maxFrameCount || sharedHeader == nullptr || (!namedHostMode && processHandle == NULL))
	{
		bypass(output, input, frameCount);
		return;
	}
	if (namedHostMode && (hostReadyEvent == NULL
		|| WaitForSingleObject(hostReadyEvent, 0) != WAIT_OBJECT_0))
	{
		// Cold start, crash backoff and GUI handoff are all expected states.
		// Do not spend the realtime timeout budget until a host has its DSP ready.
		bypass(output, input, frameCount);
		return;
	}

	if (!namedHostMode)
	{
		const DWORD processState = WaitForSingleObject(processHandle, 0);
		if (processState == WAIT_OBJECT_0 || processState == WAIT_FAILED)
		{
			logHostExited();
			disabled = true;
			bypass(output, input, frameCount);
			return;
		}
	}

	double* sharedInput = OutProcAudioInput(sharedHeader);
	for (unsigned channel = 0; channel < channelCount; ++channel)
	{
		double* channelInput = sharedInput + static_cast<size_t>(channel) * maxFrameCount;
		for (unsigned frame = 0; frame < frameCount; ++frame)
			channelInput[frame] = input[channel][frame];
	}

	sharedHeader->frameCount = frameCount;
	sharedHeader->dspType = OUTPROC_AUDIO_DSP_VST;
	sharedHeader->status = OUTPROC_AUDIO_STATUS_OK;
	sharedHeader->requestSeq = ++requestSeq;

	ResetEvent(responseEvent);
	if (!SetEvent(requestEvent))
	{
		logHostExited();
		bypass(output, input, frameCount);
		return;
	}

	const DWORD waitResult = WaitForSingleObject(responseEvent, timeoutMs);
	if (waitResult != WAIT_OBJECT_0)
	{
		logTimeout();
		consecutiveTimeouts++;
		if (!namedHostMode && !analysisMode && consecutiveTimeouts >= 1)
			disableAfterFailure(L"timeout");
		bypass(output, input, frameCount);
		return;
	}

	if (!validateProtocol(frameCount))
	{
		logProtocolMismatch();
		if (!namedHostMode && !analysisMode)
			disableAfterFailure(L"protocol mismatch");
		bypass(output, input, frameCount);
		return;
	}

	double* sharedOutput = OutProcAudioOutput(sharedHeader);
	for (unsigned channel = 0; channel < channelCount; ++channel)
	{
		double* channelOutput = sharedOutput + static_cast<size_t>(channel) * maxFrameCount;
		for (unsigned frame = 0; frame < frameCount; ++frame)
			output[channel][frame] = channelOutput[frame];
	}

	if (wasBypassed)
		logRecovered();
	wasBypassed = false;
	consecutiveTimeouts = 0;
}
#pragma AVRT_CODE_END

void OutProcVSTPluginFilter::closeHost()
{
	if (monitorStopEvent != NULL)
		SetEvent(monitorStopEvent);
	if (hostReadyEvent != NULL)
		ResetEvent(hostReadyEvent);
	if (shutdownEvent != NULL)
		SetEvent(shutdownEvent);

	if (monitorThread != NULL)
	{
		if (WaitForSingleObject(monitorThread, 3000) == WAIT_TIMEOUT)
			LogF(L"OutProcVSTPlugin: named host monitor did not stop within 3 seconds");
		CloseHandle(monitorThread);
		monitorThread = NULL;
	}
	if (monitorStopEvent != NULL)
	{
		CloseHandle(monitorStopEvent);
		monitorStopEvent = NULL;
	}

	if (processHandle != NULL)
	{
		if (WaitForSingleObject(processHandle, 1000) == WAIT_TIMEOUT)
			TerminateProcess(
				processHandle, OUTPROC_RUNTIME_FORCED_TERMINATION_EXIT_CODE);
		CloseHandle(processHandle);
		processHandle = NULL;
	}

	if (sharedHeader != nullptr)
	{
		UnmapViewOfFile(sharedHeader);
		sharedHeader = nullptr;
	}

	if (mappingHandle != NULL)
		CloseHandle(mappingHandle);
	if (requestEvent != NULL)
		CloseHandle(requestEvent);
	if (responseEvent != NULL)
		CloseHandle(responseEvent);
	if (shutdownEvent != NULL)
		CloseHandle(shutdownEvent);
	if (hostHandoffEvent != NULL)
		CloseHandle(hostHandoffEvent);
	if (hostReadyEvent != NULL)
		CloseHandle(hostReadyEvent);
	if (hostLeaseMutex != NULL)
		CloseHandle(hostLeaseMutex);
	if (runtimeOwnerToken != NULL)
		CloseHandle(runtimeOwnerToken);

	mappingHandle = NULL;
	requestEvent = NULL;
	responseEvent = NULL;
	shutdownEvent = NULL;
	hostHandoffEvent = NULL;
	hostReadyEvent = NULL;
	hostLeaseMutex = NULL;
	runtimeOwnerToken = NULL;
}

bool OutProcVSTPluginFilter::startHost()
{
	closeHost();

	if (namedHostMode)
	{
		if (startNamedHostSession())
			return true;

		LogF(L"OutProcVSTPlugin: named session unavailable, falling back to an isolated host");
		namedHostMode = false;
	}

	return startInheritedHost();
}

bool OutProcVSTPluginFilter::startNamedHostSession()
{
	if (!writeConfigFile())
	{
		logLaunchFailure(L"could not write VST config file");
		return false;
	}

	SECURITY_ATTRIBUTES securityAttributes = {};
	PSECURITY_DESCRIPTOR securityDescriptor = NULL;
	if (!createOpenSecurityAttributes(securityAttributes, securityDescriptor))
	{
		logLaunchFailure(L"could not create named-session security descriptor");
		return false;
	}

	// The event is a lifetime token, not a signal. CreateEvent atomically tells
	// us whether another runtime already owns this HostId, and closing its handle
	// releases the claim safely even if destruction happens on another thread.
	SetLastError(ERROR_SUCCESS);
	runtimeOwnerToken = CreateEventW(
		&securityAttributes, TRUE, FALSE, makeObjectName(L"RuntimeOwner").c_str());
	const DWORD ownerError = GetLastError();
	if (runtimeOwnerToken == NULL || ownerError == ERROR_ALREADY_EXISTS)
	{
		if (securityDescriptor != NULL)
			LocalFree(securityDescriptor);
		if (runtimeOwnerToken != NULL)
			CloseHandle(runtimeOwnerToken);
		runtimeOwnerToken = NULL;
		logLaunchFailure(L"duplicate or inaccessible named runtime session");
		return false;
	}

	const DWORD mappingSizeHigh = static_cast<DWORD>(sharedByteCount >> 32);
	const DWORD mappingSizeLow = static_cast<DWORD>(sharedByteCount & 0xffffffff);
	mappingHandle = CreateFileMappingW(INVALID_HANDLE_VALUE, &securityAttributes, PAGE_READWRITE, mappingSizeHigh, mappingSizeLow, makeObjectName(L"Map").c_str());
	requestEvent = CreateEventW(&securityAttributes, FALSE, FALSE, makeObjectName(L"Request").c_str());
	responseEvent = CreateEventW(&securityAttributes, FALSE, FALSE, makeObjectName(L"Response").c_str());
	shutdownEvent = CreateEventW(&securityAttributes, TRUE, FALSE, makeObjectName(L"Shutdown").c_str());
	hostLeaseMutex = CreateMutexW(&securityAttributes, FALSE, makeObjectName(L"HostLease").c_str());
	hostHandoffEvent = CreateEventW(&securityAttributes, TRUE, FALSE, makeObjectName(L"HostHandoff").c_str());
	hostReadyEvent = CreateEventW(&securityAttributes, TRUE, FALSE, makeObjectName(L"HostReady").c_str());

	if (securityDescriptor != NULL)
		LocalFree(securityDescriptor);

	if (mappingHandle == NULL || requestEvent == NULL || responseEvent == NULL || shutdownEvent == NULL
		|| hostLeaseMutex == NULL || hostHandoffEvent == NULL || hostReadyEvent == NULL)
	{
		logLaunchFailure(L"named Win32 handle creation failed");
		closeHost();
		return false;
	}
	ResetEvent(shutdownEvent);
	ResetEvent(requestEvent);
	ResetEvent(responseEvent);
	ResetEvent(hostHandoffEvent);

	sharedHeader = static_cast<OutProcAudioHeader*>(MapViewOfFile(mappingHandle, FILE_MAP_ALL_ACCESS, 0, 0, sharedByteCount));
	if (sharedHeader == nullptr)
	{
		logLaunchFailure(L"named shared memory map failed");
		closeHost();
		return false;
	}

	ZeroMemory(sharedHeader, sharedByteCount);
	sharedHeader->magic = OUTPROC_AUDIO_MAGIC;
	sharedHeader->version = OUTPROC_AUDIO_VERSION;
	sharedHeader->sampleRate = static_cast<uint32_t>(sampleRate);
	sharedHeader->channelCount = channelCount;
	sharedHeader->maxFrames = maxFrameCount;
	sharedHeader->dspType = OUTPROC_AUDIO_DSP_VST;

	const wstring hostPath = getHostPath();
	if (hostPath.empty())
	{
		logLaunchFailure(L"host path could not be resolved");
		closeHost();
		return false;
	}

	monitorStopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
	unique_ptr<NamedHostMonitorContext> context(new NamedHostMonitorContext());
	context->hostPath = hostPath;
	context->sessionId = hostId;
	for (wchar_t& ch : context->sessionId)
	{
		const bool ok = (ch >= L'0' && ch <= L'9') || (ch >= L'a' && ch <= L'z')
			|| (ch >= L'A' && ch <= L'Z') || ch == L'-' || ch == L'_';
		if (!ok)
			ch = L'_';
	}
	context->configPath = configFilePath;
	if (monitorStopEvent == NULL
		|| !duplicateMonitorHandle(monitorStopEvent, context->stopEvent)
		|| !duplicateMonitorHandle(shutdownEvent, context->shutdownEvent)
		|| !duplicateMonitorHandle(hostLeaseMutex, context->hostLeaseMutex)
		|| !duplicateMonitorHandle(hostHandoffEvent, context->hostHandoffEvent)
		|| !duplicateMonitorHandle(hostReadyEvent, context->hostReadyEvent))
	{
		logLaunchFailure(L"could not initialize named host monitor handles");
		closeHost();
		return false;
	}

	monitorThread = CreateThread(NULL, 0, namedHostMonitorProc, context.get(), 0, NULL);
	if (monitorThread == NULL)
	{
		logLaunchFailure(L"could not create named host monitor thread");
		closeHost();
		return false;
	}
	context.release();

	TraceF(L"OutProcVSTPlugin: supervising named host session %s with timeout %u ms", hostId.c_str(), timeoutMs);
	disabled = false;
	consecutiveTimeouts = 0;
	return true;
}

bool OutProcVSTPluginFilter::startInheritedHost()
{
	if (!writeConfigFile())
	{
		logLaunchFailure(L"could not write VST config file");
		return false;
	}

	SECURITY_ATTRIBUTES inheritableAttributes = {};
	inheritableAttributes.nLength = sizeof(inheritableAttributes);
	inheritableAttributes.bInheritHandle = TRUE;

	const DWORD mappingSizeHigh = static_cast<DWORD>(sharedByteCount >> 32);
	const DWORD mappingSizeLow = static_cast<DWORD>(sharedByteCount & 0xffffffff);
	mappingHandle = CreateFileMappingW(INVALID_HANDLE_VALUE, &inheritableAttributes, PAGE_READWRITE, mappingSizeHigh, mappingSizeLow, NULL);
	requestEvent = CreateEventW(&inheritableAttributes, FALSE, FALSE, NULL);
	responseEvent = CreateEventW(&inheritableAttributes, FALSE, FALSE, NULL);
	shutdownEvent = CreateEventW(&inheritableAttributes, TRUE, FALSE, NULL);

	if (mappingHandle == NULL || requestEvent == NULL || responseEvent == NULL || shutdownEvent == NULL)
	{
		logLaunchFailure(L"Win32 handle creation failed");
		closeHost();
		return false;
	}

	sharedHeader = static_cast<OutProcAudioHeader*>(MapViewOfFile(mappingHandle, FILE_MAP_ALL_ACCESS, 0, 0, sharedByteCount));
	if (sharedHeader == nullptr)
	{
		logLaunchFailure(L"shared memory map failed");
		closeHost();
		return false;
	}

	ZeroMemory(sharedHeader, sharedByteCount);
	sharedHeader->magic = OUTPROC_AUDIO_MAGIC;
	sharedHeader->version = OUTPROC_AUDIO_VERSION;
	sharedHeader->sampleRate = static_cast<uint32_t>(sampleRate);
	sharedHeader->channelCount = channelCount;
	sharedHeader->maxFrames = maxFrameCount;
	sharedHeader->dspType = OUTPROC_AUDIO_DSP_VST;

	const wstring hostPath = getHostPath();
	if (hostPath.empty())
	{
		logLaunchFailure(L"host path could not be resolved");
		closeHost();
		return false;
	}

	wstringstream commandLine;
	commandLine << L"\"" << hostPath << L"\""
		<< L" --mapping " << reinterpret_cast<uintptr_t>(mappingHandle)
		<< L" --request " << reinterpret_cast<uintptr_t>(requestEvent)
		<< L" --response " << reinterpret_cast<uintptr_t>(responseEvent)
		<< L" --shutdown " << reinterpret_cast<uintptr_t>(shutdownEvent)
		<< L" --vst-config \"" << configFilePath << L"\"";

	wstring commandLineStorage = commandLine.str();
	wstring workingDirectory = hostPath.substr(0, hostPath.find_last_of(L"\\/"));

	STARTUPINFOW startupInfo = {};
	startupInfo.cb = sizeof(startupInfo);
	PROCESS_INFORMATION processInformation = {};

	if (!CreateProcessW(hostPath.c_str(), &commandLineStorage[0], NULL, NULL, TRUE, CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS, NULL,
		workingDirectory.empty() ? NULL : workingDirectory.c_str(), &startupInfo, &processInformation))
	{
		logLaunchFailure(hostPath);
		closeHost();
		return false;
	}

	CloseHandle(processInformation.hThread);
	processHandle = processInformation.hProcess;
	disabled = false;
	consecutiveTimeouts = 0;
	TraceF(L"OutProcVSTPlugin: launched EqApoOutProcHost.exe for %s with timeout %u ms%s", vstConfig.libraryPath.c_str(), timeoutMs, analysisMode ? L" (analysis)" : L"");
	return true;
}

wstring OutProcVSTPluginFilter::makeObjectName(const wchar_t* suffix) const
{
	wstring safeId = hostId;
	for (wchar_t& ch : safeId)
	{
		const bool ok = (ch >= L'0' && ch <= L'9') || (ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z') || ch == L'-' || ch == L'_';
		if (!ok)
			ch = L'_';
	}
	return L"Global\\EqApoOutProcVST_" + safeId + L"_" + suffix;
}

bool OutProcVSTPluginFilter::createOpenSecurityAttributes(
	SECURITY_ATTRIBUTES& attributes, PSECURITY_DESCRIPTOR& securityDescriptor) const
{
	securityDescriptor = NULL;
	ZeroMemory(&attributes, sizeof(attributes));
	attributes.nLength = sizeof(attributes);
	attributes.bInheritHandle = FALSE;
	if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
		L"D:(A;;GA;;;WD)", SDDL_REVISION_1, &securityDescriptor, NULL))
	{
		if (securityDescriptor != NULL)
			LocalFree(securityDescriptor);
		securityDescriptor = NULL;
		return false;
	}
	attributes.lpSecurityDescriptor = securityDescriptor;
	return true;
}

bool OutProcVSTPluginFilter::writeConfigFile()
{
	if (!configFilePath.empty())
		DeleteFileW(configFilePath.c_str());

	wchar_t tempPath[MAX_PATH] = {};
	wchar_t tempFile[MAX_PATH] = {};
	if (GetTempPathW(MAX_PATH, tempPath) == 0)
		return false;
	if (GetTempFileNameW(tempPath, L"EAV", 0, tempFile) == 0)
		return false;

	configFilePath = tempFile;
	return OutProcWriteVSTConfig(configFilePath, vstConfig);
}

bool OutProcVSTPluginFilter::validateProtocol(unsigned frameCount) const
{
	return sharedHeader->magic == OUTPROC_AUDIO_MAGIC
		&& sharedHeader->version == OUTPROC_AUDIO_VERSION
		&& sharedHeader->channelCount == channelCount
		&& sharedHeader->maxFrames == maxFrameCount
		&& sharedHeader->frameCount == frameCount
		&& sharedHeader->responseSeq == requestSeq
		&& sharedHeader->status == OUTPROC_AUDIO_STATUS_OK;
}

void OutProcVSTPluginFilter::bypass(double** output, double** input, unsigned frameCount) const
{
	for (unsigned channel = 0; channel < channelCount; ++channel)
		for (unsigned frame = 0; frame < frameCount; ++frame)
			output[channel][frame] = input[channel][frame];

	const_cast<OutProcVSTPluginFilter*>(this)->wasBypassed = true;
}

void OutProcVSTPluginFilter::logLaunchFailure(const wstring& detail)
{
	if (!launchFailureLogged)
	{
		LogF(L"OutProcVSTPlugin: host launch failed: %s", detail.c_str());
		launchFailureLogged = true;
	}
}

void OutProcVSTPluginFilter::logTimeout()
{
	if (!timeoutLogged)
	{
		LogF(L"OutProcVSTPlugin: host timeout, bypassing audio");
		timeoutLogged = true;
	}
}

void OutProcVSTPluginFilter::logHostExited()
{
	if (!exitedLogged)
	{
		LogF(L"OutProcVSTPlugin: host exited, bypassing audio");
		exitedLogged = true;
	}
}

void OutProcVSTPluginFilter::logProtocolMismatch()
{
	if (!protocolLogged)
	{
		LogF(L"OutProcVSTPlugin: protocol mismatch, bypassing audio");
		protocolLogged = true;
	}
}

void OutProcVSTPluginFilter::disableAfterFailure(const wstring& reason)
{
	if (!disabled)
		LogF(L"OutProcVSTPlugin: disabling out-of-process host after %s; audio will bypass until the configuration is reloaded", reason.c_str());

	disabled = true;
	if (processHandle != NULL)
	{
		TerminateProcess(processHandle, 1);
		CloseHandle(processHandle);
		processHandle = NULL;
	}
}

void OutProcVSTPluginFilter::logRecovered()
{
	TraceF(L"OutProcVSTPlugin: host recovered");
	timeoutLogged = false;
	protocolLogged = false;
}

wstring OutProcVSTPluginFilter::getHostPath() const
{
	HMODULE moduleHandle = NULL;
	const BOOL gotModule = GetModuleHandleExW(
		GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		reinterpret_cast<LPCWSTR>(&outProcVSTModuleAnchor),
		&moduleHandle);

	if (!gotModule || moduleHandle == NULL)
		return L"";

	wchar_t modulePath[MAX_PATH] = {};
	const DWORD length = GetModuleFileNameW(moduleHandle, modulePath, MAX_PATH);
	if (length == 0 || length == MAX_PATH)
		return L"";

	wstring path(modulePath, length);
	const size_t slash = path.find_last_of(L"\\/");
	if (slash == wstring::npos)
		return L"EqApoOutProcHost.exe";

	return path.substr(0, slash + 1) + L"EqApoOutProcHost.exe";
}
