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
#include <helpers/ServiceHelper.h>
#include <helpers/UiSnapshot.h>
#include <VoicemeeterAPOInfo.h>
#include "DeviceTestDialog.h"
#include "OpacityIconEngine.h"
#include "../version.h"
#include "DeviceSelector.h"

namespace
{
	constexpr int DeviceInfoRole = Qt::UserRole;

	bool hasPendingChange(const std::shared_ptr<AbstractAPOInfo>& apoInfo, bool checked)
	{
		return checked != apoInfo->isInstalled()
			|| checked && apoInfo->isInstalled()
			&& (apoInfo->canBeUpgraded() || apoInfo->hasChanges() || apoInfo->isEnhancementsDisabled());
	}

	void refreshDynamicStyle(QWidget* widget)
	{
		widget->style()->unpolish(widget);
		widget->style()->polish(widget);
		widget->update();
	}
}

DeviceSelector::DeviceSelector(QWidget* parent)
	: QDialog(parent)
{
	ui.setupUi(this);

	setWindowFlags(windowFlags().setFlag(Qt::WindowContextHelpButtonHint, false));

	QString version = QString("%1.%2").arg(MAJOR).arg(MINOR);
	if (REVISION != 0)
		version += QString(".%1").arg(REVISION);
	setWindowTitle(QString("Hibiki EQAPO %1 Device Selector").arg(version));
	setAccessibleName(windowTitle());
	setAccessibleDescription(ui.requestLabel->text());

	configureDeviceTree();

	if (!RegistryHelper::isWindowsVersionAtLeast(6, 3)) // Windows 8.1
	{
		ui.installModeComboBox->removeItem(2);
		ui.installModeComboBox->removeItem(1);
	}

	connect(ui.deviceTreeWidget, &QTreeWidget::itemChanged, this, &DeviceSelector::onDeviceToggled);
	connect(ui.deviceTreeWidget, &QTreeWidget::itemSelectionChanged, this, &DeviceSelector::onDeviceSelectionChanged);
	connect(ui.deviceTreeWidget, &QTreeWidget::customContextMenuRequested, this, &DeviceSelector::onDeviceContextMenuRequested);
	connect(ui.buttonBox, &QDialogButtonBox::accepted, this, &QDialog::accept);
	connect(ui.buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
	connect(this, &QDialog::accepted, this, &DeviceSelector::onDialogAccepted);
	connect(this, &QDialog::rejected, this, &DeviceSelector::onDialogRejected);
	connect(ui.copyDeviceCommandAction, &QAction::triggered, this, &DeviceSelector::onCopyDeviceCommandClicked);
	connect(ui.troubleshootingGroupBox, &QGroupBox::toggled, this, &DeviceSelector::onTroubleShootingToggled);
	connect(ui.installPreMixCheckBox, &QCheckBox::clicked, this, &DeviceSelector::onTroubleShootingOptionChanged);
	connect(ui.installPostMixCheckBox, &QCheckBox::clicked, this, &DeviceSelector::onTroubleShootingOptionChanged);
	connect(ui.useOriginalAPOPreMixCheckBox, &QCheckBox::clicked, this, &DeviceSelector::onTroubleShootingOptionChanged);
	connect(ui.useOriginalAPOPostMixCheckBox, &QCheckBox::clicked, this, &DeviceSelector::onTroubleShootingOptionChanged);
	connect(ui.installModeComboBox, static_cast<void (QComboBox::*)(int)>(&QComboBox::activated), this, &DeviceSelector::onTroubleShootingOptionChanged);
	connect(ui.allowSilentBufferCheckBox, &QCheckBox::clicked, this, &DeviceSelector::onTroubleShootingOptionChanged);
	connect(ui.autoCheckBox, &QCheckBox::clicked, this, &DeviceSelector::onTroubleShootingOptionChanged);

	ui.troubleshootingGroupBox->setChecked(false);
	onTroubleShootingToggled(false);
	updateThemeAssets();
	updateButtons();
	const bool snapshotMode = UiSnapshot::requested();
	if (snapshotMode)
	{
		ui.deviceStack->setCurrentWidget(ui.emptyPage);
		ui.emptyStateLabel->setFocus(Qt::OtherFocusReason);
	}
	else
	{
		QTimer::singleShot(0, this, &DeviceSelector::loadDevices);
	}

	if (!snapshotMode)
	{
		bool fixedAudioDG = !DeviceAPOInfo::checkProtectedAudioDG(true);
		bool fixedRegistration = !DeviceAPOInfo::checkAPORegistration(true);
		if (fixedAudioDG || fixedRegistration)
		{
			QMessageBox::information(this, tr("Info"), tr("A registry value that is required for the operation of Hibiki EQAPO was not set correctly. "
				"This might have been caused by a driver installation or uninstallation. The value has been corrected. A reboot may be required so that the changes can take effect."));
			askForReboot = true;
		}
	}
}

void DeviceSelector::configureDeviceTree()
{
	ui.deviceTreeWidget->setContextMenuPolicy(Qt::CustomContextMenu);
	ui.deviceTreeWidget->setIconSize(
		QSize(style()->pixelMetric(QStyle::PM_SmallIconSize), style()->pixelMetric(QStyle::PM_SmallIconSize)));
	ui.deviceTreeWidget->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
	ui.deviceTreeWidget->header()->setSectionResizeMode(1, QHeaderView::Stretch);
	ui.deviceTreeWidget->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
	ui.deviceTreeWidget->setAccessibleName(tr("Audio devices"));
	ui.deviceTreeWidget->setAccessibleDescription(ui.requestLabel->text());
	ui.loadingProgressBar->setAccessibleName(tr("Loading audio devices"));
	ui.selectionSummaryLabel->setAccessibleDescription(tr("Device selection summary"));
	ui.changeStatusLabel->setAccessibleDescription(tr("Pending changes"));
	ui.installModeComboBox->setAccessibleName(tr("APO install mode"));
	ui.installModeComboBox->setAccessibleDescription(ui.autoCheckBox->toolTip());
	ui.installPreMixCheckBox->setAccessibleName(
		tr("Pre-mix:") + QStringLiteral(" ") + ui.installPreMixCheckBox->text());
	ui.useOriginalAPOPreMixCheckBox->setAccessibleName(
		tr("Pre-mix:") + QStringLiteral(" ") + ui.useOriginalAPOPreMixCheckBox->text());
	ui.installPostMixCheckBox->setAccessibleName(
		tr("Post-mix:") + QStringLiteral(" ") + ui.installPostMixCheckBox->text());
	ui.useOriginalAPOPostMixCheckBox->setAccessibleName(
		tr("Post-mix:") + QStringLiteral(" ") + ui.useOriginalAPOPostMixCheckBox->text());
	ui.copyDeviceCommandAction->setShortcutContext(Qt::WidgetWithChildrenShortcut);
	ui.deviceTreeWidget->addAction(ui.copyDeviceCommandAction);
	ui.errorStateLabel->setProperty("statusLevel", "danger");
}

void DeviceSelector::loadDevices()
{
	ui.deviceStack->setCurrentWidget(ui.loadingPage);
	ui.deviceTreeWidget->clear();
	QSignalBlocker treeSignalBlocker(ui.deviceTreeWidget);
	QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);

	try
	{
		QTreeWidgetItem* outputNode = new QTreeWidgetItem(ui.deviceTreeWidget, QStringList(tr("Playback devices")));
		outputNode->setFlags(Qt::ItemIsEnabled);
		outputNode->setFirstColumnSpanned(true);
		outputNode->setExpanded(true);
		QFont outputFont = outputNode->font(0);
		outputFont.setWeight(QFont::DemiBold);
		outputNode->setFont(0, outputFont);
		std::vector<std::shared_ptr<AbstractAPOInfo>> outputDevices = DeviceAPOInfo::loadAllInfos(false);
		addDevices(outputDevices, outputNode);

		QTreeWidgetItem* inputNode = new QTreeWidgetItem(ui.deviceTreeWidget, QStringList(tr("Capture devices")));
		inputNode->setFlags(Qt::ItemIsEnabled);
		inputNode->setFirstColumnSpanned(true);
		inputNode->setExpanded(true);
		QFont inputFont = inputNode->font(0);
		inputFont.setWeight(QFont::DemiBold);
		inputNode->setFont(0, inputFont);
		std::vector<std::shared_ptr<AbstractAPOInfo>> inputDevices = DeviceAPOInfo::loadAllInfos(true);
		addDevices(inputDevices, inputNode);

		if (outputDevices.empty())
			delete outputNode;
		if (inputDevices.empty())
			delete inputNode;
	}
	catch (RegistryException e)
	{
		ui.deviceTreeWidget->clear();
		const QString message = QString::fromStdWString(e.getMessage());
		ui.errorStateLabel->setText(tr("Error while accessing the registry") + "\n" + message);
		ui.deviceStack->setCurrentWidget(ui.errorPage);
		ui.errorStateLabel->setFocus(Qt::OtherFocusReason);
		updateButtons();
		QMessageBox::critical(this, tr("Error while accessing the registry"), message);
		return;
	}

	if (ui.deviceTreeWidget->topLevelItemCount() == 0)
	{
		ui.deviceStack->setCurrentWidget(ui.emptyPage);
		ui.emptyStateLabel->setFocus(Qt::OtherFocusReason);
	}
	else
	{
		ui.deviceStack->setCurrentWidget(ui.devicesPage);
		QTreeWidgetItem* preferredItem = nullptr;
		for (int groupIndex = 0; groupIndex < ui.deviceTreeWidget->topLevelItemCount(); ++groupIndex)
		{
			QTreeWidgetItem* group = ui.deviceTreeWidget->topLevelItem(groupIndex);
			for (int itemIndex = 0; itemIndex < group->childCount(); ++itemIndex)
			{
				QTreeWidgetItem* item = group->child(itemIndex);
				std::shared_ptr<AbstractAPOInfo> apoInfo = item->data(0, DeviceInfoRole).value<std::shared_ptr<AbstractAPOInfo>>();
				if (preferredItem == nullptr || apoInfo->isDefaultDevice())
					preferredItem = item;
				if (apoInfo->isDefaultDevice())
					break;
			}
		}
		if (preferredItem != nullptr)
		{
			ui.deviceTreeWidget->setCurrentItem(preferredItem);
			preferredItem->setSelected(true);
		}
		ui.deviceTreeWidget->setFocus(Qt::OtherFocusReason);
	}

	updateButtons();
}

