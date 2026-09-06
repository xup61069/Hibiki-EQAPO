/*
    This file is part of EqualizerAPO, a system-wide equalizer.
    Copyright (C) 2012  Jonas Thedering

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
#include <cstdarg>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "RegistryHelper.h"
#include "LogHelper.h"

using namespace std;

bool LogHelper::initialized = false;
wstring LogHelper::logPath;
bool LogHelper::enableTrace = false;
FILE* LogHelper::presetFP = NULL;
bool LogHelper::compact = false;
bool LogHelper::useConsoleColors = false;

void LogHelper::log(const char* file, int line, const void* caller, bool trace, const wchar_t* format, ...)
{
	wstring initializationError;
	wstring activeLogPath;
	bool activeEnableTrace = false;
	FILE* activePresetFP = NULL;
	bool activeCompact = false;
	bool activeUseConsoleColors = false;
	{
		std::lock_guard<std::mutex> stateLock(stateMutex());
		if (!initialized)
		{
			// Do not try to initialize again, even in case of error.
			initialized = true;

			wchar_t temp[255] = {};
			GetTempPathW(sizeof(temp) / sizeof(wchar_t), temp);

			logPath = temp;
			logPath += L"EqualizerAPO.log";

			try
			{
				if (RegistryHelper::readValue(APP_REGPATH, L"EnableTrace") != L"false")
					enableTrace = true;
			}
			catch (const RegistryException& e)
			{
				initializationError = e.getMessage();
			}
		}

		activeLogPath = logPath;
		activeEnableTrace = enableTrace;
		activePresetFP = presetFP;
		activeCompact = compact;
		activeUseConsoleColors = useConsoleColors;
	}

	if (!initializationError.empty())
		LogHelper::log(__FILE__, __LINE__, NULL, false, L"%s", initializationError.c_str());

	if (trace && !activeEnableTrace)
		return;

	std::lock_guard<std::mutex> outputLock(outputMutex());
	FILE* fp = NULL;
	if (activePresetFP == NULL)
	{
		errno_t err = _wfopen_s(&fp, activeLogPath.c_str(), L"at");
		if (err != 0 || fp == NULL)
			return;
	}
	else
	{
		fp = activePresetFP;
	}

	if (activeUseConsoleColors)
	{
		HANDLE con = GetStdHandle(STD_OUTPUT_HANDLE);
		if (trace)
			SetConsoleTextAttribute(con, 2);// Set console color to green
		else
			SetConsoleTextAttribute(con, 12);// Set console color to red
	}

	if (!activeCompact)
	{
		SYSTEMTIME ___st;
		GetLocalTime(&___st);
		DWORD threadId = GetCurrentThreadId();
		fwprintf(fp, L"%04d-%02d-%02d %02d:%02d:%02d.%03d %d %08X (%S:%d): ",
			___st.wYear, ___st.wMonth, ___st.wDay, ___st.wHour, ___st.wMinute, ___st.wSecond, ___st.wMilliseconds, threadId, (DWORD)(unsigned long long)caller, file, line);
	}

	if (trace)
		fwprintf(fp, L"(TRACE) ");

	va_list varArgs;
	va_start(varArgs, format);
	vfwprintf(fp, format, varArgs);
	va_end(varArgs);

	fwprintf(fp, L"\n");

	if (activeUseConsoleColors)
	{
		HANDLE con = GetStdHandle(STD_OUTPUT_HANDLE);
		SetConsoleTextAttribute(con, 7); // Set console color to light grey (default)
	}

	if (activePresetFP == NULL)
		fclose(fp);
	else
		fflush(fp);
}

void LogHelper::reset()
{
	std::lock_guard<std::mutex> stateLock(stateMutex());
	initialized = false;
	logPath = L"";
	enableTrace = false;
	presetFP = NULL;
	compact = false;
	useConsoleColors = false;
}

void LogHelper::set(FILE* fp, bool enableTrace, bool compact, bool useConsoleColors)
{
	std::lock_guard<std::mutex> stateLock(stateMutex());
	LogHelper::initialized = true;

	LogHelper::presetFP = fp;
	LogHelper::enableTrace = enableTrace;
	LogHelper::compact = compact;
	LogHelper::useConsoleColors = useConsoleColors;
}

std::mutex& LogHelper::stateMutex()
{
	static std::mutex mutex;
	return mutex;
}

std::mutex& LogHelper::outputMutex()
{
	static std::mutex mutex;
	return mutex;
}
