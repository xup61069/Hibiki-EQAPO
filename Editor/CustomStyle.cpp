/*
	This file is part of EqualizerAPO, a system-wide equalizer.
	Copyright (C) 2016  Jonas Thedering

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
#include "CustomStyle.h"
#include "Editor/helpers/GUIHelper.h"

#include <algorithm>
#include <cmath>
#include <QApplication>
#include <QAbstractSlider>
#include <QPainter>
#include <QLinearGradient>
#include "StudioMotion.h"
#include <QStyleOptionSlider>

CustomStyle::CustomStyle(QStyle* style)
	: QProxyStyle(style)
{
}

int CustomStyle::pixelMetric(QStyle::PixelMetric metric, const QStyleOption* option, const QWidget* widget) const
{
	switch (metric)
	{
	case PM_ToolBarIconSize:
	case PM_TabBarIconSize:
		return GUIHelper::scale(16);
	case PM_DockWidgetTitleBarButtonMargin:
		return GUIHelper::scale(baseStyle()->pixelMetric(metric, option, widget));
	default:
		return baseStyle()->pixelMetric(metric, option, widget);
	}
}

void CustomStyle::drawComplexControl(
	QStyle::ComplexControl control,
	const QStyleOptionComplex* option,
	QPainter* painter,
	const QWidget* widget) const
{
	if (control != QStyle::CC_Dial)
	{
		QProxyStyle::drawComplexControl(control, option, painter, widget);
		return;
	}

	const QStyleOptionSlider* dial =
		qstyleoption_cast<const QStyleOptionSlider*>(option);
	if (dial == nullptr || dial->maximum <= dial->minimum)
	{
		QProxyStyle::drawComplexControl(control, option, painter, widget);
		return;
	}

	painter->save();
	painter->setRenderHint(QPainter::Antialiasing, true);

	const qreal inset = GUIHelper::scale(7.0);
	const qreal diameter = (std::max)(1.0,
		(std::min)(dial->rect.width() - 2.0 * inset,
			dial->rect.height() - 2.0 * inset));
	const QPointF center = QRectF(dial->rect).center();
	const QRectF arcRect(
		center.x() - diameter / 2.0,
		center.y() - diameter / 2.0,
		diameter,
		diameter);

	const bool enabled = dial->state & QStyle::State_Enabled;
	const bool hovered = dial->state & QStyle::State_MouseOver;
	const QColor track = enabled ?
		dial->palette.color(QPalette::Mid) :
		dial->palette.color(QPalette::Disabled, QPalette::Text);
	const QColor accent = enabled ?
		dial->palette.color(QPalette::Highlight) :
		dial->palette.color(QPalette::Disabled, QPalette::Text);
	const QColor centerColor = dial->palette.color(QPalette::Button);

	const bool highContrast = qApp && qApp->property("eqapoModernThemeHighContrast").toBool();
	const qreal bodyRadius = diameter * 0.36;
	// Concentric body, directional light and engraved scale retain native QDial input.
	if (!highContrast)
	{
		QColor shadow = dial->palette.color(QPalette::Shadow);
		shadow.setAlpha(100);
		painter->setPen(Qt::NoPen);
		painter->setBrush(shadow);
		painter->drawEllipse(center + QPointF(0, 2), bodyRadius + 1, bodyRadius + 1);
	}
	QLinearGradient body(center.x(), center.y() - bodyRadius, center.x(), center.y() + bodyRadius);
	body.setColorAt(0, centerColor.lighter(highContrast ? 100 : 135));
	body.setColorAt(1, centerColor.darker(highContrast ? 100 : 115));
	painter->setPen(QPen(track, 1));
	painter->setBrush(body);
	painter->drawEllipse(center, bodyRadius, bodyRadius);
	for (int tick = 0; tick <= 10; ++tick)
	{
		const qreal a = (225.0 - tick * 27.0) * 3.14159265358979323846 / 180.0;
		const QPointF direction(std::cos(a), -std::sin(a));
		painter->setPen(QPen(track, 1));
		painter->drawLine(center + direction * (diameter / 2 + 4),
			center + direction * (diameter / 2 + (tick % 5 == 0 ? 7 : 5)));
	}
	const qreal trackWidth = GUIHelper::scale(3.0);
	painter->setBrush(Qt::NoBrush);
	painter->setPen(QPen(track, trackWidth, Qt::SolidLine, Qt::RoundCap));
	painter->drawArc(arcRect, 225 * 16, -270 * 16);

	const QAbstractSlider* slider = qobject_cast<const QAbstractSlider*>(widget);
	const bool invertedAppearance = slider != nullptr && slider->invertedAppearance();
	const int position = QStyle::sliderPositionFromValue(
		dial->minimum,
		dial->maximum,
		widget && slider && StudioMotion::allowed() && !slider->isSliderDown()
			&& widget->property("studioDialValue").isValid()
			? qRound(widget->property("studioDialValue").toReal()) : dial->sliderPosition,
		1000,
		invertedAppearance);
	const qreal progress = position / 1000.0;
	painter->setPen(QPen(
		accent,
		hovered ? GUIHelper::scale(5.0) : trackWidth,
		Qt::SolidLine,
		Qt::RoundCap));
	painter->drawArc(arcRect, 225 * 16, qRound(-270.0 * progress * 16.0));

	const qreal angle = (225.0 - 270.0 * progress) *
		3.14159265358979323846 / 180.0;
	const qreal markerRadius = diameter / 2.0;
	const QPointF marker(
		center.x() + std::cos(angle) * markerRadius,
		center.y() - std::sin(angle) * markerRadius);
	painter->setPen(QPen(centerColor, GUIHelper::scale(1.0)));
	painter->setBrush(accent);
	painter->drawEllipse(marker, GUIHelper::scale(3.0), GUIHelper::scale(3.0));

	painter->setPen(QPen(track, GUIHelper::scale(1.0)));
	painter->setBrush(centerColor);
	painter->setPen(QPen(accent, GUIHelper::scale(2.0), Qt::SolidLine, Qt::RoundCap));
	const QPointF needle(std::cos(angle), -std::sin(angle));
	painter->drawLine(center + needle * bodyRadius * 0.45, center + needle * bodyRadius * 0.82);

	if (dial->state & QStyle::State_HasFocus)
	{
		QColor focus = accent;
		const bool highContrast = qApp
			&& qApp->property("eqapoModernThemeHighContrast").toBool();
		if (!highContrast)
			focus.setAlpha(135);
		painter->setBrush(Qt::NoBrush);
		painter->setPen(QPen(focus, GUIHelper::scale(1.0), Qt::DotLine));
		painter->drawEllipse(arcRect.adjusted(
			-GUIHelper::scale(3.0),
			-GUIHelper::scale(3.0),
			GUIHelper::scale(3.0),
			GUIHelper::scale(3.0)));
	}

	painter->restore();
}

QIcon CustomStyle::standardIcon(StandardPixmap standardIcon, const QStyleOption *option, const QWidget *widget) const
{
	const QPalette iconPalette = option ? option->palette : QApplication::palette();
	const bool highContrast = qApp
		&& qApp->property("eqapoModernThemeHighContrast").toBool();
	if (!highContrast && iconPalette.color(QPalette::Window).lightnessF() < 0.5)
	{
		switch (standardIcon)
		{
		case QStyle::SP_ToolBarHorizontalExtensionButton :
			return QIcon(":/icons/dark-mode/toolbar-ext-h.svg");

		case QStyle::SP_ToolBarVerticalExtensionButton :
			return QIcon(":/icons/dark-mode/toolbar-ext-v.svg");

		default:
			break;
		}
	}

	return QProxyStyle::standardIcon(standardIcon, option, widget);
}
