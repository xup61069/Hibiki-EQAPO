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

#include <QFile>
#include <QMessageBox>
#include <QPushButton>
#include <QStyle>
#include <cmath>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <mmdeviceapi.h>
#include "Editor/helpers/QtSndfileHandle.h"

#include "LoudnessCorrectionFilterGUIDialog.h"
#include "ui_LoudnessCorrectionFilterGUIDialog.h"

namespace
{
	bool generate1kHzTone(
		QBuffer& buffer,
		bool leftEnabled,
		bool rightEnabled,
		int sampleRate = 48000)
	{
		if (!buffer.open(QIODevice::WriteOnly))
			return false;

		bool writeSucceeded = true;
		{
			QtSndfileHandle bufferHandle(
				buffer, SFM_WRITE, SF_FORMAT_WAV | SF_FORMAT_PCM_24, 2, sampleRate);
			if (bufferHandle.error() != SF_ERR_NO_ERROR)
			{
				buffer.close();
				return false;
			}

			// 1000.0 Hz pure tone: exactly 48 samples per cycle at 48000 Hz.
			// 48000 samples = 1000 full cycles (1.0 s), seamless looping without phase discontinuity.
			const double twoPiFreq = 2.0 * 3.14159265358979323846 * 1000.0;
			// RMS = 0.02210515454670527 matches pinkNoise.flac (-33.11 dBFS RMS)
			// Amplitude = RMS * sqrt(2) = 0.03126144 (-30.10 dBFS peak)
			const double amplitude = 0.02210515454670527 * 1.41421356237309504880;

			const int chunkSize = 1024;
			double stereo[2 * chunkSize];
			for (int offset = 0; offset < sampleRate && writeSucceeded; offset += chunkSize)
			{
				const int count = (std::min)(chunkSize, sampleRate - offset);
				for (int i = 0; i < count; ++i)
				{
					const double t = static_cast<double>(offset + i) / sampleRate;
					const double sample = amplitude * std::sin(twoPiFreq * t);
					stereo[2 * i] = leftEnabled ? sample : 0.0;
					stereo[2 * i + 1] = rightEnabled ? sample : 0.0;
				}
				writeSucceeded = bufferHandle.write(stereo, 2 * count) == (2 * count);
			}
		}
		buffer.close();
		return writeSucceeded && buffer.size() > 0;
	}

	bool isDefaultRenderEndpoint(
		const std::wstring& endpointId,
		ERole role)
	{
		if (endpointId.empty())
			return false;

		HRESULT initResult = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
		bool uninitialize = SUCCEEDED(initResult);
		IMMDeviceEnumerator* enumerator = NULL;
		IMMDevice* device = NULL;
		LPWSTR defaultEndpointId = NULL;
		bool matches = false;
		HRESULT result = CoCreateInstance(
			__uuidof(MMDeviceEnumerator), NULL, CLSCTX_INPROC_SERVER,
			__uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator));
		if (SUCCEEDED(result) && enumerator != NULL)
			result = enumerator->GetDefaultAudioEndpoint(eRender, role, &device);
		if (SUCCEEDED(result) && device != NULL)
			result = device->GetId(&defaultEndpointId);
		if (SUCCEEDED(result) && defaultEndpointId != NULL)
			matches = _wcsicmp(defaultEndpointId, endpointId.c_str()) == 0;

		if (defaultEndpointId != NULL)
			CoTaskMemFree(defaultEndpointId);
		if (device != NULL)
			device->Release();
		if (enumerator != NULL)
			enumerator->Release();
		if (uninitialize)
			CoUninitialize();
		return matches;
	}
}

