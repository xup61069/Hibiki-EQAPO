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
	emit takeoverToggled(enabled);
}

void VolumeTakeoverManager::setReferenceParameters(double refLevel, double refOffset)
{
	if (std::isfinite(refLevel) && refLevel > 0.0)
		referenceLevel = refLevel;
	if (std::isfinite(refOffset))
		referenceOffset = refOffset;
}

void VolumeTakeoverManager::setManualMode(bool manual, double manualDb)
{
	manualMode = manual;
	if (std::isfinite(manualDb))
		manualVolumeDb = (std::max)(-100.0, (std::min)(0.0, manualDb));
}

void VolumeTakeoverManager::setManualVolumeDb(double manualDb)
{
	if (std::isfinite(manualDb))
		manualVolumeDb = (std::max)(-100.0, (std::min)(0.0, manualDb));
}

void VolumeTakeoverManager::setVolumeFollowMode(
	LoudnessCorrectionFilter::FilterParameters::VolumeFollowMode mode)
{
	volumeFollowMode = mode;
}

void VolumeTakeoverManager::refreshEndpoint(const std::wstring& endpointId)
{
	if (volumeController && volumeController->getRequestedEndpointId() == endpointId)
		return;
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

double VolumeTakeoverManager::calculateCurrentPhon(double volumeDb, double scalar) const
{
	double effectiveDb = volumeDb;
	if (volumeFollowMode != LoudnessCorrectionFilter::FilterParameters::VOLUME_FOLLOW_OFF)
	{
		effectiveDb = LoudnessCorrectionFilter::calculateListeningVolumeDb(
			volumeFollowMode, volumeDb, scalar);
	}
	double phon = referenceLevel + effectiveDb - referenceOffset;
	return (std::max)(0.0, (std::min)(100.0, phon));
}

void VolumeTakeoverManager::stepVolume(bool up, double stepScalar)
{
	if (manualMode)
	{
		if (manualMuted && up)
		{
			manualMuted = false;
		}
		manualVolumeDb = (std::max)(-100.0, (std::min)(0.0, manualVolumeDb + (up ? 1.0 : -1.0)));
		const double scalar = (manualVolumeDb + 100.0) / 100.0;
		if (osdEnabled && osdWidget)
		{
			osdWidget->showVolume(
				manualVolumeDb, scalar, manualMuted,
				manualMuted ? 0.0 : calculateCurrentPhon(manualVolumeDb, scalar));
		}
		emit volumeChangedExternal(manualVolumeDb, scalar, manualMuted);
		return;
	}

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

	// Quantize to integer percentage (0..100) to match Windows master volume steps perfectly
	int currentPercent = static_cast<int>(std::round(state.scalar * 100.0));
	int stepPercent = static_cast<int>(std::round(stepScalar * 100.0));
	if (stepPercent <= 0) stepPercent = 2;
	int newPercent = std::clamp(currentPercent + (up ? stepPercent : -stepPercent), 0, 100);
	double newScalar = newPercent / 100.0;

	if (SUCCEEDED(volumeController->setVolumeScalar(newScalar)))
	{
		volumeController->getVolumeState(state);
		lastState = state;
		if (osdEnabled && osdWidget)
		{
			osdWidget->showVolume(
				state.levelDb, state.scalar, state.muted,
				calculateCurrentPhon(state.levelDb, state.scalar));
		}
		emit volumeChangedExternal(state.levelDb, state.scalar, state.muted);
	}
}

void VolumeTakeoverManager::toggleMute()
{
	if (manualMode)
	{
		manualMuted = !manualMuted;
		const double scalar = manualMuted ? 0.0 : (manualVolumeDb + 100.0) / 100.0;
		const double db = manualMuted ? -100.0 : manualVolumeDb;
		if (osdEnabled && osdWidget)
		{
			osdWidget->showVolume(
				db, scalar, manualMuted,
				manualMuted ? 0.0 : calculateCurrentPhon(manualVolumeDb, scalar));
		}
		emit volumeChangedExternal(db, scalar, manualMuted);
		return;
	}

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
				calculateCurrentPhon(state.levelDb, state.scalar));
		}
		emit volumeChangedExternal(state.levelDb, state.scalar, state.muted);
	}
}

void VolumeTakeoverManager::showCurrentVolumeOsd()
{
	if (!osdEnabled || !osdWidget)
		return;

	if (manualMode)
	{
		const double scalar = manualMuted ? 0.0 : (manualVolumeDb + 100.0) / 100.0;
		const double db = manualMuted ? -100.0 : manualVolumeDb;
		osdWidget->showVolume(
			db, scalar, manualMuted,
			manualMuted ? 0.0 : calculateCurrentPhon(manualVolumeDb, scalar));
		return;
	}

	if (!volumeController)
		return;

	EndpointVolumeState state;
	if (SUCCEEDED(volumeController->getVolumeState(state)))
	{
		osdWidget->showVolume(
			state.levelDb, state.scalar, state.muted,
			calculateCurrentPhon(state.levelDb, state.scalar));
	}
}

void VolumeTakeoverManager::checkVolumeChange()
{
	if (manualMode)
		return;

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
						calculateCurrentPhon(state.levelDb, state.scalar));
				}
				emit volumeChangedExternal(state.levelDb, state.scalar, state.muted);
			}
		}
	}
}
