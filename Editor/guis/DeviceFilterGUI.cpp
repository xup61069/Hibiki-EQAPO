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
#include <string>
#include <QFontMetrics>
#include <QHeaderView>
#include <QStyle>

#include "Editor/helpers/GUIHelper.h"
#include "DeviceFilterGUI.h"
#include "DeviceFilterGUIDialog.h"
#include <filters/DeviceFilterFactory.h>
#include <VoicemeeterAPOInfo.h>
#include "ui_DeviceFilterGUI.h"

using namespace std;

DeviceFilterGUI::DeviceFilterGUI(DeviceFilterGUIFactory* factory)
	: ui(new Ui::DeviceFilterGUI)
{
	ui->setupUi(this);
	ui->label->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
	QFont font = ui->label->font();
	font.setWeight(QFont::DemiBold);
	ui->label->setFont(font);
	this->factory = factory;

	ui->treeWidget->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	ui->treeWidget->setTextElideMode(Qt::ElideMiddle);
	ui->treeWidget->setUniformRowHeights(true);
	QHeaderView* header = ui->treeWidget->header();
	header->setStretchLastSection(false);
	header->setSectionResizeMode(0, QHeaderView::Fixed);
	header->setSectionResizeMode(1, QHeaderView::Fixed);
	header->setSectionResizeMode(2, QHeaderView::Stretch);
	header->setSectionResizeMode(3, QHeaderView::Fixed);
}

DeviceFilterGUI::~DeviceFilterGUI()
{
	delete ui;
}

void DeviceFilterGUI::load(const QString& parameters)
{
	pattern = parameters;

	ui->treeWidget->clear();
	ui->treeWidget->setAccessibleDescription(QString());

	QStringList labels;
	labels.append(tr("Type"));
	labels.append(tr("Connection"));
	labels.append(tr("Device"));
	labels.append(tr("State"));
	ui->treeWidget->setHeaderLabels(labels);

	QTreeWidgetItem* lastItem = NULL;
	bool missingDevice = false;
	const auto addItem = [this](const QStringList& values)
	{
		QTreeWidgetItem* item = new QTreeWidgetItem(ui->treeWidget, values);
		for (int column = 0; column < values.size(); ++column)
		{
			if (!values[column].isEmpty())
				item->setToolTip(column, values[column]);
		}
		return item;
	};

	if (parameters.trimmed() == "all")
	{
		QStringList values;
		values.append("");
		values.append("");
		values.append(tr("All devices"));
		values.append("");

		lastItem = addItem(values);
	}
	else
	{
		const QList<shared_ptr<AbstractAPOInfo>>& devices = factory->getDevices();
		bool anyInstalled = false;
		for (const shared_ptr<AbstractAPOInfo>& apoInfo : devices)
		{
			if (apoInfo->isInstalled() && DeviceFilterFactory::matchDevice(apoInfo->getDeviceString(), pattern.toStdWString()))
			{
				anyInstalled = true;
				break;
			}
		}

		for (const shared_ptr<AbstractAPOInfo>& apoInfo : devices)
		{
			if (anyInstalled && !apoInfo->isInstalled())
				continue;

			if (DeviceFilterFactory::matchDevice(apoInfo->getDeviceString(), pattern.toStdWString()))
			{
				QStringList values;
				if (apoInfo->isInput())
					values.append(tr("Capture"));
				else
					values.append(tr("Playback"));
				values.append(QString::fromStdWString(apoInfo->getConnectionName()));
				values.append(QString::fromStdWString(apoInfo->getDeviceName()));
				QString state;
				if (apoInfo->isInstalled())
					state = tr("APO installed");
				else
					state = tr("APO not installed");
				VoicemeeterAPOInfo* voicemeeterInfo = dynamic_cast<VoicemeeterAPOInfo*>(apoInfo.get());
				if (voicemeeterInfo != NULL && !voicemeeterInfo->isVoicemeeterInstalled())
					state += ", " + tr("Voicemeeter was uninstalled");
				values.append(state);
				lastItem = addItem(values);
			}
		}

		if (ui->treeWidget->topLevelItemCount() == 0)
		{
			missingDevice = true;
			const QString message = tr("No device matched")
				+ " \"" + pattern.trimmed() + "\"";
			QStringList values;
			values.append(message);
			values.append("");
			values.append("");
			values.append("");

			QTreeWidgetItem* item = addItem(values);
			item->setData(0, Qt::AccessibleTextRole, message);
			item->setIcon(0, style()->standardIcon(QStyle::SP_MessageBoxWarning));
			item->setFirstColumnSpanned(true);
			ui->treeWidget->setAccessibleDescription(message);
			lastItem = item;
		}
	}

	ui->treeWidget->setProperty(
		"deviceMatchState", missingDevice ? "missing" : "normal");
	ui->treeWidget->style()->unpolish(ui->treeWidget);
	ui->treeWidget->style()->polish(ui->treeWidget);
	updateColumnWidths();

	int headerHeight = ui->treeWidget->header()->height();
	int itemAreaHeight = 0;
	const int visibleRows = (std::min)(4, ui->treeWidget->topLevelItemCount());
	if (visibleRows > 0)
	{
		QTreeWidgetItem* lastVisibleItem =
			ui->treeWidget->topLevelItem(visibleRows - 1);
		itemAreaHeight =
			ui->treeWidget->visualItemRect(lastVisibleItem).bottom() + 1;
	}
	else if (lastItem != NULL)
	{
		itemAreaHeight = ui->treeWidget->visualItemRect(lastItem).bottom() + 1;
	}
	ui->treeWidget->setFixedHeight(
		headerHeight + itemAreaHeight + ui->treeWidget->frameWidth() * 2 + 1);
}

