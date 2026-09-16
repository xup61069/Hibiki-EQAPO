/*
    This file is part of Equalizer APO, a system-wide equalizer.
    Copyright (C) 2026 Equalizer APO contributors
*/

#pragma once

#include <QObject>
#include <QTimer>
#include <memory>
#include <string>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "filters/loudnessCorrection/VolumeController.h"
#include "filters/loudnessCorrection/LoudnessCorrectionFilter.h"

class VolumeOsdWidget;

class VolumeTakeoverManager : public QObject
{
	Q_OBJECT

public:
	static VolumeTakeoverManager* instance();

	explicit VolumeTakeoverManager(QObject* parent = nullptr);
	~VolumeTakeoverManager() override;

	void setTakeoverEnabled(bool enabled);
	bool isTakeoverEnabled() const { return takeoverEnabled; }

	void setOsdEnabled(bool enabled) { osdEnabled = enabled; }
	bool isOsdEnabled() const { return osdEnabled; }

	void setReferenceParameters(double refLevel, double refOffset);
	void setVolumeFollowMode(LoudnessCorrectionFilter::FilterParameters::VolumeFollowMode mode);
	LoudnessCorrectionFilter::FilterParameters::VolumeFollowMode getVolumeFollowMode() const { return volumeFollowMode; }
	void setManualMode(bool manual, double manualDb = 0.0);
	bool isManualMode() const { return manualMode; }
	void setManualVolumeDb(double manualDb);
	double getManualVolumeDb() const { return manualVolumeDb; }

	double getTakeoverScalar() const { return takeoverScalar; }
	double getTakeoverLevelDb() const { return takeoverLevelDb; }
	bool isTakeoverMuted() const { return takeoverMuted; }

	double calculateApoFollowTargetDb(double volumeDb, double scalar = 1.0, bool muted = false) const;

	void stepVolume(bool up, double stepScalar = 0.02);
	void toggleMute();
	void showCurrentVolumeOsd();

	VolumeController* getVolumeController() { return volumeController.get(); }
	void refreshEndpoint(const std::wstring& endpointId = L"");

	void enforceWindowsVolume100();
	void publishTakeoverSharedData();

public slots:
	void checkVolumeChange();

signals:
	void volumeChangedExternal(double levelDb, double scalar, bool muted);
	void takeoverToggled(bool enabled);

private:
	static LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam);

	void installHook();
	void removeHook();
	void openSharedMemory();
	void closeSharedMemory();
	double calculateCurrentPhon(double volumeDb, double scalar = 1.0) const;

	static VolumeTakeoverManager* s_instance;
	static HHOOK s_keyboardHook;

	std::unique_ptr<VolumeController> volumeController;
	VolumeOsdWidget* osdWidget = nullptr;
	QTimer pollTimer;

	bool takeoverEnabled = false;
	bool osdEnabled = true;
	bool manualMode = false;
	double manualVolumeDb = 0.0;
	bool manualMuted = false;
	double referenceLevel = 80.0;
	double referenceOffset = 0.0;
	LoudnessCorrectionFilter::FilterParameters::VolumeFollowMode volumeFollowMode =
		LoudnessCorrectionFilter::FilterParameters::VOLUME_FOLLOW_OFF;
	EndpointVolumeState lastState;

	double takeoverScalar = 1.0;
	double takeoverLevelDb = 0.0;
	bool takeoverMuted = false;

	HANDLE takeoverMapping = NULL;
	struct HibikiVolumeTakeoverSharedData* takeoverShared = nullptr;
};
