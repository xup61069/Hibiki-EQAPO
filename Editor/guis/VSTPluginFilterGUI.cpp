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

#include <QFileInfo>
#include <algorithm>
#include <cstdint>
#include <QFileDialog>
#include <QCoreApplication>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QStyle>
#include <QStringList>
#include <QTextStream>
#include <QTimer>
#include <QUuid>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "outproc/OutProcAudioProtocol.h"
#include "outproc/OutProcVSTConfig.h"
#include "helpers/aeffectx.h"
#include "helpers/StringHelper.h"
#include "Editor/helpers/GUIHelper.h"
#include "helpers/UiSnapshot.h"
#include "Editor/MainWindow.h"
#include "VSTPluginFilterGUIDialog.h"
#include "VSTMidiMappingDialog.h"
#include "VSTPluginFilterGUI.h"
#include "ui_VSTPluginFilterGUI.h"

using namespace std;
using namespace std::placeholders;

static std::uint64_t getOutProcProcessCreationTime(DWORD processId);
static QString makeOutProcPidPath(const QString& hostId);

static bool isCanonicalOutProcSessionId(const QString& hostId)
{
	if (hostId.isEmpty() || hostId.size() > 128)
		return false;
	for (const QChar ch : hostId)
	{
		const ushort code = ch.unicode();
		const bool ok = (code >= '0' && code <= '9') ||
			(code >= 'a' && code <= 'z') ||
			(code >= 'A' && code <= 'Z') || code == '-' || code == '_';
		if (!ok)
			return false;
	}
	return true;
}

static QString makeSafeOutProcSessionId(const QString& hostId)
{
	QString safeId = hostId;
	for (QChar& ch : safeId)
	{
		const ushort code = ch.unicode();
		const bool ok = (code >= '0' && code <= '9') ||
			(code >= 'a' && code <= 'z') ||
			(code >= 'A' && code <= 'Z') || code == '-' || code == '_';
		if (!ok)
			ch = '_';
	}
	return safeId;
}

static QString makeOutProcObjectName(const QString& hostId, const wchar_t* suffix)
{
	const QString safeId = makeSafeOutProcSessionId(hostId);
	return "Global\\EqApoOutProcVST_" + safeId + "_" + QString::fromWCharArray(suffix);
}

static QString canonicalVSTPath(const QString& value)
{
	if (value.trimmed().isEmpty())
		return QString();

	QDir pluginsDir(QString::fromStdWString(
		VSTPluginLibrary::getDefaultPluginPath()));
	QFileInfo fileInfo(pluginsDir, value.trimmed());
	QString canonical = fileInfo.canonicalFilePath();
	if (canonical.isEmpty())
		canonical = fileInfo.absoluteFilePath();
	return QDir::toNativeSeparators(QDir::cleanPath(canonical));
}

static void appendOutProcDebugLog(const QString& message)
{
	QFile file(QDir::temp().absoluteFilePath("EqApoOutProcHost-debug.log"));
	if (file.open(QIODevice::Append | QIODevice::Text))
	{
		QTextStream stream(&file);
		stream << "[editor pid=" << GetCurrentProcessId() << "] " << message << "\n";
	}
}

static std::vector<VSTParameterDescriptor> convertOutProcParameterDescriptors(
	const std::vector<OutProcVSTParameterDescriptor>& source)
{
	std::vector<VSTParameterDescriptor> result;
	result.reserve(source.size());
	for (const OutProcVSTParameterDescriptor& serialized : source)
	{
		VSTParameterDescriptor descriptor;
		descriptor.api = serialized.api == OutProcVSTParameterApi::VST3
			? VSTParameterApi::VST3 : VSTParameterApi::VST2;
		descriptor.stableId = serialized.stableId;
		descriptor.name = serialized.name;
		descriptor.stepCount = serialized.stepCount;
		descriptor.normalizedValue = serialized.normalizedValue;
		descriptor.readOnly = serialized.readOnly;
		descriptor.hidden = serialized.hidden;
		result.push_back(std::move(descriptor));
	}
	return result;
}

static std::vector<OutProcVSTParameterDescriptor> convertOutProcParameterDescriptors(
	const std::vector<VSTParameterDescriptor>& source)
{
	std::vector<OutProcVSTParameterDescriptor> result;
	result.reserve(source.size());
	for (const VSTParameterDescriptor& parameter : source)
	{
		OutProcVSTParameterDescriptor descriptor;
		descriptor.api = parameter.api == VSTParameterApi::VST3
			? OutProcVSTParameterApi::VST3 : OutProcVSTParameterApi::VST2;
		descriptor.stableId = parameter.stableId;
		descriptor.name = parameter.name;
		descriptor.stepCount = parameter.stepCount;
		descriptor.normalizedValue = parameter.normalizedValue;
		descriptor.readOnly = parameter.readOnly;
		descriptor.hidden = parameter.hidden;
		result.push_back(std::move(descriptor));
	}
	return result;
}

VSTPluginFilterGUI::VSTPluginFilterGUI(std::shared_ptr<VSTPluginLibrary> library, const std::wstring& chunkData, const std::unordered_map<std::wstring, float>& paramMap, bool outProcMode, const QString& hostId, int vst3ClassIndex, const std::wstring& midiConfig)
	: ui(new Ui::VSTPluginFilterGUI), library(library), chunkData(chunkData), paramMap(paramMap), midiConfig(midiConfig), outProcMode(outProcMode), hostId(hostId), vst3ClassIndex(vst3ClassIndex)
{
	const bool canonicalizeHostId = outProcMode &&
		!isCanonicalOutProcSessionId(this->hostId);
	if (canonicalizeHostId)
		this->hostId = QUuid::createUuid().toString(QUuid::WithoutBraces);

	ui->setupUi(this);
	ui->label->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
	QFont font = ui->label->font();
	font.setWeight(QFont::DemiBold);
	ui->label->setFont(font);
	ui->selectButton->setIcon(GUIHelper::createThemeIcon(GUIHelper::ThemeIcon::OpenFolder));
	ui->label->setBuddy(ui->pathLineEdit);
	ui->pathLineEdit->setAccessibleName(ui->label->text());
	ui->selectButton->setToolTip(tr("Select VST plugin"));
	ui->selectButton->setAccessibleName(tr("Select VST plugin"));
	ui->vst3ClassComboBox->setAccessibleName(tr("VST3 plug-in class"));
	ui->classLabel->setBuddy(ui->vst3ClassComboBox);
	ui->midiButton->setAccessibleName(tr("MIDI control"));
	ui->statusLabel->setWordWrap(true);
	ui->statusLabel->setAccessibleName(tr("VST status"));
	ui->warningTextEdit->setProperty("statusLevel", "warning");
	ui->warningTextEdit->setReadOnly(true);
	ui->warningTextEdit->setLineWrapMode(QPlainTextEdit::WidgetWidth);
	ui->warningTextEdit->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	ui->warningTextEdit->setMinimumWidth(0);
	ui->gridLayout_3->removeWidget(ui->frame);
	ui->frame->setVisible(false);
	updatePermissionWarning();

	QString absolutePath = QString::fromStdWString(library->getLibPath());
	QDir pluginsDir(QString::fromStdWString(VSTPluginLibrary::getDefaultPluginPath()));
	QString relativePath = QDir::toNativeSeparators(pluginsDir.relativeFilePath(absolutePath));
	if (relativePath.startsWith(QDir::toNativeSeparators("../../")))
		relativePath = absolutePath;
	ui->pathLineEdit->setText(relativePath);
	refreshVST3ClassComboBox();
	updateMidiButton();

	connect(&idleTimer, &QTimer::timeout, this, &VSTPluginFilterGUI::on_idle);
	// This timer only coalesces state persistence and polls out-of-process
	// status. The plug-in editor owns its own high-frequency idle timer.
	idleTimer.setTimerType(Qt::CoarseTimer);
	idleTimer.setInterval(100);
	if (canonicalizeHostId)
	{
		// FilterTableRow connects updateModel after construction. Queue the
		// canonical serialization so hand-written/legacy unsafe IDs become
		// distinct persisted UUIDs before the user can open either panel.
		QTimer::singleShot(0, this, [this]() { emit updateModel(); });
	}
}

static bool canRemoveExistingFile(const QString& path)
{
	if (path.isEmpty())
		return true;
	const DWORD attributes = GetFileAttributesW(
		reinterpret_cast<LPCWSTR>(path.utf16()));
	if (attributes == INVALID_FILE_ATTRIBUTES)
	{
		const DWORD error = GetLastError();
		return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
	}
	if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
		return false;

	HANDLE file = CreateFileW(
		reinterpret_cast<LPCWSTR>(path.utf16()),
		DELETE | FILE_READ_ATTRIBUTES,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		NULL,
		OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL,
		NULL);
	if (file == INVALID_HANDLE_VALUE)
		return false;
	CloseHandle(file);
	return true;
}

