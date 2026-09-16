/*
    This file is part of Equalizer APO, a system-wide equalizer.
    Copyright (C) 2026 Equalizer APO contributors
*/

#include "VolumeOsdWidget.h"
#include "Editor/helpers/GUIHelper.h"
#include "Editor/StudioMotion.h"
#include <QPainter>
#include <QPainterPath>
#include <QLinearGradient>
#include <QGuiApplication>
#include <QScreen>
#include <QCursor>
#include <QEasingCurve>
#include <algorithm>
#include <cmath>

VolumeOsdWidget::VolumeOsdWidget(QWidget* parent)
	: QWidget(parent, Qt::ToolTip | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::WindowDoesNotAcceptFocus),
	  targetDb(0.0),
	  targetScalar(1.0),
	  animatedScalar(1.0),
	  isMuted(false),
	  currentPhon(-1.0),
	  displayOpacity(0.0),
	  slideOffset(0.0),
	  isFadingOut(false)
{
	setAttribute(Qt::WA_TranslucentBackground);
	setAttribute(Qt::WA_ShowWithoutActivating);
	setAttribute(Qt::WA_TransparentForMouseEvents);

	setFixedSize(GUIHelper::scale(QSize(340, 88)));

	hideTimer.setSingleShot(true);
	connect(&hideTimer, &QTimer::timeout, this, &VolumeOsdWidget::startFadeOut);

	scalarAnimation.setDuration(120);
	scalarAnimation.setEasingCurve(QEasingCurve::OutCubic);
	connect(&scalarAnimation, &QVariantAnimation::valueChanged, this, &VolumeOsdWidget::onScalarAnimationChanged);

	fadeAnimation.setDuration(160);
	connect(&fadeAnimation, &QVariantAnimation::valueChanged, this, &VolumeOsdWidget::onFadeAnimationChanged);
	connect(&fadeAnimation, &QVariantAnimation::finished, this, &VolumeOsdWidget::onFadeAnimationFinished);
}

void VolumeOsdWidget::updateGeometryPosition()
{
	QScreen* screen = QGuiApplication::screenAt(QCursor::pos());
	if (!screen)
		screen = QGuiApplication::primaryScreen();
	if (!screen)
		return;

	const QRect geom = screen->availableGeometry();
	const int baseY = geom.y() + geom.height() - height() - GUIHelper::scale(80);
	const int targetX = geom.x() + (geom.width() - width()) / 2;
	const int targetY = baseY + static_cast<int>(std::round(slideOffset));
	move(targetX, targetY);
}

void VolumeOsdWidget::showVolume(double volumeDb, double scalar, bool muted, double phon)
{
	targetDb = volumeDb;
	targetScalar = (std::max)(0.0, (std::min)(1.0, scalar));
	isMuted = muted;
	currentPhon = phon;

	const bool motion = StudioMotion::allowed();

	// Smooth scalar animation
	if (motion)
	{
		scalarAnimation.stop();
		scalarAnimation.setStartValue(animatedScalar);
		scalarAnimation.setEndValue(targetScalar);
		scalarAnimation.start();
	}
	else
	{
		scalarAnimation.stop();
		animatedScalar = targetScalar;
	}

	// Entrance fade and slide animation
	hideTimer.stop();
	isFadingOut = false;

	if (motion)
	{
		fadeAnimation.stop();
		fadeAnimation.setDuration(150);
		fadeAnimation.setEasingCurve(QEasingCurve::OutCubic);
		fadeAnimation.setStartValue(displayOpacity);
		fadeAnimation.setEndValue(1.0);
		fadeAnimation.start();
	}
	else
	{
		fadeAnimation.stop();
		displayOpacity = 1.0;
		slideOffset = 0.0;
		setWindowOpacity(1.0);
		updateGeometryPosition();
	}

	if (!isVisible())
	{
		if (motion)
		{
			displayOpacity = 0.0;
			slideOffset = GUIHelper::scale(10.0);
			setWindowOpacity(0.0);
		}
		updateGeometryPosition();
		show();
	}
	else
	{
		updateGeometryPosition();
	}

	raise();
	update();

	hideTimer.start(1800);
}

void VolumeOsdWidget::startFadeOut()
{
	if (!isVisible())
		return;

	if (!StudioMotion::allowed())
	{
		hide();
		return;
	}

	isFadingOut = true;
	fadeAnimation.stop();
	fadeAnimation.setDuration(220);
	fadeAnimation.setEasingCurve(QEasingCurve::InCubic);
	fadeAnimation.setStartValue(displayOpacity);
	fadeAnimation.setEndValue(0.0);
	fadeAnimation.start();
}

