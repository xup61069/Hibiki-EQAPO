/*
    This file is part of Equalizer APO, a system-wide equalizer.
    Copyright (C) 2017  Alexander Walch
    Enhanced with robust endpoint tracking for loudness correction.
*/

#include "stdafx.h"
#include "VolumeController.h"
#include <mmdeviceapi.h>
#include <algorithm>
#include <cmath>

namespace
{
	// PKEY_AudioEndpoint_GUID. The value is a device identifier, not an opaque
	// IMMDevice endpoint ID, so resolve it by enumeration and then call GetId().
	const PROPERTYKEY ENDPOINT_GUID_PROPERTY = {
		{0x1da5d803, 0xd492, 0x4edd,
			{0x8c, 0x23, 0xe0, 0xc0, 0xff, 0xee, 0x7f, 0x0e}},
		4
	};

	// {C4A5D182-3A64-4217-916B-41DE8C3B87A0}
	const GUID HIBIKI_VOLUME_EVENT_CONTEXT =
		{ 0xc4a5d182, 0x3a64, 0x4217, { 0x91, 0x6b, 0x41, 0xde, 0x8c, 0x3b, 0x87, 0xa0 } };

	HRESULT findDeviceByEndpointGuid(
		IMMDeviceEnumerator* enumerator,
		const std::wstring& endpointGuid,
		IMMDevice** result)
	{
		if (enumerator == NULL || result == NULL)
			return E_POINTER;
		*result = NULL;

		IMMDeviceCollection* collection = NULL;
		HRESULT hr = enumerator->EnumAudioEndpoints(
			eRender, DEVICE_STATEMASK_ALL, &collection);
		if (FAILED(hr) || collection == NULL)
			return hr;

		UINT count = 0;
		hr = collection->GetCount(&count);
		for (UINT index = 0; SUCCEEDED(hr) && index < count; ++index)
		{
			IMMDevice* candidate = NULL;
			HRESULT itemResult = collection->Item(index, &candidate);
			if (FAILED(itemResult) || candidate == NULL)
				continue;

			IPropertyStore* properties = NULL;
			HRESULT propertyResult = candidate->OpenPropertyStore(
				STGM_READ, &properties);
			PROPVARIANT value;
			PropVariantInit(&value);
			if (SUCCEEDED(propertyResult) && properties != NULL)
				propertyResult = properties->GetValue(ENDPOINT_GUID_PROPERTY, &value);

			bool matches = SUCCEEDED(propertyResult) && value.vt == VT_LPWSTR &&
				value.pwszVal != NULL &&
				_wcsicmp(value.pwszVal, endpointGuid.c_str()) == 0;
			PropVariantClear(&value);
			if (properties != NULL)
				properties->Release();

			if (matches)
			{
				*result = candidate;
				collection->Release();
				return S_OK;
			}
			candidate->Release();
		}

		collection->Release();
		return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
	}
}

class EndpointVolumeCallback : public IAudioEndpointVolumeCallback
{
public:
	EndpointVolumeCallback()
		: _refCount(1), _changed(true) {}

	ULONG STDMETHODCALLTYPE AddRef()
	{
		return InterlockedIncrement(&_refCount);
	}

	ULONG STDMETHODCALLTYPE Release()
	{
		ULONG ulRef = InterlockedDecrement(&_refCount);
		if (ulRef == 0)
		{
			delete this;
		}
		return ulRef;
	}

	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, VOID** ppvInterface)
	{
		if (!ppvInterface)
			return E_POINTER;

		*ppvInterface = NULL;
		if (IsEqualIID(riid, __uuidof(IUnknown)))
		{
			AddRef();
			*ppvInterface = static_cast<IUnknown*>(this);
			return S_OK;
		}
		if (IsEqualIID(riid, __uuidof(IAudioEndpointVolumeCallback)))
		{
			AddRef();
			*ppvInterface = static_cast<IAudioEndpointVolumeCallback*>(this);
			return S_OK;
		}
		return E_NOINTERFACE;
	}

	HRESULT STDMETHODCALLTYPE OnNotify(PAUDIO_VOLUME_NOTIFICATION_DATA pNotify)
	{
		if (pNotify && !IsEqualGUID(pNotify->guidEventContext, HIBIKI_VOLUME_EVENT_CONTEXT))
			_changed.store(true, std::memory_order_release);
		return S_OK;
	}

	bool consumeChanged()
	{
		return _changed.exchange(false, std::memory_order_acq_rel);
	}