static bool removeExistingFileChecked(const QString& path)
{
	if (path.isEmpty())
		return true;
	if (QFile::remove(path))
		return true;

	// QFile::exists() also returns false when the existence probe itself fails
	// (for example because access is denied). Only a verified Win32 "not found"
	// result is safe to treat as successful cleanup.
	const DWORD attributes = GetFileAttributesW(
		reinterpret_cast<LPCWSTR>(path.utf16()));
	if (attributes != INVALID_FILE_ATTRIBUTES)
		return false;
	const DWORD error = GetLastError();
	return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
}

VSTPluginFilterGUI::~VSTPluginFilterGUI()
{
	if (!outProcRuntimeStateTransferred)
		closeOutProcPanel();
	releasePluginInstance();

	delete ui;
}

void VSTPluginFilterGUI::store(QString& command, QString& parameters)
{
	storeWithMidiConfig(command, parameters, midiConfig);
}

void VSTPluginFilterGUI::storeWithMidiConfig(
	QString& command,
	QString& parameters,
	const std::wstring& serializedMidiConfig) const
{
	command = outProcMode ? "OutProcVSTPlugin" : "VSTPlugin";

	QString absolutePath = QString::fromStdWString(library->getLibPath());
	QDir pluginsDir(QString::fromStdWString(VSTPluginLibrary::getDefaultPluginPath()));
	QString relativePath = QDir::toNativeSeparators(pluginsDir.relativeFilePath(absolutePath));
	if (relativePath.startsWith(QDir::toNativeSeparators("../../")))
		relativePath = absolutePath;

	if (relativePath.contains(" "))
		relativePath = "\"" + relativePath + "\"";
	parameters = "Library " + relativePath;
	if (library->isVST3() && vst3ClassIndex != 0)
		parameters += " ClassIndex " + QString::number(vst3ClassIndex);
	if (outProcMode)
		parameters += " HostId " + hostId;
	if (!serializedMidiConfig.empty())
		parameters += " MidiConfig \"" + QString::fromStdWString(serializedMidiConfig) + "\"";
	if (chunkData != L"")
	{
		parameters += " ChunkData \"" + QString::fromStdWString(chunkData) + "\"";
	}
	else
	{
		for (auto it : paramMap)
		{
			QString name = QString::fromStdWString(it.first);
			if (name.contains(" ") || name.contains("\""))
				name = "\"" + name.replace("\"", "\"\"") + "\"";
			parameters += " " + name + " " + QString("%1").arg(it.second);
		}
	}
}

void VSTPluginFilterGUI::loadPreferences(const QVariantMap& prefs)
{
	autoApplyDialog = prefs.value("autoApplyDialog").toBool();
#ifdef EQAPO_ENABLE_UI_SNAPSHOTS
	// Snapshot fixtures are deliberately synthetic. Never let a coincidental
	// file in the working directory turn a visual regression capture into
	// plug-in execution.
	if (UiSnapshot::requested())
		return;
#endif
	if (outProcMode)
	{
		ui->statusLabel->setText(tr("Out-of-process VST host"));
		ui->statusLabel->setProperty("statusLevel", "info");
		ui->statusLabel->style()->unpolish(ui->statusLabel);
		ui->statusLabel->style()->polish(ui->statusLabel);
	}
	else
		initPlugin();
}

void VSTPluginFilterGUI::storePreferences(QVariantMap& prefs)
{
	prefs.insert("autoApplyDialog", autoApplyDialog);
}

void VSTPluginFilterGUI::restoreRuntimeState(const QVariantMap& state)
{
	if (!outProcMode ||
		state.value("outProcHostId").toString() != hostId)
	{
		return;
	}

	bool pidOk = false;
	bool creationTimeOk = false;
	const qint64 restoredPid = state.value("outProcPid").toString().toLongLong(&pidOk);
	const qulonglong restoredCreationTime = state.value(
		"outProcProcessCreationTime").toString().toULongLong(&creationTimeOk);
	if (!pidOk || restoredPid < 0 ||
		static_cast<quint64>(restoredPid) > MAXDWORD ||
		!creationTimeOk)
	{
		appendOutProcDebugLog("invalid transferred runtime state ignored hostId=" + hostId);
		return;
	}

	outProcGuiConfigPath = state.value("outProcConfigPath").toString();
	outProcGuiPid = restoredPid;
	outProcGuiProcessCreationTime =
		static_cast<std::uint64_t>(restoredCreationTime);
	outProcGuiRunning = state.value("outProcRunning").toBool();
	outProcGuiHidden = state.value("outProcHidden").toBool();
	outProcFinalStateCommitted = state.value(
		"outProcFinalStateCommitted").toBool();
	outProcRuntimeStateTransferred = false;
	if (outProcGuiRunning)
	{
		ui->openPanelButton->setText(outProcGuiHidden
			? tr("Show panel") : tr("Hide panel"));
		ui->statusLabel->setText(tr("Out-of-process VST panel is open"));
		idleTimer.start();
	}
	else if (!outProcGuiConfigPath.isEmpty())
	{
		ui->openPanelButton->setText(tr("Open panel"));
		ui->statusLabel->setText(tr("Out-of-process VST state recovery is pending"));
		ui->statusLabel->setProperty("statusLevel", "warning");
	}
}

void VSTPluginFilterGUI::takeRuntimeState(QVariantMap& state)
{
	if (!outProcMode)
		return;

	// Runtime ownership is transferred before the old widget is queued for
	// deferred deletion. Stop its poller immediately so only the replacement
	// GUI can read or update the shared sidecar from this point on.
	idleTimer.stop();
	state.insert("outProcHostId", hostId);
	state.insert("outProcConfigPath", outProcGuiConfigPath);
	state.insert("outProcPid", QString::number(outProcGuiPid));
	state.insert("outProcProcessCreationTime",
		QString::number(outProcGuiProcessCreationTime));
	state.insert("outProcRunning", outProcGuiRunning);
	state.insert("outProcHidden", outProcGuiHidden);
	state.insert("outProcFinalStateCommitted", outProcFinalStateCommitted);
	// The replacement GUI now owns the external session. Do not let this
	// soon-to-be-deleted widget hide that session from its successor.
	outProcRuntimeStateTransferred = true;
}

bool VSTPluginFilterGUI::prepareDelete()
{
	if (!outProcMode)
		return true;
	// Do not stop the host, read its mutable state, or remove either tracking
	// file here. Callers may still cancel a close or drag after this preflight.
	return canRemoveExistingFile(outProcGuiConfigPath) &&
		canRemoveExistingFile(makeOutProcPidPath(hostId));
}

bool VSTPluginFilterGUI::commitDelete()
{
	return !outProcMode || ensureOutProcPanelStopped(true);
}

void VSTPluginFilterGUI::on_openPanelButton_clicked()
{
	if (outProcMode)
	{
		openOutProcPanel();
		return;
	}

	initPlugin();

	if (effect != NULL)
	{
		effect->writeToEffect(chunkData, paramMap);

		VSTPluginFilterGUIDialog dialog(this, effect, autoApplyDialog);
		connect(dialog.getApplyButton(), SIGNAL(pressed()), SLOT(applyDialog()));
		connect(dialog.getAutoApplyCheckBox(), SIGNAL(toggled(bool)), SLOT(autoApplyToggled(bool)));
		lastReadTimer.invalidate();
		idleTimer.start();

		if (dialog.exec() == QDialog::Accepted)
		{
			effect->readFromEffect(chunkData, paramMap);
			updateModel();
			updatePermissionWarning();
		}
		idleTimer.stop();
	}
}

void VSTPluginFilterGUI::on_reloadButton_clicked()
{
	if (library->getLibPath().empty())
		return;

	if (outProcMode)
	{
		appendOutProcDebugLog("reload requested hostId=" + hostId);
		if (!ensureOutProcPanelStopped(false))
			return;
		hostId = QUuid::createUuid().toString(QUuid::WithoutBraces);
		ui->openPanelButton->setText(tr("Open panel"));
		ui->statusLabel->setText(tr("Out-of-process VST reload requested"));
	}
	else
	{
		ui->statusLabel->setText(tr("VST plugin reload requested"));
	}

	// Rewriting the row makes the APO audio engine reconstruct its VST instance.
	emit updateModel();
}

