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

#include "Editor/helpers/GUIHelper.h"
#include "Editor/MainWindow.h"
#include "LoudnessCorrectionFilterGUIDialog.h"
#include "LoudnessCorrectionFilterGUI.h"
#include "LoudnessCorrectionStudioDialog.h"
#include "Editor/helpers/VolumeTakeoverManager.h"
#include "ui_LoudnessCorrectionFilterGUI.h"
#include <cmath>
#include <limits>
#include <QMessageBox>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QToolTip>
#include <QPushButton>
#include <QVariant>

LoudnessCorrectionFilterGUI::LoudnessCorrectionFilterGUI(
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
	bool selectedEndpointIsRender)
	: IFilterGUI(),
	  ui(new Ui::LoudnessCorrectionFilterGUI),
	  state(state),
	  selectedEndpointIsRender(selectedEndpointIsRender),
	  automaticVolumeAvailable(false),
	  endpointIdentifier(endpointIdentifier),
	  endpointId(),
	  lastVolume(std::numeric_limits<double>::quiet_NaN())
{
	ui->setupUi(this);
	setMinimumWidth(0);

	const QSize compactDialSize = GUIHelper::scale(QSize(60, 48));
	ui->refLevelDial->setFixedSize(compactDialSize);
	ui->refOffsetDial->setFixedSize(compactDialSize);
	ui->attDial->setFixedSize(compactDialSize);
	ui->refLevelDial->setAccessibleName(ui->refLevelLabel->text());
	ui->refOffsetDial->setAccessibleName(ui->refOffsetLabel->text());
	ui->attDial->setAccessibleName(ui->attLabel->text());
	ui->volumeSpinBox->setAccessibleName(tr("Listening volume"));

	if (refLevel <= 0)
		refLevel = 80;

	ui->refLevelSpinBox->setValue((int)refLevel);
	ui->refOffsetSpinBox->setValue((int)refOffset);
	ui->attSpinBox->setValue(att);
	ui->refLevelSpinBox->setProperty("defaultValue", 80);
	ui->refOffsetSpinBox->setProperty("defaultValue", 0);
	ui->attSpinBox->setProperty("defaultValue", 1.0);
	ui->refLevelDial->setProperty("resetTarget", QVariant::fromValue(static_cast<QObject*>(ui->refLevelSpinBox)));
	ui->refLevelDial->setProperty("defaultTargetValue", 80);
	ui->refOffsetDial->setProperty("resetTarget", QVariant::fromValue(static_cast<QObject*>(ui->refOffsetSpinBox)));
	ui->refOffsetDial->setProperty("defaultTargetValue", 0);
	ui->attDial->setProperty("resetTarget", QVariant::fromValue(static_cast<QObject*>(ui->attSpinBox)));
	ui->attDial->setProperty("defaultTargetValue", 1.0);

	QPushButton* resetButton = new QPushButton(tr("Reset contour"), this);
	resetButton->setObjectName(QStringLiteral("loudnessResetButton"));
	resetButton->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
	resetButton->setMinimumWidth(GUIHelper::scale(88));
	resetButton->setToolTip(tr(
		"Reset reference level, offset, and correction strength; listening and engine options are preserved."));
	ui->actionsLayout->insertWidget(1, resetButton);
	connect(resetButton, &QPushButton::clicked, this, [this]() {
		ui->refLevelSpinBox->setValue(80);
		ui->refOffsetSpinBox->setValue(0);
		ui->attSpinBox->setValue(1.0);
		emit updateModel();
	});

	bool bindingBlocked = ui->bindingComboBox->blockSignals(true);
	ui->bindingComboBox->setCurrentIndex(
		binding == LoudnessCorrectionFilter::FilterParameters::BINDING_ALL ? 1 : 0);
	ui->bindingComboBox->blockSignals(bindingBlocked);
	bool engineBlocked = ui->fastEngineCheckBox->blockSignals(true);
	ui->fastEngineCheckBox->setChecked(
		engine == LoudnessCorrectionFilter::FilterParameters::ENGINE_FAST);
	ui->fastEngineCheckBox->blockSignals(engineBlocked);
	bool volumeFollowBlocked = ui->volumeFollowComboBox->blockSignals(true);
	int volumeFollowIndex = static_cast<int>(volumeFollow);
	if (volumeFollowIndex < 0 || volumeFollowIndex > 3)
		volumeFollowIndex = 0;
	ui->volumeFollowComboBox->setCurrentIndex(volumeFollowIndex);
	ui->volumeFollowComboBox->blockSignals(volumeFollowBlocked);
	refreshVolumeController();
	updateAutomaticVolumeUi();

	bool blocked = ui->manualVolumeCheckBox->blockSignals(true);
	// Availability is an observation, not a user preference. A temporarily
	// unreadable endpoint must not silently turn an automatic row into
	// "Volume 0.0" the next time any other field is edited.
	ui->manualVolumeCheckBox->setChecked(useManualVolume);
	ui->manualVolumeCheckBox->blockSignals(blocked);
	ui->volumeSpinBox->setEnabled(useManualVolume);
	if (useManualVolume)
	{
		ui->volumeSpinBox->setValue(manualVolume);
		lastVolume = manualVolume;
	}
	else if (this->automaticVolumeAvailable)
	{
		ui->volumeSpinBox->setValue(lastVolume);
	}
	else
	{
		ui->volumeSpinBox->setSpecialValueText(tr("Unavailable"));
		ui->volumeSpinBox->setValue(ui->volumeSpinBox->minimum());
	}

	connect(&timer, SIGNAL(timeout()), this, SLOT(updateVolume()));
	VolumeTakeoverManager::instance()->setReferenceParameters(
		ui->refLevelSpinBox->value(), ui->refOffsetSpinBox->value());
	connect(VolumeTakeoverManager::instance(), &VolumeTakeoverManager::volumeChangedExternal,
		this, [this](double levelDb, double scalar, bool muted) {
			Q_UNUSED(scalar);
			Q_UNUSED(muted);
			if (!ui->manualVolumeCheckBox->isChecked())
			{
				lastVolume = levelDb;
				ui->volumeSpinBox->setValue(levelDb);
				updateVolumeReadout();
				emit updateModel();
			}
		});
	updateAutomaticVolumeUi();
	updateVolumeReadout();
	timer.start(250);
}