void DeviceSelector::changeEvent(QEvent* event)
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

void DeviceSelector::updateThemeAssets()
{
	updateTypography();
	const QIcon deviceIcon = createPaletteIcon(PaletteIconGlyph::device);
	setWindowIcon(deviceIcon);
	const int iconSize = qMax(24, style()->pixelMetric(QStyle::PM_LargeIconSize));
	const int iconSlotSize = qMax(iconSize, fontMetrics().height() * 2);
	ui.headerIconLabel->setFixedSize(iconSlotSize, iconSlotSize);
	ui.headerIconLabel->setPixmap(deviceIcon.pixmap(iconSize, iconSize));
	ui.deviceTreeWidget->setIconSize(
		QSize(style()->pixelMetric(QStyle::PM_SmallIconSize), style()->pixelMetric(QStyle::PM_SmallIconSize)));

	for (int groupIndex = 0; groupIndex < ui.deviceTreeWidget->topLevelItemCount(); ++groupIndex)
	{
		QTreeWidgetItem* group = ui.deviceTreeWidget->topLevelItem(groupIndex);
		for (int itemIndex = 0; itemIndex < group->childCount(); ++itemIndex)
			updateDeviceAppearance(group->child(itemIndex));
	}
}

void DeviceSelector::updateTypography()
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
	ui.requestLabel->setFont(headingFont);
}