void VSTPluginFilterGUI::on_midiButton_clicked()
{
	if (!outProcMode)
		initPlugin();
	if (outProcMode && !midiConfig.empty())
	{
		if (!ensureOutProcPanelStopped(true))
			return;
	}
	const std::vector<VSTParameterDescriptor> parameters =
		availableMidiParameters();
	if (parameters.empty())
	{
		QMessageBox::information(
			this,
			tr("VST MIDI control"),
			outProcMode
				? tr("Open the out-of-process panel once and close it after the plug-in state has been captured. MIDI mapping then has stable parameter IDs to target.")
				: tr("This plug-in did not expose any writable parameters for MIDI control."));
		return;
	}

	MainWindow* mainWindow = NULL;
	const bool releaseRuntimeMidi = !midiConfig.empty();
	if (releaseRuntimeMidi)
	{
		mainWindow = qobject_cast<MainWindow*>(window());
		QString temporaryCommand;
		QString temporaryParameters;
		storeWithMidiConfig(
			temporaryCommand, temporaryParameters, std::wstring());
		if (mainWindow == NULL ||
			!mainWindow->beginTemporaryFilterConfiguration(
				this,
				temporaryCommand,
				temporaryParameters,
				QStringLiteral("vst-midi-learn")))
		{
			QMessageBox::warning(
				this,
				tr("VST MIDI control"),
				tr("Save the current profile and resolve any temporary audio state before configuring MIDI."));
			return;
		}
	}

	int dialogResult = QDialog::Rejected;
	std::wstring updatedConfiguration = midiConfig;
	{
		// Destroy the learner (and close its WinMM handle) before restoring the
		// runtime row, otherwise single-client MIDI drivers can remain busy.
		VSTMidiMappingDialog dialog(parameters, midiConfig, this);
		dialogResult = dialog.exec();
		if (dialogResult == QDialog::Accepted)
			updatedConfiguration = dialog.getEncodedConfiguration();
	}

	if (releaseRuntimeMidi)
	{
		bool keptExternal = false;
		if (!mainWindow->restoreTemporaryFilterConfiguration(&keptExternal))
		{
			QMessageBox::critical(
				this,
				tr("Audio processing was not restored"),
				tr("The temporary MIDI-learning state could not be restored automatically. Close the editor and use temporary audio recovery before continuing."));
			return;
		}
		if (keptExternal)
			return;
	}

	if (dialogResult != QDialog::Accepted)
		return;
	if (updatedConfiguration == midiConfig)
		return;
	midiConfig = updatedConfiguration;
	updateMidiButton();
	emit updateModel();
}

void VSTPluginFilterGUI::on_vst3ClassComboBox_currentIndexChanged(int index)
{
	if (index < 0 || index == vst3ClassIndex)
		return;

	if (!chunkData.empty() || !paramMap.empty() || !midiConfig.empty())
	{
		const QMessageBox::StandardButton answer = QMessageBox::question(
			this,
			tr("Change VST3 plug-in class?"),
			tr("Changing the class clears the saved plug-in state and any parameter-specific controls for this row."),
			QMessageBox::Yes | QMessageBox::Cancel,
			QMessageBox::Cancel);
		if (answer != QMessageBox::Yes)
		{
			QSignalBlocker blocker(ui->vst3ClassComboBox);
			ui->vst3ClassComboBox->setCurrentIndex(vst3ClassIndex);
			return;
		}
	}

	if (outProcMode)
	{
		if (!ensureOutProcPanelStopped(false))
		{
			QSignalBlocker blocker(ui->vst3ClassComboBox);
			ui->vst3ClassComboBox->setCurrentIndex(vst3ClassIndex);
			return;
		}
		hostId = QUuid::createUuid().toString(QUuid::WithoutBraces);
	}
	else
		releasePluginInstance();

	vst3ClassIndex = index;
	chunkData = L"";
	paramMap.clear();
	midiConfig.clear();
	outProcParameterDescriptors.clear();
	updateMidiButton();
	if (!outProcMode)
		initPlugin();
	emit updateModel();
	updatePermissionWarning();
}

void VSTPluginFilterGUI::openOutProcPanel()
{
	if (outProcGuiRunning)
	{
		if (outProcGuiHidden)
		{
			if (!signalOutProcPanel(L"GuiShow"))
			{
				if (!ensureOutProcPanelStopped(true))
					return;
			}
			else
			{
				outProcGuiHidden = false;
				ui->openPanelButton->setText(tr("Hide panel"));
				return;
			}
		}
		else
		{
			if (!signalOutProcPanel(L"GuiHide"))
			{
				if (!ensureOutProcPanelStopped(true))
					return;
			}
			else
			{
				outProcGuiHidden = true;
				ui->openPanelButton->setText(tr("Show panel"));
				return;
			}
		}
	}

	// A stopped host with no final-write acknowledgement leaves its sidecar
	// attached to this row. Do not reconnect to some other same-HostId process
	// while that recovery state is pending.
	const bool recoveringPreservedState = !outProcGuiConfigPath.isEmpty();
	if (!recoveringPreservedState && signalOutProcPanel(L"GuiShow"))
	{
		outProcGuiRunning = true;
		outProcGuiHidden = false;
		ui->openPanelButton->setText(tr("Hide panel"));
		ui->statusLabel->setText(tr("Out-of-process VST panel is open"));
		idleTimer.start();
		return;
	}

	if (library->getLibPath() == L"")
		return;

	const QString hostExe = "EqApoOutProcHost.exe";
	QString hostPath = QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(hostExe);
	if (!QFile::exists(hostPath))
	{
		QMessageBox::warning(this, tr("VST plugin"), tr("%1 was not found next to Editor.exe.").arg(hostExe));
		return;
	}

	bool launchPreservedState = recoveringPreservedState;
	if (launchPreservedState)
	{
		OutProcVSTConfig preservedConfig;
		if (!OutProcReadVSTConfig(
			outProcGuiConfigPath.toStdWString(), preservedConfig))
		{
			const QMessageBox::StandardButton answer = QMessageBox::question(
				this,
				tr("VST plugin"),
				tr("The preserved temporary VST state cannot be read. Discard it and reopen the panel from the state currently stored in this row?"),
				QMessageBox::Yes | QMessageBox::Cancel,
				QMessageBox::Cancel);
			if (answer != QMessageBox::Yes)
				return;
			if (!removeExistingFileChecked(outProcGuiConfigPath))
			{
				QMessageBox::warning(
					this,
					tr("VST plugin"),
					tr("The unreadable temporary VST state could not be removed. It was left unchanged."));
				return;
			}
			outProcGuiConfigPath.clear();
			launchPreservedState = false;
		}
	}

	if (!launchPreservedState)
	{
		outProcGuiConfigPath = QDir::temp().absoluteFilePath("EqApoVSTGui-" + QUuid::createUuid().toString(QUuid::WithoutBraces) + ".opvs");
		OutProcVSTConfig config;
		config.libraryPath = library->getLibPath();
		config.vst3ClassIndex = vst3ClassIndex;
		config.chunkData = chunkData;
		config.paramMap = paramMap;
		config.midiConfig = midiConfig;
		config.parameterDescriptors =
			convertOutProcParameterDescriptors(outProcParameterDescriptors);
		if (!OutProcWriteVSTConfig(outProcGuiConfigPath.toStdWString(), config))
		{
			QMessageBox::warning(this, tr("VST plugin"), tr("Could not create temporary VST host configuration."));
			if (removeExistingFileChecked(outProcGuiConfigPath))
			{
				outProcGuiConfigPath.clear();
			}
			return;
		}
	}

	QStringList arguments;
	arguments << "--gui" << "--session" << hostId << "--vst-config" << outProcGuiConfigPath;
	// The APO starts a headless host during cold configuration load. Ask that
	// host to release the per-session lease before the interactive host starts.
	signalOutProcPanel(L"HostHandoff");
	appendOutProcDebugLog("open panel hostId=" + hostId + " config=" + outProcGuiConfigPath);

	qint64 pid = 0;
	if (!QProcess::startDetached(hostPath, arguments, QCoreApplication::applicationDirPath(), &pid))
	{
		QMessageBox::warning(this, tr("VST plugin"), tr("Could not start the out-of-process VST host."));
		if (!launchPreservedState)
		{
			if (removeExistingFileChecked(outProcGuiConfigPath))
			{
				outProcGuiConfigPath.clear();
			}
			else
			{
				QMessageBox::warning(
					this, tr("VST plugin"),
					tr("The temporary VST state could not be removed. It was left unchanged."));
			}
		}
		return;
	}

	outProcGuiRunning = true;
	outProcGuiPid = pid;
	outProcGuiProcessCreationTime = pid > 0 && static_cast<quint64>(pid) <= MAXDWORD
		? getOutProcProcessCreationTime(static_cast<DWORD>(pid)) : 0;
	outProcGuiHidden = false;
	outProcFinalStateCommitted = false;
	ui->openPanelButton->setText(tr("Hide panel"));
	ui->statusLabel->setText(tr("Out-of-process VST panel is open"));
	idleTimer.start();
}

bool VSTPluginFilterGUI::signalOutProcPanel(
	const wchar_t* suffix,
	std::uint32_t* error)
{
	if (error != nullptr)
		*error = ERROR_SUCCESS;
	QString objectName = makeOutProcObjectName(hostId, suffix);
	HANDLE eventHandle = OpenEventW(EVENT_MODIFY_STATE, FALSE, reinterpret_cast<LPCWSTR>(objectName.utf16()));
	if (eventHandle == NULL)
	{
		const DWORD openError = GetLastError();
		if (error != nullptr)
			*error = openError;
		appendOutProcDebugLog("signal " + QString::fromWCharArray(suffix) + " hostId=" + hostId + " open failed gle=" + QString::number(openError));
		return false;
	}
	const BOOL ok = SetEvent(eventHandle);
	const DWORD signalError = ok ? ERROR_SUCCESS : GetLastError();
	CloseHandle(eventHandle);
	if (error != nullptr)
		*error = signalError;
	appendOutProcDebugLog("signal " + QString::fromWCharArray(suffix) + " hostId=" + hostId + " ok=" + QString(ok ? "true" : "false") + " gle=" + QString::number(signalError));
	return ok == TRUE;
}