private:
	long _refCount;
	// The endpoint can retain this COM object if unregistering fails. Keep the
	// notification state inside the callback so a delayed OnNotify never writes
	// through a pointer to an already-destroyed VolumeController.
	std::atomic<bool> _changed;
};

VolumeController::VolumeController(const std::wstring& endpointId)
	: _endpointVolume(NULL),
	  _callback(NULL),
	  _minVol(-65.0f),
	  _maxVol(0.0f),
	  _comInitialized(false),
	  _volumeChanged(true),
	  _lastVolume(0.0),
	  _requestedEndpointId(endpointId),
	  _nextEndpointCheck(0)
{
	HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
	if (SUCCEEDED(hr))
	{
		_comInitialized = true;
	}
	else if (hr == RPC_E_CHANGED_MODE)
	{
		// COM already initialized on this thread with different apartment model; valid
		_comInitialized = false;
	}
	initEndpoint();
}

VolumeController::~VolumeController()
{
	cleanup();
	if (_comInitialized)
	{
		CoUninitialize();
		_comInitialized = false;
	}
}

void VolumeController::cleanup()
{
	if (_endpointVolume)
	{
		if (_callback)
		{
			_endpointVolume->UnregisterControlChangeNotify(_callback);
			_callback->Release();
			_callback = NULL;
		}
		_endpointVolume->Release();
		_endpointVolume = NULL;
	}
	else if (_callback)
	{
		_callback->Release();
		_callback = NULL;
	}
	_endpointId.clear();
}

bool VolumeController::initEndpoint()
{
	IMMDeviceEnumerator* deviceEnumerator = NULL;
	HRESULT hr = CoCreateInstance(
		__uuidof(MMDeviceEnumerator),
		NULL,
		CLSCTX_INPROC_SERVER,
		__uuidof(IMMDeviceEnumerator),
		reinterpret_cast<LPVOID*>(&deviceEnumerator));
	if (FAILED(hr) || !deviceEnumerator)
		return false;

	IMMDevice* device = NULL;
	if (_requestedEndpointId.empty())
		hr = deviceEnumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &device);
	else
	{
		// Accept a true opaque endpoint ID when one is available. The APO and
		// editor normally provide PKEY_AudioEndpoint_GUID, which must be resolved
		// without assuming an endpoint-ID string format.
		hr = deviceEnumerator->GetDevice(_requestedEndpointId.c_str(), &device);
		if (FAILED(hr) || device == NULL)
			hr = findDeviceByEndpointGuid(
				deviceEnumerator, _requestedEndpointId, &device);
	}
	deviceEnumerator->Release();
	if (FAILED(hr) || !device)
		return false;

	LPWSTR endpointId = NULL;
	hr = device->GetId(&endpointId);
	if (FAILED(hr) || !endpointId)
	{
		device->Release();
		return false;
	}

	IAudioEndpointVolume* endpointVolume = NULL;
	hr = device->Activate(
		__uuidof(IAudioEndpointVolume),
		CLSCTX_INPROC_SERVER,
		NULL,
		reinterpret_cast<LPVOID*>(&endpointVolume));
	device->Release();
	if (FAILED(hr) || !endpointVolume)
	{
		CoTaskMemFree(endpointId);
		return false;
	}

	float minimumVolume = -65.0f;
	float maximumVolume = 0.0f;
	float increment = 0.0f;
	endpointVolume->GetVolumeRange(&minimumVolume, &maximumVolume, &increment);

	EndpointVolumeCallback* callback = new EndpointVolumeCallback();
	if (FAILED(endpointVolume->RegisterControlChangeNotify(callback)))
	{
		callback->Release();
		callback = NULL;
	}

	std::wstring newEndpointId(endpointId);
	CoTaskMemFree(endpointId);
	cleanup();
	_endpointVolume = endpointVolume;
	_callback = callback;
	_minVol = minimumVolume;
	_maxVol = maximumVolume;
	_endpointId = newEndpointId;
	_nextEndpointCheck = GetTickCount64() + 2000;
	_volumeChanged.store(true, std::memory_order_relaxed);

	return true;
}

