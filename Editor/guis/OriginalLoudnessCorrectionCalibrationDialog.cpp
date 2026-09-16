/*
    This file is part of Equalizer APO, a system-wide equalizer.
    Copyright (C) 2026 Equalizer APO contributors

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
*/

#include "OriginalLoudnessCorrectionCalibrationDialog.h"

#include <QDialogButtonBox>
#include <QFile>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QRadioButton>
#include <QScreen>
#include <QSizePolicy>
#include <QSpinBox>
#include <QStyle>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmdeviceapi.h>
#include <mmsystem.h>

#include "Editor/helpers/QtSndfileHandle.h"
#include "Editor/helpers/GUIHelper.h"

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

		const HRESULT initResult = CoInitializeEx(
			NULL, COINIT_APARTMENTTHREADED);
		const bool uninitialize = SUCCEEDED(initResult);
		IMMDeviceEnumerator* enumerator = NULL;
		IMMDevice* device = NULL;
		LPWSTR defaultEndpointId = NULL;
		bool matches = false;
		HRESULT result = CoCreateInstance(
			__uuidof(MMDeviceEnumerator), NULL, CLSCTX_INPROC_SERVER,
			__uuidof(IMMDeviceEnumerator),
			reinterpret_cast<void**>(&enumerator));
		if (SUCCEEDED(result) && enumerator != NULL)
			result = enumerator->GetDefaultAudioEndpoint(eRender, role, &device);
		if (SUCCEEDED(result) && device != NULL)
			result = device->GetId(&defaultEndpointId);
		if (SUCCEEDED(result) && defaultEndpointId != NULL)
		{
			matches = _wcsicmp(
				defaultEndpointId, endpointId.c_str()) == 0;
		}

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

