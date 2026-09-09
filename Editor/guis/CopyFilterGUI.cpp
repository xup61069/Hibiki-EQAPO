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

#include "Editor/widgets/ResizeCorner.h"
#include "Editor/helpers/GUIHelper.h"
#include "helpers/ChannelHelper.h"
#include "CopyFilterGUIForm.h"
#include "CopyFilterGUI.h"
#include "ui_CopyFilterGUI.h"
#include <cmath>
#include <QPushButton>
#include <QSizePolicy>

namespace
{
	constexpr double DEFAULT_HEIGHT = 88.0;
	constexpr double MINIMUM_HEIGHT = 85.0;
	constexpr double MAXIMUM_HEIGHT = 360.0;

	double boundedLogicalHeight(double height)
	{
		if (!std::isfinite(height))
			return DEFAULT_HEIGHT;
		return qBound(MINIMUM_HEIGHT, height, MAXIMUM_HEIGHT);
	}

	int boundedScaledHeight(int height)
	{
		return qBound(
			GUIHelper::scale(MINIMUM_HEIGHT),
			height,
			GUIHelper::scale(MAXIMUM_HEIGHT));
	}
}

using namespace std;

CopyFilterGUI::CopyFilterGUI(CopyFilter* filter, FilterTable* filterTable)
	: ui(new Ui::CopyFilterGUI)
{
	ui->setupUi(this);
	ui->tabWidget->setTabIcon(
		ui->tabWidget->indexOf(ui->tab),
		GUIHelper::createThemeIcon(GUIHelper::ThemeIcon::Route));
	ui->tabWidget->setTabIcon(
		ui->tabWidget->indexOf(ui->tab_2),
		GUIHelper::createThemeIcon(GUIHelper::ThemeIcon::Channel));

	scene = new CopyFilterGUIScene;
	ui->graphicsView->setScene(scene);
	ui->graphicsView->setBackgroundRole(QPalette::Window);
	ui->graphicsView->setFixedHeight(boundedScaledHeight(DEFAULT_HEIGHT));

	ui->form->load(filter->getAssignments());

	QPushButton* resetButton = new QPushButton(tr("Reset"), this);
	resetButton->setObjectName(QStringLiteral("copyResetButton"));
	resetButton->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
	ui->gridLayout->addWidget(resetButton, 0, 1);
	connect(resetButton, &QPushButton::clicked, this, [this]() {
		std::vector<Assignment> emptyAssignments;
		scene->load(inputChannelNames, emptyAssignments);
		ui->form->load(emptyAssignments);
		emit updateModel();
		emit updateChannels();
	});

	ResizeCorner* cornerWidget = new ResizeCorner(filterTable,
			QSize(0, GUIHelper::scale(MINIMUM_HEIGHT)),
			QSize(0, GUIHelper::scale(MAXIMUM_HEIGHT)),
			[this]() {
		return QSize(0, ui->scrollArea->height());
	},
			[this](QSize size) {
		const int scaledHeight = boundedScaledHeight(size.height());
		ui->scrollArea->setFixedHeight(scaledHeight);
		ui->graphicsView->setFixedHeight(scaledHeight);
		updateGeometry();
	}, this);
	cornerWidget->setCursor(Qt::SizeVerCursor);
	cornerWidget->setAutoFillBackground(true);
	// Keep the resize grip reachable from both the graph and assignments tabs.
	ui->gridLayout->addWidget(
		cornerWidget, 1, 1, Qt::AlignRight | Qt::AlignBottom);

	connect(scene, SIGNAL(updateModel()), this, SIGNAL(updateModel()));
	connect(scene, SIGNAL(updateChannels()), this, SIGNAL(updateChannels()));

	connect(ui->form, SIGNAL(updateModel()), this, SIGNAL(updateModel()));
	connect(ui->form, SIGNAL(updateChannels()), this, SIGNAL(updateChannels()));
}

CopyFilterGUI::~CopyFilterGUI()
{
	delete ui;
}