static bool outProcPanelEventExists(
	const QString& hostId,
	const wchar_t* suffix,
	std::uint32_t* error)
{
	if (error != nullptr)
		*error = ERROR_SUCCESS;
	const QString objectName = makeOutProcObjectName(hostId, suffix);
	HANDLE eventHandle = OpenEventW(
		SYNCHRONIZE, FALSE, reinterpret_cast<LPCWSTR>(objectName.utf16()));
	if (eventHandle == NULL)
	{
		if (error != nullptr)
			*error = GetLastError();
		return false;
	}
	CloseHandle(eventHandle);
	return true;
}

bool VSTPluginFilterGUI::consumeOutProcPanelSignal(const wchar_t* suffix)
{
	QString objectName = makeOutProcObjectName(hostId, suffix);
	HANDLE eventHandle = OpenEventW(SYNCHRONIZE, FALSE, reinterpret_cast<LPCWSTR>(objectName.utf16()));
	if (eventHandle == NULL)
		return false;
	const DWORD waitResult = WaitForSingleObject(eventHandle, 0);
	CloseHandle(eventHandle);
	return waitResult == WAIT_OBJECT_0;
}

struct OutProcProcessIdentity
{
	DWORD processId = 0;
	std::uint64_t processCreationTime = 0;
};

static constexpr DWORD outProcGuiExitGraceMs = 2000;
static constexpr DWORD outProcGuiForceExitWaitMs = 1000;

static std::uint64_t queryOutProcProcessCreationTime(HANDLE process)
{
	FILETIME creationTime = {};
	FILETIME exitTime = {};
	FILETIME kernelTime = {};
	FILETIME userTime = {};
	if (!GetProcessTimes(process, &creationTime, &exitTime, &kernelTime, &userTime))
		return 0;

	return (static_cast<std::uint64_t>(creationTime.dwHighDateTime) << 32) |
		creationTime.dwLowDateTime;
}

static std::uint64_t getOutProcProcessCreationTime(DWORD processId)
{
	HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
	if (process == NULL)
		return 0;

	const std::uint64_t creationTime = queryOutProcProcessCreationTime(process);
	CloseHandle(process);
	return creationTime;
}

static QString makeOutProcPidPath(const QString& hostId)
{
	const QString safeId = makeSafeOutProcSessionId(hostId);
	return QDir::temp().absoluteFilePath("EqApoOutProcHost-" + safeId + ".pid");
}

static OutProcProcessIdentity readOutProcProcessIdentityMapping(
	const QString& hostId,
	bool& lookupFailed)
{
	lookupFailed = false;
	OutProcProcessIdentity identity;
	const QString objectName = makeOutProcObjectName(hostId, L"GuiInfo");
	HANDLE mapping = OpenFileMappingW(
		FILE_MAP_READ, FALSE, reinterpret_cast<LPCWSTR>(objectName.utf16()));
	if (mapping == NULL)
	{
		const DWORD error = GetLastError();
		lookupFailed = error != ERROR_FILE_NOT_FOUND;
		appendOutProcDebugLog("pid mapping open failed hostId=" + hostId +
			" gle=" + QString::number(error));
		return identity;
	}

	const SIZE_T prefixSize = offsetof(OutProcGuiInfo, processCreationTime);
	const OutProcGuiInfo* prefix = static_cast<const OutProcGuiInfo*>(
		MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, prefixSize));
	if (prefix != nullptr)
	{
		const std::uint32_t magic = prefix->magic;
		const std::uint32_t version = prefix->version;
		const DWORD processId = prefix->processId;
		UnmapViewOfFile(prefix);

		if (magic == OUTPROC_GUI_INFO_MAGIC && version == 1)
		{
			// Version 1 did not carry a creation-time token. It is safe to wait
			// on its live mapping PID, but never to force-terminate it.
			identity.processId = processId;
		}
		else if (magic == OUTPROC_GUI_INFO_MAGIC && version == OUTPROC_GUI_INFO_VERSION)
		{
			const OutProcGuiInfo* info = static_cast<const OutProcGuiInfo*>(
				MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(OutProcGuiInfo)));
			if (info != nullptr)
			{
				if (info->magic == OUTPROC_GUI_INFO_MAGIC &&
					info->version == OUTPROC_GUI_INFO_VERSION &&
					info->processId == processId)
				{
					identity.processId = processId;
					identity.processCreationTime = info->processCreationTime;
				}
				else
					lookupFailed = true;
				UnmapViewOfFile(info);
			}
			else
				lookupFailed = true;
		}
		else
			lookupFailed = true;
	}
	else
	{
		lookupFailed = true;
		appendOutProcDebugLog("pid mapping view failed hostId=" + hostId +
			" gle=" + QString::number(GetLastError()));
	}
	CloseHandle(mapping);

	if (identity.processId == 0 || identity.processId == GetCurrentProcessId())
	{
		lookupFailed = true;
		appendOutProcDebugLog("pid mapping invalid hostId=" + hostId +
			" pid=" + QString::number(identity.processId));
		return OutProcProcessIdentity();
	}
	return identity;
}

static OutProcProcessIdentity readOutProcProcessIdentityPidFile(
	const QString& hostId,
	bool& lookupFailed)
{
	lookupFailed = false;
	OutProcProcessIdentity identity;
	const QString path = makeOutProcPidPath(hostId);
	QFile file(path);
	if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
	{
		// Absence is the only benign failure: an existing but unreadable
		// identity file means a host may still be alive and must fail closed.
		const DWORD attributes = GetFileAttributesW(
			reinterpret_cast<LPCWSTR>(path.utf16()));
		const DWORD attributeError = attributes == INVALID_FILE_ATTRIBUTES
			? GetLastError() : ERROR_SUCCESS;
		lookupFailed = attributes != INVALID_FILE_ATTRIBUTES ||
			(attributeError != ERROR_FILE_NOT_FOUND &&
				attributeError != ERROR_PATH_NOT_FOUND);
		if (lookupFailed)
			appendOutProcDebugLog("pid file could not be opened hostId=" + hostId +
				" path=" + path);
		return identity;
	}
	if (file.size() <= 0 || file.size() > 128)
	{
		lookupFailed = true;
		appendOutProcDebugLog("pid file size invalid hostId=" + hostId +
			" path=" + path);
		return identity;
	}

	const QByteArray contents = file.readAll();
	if (file.error() != QFileDevice::NoError)
	{
		lookupFailed = true;
		appendOutProcDebugLog("pid file read failed hostId=" + hostId +
			" path=" + path);
		return identity;
	}
	const QStringList tokens = QString::fromLatin1(contents)
		.simplified().split(' ', Qt::SkipEmptyParts);
	if (tokens.size() != 2)
	{
		lookupFailed = true;
		appendOutProcDebugLog("unverified legacy or malformed pid file ignored hostId=" +
			hostId + " path=" + path);
		return identity;
	}

	bool processIdOk = false;
	bool creationTimeOk = false;
	const qulonglong processId = tokens[0].toULongLong(&processIdOk);
	const qulonglong creationTime = tokens[1].toULongLong(&creationTimeOk);
	if (!processIdOk || !creationTimeOk || processId == 0 ||
		processId > MAXDWORD || creationTime == 0 ||
		static_cast<DWORD>(processId) == GetCurrentProcessId())
	{
		lookupFailed = true;
		appendOutProcDebugLog("pid file identity invalid hostId=" + hostId +
			" path=" + path);
		return identity;
	}

	identity.processId = static_cast<DWORD>(processId);
	identity.processCreationTime = static_cast<std::uint64_t>(creationTime);
	return identity;
}

static QString canonicalExecutablePath(const QString& path)
{
	QFileInfo info(path);
	QString canonical = info.canonicalFilePath();
	if (canonical.isEmpty())
		canonical = info.absoluteFilePath();
	return QDir::cleanPath(canonical);
}

static bool isExpectedOutProcHostProcess(
	HANDLE process,
	bool* queryFailed = nullptr)
{
	if (queryFailed != nullptr)
		*queryFailed = false;
	wchar_t imagePath[32768] = {};
	DWORD imagePathLength = static_cast<DWORD>(sizeof(imagePath) / sizeof(imagePath[0]));
	if (!QueryFullProcessImageNameW(process, 0, imagePath, &imagePathLength))
	{
		if (queryFailed != nullptr)
			*queryFailed = true;
		return false;
	}

	const QString actualPath = canonicalExecutablePath(
		QString::fromWCharArray(imagePath, static_cast<int>(imagePathLength)));
	const QString expectedPath = canonicalExecutablePath(
		QDir(QCoreApplication::applicationDirPath()).absoluteFilePath("EqApoOutProcHost.exe"));
	return QString::compare(actualPath, expectedPath, Qt::CaseInsensitive) == 0;
}

enum class OutProcProcessIdentityStatus
{
	Live,
	Stale,
	Unverifiable
};

