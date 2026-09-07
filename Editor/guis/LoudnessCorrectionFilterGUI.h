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

#include <QTimer>
#include <memory>
#include <string>

#include "Editor/IFilterGUI.h"
#include "filters/loudnessCorrection/LoudnessCorrectionFilter.h"
#include "filters/loudnessCorrection/VolumeController.h"

namespace Ui {
class LoudnessCorrectionFilterGUI;
}

class LoudnessCorrectionFilterGUI : public IFilterGUI
{
	Q_OBJECT

public:
	explicit LoudnessCorrectionFilterGUI(
		bool state,
		double refLevel,
		double refOffset,
		double att,
		LoudnessCorrectionFilter::FilterParameters::BindingMode binding,
		bool useManualVolume,
		double manualVolume,
		LoudnessCorrectionFilter::FilterParameters::EngineMode engine,
		LoudnessCorrectionFilter::FilterParameters::VolumeFollowMode volumeFollow,
		const std::wstring& endpointIdentifier,
		bool selectedEndpointIsRender);
	~LoudnessCorrectionFilterGUI();

	void store(QString& command, QString& parameters) override;

	QSize sizeHint() const override;
	QSize minimumSizeHint() const override;
	int preferredHeight() const;

private slots:
	void on_refLevelSpinBox_valueChanged(int arg1);
	void on_refOffsetSpinBox_valueChanged(int arg1);
	void on_attDial_valueChanged(int value);
	void on_attSpinBox_valueChanged(double arg1);
	void on_bindingComboBox_currentIndexChanged(int index);
	void on_manualVolumeCheckBox_toggled(bool checked);
	void on_volumeSpinBox_valueChanged(double value);
	void on_fastEngineCheckBox_toggled(bool checked);
	void on_volumeFollowComboBox_currentIndexChanged(int index);
	void on_studioButton_clicked();
	void on_calibrateButton_clicked();
	void updateVolume();

private:
	LoudnessCorrectionFilter::FilterParameters::BindingMode getBindingMode() const;
	LoudnessCorrectionFilter::FilterParameters::VolumeFollowMode
		getVolumeFollowMode() const;
	std::wstring getRequestedEndpointId() const;
	void refreshVolumeController();
	void updateAutomaticVolumeUi();
	void updateVolumeReadout();
	bool tryReadEndpointVolumeState(EndpointVolumeState& volumeState);
	bool tryUpdateVolume();
	Ui::LoudnessCorrectionFilterGUI* ui;
	bool state;
	bool selectedEndpointIsRender;
	bool automaticVolumeAvailable;
	std::wstring endpointIdentifier;
	std::wstring endpointId;
	QTimer timer;
	std::unique_ptr<VolumeController> volumeController;
	EndpointVolumeState lastEndpointState;
	double lastVolume = -1;
};