void DeviceSelector::addDevices(std::vector<std::shared_ptr<AbstractAPOInfo>>& devices, QTreeWidgetItem* parentNode)
{
	for (const std::shared_ptr<AbstractAPOInfo>& apoInfo : devices)
	{
		QStringList values;
		values.append(QString::fromStdWString(apoInfo->getConnectionName()));
		values.append(QString::fromStdWString(apoInfo->getDeviceName()));

		VoicemeeterAPOInfo* voicemeeterInfo = dynamic_cast<VoicemeeterAPOInfo*>(apoInfo.get());
		bool checked = false;
		if (apoInfo->isInstalled())
		{
			if (voicemeeterInfo != NULL && !voicemeeterInfo->isVoicemeeterInstalled())
				checked = false;
			else
				checked = true;
		}
		QString state = getStateText(apoInfo, checked);

		values.append(state);
		QTreeWidgetItem* item = new QTreeWidgetItem(parentNode, values);

		item->setCheckState(0, checked ? Qt::Checked : Qt::Unchecked);
		item->setData(0, DeviceInfoRole, QVariant::fromValue(apoInfo));
		updateDeviceAppearance(item);
	}
}

void DeviceSelector::updateDeviceAppearance(QTreeWidgetItem* item)
{
	if (item == nullptr || item->parent() == nullptr)
		return;

	std::shared_ptr<AbstractAPOInfo> apoInfo = item->data(0, DeviceInfoRole).value<std::shared_ptr<AbstractAPOInfo>>();
	if (!apoInfo)
		return;

	const bool checked = item->checkState(0) == Qt::Checked;
	const bool pending = hasPendingChange(apoInfo, checked);
	const bool unavailable = apoInfo->isDisabled() || apoInfo->isUnplugged();

	PaletteIconGlyph glyph = PaletteIconGlyph::device;
	if (unavailable)
		glyph = PaletteIconGlyph::unavailable;
	else if (pending)
		glyph = PaletteIconGlyph::waiting;
	else if (checked)
		glyph = PaletteIconGlyph::success;
	else if (apoInfo->isExperimental())
		glyph = PaletteIconGlyph::warning;

	item->setIcon(2, createPaletteIcon(glyph));
	QFont stateFont = item->font(2);
	stateFont.setWeight(pending ? QFont::DemiBold : QFont::Normal);
	item->setFont(2, stateFont);

	QPalette::ColorGroup group = item->flags().testFlag(Qt::ItemIsEnabled)
		? QPalette::Active : QPalette::Disabled;
	QPalette::ColorRole role = QPalette::Text;
	if (pending)
		role = QPalette::Highlight;
	else if (unavailable || apoInfo->isExperimental())
		role = QPalette::PlaceholderText;
	item->setForeground(2, palette().brush(group, role));
}

