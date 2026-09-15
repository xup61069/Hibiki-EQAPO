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

#include "GUIHelper.h"

#include <QApplication>
#include <QFont>
#include <QFontMetrics>
#include <QIcon>
#include <QIconEngine>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QScreen>
#include <QStyleHints>
#include <QLibrary>

#ifdef Q_OS_WIN
#include <QtCore/qt_windows.h>
#endif

#include <algorithm>

namespace
{
	class PaletteIconEngine final : public QIconEngine
	{
	public:
		PaletteIconEngine(GUIHelper::ThemeIcon icon, bool accent)
			: icon(icon), accent(accent)
		{
		}

		QIconEngine* clone() const override
		{
			return new PaletteIconEngine(*this);
		}

		QString key() const override
		{
			return QStringLiteral("EqApoPaletteIcon");
		}

		QPixmap pixmap(const QSize& size, QIcon::Mode mode, QIcon::State state) override
		{
			QPixmap result(size);
			result.fill(Qt::transparent);
			QPainter painter(&result);
			paint(&painter, result.rect(), mode, state);
			return result;
		}

		void paint(QPainter* painter, const QRect& rect, QIcon::Mode mode, QIcon::State) override
		{
			const QPalette palette = QApplication::palette();
			const QColor color = mode == QIcon::Disabled
				? palette.color(QPalette::Disabled, QPalette::ButtonText)
				: palette.color(accent ? QPalette::Highlight : QPalette::ButtonText);
			const qreal side = (std::min)(rect.width(), rect.height());
			const qreal stroke = (std::max)(1.0, side * 0.085);
			const QPointF center = QRectF(rect).center();
			const QRectF box(
				center.x() - side * 0.31,
				center.y() - side * 0.31,
				side * 0.62,
				side * 0.62);
			const qreal arm = side * 0.27;

			painter->save();
			painter->setRenderHint(QPainter::Antialiasing, true);
			painter->setPen(QPen(color, stroke, Qt::SolidLine, Qt::SquareCap, Qt::MiterJoin));
			painter->setBrush(Qt::NoBrush);

			switch (icon)
			{
			case GUIHelper::ThemeIcon::Add:
				painter->drawLine(QPointF(center.x() - arm, center.y()), QPointF(center.x() + arm, center.y()));
				painter->drawLine(QPointF(center.x(), center.y() - arm), QPointF(center.x(), center.y() + arm));
				break;
			case GUIHelper::ThemeIcon::Remove:
				painter->drawLine(QPointF(center.x() - arm, center.y()), QPointF(center.x() + arm, center.y()));
				break;
			case GUIHelper::ThemeIcon::NewDocument:
			case GUIHelper::ThemeIcon::Profile:
			{
				QPainterPath page;
				page.moveTo(box.left(), box.top());
				page.lineTo(box.right() - side * 0.16, box.top());
				page.lineTo(box.right(), box.top() + side * 0.16);
				page.lineTo(box.right(), box.bottom());
				page.lineTo(box.left(), box.bottom());
				page.closeSubpath();
				painter->drawPath(page);
				painter->drawLine(QPointF(box.right() - side * 0.16, box.top()), QPointF(box.right() - side * 0.16, box.top() + side * 0.16));
				painter->drawLine(QPointF(box.right() - side * 0.16, box.top() + side * 0.16), QPointF(box.right(), box.top() + side * 0.16));
				if (icon == GUIHelper::ThemeIcon::Profile)
				{
					painter->drawLine(QPointF(box.left() + side * 0.12, center.y()), QPointF(box.right() - side * 0.1, center.y()));
					painter->drawLine(QPointF(box.left() + side * 0.12, center.y() + side * 0.13), QPointF(box.right() - side * 0.1, center.y() + side * 0.13));
				}
				break;
			}
			case GUIHelper::ThemeIcon::OpenFolder:
			{
				QPainterPath folder;
				folder.moveTo(box.left(), box.top() + side * 0.1);
				folder.lineTo(center.x() - side * 0.04, box.top() + side * 0.1);
				folder.lineTo(center.x() + side * 0.04, box.top() + side * 0.18);
				folder.lineTo(box.right(), box.top() + side * 0.18);
				folder.lineTo(box.right(), box.bottom());
				folder.lineTo(box.left(), box.bottom());
				folder.closeSubpath();
				painter->drawPath(folder);
				break;
			}
			case GUIHelper::ThemeIcon::Save:
			case GUIHelper::ThemeIcon::SaveAs:
				painter->drawRect(box);
				painter->drawRect(QRectF(box.left() + side * 0.1, box.top(), side * 0.27, side * 0.2));
				painter->drawRect(QRectF(box.left() + side * 0.11, center.y() + side * 0.02, box.width() - side * 0.22, side * 0.2));
				if (icon == GUIHelper::ThemeIcon::SaveAs)
				{
					painter->drawLine(QPointF(center.x() + side * 0.08, box.bottom()), QPointF(box.right() + side * 0.12, center.y() + side * 0.08));
					painter->drawLine(QPointF(box.right() + side * 0.03, center.y() + side * 0.04), QPointF(box.right() + side * 0.12, center.y() + side * 0.08));
				}
				break;
			case GUIHelper::ThemeIcon::Cut:
				painter->drawEllipse(QRectF(box.left(), box.bottom() - side * 0.18, side * 0.18, side * 0.18));
				painter->drawEllipse(QRectF(box.right() - side * 0.18, box.bottom() - side * 0.18, side * 0.18, side * 0.18));
				painter->drawLine(QPointF(box.left() + side * 0.09, box.bottom() - side * 0.18), QPointF(box.right() - side * 0.05, box.top()));
				painter->drawLine(QPointF(box.right() - side * 0.09, box.bottom() - side * 0.18), QPointF(box.left() + side * 0.05, box.top()));
				break;
			case GUIHelper::ThemeIcon::Copy:
			case GUIHelper::ThemeIcon::Duplicate:
				painter->drawRect(QRectF(box.left() + side * 0.1, box.top(), box.width() - side * 0.1, box.height() - side * 0.1));
				painter->drawRect(QRectF(box.left(), box.top() + side * 0.1, box.width() - side * 0.1, box.height() - side * 0.1));
				break;
			case GUIHelper::ThemeIcon::Paste:
				painter->drawRect(QRectF(box.left(), box.top() + side * 0.1, box.width(), box.height() - side * 0.1));
				painter->drawRect(QRectF(center.x() - side * 0.12, box.top(), side * 0.24, side * 0.16));
				break;
			case GUIHelper::ThemeIcon::Delete:
				painter->drawRect(QRectF(box.left() + side * 0.08, box.top() + side * 0.13, box.width() - side * 0.16, box.height() - side * 0.13));
				painter->drawLine(QPointF(box.left(), box.top() + side * 0.08), QPointF(box.right(), box.top() + side * 0.08));
				painter->drawLine(QPointF(center.x() - side * 0.1, box.top()), QPointF(center.x() + side * 0.1, box.top()));
				break;
			case GUIHelper::ThemeIcon::SelectAll:
				painter->drawRect(box);
				painter->setPen(QPen(color, stroke, Qt::DashLine));
				painter->drawRect(box.adjusted(-side * 0.09, -side * 0.09, side * 0.09, side * 0.09));
				break;
			case GUIHelper::ThemeIcon::Search:
				painter->drawEllipse(QRectF(box.left(), box.top(), side * 0.4, side * 0.4));
				painter->drawLine(QPointF(center.x() + side * 0.02, center.y() + side * 0.02), QPointF(box.right(), box.bottom()));
				break;
			case GUIHelper::ThemeIcon::FindNext:
			case GUIHelper::ThemeIcon::ArrowRight:
				painter->drawLine(QPointF(box.left(), center.y()), QPointF(box.right(), center.y()));
				painter->drawLine(QPointF(box.right() - side * 0.16, center.y() - side * 0.16), QPointF(box.right(), center.y()));
				painter->drawLine(QPointF(box.right() - side * 0.16, center.y() + side * 0.16), QPointF(box.right(), center.y()));
				break;
			case GUIHelper::ThemeIcon::Rename:
			case GUIHelper::ThemeIcon::Edit:
				painter->drawLine(QPointF(box.left(), box.bottom()), QPointF(box.right() - side * 0.08, box.top() + side * 0.08));
				painter->drawLine(QPointF(box.right() - side * 0.08, box.top() + side * 0.08), QPointF(box.right(), box.top() + side * 0.16));
				painter->drawLine(QPointF(box.left(), box.bottom()), QPointF(box.left() + side * 0.15, box.bottom() - side * 0.03));
				break;
			case GUIHelper::ThemeIcon::Import:
			case GUIHelper::ThemeIcon::Export:
			case GUIHelper::ThemeIcon::Up:
			{
				const bool pointsUp = icon != GUIHelper::ThemeIcon::Export;
				const qreal direction = pointsUp ? -1.0 : 1.0;
				painter->drawLine(QPointF(center.x(), center.y() + direction * side * 0.25), QPointF(center.x(), center.y() - direction * side * 0.2));
				painter->drawLine(QPointF(center.x(), center.y() + direction * side * 0.25), QPointF(center.x() - side * 0.14, center.y() + direction * side * 0.1));
				painter->drawLine(QPointF(center.x(), center.y() + direction * side * 0.25), QPointF(center.x() + side * 0.14, center.y() + direction * side * 0.1));
				if (icon != GUIHelper::ThemeIcon::Up)
					painter->drawLine(QPointF(box.left(), box.bottom()), QPointF(box.right(), box.bottom()));
				break;
			}
			case GUIHelper::ThemeIcon::Compare:
				painter->drawLine(QPointF(box.left(), center.y() - side * 0.13), QPointF(box.right(), center.y() - side * 0.13));
				painter->drawLine(QPointF(box.right() - side * 0.12, center.y() - side * 0.23), QPointF(box.right(), center.y() - side * 0.13));
				painter->drawLine(QPointF(box.left(), center.y() + side * 0.13), QPointF(box.right(), center.y() + side * 0.13));
				painter->drawLine(QPointF(box.left() + side * 0.12, center.y() + side * 0.03), QPointF(box.left(), center.y() + side * 0.13));
				break;
			case GUIHelper::ThemeIcon::Snapshot:
				painter->drawRect(QRectF(box.left(), box.top() + side * 0.1, box.width(), box.height() - side * 0.1));
				painter->drawEllipse(QRectF(center.x() - side * 0.13, center.y() - side * 0.1, side * 0.26, side * 0.26));
				painter->drawLine(QPointF(box.left() + side * 0.1, box.top() + side * 0.1), QPointF(box.left() + side * 0.18, box.top()));
				break;
			case GUIHelper::ThemeIcon::Bypass:
				painter->drawEllipse(box);
				painter->drawLine(QPointF(box.left() + side * 0.08, box.bottom() - side * 0.08), QPointF(box.right() - side * 0.08, box.top() + side * 0.08));
				break;
			case GUIHelper::ThemeIcon::Restore:
				painter->drawArc(box, 35 * 16, 285 * 16);
				painter->drawLine(QPointF(box.left(), center.y() - side * 0.08), QPointF(box.left() + side * 0.14, center.y() - side * 0.16));
				painter->drawLine(QPointF(box.left(), center.y() - side * 0.08), QPointF(box.left() + side * 0.1, center.y() + side * 0.02));
				break;
			case GUIHelper::ThemeIcon::Link:
				painter->drawArc(QRectF(box.left(), center.y() - side * 0.15, side * 0.36, side * 0.3), 40 * 16, 280 * 16);
				painter->drawArc(QRectF(box.right() - side * 0.36, center.y() - side * 0.15, side * 0.36, side * 0.3), 220 * 16, 280 * 16);
				painter->drawLine(QPointF(center.x() - side * 0.12, center.y()), QPointF(center.x() + side * 0.12, center.y()));
				break;
			case GUIHelper::ThemeIcon::Route:
				painter->drawLine(QPointF(box.left(), box.top()), QPointF(center.x(), center.y()));
				painter->drawLine(QPointF(box.left(), box.bottom()), QPointF(center.x(), center.y()));
				painter->drawLine(QPointF(center.x(), center.y()), QPointF(box.right(), center.y()));
				break;
			case GUIHelper::ThemeIcon::Channel:
				painter->drawLine(QPointF(box.left(), box.top()), QPointF(box.left(), box.bottom()));
				painter->drawLine(QPointF(center.x(), box.top() + side * 0.08), QPointF(center.x(), box.bottom() - side * 0.08));
				painter->drawLine(QPointF(box.right(), box.top()), QPointF(box.right(), box.bottom()));
				break;
			}
			painter->restore();
		}

