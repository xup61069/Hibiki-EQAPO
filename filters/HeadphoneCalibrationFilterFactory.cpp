#include "stdafx.h"
#include "helpers/MemoryHelper.h"
#include "HeadphoneCalibrationFilter.h"
#include "HeadphoneCalibrationFilterFactory.h"

using namespace std;

vector<IFilter*> HeadphoneCalibrationFilterFactory::createFilter(const wstring& configPath, wstring& command, wstring& parameters)
{
	if (command != L"HeadphoneCalibration")
		return vector<IFilter*>();

	return adoptFilter(constructFilter<HeadphoneCalibrationFilter>());
}
