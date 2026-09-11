/*
    This file is part of Equalizer APO, a system-wide equalizer.
    Copyright (C) 2026 Equalizer APO contributors
*/

#include "VolumeOsdWidget.h"
#include "Editor/helpers/GUIHelper.h"
#include <QPainter>
#include <QPainterPath>
#include <QGuiApplication>
#include <QScreen>
#include <algorithm>
#include <cmath>

VolumeOsdWidget::VolumeOsdWidget(QWidget* parent)
	: QWidget(parent, Qt::ToolTip | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::WindowDoesNotAcceptFocus),
	  currentDb(0.0),
	  currentScalar(1.0),
	  isMuted(false),
	  currentPhon(-1.0)
{
	setAttribute(Qt::WA_TranslucentBackground);
	setAttribute(Qt::WA_ShowWithoutActivating);
	setAttribute(Qt::WA_TransparentForMouseEvents);

	setFixedSize(GUIHelper::scale(QSize(320, 80)));

	hideTimer.setSingleShot(true);
	connect(&hideTimer, &QTimer::timeout, this, &QWidget::hide);
}

void VolumeOsdWidget::showVolume(double volumeDb, double scalar, bool muted, double phon)
{
	currentDb = volumeDb;
	currentScalar = (std::max)(0.0, (std::min)(1.0, scalar));
	isMuted = muted;
	currentPhon = phon;

	QScreen* screen = QGuiApplication::primaryScreen();
	if (screen)
	{
		const QRect geom = screen->availableGeometry();
		move(geom.x() + (geom.width() - width()) / 2, geom.y() + geom.height() - height() - GUIHelper::scale(80));
	}

	show();
	raise();
	update();

	hideTimer.start(1600);
}

void VolumeOsdWidget::paintEvent(QPaintEvent* event)
{
	Q_UNUSED(event);
	QPainter painter(this);
	painter.setRenderHint(QPainter::Antialiasing);

	const bool darkMode = GUIHelper::isDarkMode();
	const int r = GUIHelper::scale(12);

	// Background card
	QPainterPath cardPath;
	cardPath.addRoundedRect(rect().adjusted(1, 1, -1, -1), r, r);
	QColor bgColor = darkMode ? QColor(22, 24, 30, 235) : QColor(245, 247, 252, 240);
	painter.fillPath(cardPath, bgColor);

	// Border
	QColor borderColor = darkMode ? QColor(59, 130, 246, 110) : QColor(59, 130, 246, 130);
	painter.setPen(QPen(borderColor, 1.5));
	painter.drawPath(cardPath);

	// Title / Brand
	QFont titleFont = font();
	titleFont.setPointSize(GUIHelper::scale(9));
	titleFont.setBold(true);
	painter.setFont(titleFont);
	painter.setPen(QColor(59, 130, 246));
	painter.drawText(QRect(GUIHelper::scale(16), GUIHelper::scale(10), width() - GUIHelper::scale(32), GUIHelper::scale(16)),
		Qt::AlignLeft | Qt::AlignVCenter, tr("Hibiki EQAPO \u2022 Loudness Control"));

	// Volume / Mute text
	QFont valueFont = font();
	valueFont.setPointSize(GUIHelper::scale(13));
	valueFont.setBold(true);
	painter.setFont(valueFont);

	if (isMuted)
	{
		painter.setPen(QColor(239, 68, 68)); // Mute Red
		painter.drawText(QRect(GUIHelper::scale(16), GUIHelper::scale(28), width() - GUIHelper::scale(32), GUIHelper::scale(24)),
			Qt::AlignLeft | Qt::AlignVCenter, tr("Muted"));
	}
	else
	{
		painter.setPen(darkMode ? QColor(240, 240, 245) : QColor(20, 20, 25));
		QString text = QString::asprintf("%.1f dB  (%d%%)", currentDb, static_cast<int>(std::round(currentScalar * 100.0)));
		if (currentPhon >= 0.0)
		{
			text += QString::asprintf("  \u2022  %.0f phon", currentPhon);
		}
		painter.drawText(QRect(GUIHelper::scale(16), GUIHelper::scale(28), width() - GUIHelper::scale(32), GUIHelper::scale(24)),
			Qt::AlignLeft | Qt::AlignVCenter, text);
	}

	// Progress bar
	const int barX = GUIHelper::scale(16);
	const int barY = GUIHelper::scale(58);
	const int barW = width() - barX * 2;
	const int barH = GUIHelper::scale(6);
	const int barRadius = GUIHelper::scale(3);

	// Track background
	QPainterPath trackBg;
	trackBg.addRoundedRect(QRect(barX, barY, barW, barH), barRadius, barRadius);
	painter.fillPath(trackBg, darkMode ? QColor(60, 64, 75, 180) : QColor(200, 205, 215, 180));

	// Track fill
	if (!isMuted && currentScalar > 0.001)
	{
		const int fillW = (std::max)(barRadius * 2, static_cast<int>(std::round(barW * currentScalar)));
		QPainterPath trackFill;
		trackFill.addRoundedRect(QRect(barX, barY, fillW, barH), barRadius, barRadius);
		painter.fillPath(trackFill, QColor(59, 130, 246));
	}
}
