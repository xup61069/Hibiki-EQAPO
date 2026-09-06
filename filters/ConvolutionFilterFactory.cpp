/*
    This file is part of EqualizerAPO, a system-wide equalizer.
    Copyright (C) 2015  Jonas Thedering

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
#include <exception>
#include <filesystem>

#include "helpers/MemoryHelper.h"
#include "helpers/StringHelper.h"
#include "helpers/LogHelper.h"
#include "ConvolutionFilter.h"
#include "ConvolutionFilterFactory.h"

using namespace std;

vector<IFilter*> ConvolutionFilterFactory::createFilter(const wstring& configPath, wstring& command, wstring& parameters)
{
	ConvolutionFilter* filter = NULL;

	if (command == L"Convolution")
	{
		const wstring value = StringHelper::trim(parameters);
		if (value.empty())
			return vector<IFilter*>();

		try
		{
			const std::filesystem::path configuredPath(value);
			const std::filesystem::path absolutePath = configuredPath.is_relative()
				? (std::filesystem::path(configPath).parent_path() /
					configuredPath).lexically_normal()
				: configuredPath;

			filter = constructFilter<ConvolutionFilter>(absolutePath.wstring());
		}
		catch (const std::bad_alloc&)
		{
			throw;
		}
		catch (const std::exception&)
		{
			LogF(L"Could not resolve convolution path");
		}
	}

	return adoptFilter(filter);
}
