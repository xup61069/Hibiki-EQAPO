/*
    This file is part of Equalizer APO, a system-wide equalizer.
    Copyright (C) 2017 Alexander Walch
    Copyright (C) 2026 Equalizer APO contributors

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
*/

#include "OriginalLoudnessCorrectionFilterGUI.h"

#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QResizeEvent>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QStyle>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

#include "Editor/MainWindow.h"
#include "Editor/helpers/GUIHelper.h"
#include "OriginalLoudnessCorrectionCalibrationDialog.h"
#include "widgets/VerticalDragDial.h"

namespace
{
	QWidget* createParameterControl(
		const QString& labelText,
		VerticalDragDial*& dial,
		QDoubleSpinBox*& spinBox,
		double minimum,
		double maximum,
		double step,
		double value,
		const QString& suffix,
		QWidget* parent)
	{
		QWidget* control = new QWidget(parent);
		control->setMinimumWidth(0);
		control->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
		QHBoxLayout* layout = new QHBoxLayout(control);
		layout->setContentsMargins(0, 0, 0, 0);
		layout->setSpacing(GUIHelper::scale(7));

		dial = new VerticalDragDial(control);
		dial->setRange(
			qRound(minimum / step), qRound(maximum / step));
		dial->setValue(qRound(value / step));
		dial->setFixedSize(GUIHelper::scale(QSize(60, 48)));
		dial->setAccessibleName(labelText);
		layout->addWidget(dial);

		QVBoxLayout* textLayout = new QVBoxLayout;
		textLayout->setSpacing(GUIHelper::scale(2));
		QLabel* label = new QLabel(labelText, control);
		label->setWordWrap(true);
		spinBox = new QDoubleSpinBox(control);
		spinBox->setRange(minimum, maximum);
		spinBox->setSingleStep(step);
		spinBox->setDecimals(step < 0.1 ? 2 : 1);
		spinBox->setSuffix(suffix);
		spinBox->setValue(value);
		spinBox->setAccessibleName(labelText);
		label->setBuddy(spinBox);
		textLayout->addWidget(label);
		textLayout->addWidget(spinBox);
		layout->addLayout(textLayout, 1);

		QObject::connect(dial, &QDial::valueChanged,
			spinBox, [spinBox, step](int dialValue) {
				spinBox->setValue(dialValue * step);
			});
		QObject::connect(spinBox,
			qOverload<double>(&QDoubleSpinBox::valueChanged),
			dial, [dial, step](double spinValue) {
				QSignalBlocker blocker(dial);
				dial->setValue(qRound(spinValue / step));
			});
		return control;
	}
}