LoudnessCorrectionFilterGUI::~LoudnessCorrectionFilterGUI()
{
	delete ui;
}

void LoudnessCorrectionFilterGUI::store(QString& command, QString& parameters)
{
	command = "LoudnessCorrection";
	QString binding = getBindingMode() ==
		LoudnessCorrectionFilter::FilterParameters::BINDING_ALL ? "All" : "Single";
	parameters = QString("Schema 1 Model FormulaLoudnessV1 Binding %1 State %2 ReferenceLevel %3 ReferenceOffset %4 Attenuation ")
		.arg(binding)
		.arg(state ? 1 : 0)
		.arg(ui->refLevelSpinBox->value())
		.arg(ui->refOffsetSpinBox->value());
	double att = ui->attSpinBox->value();
	if (att == 0.0 || att == 1.0)
		parameters += QString("%1").arg(att, 0, 'f', 1);
	else
		parameters += QString("%1").arg(att);

	if (ui->manualVolumeCheckBox->isChecked())
		parameters += QString(" Volume %1").arg(
			ui->volumeSpinBox->value(), 0, 'f', 1);

	if (ui->fastEngineCheckBox->isChecked())
		parameters += QString(" Engine Fast");

	switch (getVolumeFollowMode())
	{
	case LoudnessCorrectionFilter::FilterParameters::VOLUME_FOLLOW_LINEAR:
		parameters += QString(" VolumeFollow Linear");
		break;
	case LoudnessCorrectionFilter::FilterParameters::VOLUME_FOLLOW_LOGARITHMIC:
		parameters += QString(" VolumeFollow Logarithmic");
		break;
	case LoudnessCorrectionFilter::FilterParameters::VOLUME_FOLLOW_WINDOWS:
		parameters += QString(" VolumeFollow Windows");
		break;
	default:
		break;
	}
}

