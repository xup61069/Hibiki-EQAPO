/*
	This file is part of EqualizerAPO, a system-wide equalizer.
	Copyright (C) 2024  Jonas Thedering

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

#include "stdafx.h"
#include <DeviceAPOInfo.h>
#include <helpers/RegistryHelper.h>
#include "DeviceTestDialog.h"
#include "OpacityIconEngine.h"

namespace
{
	constexpr int StatusTypeRole = Qt::UserRole + 1;
	constexpr int StatusTooltipRole = Qt::UserRole + 2;

	void refreshDynamicStyle(QWidget* widget)
	{
		widget->style()->unpolish(widget);
		widget->style()->polish(widget);
		widget->update();
	}

	PaletteIconGlyph glyphForStatus(ItemStatusType statusType)
	{
		switch (statusType)
		{
		case ItemStatusType::waiting:
			return PaletteIconGlyph::waiting;
		case ItemStatusType::success:
			return PaletteIconGlyph::success;
		case ItemStatusType::warning:
			return PaletteIconGlyph::warning;
		case ItemStatusType::error:
			return PaletteIconGlyph::error;
		case ItemStatusType::unavailable:
			return PaletteIconGlyph::unavailable;
		}
		return PaletteIconGlyph::unavailable;
	}
}

DeviceTestDialog::DeviceTestDialog(QWidget* parent)
	: QDialog(parent)
{
	ui.setupUi(this);

	setWindowFlags(windowFlags().setFlag(Qt::WindowContextHelpButtonHint, false));
	configureInterface();
	updateThemeAssets();

	animatedIconEngine = new OpacityIconEngine(PaletteIconGlyph::waiting);
	animatedIcon = QIcon(animatedIconEngine);

	QVector<std::shared_ptr<DeviceAPOInfo>> devices;
	bool initializationFailed = false;
	try
	{
		std::vector<std::shared_ptr<DeviceAPOInfo>> outputDevices = filterDevices(DeviceAPOInfo::loadAllInfos(false));
		if (!outputDevices.empty())
		{
			QTreeWidgetItem* outputNode = new QTreeWidgetItem(ui.deviceTreeWidget, QStringList(tr("Playback devices")));
			outputNode->setFlags(Qt::ItemIsEnabled);
			outputNode->setFirstColumnSpanned(true);
			outputNode->setExpanded(true);
			QFont outputFont = outputNode->font(0);
			outputFont.setWeight(QFont::DemiBold);
			outputNode->setFont(0, outputFont);
			addDevices(outputDevices, outputNode);
			devices.append(QVector<std::shared_ptr<DeviceAPOInfo>>(outputDevices.begin(), outputDevices.end()));
		}

		std::vector<std::shared_ptr<DeviceAPOInfo>> inputDevices = filterDevices(DeviceAPOInfo::loadAllInfos(true));
		if (!inputDevices.empty())
		{
			QTreeWidgetItem* inputNode = new QTreeWidgetItem(ui.deviceTreeWidget, QStringList(tr("Capture devices")));
			inputNode->setFlags(Qt::ItemIsEnabled);
			inputNode->setFirstColumnSpanned(true);
			inputNode->setExpanded(true);
			QFont inputFont = inputNode->font(0);
			inputFont.setWeight(QFont::DemiBold);
			inputNode->setFont(0, inputFont);
			addDevices(inputDevices, inputNode);
			devices.append(QVector<std::shared_ptr<DeviceAPOInfo>>(inputDevices.begin(), inputDevices.end()));
		}
	}
	catch (RegistryException e)
	{
		initializationFailed = true;
		const QString message = QString::fromStdWString(e.getMessage());
		setPhaseStatus(tr("Error while accessing the registry"), "danger");
		logError(message);
		QMessageBox::critical(this, tr("Error while accessing the registry"), message);
	}

	ui.deviceStack->setCurrentWidget(devices.isEmpty() ? ui.emptyPage : ui.devicesPage);

	connect(ui.buttonBox, &QDialogButtonBox::accepted, this, &QDialog::accept);

	iconAnimation = new QVariantAnimation(this);
	connect(iconAnimation, &QVariantAnimation::valueChanged, this, &DeviceTestDialog::animateIcon);
	iconAnimation->setStartValue(qreal(0.28));
	iconAnimation->setKeyValueAt(0.5, qreal(1.0));
	iconAnimation->setEndValue(qreal(0.28));
	iconAnimation->setLoopCount(-1);
	iconAnimation->setDuration(950);
	iconAnimation->setEasingCurve(QEasingCurve::InOutQuad);
	iconAnimation->start();

	qRegisterMetaType<ItemStatusType>();

	if (!initializationFailed && !devices.isEmpty())
	{
		thread = new DeviceTestThread(this, devices);
		connect(thread, &DeviceTestThread::log, this, &DeviceTestDialog::log);
		connect(thread, &DeviceTestThread::logError, this, &DeviceTestDialog::logError);
		connect(thread, &DeviceTestThread::showErrorDialog, this, &DeviceTestDialog::showErrorDialog);
		connect(thread, &DeviceTestThread::setItemStatus, this, &DeviceTestDialog::setItemStatus);
		connect(thread, &DeviceTestThread::completed, this, &DeviceTestDialog::onChecksCompleted);
		connect(thread, &QThread::finished, this, &DeviceTestDialog::onThreadFinished);
		connect(thread, &DeviceTestThread::abort, this, &DeviceTestDialog::onAbort);
		thread->start();
	}
	else
	{
		if (!initializationFailed)
			setPhaseStatus(tr("No enabled audio devices are available to test."), "warning");
		onThreadFinished();
	}
}

void DeviceTestDialog::configureInterface()
{
	setAccessibleName(windowTitle());
	setAccessibleDescription(ui.phaseStatusLabel->text());
	ui.deviceTreeWidget->setAccessibleName(tr("APO device test results"));
	ui.deviceTreeWidget->setAccessibleDescription(windowTitle());
	ui.statusOutputBox->setAccessibleName(tr("Device test log"));
	ui.statusOutputBox->setAccessibleDescription(tr("Detailed progress and error messages from the APO device test"));
	ui.testProgressBar->setAccessibleName(tr("Device test progress"));
	ui.phaseStatusLabel->setAccessibleDescription(tr("Current test status"));

	const int smallIconSize = style()->pixelMetric(QStyle::PM_SmallIconSize);
	ui.deviceTreeWidget->setIconSize(QSize(smallIconSize, smallIconSize));
	ui.deviceTreeWidget->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
	ui.deviceTreeWidget->header()->setSectionResizeMode(1, QHeaderView::Stretch);
	ui.deviceTreeWidget->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
	ui.deviceTreeWidget->header()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
}

void DeviceTestDialog::changeEvent(QEvent* event)
{
	QDialog::changeEvent(event);
	if (event->type() == QEvent::ApplicationPaletteChange
		|| event->type() == QEvent::ApplicationFontChange
		|| event->type() == QEvent::FontChange
		|| event->type() == QEvent::PaletteChange
		|| event->type() == QEvent::StyleChange
		|| event->type() == QEvent::ThemeChange)
	{
		updateThemeAssets();
	}
}

void DeviceTestDialog::updateThemeAssets()
{
	updateTypography();
	const QIcon deviceIcon = createPaletteIcon(PaletteIconGlyph::device);
	setWindowIcon(deviceIcon);
	const int iconSize = qMax(24, style()->pixelMetric(QStyle::PM_LargeIconSize));
	const int iconSlotSize = qMax(iconSize, fontMetrics().height() * 2);
	ui.headerIconLabel->setFixedSize(iconSlotSize, iconSlotSize);
	ui.headerIconLabel->setPixmap(deviceIcon.pixmap(iconSize, iconSize));
	const int smallIconSize = style()->pixelMetric(QStyle::PM_SmallIconSize);
	ui.deviceTreeWidget->setIconSize(QSize(smallIconSize, smallIconSize));

	for (int groupIndex = 0; groupIndex < ui.deviceTreeWidget->topLevelItemCount(); ++groupIndex)
	{
		QTreeWidgetItem* group = ui.deviceTreeWidget->topLevelItem(groupIndex);
		for (int itemIndex = 0; itemIndex < group->childCount(); ++itemIndex)
		{
			QTreeWidgetItem* item = group->child(itemIndex);
			for (int column = 2; column <= 3; ++column)
			{
				const QVariant statusData = item->data(column, StatusTypeRole);
				if (statusData.isValid())
					applyItemStatus(item, column, static_cast<ItemStatusType>(statusData.toInt()), item->data(column, StatusTooltipRole).toString());
			}
		}
	}
}

void DeviceTestDialog::updateTypography()
{
	QFont baseFont = font();
	const qreal baseSize = baseFont.pointSizeF() > 0.0
		? baseFont.pointSizeF() : QApplication::font().pointSizeF();

	QFont productFont = baseFont;
	productFont.setWeight(QFont::DemiBold);
	productFont.setPointSizeF(qMax(8.0, baseSize * 0.86));
	ui.productLabel->setFont(productFont);

	QFont headingFont = baseFont;
	headingFont.setWeight(QFont::DemiBold);
	headingFont.setPointSizeF(baseSize * 1.28);
	ui.phaseStatusLabel->setFont(headingFont);

	QFont logFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
	logFont.setPointSizeF(baseSize);
	ui.statusOutputBox->setFont(logFont);
}

std::vector<std::shared_ptr<DeviceAPOInfo>> DeviceTestDialog::filterDevices(const std::vector<std::shared_ptr<AbstractAPOInfo>>& devices)
{
	std::vector<std::shared_ptr<DeviceAPOInfo>> filteredList;
	for (const std::shared_ptr<AbstractAPOInfo>& apoInfo : devices)
	{
		if (apoInfo->isInstalled())
		{
			std::shared_ptr<DeviceAPOInfo> deviceApoInfo = std::dynamic_pointer_cast<DeviceAPOInfo>(apoInfo);
			if (deviceApoInfo)
				filteredList.push_back(deviceApoInfo);
		}
	}

	return filteredList;
}

void DeviceTestDialog::addDevices(std::vector<std::shared_ptr<DeviceAPOInfo>>& devices, QTreeWidgetItem* parentNode)
{
	for (const std::shared_ptr<DeviceAPOInfo>& apoInfo : devices)
	{
		QStringList values;
		values.append(QString::fromStdWString(apoInfo->getConnectionName()));
		values.append(QString::fromStdWString(apoInfo->getDeviceName()));

		ItemStatusType initialStatus = ItemStatusType::waiting;
		QString tooltip;
		if (apoInfo->isDisabled())
		{
			initialStatus = ItemStatusType::unavailable;
			tooltip = tr("Cannot test APO installation as device is disabled");
		}
		else if (apoInfo->isUnplugged())
		{
			initialStatus = ItemStatusType::unavailable;
			tooltip = tr("Cannot test APO installation as device is unplugged");
		}

		QTreeWidgetItem* item = new QTreeWidgetItem(parentNode, values);
		if (apoInfo->getSelectedInstallState().installPreMix)
			applyItemStatus(item, 2, initialStatus, tooltip);
		if (apoInfo->getSelectedInstallState().installPostMix && !apoInfo->isInput())
			applyItemStatus(item, 3, initialStatus, tooltip);

		itemMap.insert(QString::fromStdWString(apoInfo->getDeviceGuid()).toLower(), item);
	}
}

QString DeviceTestDialog::statusText(ItemStatusType statusType) const
{
	switch (statusType)
	{
	case ItemStatusType::waiting:
		return tr("Testing...");
	case ItemStatusType::success:
		return tr("Working");
	case ItemStatusType::warning:
		return tr("Needs attention");
	case ItemStatusType::error:
		return tr("Not working");
	case ItemStatusType::unavailable:
		return tr("Not tested");
	}
	return QString();
}

void DeviceTestDialog::applyItemStatus(QTreeWidgetItem* item, int column, ItemStatusType statusType, const QString& tooltip)
{
	if (item == nullptr)
		return;

	const QIcon icon = statusType == ItemStatusType::waiting
		? animatedIcon : createPaletteIcon(glyphForStatus(statusType));
	item->setIcon(column, icon);
	item->setText(column, statusText(statusType));
	item->setToolTip(column, tooltip.isEmpty() ? statusText(statusType) : tooltip);
	item->setData(column, StatusTypeRole, static_cast<int>(statusType));
	item->setData(column, StatusTooltipRole, tooltip);

	QFont statusFont = item->font(column);
	statusFont.setWeight(statusType == ItemStatusType::error || statusType == ItemStatusType::warning
		? QFont::DemiBold : QFont::Normal);
	item->setFont(column, statusFont);
}

void DeviceTestDialog::closeEvent(QCloseEvent* event)
{
	if (iconAnimation != nullptr)
		iconAnimation->stop();
	if (requestCooperativeShutdown(QDialog::Rejected))
	{
		event->ignore();
		return;
	}
	QDialog::closeEvent(event);
}

void DeviceTestDialog::reject()
{
	done(QDialog::Rejected);
}

void DeviceTestDialog::done(int resultCode)
{
	if (requestCooperativeShutdown(resultCode))
		return;

	QDialog::done(resultCode);
}

bool DeviceTestDialog::requestCooperativeShutdown(int resultCode)
{
	if (thread == nullptr || !thread->isRunning())
		return false;

	pendingClose = true;
	pendingResultCode = resultCode;
	thread->requestInterruption();
	setPhaseStatus(tr("Stopping device test…"), "warning");
	ui.buttonBox->setEnabled(false);
	return true;
}

void DeviceTestDialog::log(const QString& message)
{
	ui.statusOutputBox->append(message);
	const QString plainMessage = QTextDocumentFragment::fromHtml(message).toPlainText();
	if (!plainMessage.isEmpty())
		setPhaseStatus(plainMessage);
}

void DeviceTestDialog::logError(const QString& message)
{
	hasErrors = true;
	const QString plainMessage = QTextDocumentFragment::fromHtml(message).toPlainText();
	ui.statusOutputBox->append("<strong>" + plainMessage.toHtmlEscaped() + "</strong>");
	setPhaseStatus(plainMessage, "danger");
}

void DeviceTestDialog::setPhaseStatus(const QString& message, const char* statusLevel)
{
	ui.phaseStatusLabel->setText(message);
	ui.phaseStatusLabel->setAccessibleDescription(message);
	setAccessibleDescription(message);
	const QByteArray level(statusLevel);
	if (ui.phaseStatusLabel->property("statusLevel").toByteArray() != level)
	{
		ui.phaseStatusLabel->setProperty("statusLevel", level);
		refreshDynamicStyle(ui.phaseStatusLabel);
	}
}

void DeviceTestDialog::showErrorDialog(const QString& message)
{
	hasErrors = true;
	setPhaseStatus(message, "danger");
	QMessageBox::critical(this, tr("Error"), message);
}

void DeviceTestDialog::setItemStatus(const QString& guid, bool postMix, ItemStatusType statusType)
{
	QTreeWidgetItem* item = itemMap.value(guid.toLower());
	if (item != nullptr)
	{
		QString tooltip;
		switch (statusType)
		{
		case ItemStatusType::waiting:
			break;
		case ItemStatusType::success:
			break;
		case ItemStatusType::warning:
			tooltip = tr("Hibiki EQAPO works but original APO could not be initialized. Maybe unset \"Use original APO\" in troubleshooting options.");
			break;
		case ItemStatusType::error:
			tooltip = tr("Hibiki EQAPO did not respond on this processing stage.");
			break;
		case ItemStatusType::unavailable:
			break;
		}
		applyItemStatus(item, postMix ? 3 : 2, statusType, tooltip);
	}
}

void DeviceTestDialog::animateIcon(const QVariant& value)
{
	qreal opacity = value.value<qreal>();
	animatedIconEngine->setOpacity(opacity);

	ui.deviceTreeWidget->viewport()->update();
}

void DeviceTestDialog::onChecksCompleted(bool hasProblems)
{
	hasErrors = hasErrors || hasProblems;
}

void DeviceTestDialog::onThreadFinished()
{
	if (iconAnimation != nullptr)
		iconAnimation->stop();
	if (pendingClose)
	{
		const int resultCode = pendingResultCode;
		QTimer::singleShot(0, this, [this, resultCode] { done(resultCode); });
		return;
	}
	ui.testProgressBar->setRange(0, 1);
	ui.testProgressBar->setValue(1);
	ui.testProgressBar->setAccessibleDescription(hasErrors
		? tr("Device checks completed with problems") : tr("Device checks completed"));
	ui.buttonBox->setEnabled(true);
	if (QPushButton* okButton = ui.buttonBox->button(QDialogButtonBox::Ok))
		okButton->setFocus(Qt::OtherFocusReason);
}

void DeviceTestDialog::onAbort(const QString& message, int code)
{
	hasErrors = true;
	pendingClose = true;
	pendingResultCode = code;
	setPhaseStatus(message, "danger");
	QMessageBox::critical(this, tr("Error"), message);
	if (thread != nullptr)
		thread->requestInterruption();
	if (thread == nullptr || !thread->isRunning())
		QTimer::singleShot(0, this, [this, code] { done(code); });
}