void DeviceSelector::onDeviceSelectionChanged()
{
	updateButtons();
}

void DeviceSelector::onDeviceToggled(QTreeWidgetItem* item)
{
	if (item == nullptr || item->parent() == nullptr)
		return;

	updateList(item);
	updateButtons();
}

void DeviceSelector::onDeviceContextMenuRequested(const QPoint& pos)
{
	QMenu menu(this);
	menu.addAction(ui.copyDeviceCommandAction);
	menu.exec(ui.deviceTreeWidget->mapToGlobal(pos));
}

void DeviceSelector::onDialogAccepted()
{
	bool deviceUpdated = false;

	for (int index = 0; index < ui.deviceTreeWidget->topLevelItemCount(); index++)
	{
		QTreeWidgetItem* topItem = ui.deviceTreeWidget->topLevelItem(index);
		for (int i = 0; i < topItem->childCount(); i++)
		{
			QTreeWidgetItem* item = topItem->child(i);
			std::shared_ptr<AbstractAPOInfo> info = item->data(0, DeviceInfoRole).value<std::shared_ptr<AbstractAPOInfo>>();
			bool checked = item->checkState(0) == Qt::Checked;

			try
			{
				DeviceAPOInfo* deviceInfo = dynamic_cast<DeviceAPOInfo*>(info.get());
				if (checked && !info->isInstalled())
				{
					info->install();
					if (deviceInfo != NULL)
						deviceUpdated = true;
				}
				else if (!checked && info->isInstalled())
				{
					info->uninstall();
					if (deviceInfo != NULL)
						deviceUpdated = true;
				}
				else if (checked && (info->canBeUpgraded() || info->hasChanges() || info->isEnhancementsDisabled()))
				{
					info->reinstall();
					if (deviceInfo != NULL)
						deviceUpdated = true;
				}
			}
			catch (RegistryException e)
			{
				QMessageBox::critical(this, tr("Error while accessing the registry"), QString::fromStdWString(e.getMessage()));
			}
		}
	}

	VoicemeeterAPOInfo::ensureVoicemeeterClientRunning();

	finish(deviceUpdated);
}