LoudnessCorrectionFilterGUIDialog::LoudnessCorrectionFilterGUIDialog(
	const std::wstring& endpointId,
	bool automaticVolumeAvailable,
	bool followsDefaultMultimedia,
	QWidget* parent)
	: QDialog(parent),
	ui(new Ui::LoudnessCorrectionFilterGUIDialog),
	endpointId(endpointId),
	followsDefaultMultimedia(followsDefaultMultimedia),
	playbackUsesSelectedEndpoint(
		automaticVolumeAvailable &&
		isDefaultRenderEndpoint(endpointId, eConsole) &&
		(!followsDefaultMultimedia ||
			isDefaultRenderEndpoint(endpointId, eMultimedia))),
	measurementValid(false),
	volumeController(automaticVolumeAvailable
		? new VolumeController(endpointId) : nullptr)
{
	ui->setupUi(this);
	ui->levelSpinBox->setRange(1, 100);
	ui->levelSpinBox->setSuffix(tr(" dB SPL"));
	ui->levelSpinBox->setToolTip(tr(
		"Enter the slow-response, Z-weighted (flat) reading measured at the listening position."));
	ui->levelSpinBox->setAccessibleName(tr("Measured sound pressure level"));
	ui->safetyStatusLabel->setProperty("statusLevel", "warning");
	ui->stopButton->setEnabled(false);
	if (QPushButton* saveButton = ui->buttonBox->button(QDialogButtonBox::Save))
	{
		saveButton->setText(tr("Use measurement"));
		saveButton->setEnabled(false);
	}
	// The procedure is defined for one speaker. Playing both changes the level
	// at the meter and makes the resulting reference ambiguous.
	ui->bothRadioButton->hide();
	updateSignalUi();
	ui->playButton->setToolTip(tr(
		"The selected playback device must remain the audible Windows default playback device. "
		"Muted, zero-volume, unreadable, or mismatched endpoints block the calibration signal."));
	(void)updatePlaybackReadiness(false);
	connect(&endpointGuardTimer, &QTimer::timeout, this, [this]() {
		const bool wasPlaying = buffer.size() > 0;
		(void)updatePlaybackReadiness(wasPlaying);
	});
	endpointGuardTimer.start(250);
}

void LoudnessCorrectionFilterGUIDialog::setPlaybackStatus(
	const QString& text,
	const char* level)
{
	ui->playbackStatusLabel->setText(text);
	ui->playbackStatusLabel->setAccessibleName(tr("Calibration playback status"));
	ui->playbackStatusLabel->setAccessibleDescription(text);
	ui->playbackStatusLabel->setProperty("statusLevel", QString::fromLatin1(level));
	ui->playbackStatusLabel->style()->unpolish(ui->playbackStatusLabel);
	ui->playbackStatusLabel->style()->polish(ui->playbackStatusLabel);
	ui->playbackStatusLabel->update();
}

bool LoudnessCorrectionFilterGUIDialog::isPlaybackEndpointStillValid() const
{
	// PlaySound/Wave routing follows eConsole. Global binding reads the
	// eMultimedia endpoint, so both roles must still resolve to the controller's
	// actual endpoint before calibration noise is allowed to play.
	return playbackUsesSelectedEndpoint &&
		isDefaultRenderEndpoint(endpointId, eConsole) &&
		(!followsDefaultMultimedia ||
			isDefaultRenderEndpoint(endpointId, eMultimedia));
}

LoudnessCorrectionFilterGUIDialog::PlaybackReadiness
LoudnessCorrectionFilterGUIDialog::getPlaybackReadiness()
{
	if (!isPlaybackEndpointStillValid())
		return PLAYBACK_ENDPOINT_MISMATCH;
	if (!volumeController)
		return PLAYBACK_VOLUME_UNAVAILABLE;

	EndpointVolumeState volumeState;
	if (FAILED(volumeController->getVolumeState(volumeState)) ||
		!std::isfinite(volumeState.levelDb) ||
		!std::isfinite(volumeState.scalar) ||
		_wcsicmp(volumeController->getEndpointId().c_str(), endpointId.c_str()) != 0)
	{
		return PLAYBACK_VOLUME_UNAVAILABLE;
	}
	if (volumeState.muted || volumeState.scalar <= 0.0)
		return PLAYBACK_MUTED;
	return PLAYBACK_READY;
}

