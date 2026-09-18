/*
    This file is part of Equalizer APO, a system-wide equalizer.
    Copyright (C) 2026 Equalizer APO contributors
*/

#include "VolumeTakeoverManager.h"
#include "Editor/widgets/VolumeOsdWidget.h"
#include "filters/loudnessCorrection/HibikiVolumeTakeoverShared.h"
#include <QCoreApplication>
#include <algorithm>
#include <cmath>

VolumeTakeoverManager* VolumeTakeoverManager::s_instance = nullptr;
HHOOK VolumeTakeoverManager::s_keyboardHook = NULL;

VolumeTakeoverManager* VolumeTakeoverManager::instance()
{
	if (!s_instance)
		s_instance = new VolumeTakeoverManager(qApp);
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

	connect(osdWidget, &VolumeOsdWidget::volumeSliderDragged,
		this, &VolumeTakeoverManager::setVolumeScalar);
	connect(osdWidget, &VolumeOsdWidget::muteIconClicked,
		this, &VolumeTakeoverManager::toggleMute);
}

VolumeTakeoverManager::~VolumeTakeoverManager()
{
	if (takeoverEnabled)
	{
		if (takeoverShared)
		{
			HibikiTakeoverIpc::writeTakeoverSnapshot(
				takeoverShared, false, takeoverLevelDb, takeoverScalar, takeoverMuted, L"", GetCurrentProcessId());
		}
		if (volumeController)
		{
			volumeController->setVolumeScalar(takeoverScalar);
			volumeController->setMute(takeoverMuted);
		}
		closeSharedMemory();
	}
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

void VolumeTakeoverManager::openSharedMemory()
{
	if (takeoverMapping == NULL)
	{
		takeoverMapping = HibikiTakeoverIpc::createOrOpenSharedMapping(true);
	}
	if (takeoverMapping != NULL && takeoverShared == nullptr)
	{
		takeoverShared = static_cast<HibikiVolumeTakeoverSharedData*>(
			MapViewOfFile(takeoverMapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(HibikiVolumeTakeoverSharedData)));
	}
}

void VolumeTakeoverManager::closeSharedMemory()
{
	if (takeoverShared != nullptr)
	{
		HibikiTakeoverIpc::writeTakeoverSnapshot(
			takeoverShared, false, takeoverLevelDb, takeoverScalar, takeoverMuted, L"", GetCurrentProcessId());
		UnmapViewOfFile(takeoverShared);
		takeoverShared = nullptr;
	}
	if (takeoverMapping != NULL)
	{
		CloseHandle(takeoverMapping);
		takeoverMapping = NULL;
	}
}

void VolumeTakeoverManager::publishTakeoverSharedData()
{
	if (!takeoverShared)
		openSharedMemory();
	if (takeoverShared)
	{
		HibikiTakeoverIpc::writeTakeoverSnapshot(
			takeoverShared,
			takeoverEnabled,
			takeoverLevelDb,
			takeoverScalar,
			takeoverMuted,
			L"",
			GetCurrentProcessId());
	}
}

void VolumeTakeoverManager::enforceWindowsVolume100()
{
	if (!volumeController)
		return;
	EndpointVolumeState realState;
	if (SUCCEEDED(volumeController->getRealEndpointVolumeState(realState)))
	{
		if (realState.scalar < 0.999)
		{
			volumeController->setVolumeScalar(1.0);
		}
		if (realState.muted)
		{
			volumeController->setMute(false);
		}
	}
	else
	{
		volumeController->setVolumeScalar(1.0);
		volumeController->setMute(false);
	}
}

void VolumeTakeoverManager::setTakeoverEnabled(bool enabled)
{
	if (takeoverEnabled == enabled)
		return;
	takeoverEnabled = enabled;
	if (takeoverEnabled)
	{
		openSharedMemory();
		if (manualMode)
		{
			takeoverLevelDb = manualVolumeDb;
			takeoverScalar = (manualVolumeDb + 100.0) / 100.0;
			takeoverMuted = manualMuted;
		}
		else if (volumeController)
		{
			EndpointVolumeState currentEndpointState;
			if (SUCCEEDED(volumeController->getRealEndpointVolumeState(currentEndpointState)))
			{
				takeoverScalar = currentEndpointState.scalar;
				takeoverLevelDb = currentEndpointState.levelDb;
				takeoverMuted = currentEndpointState.muted;
			}
			else
			{
				takeoverScalar = lastState.scalar;
				takeoverLevelDb = lastState.levelDb;
				takeoverMuted = lastState.muted;
			}
		}
		enforceWindowsVolume100();
		publishTakeoverSharedData();
		installHook();
	}
	else
	{
		removeHook();
		if (takeoverShared)
		{
			HibikiTakeoverIpc::writeTakeoverSnapshot(
				takeoverShared, false, takeoverLevelDb, takeoverScalar, takeoverMuted, L"", GetCurrentProcessId());
		}
		if (volumeController)
		{
			volumeController->setVolumeScalar(takeoverScalar);
			volumeController->setMute(takeoverMuted);
			volumeController->getRealEndpointVolumeState(lastState);
		}
		closeSharedMemory();
	}
	emit takeoverToggled(enabled);
	emit volumeChangedExternal(takeoverLevelDb, takeoverScalar, takeoverMuted);
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
	{
		manualVolumeDb = (std::max)(-100.0, (std::min)(0.0, manualDb));
		if (takeoverEnabled && manual)
		{
			takeoverLevelDb = manualVolumeDb;
			takeoverScalar = (manualVolumeDb + 100.0) / 100.0;
			publishTakeoverSharedData();
		}
	}
}

void VolumeTakeoverManager::setManualVolumeDb(double manualDb)
{
	if (std::isfinite(manualDb))
	{
		manualVolumeDb = (std::max)(-100.0, (std::min)(0.0, manualDb));
		if (takeoverEnabled && manualMode)
		{
			takeoverLevelDb = manualVolumeDb;
			takeoverScalar = (manualVolumeDb + 100.0) / 100.0;
			publishTakeoverSharedData();
		}
	}
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
	{
		volumeController->getVolumeState(lastState);
		if (takeoverEnabled)
		{
			enforceWindowsVolume100();
			publishTakeoverSharedData();
		}
	}
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

double VolumeTakeoverManager::calculateApoFollowTargetDb(
	double volumeDb, double scalar, bool muted) const
{
	if (muted)
		return -100.0;
	if (volumeFollowMode == LoudnessCorrectionFilter::FilterParameters::VOLUME_FOLLOW_OFF)
		return 0.0;

	const double gain = LoudnessCorrectionFilter::calculateVolumeFollowGain(
		volumeFollowMode, volumeDb, scalar, false);
	return gain > 0.0 ? (20.0 * std::log10(gain)) : -100.0;
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
		takeoverLevelDb = manualVolumeDb;
		takeoverScalar = scalar;
		takeoverMuted = manualMuted;
		if (takeoverEnabled)
		{
			enforceWindowsVolume100();
			publishTakeoverSharedData();
		}
		const double apoDb = calculateApoFollowTargetDb(manualVolumeDb, scalar, manualMuted);
		if (osdEnabled && osdWidget)
		{
			osdWidget->showVolume(
				apoDb, scalar, manualMuted,
				manualMuted ? 0.0 : calculateCurrentPhon(manualVolumeDb, scalar));
		}
		emit volumeChangedExternal(manualVolumeDb, scalar, manualMuted);
		return;
	}

	if (takeoverEnabled)
	{
		EndpointVolumeState state;
		state.scalar = takeoverScalar;
		state.levelDb = takeoverLevelDb;
		state.muted = takeoverMuted;

		if (state.muted && up)
		{
			state.muted = false;
			takeoverMuted = false;
		}

		int currentPercent = static_cast<int>(std::round(state.scalar * 100.0));
		int stepPercent = static_cast<int>(std::round(stepScalar * 100.0));
		if (stepPercent <= 0) stepPercent = 2;
		int newPercent = std::clamp(currentPercent + (up ? stepPercent : -stepPercent), 0, 100);
		double newScalar = newPercent / 100.0;

		state.scalar = newScalar;
		state.levelDb = (newPercent == 0) ? -100.0 : (newPercent - 100.0);
		takeoverScalar = state.scalar;
		takeoverLevelDb = state.levelDb;
		lastState = state;

		enforceWindowsVolume100();
		publishTakeoverSharedData();

		if (osdEnabled && osdWidget)
		{
			const double apoDb = calculateApoFollowTargetDb(state.levelDb, state.scalar, state.muted);
			osdWidget->showVolume(
				apoDb, state.scalar, state.muted,
				calculateCurrentPhon(state.levelDb, state.scalar));
		}
		emit volumeChangedExternal(state.levelDb, state.scalar, state.muted);
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
		if (FAILED(volumeController->getVolumeState(state)))
		{
			state.scalar = newScalar;
			double db = 0.0;
			if (SUCCEEDED(volumeController->getVolume(db)))
				state.levelDb = db;
		}
		lastState = state;
		if (osdEnabled && osdWidget)
		{
			const double apoDb = calculateApoFollowTargetDb(state.levelDb, state.scalar, state.muted);
			osdWidget->showVolume(
				apoDb, state.scalar, state.muted,
				calculateCurrentPhon(state.levelDb, state.scalar));
		}
		emit volumeChangedExternal(state.levelDb, state.scalar, state.muted);
	}
}

void VolumeTakeoverManager::setVolumeScalar(double scalar)
{
	if (!std::isfinite(scalar))
		return;

	// Quantize to integer percentage (0..100) to match Windows master volume steps.
	const int newPercent = std::clamp(static_cast<int>(std::round(scalar * 100.0)), 0, 100);
	const double newScalar = newPercent / 100.0;
	const double newLevelDb = (newPercent == 0) ? -100.0 : (newPercent - 100.0);

	if (manualMode)
	{
		if (manualMuted && newScalar > 0.005)
			manualMuted = false;
		manualVolumeDb = newLevelDb;
		takeoverLevelDb = manualVolumeDb;
		takeoverScalar = newScalar;
		takeoverMuted = manualMuted;
		if (takeoverEnabled)
		{
			enforceWindowsVolume100();
			publishTakeoverSharedData();
		}
		const double apoDb = calculateApoFollowTargetDb(manualVolumeDb, newScalar, manualMuted);
		if (osdEnabled && osdWidget)
		{
			osdWidget->showVolume(
				apoDb, newScalar, manualMuted,
				manualMuted ? 0.0 : calculateCurrentPhon(manualVolumeDb, newScalar));
		}
		emit volumeChangedExternal(manualVolumeDb, newScalar, manualMuted);
		return;
	}

	if (takeoverEnabled)
	{
		if (takeoverMuted && newScalar > 0.005)
			takeoverMuted = false;
		takeoverScalar = newScalar;
		takeoverLevelDb = newLevelDb;
		lastState.scalar = newScalar;
		lastState.levelDb = newLevelDb;
		lastState.muted = takeoverMuted;

		enforceWindowsVolume100();
		publishTakeoverSharedData();

		if (osdEnabled && osdWidget)
		{
			const double apoDb = calculateApoFollowTargetDb(newLevelDb, newScalar, takeoverMuted);
			osdWidget->showVolume(
				apoDb, newScalar, takeoverMuted,
				calculateCurrentPhon(newLevelDb, newScalar));
		}
		emit volumeChangedExternal(newLevelDb, newScalar, takeoverMuted);
		return;
	}

	if (!volumeController)
		return;

	if (SUCCEEDED(volumeController->setVolumeScalar(newScalar)))
	{
		EndpointVolumeState state;
		if (FAILED(volumeController->getVolumeState(state)))
		{
			state.scalar = newScalar;
			state.levelDb = newLevelDb;
			state.muted = false;
		}
		if (state.muted && newScalar > 0.005)
		{
			volumeController->setMute(false);
			state.muted = false;
		}
		lastState = state;
		if (osdEnabled && osdWidget)
		{
			const double apoDb = calculateApoFollowTargetDb(state.levelDb, state.scalar, state.muted);
			osdWidget->showVolume(
				apoDb, state.scalar, state.muted,
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
		takeoverLevelDb = db;
		takeoverScalar = scalar;
		takeoverMuted = manualMuted;
		if (takeoverEnabled)
		{
			enforceWindowsVolume100();
			publishTakeoverSharedData();
		}
		const double apoDb = calculateApoFollowTargetDb(manualVolumeDb, scalar, manualMuted);
		if (osdEnabled && osdWidget)
		{
			osdWidget->showVolume(
				apoDb, scalar, manualMuted,
				manualMuted ? 0.0 : calculateCurrentPhon(manualVolumeDb, scalar));
		}
		emit volumeChangedExternal(db, scalar, manualMuted);
		return;
	}

	if (takeoverEnabled)
	{
		const bool newMute = !takeoverMuted;
		takeoverMuted = newMute;
		EndpointVolumeState state;
		state.scalar = takeoverScalar;
		state.levelDb = takeoverLevelDb;
		state.muted = newMute;
		lastState = state;

		enforceWindowsVolume100();
		publishTakeoverSharedData();

		if (osdEnabled && osdWidget)
		{
			const double apoDb = calculateApoFollowTargetDb(state.levelDb, state.scalar, state.muted);
			osdWidget->showVolume(
				apoDb, state.scalar, state.muted,
				calculateCurrentPhon(state.levelDb, state.scalar));
		}
		emit volumeChangedExternal(state.levelDb, state.scalar, state.muted);
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
		if (FAILED(volumeController->getVolumeState(state)))
		{
			state.muted = newMute;
		}
		lastState = state;
		if (osdEnabled && osdWidget)
		{
			const double apoDb = calculateApoFollowTargetDb(state.levelDb, state.scalar, state.muted);
			osdWidget->showVolume(
				apoDb, state.scalar, state.muted,
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
		const double apoDb = calculateApoFollowTargetDb(manualVolumeDb, scalar, manualMuted);
		osdWidget->showVolume(
			apoDb, scalar, manualMuted,
			manualMuted ? 0.0 : calculateCurrentPhon(manualVolumeDb, scalar));
		return;
	}

	if (takeoverEnabled)
	{
		const double apoDb = calculateApoFollowTargetDb(takeoverLevelDb, takeoverScalar, takeoverMuted);
		osdWidget->showVolume(
			apoDb, takeoverScalar, takeoverMuted,
			takeoverMuted ? 0.0 : calculateCurrentPhon(takeoverLevelDb, takeoverScalar));
		return;
	}

	if (!volumeController)
		return;

	EndpointVolumeState state;
	if (SUCCEEDED(volumeController->getVolumeState(state)))
	{
		const double apoDb = calculateApoFollowTargetDb(state.levelDb, state.scalar, state.muted);
		osdWidget->showVolume(
			apoDb, state.scalar, state.muted,
			calculateCurrentPhon(state.levelDb, state.scalar));
	}
}

void VolumeTakeoverManager::checkVolumeChange()
{
	if (takeoverEnabled)
	{
		HibikiTakeoverIpc::updateHeartbeat(takeoverShared);
		enforceWindowsVolume100();
		return;
	}

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
					const double apoDb = calculateApoFollowTargetDb(state.levelDb, state.scalar, state.muted);
					osdWidget->showVolume(
						apoDb, state.scalar, state.muted,
						calculateCurrentPhon(state.levelDb, state.scalar));
				}
				emit volumeChangedExternal(state.levelDb, state.scalar, state.muted);
			}
		}
	}
}