bool VolumeController::refreshEndpointIfChanged()
{
	// A runtime filter is bound to one APO endpoint. It must never follow the
	// system default when that default changes.
	if (!_requestedEndpointId.empty())
		return true;

	ULONGLONG now = GetTickCount64();
	if (now < _nextEndpointCheck)
		return _endpointVolume != NULL;
	_nextEndpointCheck = now + 2000;

	IMMDeviceEnumerator* deviceEnumerator = NULL;
	HRESULT hr = CoCreateInstance(
		__uuidof(MMDeviceEnumerator),
		NULL,
		CLSCTX_INPROC_SERVER,
		__uuidof(IMMDeviceEnumerator),
		reinterpret_cast<LPVOID*>(&deviceEnumerator));
	if (FAILED(hr) || !deviceEnumerator)
	{
		cleanup();
		return false;
	}

	IMMDevice* defaultDevice = NULL;
	hr = deviceEnumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &defaultDevice);
	deviceEnumerator->Release();
	if (FAILED(hr) || !defaultDevice)
	{
		cleanup();
		return false;
	}

	LPWSTR endpointId = NULL;
	hr = defaultDevice->GetId(&endpointId);
	defaultDevice->Release();
	if (FAILED(hr) || !endpointId)
	{
		cleanup();
		return false;
	}

	bool changed = _endpointId != endpointId;
	CoTaskMemFree(endpointId);
	if (changed)
	{
		// Never keep reading the previous Windows default after a failed
		// rebind. Clearing it first makes the caller fail closed to bypass.
		cleanup();
		return initEndpoint();
	}
	return _endpointVolume != NULL;
}

HRESULT VolumeController::getVolume(double& currentVolume)
{
	if (!refreshEndpointIfChanged())
	{
		currentVolume = _lastVolume;
		return E_FAIL;
	}
	if (!_endpointVolume)
	{
		if (!initEndpoint())
		{
			currentVolume = _lastVolume;
			return E_FAIL;
		}
	}

	float vol = 0.0f;
	HRESULT res = _endpointVolume->GetMasterVolumeLevel(&vol);
	if (FAILED(res))
	{
		cleanup();
		if (initEndpoint())
			res = _endpointVolume->GetMasterVolumeLevel(&vol);
		if (FAILED(res))
			cleanup();
	}
	if (SUCCEEDED(res))
	{
		currentVolume = vol;
		_lastVolume = vol;
		return S_OK;
	}

	currentVolume = _lastVolume;
	return res;
}

HRESULT VolumeController::getVolumeState(EndpointVolumeState& state)
{
	if (!refreshEndpointIfChanged())
		return E_FAIL;
	if (_endpointVolume == NULL && !initEndpoint())
		return E_FAIL;

	HRESULT result = E_FAIL;
	for (unsigned attempt = 0; attempt < 4; ++attempt)
	{
		float firstDb = 0.0f;
		float firstScalar = 1.0f;
		BOOL firstMuted = FALSE;
		float secondDb = 0.0f;
		float secondScalar = 1.0f;
		BOOL secondMuted = FALSE;

		result = _endpointVolume->GetMasterVolumeLevel(&firstDb);
		if (SUCCEEDED(result))
			result = _endpointVolume->GetMasterVolumeLevelScalar(&firstScalar);
		if (SUCCEEDED(result))
			result = _endpointVolume->GetMute(&firstMuted);
		if (SUCCEEDED(result))
			result = _endpointVolume->GetMasterVolumeLevel(&secondDb);
		if (SUCCEEDED(result))
			result = _endpointVolume->GetMasterVolumeLevelScalar(&secondScalar);
		if (SUCCEEDED(result))
			result = _endpointVolume->GetMute(&secondMuted);

		if (FAILED(result))
		{
			cleanup();
			// Match getVolume's one reconnect attempt for transient endpoint
			// invalidation, but never publish a partially collected tuple.
			if (attempt == 0 && initEndpoint())
				continue;
			return result;
		}

		if (!std::isfinite(firstDb) || !std::isfinite(firstScalar) ||
			!std::isfinite(secondDb) || !std::isfinite(secondScalar))
		{
			cleanup();
			return E_FAIL;
		}

		// Core Audio exposes these values through separate getters. Collect the
		// tuple twice and accept it only when both generations agree, preventing
		// correction dB and follow scalar/mute from describing different moments
		// while the user drags the volume slider.
		if (std::abs(firstDb - secondDb) <= 1.0e-4f &&
			std::abs(firstScalar - secondScalar) <= 1.0e-6f &&
			firstMuted == secondMuted)
		{
			state.levelDb = secondDb;
			state.scalar = (std::max)(0.0,
				(std::min)(1.0, static_cast<double>(secondScalar)));
			state.muted = secondMuted != FALSE;
			_lastVolume = secondDb;
			return S_OK;
		}

		result = HRESULT_FROM_WIN32(ERROR_RETRY);
	}

	return result;
}