void DeviceSelector::onDialogRejected()
{
	if (hasUpgrades())
	{
		if (QMessageBox::warning(this, tr("Upgrades available"), tr("The APO installation of some devices should be upgraded. Do you really want to cancel?"),
			QMessageBox::StandardButtons(QMessageBox::Yes | QMessageBox::No)) == QMessageBox::No)
			return;
	}

	finish(false);
}

void DeviceSelector::finish(bool deviceUpdated)
{
	int dialogResult = 0;
	if (QCoreApplication::instance()->arguments().contains("/i")
		|| deviceUpdated || askForReboot)
	{
		DeviceTestDialog testDialog;
		dialogResult = testDialog.exec();
	}

	int returnCode = 0;
	if (QCoreApplication::instance()->arguments().contains("/i"))
	{
		QMessageBox::information(this, tr("Info"), tr("This dialog can be reopened anytime by launching Device Selector from the start menu."));
		if (dialogResult == -1)
			returnCode = 1;
	}
	else if (dialogResult == -1)
	{
		if (QMessageBox::question(this, tr("Reboot"), tr("To apply the changes, Windows should be rebooted. Reboot now?")) == QMessageBox::Yes)
		{
			HANDLE tokenHandle;
			if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tokenHandle))
			{
				LUID luid;
				if (LookupPrivilegeValue(NULL, SE_SHUTDOWN_NAME, &luid))
				{
					TOKEN_PRIVILEGES tp;
					tp.PrivilegeCount = 1;
					tp.Privileges[0].Luid = luid;
					tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

					if (AdjustTokenPrivileges(tokenHandle, FALSE, &tp, sizeof(TOKEN_PRIVILEGES), NULL, NULL))
						InitiateShutdownW(NULL, NULL, 0, SHUTDOWN_RESTART | SHUTDOWN_GRACE_OVERRIDE, SHTDN_REASON_MAJOR_APPLICATION | SHTDN_REASON_MINOR_MAINTENANCE);
				}

				CloseHandle(tokenHandle);
			}
		}
	}

	QCoreApplication::exit(returnCode);
}

void DeviceSelector::onCopyDeviceCommandClicked()
{
	QString command = "Device: ";

	QList<QTreeWidgetItem*> list = ui.deviceTreeWidget->selectedItems();

	bool first = true;
	for (QTreeWidgetItem* item : list)
	{
		if (item->childCount() != 0)
			continue;

		if (first)
			first = false;
		else
			command += "; ";

		std::shared_ptr<AbstractAPOInfo> info = item->data(0, DeviceInfoRole).value<std::shared_ptr<AbstractAPOInfo>>();
		command += QString::fromStdWString(info->getDeviceGuid().empty() ? info->getDeviceString() : info->getDeviceGuid()).replace(';', ' ');
	}

	QClipboard* clipboard = QGuiApplication::clipboard();
	clipboard->setText(command);
}

void DeviceSelector::onTroubleShootingToggled(bool on)
{
	ui.stackedWidget->setVisible(on);
}