static OutProcProcessIdentityStatus probeOutProcProcessIdentity(
	const OutProcProcessIdentity& identity,
	const QString& hostId,
	const char* source)
{
	if (identity.processId == 0 ||
		identity.processId == GetCurrentProcessId() ||
		identity.processCreationTime == 0)
	{
		appendOutProcDebugLog(QString::fromLatin1(source) +
			" identity is incomplete hostId=" + hostId);
		return OutProcProcessIdentityStatus::Unverifiable;
	}

	HANDLE process = OpenProcess(
		SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
		FALSE,
		identity.processId);
	if (process == NULL)
	{
		const DWORD error = GetLastError();
		appendOutProcDebugLog(QString::fromLatin1(source) +
			" identity process open result hostId=" + hostId +
			" pid=" + QString::number(identity.processId) +
			" gle=" + QString::number(error));
		return error == ERROR_INVALID_PARAMETER
			? OutProcProcessIdentityStatus::Stale
			: OutProcProcessIdentityStatus::Unverifiable;
	}

	const DWORD waitResult = WaitForSingleObject(process, 0);
	if (waitResult == WAIT_OBJECT_0)
	{
		CloseHandle(process);
		return OutProcProcessIdentityStatus::Stale;
	}
	if (waitResult != WAIT_TIMEOUT)
	{
		CloseHandle(process);
		return OutProcProcessIdentityStatus::Unverifiable;
	}

	const std::uint64_t actualCreationTime =
		queryOutProcProcessCreationTime(process);
	if (actualCreationTime == 0)
	{
		CloseHandle(process);
		return OutProcProcessIdentityStatus::Unverifiable;
	}
	if (actualCreationTime != identity.processCreationTime)
	{
		CloseHandle(process);
		return OutProcProcessIdentityStatus::Stale;
	}

	bool imageQueryFailed = false;
	const bool expectedImage = isExpectedOutProcHostProcess(
		process, &imageQueryFailed);
	CloseHandle(process);
	if (imageQueryFailed)
		return OutProcProcessIdentityStatus::Unverifiable;
	return expectedImage
		? OutProcProcessIdentityStatus::Live
		: OutProcProcessIdentityStatus::Stale;
}

static OutProcProcessIdentity readLiveOutProcProcessIdentityPidFile(
	const QString& hostId,
	bool& lookupFailed)
{
	OutProcProcessIdentity identity = readOutProcProcessIdentityPidFile(
		hostId, lookupFailed);
	if (lookupFailed || identity.processId == 0)
		return identity;

	const OutProcProcessIdentityStatus status = probeOutProcProcessIdentity(
		identity, hostId, "pid file");
	if (status == OutProcProcessIdentityStatus::Unverifiable)
	{
		lookupFailed = true;
		appendOutProcDebugLog("pid file live identity unverifiable hostId=" + hostId);
		return OutProcProcessIdentity();
	}
	if (status == OutProcProcessIdentityStatus::Stale)
	{
		appendOutProcDebugLog(
			"stale pid file ignored; reused or exited pid file retained for commit cleanup hostId=" +
			hostId);
		return OutProcProcessIdentity();
	}
	return identity;
}

static bool stopOutProcProcess(
	const OutProcProcessIdentity& identity,
	const QString& hostId,
	bool& finalStateCommitted)
{
	finalStateCommitted = false;
	if (identity.processId == 0 || identity.processId == GetCurrentProcessId())
		return false;

	const bool forceTerminationAllowed = identity.processCreationTime != 0;
	DWORD access = SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION;
	if (forceTerminationAllowed)
		access |= PROCESS_TERMINATE;
	HANDLE process = OpenProcess(access, FALSE, identity.processId);
	if (process == NULL)
	{
		const DWORD error = GetLastError();
		appendOutProcDebugLog("host process open failed hostId=" + hostId +
			" pid=" + QString::number(identity.processId) +
			" gle=" + QString::number(error));
		return error == ERROR_INVALID_PARAMETER;
	}

	if (forceTerminationAllowed &&
		(queryOutProcProcessCreationTime(process) != identity.processCreationTime ||
			!isExpectedOutProcHostProcess(process)))
	{
		appendOutProcDebugLog("host process identity mismatch; refusing termination hostId=" +
			hostId + " pid=" + QString::number(identity.processId));
		CloseHandle(process);
		return false;
	}

	bool forcedTermination = false;
	DWORD waitResult = WaitForSingleObject(process, outProcGuiExitGraceMs);
	if (waitResult == WAIT_TIMEOUT && forceTerminationAllowed)
	{
		appendOutProcDebugLog("host graceful exit timed out; forcing termination hostId=" +
			hostId + " pid=" + QString::number(identity.processId));
		if (TerminateProcess(process, 23))
		{
			forcedTermination = true;
			waitResult = WaitForSingleObject(process, outProcGuiForceExitWaitMs);
		}
		else if (WaitForSingleObject(process, 0) == WAIT_OBJECT_0)
			waitResult = WAIT_OBJECT_0;
	}
	else if (waitResult == WAIT_TIMEOUT)
	{
		appendOutProcDebugLog("legacy host did not exit during grace period; "
			"refusing unverified force termination hostId=" + hostId +
			" pid=" + QString::number(identity.processId));
	}
	const bool stopped = waitResult == WAIT_OBJECT_0;
	if (stopped && !forcedTermination)
	{
		DWORD processExitCode = STILL_ACTIVE;
		const BOOL exitCodeRead = GetExitCodeProcess(process, &processExitCode);
		const DWORD exitCodeReadError = exitCodeRead
			? ERROR_SUCCESS
			: GetLastError();
		if (exitCodeRead &&
			processExitCode == ERROR_SUCCESS)
		{
			finalStateCommitted = true;
		}
		else
		{
			appendOutProcDebugLog("host final state was not acknowledged hostId=" +
				hostId + " exitCode=" + QString::number(processExitCode) +
				" gle=" + QString::number(exitCodeReadError));
		}
	}
	CloseHandle(process);
	return stopped;
}

static OutProcProcessIdentity resolveOutProcProcessIdentity(
	const QString& hostId,
	qint64 launchedProcessId,
	std::uint64_t launchedProcessCreationTime,
	bool& lookupFailed,
	bool& launchedIdentityStale)
{
	lookupFailed = false;
	launchedIdentityStale = false;
	OutProcProcessIdentity launched;
	if (launchedProcessId > 0 && static_cast<quint64>(launchedProcessId) <= MAXDWORD)
	{
		launched.processId = static_cast<DWORD>(launchedProcessId);
		launched.processCreationTime = launchedProcessCreationTime;
	}

	bool mappingLookupFailed = false;
	OutProcProcessIdentity mapped = readOutProcProcessIdentityMapping(
		hostId, mappingLookupFailed);
	if (mappingLookupFailed)
	{
		lookupFailed = true;
		return OutProcProcessIdentity();
	}

	auto identitiesConflict = [](const OutProcProcessIdentity& first,
		const OutProcProcessIdentity& second)
	{
		return first.processId != 0 && second.processId != 0 &&
			(first.processId != second.processId ||
				(first.processCreationTime != 0 &&
					second.processCreationTime != 0 &&
					first.processCreationTime != second.processCreationTime));
	};
	auto failIdentityConflict = [&]()
	{
		lookupFailed = true;
		appendOutProcDebugLog("conflicting host identities; refusing termination hostId=" +
			hostId + " launchedPid=" + QString::number(launched.processId) +
			" mappedPid=" + QString::number(mapped.processId));
		return OutProcProcessIdentity();
	};

	// A PID and creation token recorded from this QProcess launch are the
	// authoritative identity. A named mapping may confirm them, but must never
	// redirect termination to another same-session host.
	if (launched.processId != 0)
	{
		if (launched.processCreationTime != 0)
		{
			const OutProcProcessIdentityStatus launchedStatus =
				probeOutProcProcessIdentity(launched, hostId, "launched");
			if (launchedStatus == OutProcProcessIdentityStatus::Unverifiable)
			{
				lookupFailed = true;
				appendOutProcDebugLog(
					"launched host identity is unverifiable; refusing termination hostId=" +
					hostId);
				return OutProcProcessIdentity();
			}
			if (launchedStatus == OutProcProcessIdentityStatus::Stale)
			{
				launchedIdentityStale = true;
				// A different named mapping must not redirect a stale launch
				// record to some other process. Fail closed while such a mapping
				// exists; otherwise the caller can preserve the sidecar and clear
				// the obsolete in-memory PID.
				if (mapped.processId != 0)
					return failIdentityConflict();
				appendOutProcDebugLog(
					"launched host identity is stale hostId=" + hostId +
					" pid=" + QString::number(launched.processId));
				return OutProcProcessIdentity();
			}
		}
		if (identitiesConflict(launched, mapped))
			return failIdentityConflict();
		if (launched.processCreationTime != 0)
			return launched;
		if (mapped.processId == launched.processId &&
			mapped.processCreationTime != 0)
			return mapped;

		bool pidLookupFailed = false;
		const OutProcProcessIdentity pidFile =
			readLiveOutProcProcessIdentityPidFile(hostId, pidLookupFailed);
		if (pidLookupFailed)
		{
			lookupFailed = true;
			return OutProcProcessIdentity();
		}
		if (pidFile.processId == 0)
			return launched;
		if (identitiesConflict(launched, pidFile))
			return failIdentityConflict();
		return pidFile;
	}

	if (mapped.processId != 0)
	{
		if (mapped.processCreationTime != 0)
			return mapped;

		bool pidLookupFailed = false;
		const OutProcProcessIdentity pidFile =
			readLiveOutProcProcessIdentityPidFile(hostId, pidLookupFailed);
		if (pidLookupFailed)
		{
			lookupFailed = true;
			return OutProcProcessIdentity();
		}
		if (pidFile.processId == 0)
			return mapped;
		if (identitiesConflict(mapped, pidFile))
			return failIdentityConflict();
		mapped.processCreationTime = pidFile.processCreationTime;
		return mapped;
	}

	bool pidLookupFailed = false;
	const OutProcProcessIdentity pidFile =
		readLiveOutProcProcessIdentityPidFile(hostId, pidLookupFailed);
	lookupFailed = pidLookupFailed;
	return pidFile;
}