OriginalLoudnessCorrectionCalibrationDialog::
	OriginalLoudnessCorrectionCalibrationDialog(
		const std::wstring& endpointId,
		QWidget* parent)
	: QDialog(parent),
	  endpointId(endpointId),
	  volumeController(endpointId.empty()
		  ? nullptr : new VolumeController(endpointId))
{
	setWindowTitle(tr("Original loudness correction calibration"));
	setModal(true);
	setMinimumWidth(0);
	setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

	QVBoxLayout* layout = new QVBoxLayout(this);
	QLabel* instructions = new QLabel(tr(
		"Use a sound-level meter at the listening position. While this "
		"window is open, only this original loudness-correction row is "
		"temporarily bypassed. Play one speaker, measure the calibration signal, "
		"then enter the sound pressure level reading."), this);
	instructions->setWordWrap(true);
	layout->addWidget(instructions);

	QGroupBox* channelGroup = new QGroupBox(tr("Speaker"), this);
	QHBoxLayout* channelLayout = new QHBoxLayout(channelGroup);
	leftRadioButton = new QRadioButton(tr("Left"), channelGroup);
	rightRadioButton = new QRadioButton(tr("Right"), channelGroup);
	leftRadioButton->setChecked(true);
	channelLayout->addWidget(leftRadioButton);
	channelLayout->addWidget(rightRadioButton);
	channelLayout->addStretch(1);
	layout->addWidget(channelGroup);

	QGroupBox* signalGroup = new QGroupBox(tr("Calibration signal"), this);
	QVBoxLayout* signalLayout = new QVBoxLayout(signalGroup);
	QHBoxLayout* signalRadioLayout = new QHBoxLayout;
	sine1kHzRadioButton = new QRadioButton(
		tr("1 kHz reference tone (Recommended)"), signalGroup);
	pinkNoiseRadioButton = new QRadioButton(
		tr("Pink noise"), signalGroup);
	sine1kHzRadioButton->setChecked(true);
	signalRadioLayout->addWidget(sine1kHzRadioButton);
	signalRadioLayout->addWidget(pinkNoiseRadioButton);
	signalRadioLayout->addStretch(1);
	signalLayout->addLayout(signalRadioLayout);

	signalHintLabel = new QLabel(signalGroup);
	signalHintLabel->setWordWrap(true);
	signalLayout->addWidget(signalHintLabel);
	layout->addWidget(signalGroup);

	QHBoxLayout* transportLayout = new QHBoxLayout;
	playButton = new QPushButton(this);
	stopButton = new QPushButton(tr("Stop"), this);
	stopButton->setEnabled(false);
	transportLayout->addWidget(playButton);
	transportLayout->addWidget(stopButton);
	transportLayout->addStretch(1);
	layout->addLayout(transportLayout);

	statusLabel = new QLabel(this);
	statusLabel->setWordWrap(true);
	layout->addWidget(statusLabel);

	QFormLayout* measurementLayout = new QFormLayout;
	levelSpinBox = new QSpinBox(this);
	levelSpinBox->setRange(1, 120);
	levelSpinBox->setValue(75);
	levelSpinBox->setSuffix(tr(" dB SPL"));
	levelSpinBox->setAccessibleName(tr("Measured sound pressure level"));
	measurementLayout->addRow(tr("Measured level:"), levelSpinBox);
	layout->addLayout(measurementLayout);

	QDialogButtonBox* buttonBox = new QDialogButtonBox(
		QDialogButtonBox::Save | QDialogButtonBox::Cancel,
		Qt::Horizontal, this);
	saveButton = buttonBox->button(QDialogButtonBox::Save);
	saveButton->setText(tr("Use measurement"));
	saveButton->setEnabled(false);
	layout->addWidget(buttonBox);

	connect(playButton, &QPushButton::clicked,
		this, &OriginalLoudnessCorrectionCalibrationDialog::playNoise);
	connect(stopButton, &QPushButton::clicked,
		this, &OriginalLoudnessCorrectionCalibrationDialog::stopNoise);
	connect(leftRadioButton, &QRadioButton::toggled, this,
		[this](bool checked) {
			if (checked && buffer.size() > 0)
				playNoise();
		});
	connect(rightRadioButton, &QRadioButton::toggled, this,
		[this](bool checked) {
			if (checked && buffer.size() > 0)
				playNoise();
		});
	connect(sine1kHzRadioButton, &QRadioButton::toggled, this,
		[this](bool checked) {
			if (checked)
			{
				updateSignalUi();
				if (buffer.size() > 0)
					playNoise();
			}
		});
	connect(pinkNoiseRadioButton, &QRadioButton::toggled, this,
		[this](bool checked) {
			if (checked)
			{
				updateSignalUi();
				if (buffer.size() > 0)
					playNoise();
			}
		});
	connect(buttonBox, &QDialogButtonBox::accepted,
		this, &OriginalLoudnessCorrectionCalibrationDialog::accept);
	connect(buttonBox, &QDialogButtonBox::rejected,
		this, &OriginalLoudnessCorrectionCalibrationDialog::reject);

	updateSignalUi();

	endpointGuardTimer.setTimerType(Qt::CoarseTimer);
	endpointGuardTimer.setInterval(250);
	connect(&endpointGuardTimer, &QTimer::timeout, this, [this]() {
		const bool wasPlaying = buffer.size() > 0;
		(void)updatePlaybackReadiness(wasPlaying);
	});
	endpointGuardTimer.start();
	(void)updatePlaybackReadiness(false);

	QScreen* targetScreen = parent != nullptr ? parent->screen() : screen();
	if (targetScreen != nullptr)
	{
		const QSize available = targetScreen->availableGeometry().size();
		resize((std::min)(GUIHelper::scale(520), available.width()),
			(std::min)(sizeHint().height(), available.height()));
	}
}

OriginalLoudnessCorrectionCalibrationDialog::
	~OriginalLoudnessCorrectionCalibrationDialog()
{
	stopNoise();
}

int OriginalLoudnessCorrectionCalibrationDialog::getMeasuredLevel() const
{
	return levelSpinBox->value();
}

bool OriginalLoudnessCorrectionCalibrationDialog::
	hasValidMeasurement() const
{
	return measurementValid;
}

bool OriginalLoudnessCorrectionCalibrationDialog::
	isPlaybackEndpointStillValid() const
{
	// The original component tracks eMultimedia while PlaySound follows
	// eConsole. Requiring both roles prevents measuring another device.
	return isDefaultRenderEndpoint(endpointId, eMultimedia) &&
		isDefaultRenderEndpoint(endpointId, eConsole);
}