LoudnessCorrectionFilter::FilterParameters::BindingMode
LoudnessCorrectionFilterGUI::getBindingMode() const
{
	return ui->bindingComboBox->currentIndex() == 1 ?
		LoudnessCorrectionFilter::FilterParameters::BINDING_ALL :
		LoudnessCorrectionFilter::FilterParameters::BINDING_SINGLE;
}

LoudnessCorrectionFilter::FilterParameters::VolumeFollowMode
LoudnessCorrectionFilterGUI::getVolumeFollowMode() const
{
	switch (ui->volumeFollowComboBox->currentIndex())
	{
	case 1:
		return LoudnessCorrectionFilter::FilterParameters::VOLUME_FOLLOW_LINEAR;
	case 2:
		return LoudnessCorrectionFilter::FilterParameters::VOLUME_FOLLOW_LOGARITHMIC;
	case 3:
		return LoudnessCorrectionFilter::FilterParameters::VOLUME_FOLLOW_WINDOWS;
	default:
		return LoudnessCorrectionFilter::FilterParameters::VOLUME_FOLLOW_OFF;
	}
}

std::wstring LoudnessCorrectionFilterGUI::getRequestedEndpointId() const
{
	if (getBindingMode() == LoudnessCorrectionFilter::FilterParameters::BINDING_ALL)
		return L"";
	return endpointIdentifier;
}

void LoudnessCorrectionFilterGUI::refreshVolumeController()
{
	volumeController.reset();
	endpointId.clear();
	automaticVolumeAvailable = false;
	lastVolume = std::numeric_limits<double>::quiet_NaN();
	if (getBindingMode() ==
		LoudnessCorrectionFilter::FilterParameters::BINDING_SINGLE &&
		!selectedEndpointIsRender)
		return;

	std::wstring requestedEndpointId = getRequestedEndpointId();
	if (getBindingMode() == LoudnessCorrectionFilter::FilterParameters::BINDING_SINGLE &&
		requestedEndpointId.empty())
	{
		return;
	}

	// Resolve the actual endpoint used by either binding strategy. Calibration
	// guards and the displayed automatic value both use this resolved identity.
	volumeController.reset(new VolumeController(requestedEndpointId));
	EndpointVolumeState endpointVolumeState;
	if (FAILED(volumeController->getVolumeState(endpointVolumeState)) ||
		!std::isfinite(endpointVolumeState.levelDb) ||
		!std::isfinite(endpointVolumeState.scalar))
	{
		volumeController.reset();
		return;
	}

	automaticVolumeAvailable = true;
	lastEndpointState = endpointVolumeState;
	lastVolume = endpointVolumeState.levelDb;
	endpointId = volumeController->getEndpointId();
}

void LoudnessCorrectionFilterGUI::updateAutomaticVolumeUi()
{
	ui->manualVolumeCheckBox->setText(tr("Manual volume:"));
	if (automaticVolumeAvailable || ui->manualVolumeCheckBox->isChecked())
	{
		ui->manualVolumeCheckBox->setToolTip(tr(
			"Off: describes external attenuation only. With volume follow enabled, this controls APO gain. For direct dB control choose Follow dB. Linear/Squared retain the legacy mapping: -50 dB input means 50% control position, not -50 dB output."));
		ui->volumeSpinBox->setSpecialValueText(QString());
	}
	else
	{
		ui->manualVolumeCheckBox->setToolTip(tr(
			"Automatic volume is temporarily unavailable. The saved automatic mode is preserved and runtime correction pauses safely until the endpoint returns. Enable manual volume only if you want to replace automatic tracking."));
		if (!ui->manualVolumeCheckBox->isChecked())
		{
			ui->volumeSpinBox->setSpecialValueText(tr("Unavailable"));
			ui->volumeSpinBox->setValue(ui->volumeSpinBox->minimum());
		}
	}
	ui->volumeSpinBox->setToolTip(ui->manualVolumeCheckBox->toolTip());
	ui->volumeSpinBox->setAccessibleDescription(
		ui->manualVolumeCheckBox->toolTip());
}

