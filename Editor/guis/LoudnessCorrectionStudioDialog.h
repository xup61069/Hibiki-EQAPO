/*
    This file is part of Equalizer APO, a system-wide equalizer.
    Copyright (C) 2026  Equalizer APO contributors

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
*/

#pragma once

#include <QDialog>
#include "filters/loudnessCorrection/LoudnessCorrectionFilter.h"

class QEvent;
class QPainter;
class QResizeEvent;

namespace Ui {
class LoudnessCorrectionStudioDialog;
}

class LoudnessCorrectionStudioDialog : public QDialog
{
	Q_OBJECT

public:
	explicit LoudnessCorrectionStudioDialog(
		int referenceLevel,
		int referenceOffset,
		double attenuation,
		bool globalBinding,
		bool useManualVolume,
		double volume,
		bool automaticVolumeAvailable,
		LoudnessCorrectionFilter::FilterParameters::VolumeFollowMode volumeFollow,
		double automaticVolumeScalar,
		double automaticVolumeDb,
		QWidget* parent = 0);
	~LoudnessCorrectionStudioDialog();

	int getReferenceLevel() const;
	int getReferenceOffset() const;
	double getAttenuation() const;
	bool getGlobalBinding() const;
	bool getUseManualVolume() const;
	double getVolume() const;
	bool shouldCalibrateAfterApply() const;

protected:
	void changeEvent(QEvent* event) override;
	bool eventFilter(QObject* watched, QEvent* event) override;
	void resizeEvent(QResizeEvent* event) override;

private slots:
	void on_referenceSlider_valueChanged(int value);
	void on_refLevelSpinBox_valueChanged(int value);
	void on_offsetSlider_valueChanged(int value);
	void on_refOffsetSpinBox_valueChanged(int value);
	void on_strengthSlider_valueChanged(int value);
	void on_strengthSpinBox_valueChanged(double value);
	void on_bindingComboBox_currentIndexChanged(int index);
	void on_manualVolumeCheckBox_toggled(bool checked);
	void on_volumeSpinBox_valueChanged(double value);
	void on_resetButton_clicked();
	void on_calibrateButton_clicked();

private:
	void applyModernStyle();
	void updateModernUi();
	void updateResponsiveLayout();
	void paintCurvePreview(QPainter& painter) const;
	void refreshStatusStyle(const char* status);
	double listeningVolumeDb() const;

	Ui::LoudnessCorrectionStudioDialog* ui;
	bool initialGlobalBinding;
	bool automaticVolumeAvailable;
	LoudnessCorrectionFilter::FilterParameters::VolumeFollowMode volumeFollow;
	double automaticVolumeScalar;
	double automaticVolumeDb;
	bool calibrateAfterApply;
	bool applyingModernStyle;
	bool responsiveLayoutInitialized = false;
	bool compactLayout = false;
};