OriginalLoudnessCorrectionCalibrationDialog::PlaybackReadiness
OriginalLoudnessCorrectionCalibrationDialog::getPlaybackReadiness()
{
	if (!isPlaybackEndpointStillValid())
		return PLAYBACK_ENDPOINT_MISMATCH;
	if (!volumeController)
		return PLAYBACK_VOLUME_UNAVAILABLE;

	EndpointVolumeState state;
	if (FAILED(volumeController->getVolumeState(state)) ||
		!std::isfinite(state.levelDb) || !std::isfinite(state.scalar) ||
		_wcsicmp(volumeController->getEndpointId().c_str(),
			endpointId.c_str()) != 0)
	{
		return PLAYBACK_VOLUME_UNAVAILABLE;
	}
	if (state.muted || state.scalar <= 0.0)
		return PLAYBACK_MUTED;
	return PLAYBACK_READY;
}

void OriginalLoudnessCorrectionCalibrationDialog::setPlaybackStatus(
	const QString& text,
	const char* level)
{
	statusLabel->setText(text);
	statusLabel->setAccessibleName(tr("Calibration playback status"));
	statusLabel->setAccessibleDescription(text);
	statusLabel->setProperty(
		"statusLevel", QString::fromLatin1(level));
	statusLabel->style()->unpolish(statusLabel);
	statusLabel->style()->polish(statusLabel);
}

bool OriginalLoudnessCorrectionCalibrationDialog::
	updatePlaybackReadiness(bool showWarning)
{
	const PlaybackReadiness readiness = getPlaybackReadiness();
	if (readiness == PLAYBACK_READY)
	{
		if (buffer.size() == 0)
		{
			playButton->setEnabled(true);
			stopButton->setEnabled(false);
			setPlaybackStatus(tr("Ready to play"), "normal");
		}
		return true;
	}

	const bool wasPlaying = buffer.size() > 0;
	if (wasPlaying)
		stopPlayback();
	measurementValid = false;
	saveButton->setEnabled(false);
	playButton->setEnabled(false);
	stopButton->setEnabled(false);

	QString title;
	QString message;
	if (readiness == PLAYBACK_ENDPOINT_MISMATCH)
	{
		setPlaybackStatus(tr(
			"Playback blocked - Windows default devices do not match"),
			"danger");
		title = tr("Playback device mismatch");
		message = tr(
			"Set the same audible device as both the Windows Console and "
			"Multimedia default, then reopen calibration. No test noise was played.");
	}
	else if (readiness == PLAYBACK_MUTED)
	{
		setPlaybackStatus(tr(
			"Playback blocked - endpoint is muted or at zero volume"),
			"danger");
		title = tr("Playback endpoint is inaudible");
		message = tr(
			"Unmute the default playback endpoint and raise its Windows "
			"volume before playing the calibration signal.");
	}
	else
	{
		setPlaybackStatus(tr(
			"Playback blocked - endpoint volume is unavailable"),
			"danger");
		title = tr("Playback endpoint unavailable");
		message = tr(
			"The default Multimedia playback volume cannot be read. "
			"Reconnect the device before calibrating.");
	}

	if (showWarning)
		QMessageBox::warning(this, title, message);
	return false;
}