void LoudnessCorrectionFilterGUI::updateVolumeReadout()
{
	const bool manual = ui->manualVolumeCheckBox->isChecked();
	const auto mode = getVolumeFollowMode();
	const double sourceDb = manual ? ui->volumeSpinBox->value() : lastEndpointState.levelDb;
	const double scalar = manual ? (sourceDb + 100.0) / 100.0 : lastEndpointState.scalar;
	const bool available = manual || automaticVolumeAvailable;
	ui->volumeSourceLabel->setText(available ?
		(manual ? tr("Manual input: %1 dB · control %2%") :
			tr("Endpoint: %1 dB · control %2%"))
			.arg(sourceDb, 0, 'f', 1).arg(qBound(0.0, scalar * 100.0, 100.0), 0, 'f', 1) :
		tr("Endpoint unavailable"));
	QString target;
	if (!state)
		target = tr("APO follow target: 0.00 dB (bypassed)");
	else if (mode == LoudnessCorrectionFilter::FilterParameters::VOLUME_FOLLOW_OFF)
		target = tr("APO follow target: 0.00 dB (off)");
	else if (!available)
		target = tr("APO follow target: unknown (source unavailable)");
	else
	{
		const double gain = LoudnessCorrectionFilter::calculateVolumeFollowGain(
			mode, sourceDb, scalar, !manual && lastEndpointState.muted);
		target = gain == 0.0 ? tr("APO follow target: muted (-inf dB)") :
			tr("APO follow target: %1 dB").arg(20.0 * std::log10(gain), 0, 'f', 2);
	}
	ui->volumeFollowStatusLabel->setText(target);
	ui->volumeFollowStatusLabel->setToolTip(tr(
		"Calculated target for this row, not a measurement or confirmation that APO is active. Excludes EQ/headroom and Windows/hardware attenuation. Apply/save changes to affect audio. Source loss holds the runtime's last gain; cold start without a source is muted. Bypassing this row removes its attenuation."));
	ui->volumeFollowStatusLabel->setAccessibleDescription(
		target + QStringLiteral(". ") + ui->volumeFollowStatusLabel->toolTip());
}

void LoudnessCorrectionFilterGUI::on_refLevelSpinBox_valueChanged(int value)
{
	VolumeTakeoverManager::instance()->setReferenceParameters(
		value, ui->refOffsetSpinBox->value());
	emit updateModel();
}

void LoudnessCorrectionFilterGUI::on_refOffsetSpinBox_valueChanged(int value)
{
	VolumeTakeoverManager::instance()->setReferenceParameters(
		ui->refLevelSpinBox->value(), value);
	emit updateModel();
}

void LoudnessCorrectionFilterGUI::on_attDial_valueChanged(int value)
{
	ui->attSpinBox->setValue(value / 100.0);
}

void LoudnessCorrectionFilterGUI::on_attSpinBox_valueChanged(double value)
{
	bool previousValue = ui->attDial->blockSignals(true);
	ui->attDial->setValue(round(value * 100.0));
	ui->attDial->blockSignals(previousValue);

	emit updateModel();
}

void LoudnessCorrectionFilterGUI::on_bindingComboBox_currentIndexChanged(int index)
{
	(void)index;
	bool manual = ui->manualVolumeCheckBox->isChecked();
	double manualVolume = ui->volumeSpinBox->value();
	refreshVolumeController();
	updateAutomaticVolumeUi();

	if (manual)
	{
		ui->volumeSpinBox->setValue(manualVolume);
		ui->volumeSpinBox->setEnabled(true);
	}
	else if (automaticVolumeAvailable)
	{
		ui->volumeSpinBox->setSpecialValueText(QString());
		ui->volumeSpinBox->setValue(lastVolume);
		ui->volumeSpinBox->setEnabled(false);
	}
	else
	{
		ui->volumeSpinBox->setSpecialValueText(tr("Unavailable"));
		ui->volumeSpinBox->setValue(ui->volumeSpinBox->minimum());
		ui->volumeSpinBox->setEnabled(false);
	}

	updateVolumeReadout();
	emit updateModel();
}

