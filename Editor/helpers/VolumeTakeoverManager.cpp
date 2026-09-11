/*
    This file is part of Equalizer APO, a system-wide equalizer.
    Copyright (C) 2026 Equalizer APO contributors
*/

#include "VolumeTakeoverManager.h"
#include "Editor/widgets/VolumeOsdWidget.h"
#include <algorithm>
#include <cmath>

VolumeTakeoverManager* VolumeTakeoverManager::s_instance = nullptr;
HHOOK VolumeTakeoverManager::s_keyboardHook = NULL;

VolumeTakeoverManager* VolumeTakeoverManager::instance()
{
	if (!s_instance)
		s_instance = new VolumeTakeoverManager();
	return s_instance;
}

VolumeTakeoverManager::VolumeTakeoverManager(QObject* parent)
	: QObject(parent),
	  volumeController(std::make_unique<VolumeController>()),
	  osdWidget(new VolumeOsdWidget()),
	  takeoverEnabled(false),
	  osdEnabled(true),
	  referenceLevel(80.0),
	  referenceOffset(0.0)
{
	s_instance = this;
	lastState.levelDb = 0.0;
	lastState.scalar = 1.0;
	lastState.muted = false;

	if (volumeController)
		volumeController->getVolumeState(lastState);

	connect(&pollTimer, &QTimer::timeout, this, &VolumeTakeoverManager::checkVolumeChange);
	pollTimer.start(250);
}

VolumeTakeoverManager::~VolumeTakeoverManager()
{
	removeHook();
	pollTimer.stop();
	if (osdWidget)
	{
		delete osdWidget;
		osdWidget = nullptr;
	}
	if (s_instance == this)
		s_instance = nullptr;
}

void VolumeTakeoverManager::setTakeoverEnabled(bool enabled)
{
	if (takeoverEnabled == enabled)
		return;
	takeoverEnabled = enabled;
	if (takeoverEnabled)
		installHook();
	else
		removeHook();
}

void VolumeTakeoverManager::setReferenceParameters(double refLevel, double refOffset)
{
	if (std::isfinite(refLevel) && refLevel > 0.0)
		referenceLevel = refLevel;
	if (std::isfinite(refOffset))
		referenceOffset = refOffset;
}

void VolumeTakeoverManager::refreshEndpoint(const std::wstring& endpointId)
{
	volumeController = std::make_unique<VolumeController>(endpointId);
	if (volumeController)
		volumeController->getVolumeState(lastState);
}

void VolumeTakeoverManager::installHook()
{
	if (s_keyboardHook == NULL)
	{
		s_keyboardHook = SetWindowsHookExW(
			WH_KEYBOARD_LL,
			LowLevelKeyboardProc,
			GetModuleHandleW(NULL),
			0);
	}
}

void VolumeTakeoverManager::removeHook()
{
	if (s_keyboardHook != NULL)
	{
		UnhookWindowsHookEx(s_keyboardHook);
		s_keyboardHook = NULL;
	}
}

LRESULT CALLBACK VolumeTakeoverManager::LowLevelKeyboardProc(
	int nCode, WPARAM wParam, LPARAM lParam)
{
	if (nCode == HC_ACTION && (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN))
	{
		KBDLLHOOKSTRUCT* pKey = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
		if (pKey && s_instance && s_instance->takeoverEnabled)
		{
			if (pKey->vkCode == VK_VOLUME_UP)
			{
				s_instance->stepVolume(true);
				return 1;
			}
			else if (pKey->vkCode == VK_VOLUME_DOWN)
			{
				s_instance->stepVolume(false);
				return 1;
			}
			else if (pKey->vkCode == VK_VOLUME_MUTE)
			{
				s_instance->toggleMute();
				return 1;
			}
		}
	}
	return CallNextHookEx(s_keyboardHook, nCode, wParam, lParam);
}

double VolumeTakeoverManager::calculateCurrentPhon(double volumeDb) const
{
	double phon = referenceLevel + volumeDb - referenceOffset;
	return (std::max)(0.0, (std::min)(100.0, phon));
}

void VolumeTakeoverManager::stepVolume(bool up, double stepScalar)
{
	if (!volumeController)
		return;

	EndpointVolumeState state;
	if (FAILED(volumeController->getVolumeState(state)))
		return;

	// If currently muted and increasing volume, unmute first
	if (state.muted && up)
	{
		volumeController->setMute(false);
		state.muted = false;
	}

	double newScalar = state.scalar + (up ? stepScalar : -stepScalar);
	newScalar = (std::max)(0.0, (std::min)(1.0, newScalar));

	if (SUCCEEDED(volumeController->setVolumeScalar(newScalar)))
	{
		volumeController->getVolumeState(state);
		lastState = state;
		if (osdEnabled && osdWidget)
		{
			osdWidget->showVolume(
				state.levelDb, state.scalar, state.muted,
				calculateCurrentPhon(state.levelDb));
		}
		emit volumeChangedExternal(state.levelDb, state.scalar, state.muted);
	}
}

void VolumeTakeoverManager::toggleMute()
{
	if (!volumeController)
		return;

	EndpointVolumeState state;
	if (FAILED(volumeController->getVolumeState(state)))
		return;

	const bool newMute = !state.muted;
	if (SUCCEEDED(volumeController->setMute(newMute)))
	{
		volumeController->getVolumeState(state);
		lastState = state;
		if (osdEnabled && osdWidget)
		{
			osdWidget->showVolume(
				state.levelDb, state.scalar, state.muted,
				calculateCurrentPhon(state.levelDb));
		}
		emit volumeChangedExternal(state.levelDb, state.scalar, state.muted);
	}
}

void VolumeTakeoverManager::showCurrentVolumeOsd()
{
	if (!volumeController || !osdEnabled || !osdWidget)
		return;

	EndpointVolumeState state;
	if (SUCCEEDED(volumeController->getVolumeState(state)))
	{
		osdWidget->showVolume(
			state.levelDb, state.scalar, state.muted,
			calculateCurrentPhon(state.levelDb));
	}
}

void VolumeTakeoverManager::checkVolumeChange()
{
	if (!volumeController)
		return;

	if (volumeController->hasVolumeChanged())
	{
		EndpointVolumeState state;
		if (SUCCEEDED(volumeController->getVolumeState(state)))
		{
			const bool changed = std::abs(state.levelDb - lastState.levelDb) > 0.05
				|| std::abs(state.scalar - lastState.scalar) > 0.005
				|| state.muted != lastState.muted;
			if (changed)
			{
				lastState = state;
				if (osdEnabled && takeoverEnabled && osdWidget)
				{
					osdWidget->showVolume(
						state.levelDb, state.scalar, state.muted,
						calculateCurrentPhon(state.levelDb));
				}
				emit volumeChangedExternal(state.levelDb, state.scalar, state.muted);
			}
		}
	}
}
