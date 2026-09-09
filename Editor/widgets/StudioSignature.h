// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "Editor/StudioMotion.h"
#include <QApplication>
#include <QEvent>
#include <QPainter>
#include <QPainterPath>
#include <QVariantAnimation>
#include <QWidget>
#include <cmath>

// A decorative brand signature, deliberately not an audio level meter.
class StudioSignature final : public QWidget
{
public:
	explicit StudioSignature(QWidget* parent = nullptr) : QWidget(parent), motion(this)
	{
		setFixedWidth(160);
		setMinimumHeight(42);
		setFocusPolicy(Qt::NoFocus);
		motion.setDuration(900);
		motion.setStartValue(0.0);
		motion.setEndValue(1.0);
		motion.setEasingCurve(QEasingCurve::OutCubic);
		connect(&motion, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) {
			phase = value.toReal();
			if (!StudioMotion::allowed())
			{
				motion.stop();
				phase = 1.0;
			}
			update();
		});
	}

protected:
	bool event(QEvent* event) override
	{
		if ((event->type() == QEvent::Enter || event->type() == QEvent::Show) && StudioMotion::allowed())
		{
			motion.stop();
			motion.start();
		}
		if (event->type() == QEvent::Hide)
		{
			motion.stop();
			phase = 1.0;
		}
		return QWidget::event(event);
	}

	void paintEvent(QPaintEvent*) override
	{
		QPainter painter(this);
		painter.setRenderHint(QPainter::Antialiasing);
		const bool highContrast = qApp && qApp->property("eqapoModernThemeHighContrast").toBool();
		for (int line = 0; line < 5; ++line)
		{
			QPainterPath path;
			for (int x = 0; x <= width(); x += 2)
			{
				const qreal t = qreal(x) / width();
				const qreal envelope = std::pow(std::sin(t * 3.14159265), 2.0);
				const qreal wave = std::sin(t * 13.0 + line * 0.52 + (1.0 - phase) * 3.0);
				const qreal y = height() / 2.0 + envelope * wave * height() * (0.32 - line * 0.035);
				if (x == 0) path.moveTo(x, y); else path.lineTo(x, y);
			}
			QColor color = palette().color(QPalette::Highlight);
			if (!highContrast) color.setAlpha(210 - line * 33);
			painter.setPen(QPen(color, line == 0 ? 1.6 : 1.0));
			painter.drawPath(path);
		}
	}

private:
	QVariantAnimation motion;
	qreal phase = 1.0;
};
