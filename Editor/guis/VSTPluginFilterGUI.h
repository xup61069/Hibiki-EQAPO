/*
    This file is part of Equalizer APO, a system-wide equalizer.
    Copyright (C) 2017  Jonas Thedering

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

#pragma once

#include <cstdint>
#include <memory>
#include <QElapsedTimer>
#include <QString>
#include <QTimer>
#include <vector>
#include "Editor/IFilterGUI.h"
#include "helpers/VSTPluginLibrary.h"
#include "helpers/VSTPluginInstance.h"

namespace Ui {
class VSTPluginFilterGUI;
}

class VSTPluginFilterGUI : public IFilterGUI
{
	Q_OBJECT

public:
	explicit VSTPluginFilterGUI(std::shared_ptr<VSTPluginLibrary> library, const std::wstring& chunkData, const std::unordered_map<std::wstring, float>& paramMap, bool outProcMode = false, const QString& hostId = QString(), int vst3ClassIndex = 0, const std::wstring& midiConfig = std::wstring());
	~VSTPluginFilterGUI();

	void store(QString& command, QString& parameters) override;
	void loadPreferences(const QVariantMap& prefs) override;
	void storePreferences(QVariantMap& prefs) override;
	void restoreRuntimeState(const QVariantMap& state) override;
	void takeRuntimeState(QVariantMap& state) override;
	bool prepareDelete() override;
	bool commitDelete() override;
	void onAutomate();
	void onSizeWindow(int w, int h);

private slots:
	void on_openPanelButton_clicked();
	void on_reloadButton_clicked();
	void on_midiButton_clicked();
	void on_vst3ClassComboBox_currentIndexChanged(int index);
	void applyDialog();
	void autoApplyToggled(bool checked);
	void on_pathLineEdit_editingFinished();
	void on_selectButton_clicked();
	void on_idle();

private:
	void storeWithMidiConfig(
		QString& command,
		QString& parameters,
		const std::wstring& serializedMidiConfig) const;
	void initPlugin();
	void openOutProcPanel();
	bool signalOutProcPanel(const wchar_t* suffix, std::uint32_t* error = nullptr);
	bool consumeOutProcPanelSignal(const wchar_t* suffix);
	void closeOutProcPanel();
	bool terminateOutProcPanel(
		bool* stateChanged = nullptr,
		QString* failureMessage = nullptr,
		bool* failureReported = nullptr);
	bool ensureOutProcPanelStopped(bool synchronizeRecoveredState);
	bool markOutProcPanelStoppedPreservingState(
		QString* failureMessage = nullptr);
	void releasePluginInstance();
	void refreshVST3ClassComboBox();
	void updatePermissionWarning();
	bool capturePluginStateIfChanged();
	std::vector<VSTParameterDescriptor> availableMidiParameters() const;
	void updateMidiButton();

	Ui::VSTPluginFilterGUI* ui;
	std::shared_ptr<VSTPluginLibrary> library;
	VSTPluginInstance* effect = NULL;
	QTimer idleTimer;
	std::wstring chunkData;
	std::unordered_map<std::wstring, float> paramMap;
	std::wstring midiConfig;
	std::vector<VSTParameterDescriptor> outProcParameterDescriptors;
	bool outProcMode = false;
	QString hostId;
	bool outProcGuiRunning = false;
	qint64 outProcGuiPid = 0;
	std::uint64_t outProcGuiProcessCreationTime = 0;
	QString outProcGuiConfigPath;
	bool outProcGuiHidden = false;
	bool outProcFinalStateCommitted = false;
	bool autoApplyDialog = false;
	bool outProcRuntimeStateTransferred = false;
	QElapsedTimer lastReadTimer;
	int vst3ClassIndex = 0;
	bool automationDirty = false;
};