void DeviceSelector::onTroubleShootingOptionChanged()
{
	QList<QTreeWidgetItem*> list = ui.deviceTreeWidget->selectedItems();
	for (QTreeWidgetItem* item : list)
	{
		if (item->childCount() != 0)
			continue;

		std::shared_ptr<AbstractAPOInfo> info = item->data(0, DeviceInfoRole).value<std::shared_ptr<AbstractAPOInfo>>();
		DeviceAPOInfo* deviceInfo = dynamic_cast<DeviceAPOInfo*>(info.get());
		if (deviceInfo != NULL)
		{
			QObject* sender = QObject::sender();
			if (sender == ui.installPreMixCheckBox)
				deviceInfo->getSelectedInstallState().installPreMix = ui.installPreMixCheckBox->isChecked();
			else if (sender == ui.installPostMixCheckBox)
				deviceInfo->getSelectedInstallState().installPostMix = ui.installPostMixCheckBox->isChecked();
			else if (sender == ui.useOriginalAPOPreMixCheckBox)
				deviceInfo->getSelectedInstallState().useOriginalAPOPreMix = ui.useOriginalAPOPreMixCheckBox->isChecked();
			else if (sender == ui.useOriginalAPOPostMixCheckBox)
				deviceInfo->getSelectedInstallState().useOriginalAPOPostMix = ui.useOriginalAPOPostMixCheckBox->isChecked();
			else if (sender == ui.installModeComboBox)
				deviceInfo->getSelectedInstallState().installMode = (DeviceAPOInfo::InstallMode)ui.installModeComboBox->currentIndex();
			else if (sender == ui.allowSilentBufferCheckBox)
				deviceInfo->getSelectedInstallState().allowSilentBufferModification = ui.allowSilentBufferCheckBox->isChecked();
			else if (sender == ui.autoCheckBox)
				deviceInfo->getSelectedInstallState().autoAdjust = ui.autoCheckBox->isChecked();
		}

		updateList(item);
	}

	updateButtons();
}

void DeviceSelector::updateList(QTreeWidgetItem* item)
{
	std::shared_ptr<AbstractAPOInfo> apoInfo = item->data(0, DeviceInfoRole).value<std::shared_ptr<AbstractAPOInfo>>();
	bool checked = item->checkState(0) == Qt::Checked;

	QString state = getStateText(apoInfo, checked);
	item->setText(2, state);
	updateDeviceAppearance(item);
}

