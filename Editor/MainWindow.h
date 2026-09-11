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

#pragma once

#include <string>
#include <vector>
#include <QByteArray>
#include <QMainWindow>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QPointer>

#include "FilterTable.h"
#include "DeviceAPOInfo.h"
#include "Editor/AnalysisPlotScene.h"
#include "Editor/AnalysisThread.h"
#include "helpers/RegistryHelper.h"

#define EDITOR_REGPATH USER_REGPATH L"\\Configuration Editor"
#define EDITOR_PER_FILE_REGPATH EDITOR_REGPATH L"\\file-specific"

namespace Ui {
class MainWindow;
}

class QAction;
class QFileSystemWatcher;
class QLabel;
class QLineEdit;
class QMenu;
class QPushButton;
class QSystemTrayIcon;
class QToolBar;

class MainWindow : public QMainWindow
{
	Q_OBJECT

public:
	explicit MainWindow(QDir configDir, QWidget* parent = 0);
	~MainWindow();
	void doChecks();
	void runDeviceSelector();
	bool load(QString path);
	bool save(FilterTable* filterTable, QString path);
	bool isEmpty();
	bool shouldRestart();
	void startAnalysis();
	bool loadSnapshotScenario(const QString& scenario);
	bool snapshotLayoutIsValid() const;
	bool beginTemporaryFilterConfiguration(
		IFilterGUI* filterGui,
		const QString& command,
		const QString& parameters,
		const QString& mode);
	bool restoreTemporaryFilterConfiguration(bool* keptExternal = NULL);

protected:
	void closeEvent(QCloseEvent* event) override;

private slots:
	void deviceSelected(int index);
	void channelConfigurationSelected(int index);
	void linesChanged();

	bool on_tabWidget_tabCloseRequested(int index);
	void on_actionOpen_triggered();
	void on_actionSave_triggered();
	void on_actionSaveAs_triggered();
	void on_actionNew_triggered();
	void recentFileSelected();

	void on_actionCut_triggered();
	void on_actionCopy_triggered();
	void on_actionPaste_triggered();
	void on_actionDelete_triggered();
	void on_actionSelectAll_triggered();

	void instantModeEnabled(bool enabled);
	void on_tabWidget_currentChanged(int index);
	void on_startFromComboBox_activated(int index);
	void on_analysisChannelComboBox_activated(int index);
	void on_resolutionSpinBox_valueChanged(int value);
	void updateAnalysisPanel();
	void profileSelected(int index);
	void duplicateCurrentProfile();
	void renameCurrentProfile();
	void importProfile();
	void exportCurrentProfile();
	void linkCurrentProfileToDevice();
	void clearCurrentDeviceProfileLink();
	void findNext();
	void findPrevious();
	void searchTextChanged(const QString& text);
	void captureComparisonA();
	void comparisonToggled(bool showA);
	void bypassToggled(bool enabled);
	void asioDeviceSelected(int index);
	void closeToTrayToggled(bool enabled);
	void takeoverVolumeKeysToggled(bool enabled);

	void on_mainToolBar_visibilityChanged(bool visible);
	void on_analysisDockWidget_visibilityChanged(bool visible);
	void on_actionToolbar_triggered(bool checked);
	void on_actionAnalysisPanel_triggered(bool checked);
	void dockAnalysisPanel();
	void lowerPreampToPreventClipping();

	void languageSelected(bool selected);
	void on_actionResetAllGlobalPreferences_triggered();
	void on_actionResetAllFileSpecificPreferences_triggered();

private:
	FilterTable* addTab(QString title, QString tooltip, QString configPath, QList<QString> lines);
	void getDeviceAndChannelMask(std::shared_ptr<AbstractAPOInfo>* selectedDevice, int* channelMask) const;
	bool askForClose(int tabIndex);
	void loadPreferences();
	void savePreferences();
	void updateRecentFiles();
	void setupWorkspaceTools();
	void refreshAsioDeviceControl();
	bool restoreWindowLayoutState(const QByteArray& state);
	QByteArray saveWindowLayoutState() const;
	void setupTrayIcon();
	void refreshProfiles();
	void refreshProfileMenus();
	void syncProfileSelection();
	void refreshWorkspaceActionState();
	QAction* doublePrecisionAction = nullptr;
	void invalidateAnalysisResult();
	void refreshAutoPreampActionState();
	bool analysisResultCanAdjustPreamp() const;
	bool restoreTemporaryProcessingState();
	bool prepareTemporaryContents(
		FilterTable* filterTable,
		const QList<QString>& expectedLines,
		const QList<QString>& temporaryLines,
		QByteArray* originalContent,
		QByteArray* temporaryContent,
		bool* externalConflict);
	bool setTemporaryLines(
		FilterTable* filterTable,
		const QList<QString>& lines,
		const QByteArray& expectedContent,
		const QByteArray& temporaryContent,
		bool* externalConflict = NULL);
	bool restoreTemporaryLines(
		FilterTable* filterTable,
		const QList<QString>& originalLines,
		const QByteArray& originalContent,
		const QByteArray& expectedTemporaryContent);
	bool writeTemporaryRecoveryJournal(
		FilterTable* filterTable,
		const QList<QString>& originalLines,
		const QByteArray& originalContent,
		const QByteArray& temporaryContent,
		const QString& mode);
	void clearTemporaryRecoveryJournal();
	void recoverInterruptedTemporaryProcessingState();
	void showWorkspaceStatus(const QString& text, const char* level = "normal", int timeoutMs = 4500);
	FilterTable* currentFilterTable() const;
	QString currentProfilePath() const;
	bool isFilterTableDirty(const FilterTable* filterTable) const;
	QString linkedProfileForCurrentDevice() const;
	QString deviceLinkKey() const;
	bool validateProfileName(const QString& name, QString* fileName) const;
	template<class T> QList<T> toQList(const std::vector<T>& vector);

