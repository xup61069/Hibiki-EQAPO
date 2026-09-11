/*
    This file is part of Equalizer APO, a system-wide equalizer.
    Copyright (C) 2026 Equalizer APO contributors
*/

#pragma once

#include <QWidget>
#include <QTimer>

class VolumeOsdWidget : public QWidget
{
	Q_OBJECT

public:
	explicit VolumeOsdWidget(QWidget* parent = nullptr);
	~VolumeOsdWidget() override = default;

	void showVolume(double volumeDb, double scalar, bool muted, double phon = -1.0);

protected:
	void paintEvent(QPaintEvent* event) override;

private:
	QTimer hideTimer;
	double currentDb;
	double currentScalar;
	bool isMuted;
	double currentPhon;
};
