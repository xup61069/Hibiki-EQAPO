/*
    This file is part of Equalizer APO, a system-wide equalizer.
    Copyright (C) 2017  Jonas Thedering

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
*/

#include "stdafx.h"

#include "helpers/LogHelper.h"
#include "helpers/MemoryHelper.h"
#include "helpers/StringHelper.h"
#include "helpers/VSTParameterParser.h"
#include "helpers/VSTPluginLibrary.h"
#include "FilterEngine.h"
#include "OutProcVSTPluginFilter.h"
#include "OutProcVSTPluginFilterFactory.h"

using namespace std;

void OutProcVSTPluginFilterFactory::initialize(FilterEngine* engine)
{
	this->engine = engine;
}

vector<IFilter*> OutProcVSTPluginFilterFactory::createFilter(const wstring& configPath, wstring& command, wstring& parameters)
{
	OutProcVSTPluginFilter* filter = NULL;

	if (command == L"OutProcVSTPlugin")
	{
		wstring libPath;
		wstring chunkData;
		wstring hostId;
		wstring midiConfig;
		int vst3ClassIndex = 0;
		unordered_map<wstring, float> paramMap;
		vector<wstring> parts = StringHelper::splitQuoted(parameters, ' ');
		for (size_t i = 0; i < parts.size();)
		{
			if (i + 1 >= parts.size())
			{
				LogF(L"OutProcVSTPlugin: ignoring trailing token %s", parts[i].c_str());
				break;
			}

			wstring key = parts[i];
			wstring value = parts[i + 1];

			if (key == L"Library")
			{
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
				i += 2;
			}
			else if (key == L"ChunkData")
			{
				chunkData = value;
				i += 2;
			}
			else if (key == L"HostId")
			{
				hostId = value;
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
				// Reserved for compatibility with experimental editor output.
				i += 2;
			}
			else
			{
				if (!VSTConsumeParameter(parts, i, paramMap))
				{
					LogF(L"OutProcVSTPlugin: ignoring malformed parameter %s %s", key.c_str(), value.c_str());
					// Advance one token so a valid field after an unknown or malformed
					// token can become the next key instead of being skipped with it.
					++i;
				}
			}
		}

		if (!libPath.empty())
		{
			if (VSTPluginLibrary::isCurrentModulePath(libPath))
			{
				LogF(L"Recursive out-of-process VST library load rejected for %s",
					libPath.c_str());
			}
			else
			{
				TraceF(L"Adding out-of-process VST plugin %s", libPath.c_str());
				filter = constructFilter<OutProcVSTPluginFilter>(
					libPath, chunkData, paramMap, hostId,
					engine != nullptr && engine->isAnalysisMode(),
					vst3ClassIndex, midiConfig);
			}
		}
	}

	return adoptFilter(filter);
}