bool LoudnessCorrectionFilterGUIDialog::updatePlaybackReadiness(
	bool showWarning)
{
	const PlaybackReadiness readiness = getPlaybackReadiness();
	if (readiness == PLAYBACK_READY)
	{
		if (buffer.size() == 0)
		{
			ui->playButton->setEnabled(true);
			ui->stopButton->setEnabled(false);
			setPlaybackStatus(tr("Ready to play"), "normal");
		}
		return true;
	}

	const bool wasPlaying = buffer.size() > 0;
	if (wasPlaying)
		stopPlayback();
	measurementValid = false;
	if (QPushButton* saveButton =
		ui->buttonBox->button(QDialogButtonBox::Save))
	{
		saveButton->setEnabled(false);
	}
	ui->playButton->setEnabled(false);
	ui->stopButton->setEnabled(false);

	QString title;
	QString message;
	if (readiness == PLAYBACK_ENDPOINT_MISMATCH)
	{
		setPlaybackStatus(
			wasPlaying
				? tr("Playback device changed · signal stopped")
				: tr("Playback blocked · default device mismatch"),
			"danger");
		title = tr("Playback device mismatch");
		message = wasPlaying
			? tr("The Windows default playback device changed. Test noise was stopped to prevent calibration on the wrong speaker.")
			: tr("Make the selected playback device the Windows default playback device, then reopen calibration. No test noise was played.");
	}
	else if (readiness == PLAYBACK_MUTED)
	{
		setPlaybackStatus(
			wasPlaying
				? tr("Endpoint muted - signal stopped")
				: tr("Playback blocked - endpoint is muted or at zero volume"),
			"danger");
		title = tr("Playback endpoint is inaudible");
		message = wasPlaying
			? tr("The playback endpoint was muted or set to zero volume. Test noise was stopped and this measurement cannot be used.")
			: tr("Unmute the playback endpoint and raise the Windows volume before playing the calibration signal.");
	}
	else
	{
		setPlaybackStatus(
			wasPlaying
				? tr("Endpoint unavailable - signal stopped")
				: tr("Playback blocked - endpoint volume is unavailable"),
			"danger");
		title = tr("Playback endpoint unavailable");
		message = wasPlaying
			? tr("The playback endpoint volume became unreadable. Test noise was stopped and this measurement cannot be used.")
			: tr("The playback endpoint volume cannot be read. Reconnect the device before calibrating.");
	}

	if (showWarning)
		QMessageBox::warning(this, title, message);
	return false;
}

void LoudnessCorrectionFilterGUIDialog::stopPlayback()
{
	if (buffer.size() > 0)
	{
		(void)PlaySoundA(NULL, NULL, 0);
		buffer.buffer().clear();
	}
}

LoudnessCorrectionFilterGUIDialog::~LoudnessCorrectionFilterGUIDialog()
{
	on_stopButton_clicked();
	delete ui;
}

int LoudnessCorrectionFilterGUIDialog::getMeasuredLevel()
{
	return ui->levelSpinBox->value();
}

bool LoudnessCorrectionFilterGUIDialog::hasValidMeasurement() const
{
	return measurementValid;
}

void LoudnessCorrectionFilterGUIDialog::accept()
{
	// The timer is intentionally only a periodic guard. Close its final race
	// window by revalidating the exact playback route and endpoint volume at the
	// moment the measurement is accepted.
	if (!measurementValid || !updatePlaybackReadiness(true))
		return;
	stopPlayback();
	QDialog::accept();
}

void LoudnessCorrectionFilterGUIDialog::reject()
{
	stopPlayback();
	QDialog::reject();
}

