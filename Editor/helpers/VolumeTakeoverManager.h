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

	void stepVolume(bool up, double stepScalar = 0.02);
	void toggleMute();
	void showCurrentVolumeOsd();

	VolumeController* getVolumeController() { return volumeController.get(); }
	void refreshEndpoint(const std::wstring& endpointId = L"");

public slots:
	void checkVolumeChange();

signals:
	void volumeChangedExternal(double levelDb, double scalar, bool muted);

private:
	static LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam);

	void installHook();
	void removeHook();
	double calculateCurrentPhon(double volumeDb) const;

	static VolumeTakeoverManager* s_instance;
	static HHOOK s_keyboardHook;

	std::unique_ptr<VolumeController> volumeController;
	VolumeOsdWidget* osdWidget = nullptr;
	QTimer pollTimer;

	bool takeoverEnabled = false;
	bool osdEnabled = true;
	double referenceLevel = 80.0;
	double referenceOffset = 0.0;
	EndpointVolumeState lastState;
};
