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

#include <cmath>
#include <QApplication>
#include <QEasingCurve>
#include <QStyle>
#include <QVariantAnimation>
#include "StudioMotion.h"

#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include "helpers/UiSnapshot.h"
#include "AnalysisPlotScene.h"

using namespace std;

AnalysisPlotScene::AnalysisPlotScene(QObject* parent)
	: FrequencyPlotScene(parent),
	  responseAnimation(new QVariantAnimation(this))
{
	responseAnimation->setDuration(180);
	responseAnimation->setStartValue(0.0);
	responseAnimation->setEndValue(1.0);
	responseAnimation->setEasingCurve(QEasingCurve::OutCubic);
	connect(
		responseAnimation,
		&QVariantAnimation::valueChanged,
		this,
		[this](const QVariant& value)
		{
			setResponseAnimationProgress(value.toReal());
		});
	connect(
		responseAnimation,
		&QVariantAnimation::finished,
		this,
		[this]()
		{
			nodes = targetNodes;
			animationStartNodes.clear();
			update();
		});
}

void AnalysisPlotScene::setFreqData(fftw_complex* freqData, int frameCount, unsigned sampleRate)
{
	std::vector<FilterNode> nextNodes;
	if (freqData == NULL || frameCount <= 0 || sampleRate == 0)
	{
		responseAnimation->stop();
		nodes.clear();
		animationStartNodes.clear();
		targetNodes.clear();
		update();
		return;
	}

	nextNodes.reserve(frameCount / 2);
	for (int i = 0; i < frameCount / 2; i++)
	{
		double freq = (i * 1.0 / frameCount) * sampleRate;
		// GainIterator can't handle 0 Hz node
		if (freq == 0.0)
			freq = 0.001;
		double gain = sqrt(freqData[i][0] * freqData[i][0] + freqData[i][1] * freqData[i][1]);
		// A zero FFT bin is visually below the plot.  Keep the animation math
		// finite so transitions to or from silence cannot create NaN nodes.
		double dbGain = gain > 0.0 ? log10(gain) * 20 : -300.0;
		nextNodes.push_back(FilterNode(freq, dbGain));
	}

	// A newer analysis result supersedes the previous target.  Snapshot the
	// currently displayed interpolation before stopping so rapid edits remain
	// continuous and never build an animation queue.
	if (responseAnimation->state() == QAbstractAnimation::Running)
	{
		setResponseAnimationProgress(responseAnimation->currentValue().toReal());
		responseAnimation->stop();
	}

	if (nodes.empty() || !responseAnimationAllowed()
		|| !responseShapesMatch(nodes, nextNodes))
	{
		nodes = nextNodes;
		animationStartNodes.clear();
		targetNodes = nextNodes;
		update();
		return;
	}

	animationStartNodes = nodes;
	targetNodes = nextNodes;
	responseAnimation->start();
}

const vector<FilterNode>& AnalysisPlotScene::getNodes() const
{
	return nodes;
}

bool AnalysisPlotScene::responseAnimationAllowed() const
{
	if (!StudioMotion::allowed())
		return false;
	// Visual-regression captures and explicit test runs must settle on the
	// target response immediately.
	if (UiSnapshot::requested()
		|| (qApp != NULL && qApp->property("eqapoDisableAnimations").toBool()))
		return false;

	if (qApp == NULL || qApp->style() == NULL
		|| !qApp->style()->styleHint(QStyle::SH_Widget_Animate))
		return false;

#ifdef Q_OS_WIN
	BOOL clientAreaAnimationsEnabled = TRUE;
	if (SystemParametersInfoW(
		SPI_GETCLIENTAREAANIMATION,
		0,
		&clientAreaAnimationsEnabled,
		0)
		&& !clientAreaAnimationsEnabled)
		return false;
#endif

	return true;
}

bool AnalysisPlotScene::responseShapesMatch(
	const vector<FilterNode>& left,
	const vector<FilterNode>& right) const
{
	if (left.size() != right.size())
		return false;

	for (size_t i = 0; i < left.size(); ++i)
	{
		const double tolerance = max(1e-9, abs(right[i].freq) * 1e-9);
		if (abs(left[i].freq - right[i].freq) > tolerance)
			return false;
	}
	return true;
}

void AnalysisPlotScene::setResponseAnimationProgress(qreal progress)
{
	if (!responseShapesMatch(animationStartNodes, targetNodes))
		return;

	const qreal boundedProgress = qBound<qreal>(0.0, progress, 1.0);
	if (!responseShapesMatch(nodes, targetNodes))
		nodes = animationStartNodes;
	for (size_t i = 0; i < nodes.size(); ++i)
	{
		nodes[i].freq = targetNodes[i].freq;
		nodes[i].dbGain = animationStartNodes[i].dbGain
			+ boundedProgress
				* (targetNodes[i].dbGain - animationStartNodes[i].dbGain);
	}
	update();
}