void CopyFilterGUI::configureChannels(vector<wstring>& channelNames)
{
	vector<Assignment> assignments = ui->form->buildAssignments();

	if (channelNames != inputChannelNames)
	{
		inputChannelNames = channelNames;

		scene->load(inputChannelNames, assignments);
		ui->form->setChannelNames(channelNames);
	}

	for (Assignment assignment : assignments)
	{
		if (assignment.targetChannel == L"")
			continue;
		bool hasSummand = false;
		for (Assignment::Summand summand : assignment.sourceSum)
		{
			if (summand.channel != L" ")
			{
				hasSummand = true;
				break;
			}
		}
		if (!hasSummand)
			continue;

		int channelIndex = ChannelHelper::getChannelIndex(assignment.targetChannel, channelNames, true);
		if (channelIndex == -1)
			channelNames.push_back(assignment.targetChannel);
	}
}

void CopyFilterGUI::store(QString& command, QString& parameters)
{
	command = "Copy";

	std::vector<Assignment> assignments;

	if (ui->tabWidget->currentIndex() == 0)
		assignments = scene->buildAssignments();
	else
		assignments = ui->form->buildAssignments();

	bool firstAssignment = true;
	for (const Assignment& assignment : assignments)
	{
		if (assignment.targetChannel == L"")
			continue;

		bool firstSummand = true;
		for (const Assignment::Summand& summand : assignment.sourceSum)
		{
			// skip not yet filled row
			if (summand.channel == L" ")
				continue;

			if (firstSummand)
			{
				firstSummand = false;

				if (firstAssignment)
					firstAssignment = false;
				else
					parameters += " ";

				parameters += QString::fromStdWString(assignment.targetChannel);
				parameters += "=";
			}
			else
			{
				parameters += "+";
			}

			bool hasChannel = summand.channel != L"";
			bool hasFactor = !hasChannel || summand.factor != 1.0 || summand.isDecibel;

			if (hasFactor)
			{
				QString factorString;
				factorString.setNum(summand.factor);
				if (factorString != "0" && !factorString.contains('.'))
					factorString += ".0";
				parameters += factorString;
				if (summand.isDecibel)
					parameters += "dB";
			}

			if (hasFactor && hasChannel)
				parameters += "*";

			if (hasChannel)
				parameters += QString::fromStdWString(summand.channel);
		}

		if (ui->tabWidget->currentIndex() == 0)
			ui->form->load(assignments);
		else
			scene->load(inputChannelNames, assignments);
	}
}

void CopyFilterGUI::loadPreferences(const QVariantMap& prefs)
{
	bool validHeight = false;
	double storedHeight = prefs.value("height", DEFAULT_HEIGHT).toDouble(&validHeight);
	if (!validHeight || !std::isfinite(storedHeight))
		storedHeight = DEFAULT_HEIGHT;
	const int scaledHeight = GUIHelper::scale(boundedLogicalHeight(storedHeight));
	ui->scrollArea->setFixedHeight(scaledHeight);
	ui->graphicsView->setFixedHeight(scaledHeight);
	updateGeometry();
	ui->tabWidget->setCurrentIndex(prefs.value("tabIndex", 0).toInt());
}

void CopyFilterGUI::storePreferences(QVariantMap& prefs)
{
	const double storedHeight = boundedLogicalHeight(
		GUIHelper::invScale(ui->scrollArea->height()));
	if (storedHeight != DEFAULT_HEIGHT)
		prefs.insert("height", storedHeight);
	if (ui->tabWidget->currentIndex() != 0)
		prefs.insert("tabIndex", ui->tabWidget->currentIndex());
}


int CopyFilterGUI::preferredHeight() const
{
	const int contentHeight = ui->scrollArea ? ui->scrollArea->height() : boundedScaledHeight(DEFAULT_HEIGHT);
	const int labelHeight = ui->copyLabel ? ui->copyLabel->sizeHint().height() : GUIHelper::scale(20);
	const int spacing = ui->gridLayout ? ui->gridLayout->verticalSpacing() : GUIHelper::scale(4);
	return qMax(contentHeight + labelHeight + spacing + GUIHelper::scale(8),
		layout()->totalSizeHint().height());
}

QSize CopyFilterGUI::sizeHint() const
{
	QSize size = QWidget::sizeHint();
	size.setHeight(preferredHeight());
	return size;
}

QSize CopyFilterGUI::minimumSizeHint() const
{
	QSize size = QWidget::minimumSizeHint();
	size.setHeight(preferredHeight());
	return size;
}