void DeviceSelector::updateButtons()
{
	const bool changed = isChanged();
	const bool anySelected = isAnySelected();
	int deviceCount = 0;
	int enabledCount = 0;
	int pendingCount = 0;
	for (int groupIndex = 0; groupIndex < ui.deviceTreeWidget->topLevelItemCount(); ++groupIndex)
	{
		QTreeWidgetItem* group = ui.deviceTreeWidget->topLevelItem(groupIndex);
		for (int itemIndex = 0; itemIndex < group->childCount(); ++itemIndex)
		{
			QTreeWidgetItem* item = group->child(itemIndex);
			std::shared_ptr<AbstractAPOInfo> apoInfo = item->data(0, DeviceInfoRole).value<std::shared_ptr<AbstractAPOInfo>>();
			if (!apoInfo)
				continue;
			const bool checked = item->checkState(0) == Qt::Checked;
			++deviceCount;
			if (checked)
				++enabledCount;
			if (hasPendingChange(apoInfo, checked))
				++pendingCount;
		}
	}

	if (deviceCount > 0)
		ui.selectionSummaryLabel->setText(tr("%1 enabled · %2 pending").arg(enabledCount).arg(pendingCount));
	else
		ui.selectionSummaryLabel->clear();

	const QByteArray statusLevel = pendingCount > 0 ? QByteArray("warning") : QByteArray("normal");
	if (ui.selectionSummaryLabel->property("statusLevel").toByteArray() != statusLevel)
	{
		ui.selectionSummaryLabel->setProperty("statusLevel", statusLevel);
		refreshDynamicStyle(ui.selectionSummaryLabel);
	}

	if (deviceCount == 0)
		ui.changeStatusLabel->clear();
	else if (pendingCount > 0)
		ui.changeStatusLabel->setText(tr("Review the pending changes, then choose OK to apply them."));
	else if (enabledCount > 0)
		ui.changeStatusLabel->setText(tr("No pending changes."));
	else
		ui.changeStatusLabel->setText(tr("Select at least one device to enable Hibiki EQAPO."));
	if (ui.changeStatusLabel->property("statusLevel").toByteArray() != statusLevel)
	{
		ui.changeStatusLabel->setProperty("statusLevel", statusLevel);
		refreshDynamicStyle(ui.changeStatusLabel);
	}

	if (deviceCount == 0)
	{
		QPushButton* okButton = ui.buttonBox->button(QDialogButtonBox::Ok);
		okButton->setVisible(false);
		QPushButton* cancelButton = ui.buttonBox->button(QDialogButtonBox::Cancel);
		cancelButton->setText(tr("Close"));
	}
	else if (changed || !anySelected)
	{
		QPushButton* okButton = ui.buttonBox->button(QDialogButtonBox::Ok);
		okButton->setVisible(true);
		okButton->setEnabled(changed);
		QPushButton* cancelButton = ui.buttonBox->button(QDialogButtonBox::Cancel);
		cancelButton->setText(tr("Cancel"));
	}
	else
	{
		QPushButton* okButton = ui.buttonBox->button(QDialogButtonBox::Ok);
		okButton->setVisible(false);
		QPushButton* cancelButton = ui.buttonBox->button(QDialogButtonBox::Cancel);
		cancelButton->setText(tr("Close"));
	}

	QList<QTreeWidgetItem*> list = ui.deviceTreeWidget->selectedItems();
	bool noGroupsSelected = !list.isEmpty();
	for (QTreeWidgetItem* item : list)
	{
		if (item->childCount() != 0)
		{
			noGroupsSelected = false;
			break;
		}
	}

	ui.copyDeviceCommandAction->setEnabled(noGroupsSelected);

	bool enable = false;
	bool isInput = false;
	bool hasOriginalAPOPreMix = true;
	bool hasOriginalAPOPostMix = true;
	DeviceAPOInfo::InstallState installState;
	if (noGroupsSelected && list.size() == 1)
	{
		QTreeWidgetItem* item = list[0];
		enable = item->checkState(0) == Qt::Checked;

		std::shared_ptr<AbstractAPOInfo> apoInfo = item->data(0, DeviceInfoRole).value<std::shared_ptr<AbstractAPOInfo>>();
		DeviceAPOInfo* deviceApoInfo = dynamic_cast<DeviceAPOInfo*>(apoInfo.get());
		if (deviceApoInfo != NULL)
		{
			isInput = deviceApoInfo->isInput();
			hasOriginalAPOPreMix = deviceApoInfo->getOriginalAPOPreMix() != L"";
			hasOriginalAPOPostMix = deviceApoInfo->getOriginalAPOPostMix() != L"";
			installState = deviceApoInfo->getSelectedInstallState();
		}
	}

	ui.preMixLabel->setEnabled(enable);
	ui.postMixLabel->setEnabled(enable && !isInput);
	ui.installPreMixCheckBox->setEnabled(enable);
	ui.installPostMixCheckBox->setEnabled(enable && !isInput);
	ui.useOriginalAPOPreMixCheckBox->setEnabled(enable && hasOriginalAPOPreMix && installState.installPreMix);
	ui.useOriginalAPOPostMixCheckBox->setEnabled(enable && !isInput && hasOriginalAPOPostMix && installState.installPostMix);
	ui.installModeComboBox->setEnabled(enable);
	ui.allowSilentBufferCheckBox->setEnabled(enable);
	ui.autoCheckBox->setEnabled(enable);
	ui.stackedWidget->setCurrentIndex(enable ? 1 : 0);

	ui.installPreMixCheckBox->setChecked(installState.installPreMix);
	ui.installPostMixCheckBox->setChecked(installState.installPostMix);
	ui.useOriginalAPOPreMixCheckBox->setChecked(installState.useOriginalAPOPreMix && hasOriginalAPOPreMix);
	ui.useOriginalAPOPostMixCheckBox->setChecked(installState.useOriginalAPOPostMix && hasOriginalAPOPostMix);

	if (RegistryHelper::isWindowsVersionAtLeast(6, 3)) // Windows 8.1
		ui.installModeComboBox->setCurrentIndex(installState.installMode);

	ui.allowSilentBufferCheckBox->setChecked(installState.allowSilentBufferModification);
	ui.autoCheckBox->setChecked(installState.autoAdjust);
}

