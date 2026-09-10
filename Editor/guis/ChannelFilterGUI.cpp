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

#include "Editor/helpers/GUIHelper.h"
#include "ChannelFilterGUI.h"
#include "ChannelFilterGUIDialog.h"
#include "ui_ChannelFilterGUI.h"

using namespace std;

ChannelFilterGUI::ChannelFilterGUI(const QString& parameters, int selectedChannelMask)
	: ui(new Ui::ChannelFilterGUI)
{
	ui->setupUi(this);
	ui->label->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
	QFont font = ui->label->font();
	font.setWeight(QFont::DemiBold);
	ui->label->setFont(font);

	this->selectedChannelMask = selectedChannelMask;

	scene = new ChannelFilterGUIScene;
	ui->graphicsView->setScene(scene);
	ui->graphicsView->setBackgroundRole(QPalette::Window);
	ui->graphicsView->setFixedHeight(GUIHelper::scale(28));
	ui->graphicsView->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	ui->graphicsView->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	ui->graphicsView->setFrameShape(QFrame::NoFrame);
	ui->graphicsView->setStyleSheet(QStringLiteral("background: transparent; border: none;"));
	connect(scene, SIGNAL(selectionChanged()), this, SLOT(updateSelectedChannels()));

	selectedChannels.clear();

	QStringList words = parameters.trimmed().split(' ', Qt::SkipEmptyParts);
	for (QString word : words)
		if (word.length() > 0)
			selectedChannels.append(word.toUpper());
}

ChannelFilterGUI::~ChannelFilterGUI()
{
	delete ui;
}

void ChannelFilterGUI::configureChannels(vector<wstring>& channelNames)
{
	this->channelNames = channelNames;

	refreshGui();
}

void ChannelFilterGUI::store(QString& command, QString& parameters)
{
	command = "Channel";

	selectedChannels = scene->getSelectedChannels();
	parameters = selectedChannels.join(' ');
}

void ChannelFilterGUI::on_pushButton_clicked()
{
	ChannelFilterGUIDialog dialog(this, selectedChannels, selectedChannelMask, channelNames);
	if (dialog.exec() == QDialog::Accepted)
	{
		selectedChannels = dialog.getSelectedChannels();
		refreshGui();
		emit updateModel();
	}
}

void ChannelFilterGUI::updateSelectedChannels()
{
	selectedChannels = scene->getSelectedChannels();
	updateModel();
}

void ChannelFilterGUI::refreshGui()
{
	scene->load(channelNames, selectedChannels);
	const int sceneH = static_cast<int>(std::ceil(scene->sceneRect().height()));
	ui->graphicsView->setFixedHeight(std::max(GUIHelper::scale(28), sceneH + GUIHelper::scale(4)));
}