void LoudnessCorrectionFilterGUI::on_manualVolumeCheckBox_toggled(bool checked)
{
	ui->volumeSpinBox->setEnabled(checked);
	if (checked)
	{
		ui->volumeSpinBox->setSpecialValueText(QString());
		if (!std::isfinite(lastVolume))
			lastVolume = -20.0;
		ui->volumeSpinBox->setValue(lastVolume);
		lastVolume = ui->volumeSpinBox->value();
	}
	else
	{
		// Use the same Single/render/identity guards as initial binding.
		refreshVolumeController();
		if (!automaticVolumeAvailable)
		{
			updateAutomaticVolumeUi();
			updateVolumeReadout();
			ui->volumeSpinBox->setEnabled(false);
			if (isVisible())
				QToolTip::showText(
					ui->manualVolumeCheckBox->mapToGlobal(
						QPoint(0, ui->manualVolumeCheckBox->height())),
					ui->manualVolumeCheckBox->toolTip(),
					ui->manualVolumeCheckBox);
			emit updateModel();
			return;
		}
		ui->volumeSpinBox->setSpecialValueText(QString());
		updateAutomaticVolumeUi();
		ui->volumeSpinBox->setValue(lastVolume);
	}
	updateAutomaticVolumeUi();
	updateVolumeReadout();
	emit updateModel();
}

void LoudnessCorrectionFilterGUI::on_volumeSpinBox_valueChanged(double value)
{
	if (ui->manualVolumeCheckBox->isChecked())
	{
		lastVolume = value;
		if (volumeController)
			volumeController->setVolume(value);
		updateVolumeReadout();
		emit updateModel();
	}
}

void LoudnessCorrectionFilterGUI::on_fastEngineCheckBox_toggled(bool checked)
{
	(void)checked;
	emit updateModel();
}

void LoudnessCorrectionFilterGUI::on_volumeFollowComboBox_currentIndexChanged(
	int index)
{
	(void)index;
	updateAutomaticVolumeUi();
	updateVolumeReadout();
	emit updateModel();
}

void LoudnessCorrectionFilterGUI::on_studioButton_clicked()
{
	LoudnessCorrectionStudioDialog dialog(
		ui->refLevelSpinBox->value(),
		ui->refOffsetSpinBox->value(),
		ui->attSpinBox->value(),
		getBindingMode() == LoudnessCorrectionFilter::FilterParameters::BINDING_ALL,
		ui->manualVolumeCheckBox->isChecked(),
		ui->volumeSpinBox->value(),
		automaticVolumeAvailable,
		getVolumeFollowMode(),
		lastEndpointState.scalar,
		lastEndpointState.levelDb,
		this);
	if (dialog.exec() != QDialog::Accepted)
		return;

	{
		QSignalBlocker refLevelDialBlocker(ui->refLevelDial);
		QSignalBlocker refLevelSpinBlocker(ui->refLevelSpinBox);
		QSignalBlocker refOffsetDialBlocker(ui->refOffsetDial);
		QSignalBlocker refOffsetSpinBlocker(ui->refOffsetSpinBox);
		QSignalBlocker attenuationDialBlocker(ui->attDial);
		QSignalBlocker attenuationSpinBlocker(ui->attSpinBox);
		QSignalBlocker bindingBlocker(ui->bindingComboBox);

		ui->refLevelDial->setValue(dialog.getReferenceLevel());
		ui->refLevelSpinBox->setValue(dialog.getReferenceLevel());
		ui->refOffsetDial->setValue(dialog.getReferenceOffset());
		ui->refOffsetSpinBox->setValue(dialog.getReferenceOffset());
		ui->attDial->setValue(qRound(dialog.getAttenuation() * 100.0));
		ui->attSpinBox->setValue(dialog.getAttenuation());
		ui->bindingComboBox->setCurrentIndex(dialog.getGlobalBinding() ? 1 : 0);
	}

	refreshVolumeController();
	updateAutomaticVolumeUi();
	const bool useManualVolume = dialog.getUseManualVolume();
	{
		QSignalBlocker manualBlocker(ui->manualVolumeCheckBox);
		QSignalBlocker volumeBlocker(ui->volumeSpinBox);
		ui->manualVolumeCheckBox->setChecked(useManualVolume);
		ui->volumeSpinBox->setEnabled(useManualVolume);
		if (useManualVolume)
		{
			ui->volumeSpinBox->setValue(dialog.getVolume());
			lastVolume = dialog.getVolume();
		}
		else
		{
			if (automaticVolumeAvailable)
			{
				ui->volumeSpinBox->setSpecialValueText(QString());
				ui->volumeSpinBox->setValue(lastVolume);
			}
			else
			{
				ui->volumeSpinBox->setSpecialValueText(tr("Unavailable"));
				ui->volumeSpinBox->setValue(ui->volumeSpinBox->minimum());
			}
		}
	}

	updateAutomaticVolumeUi();
	updateVolumeReadout();
	emit updateModel();
	if (dialog.shouldCalibrateAfterApply())
		on_calibrateButton_clicked();
}