void OriginalLoudnessCorrectionCalibrationDialog::playNoise()
{
	if (!updatePlaybackReadiness(true))
		return;
	if (buffer.size() > 0)
		stopNoise();

	measurementValid = false;
	saveButton->setEnabled(false);
	const bool use1kHz = sine1kHzRadioButton != nullptr && sine1kHzRadioButton->isChecked();
	bool writeSucceeded = true;
	if (use1kHz)
	{
		setPlaybackStatus(tr("Preparing 1 kHz reference tone…"), "normal");
		const bool leftEnabled = leftRadioButton->isChecked();
		writeSucceeded = generate1kHzTone(buffer, leftEnabled, !leftEnabled, 48000);
	}
	else
	{
		setPlaybackStatus(tr("Preparing pink noise…"), "normal");
		QFile file(":/sounds/pinkNoise.flac");
		if (!file.open(QIODevice::ReadOnly))
		{
			setPlaybackStatus(
				tr("Pink-noise resource could not be opened"), "danger");
			return;
		}

		QtSndfileHandle fileHandle(file, SFM_READ);
		if (fileHandle.error() != SF_ERR_NO_ERROR ||
			fileHandle.samplerate() <= 0 || !buffer.open(QIODevice::WriteOnly))
		{
			setPlaybackStatus(
				tr("Calibration signal could not be prepared"), "danger");
			return;
		}

		{
			QtSndfileHandle bufferHandle(
				buffer, SFM_WRITE, SF_FORMAT_WAV | SF_FORMAT_PCM_24,
				2, fileHandle.samplerate());
			writeSucceeded = bufferHandle.error() == SF_ERR_NO_ERROR;
			const bool leftEnabled = leftRadioButton->isChecked();
			int source[1024];
			int stereo[2 * ARRAYSIZE(source)];
			int samplesRead = 0;
			while (writeSucceeded &&
				(samplesRead = fileHandle.read(
					source, ARRAYSIZE(source))) > 0)
			{
				for (int i = 0; i < samplesRead; ++i)
				{
					stereo[2 * i] = leftEnabled ? source[i] : 0;
					stereo[2 * i + 1] = leftEnabled ? 0 : source[i];
				}
				writeSucceeded = bufferHandle.write(
					stereo, 2 * samplesRead) == 2 * samplesRead;
			}
		}
		buffer.close();
	}

	if (!writeSucceeded || buffer.size() == 0 ||
		!updatePlaybackReadiness(true))
	{
		buffer.buffer().clear();
		if (!writeSucceeded)
		{
			setPlaybackStatus(
				tr("Calibration signal could not be prepared"), "danger");
		}
		return;
	}

	if (!PlaySoundA(
		buffer.data().data(), NULL,
		SND_MEMORY | SND_ASYNC | SND_LOOP))
	{
		buffer.buffer().clear();
		setPlaybackStatus(
			tr("Windows could not start calibration playback"), "danger");
		return;
	}

	playButton->setEnabled(false);
	stopButton->setEnabled(true);
	measurementValid = true;
	saveButton->setEnabled(true);
	setPlaybackStatus(
		leftRadioButton->isChecked()
			? (use1kHz ? tr("Playing 1 kHz reference tone on the left speaker") : tr("Playing on the left speaker"))
			: (use1kHz ? tr("Playing 1 kHz reference tone on the right speaker") : tr("Playing on the right speaker")),
		"warning");
}

void OriginalLoudnessCorrectionCalibrationDialog::stopPlayback()
{
	if (buffer.size() > 0)
	{
		(void)PlaySoundA(NULL, NULL, 0);
		buffer.buffer().clear();
	}
}

void OriginalLoudnessCorrectionCalibrationDialog::stopNoise()
{
	stopPlayback();
	if (playButton != nullptr)
		(void)updatePlaybackReadiness(false);
}

void OriginalLoudnessCorrectionCalibrationDialog::accept()
{
	if (!measurementValid || !updatePlaybackReadiness(true))
		return;
	stopPlayback();
	QDialog::accept();
}

void OriginalLoudnessCorrectionCalibrationDialog::reject()
{
	stopPlayback();
	QDialog::reject();
}

void OriginalLoudnessCorrectionCalibrationDialog::updateSignalUi()
{
	const bool use1kHz = sine1kHzRadioButton != nullptr && sine1kHzRadioButton->isChecked();
	if (use1kHz)
	{
		if (signalHintLabel != nullptr)
		{
			signalHintLabel->setText(tr(
				"1 kHz pure tone is the ISO 226 reference where dB SPL equals phon level. "
				"Recommended for headphones, near-field monitoring, or calibrators without room acoustic bias."));
		}
		if (playButton != nullptr)
		{
			playButton->setText(tr("Play 1 kHz tone"));
			playButton->setAccessibleDescription(tr(
				"Starts a looping 1 kHz calibration sine tone on the selected speaker"));
		}
	}
	else
	{
		if (signalHintLabel != nullptr)
		{
			signalHintLabel->setText(tr(
				"Pink noise distributes equal energy per octave across the spectrum. "
				"Recommended for stereo speakers and room measurements with an SPL meter set to Z or C weighting and Slow response."));
		}
		if (playButton != nullptr)
		{
			playButton->setText(tr("Play pink noise"));
			playButton->setAccessibleDescription(tr(
				"Starts a looping pink-noise signal on the selected speaker"));
		}
	}
}
