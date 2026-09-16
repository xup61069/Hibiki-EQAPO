/*
    This file is part of Equalizer APO, a system-wide equalizer.
    Copyright (C) 2017 Alexander Walch
    Copyright (C) 2026 Equalizer APO contributors

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
*/

#pragma once

#include <QTimer>
#include <memory>

#include "Editor/IFilterGUI.h"
#include "filters/loudnessCorrection/VolumeController.h"

class QCheckBox;
class QGridLayout;
class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QResizeEvent;
class VerticalDragDial;

class OriginalLoudnessCorrectionFilterGUI : public IFilterGUI
{
	Q_OBJECT

public:
	OriginalLoudnessCorrectionFilterGUI(
		bool state,
		double referenceLevel,
		double referenceOffset,
		double attenuation);
	~OriginalLoudnessCorrectionFilterGUI() override;

	void store(QString& command, QString& parameters) override;
	QSize sizeHint() const override;
	QSize minimumSizeHint() const override;

protected:
	void resizeEvent(QResizeEvent* event) override;

private:
	void updateResponsiveLayout(int availableWidth);
	void updateVolume();
	bool readDefaultVolume(EndpointVolumeState& state);
	void updateVolumeUi();
	void calibrate();
	void resetCurve();
	QString serializedParameters(bool enabled) const;

	QCheckBox* enabledCheckBox = nullptr;
	QGridLayout* headerLayout = nullptr;
	QGridLayout* parameterLayout = nullptr;
	QLabel* titleLabel = nullptr;
	QLabel* attributionLabel = nullptr;
	QWidget* parameterControls[4] = {};
	VerticalDragDial* referenceLevelDial = nullptr;
	QDoubleSpinBox* referenceLevelSpinBox = nullptr;
	VerticalDragDial* referenceOffsetDial = nullptr;
	QDoubleSpinBox* referenceOffsetSpinBox = nullptr;
	VerticalDragDial* attenuationDial = nullptr;
	QDoubleSpinBox* attenuationSpinBox = nullptr;
	QDoubleSpinBox* volumeSpinBox = nullptr;
	QLabel* volumeStatusLabel = nullptr;
	QPushButton* calibrateButton = nullptr;
	std::unique_ptr<VolumeController> volumeController;
	std::wstring resolvedEndpointId;
	QTimer volumeTimer;
	bool volumeAvailable = false;
	bool volumeMuted = false;
	int responsiveColumnCount = 0;
};