void LoudnessCorrectionFilterGUI::on_calibrateButton_clicked()
{
	EndpointVolumeState calibrationEndpointState;
	if (!tryReadEndpointVolumeState(calibrationEndpointState))
	{
		QMessageBox::warning(
			this,
			tr("Calibration not applied"),
			tr("The bound playback volume could not be read. Reconnect the device or choose another readable playback binding, then try again."));
		return;
	}
	if (calibrationEndpointState.muted || calibrationEndpointState.scalar <= 0.0)
	{
		QMessageBox::warning(
			this,
			tr("Calibration not applied"),
			tr("The bound playback endpoint is muted or set to zero volume. Unmute it and raise the Windows volume before calibrating."));
		return;
	}
	if (!ui->manualVolumeCheckBox->isChecked())
	{
		lastVolume = calibrationEndpointState.levelDb;
		ui->volumeSpinBox->setValue(lastVolume);
	}

	const bool keepVolumeFollow =
		getVolumeFollowMode() !=
		LoudnessCorrectionFilter::FilterParameters::VOLUME_FOLLOW_OFF;
	const bool previousState = state;
	const double previousAttenuation = ui->attSpinBox->value();
	if (keepVolumeFollow)
	{
		// Keep the APO-owned master-volume attenuation active while removing
		// only the equal-loudness contour. A full State 0 bypass can make a
		// Matrix route jump to unity gain during calibration.
		state = true;
		QSignalBlocker attenuationDialBlocker(ui->attDial);
		QSignalBlocker attenuationSpinBlocker(ui->attSpinBox);
		ui->attDial->setValue(0);
		ui->attSpinBox->setValue(0.0);
	}
	else
	{
		state = false;
	}
	QString temporaryCommand;
	QString temporaryParameters;
	store(temporaryCommand, temporaryParameters);
	state = previousState;
	if (keepVolumeFollow)
	{
		QSignalBlocker attenuationDialBlocker(ui->attDial);
		QSignalBlocker attenuationSpinBlocker(ui->attSpinBox);
		ui->attDial->setValue(qRound(previousAttenuation * 100.0));
		ui->attSpinBox->setValue(previousAttenuation);
	}

	MainWindow* mainWindow = qobject_cast<MainWindow*>(window());
	if (mainWindow == NULL || !mainWindow->beginTemporaryFilterConfiguration(
		this, temporaryCommand, temporaryParameters,
		QStringLiteral("loudness-calibration")))
	{
		QMessageBox::warning(
			this,
			tr("Calibration not started"),
			tr("Save the current profile and resolve any temporary audio state before calibrating."));
		return;
	}

	LoudnessCorrectionFilterGUIDialog dialog(
		endpointId,
		automaticVolumeAvailable,
		getBindingMode() == LoudnessCorrectionFilter::FilterParameters::BINDING_ALL);
	const int dialogResult = dialog.exec();
	bool keptExternal = false;
	if (!mainWindow->restoreTemporaryFilterConfiguration(&keptExternal))
	{
		QMessageBox::critical(
			this,
			tr("Audio processing was not restored"),
			tr("The temporary calibration state could not be restored automatically. Close the editor and use temporary audio recovery before continuing."));
		return;
	}
	if (keptExternal)
		return;

	if (dialogResult == QDialog::Accepted)
	{
		if (!dialog.hasValidMeasurement() ||
			!tryReadEndpointVolumeState(calibrationEndpointState) ||
			calibrationEndpointState.muted ||
			calibrationEndpointState.scalar <= 0.0)
		{
			QMessageBox::warning(
				this,
				tr("Calibration not applied"),
				tr("Playback became muted, inaudible, or unavailable. The measured level was not applied; restore the endpoint and run calibration again."));
			return;
		}
		if (!ui->manualVolumeCheckBox->isChecked())
		{
			lastVolume = calibrationEndpointState.levelDb;
			ui->volumeSpinBox->setValue(lastVolume);
		}
		double measuredSpl = dialog.getMeasuredLevel();
		double effectiveVolume = lastVolume;
		if (keepVolumeFollow)
		{
			double followVolumeDb = lastVolume;
			double followScalar = calibrationEndpointState.scalar;
			if (ui->manualVolumeCheckBox->isChecked())
			{
				followVolumeDb = ui->volumeSpinBox->value();
				followScalar = (followVolumeDb + 100.0) / 100.0;
				if (followScalar < 0.0) followScalar = 0.0;
				if (followScalar > 1.0) followScalar = 1.0;
			}
			const double followGain =
				LoudnessCorrectionFilter::calculateVolumeFollowGain(
					getVolumeFollowMode(), followVolumeDb, followScalar, false);
			if (!std::isfinite(followGain) || followGain <= 0.0)
			{
				QMessageBox::warning(
					this,
					tr("Calibration not applied"),
					tr("The selected volume-follow curve produced an inaudible level. Raise the volume and run calibration again."));
				return;
			}
			effectiveVolume = 20.0 * std::log10(followGain);
		}
		double refLevel = measuredSpl - effectiveVolume +
			ui->refOffsetSpinBox->value();
		if (refLevel < 1.0) refLevel = 1.0;
		if (refLevel > 100.0) refLevel = 100.0;
		ui->refLevelSpinBox->setValue((int)round(refLevel));
	}

}