OriginalLoudnessCorrectionFilterGUI::OriginalLoudnessCorrectionFilterGUI(
	bool state,
	double referenceLevel,
	double referenceOffset,
	double attenuation)
	: IFilterGUI()
{
	setObjectName(QStringLiteral("OriginalLoudnessCorrectionFilterGUI"));
	setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
	setMinimumWidth(0);
	QGridLayout* root = new QGridLayout(this);
	root->setContentsMargins(0, 0, 0, 0);
	root->setHorizontalSpacing(GUIHelper::scale(10));
	root->setVerticalSpacing(GUIHelper::scale(12));

	headerLayout = new QGridLayout;
	headerLayout->setContentsMargins(0, 0, 0, 0);
	headerLayout->setHorizontalSpacing(GUIHelper::scale(10));
	headerLayout->setVerticalSpacing(GUIHelper::scale(3));
	root->addLayout(headerLayout, 0, 0);

	titleLabel = new QLabel(
		tr("Loudness correction (original):"), this);
	titleLabel->setObjectName(QStringLiteral("originalLoudnessTitle"));
	titleLabel->setMinimumHeight(GUIHelper::scale(32));
	titleLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
	QFont titleFont = titleLabel->font();
	titleFont.setWeight(QFont::DemiBold);
	titleLabel->setFont(titleFont);
	enabledCheckBox = new QCheckBox(tr("Enabled"), this);
	enabledCheckBox->setChecked(state);
	enabledCheckBox->setToolTip(tr(
		"Enables only the original two-shelf correction component."));
	attributionLabel = new QLabel(
		tr("Original shelf curve by Alexander Walch · Mixomo fork"), this);
	attributionLabel->setObjectName(QStringLiteral("originalLoudnessAttribution"));
	attributionLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
	attributionLabel->setWordWrap(true);

	parameterControls[0] = createParameterControl(
		tr("Reference level:"), referenceLevelDial,
		referenceLevelSpinBox, -999.0, 999.0, 1.0,
		referenceLevel, QStringLiteral(" dB"), this);
	parameterControls[0]->setObjectName(
		QStringLiteral("originalReferenceLevelControl"));
	parameterControls[1] = createParameterControl(
		tr("Reference offset:"), referenceOffsetDial,
		referenceOffsetSpinBox, -999.0, 999.0, 1.0,
		referenceOffset, QStringLiteral(" dB"), this);
	parameterControls[1]->setObjectName(
		QStringLiteral("originalReferenceOffsetControl"));
	parameterControls[2] = createParameterControl(
		tr("Correction strength:"), attenuationDial,
		attenuationSpinBox, 0.0, 1.0, 0.01,
		attenuation, QString(), this);
	parameterControls[2]->setObjectName(
		QStringLiteral("originalAttenuationControl"));

	parameterControls[3] = new QWidget(this);
	parameterControls[3]->setObjectName(
		QStringLiteral("originalVolumeControl"));
	parameterControls[3]->setMinimumWidth(0);
	parameterControls[3]->setSizePolicy(
		QSizePolicy::Expanding, QSizePolicy::Preferred);
	QVBoxLayout* volumeLayout = new QVBoxLayout(parameterControls[3]);
	volumeLayout->setContentsMargins(0, 0, 0, 0);
	volumeLayout->setSpacing(GUIHelper::scale(2));
	QLabel* volumeLabel = new QLabel(
		tr("Default master volume:"), parameterControls[3]);
	volumeLabel->setWordWrap(true);
	volumeSpinBox = new QDoubleSpinBox(parameterControls[3]);
	volumeSpinBox->setRange(-160.0, 0.0);
	volumeSpinBox->setDecimals(1);
	volumeSpinBox->setSuffix(QStringLiteral(" dB"));
	volumeSpinBox->setButtonSymbols(QAbstractSpinBox::NoButtons);
	volumeSpinBox->setReadOnly(true);
	volumeSpinBox->setFocusPolicy(Qt::NoFocus);
	volumeSpinBox->setAccessibleName(tr("Windows default master volume"));
	volumeLabel->setBuddy(volumeSpinBox);
	volumeStatusLabel = new QLabel(parameterControls[3]);
	volumeStatusLabel->setWordWrap(true);
	volumeLayout->addWidget(volumeLabel);
	volumeLayout->addWidget(volumeSpinBox);
	volumeLayout->addWidget(volumeStatusLabel);
	parameterLayout = new QGridLayout;
	parameterLayout->setContentsMargins(0, 0, 0, 0);
	parameterLayout->setHorizontalSpacing(GUIHelper::scale(10));
	parameterLayout->setVerticalSpacing(GUIHelper::scale(5));
	root->addLayout(parameterLayout, 1, 0);

	QHBoxLayout* actions = new QHBoxLayout;
	actions->addStretch(1);
	QPushButton* resetButton = new QPushButton(tr("Reset original curve"), this);
	resetButton->setToolTip(tr(
		"Resets the original reference, offset, and correction strength. "
		"The enabled state is preserved."));
	calibrateButton = new QPushButton(tr("Calibrate original…"), this);
	calibrateButton->setToolTip(tr(
		"Calibrates this original component against the Windows default "
		"Multimedia playback volume."));
	actions->addWidget(resetButton);
	actions->addWidget(calibrateButton);
	root->addLayout(actions, 2, 0);

	connect(enabledCheckBox, &QCheckBox::toggled,
		this, [this](bool) { emit updateModel(); });
	connect(referenceLevelSpinBox,
		qOverload<double>(&QDoubleSpinBox::valueChanged),
		this, [this](double) { emit updateModel(); });
	connect(referenceOffsetSpinBox,
		qOverload<double>(&QDoubleSpinBox::valueChanged),
		this, [this](double) { emit updateModel(); });
	connect(attenuationSpinBox,
		qOverload<double>(&QDoubleSpinBox::valueChanged),
		this, [this](double) { emit updateModel(); });
	connect(resetButton, &QPushButton::clicked,
		this, &OriginalLoudnessCorrectionFilterGUI::resetCurve);
	connect(calibrateButton, &QPushButton::clicked,
		this, &OriginalLoudnessCorrectionFilterGUI::calibrate);

	volumeTimer.setTimerType(Qt::CoarseTimer);
	volumeTimer.setInterval(250);
	connect(&volumeTimer, &QTimer::timeout,
		this, &OriginalLoudnessCorrectionFilterGUI::updateVolume);
	volumeTimer.start();
	updateResponsiveLayout(width());
	updateVolumeUi();
	updateVolume();
}