void DeviceFilterGUI::resizeEvent(QResizeEvent* event)
{
	QWidget::resizeEvent(event);
	updateColumnWidths();
}

void DeviceFilterGUI::updateColumnWidths()
{
	QHeaderView* header = ui->treeWidget->header();
	const int availableWidth = (std::max)(
		1, ui->treeWidget->viewport()->width());
	const auto contentWidth = [this](int column)
	{
		int width = ui->treeWidget->header()->fontMetrics().horizontalAdvance(
			ui->treeWidget->headerItem()->text(column));
		for (int row = 0; row < ui->treeWidget->topLevelItemCount(); ++row)
		{
			QTreeWidgetItem* item = ui->treeWidget->topLevelItem(row);
			int itemWidth =
				ui->treeWidget->fontMetrics().horizontalAdvance(item->text(column));
			if (!item->icon(column).isNull())
				itemWidth += ui->treeWidget->iconSize().width() + GUIHelper::scale(4);
			width = (std::max)(
				width,
				itemWidth);
		}
		return width + GUIHelper::scale(28);
	};
	const auto cappedWidth = [this, availableWidth, &contentWidth](
		int column, int minimum, int maximum, int percent)
	{
		const int minimumWidth = GUIHelper::scale(minimum);
		const int maximumWidth = (std::max)(
			minimumWidth,
			(std::min)(GUIHelper::scale(maximum), availableWidth * percent / 100));
		return (std::max)(
			minimumWidth,
			(std::min)(contentWidth(column), maximumWidth));
	};

	header->resizeSection(0, cappedWidth(0, 64, 140, 18));
	header->resizeSection(1, cappedWidth(1, 96, 220, 25));
	header->resizeSection(3, cappedWidth(3, 96, 200, 25));
}

void DeviceFilterGUI::store(QString& command, QString& parameters)
{
	command = "Device";
	parameters = pattern;
}

void DeviceFilterGUI::on_pushButton_clicked()
{
	DeviceFilterGUIDialog dialog(this, factory, pattern);
	if (dialog.exec() == QDialog::Accepted)
	{
		load(dialog.getPattern());
		emit updateModel();
	}
}