bool LoudnessCorrectionFilterGUI::tryReadEndpointVolumeState(
	EndpointVolumeState& volumeState)
{
	if (!volumeController)
		return false;
	const HRESULT result = volumeController->getVolumeState(volumeState);
	if (FAILED(result) || !std::isfinite(volumeState.levelDb) ||
		!std::isfinite(volumeState.scalar))
	{
		return false;
	}
	endpointId = volumeController->getEndpointId();
	lastEndpointState = volumeState;
	return !endpointId.empty();
}

bool LoudnessCorrectionFilterGUI::tryUpdateVolume()
{
	if (ui->manualVolumeCheckBox->isChecked())
		return false;
	if (!volumeController)
		refreshVolumeController();
	if (!volumeController)
	{
		updateAutomaticVolumeUi();
		return false;
	}

	EndpointVolumeState volumeState;
	if (tryReadEndpointVolumeState(volumeState))
	{
		if (!automaticVolumeAvailable)
		{
			automaticVolumeAvailable = true;
			updateAutomaticVolumeUi();
		}
		const double volume = volumeState.levelDb;
		if (!std::isfinite(lastVolume) || std::abs(volume - lastVolume) > 0.05)
			ui->volumeSpinBox->setValue(volume);
		lastVolume = volume;
		return true;
	}

	automaticVolumeAvailable = false;
	lastVolume = std::numeric_limits<double>::quiet_NaN();
	volumeController.reset();
	updateAutomaticVolumeUi();
	return false;
}

void LoudnessCorrectionFilterGUI::updateVolume()
{
	(void)tryUpdateVolume();
	updateVolumeReadout();
}


int LoudnessCorrectionFilterGUI::preferredHeight() const
{
	// Include panel margins, the shared module header and wrapped status text.
	// Summing selected child controls undercounts the redesigned nested layout.
	return qMax(QWidget::sizeHint().height(), layout()->totalHeightForWidth(width()));
}

QSize LoudnessCorrectionFilterGUI::sizeHint() const
{
	QSize size = QWidget::sizeHint();
	size.setHeight(preferredHeight());
	return size;
}

QSize LoudnessCorrectionFilterGUI::minimumSizeHint() const
{
	QSize size = QWidget::minimumSizeHint();
	size.setWidth(0);
	size.setHeight(preferredHeight());
	return size;
}