OriginalLoudnessCorrectionFilterGUI::~OriginalLoudnessCorrectionFilterGUI()
{
	volumeTimer.stop();
}

void OriginalLoudnessCorrectionFilterGUI::resizeEvent(QResizeEvent* event)
{
	IFilterGUI::resizeEvent(event);
	updateResponsiveLayout(event->size().width());
}

void OriginalLoudnessCorrectionFilterGUI::updateResponsiveLayout(
	int availableWidth)
{
	const int columns = availableWidth >= GUIHelper::scale(620)
		? 4 : (availableWidth >= GUIHelper::scale(280) ? 2 : 1);
	if (responsiveColumnCount == columns)
		return;
	responsiveColumnCount = columns;

	headerLayout->removeWidget(titleLabel);
	headerLayout->removeWidget(enabledCheckBox);
	headerLayout->removeWidget(attributionLabel);
	if (columns == 4)
	{
		headerLayout->addWidget(titleLabel, 0, 0);
		headerLayout->addWidget(enabledCheckBox, 0, 1);
		headerLayout->addWidget(attributionLabel, 0, 2);
		headerLayout->setColumnStretch(2, 1);
		attributionLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
	}
	else if (columns == 2)
	{
		headerLayout->addWidget(titleLabel, 0, 0);
		headerLayout->addWidget(enabledCheckBox, 0, 1);
		headerLayout->addWidget(attributionLabel, 1, 0, 1, 2);
		headerLayout->setColumnStretch(2, 0);
		attributionLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
	}
	else
	{
		headerLayout->addWidget(titleLabel, 0, 0);
		headerLayout->addWidget(enabledCheckBox, 1, 0);
		headerLayout->addWidget(attributionLabel, 2, 0);
		headerLayout->setColumnStretch(1, 0);
		headerLayout->setColumnStretch(2, 0);
		attributionLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
	}

	for (QWidget* control : parameterControls)
		parameterLayout->removeWidget(control);
	for (int column = 0; column < 4; ++column)
		parameterLayout->setColumnStretch(column, column < columns ? 1 : 0);
	for (int index = 0; index < 4; ++index)
		parameterLayout->addWidget(
			parameterControls[index], index / columns, index % columns);

	parameterLayout->activate();
	layout()->activate();
	setMinimumHeight(layout()->minimumSize().height());
	updateGeometry();
	if (parentWidget() != nullptr)
		parentWidget()->updateGeometry();
}

QString OriginalLoudnessCorrectionFilterGUI::serializedParameters(
	bool enabled) const
{
	return QString(
		"Schema 1 Model MixomoShelfV1 State %1 ReferenceLevel %2 "
		"ReferenceOffset %3 Attenuation %4")
		.arg(enabled ? 1 : 0)
		.arg(referenceLevelSpinBox->value(), 0, 'f', 1)
		.arg(referenceOffsetSpinBox->value(), 0, 'f', 1)
		.arg(attenuationSpinBox->value(), 0, 'f', 2);
}

void OriginalLoudnessCorrectionFilterGUI::store(
	QString& command,
	QString& parameters)
{
	command = QStringLiteral("LoudnessCorrectionOriginal");
	parameters = serializedParameters(enabledCheckBox->isChecked());
}

bool OriginalLoudnessCorrectionFilterGUI::readDefaultVolume(
	EndpointVolumeState& state)
{
	if (!volumeController)
		volumeController.reset(new VolumeController());
	if (FAILED(volumeController->getVolumeState(state)) ||
		!std::isfinite(state.levelDb) || !std::isfinite(state.scalar) ||
		volumeController->getEndpointId().empty())
	{
		volumeController.reset();
		resolvedEndpointId.clear();
		return false;
	}
	resolvedEndpointId = volumeController->getEndpointId();
	return true;
}

