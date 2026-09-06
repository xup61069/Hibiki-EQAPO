#include "stdafx.h"
#include "helpers/MemoryHelper.h"
#include "helpers/StringHelper.h"
#include "ChorusFilter.h"
#include "ChorusFilterFactory.h"

using namespace std;

vector<IFilter*> ChorusFilterFactory::createFilter(const wstring& configPath, wstring& command, wstring& parameters)
{
	if (command != L"Chorus")
		return vector<IFilter*>();

	double rate = 0.4;
	double depth = 8.0;
	double mix = 25.0;
	double feedback = 0.0;
	vector<wstring> parts = StringHelper::splitQuoted(StringHelper::replaceCharacters(parameters, L",", L"."), ' ');
	// Unit tokens written by the Editor (Hz, ms and %) are optional. Scan for
	// every named field so both the unit-bearing format and the legacy compact
	// "key value" format round-trip to the same runtime values.
	for (unsigned i = 0; i + 1 < parts.size(); ++i)
	{
		if (parts[i] == L"Rate")
			rate = wcstod(parts[i + 1].c_str(), NULL);
		else if (parts[i] == L"Depth")
			depth = wcstod(parts[i + 1].c_str(), NULL);
		else if (parts[i] == L"Mix")
			mix = wcstod(parts[i + 1].c_str(), NULL);
		else if (parts[i] == L"Feedback")
			feedback = wcstod(parts[i + 1].c_str(), NULL);
	}

	return adoptFilter(constructFilter<ChorusFilter>(
		rate, depth, mix, feedback));
}