	private:
		GUIHelper::ThemeIcon icon;
		bool accent;
	};
}

QSize GUIHelper::scale(QSize size)
{
	if (qApp->testAttribute(Qt::AA_Use96Dpi))
		return size;

	qreal dpi = QGuiApplication::primaryScreen()->logicalDotsPerInchX();
	return QSize(qRound(size.width() * dpi / 96), qRound(size.height() * dpi / 96));
}

int GUIHelper::scale(double pixel)
{
	if (qApp->testAttribute(Qt::AA_Use96Dpi))
		return qRound(pixel);

	qreal dpi = QGuiApplication::primaryScreen()->logicalDotsPerInchX();
	return qRound(pixel * dpi / 96);
}

double GUIHelper::scaleZoom(double zoom)
{
	if (qApp->testAttribute(Qt::AA_Use96Dpi))
		return zoom;

	qreal dpi = QGuiApplication::primaryScreen()->logicalDotsPerInchX();
	return zoom * dpi / 96;
}

double GUIHelper::invScale(int pixel)
{
	if (qApp->testAttribute(Qt::AA_Use96Dpi))
		return pixel;

	qreal dpi = QGuiApplication::primaryScreen()->logicalDotsPerInchX();
	return pixel * 96 / dpi;
}

double GUIHelper::invScaleZoom(double zoom)
{
	if (qApp->testAttribute(Qt::AA_Use96Dpi))
		return zoom;

	qreal dpi = QGuiApplication::primaryScreen()->logicalDotsPerInchX();
	return zoom * 96 / dpi;
}

