/*
    This file is part of Equalizer APO, a system-wide equalizer.
    Copyright (C) 2026 Equalizer APO contributors
*/

#pragma once

#include <QWidget>
#include <QTimer>
#include <QVariantAnimation>

class VolumeOsdWidget : public QWidget
{
	Q_OBJECT

public:
	explicit VolumeOsdWidget(QWidget* parent = nullptr);
	~VolumeOsdWidget() override = default;

	void showVolume(double volumeDb, double scalar, bool muted, double phon = -1.0);

protected:
	void paintEvent(QPaintEvent* event) override;

private slots:
	void onScalarAnimationChanged(const QVariant& value);
	void onFadeAnimationChanged(const QVariant& value);
	void onFadeAnimationFinished();
	void startFadeOut();

private:
	void updateGeometryPosition();
	void drawSpeakerIcon(QPainter& painter, const QRectF& rect, const QColor& color, bool muted, double scalar);

	QTimer hideTimer;
	QVariantAnimation scalarAnimation;
	QVariantAnimation fadeAnimation;

	double targetDb = 0.0;
	double targetScalar = 1.0;
	double animatedScalar = 1.0;
	bool isMuted = false;
	double currentPhon = -1.0;

	qreal displayOpacity = 0.0;
	qreal slideOffset = 0.0;
	bool isFadingOut = false;
};
