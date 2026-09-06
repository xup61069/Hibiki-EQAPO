#include "stdafx.h"
#include "helpers/MemoryHelper.h"
#include "helpers/StringHelper.h"
#include "VUMeterFilter.h"
#include "VUMeterFilterFactory.h"

using namespace std;

vector<IFilter*> VUMeterFilterFactory::createFilter(const wstring& configPath, wstring& command, wstring& parameters)
{
	if (command != L"VUMeter")
		return vector<IFilter*>();

	wstring meterId = L"default";
	wstring channels = L"all";
	wstring rmsStandard = L"AES17";
	wstring lufsStandard = L"ITU-R BS.1770-5";
	vector<wstring> parts = StringHelper::splitQuoted(parameters, ' ');
	for (unsigned i = 0; i + 1 < parts.size(); i += 2)
	{
		const wstring key = StringHelper::toLowerCase(parts[i]);
		if (key == L"meterid")
			meterId = parts[i + 1];
		else if (key == L"channels")
			channels = parts[i + 1];
		else if (key == L"rms" || key == L"rmsstandard")
			rmsStandard = parts[i + 1];
		else if (key == L"lufs" || key == L"lufsstandard")
			lufsStandard = parts[i + 1];
	}

	return adoptFilter(constructFilter<VUMeterFilter>(
		meterId, channels, rmsStandard, lufsStandard));
}