void VSTPluginFilterGUI::closeOutProcPanel()
{
	if (!outProcGuiRunning)
		return;

	signalOutProcPanel(L"GuiHide");
	outProcGuiHidden = true;

	if (ui != nullptr)
	{
		ui->openPanelButton->setText(tr("Show panel"));
		ui->statusLabel->setText(tr("Out-of-process VST host"));
	}
}

bool VSTPluginFilterGUI::terminateOutProcPanel(
	bool* stateChanged,
	QString* failureMessage,
	bool* failureReported)
{
	if (stateChanged != nullptr)
		*stateChanged = false;
	if (failureMessage != nullptr)
		failureMessage->clear();
	if (failureReported != nullptr)
		*failureReported = false;
	idleTimer.stop();
	appendOutProcDebugLog("terminate panel hostId=" + hostId + " running=" + QString(outProcGuiRunning ? "true" : "false") + " pid=" + QString::number(outProcGuiPid));
	bool identityLookupFailed = false;
	bool launchedIdentityStale = false;
	const OutProcProcessIdentity processIdentity = resolveOutProcProcessIdentity(
		hostId, outProcGuiPid, outProcGuiProcessCreationTime,
		identityLookupFailed, launchedIdentityStale);
	std::uint32_t exitEventLookupError = ERROR_SUCCESS;
	const bool exitEventExists = outProcPanelEventExists(
		hostId, L"GuiExit", &exitEventLookupError);
	const bool exitEventLookupFailed = !exitEventExists &&
		exitEventLookupError != ERROR_FILE_NOT_FOUND;
	auto resolveUnconfirmedState = [&](const QString& canceledMessage)
	{
		OutProcVSTConfig snapshot;
		const bool snapshotReadable = !outProcGuiConfigPath.isEmpty() &&
			OutProcReadVSTConfig(outProcGuiConfigPath.toStdWString(), snapshot);
		QMessageBox recoveryBox(
			QMessageBox::Warning,
			tr("VST plugin"),
			tr("The out-of-process VST host stopped without confirming its final state. Its last readable snapshot may be incomplete. Choose whether to use that snapshot, discard the temporary state, or leave this row unchanged."),
			QMessageBox::NoButton,
			this);
		QPushButton* useSnapshotButton = snapshotReadable
			? recoveryBox.addButton(
				tr("Use last readable snapshot"), QMessageBox::AcceptRole)
			: nullptr;
		QPushButton* discardButton = recoveryBox.addButton(QMessageBox::Discard);
		QPushButton* cancelButton = recoveryBox.addButton(QMessageBox::Cancel);
		recoveryBox.setDefaultButton(cancelButton);
		recoveryBox.setEscapeButton(cancelButton);
		recoveryBox.exec();
		if (failureReported != nullptr)
			*failureReported = true;

		const bool useSnapshot = useSnapshotButton != nullptr &&
			recoveryBox.clickedButton() == useSnapshotButton;
		if (!useSnapshot && recoveryBox.clickedButton() != discardButton)
		{
			if (failureMessage != nullptr)
				*failureMessage = canceledMessage;
			return false;
		}

		const QString preservedPath = outProcGuiConfigPath;
		if (!removeExistingFileChecked(preservedPath))
		{
			appendOutProcDebugLog(
				"confirmed recovery choice could not remove sidecar hostId=" +
				hostId + " path=" + preservedPath);
			if (failureMessage != nullptr)
			{
				*failureMessage = tr("The temporary VST state could not be removed. It was left unchanged.");
			}
			QMessageBox::warning(
				this, tr("VST plugin"),
				failureMessage != nullptr ? *failureMessage :
					tr("The temporary VST state could not be removed. It was left unchanged."));
			return false;
		}

		outProcGuiConfigPath.clear();
		outProcFinalStateCommitted = false;
		bool recoveredStateChanged = false;
		if (useSnapshot)
		{
			recoveredStateChanged = chunkData != snapshot.chunkData ||
				paramMap != snapshot.paramMap;
			chunkData = snapshot.chunkData;
			paramMap = snapshot.paramMap;
			outProcParameterDescriptors = convertOutProcParameterDescriptors(
				snapshot.parameterDescriptors);
		}
		if (stateChanged != nullptr)
			*stateChanged = recoveredStateChanged;
		return true;
	};
	if (identityLookupFailed || exitEventLookupFailed ||
		(outProcGuiConfigPath.isEmpty() && outProcGuiPid <= 0 &&
			(processIdentity.processId != 0 || exitEventExists)))
	{
		appendOutProcDebugLog("host identity or state sidecar is not verifiable; preserving row hostId=" +
			hostId);
		if (failureMessage != nullptr)
		{
			*failureMessage = tr("The existing out-of-process VST host or its state file could not be verified. This row was left unchanged. Close the host and reload the profile before trying again.");
		}
		if (outProcGuiRunning)
			idleTimer.start();
		return false;
	}
	bool finalStateCommitted = outProcFinalStateCommitted;
	bool processStopped = launchedIdentityStale && !exitEventExists;
	if (!processStopped && processIdentity.processId == 0 &&
		!exitEventExists && !exitEventLookupFailed && outProcGuiPid <= 0)
	{
		processStopped = true;
	}
	if (!processStopped &&
		(processIdentity.processId != 0 || exitEventExists))
		signalOutProcPanel(L"GuiExit");
	if (!processStopped && processIdentity.processId != 0)
	{
		bool stopAcknowledged = false;
		processStopped = stopOutProcProcess(
			processIdentity, hostId, stopAcknowledged);
		finalStateCommitted = finalStateCommitted || stopAcknowledged;
	}
	if (!processStopped)
	{
		appendOutProcDebugLog("host termination not confirmed; preserving state hostId=" +
			hostId);
		if (failureMessage != nullptr)
		{
			*failureMessage = tr("The out-of-process VST host could not be stopped. This row was left unchanged. Close the host and try again.");
		}
		if (outProcGuiRunning)
			idleTimer.start();
		return false;
	}
	outProcFinalStateCommitted = finalStateCommitted;
	if (!outProcGuiConfigPath.isEmpty() && !finalStateCommitted)
	{
		appendOutProcDebugLog("host stopped without a final state acknowledgement; preserving sidecar hostId=" +
			hostId + " path=" + outProcGuiConfigPath);
		if (failureMessage != nullptr)
		{
			*failureMessage = tr("The out-of-process VST host stopped, but did not confirm that its final state was saved. This row and its temporary state file were left unchanged. Open the panel again to recover from the last readable snapshot.");
		}
		if (!markOutProcPanelStoppedPreservingState(failureMessage))
			return false;
		return resolveUnconfirmedState(
			failureMessage != nullptr ? *failureMessage : QString());
	}

	bool recoveredStateChanged = false;
	OutProcVSTConfig updatedConfig;
	bool hasUpdatedConfig = false;
	if (!outProcGuiConfigPath.isEmpty())
	{
		if (!OutProcReadVSTConfig(outProcGuiConfigPath.toStdWString(), updatedConfig))
		{
			appendOutProcDebugLog("host stopped but state recovery failed; preserving sidecar hostId=" +
				hostId + " path=" + outProcGuiConfigPath);
			if (failureMessage != nullptr)
			{
				*failureMessage = tr("The out-of-process VST host stopped, but its latest state could not be recovered. This row and its temporary state file were left unchanged. Try again before removing the row.");
			}
			if (!markOutProcPanelStoppedPreservingState(failureMessage))
				return false;
			return false;
		}
		recoveredStateChanged = chunkData != updatedConfig.chunkData ||
			paramMap != updatedConfig.paramMap;
		hasUpdatedConfig = true;
	}

	if (!markOutProcPanelStoppedPreservingState(failureMessage))
		return false;
	if (!removeExistingFileChecked(outProcGuiConfigPath))
	{
		appendOutProcDebugLog(
			"recovered state sidecar cleanup failed; retaining retry state hostId=" +
			hostId + " path=" + outProcGuiConfigPath);
		if (failureMessage != nullptr)
		{
			*failureMessage = tr("The temporary VST state could not be removed. It was left unchanged.");
		}
		return false;
	}
	if (hasUpdatedConfig)
	{
		chunkData = updatedConfig.chunkData;
		paramMap = updatedConfig.paramMap;
		outProcParameterDescriptors = convertOutProcParameterDescriptors(
			updatedConfig.parameterDescriptors);
	}
	outProcGuiConfigPath.clear();
	outProcFinalStateCommitted = false;
	if (stateChanged != nullptr)
		*stateChanged = recoveredStateChanged;
	return true;
}