void LoudnessCorrectionFilterGUIDialog::on_playButton_clicked()
{
	// Recheck immediately before playback: the Windows default can change while
	// the dialog is open, and PlaySound would then route to a different speaker.
	if (!updatePlaybackReadiness(true))
		return;

	if (buffer.size() > 0)
		on_stopButton_clicked();
	measurementValid = false;
	if (QPushButton* saveButton =
		ui->buttonBox->button(QDialogButtonBox::Save))
	{
		saveButton->setEnabled(false);
	}

	const bool use1kHz = ui->sine1kHzRadioButton->isChecked();
	const bool leftEnabled = ui->leftRadioButton->isChecked() || ui->bothRadioButton->isChecked();
	const bool rightEnabled = ui->rightRadioButton->isChecked() || ui->bothRadioButton->isChecked();

	if (use1kHz)
	{
		setPlaybackStatus(tr("Preparing 1 kHz reference tone…"), "normal");
		if (!generate1kHzTone(buffer, leftEnabled, rightEnabled, 48000))
		{
			buffer.buffer().clear();
			setPlaybackStatus(tr("Calibration signal could not be prepared"), "danger");
			return;
		}
	}
	else
	{
		setPlaybackStatus(tr("Preparing pink noise…"), "normal");

		QFile file(":/sounds/pinkNoise.flac");
		if (!file.open(QIODevice::ReadOnly))
		{
			setPlaybackStatus(tr("Pink-noise resource could not be opened"), "danger");
			return;
		}

		QtSndfileHandle fileHandle(file, SFM_READ);
		if (fileHandle.error() != SF_ERR_NO_ERROR || fileHandle.samplerate() <= 0)
		{
			setPlaybackStatus(tr("Pink-noise resource could not be decoded"), "danger");
			return;
		}

		if (!buffer.open(QIODevice::WriteOnly))
		{
			setPlaybackStatus(tr("Calibration audio buffer could not be created"), "danger");
			return;
		}

		bool writeSucceeded = true;
		{
			QtSndfileHandle bufferHandle(buffer, SFM_WRITE, SF_FORMAT_WAV | SF_FORMAT_PCM_24, 2, fileHandle.samplerate());
			if (bufferHandle.error() != SF_ERR_NO_ERROR)
				writeSucceeded = false;

			int buf[1024];
			int buf2[2 * ARRAYSIZE(buf)];
			int samplesRead;
			while (writeSucceeded && (samplesRead = fileHandle.read(buf, ARRAYSIZE(buf))) > 0)
			{
				for (int i = 0; i < samplesRead; i++)
				{
					buf2[2 * i] = leftEnabled ? buf[i] : 0;
					buf2[2 * i + 1] = rightEnabled ? buf[i] : 0;
				}
				writeSucceeded = bufferHandle.write(buf2, 2 * samplesRead) == 2 * samplesRead;
			}
		}
		buffer.close();

		if (!writeSucceeded || buffer.size() == 0)
		{
			buffer.buffer().clear();
			setPlaybackStatus(tr("Calibration signal could not be prepared"), "danger");
			return;
		}
	}

	// Decoding above is synchronous, so the Qt timer cannot observe a default
	// endpoint switch while it runs. Recheck after the buffer is complete and
	// immediately before PlaySound chooses its destination.
	if (!updatePlaybackReadiness(true))
	{
		buffer.buffer().clear();
		return;
	}

	if (!PlaySoundA(buffer.data().data(), NULL, SND_MEMORY | SND_ASYNC | SND_LOOP))
	{
		buffer.buffer().clear();
		setPlaybackStatus(tr("Windows could not start calibration playback"), "danger");
		return;
	}
	ui->playButton->setEnabled(false);
	ui->stopButton->setEnabled(true);
	measurementValid = true;
	if (QPushButton* saveButton =
		ui->buttonBox->button(QDialogButtonBox::Save))
	{
		saveButton->setEnabled(true);
	}
	if (use1kHz)
	{
		setPlaybackStatus(
			ui->leftRadioButton->isChecked()
				? tr("Playing 1 kHz tone on the left speaker")
				: tr("Playing 1 kHz tone on the right speaker"),
			"warning");
	}
	else
	{
		setPlaybackStatus(
			ui->leftRadioButton->isChecked()
				? tr("Playing pink noise on the left speaker")
				: tr("Playing pink noise on the right speaker"),
			"warning");
	}
}

void LoudnessCorrectionFilterGUIDialog::on_stopButton_clicked()
{
	stopPlayback();
	(void)updatePlaybackReadiness(false);
}

void LoudnessCorrectionFilterGUIDialog::on_leftRadioButton_toggled(bool checked)
{
	if (checked && buffer.size() > 0)
		on_playButton_clicked();
}

void LoudnessCorrectionFilterGUIDialog::on_rightRadioButton_toggled(bool checked)
{
	if (checked && buffer.size() > 0)
		on_playButton_clicked();
}

void LoudnessCorrectionFilterGUIDialog::on_bothRadioButton_toggled(bool checked)
{
	if (checked && buffer.size() > 0)
		on_playButton_clicked();
}

void LoudnessCorrectionFilterGUIDialog::on_sine1kHzRadioButton_toggled(bool checked)
{
	if (checked)
	{
		updateSignalUi();
		if (buffer.size() > 0)
			on_playButton_clicked();
	}
}

void LoudnessCorrectionFilterGUIDialog::on_pinkNoiseRadioButton_toggled(bool checked)
{
	if (checked)
	{
		updateSignalUi();
		if (buffer.size() > 0)
			on_playButton_clicked();
	}
}

void LoudnessCorrectionFilterGUIDialog::updateSignalUi()
{
	const bool use1kHz = ui->sine1kHzRadioButton->isChecked();
	if (use1kHz)
	{
		ui->signalHintLabel->setText(tr(
			"1 kHz pure tone is the ISO 226 reference where dB SPL equals phon level. "
			"Recommended for headphones, near-field monitoring, or calibrators without room acoustic bias."));
		ui->playButton->setText(tr("Play 1 kHz tone"));
		ui->playButton->setAccessibleDescription(tr(
			"Starts a looping 1 kHz calibration sine tone on the selected speaker"));
	}
	else
	{
		ui->signalHintLabel->setText(tr(
			"Pink noise distributes equal energy per octave across the spectrum. "
			"Recommended for stereo speakers and room measurements with an SPL meter set to Z or C weighting and Slow response."));
		ui->playButton->setText(tr("Play pink noise"));
		ui->playButton->setAccessibleDescription(tr(
			"Starts a looping pink-noise signal on the selected speaker"));
	}
}