bool DeviceSelector::isAnySelected()
{
	bool anySelected = false;

	for (int index = 0; index < ui.deviceTreeWidget->topLevelItemCount(); index++)
	{
		QTreeWidgetItem* topItem = ui.deviceTreeWidget->topLevelItem(index);
		for (int i = 0; i < topItem->childCount(); i++)
		{
			QTreeWidgetItem* item = topItem->child(i);
			if (item->checkState(0) == Qt::Checked)
			{
				anySelected = true;
				break;
			}
		}
	}

	return anySelected;
}

bool DeviceSelector::isChanged()
{
	bool changed = false;

	for (int index = 0; index < ui.deviceTreeWidget->topLevelItemCount(); index++)
	{
		QTreeWidgetItem* topItem = ui.deviceTreeWidget->topLevelItem(index);
		for (int i = 0; i < topItem->childCount(); i++)
		{
			QTreeWidgetItem* item = topItem->child(i);
			std::shared_ptr<AbstractAPOInfo> apoInfo = item->data(0, DeviceInfoRole).value<std::shared_ptr<AbstractAPOInfo>>();
			bool checked = item->checkState(0) == Qt::Checked;
			if (checked != apoInfo->isInstalled()
				|| checked && apoInfo->isInstalled() && (apoInfo->canBeUpgraded() || apoInfo->hasChanges() || apoInfo->isEnhancementsDisabled()))
			{
				changed = true;
				break;
			}
		}
	}

	return changed;
}

bool DeviceSelector::hasUpgrades()
{
	bool hasUpgrades = false;

	for (int index = 0; index < ui.deviceTreeWidget->topLevelItemCount(); index++)
	{
		QTreeWidgetItem* topItem = ui.deviceTreeWidget->topLevelItem(index);
		for (int i = 0; i < topItem->childCount(); i++)
		{
			QTreeWidgetItem* item = topItem->child(i);
			std::shared_ptr<AbstractAPOInfo> apoInfo = item->data(0, DeviceInfoRole).value<std::shared_ptr<AbstractAPOInfo>>();
			bool checked = item->checkState(0) == Qt::Checked;
			if (checked && apoInfo->isInstalled() && (apoInfo->canBeUpgraded() || apoInfo->isEnhancementsDisabled()))
			{
				hasUpgrades = true;
				break;
			}
		}
	}

	return hasUpgrades;
}

QString DeviceSelector::getStateText(const std::shared_ptr<AbstractAPOInfo>& apoInfo, bool checked)
{
	QString state;
	if (checked && !apoInfo->isInstalled())
		state = tr("APO will be installed");
	else if (!checked && apoInfo->isInstalled())
		state = tr("APO will be uninstalled");
	else if (apoInfo->isInstalled() && apoInfo->canBeUpgraded())
		state = tr("APO will be upgraded");
	else if (apoInfo->isInstalled() && apoInfo->hasChanges())
		state = tr("APO installation will be changed");
	else if (apoInfo->isInstalled() && apoInfo->isEnhancementsDisabled())
		state = tr("Audio enhancements will be enabled");
	else if (apoInfo->isInstalled())
		state = tr("APO is already installed");
	else if (apoInfo->isExperimental())
		state = tr("APO can be installed (experimental)");
	else
		state = tr("APO can be installed");

	VoicemeeterAPOInfo* voicemeeterInfo = dynamic_cast<VoicemeeterAPOInfo*>(apoInfo.get());
	if (voicemeeterInfo != NULL && !voicemeeterInfo->isVoicemeeterInstalled())
		state += ", " + tr("Voicemeeter was uninstalled");
	else if (apoInfo->isDefaultDevice())
		state += ", " + tr("Default device");

	if (apoInfo->isDisabled())
		state += ", " + tr("Disabled");
	if (apoInfo->isUnplugged())
		state += ", " + tr("Unplugged");

	return state;
}