void VolumeOsdWidget::onScalarAnimationChanged(const QVariant& value)
{
	animatedScalar = value.toDouble();
	update();
}

void VolumeOsdWidget::onFadeAnimationChanged(const QVariant& value)
{
	displayOpacity = value.toReal();
	setWindowOpacity(displayOpacity);

	if (!isFadingOut)
	{
		// Float upward into place during entrance
		slideOffset = GUIHelper::scale(10.0) * (1.0 - displayOpacity);
	}
	else
	{
		// Float slightly downward as it fades out
		slideOffset = GUIHelper::scale(4.0) * (1.0 - displayOpacity);
	}
	updateGeometryPosition();
	update();
}

void VolumeOsdWidget::onFadeAnimationFinished()
{
	if (isFadingOut && displayOpacity <= 0.01)
	{
		hide();
		isFadingOut = false;
	}
}

void VolumeOsdWidget::drawSpeakerIcon(QPainter& painter, const QRectF& rect, const QColor& color, bool muted, double scalar)
{
	painter.save();
	painter.setRenderHint(QPainter::Antialiasing, true);

	const qreal w = rect.width();
	const qreal h = rect.height();
	const qreal cx = rect.left();
	const qreal cy = rect.top();

	QPen pen(color, GUIHelper::scale(1.6), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
	painter.setPen(pen);
	painter.setBrush(color);

	// Speaker body and cone
	QPainterPath speaker;
	const qreal bodyW = w * 0.22;
	const qreal bodyH = h * 0.38;
	const qreal bodyLeft = cx + w * 0.08;
	const qreal bodyTop = cy + (h - bodyH) / 2.0;

	speaker.moveTo(bodyLeft, bodyTop);
	speaker.lineTo(bodyLeft + bodyW, bodyTop);
	// Flare out to cone
	speaker.lineTo(bodyLeft + bodyW + w * 0.22, bodyTop - h * 0.18);
	speaker.lineTo(bodyLeft + bodyW + w * 0.22, bodyTop + bodyH + h * 0.18);
	speaker.lineTo(bodyLeft + bodyW, bodyTop + bodyH);
	speaker.lineTo(bodyLeft, bodyTop + bodyH);
	speaker.closeSubpath();
	painter.drawPath(speaker);

	painter.setBrush(Qt::NoBrush);

	if (muted)
	{
		// Draw clear mute diagonal cross line
		QPen mutePen(color, GUIHelper::scale(2.0), Qt::SolidLine, Qt::RoundCap);
		painter.setPen(mutePen);
		const qreal crossX = cx + w * 0.62;
		const qreal crossY = cy + h * 0.32;
		const qreal crossSize = w * 0.28;
		painter.drawLine(QPointF(crossX, crossY), QPointF(crossX + crossSize, crossY + crossSize));
		painter.drawLine(QPointF(crossX + crossSize, crossY), QPointF(crossX, crossY + crossSize));
	}
	else
	{
		// Sound wave arcs based on volume scalar
		const qreal arcCenterX = bodyLeft + bodyW * 0.5;
		const qreal arcCenterY = cy + h * 0.5;

		// Wave 1: low (>= 1%)
		if (scalar > 0.01)
		{
			const qreal r1 = w * 0.38;
			QRectF arcRect1(arcCenterX - r1, arcCenterY - r1, r1 * 2, r1 * 2);
			painter.drawArc(arcRect1, -38 * 16, 76 * 16);
		}
		// Wave 2: medium (>= 34%)
		if (scalar >= 0.34)
		{
			const qreal r2 = w * 0.52;
			QRectF arcRect2(arcCenterX - r2, arcCenterY - r2, r2 * 2, r2 * 2);
			painter.drawArc(arcRect2, -42 * 16, 84 * 16);
		}
		// Wave 3: high (>= 67%)
		if (scalar >= 0.67)
		{
			const qreal r3 = w * 0.66;
			QRectF arcRect3(arcCenterX - r3, arcCenterY - r3, r3 * 2, r3 * 2);
			painter.drawArc(arcRect3, -46 * 16, 92 * 16);
		}
	}

	painter.restore();
}

void VolumeOsdWidget::paintEvent(QPaintEvent* event)
{
	Q_UNUSED(event);
	QPainter painter(this);
	painter.setRenderHint(QPainter::Antialiasing, true);

	const bool darkMode = GUIHelper::isDarkMode();
	const QColor accent = GUIHelper::accentColor();
	const int r = GUIHelper::scale(14);

	// Background card: smooth rounded acrylic/surface feel
	const QRectF cardRect = rect().adjusted(1, 1, -1, -1);
	QPainterPath cardPath;
	cardPath.addRoundedRect(cardRect, r, r);

	const QColor bgColor = darkMode
		? QColor(22, 25, 32, 242)
		: QColor(252, 253, 255, 246);
	painter.fillPath(cardPath, bgColor);

	// Multi-layer ambient accent border
	const QColor accentGlow = QColor(accent.red(), accent.green(), accent.blue(), darkMode ? 45 : 35);
	painter.setPen(QPen(accentGlow, GUIHelper::scale(3.0)));
	painter.drawPath(cardPath);

	const QColor accentBorder = QColor(accent.red(), accent.green(), accent.blue(), darkMode ? 140 : 160);
	painter.setPen(QPen(accentBorder, GUIHelper::scale(1.2)));
	painter.drawPath(cardPath);

	// Brand title: matching Windows Accent Color
	QFont titleFont = font();
	titleFont.setPointSize(GUIHelper::scale(8.5));
	titleFont.setBold(true);
	titleFont.setLetterSpacing(QFont::AbsoluteSpacing, GUIHelper::scale(0.6));
	painter.setFont(titleFont);
	painter.setPen(accent);
	painter.drawText(
		QRect(GUIHelper::scale(20), GUIHelper::scale(11), width() - GUIHelper::scale(40), GUIHelper::scale(16)),
		Qt::AlignLeft | Qt::AlignVCenter,
		tr("HIBIKI EQAPO \u2022 LOUDNESS CONTROL"));

	// Speaker icon
	const QRectF iconRect(GUIHelper::scale(18), GUIHelper::scale(31), GUIHelper::scale(24), GUIHelper::scale(24));
	const QColor iconColor = isMuted ? QColor(239, 68, 68) : accent;
	drawSpeakerIcon(painter, iconRect, iconColor, isMuted, animatedScalar);

	// Volume / Mute / Phon readout text
	QFont valueFont = font();
	valueFont.setPointSize(GUIHelper::scale(12.5));
	valueFont.setBold(true);
	painter.setFont(valueFont);

	const int textX = GUIHelper::scale(48);
	const int textW = width() - textX - GUIHelper::scale(20);
	const QRect textRect(textX, GUIHelper::scale(31), textW, GUIHelper::scale(24));

	if (isMuted)
	{
		painter.setPen(QColor(239, 68, 68)); // Mute warning red
		painter.drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter, tr("Muted"));
	}
	else
	{
		const QColor primaryText = darkMode ? QColor(245, 246, 250) : QColor(20, 22, 28);
		painter.setPen(primaryText);

		const int percent = static_cast<int>(std::round(animatedScalar * 100.0));
		QString text = QString::asprintf("%.1f dB  (%d%%)", targetDb, percent);
		if (currentPhon >= 0.0)
		{
			text += QString::asprintf("  \u2022  %.0f phon", currentPhon);
		}
		painter.drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter, text);
	}

	// Progress bar geometry
	const int barX = GUIHelper::scale(20);
	const int barY = GUIHelper::scale(65);
	const int barW = width() - barX * 2;
	const int barH = GUIHelper::scale(7);
	const int barRadius = barH / 2;

	// Track background
	QPainterPath trackBg;
	trackBg.addRoundedRect(QRect(barX, barY, barW, barH), barRadius, barRadius);
	const QColor trackBgColor = darkMode ? QColor(50, 54, 66, 190) : QColor(215, 220, 230, 190);
	painter.fillPath(trackBg, trackBgColor);

	// Animated track fill with Windows Accent gradient
	if (!isMuted && animatedScalar > 0.001)
	{
		const int fillW = (std::max)(barH, static_cast<int>(std::round(barW * animatedScalar)));
		const QRect fillRect(barX, barY, fillW, barH);
		QPainterPath trackFill;
		trackFill.addRoundedRect(fillRect, barRadius, barRadius);

		QLinearGradient fillGradient(fillRect.topLeft(), fillRect.topRight());
		fillGradient.setColorAt(0.0, accent.lighter(112));
		fillGradient.setColorAt(1.0, accent);

		painter.fillPath(trackFill, fillGradient);

		// Glowing end indicator pill
		if (fillW >= barH)
		{
			QPainterPath endDot;
			const qreal dotSize = barH * 0.75;
			const qreal dotX = barX + fillW - barH + (barH - dotSize) / 2.0;
			const qreal dotY = barY + (barH - dotSize) / 2.0;
			endDot.addEllipse(QRectF(dotX, dotY, dotSize, dotSize));
			painter.fillPath(endDot, QColor(255, 255, 255, 210));
		}
	}
}