void OriginalLoudnessCorrectionFilterGUI::updateVolumeUi()
{
	if (volumeAvailable)
	{
		volumeSpinBox->setSpecialValueText(QString());
		volumeStatusLabel->setText(tr(
			"Following Windows volume"));
		volumeStatusLabel->setProperty("statusLevel", "normal");
		calibrateButton->setEnabled(true);
	}
	else
	{
		volumeSpinBox->setSpecialValueText(tr("Unavailable"));
		volumeSpinBox->setValue(volumeSpinBox->minimum());
		volumeStatusLabel->setText(tr(
			"Unavailable - paused"));
		volumeStatusLabel->setProperty("statusLevel", "warning");
		calibrateButton->setEnabled(false);
	}
	volumeStatusLabel->setAccessibleName(tr("Original correction volume status"));
	volumeStatusLabel->setAccessibleDescription(volumeStatusLabel->text());
	volumeStatusLabel->style()->unpolish(volumeStatusLabel);
	volumeStatusLabel->style()->polish(volumeStatusLabel);
}

void OriginalLoudnessCorrectionFilterGUI::updateVolume()
{
	EndpointVolumeState state;
	const bool available = readDefaultVolume(state);
	if (available)
		volumeSpinBox->setValue(state.levelDb);
	if (available != volumeAvailable)
	{
		volumeAvailable = available;
		updateVolumeUi();
	}
}

void OriginalLoudnessCorrectionFilterGUI::resetCurve()
{
	QSignalBlocker referenceBlocker(referenceLevelSpinBox);
	QSignalBlocker offsetBlocker(referenceOffsetSpinBox);
	QSignalBlocker attenuationBlocker(attenuationSpinBox);
	referenceLevelSpinBox->setValue(0.0);
	referenceOffsetSpinBox->setValue(0.0);
	attenuationSpinBox->setValue(1.0);
	referenceLevelDial->setValue(0);
	referenceOffsetDial->setValue(0);
	attenuationDial->setValue(100);
	emit updateModel();
}

void OriginalLoudnessCorrectionFilterGUI::calibrate()
{
	EndpointVolumeState before;
	if (!readDefaultVolume(before) || before.muted || before.scalar <= 0.0)
	{
		QMessageBox::warning(
			this,
			tr("Original calibration not started"),
			tr("The Windows default Multimedia playback endpoint is "
				"unavailable, muted, or at zero volume."));
		return;
	}

	MainWindow* mainWindow = qobject_cast<MainWindow*>(window());
	if (mainWindow == NULL || !mainWindow->beginTemporaryFilterConfiguration(
		this,
		QStringLiteral("LoudnessCorrectionOriginal"),
		serializedParameters(false),
		QStringLiteral("original-loudness-calibration")))
	{
		QMessageBox::warning(
			this,
			tr("Original calibration not started"),
			tr("Save the current profile and resolve any temporary audio "
				"state before calibrating."));
		return;
	}

	OriginalLoudnessCorrectionCalibrationDialog dialog(
		resolvedEndpointId, this);
	const int result = dialog.exec();
	bool keptExternal = false;
	if (!mainWindow->restoreTemporaryFilterConfiguration(&keptExternal))
	{
		QMessageBox::critical(
			this,
			tr("Audio processing was not restored"),
			tr("The temporary original-calibration state could not be "
				"restored automatically. Close the editor and use temporary "
				"audio recovery before continuing."));
		return;
	}
	if (keptExternal)
		return;
	if (result != QDialog::Accepted || !dialog.hasValidMeasurement())
		return;

	EndpointVolumeState after;
	if (!readDefaultVolume(after) || after.muted || after.scalar <= 0.0)
	{
		QMessageBox::warning(
			this,
			tr("Original calibration not applied"),
			tr("Playback became muted or unavailable. The measurement "
				"was discarded."));
		return;
	}

	// This is the released fork's calibration relationship, with the saved
	// reference offset included so a non-zero offset does not bias the result.
	double reference = 75.0 - dialog.getMeasuredLevel() +
		after.levelDb + referenceOffsetSpinBox->value();
	reference = (std::max)(-999.0, (std::min)(999.0, reference));
	referenceLevelSpinBox->setValue(reference);
}

QSize OriginalLoudnessCorrectionFilterGUI::sizeHint() const
{
	QSize result = QWidget::sizeHint();
	// The English endpoint status wraps to two lines at the four-column
	// breakpoint. Reserve the action row as well so FilterTable's initial
	// minimum-height snapshot cannot clip the buttons before the first resize.
	result.setHeight((std::max)(result.height(), GUIHelper::scale(184)));
	return result;
}

QSize OriginalLoudnessCorrectionFilterGUI::minimumSizeHint() const
{
	QSize result = QWidget::minimumSizeHint();
	result.setWidth(0);
	result.setHeight((std::max)(result.height(), GUIHelper::scale(184)));
	return result;
}
