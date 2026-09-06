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
#include "helpers/StringHelper.h"
#include "helpers/VSTDiagnostics.h"
#include "helpers/VSTParameterParser.h"
#include "helpers/VSTPluginLibrary.h"
#include "helpers/LogHelper.h"
#include "VSTPluginFilter.h"
#include "VSTPluginFilterFactory.h"

using namespace std;

vector<IFilter*> VSTPluginFilterFactory::createFilter(const wstring& configPath, wstring& command, wstring& parameters)
{
	VSTPluginFilter* filter = NULL;

	if (command == L"VSTPlugin")
	{
		shared_ptr<VSTPluginLibrary> library;
		wstring chunkData;
		wstring midiConfig;
		int vst3ClassIndex = 0;
		unordered_map<wstring, float> paramMap;
		vector<wstring> parts = StringHelper::splitQuoted(parameters, ' ');
		for (size_t i = 0; i < parts.size();)
		{
			if (i + 1 >= parts.size())
			{
				LogF(L"VSTPlugin: ignoring trailing token %s", parts[i].c_str());
				break;
			}

			wstring key = parts[i];
			wstring value = parts[i + 1];

			if (key == L"Library")
			{
				wstring libPath;
				if (PathIsRelativeW(value.c_str()))
				{
					wchar_t filePath[MAX_PATH];
					wstring pluginPath = VSTPluginLibrary::getDefaultPluginPath();
					pluginPath._Copy_s(filePath, sizeof(filePath) / sizeof(wchar_t), MAX_PATH);
					if (pluginPath.size() < MAX_PATH)
						filePath[pluginPath.size()] = L'\0';
					else
						filePath[MAX_PATH - 1] = L'\0';
					PathAppendW(filePath, value.c_str());
					libPath = filePath;
				}
				else
					libPath = value;

				library = VSTPluginLibrary::getInstance(libPath);
				i += 2;
			}
			else if (key == L"ChunkData")
			{
				chunkData = value;
				i += 2;
			}
			else if (key == L"ClassIndex")
			{
				vst3ClassIndex = _wtoi(value.c_str());
				i += 2;
			}
			else if (key == L"MidiConfig")
			{
				midiConfig = value;
				i += 2;
			}
			else if (key == L"Engine")
			{
				// Legacy configs may contain an Engine token from experimental hosts.
				i += 2;
			}
			else
			{
				if (!VSTConsumeParameter(parts, i, paramMap))
				{
					LogF(L"VSTPlugin: ignoring malformed parameter %s %s", key.c_str(), value.c_str());
					// Advance one token so a valid field after an unknown or malformed
					// token can become the next key instead of being skipped with it.
					++i;
				}
			}
		}

		if (library != NULL)
		{
			bool create = true;
			if (configPath != L"")
			{
				create = false;
				TraceF(L"Adding VST plugin %s", library->getLibPath().c_str());
				int res = library->initialize();
				if (res < 0)
				{
					VSTDiag(L"in-process VST load failed path=%s result=%d", library->getLibPath().c_str(), res);
#ifdef _WIN64
					int bitDepth = 64;
#else
					int bitDepth = 32;
#endif
					if (res == AbstractLibrary::FILE_NOT_FOUND)
						LogF(L"File %s not found", library->getLibPath().c_str());
					else if (res == AbstractLibrary::LOADING_FAILED)
						LogF(L"Library %s could not be loaded", library->getLibPath().c_str());
					else if (res == AbstractLibrary::FUNCTIONS_MISSING)
						LogF(L"Library %s does not contain needed functions", library->getLibPath().c_str());
					else if (res == AbstractLibrary::WRONG_ARCHITECTURE)
						LogF(L"Library %s has wrong architecture, must be %d-bit", library->getLibPath().c_str(), bitDepth);
					else if (res == AbstractLibrary::RECURSIVE_LOADING)
						LogF(L"Recursive VST library load rejected for %s", library->getLibPath().c_str());
				}
				else
				{
					create = true;
				}
			}

			if (create)
			{
				filter = constructFilter<VSTPluginFilter>(
					library, chunkData, paramMap, vst3ClassIndex, midiConfig);
			}
		}
	}

	return adoptFilter(filter);
}