bool GUIHelper::isDarkMode()
{
	return QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;
}

QColor GUIHelper::accentColor()
{
	const QPalette palette = QApplication::palette();
	const QColor accent = palette.color(QPalette::Highlight);
	if (accent.isValid() && accent != QColor(Qt::transparent) && accent.alpha() > 0)
		return accent;

#ifdef Q_OS_WIN
	QLibrary dwmApi(QStringLiteral("dwmapi"));
	using DwmGetColorizationColorFunction = HRESULT (WINAPI *)(DWORD*, BOOL*);
	auto getColorizationColor = reinterpret_cast<DwmGetColorizationColorFunction>(
		dwmApi.resolve("DwmGetColorizationColor"));
	DWORD colorization = 0;
	BOOL opaqueBlend = FALSE;
	if (getColorizationColor && SUCCEEDED(getColorizationColor(&colorization, &opaqueBlend)))
	{
		return QColor(
			static_cast<int>((colorization >> 16) & 0xff),
			static_cast<int>((colorization >> 8) & 0xff),
			static_cast<int>(colorization & 0xff));
	}
	const COLORREF highlight = GetSysColor(COLOR_HIGHLIGHT);
	return QColor(GetRValue(highlight), GetGValue(highlight), GetBValue(highlight));
#else
	return QColor(59, 130, 246);
#endif
}

QIcon GUIHelper::createAccentAddIcon()
{
	return createThemeIcon(ThemeIcon::Add, true);
}

QIcon GUIHelper::createThemeIcon(ThemeIcon icon, bool accent)
{
	return QIcon(new PaletteIconEngine(icon, accent));
}
