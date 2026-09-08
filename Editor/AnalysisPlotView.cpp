/*
    This file is part of EqualizerAPO, a system-wide equalizer.
    Copyright (C) 2015  Jonas Thedering

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

#include <algorithm>
#include <QApplication>
#include <QLinearGradient>

#include "helpers/GainIterator.h"
#include "Editor/helpers/GUIHelper.h"
#include "AnalysisPlotScene.h"
#include "AnalysisPlotView.h"

using namespace std;

AnalysisPlotView::AnalysisPlotView(QWidget* parent)
	: FrequencyPlotView(parent)
{
}

void AnalysisPlotView::drawBackground(QPainter* painter, const QRectF& rect)
{
	FrequencyPlotView::drawBackground(painter, rect);

	painter->setRenderHint(QPainter::Antialiasing, true);
	AnalysisPlotScene* s = qobject_cast<AnalysisPlotScene*>(scene());
	const std::vector<FilterNode>& nodes = s->getNodes();
	GainIterator gainIterator(nodes);
	QPainterPath path;
	bool first = true;
	double lastDb = -1000;
	for (int x = rect.left() - 1; x <= rect.right() + 1; x++)
	{
		double hz = s->xToHz(x);
		double db = gainIterator.gainAt(hz);
		double y = s->dbToY(db);
		if (y == -1)
		{
			if (db < 0)
				y = sceneRect().bottom() + 1;
			else
				y = sceneRect().top() - 1;
		}

        if (abs(db-lastDb) < 0.001f)
			y = floor(y) + 0.5;
		lastDb = db;
		if (first)
		{
			path.moveTo(x, y);
			first = false;
		}
		else
		{
			path.lineTo(x, y);
		}
	}

	const QPainterPath responsePath = path;
	path.lineTo(rect.right() + 1, rect.bottom() + 1);
	path.lineTo(rect.left() - 1, rect.bottom() + 1);

	const QPalette plotPalette = palette();
	const bool highContrast = qApp
		&& qApp->property("eqapoModernThemeHighContrast").toBool();
	const bool dark = plotPalette.color(QPalette::Window).lightnessF() < 0.5;
	double thresholdY = s->dbToY(0);
	if (!highContrast)
	{
		QColor tint = plotPalette.color(QPalette::Highlight);
		tint.setAlpha(dark ? 38 : 25);
		QLinearGradient fill(0, rect.top(), 0, rect.bottom());
		fill.setColorAt(0, tint);
		tint.setAlpha(0);
		fill.setColorAt(1, tint);
		painter->fillPath(path, fill);
		QColor glow = plotPalette.color(QPalette::Highlight);
		glow.setAlpha(dark ? 25 : 14);
		painter->setPen(QPen(glow, GUIHelper::scale(6.0), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
		painter->setBrush(Qt::NoBrush);
		painter->drawPath(responsePath);
	}
	if (rect.top() < thresholdY)
	{
		QPainterPath rectPath;
		rectPath.addRect(rect.left(), rect.top(), rect.width(), min(thresholdY - rect.top(), rect.height()));
		QPainterPath clippingPath = path.intersected(rectPath);
		painter->setPen(Qt::NoPen);
		if (highContrast)
			painter->setBrush(QBrush(plotPalette.color(QPalette::Highlight), Qt::Dense4Pattern));
		else
			painter->setBrush(dark ?
				QColor(255, 138, 134, 76) :
				QColor(179, 38, 30, 48));
		painter->drawPath(clippingPath);
		painter->setBrush(Qt::NoBrush);
	}

	painter->setPen(QPen(
		plotPalette.color(QPalette::Highlight),
		GUIHelper::scale(2.0),
		Qt::SolidLine,
		Qt::RoundCap,
		Qt::RoundJoin));
	painter->drawPath(responsePath);
}
