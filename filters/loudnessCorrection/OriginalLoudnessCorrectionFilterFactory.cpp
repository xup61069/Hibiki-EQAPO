/*
    This file is part of Equalizer APO, a system-wide equalizer.
    Copyright (C) 2017  Alexander Walch
    Copyright (C) 2026  Equalizer APO contributors

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
*/

#include "stdafx.h"
#include "OriginalLoudnessCorrectionFilterFactory.h"
#include "OriginalLoudnessCorrectionFilter.h"
#include "helpers/LogHelper.h"
#include "helpers/MemoryHelper.h"

std::vector<IFilter*> OriginalLoudnessCorrectionFilterFactory::createFilter(
	const std::wstring& configPath,
	std::wstring& command,
	std::wstring& parameters)
{
	(void)configPath;
	std::vector<IFilter*> filters;
	const bool isOriginalCommand =
		command == L"LoudnessCorrectionOriginal";
	if (!isOriginalCommand)
		return filters;

	OriginalLoudnessCorrectionFilter::FilterParameters filterParameters(
		parameters);
	if (!filterParameters.isInitialized())
		return filters;

	TraceF(L"Adding original loudness correction filter");
	return adoptFilter(constructFilter<OriginalLoudnessCorrectionFilter>(
		filterParameters));
}
