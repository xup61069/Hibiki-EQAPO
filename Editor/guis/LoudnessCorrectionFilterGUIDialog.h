/*
    This file is part of Equalizer APO, a system-wide equalizer.
    Copyright (C) 2017  Jonas Thedering

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with this program; if not, write to the Free Software Foundation, Inc.,
    51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/

#pragma once

#include <QDialog>
#include <QBuffer>
#include <QTimer>
#include <memory>
#include <string>

#include "filters/loudnessCorrection/VolumeController.h"

namespace Ui {
class LoudnessCorrectionFilterGUIDialog;
}

class LoudnessCorrectionFilterGUIDialog : public QDialog
{
	Q_OBJECT

public:
	explicit LoudnessCorrectionFilterGUIDialog(
		const std::wstring& endpointId,
		bool automaticVolumeAvailable,
		bool followsDefaultMultimedia,
		QWidget* parent = 0);
	~LoudnessCorrectionFilterGUIDialog();

	int getMeasuredLevel();
	bool hasValidMeasurement() const;

public slots:
	void accept() override;
	void reject() override;

private slots:
	void on_playButton_clicked();
	void on_stopButton_clicked();

	void on_leftRadioButton_toggled(bool checked);
	void on_rightRadioButton_toggled(bool checked);
	void on_bothRadioButton_toggled(bool checked);
	void on_sine1kHzRadioButton_toggled(bool checked);
	void on_pinkNoiseRadioButton_toggled(bool checked);

private:
	void updateSignalUi();
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
	void stopPlayback();
	void setPlaybackStatus(const QString& text, const char* level);
	Ui::LoudnessCorrectionFilterGUIDialog* ui;
	QBuffer buffer;
	std::wstring endpointId;
	bool followsDefaultMultimedia;
	bool playbackUsesSelectedEndpoint;
	bool measurementValid;
	std::unique_ptr<VolumeController> volumeController;
	QTimer endpointGuardTimer;
};