bool VSTPluginFilterGUI::markOutProcPanelStoppedPreservingState(
	QString* failureMessage)
{
	idleTimer.stop();
	const QString pidPath = makeOutProcPidPath(hostId);
	const bool pidRemoved = removeExistingFileChecked(pidPath);
	if (!pidRemoved)
	{
		appendOutProcDebugLog(
			"stopped host pid file could not be removed; retaining retry state hostId=" +
			hostId + " path=" + pidPath);
		if (failureMessage != nullptr)
		{
			*failureMessage = tr("The temporary VST state could not be removed. It was left unchanged.");
		}
		return false;
	}
	outProcGuiRunning = false;
	outProcGuiPid = 0;
	outProcGuiProcessCreationTime = 0;
	outProcGuiHidden = false;
	ui->openPanelButton->setText(tr("Open panel"));
	return true;
}

bool VSTPluginFilterGUI::ensureOutProcPanelStopped(
	bool synchronizeRecoveredState)
{
	bool stateChanged = false;
	QString failureMessage;
	bool failureReported = false;
	if (!terminateOutProcPanel(
		&stateChanged, &failureMessage, &failureReported))
	{
		ui->statusLabel->setText(failureMessage);
		ui->statusLabel->setProperty("statusLevel", "danger");
		ui->statusLabel->style()->unpolish(ui->statusLabel);
		ui->statusLabel->style()->polish(ui->statusLabel);
		if (!failureReported)
			QMessageBox::warning(this, tr("VST plugin"), failureMessage);
		return false;
	}

	// The row can remain visible after a successful commit (for example, when
	// a later row vetoes a batch commit).
	// Reflect the stopped host instead of leaving stale "panel is open" UI.
	ui->openPanelButton->setText(tr("Open panel"));
	ui->statusLabel->setText(tr("Out-of-process VST host"));
	ui->statusLabel->setProperty("statusLevel", "info");
	ui->statusLabel->style()->unpolish(ui->statusLabel);
	ui->statusLabel->style()->polish(ui->statusLabel);
	if (synchronizeRecoveredState && stateChanged)
		emit updateModel();
	return true;
}

void VSTPluginFilterGUI::applyDialog()
{
	if (capturePluginStateIfChanged())
		updateModel();
	updatePermissionWarning();
}

void VSTPluginFilterGUI::autoApplyToggled(bool checked)
{
	autoApplyDialog = checked;
}

void VSTPluginFilterGUI::initPlugin()
{
	if (effect != NULL)
		return;

	QString text;
	const char* statusLevel = "danger";
	if (library->getLibPath() == L"")
	{
		text = tr("No file selected.");
	}
	else
	{
		int result = library->initialize();
		if (result < 0)
		{
			switch (result)
			{
			case AbstractLibrary::FILE_NOT_FOUND:
				text = tr("File not found.");
				break;
			case AbstractLibrary::LOADING_FAILED:
				text = tr("Library could not be loaded.");
				break;
			case AbstractLibrary::FUNCTIONS_MISSING:
				text = tr("Library does not contain needed functions.");
				break;
			case AbstractLibrary::WRONG_ARCHITECTURE:
#ifdef _WIN64
				int bitDepth = 64;
#else
				int bitDepth = 32;
#endif
				text = tr("Library has the wrong architecture. Only %1-bit libraries are supported.").arg(bitDepth);
				break;
			}
		}
		else
		{
			effect = new VSTPluginInstance(library, 1, vst3ClassIndex);
			if (effect->initialize())
			{
				effect->setLanguage(QLocale().language() == QLocale::German ? 2 : 1);
				effect->setAutomateFunc(bind(&VSTPluginFilterGUI::onAutomate, this));

				statusLevel = "normal";
				text = QString::fromStdWString(effect->getName());
			}
			else
			{
				delete effect;
				effect = NULL;

				text = tr("Plugin crashed during initialization.");
			}
		}
	}

	ui->statusLabel->setProperty("statusLevel", statusLevel);
	ui->statusLabel->style()->unpolish(ui->statusLabel);
	ui->statusLabel->style()->polish(ui->statusLabel);
	ui->statusLabel->setText(text);
}

void VSTPluginFilterGUI::releasePluginInstance()
{
	if (effect == NULL)
		return;

	idleTimer.stop();
	effect->setAutomateFunc(nullptr);
	effect->setParameterAutomateFunc(nullptr);
	effect->setSizeWindowFunc(nullptr);
	effect->stopEditing();

	VSTPluginInstance* instance = effect;
	effect = NULL;
	delete instance;
}

void VSTPluginFilterGUI::on_pathLineEdit_editingFinished()
{
	const QString requestedPath = canonicalVSTPath(ui->pathLineEdit->text());
	const QString currentPath = canonicalVSTPath(
		QString::fromStdWString(library->getLibPath()));
	if (QString::compare(requestedPath, currentPath, Qt::CaseInsensitive) != 0)
	{
		if (!currentPath.isEmpty() &&
			(!chunkData.empty() || !paramMap.empty() || !midiConfig.empty()) &&
			QMessageBox::question(
				this,
				tr("Change VST plug-in?"),
				tr("Changing the plug-in may clear its saved state and MIDI mappings. Continue?"),
				QMessageBox::Yes | QMessageBox::Cancel,
				QMessageBox::Cancel) != QMessageBox::Yes)
		{
			QDir pluginsDir(QString::fromStdWString(
				VSTPluginLibrary::getDefaultPluginPath()));
			QString displayPath = QDir::toNativeSeparators(
				pluginsDir.relativeFilePath(currentPath));
			if (displayPath.startsWith(QDir::toNativeSeparators("../../")))
				displayPath = currentPath;
			ui->pathLineEdit->setText(displayPath);
			return;
		}
		int oldId = 0;
		if (outProcMode)
		{
			if (!ensureOutProcPanelStopped(false))
			{
				QDir pluginsDir(QString::fromStdWString(
					VSTPluginLibrary::getDefaultPluginPath()));
				QString displayPath = QDir::toNativeSeparators(
					pluginsDir.relativeFilePath(currentPath));
				if (displayPath.startsWith(QDir::toNativeSeparators("../../")))
					displayPath = currentPath;
				ui->pathLineEdit->setText(displayPath);
				return;
			}
			hostId = QUuid::createUuid().toString(QUuid::WithoutBraces);
		}
		if (effect != NULL)
		{
			oldId = effect->uniqueID();
			releasePluginInstance();
		}

		library = VSTPluginLibrary::getInstance(requestedPath.toStdWString());
		vst3ClassIndex = 0;
		refreshVST3ClassComboBox();
		if (!outProcMode)
			initPlugin();

		if (outProcMode || effect == NULL || oldId == 0 || effect->uniqueID() != oldId)
		{
			chunkData = L"";
			paramMap.clear();
			midiConfig.clear();
			outProcParameterDescriptors.clear();
			updateMidiButton();
		}

		updateModel();
		updatePermissionWarning();
	}
}

bool VSTPluginFilterGUI::capturePluginStateIfChanged()
{
	if (effect == NULL)
		return false;

	std::wstring updatedChunkData;
	std::unordered_map<std::wstring, float> updatedParamMap;
	effect->readFromEffect(updatedChunkData, updatedParamMap);
	if (updatedChunkData == chunkData && updatedParamMap == paramMap)
		return false;

	chunkData = std::move(updatedChunkData);
	paramMap = std::move(updatedParamMap);
	return true;
}

