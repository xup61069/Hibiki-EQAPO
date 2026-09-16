/*
    This file is part of Equalizer APO, a system-wide equalizer.
    Copyright (C) 2026 Equalizer APO contributors

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
*/

#pragma once

#include <QBuffer>
#include <QDialog>
#include <QTimer>
#include <memory>
#include <string>

#include "filters/loudnessCorrection/VolumeController.h"

class QLabel;
class QPushButton;
class QRadioButton;
class QSpinBox;

class OriginalLoudnessCorrectionCalibrationDialog : public QDialog
{
	Q_OBJECT

public:
	explicit OriginalLoudnessCorrectionCalibrationDialog(
		const std::wstring& endpointId,
		QWidget* parent = nullptr);
	~OriginalLoudnessCorrectionCalibrationDialog() override;

	int getMeasuredLevel() const;
	bool hasValidMeasurement() const;

public slots:
	void accept() override;
	void reject() override;

private:
	enum PlaybackReadiness
	{
		PLAYBACK_READY,
		PLAYBACK_ENDPOINT_MISMATCH,
		PLAYBACK_VOLUME_UNAVAILABLE,
		PLAYBACK_MUTED
	};

	bool isPlaybackEndpointStillValid() const;
	PlaybackReadiness getPlaybackReadiness();
	bool updatePlaybackReadiness(bool showWarning);
	void playNoise();
	void stopPlayback();
	void stopNoise();
	void setPlaybackStatus(const QString& text, const char* level);
	void updateSignalUi();

	std::wstring endpointId;
	std::unique_ptr<VolumeController> volumeController;
	QBuffer buffer;
	QTimer endpointGuardTimer;
	QRadioButton* leftRadioButton = nullptr;
	QRadioButton* rightRadioButton = nullptr;
	QRadioButton* sine1kHzRadioButton = nullptr;
	QRadioButton* pinkNoiseRadioButton = nullptr;
	QLabel* signalHintLabel = nullptr;
	QPushButton* playButton = nullptr;
	QPushButton* stopButton = nullptr;
	QPushButton* saveButton = nullptr;
	QLabel* statusLabel = nullptr;
	QSpinBox* levelSpinBox = nullptr;
	bool measurementValid = false;
};