HRESULT VolumeController::setVolume(double volume)
{
	if (!refreshEndpointIfChanged())
		return E_FAIL;
	if (!_endpointVolume)
	{
		if (!initEndpoint())
		{
			return E_FAIL;
		}
	}
	volume = (std::min)(volume, static_cast<double>(_maxVol));
	volume = (std::max)(volume, static_cast<double>(_minVol));
	HRESULT result = _endpointVolume->SetMasterVolumeLevel(
		(float)volume, const_cast<GUID*>(&HIBIKI_VOLUME_EVENT_CONTEXT));
	if (FAILED(result))
		cleanup();
	return result;
}

HRESULT VolumeController::setVolumeScalar(double scalar)
{
	if (!refreshEndpointIfChanged())
		return E_FAIL;
	if (!_endpointVolume)
	{
		if (!initEndpoint())
		{
			return E_FAIL;
		}
	}
	scalar = (std::min)(1.0, (std::max)(0.0, scalar));
	HRESULT result = _endpointVolume->SetMasterVolumeLevelScalar(
		(float)scalar, const_cast<GUID*>(&HIBIKI_VOLUME_EVENT_CONTEXT));
	if (FAILED(result))
		cleanup();
	return result;
}

HRESULT VolumeController::setMute(bool mute)
{
	if (!refreshEndpointIfChanged())
		return E_FAIL;
	if (!_endpointVolume)
	{
		if (!initEndpoint())
		{
			return E_FAIL;
		}
	}
	HRESULT result = _endpointVolume->SetMute(
		mute ? TRUE : FALSE, const_cast<GUID*>(&HIBIKI_VOLUME_EVENT_CONTEXT));
	if (FAILED(result))
		cleanup();
	return result;
}

HRESULT VolumeController::getMute(bool& mute)
{
	if (!refreshEndpointIfChanged())
		return E_FAIL;
	if (!_endpointVolume && !initEndpoint())
		return E_FAIL;

	BOOL muted = FALSE;
	HRESULT result = _endpointVolume->GetMute(&muted);
	if (SUCCEEDED(result))
		mute = (muted != FALSE);
	return result;
}

HRESULT VolumeController::getVolumeScalar(double& scalar)
{
	if (!refreshEndpointIfChanged())
		return E_FAIL;
	if (!_endpointVolume && !initEndpoint())
		return E_FAIL;

	float s = 1.0f;
	HRESULT result = _endpointVolume->GetMasterVolumeLevelScalar(&s);
	if (SUCCEEDED(result))
		scalar = static_cast<double>(s);
	return result;
}

HRESULT VolumeController::getVolumeRange(float& minDb, float& maxDb, float& stepDb)
{
	if (!refreshEndpointIfChanged())
		return E_FAIL;
	if (!_endpointVolume && !initEndpoint())
		return E_FAIL;

	minDb = _minVol;
	maxDb = _maxVol;
	stepDb = 0.5f;
	return S_OK;
}

const GUID& VolumeController::getEventContextGuid()
{
	return HIBIKI_VOLUME_EVENT_CONTEXT;
}

bool VolumeController::hasVolumeChanged()
{
	bool changed = _volumeChanged.exchange(false, std::memory_order_acq_rel);
	if (_callback != NULL)
		changed = _callback->consumeChanged() || changed;
	return changed;
}