	Ui::MainWindow* ui;

	QDir configDir;
	QCheckBox* instantModeCheckBox;
	QCheckBox* doublePrecisionCheckBox = NULL;
	QComboBox* deviceComboBox;
	QComboBox* channelConfigurationComboBox;
	QComboBox* asioDeviceComboBox = NULL;
	QToolBar* workspaceToolBar = NULL;
	QComboBox* profileComboBox = NULL;
	QLineEdit* searchLineEdit = NULL;
	QLabel* workspaceStatusLabel = NULL;
	QLabel* headroomValueLabel = NULL;
	QLabel* analysisStateLabel = NULL;
	QPushButton* autoPreampButton = NULL;
	QFileSystemWatcher* profileWatcher = NULL;
	QSystemTrayIcon* trayIcon = NULL;
	QMenu* profileMenu = NULL;
	QMenu* openProfilesMenu = NULL;
	QMenu* trayProfileMenu = NULL;
	QAction* captureComparisonAction = NULL;
	QAction* comparisonAction = NULL;
	QAction* bypassAction = NULL;
	QAction* closeToTrayAction = NULL;
	QAction* takeoverVolumeKeysAction = NULL;
	bool takeoverVolumeKeys = false;
	QAction* trayBypassAction = NULL;
	QAction* dockAnalysisPanelAction = NULL;
	QList<std::shared_ptr<AbstractAPOInfo>> outputDevices;
	QList<std::shared_ptr<AbstractAPOInfo>> inputDevices;
	std::shared_ptr<AbstractAPOInfo> defaultOutputDevice;
	AnalysisPlotScene* analysisPlotScene;
	AnalysisThread* analysisThread = NULL;
	QPointer<FilterTable> requestedAnalysisTable;
	QByteArray requestedAnalysisLinesHash;
	QString requestedAnalysisConfigPath;
	QString requestedAnalysisDeviceId;
	quint64 requestedAnalysisGeneration = 0;
	quint64 acceptedAnalysisGeneration = 0;
	double latestAnalysisPeakGain = 0.0;
	QList<AnalysisConfigurationFileSnapshot> latestAnalysisConfigurationFiles;
	QList<AnalysisVolumeSnapshot> latestAnalysisVolumeSnapshots;
	int requestedAnalysisChannelMask = 0;
	int requestedAnalysisChannelIndex = -1;
	int requestedAnalysisStartFrom = -1;
	bool latestAnalysisResultValid = false;
	bool restart = false;
	bool noSavePreferences = false;
	bool noSaveFilePreferences = false;
	bool closeToTray = false;
	bool quitRequested = false;
	bool restoringTemporaryState = false;
	bool suppressInstantSave = false;
	bool applyingAutoPreampAdjustment = false;
	QString temporaryRecoveryOwner;
	quint64 temporaryRecoveryProcessStartedAt = 0;
	QPointer<FilterTable> temporaryFilterTable;
	QList<QString> temporaryFilterOriginalLines;
	QByteArray temporaryFilterOriginalContent;
	QByteArray temporaryFilterContent;
	quint64 workspaceStatusRevision = 0;
	QPointer<FilterTable> comparisonTable;
	QList<QString> comparisonALines;
	QList<QString> comparisonBLines;
	QByteArray comparisonOriginalContent;
	QByteArray comparisonTemporaryContent;
	bool showingComparisonA = false;
	QPointer<FilterTable> bypassTable;
	QList<QString> bypassOriginalLines;
	QList<QString> bypassTemporaryLines;
	QByteArray bypassOriginalContent;
	QByteArray bypassTemporaryContent;
	QStringList recentFiles;
};

Q_DECLARE_METATYPE(std::shared_ptr<AbstractAPOInfo>)
