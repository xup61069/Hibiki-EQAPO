/*
    This file is part of Equalizer APO, a system-wide equalizer.
    Copyright (C) 2017  Alexander Walch
    Enhanced with robust endpoint tracking for loudness correction.
*/

#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <EndpointVolume.h>
#include <atomic>
#include <string>

class EndpointVolumeCallback;

struct EndpointVolumeState
{
	double levelDb = 0.0;
	double scalar = 1.0;
	bool muted = false;
};

class VolumeController
{
public:
	explicit VolumeController(const std::wstring& endpointId = L"");
	~VolumeController();
	HRESULT getVolume(double& currentVolume);
	HRESULT getVolumeState(EndpointVolumeState& state);
	HRESULT setVolume(double volume);
	HRESULT setVolumeScalar(double scalar);
	HRESULT setMute(bool mute);
	HRESULT getMute(bool& mute);
	HRESULT getVolumeScalar(double& scalar);
	HRESULT getVolumeRange(float& minDb, float& maxDb, float& stepDb);
	static const GUID& getEventContextGuid();
	bool hasVolumeChanged();
	const std::wstring& getEndpointId() const { return _endpointId; }

private:
	bool initEndpoint();
	bool refreshEndpointIfChanged();
	void cleanup();

	IAudioEndpointVolume* _endpointVolume;
	EndpointVolumeCallback* _callback;
	float _minVol;
	float _maxVol;
	bool _comInitialized;
	std::atomic<bool> _volumeChanged;
	double _lastVolume;
	std::wstring _requestedEndpointId;
	std::wstring _endpointId;
	ULONGLONG _nextEndpointCheck;
};