std::vector<VSTParameterDescriptor> VSTPluginFilterGUI::availableMidiParameters() const
{
	auto eligibleParameters = [](const std::vector<VSTParameterDescriptor>& source) {
		std::vector<VSTParameterDescriptor> result;
		result.reserve(source.size());
		for (const VSTParameterDescriptor& parameter : source)
		{
			if (!parameter.readOnly && !parameter.hidden)
				result.push_back(parameter);
		}
		return result;
	};

	if (effect != NULL)
		return eligibleParameters(effect->getParameterDescriptors());
	if (!outProcParameterDescriptors.empty())
		return eligibleParameters(outProcParameterDescriptors);
	// A VST3 parameter title cannot be recovered from its stable ParamID.
	// Require one successful host snapshot instead of presenting invented names.
	if (outProcMode && library->isVST3())
		return {};

	std::vector<VSTParameterDescriptor> result;
	for (const auto& entry : paramMap)
	{
		const std::wstring& key = entry.first;
		if (key.size() < 2 || key[0] != L'#')
			continue;
		wchar_t* end = NULL;
		const unsigned long id = wcstoul(key.c_str() + 1, &end, 10);
		if (end == key.c_str() + 1 || id > UINT32_MAX)
			continue;

		VSTParameterDescriptor descriptor;
		descriptor.api = library->isVST3()
			? VSTParameterApi::VST3 : VSTParameterApi::VST2;
		descriptor.stableId = static_cast<std::uint32_t>(id);
		descriptor.normalizedValue = entry.second;
		if (descriptor.api == VSTParameterApi::VST2)
		{
			if (*end != L':' || *(end + 1) == L'\0')
				continue;
			descriptor.name = end + 1;
		}
		else
		{
			if (*end != L'\0')
				continue;
			descriptor.name.clear();
		}
		result.push_back(std::move(descriptor));
	}
	std::sort(result.begin(), result.end(),
		[](const VSTParameterDescriptor& left,
			const VSTParameterDescriptor& right) {
			return left.stableId < right.stableId;
		});
	return result;
}

void VSTPluginFilterGUI::updateMidiButton()
{
	VSTMidiConfiguration configuration;
	const bool valid = !midiConfig.empty() &&
		VSTMidiBindingCodec::deserialize(midiConfig, configuration);
	if (valid && !configuration.bindings.empty())
	{
		ui->midiButton->setText(tr("MIDI control… (%1)")
			.arg(configuration.bindings.size()));
		ui->midiButton->setToolTip(tr("%1 MIDI mapping(s) active")
			.arg(configuration.bindings.size()));
		ui->midiButton->setProperty("statusLevel", "normal");
	}
	else
	{
		ui->midiButton->setText(tr("MIDI control…"));
		ui->midiButton->setToolTip(tr(
			"Bind MIDI knobs, faders, and buttons to VST parameters"));
		ui->midiButton->setProperty("statusLevel",
			midiConfig.empty() ? QVariant() : QVariant("warning"));
	}
	ui->midiButton->style()->unpolish(ui->midiButton);
	ui->midiButton->style()->polish(ui->midiButton);
}

void VSTPluginFilterGUI::refreshVST3ClassComboBox()
{
	ui->vst3ClassComboBox->blockSignals(true);
	ui->vst3ClassComboBox->clear();
	ui->vst3ClassComboBox->setVisible(false);
	ui->classLabel->setVisible(false);

	if (library != NULL && library->isVST3() && library->initialize() >= 0 && library->getVST3ClassCount() > 1)
	{
		for (int i = 0; i < library->getVST3ClassCount(); ++i)
			ui->vst3ClassComboBox->addItem(QString::fromUtf8(library->getVST3ClassInfo(i).name));
		if (vst3ClassIndex < 0 || vst3ClassIndex >= library->getVST3ClassCount())
			vst3ClassIndex = 0;
		ui->vst3ClassComboBox->setCurrentIndex(vst3ClassIndex);
		ui->vst3ClassComboBox->setVisible(true);
		ui->classLabel->setVisible(true);
	}

	ui->vst3ClassComboBox->blockSignals(false);
}

void VSTPluginFilterGUI::on_selectButton_clicked()
{
	QDir pluginsDir(QString::fromStdWString(VSTPluginLibrary::getDefaultPluginPath()));

	QSettings settings(QString::fromWCharArray(EDITOR_REGPATH), QSettings::NativeFormat);
	QString lastDir = settings.value("vst/lastDir", "").toString();
	if (lastDir == "")
		lastDir = pluginsDir.absolutePath();

	QFileInfo fileInfo(lastDir);
	QString path = ui->pathLineEdit->text();
	if (path.length() > 0)
		fileInfo.setFile(pluginsDir, path);

	QFileDialog dialog(this, tr("Select VST plugin"), fileInfo.absoluteFilePath(), "*.dll *.vst3");
	dialog.setFileMode(QFileDialog::ExistingFile);
	dialog.setNameFilter(tr("VST plugins (*.dll *.vst3)"));
	if (path.length() > 0)
		dialog.selectFile(fileInfo.fileName());
	if (dialog.exec() == QDialog::Accepted)
	{
		QString absolutePath = dialog.selectedFiles().first();
		settings.setValue("vst/lastDir", QDir::toNativeSeparators(QFileInfo(absolutePath).absolutePath()));
		QString relativePath = pluginsDir.relativeFilePath(absolutePath);
		if (relativePath.startsWith("../../"))
			relativePath = absolutePath;
		ui->pathLineEdit->setText(QDir::toNativeSeparators(relativePath));
		on_pathLineEdit_editingFinished();
	}
}

void VSTPluginFilterGUI::on_idle()
{
	if (outProcMode && outProcGuiRunning && consumeOutProcPanelSignal(L"GuiHidden"))
	{
		outProcGuiHidden = true;
		ui->openPanelButton->setText(tr("Show panel"));
	}

	if (outProcMode && outProcGuiRunning && !outProcGuiConfigPath.isEmpty())
	{
		if (!lastReadTimer.isValid() || lastReadTimer.elapsed() > 500)
		{
			OutProcVSTConfig updatedConfig;
			if (OutProcReadVSTConfig(outProcGuiConfigPath.toStdWString(), updatedConfig))
			{
				std::vector<VSTParameterDescriptor> updatedDescriptors =
					convertOutProcParameterDescriptors(updatedConfig.parameterDescriptors);
				const bool stateChanged = chunkData != updatedConfig.chunkData ||
					paramMap != updatedConfig.paramMap;
				chunkData = updatedConfig.chunkData;
				paramMap = updatedConfig.paramMap;
				outProcParameterDescriptors = std::move(updatedDescriptors);
				if (stateChanged)
				{
					updateModel();
				}
			}
			lastReadTimer.restart();
		}
	}

	if (effect != NULL && autoApplyDialog)
	{
		const qint64 elapsed = lastReadTimer.isValid()
			? lastReadTimer.elapsed() : 1000;
		// A plug-in may omit automation notifications, so retain a slow
		// fallback poll. Continuous controls are coalesced to one save rather
		// than serializing and rebuilding the APO for every MIDI/drag event.
		if ((automationDirty && elapsed >= 75) || elapsed >= 1000)
		{
			if (capturePluginStateIfChanged())
				updateModel();
			automationDirty = false;
			lastReadTimer.restart();
		}
	}
}

void VSTPluginFilterGUI::onAutomate()
{
	if (autoApplyDialog)
		automationDirty = true;
}

void VSTPluginFilterGUI::onSizeWindow(int w, int h)
{
	Q_UNUSED(w);
	Q_UNUSED(h);
}

void VSTPluginFilterGUI::updatePermissionWarning()
{
	if (effect == NULL)
	{
		ui->warningTextEdit->setVisible(false);
		return;
	}

	ACCESS_MASK mask = GENERIC_READ;
	try
	{
		mask = RegistryHelper::getFileAccessForUser(library->getLibPath(), SECURITY_LOCAL_SERVICE_RID);
	}
	catch (RegistryException e)
	{
		// ignore
	}

	if ((mask & GENERIC_READ) != GENERIC_READ && (mask & FILE_GENERIC_READ) != FILE_GENERIC_READ)
	{
		QString text = tr("The library is not readable by the audio service.\nChange the file permissions or copy the file to the VSTPlugins directory.");

		ui->warningTextEdit->setPlainText(text);
		ui->warningTextEdit->setVisible(true);
		return;
	}

	QStringList files;
	if (chunkData != L"" && chunkData.length() < 100000)
	{
		QByteArray bytes = QByteArray::fromBase64(QString::fromStdWString(chunkData).toUtf8());
		QString string = QString::fromUtf8(bytes.data(), bytes.length());
		QRegularExpression regexp("[A-Za-z]:(?:\\\\[\\w \\(\\)-]+)+\\.[A-Za-z]{3}");
		QRegularExpressionMatchIterator it = regexp.globalMatch(string);
		while (it.hasNext())
		{
			QRegularExpressionMatch m = it.next();
			QString path = m.captured();
			QFile file(path);
			if (file.exists())
			{
				ACCESS_MASK mask = GENERIC_READ;
				try
				{
					mask = RegistryHelper::getFileAccessForUser(path.toStdWString(), SECURITY_LOCAL_SERVICE_RID);
				}
				catch (RegistryException e)
				{
					// ignore
				}

				if ((mask & GENERIC_READ) != GENERIC_READ && (mask & FILE_GENERIC_READ) != FILE_GENERIC_READ)
					files.append(path);
			}
		}
	}

	if (files.isEmpty())
	{
		ui->warningTextEdit->setVisible(false);
		ui->warningTextEdit->setPlainText("");
	}
	else
	{
		files.removeDuplicates();
		QString text = tr("The plugin seemingly accesses these files not readable by the audio service:\n"
				"%1\n"
				"Change the file permissions or copy the files to the config directory.").arg(files.join("\n"));
		ui->warningTextEdit->setPlainText(text);
		ui->warningTextEdit->setVisible(true);
	}
}
